/* logger.h - 日志模块
 *
 * 设计（对应 Python 版 eportal/logger.py）：
 *   1. 写入 logs/eportal_YYYY-MM-DD.log（UTF-8），保证留痕
 *   2. 同时写入内存队列，供 GUI 日志面板定时拉取显示（线程安全）
 * 安全要求：调用方写入日志时一律对密码脱敏（绝不输出明文密码）。
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_LEVEL_DEBUG   0
#define LOG_LEVEL_INFO    1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_ERROR   3

/* 初始化：创建 logs 目录并打开当日日志文件；返回 0 成功。
 * app_dir_utf8 为 exe（或源码）所在目录（UTF-8）。 */
int  logger_init(const char *app_dir_utf8);
void logger_close(void);

/* 写日志（线程安全），消息自动追加换行 */
void logger_log(int level, const char *fmt, ...);

/* 便捷宏 */
#define LOG_DEBUG(...)    logger_log(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)     logger_log(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARNING(...)  logger_log(LOG_LEVEL_WARNING, __VA_ARGS__)
#define LOG_ERROR(...)    logger_log(LOG_LEVEL_ERROR, __VA_ARGS__)

/* GUI 消费：从队列取出一条日志（malloc，调用方 free）；空队列返回 NULL */
char *logger_queue_pop(void);

/* 当日日志文件完整路径（malloc，UTF-8） */
char *logger_get_file_path(void);

#ifdef __cplusplus
}
#endif

#endif /* LOGGER_H */
