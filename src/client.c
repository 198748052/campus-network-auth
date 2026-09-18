/* client.c - ePortal 认证协议客户端实现（详见 client.h） */
#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>

#include "util.h"
#include "logger.h"
#include "rsa.h"

/* 登录最大尝试次数与重试间隔（服务端存在「同时在线上限」瞬时错误） */
#define LOGIN_MAX_ATTEMPTS 3
#define LOGIN_RETRY_INTERVAL_MS 2000

/* 会话令牌默认公钥指数 */
#define DEFAULT_EXPONENT "10001"

static void set_err(PortalError *err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
}

/* 拼接完整 URL：portal_base + endpoint */
static char *build_url(const PortalClient *pc, const char *endpoint)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, pc->portal_base);
    sb_append(&sb, endpoint);
    return sb_detach(&sb);
}

/* POST 表单并解析 JSON；失败返回 NULL 并写 err */
static cJSON *post_json(PortalClient *pc, const char *endpoint, const char *body,
                        const char *referer, PortalError *err)
{
    char *url = build_url(pc, endpoint);
    HttpResp resp;
    int rc = http_post_form(&pc->http, url, body, referer, &resp);
    free(url);
    if (rc != 0) {
        set_err(err, "请求 %s 失败: %s", endpoint, http_last_error());
        return NULL;
    }
    if (resp.status != 200) {
        set_err(err, "请求 %s 返回 HTTP %ld", endpoint, resp.status);
        http_resp_free(&resp);
        return NULL;
    }
    cJSON *j = cJSON_Parse(resp.body);
    if (!j) {
        set_err(err, "接口返回非 JSON 数据: %s", resp.body ? resp.body : "");
        http_resp_free(&resp);
        return NULL;
    }
    http_resp_free(&resp);
    return j;
}

/* 获取 JSON 字符串字段（可带多个候选键，取第一个非空值） */
static const char *jstr(const cJSON *obj, const char *k1, const char *k2)
{
    const cJSON *v = NULL;
    if (obj) {
        if (k1) {
            v = cJSON_GetObjectItemCaseSensitive(obj, k1);
            if (v && cJSON_IsString(v) && v->valuestring && *v->valuestring)
                return v->valuestring;
        }
        if (k2) {
            v = cJSON_GetObjectItemCaseSensitive(obj, k2);
            if (v && cJSON_IsString(v) && v->valuestring && *v->valuestring)
                return v->valuestring;
        }
    }
    return NULL;
}

int portal_init(PortalClient *pc, const char *portal_base, DWORD timeout_ms)
{
    memset(pc, 0, sizeof(*pc));
    pc->timeout_ms = timeout_ms ? timeout_ms : 10000;
    /* 去除末尾 '/' */
    size_t n = strlen(portal_base);
    while (n > 0 && portal_base[n - 1] == '/')
        n--;
    pc->portal_base = xstrndup(portal_base, n);
    return http_init(&pc->http, pc->timeout_ms);
}

void portal_free(PortalClient *pc)
{
    http_free(&pc->http);
    free(pc->portal_base);
    memset(pc, 0, sizeof(*pc));
}

/* ------------------------------------------------------------------ */
/* 1. 解析登录页 URL                                                    */
/* ------------------------------------------------------------------ */
int portal_parse_index_url(const char *index_url, char *out, size_t cap, PortalError *err)
{
    char *q = url_get_query(index_url);
    if (!q || !*q) {
        free(q);
        set_err(err, "登录页 URL 中未包含查询参数（queryString）");
        return -1;
    }
    if (strlen(q) >= cap) {
        free(q);
        set_err(err, "queryString 过长");
        return -1;
    }
    strcpy(out, q);
    free(q);
    return 0;
}

/* 登录所必需的接入参数（由网关注入）。任一缺失或为空，服务端都无法定位
 * 认证设备（NAS），会返回「设备未注册」等误导性错误——因此解析/登录前必须校验。 */
static const char *const REQUIRED_QS_KEYS[] = {
    "wlanuserip", "wlanacname", "ssid", "nasip", "mac", "t", "url", NULL
};

