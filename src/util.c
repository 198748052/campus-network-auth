/* util.c - 通用工具函数实现（详见 util.h） */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ================================================================== */
/* 动态字符串                                                          */
/* ================================================================== */
void sb_init(StrBuf *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf *sb)
{
    free(sb->buf);
    sb_init(sb);
}

void sb_reserve(StrBuf *sb, size_t extra)
{
    /* 保证还能容纳 extra 字节（不含 '\0'） */
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return;
    size_t newcap = sb->cap ? sb->cap : 64;
    while (newcap < need)
        newcap *= 2;
    char *nb = (char *)realloc(sb->buf, newcap);
    if (!nb)
        return; /* 内存不足时保持原状 */
    sb->buf = nb;
    sb->cap = newcap;
}

void sb_append_n(StrBuf *sb, const char *s, size_t n)
{
    if (!s || n == 0)
        return;
    sb_reserve(sb, n);
    if (!sb->buf)
        return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s)
{
    if (s)
        sb_append_n(sb, s, strlen(s));
}

void sb_append_char(StrBuf *sb, char c)
{
    sb_append_n(sb, &c, 1);
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = _vscprintf(fmt, ap); /* 需要的字符数（不含 '\0'） */
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(sb, (size_t)n);
    if (!sb->buf) {
        va_end(ap2);
        return;
    }
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

char *sb_detach(StrBuf *sb)
{
    char *ret = sb->buf ? sb->buf : xstrdup("");
    sb_init(sb);
    return ret;
}

char *xstrdup(const char *s)
{
    if (!s)
        s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ================================================================== */
/* URL 编码                                                            */
/* ================================================================== */
static bool uri_unreserved(char c)
{
    /* encodeURIComponent 不编码的字符集（RFC 3986 未保留字符 + !'()*~） */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
    case '-': case '_': case '.': case '!': case '~':
    case '*': case '\'': case '(': case ')':
        return true;
    default:
        return false;
    }
}

static void percent_encode(StrBuf *sb, unsigned char c)
{
    static const char hex[] = "0123456789ABCDEF";
    sb_append_char(sb, '%');
    sb_append_char(sb, hex[c >> 4]);
    sb_append_char(sb, hex[c & 0x0F]);
}

char *uri_encode_component(const char *value)
{
    StrBuf sb;
    sb_init(&sb);
    const unsigned char *p = (const unsigned char *)value;
    while (*p) {
        if (uri_unreserved((char)*p))
            sb_append_char(&sb, (char)*p);
        else
            percent_encode(&sb, *p);
        p++;
    }
    return sb_detach(&sb);
}

char *form_value(const char *value)
{
    /* 先 encodeURIComponent，再把 '%' -> '%25'，形成双重编码 */
    char *enc = uri_encode_component(value);
    StrBuf sb;
    sb_init(&sb);
    const char *p = enc;
    while (*p) {
        if (*p == '%')
            sb_append(&sb, "%25");
        else
            sb_append_char(&sb, *p);
        p++;
    }
    free(enc);
    return sb_detach(&sb);
}

char *urlencode_requests(const char *value)
{
    /* 对应 urllib.parse.quote_plus：保留 A-Z a-z 0-9 - _ . ~，空格->'+'，其余 %XX */
    StrBuf sb;
    sb_init(&sb);
    const unsigned char *p = (const unsigned char *)value;
    while (*p) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            sb_append_char(&sb, (char)c);
        } else if (c == ' ') {
            sb_append_char(&sb, '+');
        } else {
            percent_encode(&sb, c);
        }
        p++;
    }
    return sb_detach(&sb);
}

char *url_get_query(const char *url)
{
    const char *q = strchr(url, '?');
    if (!q)
        return NULL;
    return xstrdup(q + 1);
}

int url_parse(const char *url, UrlParts *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = url;

    /* scheme */
    const char *colon = strstr(p, "://");
    if (!colon)
        return -1;
    size_t slen = (size_t)(colon - p);
    if (slen >= sizeof(out->scheme))
        return -1;
    memcpy(out->scheme, p, slen);
    out->scheme[slen] = '\0';
    out->port = (_stricmp(out->scheme, "https") == 0) ? 443 : 80;
    p = colon + 3;

    /* host[:port] */
    const char *host_start = p;
    const char *host_end = strchr(p, ':');
    const char *path_start = strchr(p, '/');
    if (!host_end || (path_start && path_start < host_end))
        host_end = path_start ? path_start : p + strlen(p);
    size_t hlen = (size_t)(host_end - host_start);
    if (hlen >= sizeof(out->host))
        return -1;
    memcpy(out->host, host_start, hlen);
    out->host[hlen] = '\0';

    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        const char *port_end = path_start ? path_start : port_start + strlen(port_start);
        char portbuf[16];
        size_t plen = (size_t)(port_end - port_start);
        if (plen >= sizeof(portbuf))
            return -1;
        memcpy(portbuf, port_start, plen);
        portbuf[plen] = '\0';
        out->port = atoi(portbuf);
        p = port_end;
    } else {
        p = host_end;
    }

    /* path（含 query） */
    if (*p == '/') {
        size_t plen = strlen(p);
        if (plen >= sizeof(out->path))
            return -1;
        strcpy(out->path, p);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    return 0;
}

