/* logger.c - 日志模块实现（详见 logger.h） */
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "util.h"

/* 队列节点 */
typedef struct QNode {
    char *msg;
    struct QNode *next;
} QNode;

static CRITICAL_SECTION g_lock;
static FILE *g_file = NULL;
static char *g_log_dir = NULL;   /* UTF-8 */
static QNode *g_head = NULL;
static QNode *g_tail = NULL;
static bool g_inited = false;

static const char *level_name(int level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG:   return "DEBUG";
    case LOG_LEVEL_INFO:    return "INFO";
    case LOG_LEVEL_WARNING: return "WARNING";
    case LOG_LEVEL_ERROR:   return "ERROR";
    default:                return "INFO";
    }
}

int logger_init(const char *app_dir_utf8)
{
    if (g_inited)
        return 0;
    InitializeCriticalSection(&g_lock);

    StrBuf dir;
    sb_init(&dir);
    sb_append(&dir, app_dir_utf8 ? app_dir_utf8 : "");
    if (dir.len > 0 && dir.buf[dir.len - 1] != '\\')
        sb_append_char(&dir, '\\');
    sb_append(&dir, "logs");
    g_log_dir = sb_detach(&dir);

    /* 创建目录（目录不存在时 CreateDirectoryA 按 UTF-8 路径创建） */
    wchar_t *wdir = utf8_to_wide(g_log_dir);
    if (wdir) {
        CreateDirectoryW(wdir, NULL);
        free(wdir);
    }

    char *path = logger_get_file_path();
    if (path) {
        wchar_t *wp = utf8_to_wide(path);
        if (wp) {
            /* 注意：不能用 "a, ccs=UTF-8"。MSVC CRT 下 ccs=UTF-8 文本模式流
             * 在 fflush/fclose 时存在栈缓冲区溢出缺陷（0xC0000409）。
             * 本模块写入的 line 已是 UTF-8 字节串，直接用二进制追加模式即可。 */
            g_file = _wfopen(wp, L"ab");
            free(wp);
        }
        free(path);
    }
    g_inited = true;
    return 0;
}

void logger_close(void)
{
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    EnterCriticalSection(&g_lock);
    QNode *n = g_head;
    while (n) {
        QNode *nx = n->next;
        free(n->msg);
        free(n);
        n = nx;
    }
    g_head = g_tail = NULL;
    LeaveCriticalSection(&g_lock);
    free(g_log_dir);
    g_log_dir = NULL;
    DeleteCriticalSection(&g_lock);
    g_inited = false;
}

void logger_log(int level, const char *fmt, ...)
{
    if (!g_inited)
        return;

    char ts[32];
    now_datetime(ts, sizeof(ts));

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = _vscprintf(fmt, ap);
    va_end(ap);
    char *body = (char *)malloc((size_t)n + 1);
    if (!body) {
        va_end(ap2);
        return;
    }
    vsnprintf(body, (size_t)n + 1, fmt, ap2);
    va_end(ap2);

    /* 组装完整行：2026-09-16 10:00:00 [INFO] msg */
    size_t msglen = strlen(ts) + strlen(level_name(level)) + strlen(body) + 16;
    char *line = (char *)malloc(msglen);
    if (!line) {
        free(body);
        return;
    }
    _snprintf(line, msglen, "%s [%s] %s\r\n", ts, level_name(level), body);

    EnterCriticalSection(&g_lock);

    /* 写入文件 */
    if (g_file) {
        fwrite(line, 1, strlen(line), g_file);
        fflush(g_file);
    }

    /* 入队（供 GUI 显示，去掉换行） */
    QNode *node = (QNode *)malloc(sizeof(QNode));
    if (node) {
        size_t l = strlen(line);
        /* 去掉尾部 \r\n */
        node->msg = xstrndup(line, (l >= 2 && line[l - 2] == '\r') ? l - 2 : l);
        node->next = NULL;
        if (g_tail)
            g_tail->next = node;
        else
            g_head = node;
        g_tail = node;
    }

    LeaveCriticalSection(&g_lock);

    free(line);
    free(body);
}

char *logger_queue_pop(void)
{
    EnterCriticalSection(&g_lock);
    QNode *n = g_head;
    if (!n) {
        LeaveCriticalSection(&g_lock);
        return NULL;
    }
    g_head = n->next;
    if (!g_head)
        g_tail = NULL;
    char *msg = n->msg;
    free(n);
    LeaveCriticalSection(&g_lock);
    return msg;
}

char *logger_get_file_path(void)
{
    if (!g_log_dir)
        return NULL;
    char date[16];
    now_date(date, sizeof(date));
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, g_log_dir);
    sb_append(&sb, "\\eportal_");
    sb_append(&sb, date);
    sb_append(&sb, ".log");
    return sb_detach(&sb);
}