/* 校验 queryString 完整性：全部必需参数必须存在且值非空。
 * 返回 0 通过；-1 不完整（err 写入缺失项与修复提示）。 */
int portal_validate_query_string(const char *query_string, PortalError *err)
{
    if (!query_string || !*query_string) {
        set_err(err, "登录参数为空，请先在浏览器打开门户登录页，复制地址栏完整 URL 后点击「解析 URL」");
        return -1;
    }

    char *copy = xstrdup(query_string);
    char *ctx = NULL;
    char *tok = strtok_s(copy, "&", &ctx);
    bool present[7] = { false };
    while (tok) {
        char *eq = strchr(tok, '=');
        size_t klen = eq ? (size_t)(eq - tok) : strlen(tok);
        const char *val = eq ? eq + 1 : "";
        for (int i = 0; REQUIRED_QS_KEYS[i]; i++) {
            if (strlen(REQUIRED_QS_KEYS[i]) == klen &&
                strncmp(tok, REQUIRED_QS_KEYS[i], klen) == 0 && *val)
                present[i] = true;
        }
        tok = strtok_s(NULL, "&", &ctx);
    }
    free(copy);

    char missing[256] = "";
    bool ok = true;
    for (int i = 0; REQUIRED_QS_KEYS[i]; i++) {
        if (!present[i]) {
            if (missing[0])
                strcat_s(missing, sizeof(missing), "、");
            strcat_s(missing, sizeof(missing), REQUIRED_QS_KEYS[i]);
            ok = false;
        }
    }
    if (!ok) {
        set_err(err, "登录参数不完整（缺少：%s）。该参数通常由网关注入，不可手工简写；\n"
                     "请在浏览器重新打开门户登录页，复制地址栏完整 URL 后点击「解析 URL」", missing);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. 页面初始化                                                        */
/* ------------------------------------------------------------------ */
int portal_init_context(PortalClient *pc, const char *query_string,
                        const char *referer, InitContext *ctx, PortalError *err)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->public_key_modulus = NULL;
    ctx->public_key_exponent = NULL;

    LOG_INFO("开始页面初始化（pageInfo/getServices/getOnlineUserInfo）");

    /* 2.1 pageInfo：获取加密开关与 RSA 公钥 */
    char *enc_qs = urlencode_requests(query_string);
    StrBuf body;
    sb_init(&body);
    sb_append(&body, "queryString=");
    sb_append(&body, enc_qs);
    cJSON *page = post_json(pc, "/eportal/InterFace.do?method=pageInfo",
                            body.buf, referer, err);
    if (!page) {
        free(enc_qs);
        sb_free(&body);
        return -1;
    }

    /* passwordEncrypt 可能是字符串 "true"/"false" 或布尔值 */
    const cJSON *pe = cJSON_GetObjectItemCaseSensitive(page, "passwordEncrypt");
    if (pe && cJSON_IsBool(pe))
        ctx->password_encrypt = cJSON_IsTrue(pe);
    else if (pe && cJSON_IsString(pe))
        ctx->password_encrypt = (_stricmp(pe->valuestring, "true") == 0);

    const cJSON *mod = cJSON_GetObjectItemCaseSensitive(page, "publicKeyModulus");
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(page, "publicKeyExponent");
    ctx->public_key_modulus = (mod && cJSON_IsString(mod)) ? xstrdup(mod->valuestring) : xstrdup("");
    ctx->public_key_exponent = (exp && cJSON_IsString(exp) && *exp->valuestring)
                                   ? xstrdup(exp->valuestring)
                                   : xstrdup(DEFAULT_EXPONENT);
    cJSON_Delete(page);

    /* 2.2 getServices（失败忽略） */
    StrBuf body2;
    sb_init(&body2);
    sb_append(&body2, "queryString=");
    sb_append(&body2, enc_qs);
    PortalError ignore;
    cJSON *services = post_json(pc, "/eportal/InterFace.do?method=getServices",
                                body2.buf, referer, &ignore);
    if (services)
        cJSON_Delete(services);
    else
        LOG_WARNING("getServices 失败（忽略）: %s", ignore.msg);
    sb_free(&body2);
    free(enc_qs);

    /* 2.3 getOnlineUserInfo（未登录探测，失败忽略） */
    cJSON *online = post_json(pc, "/eportal/InterFace.do?method=getOnlineUserInfo",
                              "userIndex=", referer, &ignore);
    if (online) {
        ctx->online_info = online;
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(online, "result");
        ctx->already_online = (r && cJSON_IsString(r) && strcmp(r->valuestring, "success") == 0);
    } else {
        LOG_WARNING("getOnlineUserInfo 失败（忽略）: %s", ignore.msg);
        ctx->online_info = NULL;
        ctx->already_online = false;
    }

    LOG_INFO("页面初始化完成，passwordEncrypt=%s，already_online=%s",
             ctx->password_encrypt ? "true" : "false",
             ctx->already_online ? "true" : "false");
    sb_free(&body);
    return 0;
}

void portal_init_context_free(InitContext *ctx)
{
    if (!ctx)
        return;
    free(ctx->public_key_modulus);
    free(ctx->public_key_exponent);
    if (ctx->online_info)
        cJSON_Delete(ctx->online_info);
    memset(ctx, 0, sizeof(*ctx));
}

/* ------------------------------------------------------------------ */
/* 3. 登录                                                              */
/* ------------------------------------------------------------------ */
/* 单次登录请求：
 * 返回 0 成功（out_user_index 写入）；1 业务失败（last_err 写入原因）；
 * -1 网络错误（err 写入原因） */
static int do_login_once(PortalClient *pc, const char *user_id, const char *password,
                         const char *query_string, bool password_encrypt,
                         const char *modulus, const char *exponent, const char *referer,
                         char *out_user_index, size_t cap,
                         char *last_err, size_t last_err_cap, PortalError *err)
{
    /* 根据 passwordEncrypt 决定明文还是 RSA 密文 */
    const char *body_password = password;
    char *alloc = NULL;
    const char *flag = "false";
    if (password_encrypt) {
        /* 与 security.js 一致：密码超长时不加密，明文提交 */
        size_t mac_len = strlen(password) + 1 + strlen("111111111");
        if (mac_len >= 150) {
            body_password = password;
        } else {
            alloc = rsa_encrypt_password(password, modulus, exponent);
            if (!alloc) {
                set_err(err, "服务端未返回 RSA 公钥模数，无法加密密码");
                return -1;
            }
            body_password = alloc;
        }
        flag = "true";
    }

    /* 表单编码（报告 4.3 节）：前端 encodeURIComponent(encodeURIComponent(x))
     * 双重编码；这里用 form_value（encodeURIComponent + %->%25）与抓包一致 */
    char *enc_user = form_value(user_id);
    char *enc_pwd = form_value(body_password);
    char *enc_qs = form_value(query_string);
    StrBuf body;
    sb_init(&body);
    sb_printf(&body,
              "userId=%s&password=%s&service=&queryString=%s&operatorPwd="
              "&operatorUserId=&validcode=&passwordEncrypt=%s",
              enc_user, enc_pwd, enc_qs, flag);
    free(enc_user);
    free(enc_pwd);
    free(enc_qs);

    cJSON *data = post_json(pc, "/eportal/InterFace.do?method=login",
                            body.buf, referer, err);
    sb_free(&body);
    free(alloc);
    if (!data) {
        /* 网络错误 */
        return -1;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(data, "result");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(data, "message");
    const char *result_s = (result && cJSON_IsString(result)) ? result->valuestring : "";
    const char *message_s = (msg && cJSON_IsString(msg)) ? msg->valuestring : "";
    const char *user_index = jstr(data, "userIndex", NULL);

    LOG_INFO("login 请求完成，userId=%s，result=%s，message=%s", user_id, result_s, message_s);

    if (strcmp(result_s, "success") == 0) {
        if (user_index && *user_index) {
            if (strlen(user_index) < cap)
                strcpy(out_user_index, user_index);
            else
                set_err(err, "userIndex 过长");
            cJSON_Delete(data);
            return 0;
        }
        /* 服务端返回 success 却无 userIndex，属异常，直接失败不重试 */
        _snprintf(last_err, last_err_cap, "登录成功但未返回 userIndex，请重试");
        cJSON_Delete(data);
        return 1;
    }

    /* 业务失败原因（如密码错误 / 设备未注册 / 同时在线等） */
    _snprintf(last_err, last_err_cap, "%s",
              (message_s && *message_s) ? message_s : "登录失败（result=xx）");
    if (!message_s || !*message_s)
        _snprintf(last_err, last_err_cap, "登录失败（result=%s）", result_s);
    cJSON_Delete(data);
    return 1;
}

int portal_login(PortalClient *pc, const char *user_id, const char *password,
                 const char *query_string, bool password_encrypt,
                 const char *modulus, const char *exponent, const char *referer,
                 char *out_user_index, size_t cap, PortalError *err)
{
    char last_err[512] = "";
    for (int attempt = 1; attempt <= LOGIN_MAX_ATTEMPTS; attempt++) {
        PortalError perr;
        perr.msg[0] = '\0';
        int rc = do_login_once(pc, user_id, password, query_string, password_encrypt,
                               modulus, exponent, referer, out_user_index, cap,
                               last_err, sizeof(last_err), &perr);
        if (rc == 0) {
            LOG_INFO("登录成功，userIndex=%s", out_user_index);
            return 0;
        }
        if (rc < 0) {
            LOG_WARNING("登录请求异常（第 %d 次）: %s", attempt, perr.msg);
            if (attempt == LOGIN_MAX_ATTEMPTS) {
                set_err(err, "%s", perr.msg);
                return -1;
            }
            Sleep(LOGIN_RETRY_INTERVAL_MS);
            continue;
        }
        /* 并发上限属于瞬时错误，重试；其他业务错误直接抛出 */
        if (strstr(last_err, "同时在线") || strstr(last_err, "在线用户")) {
            LOG_WARNING("登录失败：%s（第 %d 次），%d 毫秒后重试",
                        last_err, attempt, LOGIN_RETRY_INTERVAL_MS);
            if (attempt == LOGIN_MAX_ATTEMPTS)
                break;
            Sleep(LOGIN_RETRY_INTERVAL_MS);
            continue;
        }
        char *full = portal_build_login_error(last_err, query_string);
        set_err(err, "%s", full ? full : last_err);
        free(full);
        return -1;
    }
    char *full = portal_build_login_error(last_err, query_string);
    set_err(err, "%s", full ? full : last_err);
    free(full);
    return -1;
}

/* ------------------------------------------------------------------ */
/* 3.5 访问登录成功页（推进服务端状态 + 获取保活间隔）                     */
/* ------------------------------------------------------------------ */
int portal_visit_success_page(PortalClient *pc, const char *user_index,
                              int keepalive_interval, const char *referer,
                              PortalError *err)
{
    StrBuf url;
    sb_init(&url);
    sb_printf(&url, "%s/eportal/success.jsp?userIndex=%s&keepaliveInterval=%d",
              pc->portal_base, user_index, keepalive_interval);
    HttpResp resp;
    int rc = http_get(&pc->http, url.buf, referer, &resp);
    sb_free(&url);
    if (rc != 0) {
        set_err(err, "访问成功页失败: %s", http_last_error());
        return keepalive_interval;
    }
    if (resp.status != 200) {
        set_err(err, "访问成功页返回 HTTP %ld", resp.status);
        http_resp_free(&resp);
        return keepalive_interval;
    }

    /* 从页面文本解析 keepaliveInterval（兼容 = : 与引号等写法） */
    int interval = keepalive_interval;
    const char *p = resp.body;
    while ((p = strstr(p, "keepaliveInterval")) != NULL) {
        p += strlen("keepaliveInterval");
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '=' || *p == ':') {
            p++;
            while (*p == ' ' || *p == '\t')
                p++;
        }
        if (*p == '"' || *p == '\'')
            p++;
        if (*p >= '0' && *p <= '9') {
            interval = atoi(p);
            break;
        }
    }
    LOG_INFO("成功页访问完成，keepaliveInterval=%d", interval);
    http_resp_free(&resp);
    return interval;
}

/* ------------------------------------------------------------------ */
/* 4. 在线状态查询 / 轮询                                               */
/* ------------------------------------------------------------------ */
int portal_get_online_info(PortalClient *pc, const char *user_index,
                           const char *referer, cJSON **out, PortalError *err)
{
    char *enc_ui = urlencode_requests(user_index);
    StrBuf body;
    sb_init(&body);
    sb_append(&body, "userIndex=");
    sb_append(&body, enc_ui);
    cJSON *j = post_json(pc, "/eportal/InterFace.do?method=getOnlineUserInfo",
                         body.buf, referer, err);
    sb_free(&body);
    free(enc_ui);
    *out = j;
    return j ? 0 : -1;
}

int portal_get_error_msg(PortalClient *pc, const char *user_index,
                         const char *referer, char **out, PortalError *err)
{
    char *enc_ui = urlencode_requests(user_index);
    StrBuf body;
    sb_init(&body);
    sb_append(&body, "userIndex=");
    sb_append(&body, enc_ui);
    char *url = build_url(pc, "/eportal/userV2.do?method=getErrorMsg");
    HttpResp resp;
    int rc = http_post_form(&pc->http, url, body.buf, referer, &resp);
    free(url);
    sb_free(&body);
    free(enc_ui);
    if (rc != 0) {
        set_err(err, "请求 getErrorMsg 失败: %s", http_last_error());
        return -1;
    }
    /* 去掉首尾空白 */
    const char *s = resp.body;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *out = xstrndup(s, (size_t)(e - s));
    http_resp_free(&resp);
    return 0;
}

int portal_wait_online(PortalClient *pc, const char *user_index,
                       const char *referer, cJSON **out, PortalError *err)
{
    const double interval_s = 2.0;
    const double max_wait_s = 30.0;
    double elapsed = 0.0;
    while (elapsed < max_wait_s) {
        PortalError perr;
        cJSON *data = NULL;
        if (portal_get_online_info(pc, user_index, referer, &data, &perr) == 0) {
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(data, "result");
            if (r && cJSON_IsString(r) && strcmp(r->valuestring, "success") == 0) {
                const cJSON *g = cJSON_GetObjectItemCaseSensitive(data, "userGroup");
                LOG_INFO("用户已上线，userGroup=%s",
                         (g && cJSON_IsString(g)) ? g->valuestring : "");
                *out = data;
                return 0;
            }
            cJSON_Delete(data);
        } else {
            LOG_WARNING("查询在线状态失败: %s", perr.msg);
        }

        char *emsg = NULL;
        if (portal_get_error_msg(pc, user_index, referer, &emsg, &perr) == 0 && emsg && *emsg)
            LOG_WARNING("认证错误信息: %s", emsg);
        free(emsg);

        elapsed += interval_s;
        if (elapsed < max_wait_s)
            Sleep((DWORD)(interval_s * 1000));
    }
    set_err(err, "等待上线超时（%.0f 秒），请检查网络与账号状态", max_wait_s);
    return -1;
}

/* ------------------------------------------------------------------ */
/* 5. 注销 / 保活                                                       */
/* ------------------------------------------------------------------ */
int portal_logout(PortalClient *pc, const char *user_index, const char *referer,
                  cJSON **out, PortalError *err)
{
    char *enc_ui = urlencode_requests(user_index);
    StrBuf body;
    sb_init(&body);
    sb_append(&body, "userIndex=");
    sb_append(&body, enc_ui);
    cJSON *j = post_json(pc, "/eportal/InterFace.do?method=logout", body.buf, referer, err);
    sb_free(&body);
    free(enc_ui);
    if (j) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "result");
        LOG_INFO("注销请求完成，result=%s",
                 (r && cJSON_IsString(r)) ? r->valuestring : "");
    }
    *out = j;
    return j ? 0 : -1;
}