/* ================================================================== */
/* hex 编解码                                                          */
/* ================================================================== */
char *hex_encode(const unsigned char *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    char *out = (char *)malloc(len * 2 + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int hex_decode(const char *hex, unsigned char *out, size_t out_cap)
{
    size_t n = strlen(hex);
    if (n % 2 != 0)
        return -1;
    size_t bytes = n / 2;
    if (bytes > out_cap)
        return -1;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return (int)bytes;
}

/* ================================================================== */
/* 编码转换                                                            */
/* ================================================================== */
wchar_t *utf8_to_wide(const char *utf8)
{
    if (!utf8)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * n);
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

char *wide_to_utf8(const wchar_t *w)
{
    if (!w)
        return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = (char *)malloc(n);
    if (!s)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* GBK(CP936) <-> UTF-8 */
char *gbk_to_utf8(const char *gbk)
{
    if (!gbk)
        return NULL;
    int wn = MultiByteToWideChar(936, 0, gbk, -1, NULL, 0);
    if (wn <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * wn);
    if (!w)
        return NULL;
    MultiByteToWideChar(936, 0, gbk, -1, w, wn);
    char *s = wide_to_utf8(w);
    free(w);
    return s;
}

char *utf8_to_gbk(const char *utf8)
{
    if (!utf8)
        return NULL;
    wchar_t *w = utf8_to_wide(utf8);
    if (!w)
        return NULL;
    int n = WideCharToMultiByte(936, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n ? n : 1);
    if (s && n > 0)
        WideCharToMultiByte(936, 0, w, -1, s, n, NULL, NULL);
    free(w);
    return s;
}

/* 解析一个 UTF-8 序列，返回序列长度（0 表示非法起始字节） */
static size_t utf8_seq_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

bool utf8_bytes_printable(const unsigned char *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t n = utf8_seq_len(data[i]);
        if (n == 0 || i + n > len)
            return false;
        /* 校验后续字节均为 10xxxxxx */
        for (size_t k = 1; k < n; k++) {
            if ((data[i + k] & 0xC0) != 0x80)
                return false;
        }
        /* 解码码点 */
        unsigned int cp;
        if (n == 1) cp = data[i];
        else if (n == 2) cp = ((data[i] & 0x1F) << 6) | (data[i + 1] & 0x3F);
        else if (n == 3) cp = ((data[i] & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6) | (data[i + 2] & 0x3F);
        else cp = ((data[i] & 0x07) << 18) | ((data[i + 1] & 0x3F) << 12) | ((data[i + 2] & 0x3F) << 6) | (data[i + 3] & 0x3F);
        /* Python str.isprintable()：空格可打印；控制符与换行等不可打印 */
        if (cp < 0x20 || cp == 0x7F)
            return false;
        i += n;
    }
    return true;
}

char *utf8_sanitize(const unsigned char *data, size_t len)
{
    StrBuf sb;
    sb_init(&sb);
    size_t i = 0;
    while (i < len) {
        size_t n = utf8_seq_len(data[i]);
        bool valid = (n != 0 && i + n <= len);
        if (valid) {
            for (size_t k = 1; k < n; k++) {
                if ((data[i + k] & 0xC0) != 0x80) { valid = false; break; }
            }
        }
        if (valid) {
            sb_append_n(&sb, (const char *)(data + i), n);
            i += n;
        } else {
            sb_append(&sb, "\xEF\xBF\xBD"); /* U+FFFD */
            i++;
        }
    }
    return sb_detach(&sb);
}

/* ================================================================== */
/* 时间                                                                */
/* ================================================================== */
void now_datetime(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void now_date(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(out, cap, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

/* ================================================================== */
/* 文件与路径                                                          */
/* ================================================================== */
char *file_read(const char *path_utf8, size_t *out_len)
{
    wchar_t *wp = utf8_to_wide(path_utf8);
    if (!wp)
        return NULL;
    FILE *f = _wfopen(wp, L"rb");
    free(wp);
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len)
        *out_len = rd;
    return buf;
}

int file_write(const char *path_utf8, const void *data, size_t len)
{
    wchar_t *wp = utf8_to_wide(path_utf8);
    if (!wp)
        return -1;
    FILE *f = _wfopen(wp, L"wb");
    free(wp);
    if (!f)
        return -1;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return (wr == len) ? 0 : -1;
}

char *get_exe_path_utf8(void)
{
    wchar_t buf[2048];
    DWORD n = GetModuleFileNameW(NULL, buf, 2048);
    if (n == 0 || n >= 2048)
        return NULL;
    return wide_to_utf8(buf);
}

char *get_app_dir_utf8(void)
{
    char *exe = get_exe_path_utf8();
    if (!exe)
        return xstrdup("");
    char *slash = strrchr(exe, '\\');
    if (slash)
        *slash = '\0';
    return exe;
}
