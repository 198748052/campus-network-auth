/* http.c - WinINet HTTP 客户端实现（详见 http.h） */
#include "http.h"

#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#pragma comment(lib, "wininet.lib")

static char g_last_error[512] = "";

const char *http_last_error(void)
{
    return g_last_error;
}

static void set_error(const char *msg)
{
    _snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

/* 读取响应体全部字节 */
static char *read_all(HINTERNET hReq, size_t *out_len)
{
    StrBuf sb;
    sb_init(&sb);
    char tmp[8192];
    DWORD got = 0;
    for (;;) {
        if (!InternetReadFile(hReq, tmp, sizeof(tmp), &got) || got == 0)
            break;
        sb_append_n(&sb, tmp, got);
    }
    *out_len = sb.len;
    return sb_detach(&sb);
}

/* 查询一个 HTTP 头，成功返回 malloc 字符串 */
static char *query_header(HINTERNET hReq, DWORD info)
{
    DWORD size = 0;
    HttpQueryInfoW(hReq, info, NULL, &size, NULL);
    if (size == 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc(size + 2);
    if (!w)
        return NULL;
    DWORD n = size;
    if (!HttpQueryInfoW(hReq, info, w, &n, NULL)) {
        free(w);
        return NULL;
    }
    w[n / sizeof(wchar_t)] = L'\0';
    char *s = wide_to_utf8(w);
    free(w);
    return s;
}

/* 从 Set-Cookie 头中更新会话 Cookie（name=value; ... 取第一段） */
static void update_cookies(HttpClient *c, const char *set_cookie_all)
{
    if (!set_cookie_all || !*set_cookie_all)
        return;
    const char *p = set_cookie_all;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (n > 0 && n < 2048) {
            char line[2048];
            memcpy(line, p, n);
            line[n] = '\0';
            /* 忽略 httpOnly 等标记行（不含 '=' 的行） */
            char *eq = strchr(line, '=');
            if (eq) {
                char *semi = strchr(line, ';');
                size_t vlen = semi ? (size_t)(semi - eq - 1) : strlen(eq + 1);
                size_t nlen = (size_t)(eq - line);
                char name[512], value[2048];
                if (nlen > 0 && nlen < sizeof(name) && vlen < sizeof(value)) {
                    memcpy(name, line, nlen);
                    name[nlen] = '\0';
                    memcpy(value, eq + 1, vlen);
                    value[vlen] = '\0';
                    /* 更新：移除同名旧值，再追加 */
                    StrBuf sb;
                    sb_init(&sb);
                    /* 逐对检查 */
                    if (c->cookie && *c->cookie) {
                        /* 拆分为 "k=v" 对，跳过分隔 "; " */
                        const char *q = c->cookie;
                        while (*q) {
                            const char *sp = strstr(q, "; ");
                            size_t plen = sp ? (size_t)(sp - q) : strlen(q);
                            if (plen > 0) {
                                /* 提取该对的名字（'=' 之前） */
                                char pair[2048];
                                if (plen < sizeof(pair)) {
                                    memcpy(pair, q, plen);
                                    pair[plen] = '\0';
                                    char *peq = strchr(pair, '=');
                                    if (peq) {
                                        *peq = '\0';
                                        if (strcmp(pair, name) != 0) {
                                            if (sb.len > 0)
                                                sb_append(&sb, "; ");
                                            sb_append(&sb, pair);
                                            sb_append_char(&sb, '=');
                                            sb_append(&sb, peq + 1);
                                        }
                                    }
                                }
                            }
                            q = sp ? sp + 2 : q + plen;
                        }
                    }
                    if (sb.len > 0)
                        sb_append(&sb, "; ");
                    sb_append(&sb, name);
                    sb_append_char(&sb, '=');
                    sb_append(&sb, value);
                    free(c->cookie);
                    c->cookie = sb_detach(&sb);
                }
            }
        }
        p = eol ? eol + 2 : p + n;
    }
}

/* 根据 Content-Type 的 charset 与内容合法性把原始字节解码为 UTF-8 */
static char *decode_body(const unsigned char *data, size_t len, const char *content_type)
{
    if (len == 0)
        return xstrdup("");
    bool has_charset = false;
    bool charset_gbk = false;
    bool charset_utf8 = false;
    if (content_type) {
        const char *cs = strstr(content_type, "charset");
        if (cs) {
            has_charset = true;
            const char *p = cs + 7;
            while (*p && (*p == ' ' || *p == '=' || *p == '"' || *p == '\''))
                p++;
            charset_utf8 = (_strnicmp(p, "utf-8", 5) == 0) || (_strnicmp(p, "utf8", 4) == 0);
            charset_gbk = (_strnicmp(p, "gbk", 3) == 0) || (_strnicmp(p, "gb2312", 6) == 0) || (_strnicmp(p, "gb18030", 7) == 0);
        }
    }
    /* 响应为 JSON 但无 charset 时 Python 显式按 UTF-8 处理，这里用合法性探测兜底 */
    if (has_charset) {
        if (charset_utf8)
            return utf8_sanitize(data, len);
        if (charset_gbk)
            return gbk_to_utf8((const char *)data);
        /* 其他编码（ISO-8859-1 等）：按 UTF-8 合法性探测 */
    }
    /* 无 charset 或未知编码：合法 UTF-8 直接使用，否则按 GBK 转 */
    if (utf8_bytes_printable(data, len) || len == 0) {
        return xstrndup((const char *)data, len);
    }
    return gbk_to_utf8((const char *)data);
}

/* 核心请求 */
static int do_request(HttpClient *c, const char *method, const char *url,
                      const char *body, const char *referer, HttpResp *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->status = -1;

    UrlParts up;
    if (url_parse(url, &up) != 0) {
        set_error("URL 解析失败");
        return -1;
    }

    wchar_t *whost = utf8_to_wide(up.host);
    wchar_t *wobj = utf8_to_wide(up.path);
    if (!whost || !wobj) {
        free(whost);
        free(wobj);
        set_error("URL 转宽字符失败");
        return -1;
    }

    HINTERNET hConn = InternetConnectW(c->hInternet, whost, (INTERNET_PORT)up.port,
                                       NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    free(whost);
    if (!hConn) {
        free(wobj);
        set_error("InternetConnect 失败（网络不可达？）");
        return -1;
    }

    DWORD flags = INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_CACHE_WRITE;
    if (up.port == 443 || _stricmp(up.scheme, "https") == 0)
        flags |= INTERNET_FLAG_SECURE;

    wchar_t *wreferer = referer && *referer ? utf8_to_wide(referer) : NULL;
    HINTERNET hReq = HttpOpenRequestW(hConn, method[0] == 'G' ? L"GET" : L"POST", wobj,
                                      NULL, wreferer, NULL, flags, 0);
    free(wobj);
    free(wreferer);
    if (!hReq) {
        InternetCloseHandle(hConn);
        set_error("HttpOpenRequest 失败");
        return -1;
    }

    /* 超时设置 */
    DWORD tmo = c->timeout_ms;
    InternetSetOptionW(hReq, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionW(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionW(hReq, INTERNET_OPTION_SEND_TIMEOUT, &tmo, sizeof(tmo));

    /* 附加请求头：浏览器 UA、Accept、Cookie */
    StrBuf headers;
    sb_init(&headers);
    if (c->user_agent)
        sb_printf(&headers, "User-Agent: %s\r\n", c->user_agent);
    sb_append(&headers,
              "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
              "Accept-Language: zh-CN,zh;q=0.9\r\n");
    if (c->cookie && *c->cookie) {
        sb_append(&headers, "Cookie: ");
        sb_append(&headers, c->cookie);
        sb_append(&headers, "\r\n");
    }
    if (method[0] != 'G') {
        sb_append(&headers, "Content-Type: application/x-www-form-urlencoded\r\n");
    }
    sb_append(&headers, "\r\n");
    wchar_t *wheaders = utf8_to_wide(headers.buf);
    sb_free(&headers);

    BOOL ok;
    if (method[0] != 'G') {
        ok = HttpSendRequestW(hReq, wheaders, (DWORD)wcslen(wheaders),
                              (LPVOID)body, (DWORD)strlen(body ? body : ""));
    } else {
        ok = HttpSendRequestW(hReq, wheaders, (DWORD)wcslen(wheaders), NULL, 0);
    }
    free(wheaders);
    if (!ok) {
        DWORD err = GetLastError();
        char msg[128];
        _snprintf(msg, sizeof(msg), "HTTP 请求失败（错误码 %lu）", err);
        set_error(msg);
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        return -1;
    }

    /* 状态码 */
    DWORD code = 0;
    DWORD csz = sizeof(code);
    if (!HttpQueryInfoW(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &csz, NULL))
        code = 0;
    resp->status = (long)code;

    /* Set-Cookie */
    char *sc = query_header(hReq, HTTP_QUERY_SET_COOKIE);
    if (sc) {
        update_cookies(c, sc);
        free(sc);
    }

    /* Content-Type */
    char *ct = query_header(hReq, HTTP_QUERY_CONTENT_TYPE);

    /* 响应体 */
    size_t blen = 0;
    char *raw = read_all(hReq, &blen);

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);

    if (raw) {
        resp->body = decode_body((const unsigned char *)raw, blen, ct);
        resp->body_len = strlen(resp->body);
        free(raw);
    } else {
        resp->body = xstrdup("");
        resp->body_len = 0;
    }
    free(ct);
    return 0;
}

int http_init(HttpClient *c, DWORD timeout_ms)
{
    memset(c, 0, sizeof(*c));
    c->timeout_ms = timeout_ms ? timeout_ms : 10000;
    c->hInternet = InternetOpenW(L"CampusAuth/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!c->hInternet) {
        set_error("InternetOpen 失败");
        return -1;
    }
    c->user_agent = xstrdup(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    return 0;
}

void http_free(HttpClient *c)
{
    if (c->hInternet)
        InternetCloseHandle(c->hInternet);
    free(c->cookie);
    free(c->user_agent);
    memset(c, 0, sizeof(*c));
}

int http_post_form(HttpClient *c, const char *url, const char *body,
                   const char *referer, HttpResp *resp)
{
    return do_request(c, "POST", url, body ? body : "", referer, resp);
}

int http_get(HttpClient *c, const char *url, const char *referer, HttpResp *resp)
{
    return do_request(c, "GET", url, NULL, referer, resp);
}

void http_resp_free(HttpResp *resp)
{
    if (resp) {
        free(resp->body);
        memset(resp, 0, sizeof(*resp));
    }
}
