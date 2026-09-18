/* gui.c - 主窗口实现（详见 gui.h） */
#include "gui.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "util.h"
#include "logger.h"
#include "config.h"
#include "client.h"

/* 控件 ID */
#define IDC_BTN_LOGIN  1001
#define IDC_BTN_LOGOUT 1002
#define IDC_BTN_SELF   1003
#define IDC_BTN_PARSE  1004
#define IDC_CHK_ACCT   1005
#define IDC_CHK_PWD    1006
#define IDC_CHK_START  1007

/* 定时器 ID */
#define TIMER_LOG   1
#define TIMER_AUTOLOGIN 2
#define TIMER_EXIT  3

/* 后台线程 -> 主线程消息 */
#define WM_APP_UI (WM_APP + 0x100)

/* UI 消息类型 */
enum {
    UI_BUSY,             /* payload "1"/"0" 忙碌状态 */
    UI_STATUS,           /* payload 状态栏文本 */
    UI_ONLINE,           /* payload "1"/"0" 上线状态 */
    UI_ONLINE_INFO,      /* payload 多行在线信息 */
    UI_SELF_URL,         /* payload 自助服务地址 */
    UI_INFO,             /* payload 弹窗提示 */
    UI_ERROR,            /* payload 弹窗错误 */
    UI_AUTOLOGIN_SUCCESS,/* 开机自启动认证成功 */
    UI_SET_USERINDEX,    /* payload userIndex */
    UI_SET_CLIENT,       /* payload = PortalClient*（不释放） */
    UI_START_KEEPALIVE,  /* payload 保活间隔（秒）字符串 */
};

/* 后台线程到主线程的一条消息 */
typedef struct {
    int  kind;
    char *payload; /* malloc，主线程处理后释放（UI_SET_CLIENT 除外） */
} UiMsg;

/* 开机自启动：Windows 注册表 HKCU Run 键（无需管理员权限） */
#define RUN_KEY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_ENTRY_NAME L"校园网认证"
/* 自动登录成功后延迟退出毫秒数（留出时间刷新界面日志与状态） */
#define AUTOLOGIN_EXIT_DELAY_MS 2000
/* 自助服务系统地址（「设备未注册」时引导用户登记接入设备） */
#define SELF_SERVICE_BASE "http://172.17.101.57:8080/selfservice/"

/* 全局界面状态（单窗口程序） */
typedef struct {
    HINSTANCE hInst;
    HWND hMain;
    HWND hGroup1, hGroup2, hGroup3;
    HFONT fUi, fLog;
    HBRUSH hBrushGray;

    /* 控件 */
    HWND hPortal, hUser, hPwd, hUrl, hQs, hInfo, hLog, hStatus;
    HWND btnLogin, btnLogout, btnSelf, btnParse;
    HWND chkAcct, chkPwd, chkStart;
    HWND lblPortal, lblUser, lblPwd, lblUrl;

    /* 状态 */
    AppConfig cfg;
    char *query_string;   /* 当前登录参数 */
    char *index_url;      /* 当前登录页 URL（Referer） */
    char *user_index;     /* 登录成功后的会话令牌 */
    char *self_url;       /* 自助服务免登地址 */
    bool busy;            /* 是否正在执行网络操作 */
    bool online;          /* 是否已上线 */
    bool auto_login;      /* 开机自启动模式 */
    PortalClient *client; /* 当前客户端（登录成功后由 UI 线程持有） */

    /* 保活线程 */
    volatile bool keepalive_stop;
    HANDLE hKeepalive;
} GuiState;

static GuiState g;

/* ------------------------------------------------------------------ */
/* 辅助：宽字符串弹窗 / 文本读写                                         */
/* ------------------------------------------------------------------ */
static void ui_msg_wide(UINT uType, const wchar_t *title, const char *utf8_msg)
{
    wchar_t *w = utf8_to_wide(utf8_msg ? utf8_msg : "");
    MessageBoxW(g.hMain, w ? w : L"", title, uType);
    free(w);
}

/* 读取编辑框文本（UTF-8，malloc） */
static char *get_text(HWND h)
{
    int n = GetWindowTextLengthW(h);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n + 1));
    if (!w)
        return xstrdup("");
    GetWindowTextW(h, w, n + 1);
    char *s = wide_to_utf8(w);
    free(w);
    return s ? s : xstrdup("");
}

static void set_text(HWND h, const char *utf8)
{
    wchar_t *w = utf8_to_wide(utf8 ? utf8 : "");
    if (w) {
        SetWindowTextW(h, w);
        free(w);
    }
}

static void post_ui(int kind, const char *payload)
{
    UiMsg *m = (UiMsg *)malloc(sizeof(UiMsg));
    if (!m)
        return;
    m->kind = kind;
    m->payload = payload ? xstrdup(payload) : xstrdup("");
    PostMessageW(g.hMain, WM_APP_UI, 0, (LPARAM)m);
}

static void post_ui_ptr(int kind, void *ptr)
{
    UiMsg *m = (UiMsg *)malloc(sizeof(UiMsg));
    if (!m)
        return;
    m->kind = kind;
    m->payload = (char *)ptr; /* 指针直接传递，主线程不释放 */
    PostMessageW(g.hMain, WM_APP_UI, 0, (LPARAM)m);
}