int portal_keepalive(PortalClient *pc, const char *user_index, const char *referer,
                     PortalError *err)
{
    char *enc_ui = urlencode_requests(user_index);
    StrBuf body;
    sb_init(&body);
    sb_append(&body, "userIndex=");
    sb_append(&body, enc_ui);
    char *url = build_url(pc, "/eportal/InterFace.do?method=keepalive");
    HttpResp resp;
    int rc = http_post_form(&pc->http, url, body.buf, referer, &resp);
    free(url);
    sb_free(&body);
    free(enc_ui);
    if (rc != 0) {
        set_err(err, "保活请求失败: %s", http_last_error());
        return -1;
    }
    http_resp_free(&resp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 解码 / 格式化工具                                                    */
/* ------------------------------------------------------------------ */
void portal_decode_query_string(const char *query_string, cJSON *out)
{
    if (!query_string || !*query_string)
        return;
    char *copy = xstrdup(query_string);
    char *ctx = NULL;
    char *tok = strtok_s(copy, "&", &ctx);
    while (tok) {
        char key[256] = "";
        const char *value = "";
        char *eq = strchr(tok, '=');
        if (eq) {
            size_t klen = (size_t)(eq - tok);
            if (klen < sizeof(key)) {
                memcpy(key, tok, klen);
                key[klen] = '\0';
            }
            value = eq + 1;
        } else {
            if (strlen(tok) < sizeof(key))
                strcpy(key, tok);
        }

        /* hex 解码：解码结果可打印时采用，否则保留原值（加密的 url 字段） */
        unsigned char buf[1024];
        int n = hex_decode(value, buf, sizeof(buf));
        char *final = NULL;
        if (n > 0 && utf8_bytes_printable(buf, (size_t)n)) {
            final = utf8_sanitize(buf, (size_t)n);
        } else {
            final = xstrdup(value);
        }
        cJSON_AddStringToObject(out, key, final);
        free(final);
        tok = strtok_s(NULL, "&", &ctx);
    }
    free(copy);
}

void portal_decode_user_index(const char *user_index,
                              char *out_session, size_t s_cap,
                              char *out_ip, size_t i_cap,
                              char *out_id, size_t u_cap)
{
    out_session[0] = '\0';
    out_ip[0] = '\0';
    out_id[0] = '\0';
    if (!user_index || !*user_index)
        return;
    unsigned char buf[1024];
    int n = hex_decode(user_index, buf, sizeof(buf));
    if (n <= 0)
        return;
    char *raw = utf8_sanitize(buf, (size_t)n);
    size_t len = strlen(raw);

    /* 统计下划线数量 */
    int under = 0;
    for (size_t i = 0; i < len; i++)
        if (raw[i] == '_')
            under++;

    if (under < 2) {
        /* 会话键本身可能不含下划线，无法拆分出 IP/账号 */
        _snprintf(out_session, s_cap, "%s", raw);
        free(raw);
        return;
    }

    /* 从后往前找最后两个下划线 */
    const char *u2 = NULL; /* 最后一个 */
    const char *u1 = NULL; /* 倒数第二个 */
    for (const char *q = raw + len - 1; q >= raw; q--) {
        if (*q == '_') {
            if (!u2)
                u2 = q;
            else if (!u1) {
                u1 = q;
                break;
            }
        }
    }
    /* 账号 = u2+1；IP = (u1+1 .. u2-1)；会话键 = raw[0..u1) */
    if (u2 && u1) {
        _snprintf(out_id, u_cap, "%s", u2 + 1);
        if (u2 > u1 + 1)
            _snprintf(out_ip, i_cap, "%.*s", (int)(u2 - u1 - 1), u1 + 1);
        _snprintf(out_session, s_cap, "%.*s", (int)(u1 - raw), raw);
    } else {
        _snprintf(out_session, s_cap, "%s", raw);
    }
    free(raw);
}

char *portal_extract_self_url(const cJSON *online_info)
{
    if (!online_info)
        return NULL;
    const char *v = jstr(online_info, "selfUrl", NULL);
    if (!v)
        return NULL;
    /* 去除首尾空白 */
    const char *s = v;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    if (e == s)
        return NULL;
    return xstrndup(s, (size_t)(e - s));
}

char *portal_format_online_info(const cJSON *online_info, const char *user_index)
{
    if (!online_info) {
        return xstrdup("（尚未获取到完整的在线用户信息）");
    }
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(online_info, "result");
    if (!r || !cJSON_IsString(r) || strcmp(r->valuestring, "success") != 0)
        return xstrdup("（尚未获取到完整的在线用户信息）");

    StrBuf sb;
    sb_init(&sb);

    const char *user_name = jstr(online_info, "userName", NULL);
    const char *user_id = jstr(online_info, "userId", NULL);
    const char *user_ip = jstr(online_info, "userIp", NULL);
    const char *user_mac = jstr(online_info, "userMac", NULL);
    const char *user_group = jstr(online_info, "userGroup", "groupName");
    const char *package = jstr(online_info, "userPackage", "packageName");
    const char *fee = jstr(online_info, "accountFee", "fee");
    const char *left = jstr(online_info, "maxLeavingTime", "leftTime");

    sb_printf(&sb, "用户名: %s    账号: %s",
              user_name ? user_name : "-", user_id ? user_id : "-");
    sb_printf(&sb, "\r\nIP: %s    MAC: %s",
              user_ip ? user_ip : "-", user_mac ? user_mac : "-");
    sb_printf(&sb, "\r\n用户组: %s    套餐: %s",
              user_group ? user_group : "-", package ? package : "-");
    sb_printf(&sb, "\r\n费用(元): %s    剩余时长: %s",
              fee ? fee : "-", left ? left : "-");

    /* userIndex 解码：<会话ID>_<用户IP>_<账号> */
    if (user_index && *user_index) {
        char ses[512] = "", ip[128] = "", id[128] = "";
        portal_decode_user_index(user_index, ses, sizeof(ses), ip, sizeof(ip), id, sizeof(id));
        if (ses[0])
            sb_printf(&sb, "\r\n会话令牌(userIndex)解码: %s | %s | %s", ses, ip, id);
    }

    /* 自助服务免登地址 */
    char *self_url = portal_extract_self_url(online_info);
    if (self_url) {
        sb_printf(&sb, "\r\n自助服务: %s", self_url);
        free(self_url);
    }

    /* MAB（MAC 认证绑定）设备列表 */
    const cJSON *mab = cJSON_GetObjectItemCaseSensitive(online_info, "mabInfo");
    if (mab && cJSON_IsArray(mab) && cJSON_GetArraySize(mab) > 0) {
        sb_append(&sb, "\r\n绑定设备(MAB): ");
        int first = 1;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, mab) {
            const char *mac = jstr(item, "userMac", NULL);
            const char *reg = jstr(item, "mabRegister", NULL);
            if (!first)
                sb_append(&sb, "; ");
            sb_printf(&sb, "%s → %s", mac ? mac : "-", reg ? reg : "-");
            first = 0;
        }
    }
    return sb_detach(&sb);
}

char *portal_build_login_error(const char *message, const char *query_string)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, (message && *message) ? message : "登录失败");

    cJSON *decoded = cJSON_CreateObject();
    portal_decode_query_string(query_string, decoded);
    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(decoded, "wlanuserip");
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(decoded, "mac");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(decoded, "ssid");
    bool has = (ip && cJSON_IsString(ip) && *ip->valuestring) ||
               (mac && cJSON_IsString(mac) && *mac->valuestring) ||
               (ssid && cJSON_IsString(ssid) && *ssid->valuestring);
    if (has) {
        sb_append(&sb, "\r\n\r\n接入信息: ");
        int first = 1;
        if (ip && cJSON_IsString(ip) && *ip->valuestring) {
            sb_printf(&sb, "接入IP: %s", ip->valuestring);
            first = 0;
        }
        if (mac && cJSON_IsString(mac) && *mac->valuestring) {
            if (!first)
                sb_append(&sb, "，");
            sb_printf(&sb, "接入MAC: %s", mac->valuestring);
            first = 0;
        }
        if (ssid && cJSON_IsString(ssid) && *ssid->valuestring) {
            if (!first)
                sb_append(&sb, "，");
            sb_printf(&sb, "SSID: %s", ssid->valuestring);
        }
    }
    cJSON_Delete(decoded);
    return sb_detach(&sb);
}
