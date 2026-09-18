/* http.h - 基于 WinINet 的轻量 HTTP 客户端
 *
 * 设计目标：复刻 Python requests.Session 的关键行为——
 *   1. 会话 Cookie 持久化（Set-Cookie 收集、请求时自动携带）
 *   2. 表单 POST / GET，统一超时
 *   3. 响应体自动解码为 UTF-8（UTF-8 优先，GBK 兜底）
 * 依赖系统自带 wininet.dll，零第三方依赖。
 */
#ifndef HTTP_H
#define HTTP_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <wininet.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP 响应 */
typedef struct {
    long   status;     /* HTTP 状态码（200 等） */
    char  *body;       /* 响应体，已解码为 UTF-8（malloc，http_resp_free 释放） */
    size_t body_len;
} HttpResp;

/* HTTP 客户端（对应 requests.Session） */
typedef struct {
    HINTERNET hInternet;   /* InternetOpen 句柄 */
    char     *cookie;      /* 会话 Cookie 串："name=value; name2=value2" */
    DWORD     timeout_ms;  /* 单次请求超时（毫秒） */
    char     *user_agent;
} HttpClient;

/* 初始化；返回 0 成功，-1 失败 */
int  http_init(HttpClient *c, DWORD timeout_ms);
void http_free(HttpClient *c);

/* POST application/x-www-form-urlencoded 表单
 * body 为已经编码好的请求体（如 "queryString=...&userId=..."）
 * referer 可为 NULL。
 * 返回 0 表示请求成功（含 HTTP 非 200，此时看 resp->status）；
 * 返回 -1 表示网络层失败（无法连接/超时）。
 */
int  http_post_form(HttpClient *c, const char *url, const char *body,
                    const char *referer, HttpResp *resp);

/* GET 请求 */
int  http_get(HttpClient *c, const char *url, const char *referer, HttpResp *resp);

/* 释放响应资源 */
void http_resp_free(HttpResp *resp);

/* 最近一次网络错误描述（UTF-8，供日志） */
const char *http_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_H */
