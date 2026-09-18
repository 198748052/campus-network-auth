/* util.h - 通用工具函数：动态字符串、URL 编码、hex、编码转换、时间、文件
 *
 * 说明：本项目全部使用 UTF-8 窄字符串作为内部表示；与 Windows API 交互时
 * 通过 utf8_to_wide / wide_to_utf8 转换。网络层与 JSON 均为 UTF-8。
 */
#ifndef UTIL_H
#define UTIL_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 动态字符串（自动扩容，始终以 '\0' 结尾）                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *buf;      /* 缓冲区（含结尾 '\0'） */
    size_t len;      /* 当前字符串长度（不含 '\0'） */
    size_t cap;      /* 缓冲区容量 */
} StrBuf;

void    sb_init(StrBuf *sb);
void    sb_free(StrBuf *sb);
void    sb_reserve(StrBuf *sb, size_t extra);      /* 保证还能追加 extra 字节 */
void    sb_append(StrBuf *sb, const char *s);
void    sb_append_n(StrBuf *sb, const char *s, size_t n);
void    sb_append_char(StrBuf *sb, char c);
void    sb_printf(StrBuf *sb, const char *fmt, ...);
char   *sb_detach(StrBuf *sb);                     /* 取出缓冲区（调用方负责 free），并重置 sb */

/* 字符串复制（malloc） */
char   *xstrdup(const char *s);
char   *xstrndup(const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* URL 编码（模拟 JavaScript / Python 行为）                            */
/* ------------------------------------------------------------------ */
/* encodeURIComponent 模拟：仅保留 RFC3986 未保留字符
 * A-Z a-z 0-9 - _ . ! ~ * ' ( )，其余全部 %XX 编码（UTF-8 字节级）。 */
char   *uri_encode_component(const char *value);

/* 对应 Python 端 _form_value：先 encodeURIComponent，再把每个 '%' 编码为 %25，
 * 配合请求端再次 %XX 编码形成与浏览器抓包一致的双重编码（%3D -> %253D）。 */
char   *form_value(const char *value);

/* 对应 requests/urllib 的 urlencode(quote_plus)：保留 A-Z a-z 0-9 - _ . ~，
 * 空格转 '+',其余 %XX 编码。用于 pageInfo 等接口的 queryString 单次编码。 */
char   *urlencode_requests(const char *value);

/* 解析 URL，返回查询串（'?' 之后部分）；无查询参数返回 NULL */
char   *url_get_query(const char *url);

/* 解析 URL 的 scheme/host/port/path(含 query)，返回 0 成功 */
typedef struct {
    char scheme[16];
    char host[256];
    int  port;
    char path[1024];   /* 形如 /eportal/index.jsp?wlanuserip=... */
} UrlParts;
int     url_parse(const char *url, UrlParts *out);

/* ------------------------------------------------------------------ */
/* hex 编解码                                                           */
/* ------------------------------------------------------------------ */
char   *hex_encode(const unsigned char *data, size_t len);  /* 小写 */
int     hex_decode(const char *hex, unsigned char *out, size_t out_cap); /* 返回字节数，失败 -1 */

/* ------------------------------------------------------------------ */
/* 编码转换：UTF-8 <-> UTF-16 <-> GBK(CP936)                            */
/* ------------------------------------------------------------------ */
wchar_t *utf8_to_wide(const char *utf8);
char    *wide_to_utf8(const wchar_t *w);
char    *utf8_to_gbk(const char *utf8);
char    *gbk_to_utf8(const char *gbk);

/* 若 bytes 是合法 UTF-8 且全部可打印（含空格）返回 true */
bool     utf8_bytes_printable(const unsigned char *data, size_t len);
/* 把任意字节串按 UTF-8 解码（非法字节替换为 U+FFFD），输出 malloc 字符串 */
char    *utf8_sanitize(const unsigned char *data, size_t len);

/* ------------------------------------------------------------------ */
/* 时间                                                                 */
/* ------------------------------------------------------------------ */
void    now_datetime(char *out, size_t cap);   /* YYYY-MM-DD HH:MM:SS */
void    now_date(char *out, size_t cap);       /* YYYY-MM-DD */

/* ------------------------------------------------------------------ */
/* 文件与路径                                                           */
/* ------------------------------------------------------------------ */
/* 读取整个文件（二进制），返回 malloc 缓冲区（可能含 \0），长度写 out_len */
char   *file_read(const char *path_utf8, size_t *out_len);
/* 写文件，返回 0 成功 */
int     file_write(const char *path_utf8, const void *data, size_t len);

/* 当前可执行文件完整路径（UTF-8） */
char   *get_exe_path_utf8(void);
/* 当前可执行文件所在目录（UTF-8，结尾无反斜杠） */
char   *get_app_dir_utf8(void);

#ifdef __cplusplus
}
#endif

#endif /* UTIL_H */
