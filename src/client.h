/* client.h - ePortal 认证协议客户端
 *
 * 基于协议分析实现的完整登录流程（对应 Python 版 eportal/client.py）：
 *   1. 解析登录页 URL，提取 queryString（网关注入的会话上下文参数）
 *   2. 页面初始化：pageInfo / getServices / getOnlineUserInfo（未登录探测）
 *   3. 登录：InterFace.do?method=login（支持明文与 RSA 加密两种模式）
 *   4. 轮询在线状态：getOnlineUserInfo / userV2.do?method=getErrorMsg
 *
 * 安全说明：所有日志一律对密码脱敏，绝不输出明文密码。
 */
#ifndef CLIENT_H
#define CLIENT_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdbool.h>

#include "http.h"
#include "third_party/cjson/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 门户交互错误信息（调用方传入，失败时写入中文描述） */
typedef struct {
    char msg[2048];
} PortalError;

typedef struct {
    bool   password_encrypt;        /* 是否开启密码 RSA 加密 */
    char  *public_key_modulus;      /* RSA 公钥模数（hex） */
    char  *public_key_exponent;     /* RSA 公钥指数（hex） */
    bool   already_online;          /* 初始化时已在线（result==success） */
    cJSON *online_info;             /* 在线信息原始 JSON（可能为 NULL，调用方负责释放） */
} InitContext;

/* ePortal 客户端（对应 requests.Session，复用连接 + 维持 Cookie） */
typedef struct {
    char       *portal_base;   /* 门户根地址，如 http://10.10.1.101 */
    DWORD       timeout_ms;
    HttpClient  http;
} PortalClient;

/* 初始化 / 释放 */
int  portal_init(PortalClient *pc, const char *portal_base, DWORD timeout_ms);
void portal_free(PortalClient *pc);

/* ------------------------------------------------------------------ */
/* 1. 解析登录页 URL → queryString（原样返回查询串）                     */
/* 返回 0 成功；-1 失败（无查询参数），错误写入 err                       */
/* ------------------------------------------------------------------ */
int portal_parse_index_url(const char *index_url, char *out, size_t cap, PortalError *err);

/* 校验 queryString 完整性：全部必需接入参数（wlanuserip/wlanacname/ssid/
 * nasip/mac/t/url）必须存在且值非空，否则服务端无法定位 NAS，会返回
 * 「设备未注册」等误导性错误。返回 0 通过；-1 不完整（err 写入缺失项） */
int portal_validate_query_string(const char *query_string, PortalError *err);

/* ------------------------------------------------------------------ */
/* 2. 页面初始化：pageInfo / getServices / getOnlineUserInfo            */
/* ------------------------------------------------------------------ */
int portal_init_context(PortalClient *pc, const char *query_string,
                        const char *referer, InitContext *ctx, PortalError *err);

/* 释放 InitContext（含 online_info） */
void portal_init_context_free(InitContext *ctx);

/* ------------------------------------------------------------------ */
/* 3. 登录（含「同时在线上限」自动重试），成功返回 0 并写入 out_user_index */
/* ------------------------------------------------------------------ */
int portal_login(PortalClient *pc, const char *user_id, const char *password,
                 const char *query_string, bool password_encrypt,
                 const char *modulus, const char *exponent, const char *referer,
                 char *out_user_index, size_t cap, PortalError *err);

/* ------------------------------------------------------------------ */
/* 3.5 访问登录成功页，返回保活间隔秒数（解析失败返回传入值）              */
/* ------------------------------------------------------------------ */
int portal_visit_success_page(PortalClient *pc, const char *user_index,
                              int keepalive_interval, const char *referer,
                              PortalError *err);

/* ------------------------------------------------------------------ */
/* 4. 在线状态查询 / 轮询                                               */
/* ------------------------------------------------------------------ */
/* 查询在线信息：成功返回 0，*out 为 malloc 的 cJSON（调用方 cJSON_Delete） */
int portal_get_online_info(PortalClient *pc, const char *user_index,
                           const char *referer, cJSON **out, PortalError *err);

/* 查询认证错误信息（getErrorMsg），成功返回 0，out 为 malloc 文本 */
int portal_get_error_msg(PortalClient *pc, const char *user_index,
                         const char *referer, char **out, PortalError *err);

/* 轮询等待上线：直至 result==success 或超时（默认 30s） */
int portal_wait_online(PortalClient *pc, const char *user_index,
                       const char *referer, cJSON **out, PortalError *err);

/* ------------------------------------------------------------------ */
/* 5. 注销 / 保活                                                       */
/* ------------------------------------------------------------------ */
int portal_logout(PortalClient *pc, const char *user_index, const char *referer,
                  cJSON **out, PortalError *err);
int portal_keepalive(PortalClient *pc, const char *user_index, const char *referer,
                     PortalError *err);

/* ------------------------------------------------------------------ */
/* 解码 / 格式化工具                                                    */
/* ------------------------------------------------------------------ */
/* 解析并 hex 解码 queryString，输出到 out 对象（调用方先 cJSON_CreateObject） */
void portal_decode_query_string(const char *query_string, cJSON *out);

/* 解码 userIndex（hex(<会话ID>_<用户IP>_<账号>)） */
void portal_decode_user_index(const char *user_index,
                              char *out_session, size_t s_cap,
                              char *out_ip, size_t i_cap,
                              char *out_id, size_t u_cap);

/* 提取自助服务免登地址（selfUrl） */
char *portal_extract_self_url(const cJSON *online_info);

/* 把在线信息整理为多行可读文本（返回 malloc，UTF-8） */
char *portal_format_online_info(const cJSON *online_info, const char *user_index);

/* 构造登录失败错误文本：附加解码后的接入信息（MAC/IP/SSID） */
char *portal_build_login_error(const char *message, const char *query_string);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_H */