/* ------------------------------------------------------------------ */
/* 开机自启动（注册表）                                                  */
/* ------------------------------------------------------------------ */
static bool get_auto_start_enabled(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024];
    DWORD type = 0, sz = sizeof(buf);
    LONG r = RegQueryValueExW(key, RUN_ENTRY_NAME, NULL, &type, (LPBYTE)buf, &sz);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static bool set_auto_start(bool enabled)
{
    if (enabled) {
        wchar_t exe[2048];
        DWORD n = GetModuleFileNameW(NULL, exe, 2048);
        if (n == 0 || n >= 2048)
            return false;
        wchar_t cmd[4096];
        swprintf_s(cmd, 4096, L"\"%s\" --autologin", exe);
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
            return false;
        LONG r = RegSetValueExW(key, RUN_ENTRY_NAME, 0, REG_SZ,
                                (const BYTE *)cmd,
                                (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return r == ERROR_SUCCESS;
    } else {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
            return true; /* 键不存在视为成功（本就未注册） */
        LONG r = RegDeleteValueW(key, RUN_ENTRY_NAME);
        RegCloseKey(key);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
}

/* ------------------------------------------------------------------ */
/* 配置读写（界面 <-> config.json）                                     */
/* ------------------------------------------------------------------ */
static void cfg_from_ui(void)
{
    char *s;

    s = get_text(g.hPortal);
    free(g.cfg.portal_base);
    g.cfg.portal_base = s;

    g.cfg.remember_account = (SendMessageW(g.chkAcct, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g.cfg.remember_password = (SendMessageW(g.chkPwd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (g.cfg.remember_account) {
        s = get_text(g.hUser);
        free(g.cfg.saved_user_id);
        g.cfg.saved_user_id = s;
    }
    if (g.cfg.remember_password) {
        s = get_text(g.hPwd);
        free(g.cfg.saved_password);
        g.cfg.saved_password = s;
    }
    /* 记住当前登录参数（解析成功后写入），下次启动直接复用 */
    if (g.query_string && *g.query_string) {
        free(g.cfg.saved_query_string);
        g.cfg.saved_query_string = xstrdup(g.query_string);
        free(g.cfg.saved_index_url);
        g.cfg.saved_index_url = xstrdup(g.index_url ? g.index_url : "");
    }
}

static void cfg_to_ui(void)
{
    set_text(g.hPortal, g.cfg.portal_base);
    set_text(g.hUser, g.cfg.saved_user_id);
    set_text(g.hPwd, g.cfg.remember_password ? g.cfg.saved_password : "");
    SendMessageW(g.chkAcct, BM_SETCHECK, g.cfg.remember_account ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.chkPwd, BM_SETCHECK, g.cfg.remember_password ? BST_CHECKED : BST_UNCHECKED, 0);

    /* 恢复上次记住的登录参数：同一设备/IP 下参数稳定，可直接一键登录 */
    if (g.cfg.saved_query_string && *g.cfg.saved_query_string) {
        free(g.query_string);
        g.query_string = xstrdup(g.cfg.saved_query_string);
        free(g.index_url);
        g.index_url = xstrdup(g.cfg.saved_index_url ? g.cfg.saved_index_url : "");
        set_text(g.hQs, g.query_string);
        set_text(g.hUrl, g.index_url);
        set_text(g.hStatus, "已加载上次登录参数，可直接登录");
    }
    /* 开机自启动状态以注册表为准（可反映任务管理器等外部修改） */
    SendMessageW(g.chkStart, BM_SETCHECK, get_auto_start_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void save_cfg(void)
{
    cfg_from_ui();
    config_save(&g.cfg);
}

/* ------------------------------------------------------------------ */
/* 字体 / 布局                                                          */
/* ------------------------------------------------------------------ */
static HFONT make_font(const wchar_t *face, int pt, bool bold)
{
    HDC hdc = GetDC(NULL);
    int h = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

/* 重新布局全部控件（随窗口大小变化） */
static void layout(void)
{
    RECT rc;
    GetClientRect(g.hMain, &rc);
    int W = rc.right, H = rc.bottom;
    const int M = 12;     /* 外边距 */
    const int GM = 8;     /* 分组框间距 */
    int x1 = M, x2 = W - M;
    int g1top = 8, g1h = 258;                 /* 认证设置 */
    int g2top = g1top + g1h + GM, g2h = 148;  /* 在线信息 */
    int g3top = g2top + g2h + GM;             /* 日志 */
    int g3h = H - g3top - M;
    if (g3h < 80)
        g3h = 80;

    MoveWindow(g.hGroup1, x1, g1top, x2 - x1, g1h, TRUE);
    MoveWindow(g.hGroup2, x1, g2top, x2 - x1, g2h, TRUE);
    MoveWindow(g.hGroup3, x1, g3top, x2 - x1, g3h, TRUE);

    const int cx = x1 + 16;      /* 内容左边缘 */
    const int cw = x2 - x1 - 32; /* 内容宽 */
    const int lblw = 66;
    int cy = g1top + 28;
    const int pitch = 34;
    const int eh = 24;           /* 编辑框高度 */

    /* 行1 门户地址 */
    SetWindowPos(g.lblPortal, NULL, cx, cy, lblw, 20, SWP_NOZORDER);
    SetWindowPos(g.hPortal, NULL, cx + lblw + 6, cy - 2, cw - lblw - 6, eh, SWP_NOZORDER);

    /* 行2 账号/密码 */
    cy += pitch;
    SetWindowPos(g.lblUser, NULL, cx, cy, lblw, 20, SWP_NOZORDER);
    SetWindowPos(g.hUser, NULL, cx + lblw + 6, cy - 2, 170, eh, SWP_NOZORDER);
    SetWindowPos(g.lblPwd, NULL, cx + lblw + 6 + 170 + 10, cy, 40, 20, SWP_NOZORDER);
    SetWindowPos(g.hPwd, NULL, cx + lblw + 6 + 170 + 54, cy - 2, 170, eh, SWP_NOZORDER);

    /* 行3 记住账号/密码 */
    cy += pitch;
    SetWindowPos(g.chkAcct, NULL, cx + lblw + 6, cy - 2, 110, 24, SWP_NOZORDER);
    SetWindowPos(g.chkPwd, NULL, cx + lblw + 6 + 118, cy - 2, 220, 24, SWP_NOZORDER);

    /* 行4 登录参数 + 解析按钮 */
    cy += pitch;
    SetWindowPos(g.lblUrl, NULL, cx, cy, lblw, 20, SWP_NOZORDER);
    SetWindowPos(g.hUrl, NULL, cx + lblw + 6, cy - 2, cw - lblw - 6 - 96, eh, SWP_NOZORDER);
    SetWindowPos(g.btnParse, NULL, cx + lblw + 6 + (cw - lblw - 6 - 96) + 8, cy - 4, 88, 28, SWP_NOZORDER);

    /* 行5 queryString 只读显示 */
    cy += pitch;
    SetWindowPos(g.hQs, NULL, cx + lblw + 6, cy - 2, cw - lblw - 6, eh, SWP_NOZORDER);

    /* 行6 开机自启动 */
    cy += pitch;
    SetWindowPos(g.chkStart, NULL, cx + lblw + 6, cy - 2, 500, 24, SWP_NOZORDER);

    /* 行7 操作按钮 + 状态 */
    cy += pitch;
    SetWindowPos(g.btnLogin, NULL, cx, cy - 2, 90, 30, SWP_NOZORDER);
    SetWindowPos(g.btnLogout, NULL, cx + 98, cy - 2, 90, 30, SWP_NOZORDER);
    SetWindowPos(g.btnSelf, NULL, cx + 196, cy - 2, 116, 30, SWP_NOZORDER);
    SetWindowPos(g.hStatus, NULL, cx + 320, cy + 2, cw - 320 - 8, 24, SWP_NOZORDER);

    /* 在线信息 */
    SetWindowPos(g.hInfo, NULL, x1 + 8, g2top + 24, x2 - x1 - 16, g2h - 30, SWP_NOZORDER);

    /* 日志 */
    SetWindowPos(g.hLog, NULL, x1 + 8, g3top + 24, x2 - x1 - 16, g3h - 30, SWP_NOZORDER);
}

/* ------------------------------------------------------------------ */
/* 保活线程                                                            */
/* ------------------------------------------------------------------ */
static void stop_keepalive(void)
{
    g.keepalive_stop = true;
    if (g.hKeepalive) {
        WaitForSingleObject(g.hKeepalive, 3000);
        CloseHandle(g.hKeepalive);
        g.hKeepalive = NULL;
    }
}

/* 保活线程参数：客户端指针（不释放）+ 间隔秒数 */
typedef struct {
    PortalClient *pc;
    int           interval;
} KeepaliveCtx;

static DWORD WINAPI keepalive_worker(LPVOID param)
{
    KeepaliveCtx *kc = (KeepaliveCtx *)param;
    if (!kc || !kc->pc || kc->interval <= 0) {
        free(kc);
        return 1;
    }
    /* 复制必要状态，避免与 UI 线程竞争 g 全局变量 */
    PortalClient *pc = kc->pc;
    int interval = kc->interval;
    char *user_index = xstrdup(g.user_index ? g.user_index : "");
    char *index_url = xstrdup(g.index_url ? g.index_url : "");
    free(kc);

    LOG_INFO("保活线程启动，间隔 %d 秒", interval);
    while (!g.keepalive_stop) {
        Sleep((DWORD)interval * 1000);
        if (g.keepalive_stop)
            break;
        PortalError err;
        if (portal_keepalive(pc, user_index, index_url, &err) != 0) {
            LOG_ERROR("保活请求失败，停止保活: %s", err.msg);
            post_ui(UI_STATUS, "保活失败，可能需要重新认证");
            break;
        }
    }
    free(user_index);
    free(index_url);
    LOG_INFO("保活线程退出");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 后台任务：登录                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char *portal_base;
    char *user_id;
    char *password;
    char *query_string;
    char *index_url;
} LoginCtx;

static void free_login_ctx(LoginCtx *lc)
{
    if (!lc)
        return;
    free(lc->portal_base);
    free(lc->user_id);
    free(lc->password);
    free(lc->query_string);
    free(lc->index_url);
    free(lc);
}

static DWORD WINAPI login_worker(LPVOID param)
{
    LoginCtx *lc = (LoginCtx *)param;
    PortalError err;

    PortalClient *pc = (PortalClient *)calloc(1, sizeof(PortalClient));
    if (!pc) {
        post_ui(UI_ERROR, "内存不足");
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 1;
    }
    if (portal_init(pc, lc->portal_base, 10000) != 0) {
        LOG_ERROR("HTTP 客户端初始化失败: %s", http_last_error());
        post_ui(UI_ERROR, "HTTP 客户端初始化失败，请检查网络");
        portal_free(pc);
        free(pc);
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 1;
    }

    /* 登录前校验接入参数完整性：缺失则服务端无法定位 NAS，会返回
     * 「设备未注册」等误导性错误，必须提前拦截并给出明确提示 */
    if (portal_validate_query_string(lc->query_string, &err) != 0) {
        LOG_ERROR("登录参数不完整: %s", err.msg);
        post_ui(UI_STATUS, "登录参数不完整");
        post_ui(UI_ERROR, err.msg);
        portal_free(pc);
        free(pc);
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 1;
    }

    /* 直接尝试认证：当前已连上校园网即可进入认证，不再主动连接 WiFi */
    /* 1. 页面初始化：获取加密开关、RSA 公钥，并探测是否已在线上网 */
    post_ui(UI_STATUS, "正在连接门户并初始化...");
    InitContext ctx;
    if (portal_init_context(pc, lc->query_string, lc->index_url, &ctx, &err) != 0) {
        LOG_ERROR("页面初始化失败: %s", err.msg);
        post_ui(UI_STATUS, "初始化失败");
        post_ui(UI_ERROR, err.msg);
        portal_free(pc);
        free(pc);
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 1;
    }

    /* 在线探测：已在线时避免重复登录 */
    if (ctx.already_online) {
        if (g.auto_login) {
            LOG_INFO("开机自启动：检测到当前设备已在线，无需重复登录，自动退出");
            portal_init_context_free(&ctx);
            portal_free(pc);
            free(pc);
            post_ui(UI_AUTOLOGIN_SUCCESS, NULL);
            post_ui(UI_BUSY, "0");
            free_login_ctx(lc);
            return 0;
        }
        LOG_WARNING("检测到当前设备已在线上网，跳过重复登录");
        char *info = portal_format_online_info(ctx.online_info, NULL);
        post_ui(UI_STATUS, "当前设备已在线，无需重复登录（可先注销）");
        post_ui(UI_ONLINE_INFO, info);
        post_ui(UI_ERROR, "检测到当前设备已在线上网（可能已认证）。\n如需切换账号，请先注销后再登录。");
        free(info);
        portal_init_context_free(&ctx);
        portal_free(pc);
        free(pc);
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 0;
    }

    /* 2. 登录（含并发上限自动重试） */
    post_ui(UI_STATUS, "正在认证...");
    char user_index[512] = "";
    if (portal_login(pc, lc->user_id, lc->password, lc->query_string,
                     ctx.password_encrypt, ctx.public_key_modulus,
                     ctx.public_key_exponent, lc->index_url,
                     user_index, sizeof(user_index), &err) != 0) {
        LOG_ERROR("登录失败: %s", err.msg);
        char *text = portal_build_login_error(err.msg, lc->query_string);
        char status[1600];
        _snprintf(status, sizeof(status), "登录失败：%s", text ? text : err.msg);
        post_ui(UI_STATUS, status);
        /* 设备未注册类错误：引导到自助服务系统登记接入设备 */
        if (strstr(text, "设备未注册") || strstr(text, "添加认证设备")) {
            StrBuf sb;
            sb_init(&sb);
            sb_append(&sb, text ? text : err.msg);
            sb_append(&sb, "\n\n提示：服务端要求当前接入设备已在账号下登记。\n"
                           "请在浏览器打开自助服务系统添加认证设备后重试：\n");
            sb_append(&sb, SELF_SERVICE_BASE);
            post_ui(UI_ERROR, sb.buf);
            sb_free(&sb);
        } else {
            post_ui(UI_ERROR, text ? text : err.msg);
        }
        free(text);
        portal_init_context_free(&ctx);
        portal_free(pc);
        free(pc);
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 1;
    }
    portal_init_context_free(&ctx);
    post_ui(UI_SET_USERINDEX, user_index);
    post_ui_ptr(UI_SET_CLIENT, pc); /* 客户端转交 UI 线程持有（用于注销/保活） */

    /* 3. 访问登录成功页（推进服务端状态并获取保活间隔；失败不阻断登录） */
    int keepalive_interval = 0;
    PortalError perr;
    keepalive_interval = portal_visit_success_page(pc, user_index, 0, lc->index_url, &perr);
    if (keepalive_interval == 0 && perr.msg[0])
        LOG_WARNING("访问成功页失败（不影响登录）: %s", perr.msg);

    /* 4. 轮询在线状态直至 success */
    post_ui(UI_STATUS, "已认证，正在确认上线状态...");
    cJSON *online = NULL;
    if (portal_wait_online(pc, user_index, lc->index_url, &online, &err) != 0) {
        LOG_ERROR("等待上线失败: %s", err.msg);
        post_ui(UI_STATUS, "等待上线失败");
        post_ui(UI_ERROR, err.msg);
        portal_free(pc);
        free(pc);
        post_ui(UI_BUSY, "0");
        free_login_ctx(lc);
        return 1;
    }

    post_ui(UI_ONLINE, "1");
    char status[512];
    _snprintf(status, sizeof(status), "已上线！账号 %s 认证成功", lc->user_id);
    post_ui(UI_STATUS, status);
    const cJSON *ug = cJSON_GetObjectItemCaseSensitive(online, "userGroup");
    LOG_INFO("上线成功，userGroup=%s", (ug && cJSON_IsString(ug)) ? ug->valuestring : "");

    /* 展示完整在线用户信息 */
    char *info = portal_format_online_info(online, user_index);
    post_ui(UI_ONLINE_INFO, info);
    LOG_INFO("在线信息:\r\n%s", info);
    free(info);

    /* 自助服务免登入口 */
    char *self_url = portal_extract_self_url(online);
    if (self_url) {
        post_ui(UI_SELF_URL, self_url);
        LOG_INFO("已获取自助服务免登地址");
        free(self_url);
    }
    cJSON_Delete(online);

    /* keepaliveInterval > 0 时按间隔发送保活心跳；自动登录模式即将退出无需保活 */
    if (keepalive_interval > 0 && !g.auto_login) {
        char iv[32];
        _snprintf(iv, sizeof(iv), "%d", keepalive_interval);
        post_ui(UI_START_KEEPALIVE, iv);
        LOG_INFO("已启用自动保活，间隔 %d 秒", keepalive_interval);
    }

    post_ui(UI_INFO, "校园网认证成功，已上线！");
    if (g.auto_login)
        post_ui(UI_AUTOLOGIN_SUCCESS, NULL);
    post_ui(UI_BUSY, "0");
    free_login_ctx(lc);
    return 0;
}

/* 后台任务：注销 */
static DWORD WINAPI logout_worker(LPVOID param)
{
    (void)param;
    PortalError err;
    PortalClient *pc = g.client;
    if (!pc) {
        post_ui(UI_BUSY, "0");
        return 1;
    }
    post_ui(UI_STATUS, "正在注销...");
    cJSON *out = NULL;
    int rc = portal_logout(pc, g.user_index ? g.user_index : "", g.index_url ? g.index_url : "", &out, &err);
    if (out)
        cJSON_Delete(out);
    if (rc != 0) {
        LOG_ERROR("注销失败: %s", err.msg);
        post_ui(UI_STATUS, "注销失败");
        post_ui(UI_ERROR, err.msg);
        post_ui(UI_BUSY, "0");
        return 1;
    }
    post_ui(UI_ONLINE, "0");
    post_ui(UI_SELF_URL, "");
    post_ui(UI_ONLINE_INFO, "（已注销下线，可重新登录）");
    post_ui(UI_STATUS, "已注销下线");
    post_ui(UI_INFO, "已注销下线");
    LOG_INFO("注销成功");
    post_ui(UI_BUSY, "0");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 界面操作事件                                                          */
/* ------------------------------------------------------------------ */
static void on_parse_url(void)
{
    char *url = get_text(g.hUrl);
    if (!url || !*url) {
        ui_msg_wide(MB_ICONWARNING, L"提示", "请先粘贴登录页 URL（浏览器地址栏的内容）");
        free(url);
        return;
    }
    char qs[4096];
    PortalError err;
    if (portal_parse_index_url(url, qs, sizeof(qs), &err) != 0) {
        ui_msg_wide(MB_ICONERROR, L"错误", err.msg);
        free(url);
        return;
    }
    /* 校验接入参数完整性：缺失则直接拦截，避免触发「设备未注册」等误导性错误 */
    if (portal_validate_query_string(qs, &err) != 0) {
        ui_msg_wide(MB_ICONERROR, L"错误", err.msg);
        free(url);
        return;
    }
    free(g.query_string);
    g.query_string = xstrdup(qs);
    free(g.index_url);
    g.index_url = xstrdup(url);
    set_text(g.hQs, qs);
    save_cfg(); /* 记住登录参数，下次启动可直接登录 */
    LOG_INFO("解析成功，已获取登录参数（已记住），长度 %zu", strlen(qs));
    set_text(g.hStatus, "已获取登录参数（已记住），下次打开可直接登录");
    free(url);
}

static void on_toggle_auto_start(void)
{
    bool enabled = (SendMessageW(g.chkStart, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (enabled) {
        if (!set_auto_start(true)) {
            SendMessageW(g.chkStart, BM_SETCHECK, BST_UNCHECKED, 0);
            ui_msg_wide(MB_ICONWARNING, L"提示",
                        "开机自启动注册失败。\n"
                        "请确认当前用户有写注册表权限。");
            return;
        }
        LOG_INFO("已启用开机自启动（开机后自动登录，成功后自动退出）");
        set_text(g.hStatus, "已启用开机自启动");
    } else {
        set_auto_start(false);
        LOG_INFO("已关闭开机自启动");
        set_text(g.hStatus, "已关闭开机自启动");
    }
    g.cfg.auto_start = enabled;
    save_cfg();
}

static void on_login(void)
{
    if (g.busy)
        return;
    char *user = get_text(g.hUser);
    char *pwd = get_text(g.hPwd);
    if (!user || !*user || !pwd || !*pwd) {
        ui_msg_wide(MB_ICONWARNING, L"提示", "请填写账号和密码");
        free(user);
        free(pwd);
        return;
    }
    if (!g.query_string || !*g.query_string) {
        ui_msg_wide(MB_ICONWARNING, L"提示",
                    "尚未获取登录参数。请在浏览器打开门户登录页，"
                    "把地址栏 URL 粘贴到上方输入框后点击「解析 URL」。");
        free(user);
        free(pwd);
        return;
    }
    char *portal = get_text(g.hPortal);
    /* 自动补全协议头 */
    if (portal && *portal && strncmp(portal, "http://", 7) != 0 && strncmp(portal, "https://", 8) != 0) {
        char *tmp = portal;
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, "http://");
        sb_append(&sb, tmp);
        portal = sb_detach(&sb);
        free(tmp);
        set_text(g.hPortal, portal);
    }
    save_cfg();

    LoginCtx *lc = (LoginCtx *)calloc(1, sizeof(LoginCtx));
    if (!lc) {
        free(user);
        free(pwd);
        free(portal);
        return;
    }
    lc->portal_base = portal;
    lc->user_id = user;
    lc->password = pwd;
    lc->query_string = xstrdup(g.query_string);
    lc->index_url = xstrdup(g.index_url ? g.index_url : "");

    post_ui(UI_BUSY, "1");
    CreateThread(NULL, 0, login_worker, lc, 0, NULL);
}

static void on_logout(void)
{
    if (g.busy || !g.user_index || !*g.user_index) {
        ui_msg_wide(MB_ICONWARNING, L"提示", "当前未登录，无需注销");
        return;
    }
    stop_keepalive(); /* 下线后停止保活心跳 */
    post_ui(UI_BUSY, "1");
    CreateThread(NULL, 0, logout_worker, NULL, 0, NULL);
}

static void on_open_self(void)
{
    if (!g.self_url || !*g.self_url) {
        ui_msg_wide(MB_ICONWARNING, L"提示", "当前没有可用的自助服务地址（登录成功后获取）");
        return;
    }
    wchar_t *w = utf8_to_wide(g.self_url);
    if (!w)
        return;
    HINSTANCE r = ShellExecuteW(g.hMain, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    if ((INT_PTR)r <= 32)
        LOG_WARNING("打开自助服务失败");
    else
        LOG_INFO("已在浏览器打开自助服务: %s", g.self_url);
}

/* 开机自启动模式：读取已保存的账号密码与登录参数，后台自动登录 */
static void trigger_auto_login(void)
{
    char *user = get_text(g.hUser);
    char *pwd = get_text(g.hPwd);
    if (!user || !*user || !pwd || !*pwd || !g.query_string || !*g.query_string) {
        LOG_ERROR("自动登录失败：缺少已保存的账号/密码或登录参数（queryString）");
        set_text(g.hStatus, "自动登录失败：缺少已保存的登录信息，请手动填写后登录");
        ShowWindow(g.hMain, SW_SHOW);
        free(user);
        free(pwd);
        return;
    }
    char *portal = get_text(g.hPortal);
    if (portal && *portal && strncmp(portal, "http://", 7) != 0 && strncmp(portal, "https://", 8) != 0) {
        char *tmp = portal;
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, "http://");
        sb_append(&sb, tmp);
        portal = sb_detach(&sb);
        free(tmp);
    }

    LoginCtx *lc = (LoginCtx *)calloc(1, sizeof(LoginCtx));
    if (!lc) {
        free(user);
        free(pwd);
        free(portal);
        return;
    }
    lc->portal_base = portal;
    lc->user_id = user;
    lc->password = pwd;
    lc->query_string = xstrdup(g.query_string);
    lc->index_url = xstrdup(g.index_url ? g.index_url : "");

    post_ui(UI_BUSY, "1");
    CreateThread(NULL, 0, login_worker, lc, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* UI 消息处理（主线程）                                                 */
/* ------------------------------------------------------------------ */
static void append_log_line(const char *line)
{
    wchar_t *w = utf8_to_wide(line ? line : "");
    if (!w)
        return;
    /* 限制日志面板大小，避免长时间运行卡顿 */
    int len = GetWindowTextLengthW(g.hLog);
    if (len > 150000)
        SetWindowTextW(g.hLog, L"");
    SendMessageW(g.hLog, EM_SETSEL, -1, -1);
    SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)(w));
    SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g.hLog, EM_SCROLLCARET, 0, 0);
    free(w);
}

static void handle_ui_msg(UiMsg *m)
{
    switch (m->kind) {
    case UI_BUSY: {
        bool busy = strcmp(m->payload, "1") == 0;
        g.busy = busy;
        EnableWindow(g.btnLogin, busy ? FALSE : TRUE);
        EnableWindow(g.btnParse, busy ? FALSE : TRUE);
        break;
    }
    case UI_STATUS:
        set_text(g.hStatus, m->payload);
        break;
    case UI_ONLINE: {
        g.online = strcmp(m->payload, "1") == 0;
        EnableWindow(g.btnLogout, g.online ? TRUE : FALSE);
        break;
    }
    case UI_ONLINE_INFO:
        set_text(g.hInfo, m->payload);
        break;
    case UI_SELF_URL: {
        free(g.self_url);
        g.self_url = (*m->payload) ? xstrdup(m->payload) : xstrdup("");
        EnableWindow(g.btnSelf, g.self_url && *g.self_url ? TRUE : FALSE);
        break;
    }
    case UI_INFO:
        if (g.auto_login)
            LOG_INFO("提示（自动模式不弹窗）: %s", m->payload);
        else
            ui_msg_wide(MB_ICONINFORMATION, L"提示", m->payload);
        break;
    case UI_ERROR:
        if (g.auto_login) {
            LOG_ERROR("自动登录出错: %s", m->payload);
            char status[1600];
            _snprintf(status, sizeof(status), "自动登录失败：%s", m->payload);
            set_text(g.hStatus, status);
            ShowWindow(g.hMain, SW_SHOW); /* 失败时恢复显示窗口，便于手动处理 */
        } else {
            ui_msg_wide(MB_ICONERROR, L"错误", m->payload);
        }
        break;
    case UI_AUTOLOGIN_SUCCESS:
        LOG_INFO("自动登录成功，%d 秒后自动退出", AUTOLOGIN_EXIT_DELAY_MS / 1000);
        SetTimer(g.hMain, TIMER_EXIT, AUTOLOGIN_EXIT_DELAY_MS, NULL);
        break;
    case UI_SET_USERINDEX:
        free(g.user_index);
        g.user_index = xstrdup(m->payload);
        break;
    case UI_SET_CLIENT:
        /* 释放旧的客户端（若有）并接管新客户端 */
        if (g.client) {
            portal_free(g.client);
            free(g.client);
        }
        g.client = (PortalClient *)m->payload;
        break;
    case UI_START_KEEPALIVE: {
        int interval = atoi(m->payload);
        if (interval > 0 && g.client) {
            g.keepalive_stop = false;
            /* 封装线程参数：客户端指针 + 保活间隔 */
            KeepaliveCtx *kc = (KeepaliveCtx *)malloc(sizeof(KeepaliveCtx));
            if (kc) {
                kc->pc = g.client;
                kc->interval = interval;
                g.hKeepalive = CreateThread(NULL, 0, keepalive_worker, kc, 0, NULL);
            }
        }
        break;
    }
    default:
        break;
    }
    /* UI_SET_CLIENT 的 payload 是指针（客户端对象），由 UI 线程持有并负责释放 */
    if (m->kind != UI_SET_CLIENT)
        free(m->payload);
    free(m);
}

/* ------------------------------------------------------------------ */
/* 窗口过程                                                            */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_LOGIN:
            on_login();
            break;
        case IDC_BTN_LOGOUT:
            on_logout();
            break;
        case IDC_BTN_SELF:
            on_open_self();
            break;
        case IDC_BTN_PARSE:
            on_parse_url();
            break;
        case IDC_CHK_START:
            if (HIWORD(wParam) == BN_CLICKED)
                on_toggle_auto_start();
            break;
        default:
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_LOG) {
            /* 日志队列轮询 */
            char *line;
            while ((line = logger_queue_pop()) != NULL) {
                append_log_line(line);
                free(line);
            }
        } else if (wParam == TIMER_AUTOLOGIN) {
            KillTimer(hWnd, TIMER_AUTOLOGIN);
            trigger_auto_login();
        } else if (wParam == TIMER_EXIT) {
            KillTimer(hWnd, TIMER_EXIT);
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hctl = (HWND)lParam;
        if (hctl == g.hQs || hctl == g.hInfo || hctl == g.hLog) {
            SetBkColor(hdc, RGB(0xF2, 0xF2, 0xF2));
            return (LRESULT)g.hBrushGray;
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 680;
        mmi->ptMinTrackSize.y = 660;
        return 0;
    }

    case WM_SIZE:
        if (g.hGroup1)
            layout();
        return 0;

    case WM_APP_UI:
        handle_ui_msg((UiMsg *)lParam);
        return 0;

    case WM_CLOSE: {
        /* 保存配置并停止保活线程后退出 */
        stop_keepalive();
        save_cfg();
        DestroyWindow(hWnd);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* 控件创建                                                            */
/* ------------------------------------------------------------------ */
static void create_controls(void)
{
    DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER;

    /* 分组框 */
    g.hGroup1 = CreateWindowExW(0, L"BUTTON", L"认证设置", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.hGroup2 = CreateWindowExW(0, L"BUTTON", L"在线信息（用户名/IP/用户组/套餐/时长/MAB 绑定设备）",
                                WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.hGroup3 = CreateWindowExW(0, L"BUTTON", L"日志（同时写入 logs 目录）",
                                WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 行1 门户地址 */
    g.lblPortal = CreateWindowExW(0, L"STATIC", L"门户地址:", WS_CHILD | WS_VISIBLE,
                                  0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.hPortal = CreateWindowExW(0, L"EDIT", L"", edit_style,
                                0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 行2 账号/密码 */
    g.lblUser = CreateWindowExW(0, L"STATIC", L"账号:", WS_CHILD | WS_VISIBLE,
                                0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.hUser = CreateWindowExW(0, L"EDIT", L"", edit_style,
                              0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.lblPwd = CreateWindowExW(0, L"STATIC", L"密码:", WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.hPwd = CreateWindowExW(0, L"EDIT", L"", edit_style | ES_PASSWORD,
                             0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 行3 记住选项 */
    g.chkAcct = CreateWindowExW(0, L"BUTTON", L"记住账号", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                0, 0, 0, 0, g.hMain, (HMENU)IDC_CHK_ACCT, g.hInst, NULL);
    g.chkPwd = CreateWindowExW(0, L"BUTTON", L"记住密码（明文存本地，慎用）",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g.hMain, (HMENU)IDC_CHK_PWD, g.hInst, NULL);

    /* 行4 登录参数 */
    g.lblUrl = CreateWindowExW(0, L"STATIC", L"登录参数:", WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.hUrl = CreateWindowExW(0, L"EDIT", L"", edit_style,
                             0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);
    g.btnParse = CreateWindowExW(0, L"BUTTON", L"解析 URL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, g.hMain, (HMENU)IDC_BTN_PARSE, g.hInst, NULL);

    /* 行5 queryString 只读 */
    g.hQs = CreateWindowExW(0, L"EDIT", L"", edit_style | ES_READONLY,
                            0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 行6 开机自启动 */
    g.chkStart = CreateWindowExW(0, L"BUTTON",
                                 L"开机自启动（注册到 Windows 启动项，开机后自动登录，登录成功后自动退出）",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 0, 0, 0, 0, g.hMain, (HMENU)IDC_CHK_START, g.hInst, NULL);

    /* 行7 按钮 + 状态 */
    g.btnLogin = CreateWindowExW(0, L"BUTTON", L"登 录",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, g.hMain, (HMENU)IDC_BTN_LOGIN, g.hInst, NULL);
    g.btnLogout = CreateWindowExW(0, L"BUTTON", L"注 销",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 0, 0, g.hMain, (HMENU)IDC_BTN_LOGOUT, g.hInst, NULL);
    g.btnSelf = CreateWindowExW(0, L"BUTTON", L"打开自助服务",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 0, 0, g.hMain, (HMENU)IDC_BTN_SELF, g.hInst, NULL);
    g.hStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 在线信息面板 */
    g.hInfo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"（登录成功后在此显示完整在线用户信息）",
                              WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                              ES_AUTOVSCROLL | WS_VSCROLL,
                              0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 日志面板 */
    g.hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                             ES_AUTOVSCROLL | WS_VSCROLL,
                             0, 0, 0, 0, g.hMain, NULL, g.hInst, NULL);

    /* 应用字体 */
    SendMessageW(g.hLog, WM_SETFONT, (WPARAM)g.fLog, TRUE);
    SendMessageW(g.hInfo, WM_SETFONT, (WPARAM)g.fLog, TRUE);
    HWND all[] = { g.hGroup1, g.hGroup2, g.hGroup3, g.lblPortal, g.hPortal, g.lblUser,
                   g.hUser, g.lblPwd, g.hPwd, g.chkAcct, g.chkPwd, g.lblUrl, g.hUrl,
                   g.btnParse, g.hQs, g.chkStart, g.btnLogin, g.btnLogout, g.btnSelf, g.hStatus };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        SendMessageW(all[i], WM_SETFONT, (WPARAM)g.fUi, TRUE);

    /* 初始状态 */
    EnableWindow(g.btnLogout, FALSE);
    EnableWindow(g.btnSelf, FALSE);
}

/* ------------------------------------------------------------------ */
/* 主入口                                                              */
/* ------------------------------------------------------------------ */
int gui_run(HINSTANCE hInstance, int nCmdShow, bool auto_login)
{
    g.hInst = hInstance;
    g.auto_login = auto_login;

    const wchar_t *cls = L"CampusAuthWindowClass";
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    g.fUi = make_font(L"Microsoft YaHei UI", 10, false);
    g.fLog = make_font(L"Consolas", 9, false);
    g.hBrushGray = CreateSolidBrush(RGB(0xF2, 0xF2, 0xF2));

    /* 加载配置 */
    config_load(&g.cfg);

    g.hMain = CreateWindowExW(0, cls, L"校园网认证客户端 (ePortal)",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 780, 780,
                              NULL, NULL, hInstance, NULL);
    if (!g.hMain)
        return 1;

    create_controls();
    cfg_to_ui();
    layout();

    /* 日志队列轮询 */
    SetTimer(g.hMain, TIMER_LOG, 200, NULL);

    /* 开机自启动模式也正常显示前端窗口（不再是隐藏静默），
     * 自动登录流程照常执行，成功后按既定逻辑自动退出 */
    ShowWindow(g.hMain, auto_login ? SW_SHOW : nCmdShow);
    UpdateWindow(g.hMain);

    if (auto_login) {
        LOG_INFO("开机自启动模式：自动登录，认证成功后自动退出");
        set_text(g.hStatus, "开机自启动：正在自动登录...");
        SetTimer(g.hMain, TIMER_AUTOLOGIN, 300, NULL);
    } else {
        set_text(g.hStatus, "就绪：请填写账号密码并粘贴登录页 URL 解析后点击登录");
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* 清理 */
    stop_keepalive();
    if (g.client) {
        portal_free(g.client);
        free(g.client);
    }
    KillTimer(g.hMain, TIMER_LOG);
    free(g.query_string);
    free(g.index_url);
    free(g.user_index);
    free(g.self_url);
    config_free(&g.cfg);
    DeleteObject(g.fUi);
    DeleteObject(g.fLog);
    DeleteObject(g.hBrushGray);
    return 0;
}
