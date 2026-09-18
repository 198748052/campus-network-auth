/* config.h - 配置读写（config.json）
 *
 * 配置文件位于 exe（或源码）同目录下的 config.json，便于携带分发。
 * 与 Python 版 config.py 字段保持一致。
 * 安全警示：saved_password 为本地明文存储，仅在用户勾选「记住密码」时写入。
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *portal_base;        /* 门户地址，如 http://10.10.1.101 */
    bool   remember_account;   /* 是否记住账号 */
    char  *saved_user_id;      /* 记住的账号 */
    bool   remember_password;  /* 是否记住密码（默认关闭） */
    char  *saved_password;     /* 记住的密码（明文，慎用） */
    char  *saved_query_string; /* 记住的登录参数（queryString） */
    char  *saved_index_url;    /* 对应的登录页 URL */
    bool   auto_start;         /* 开机自启动（配置记忆；实际以注册表为准） */
} AppConfig;

/* 加载配置：文件不存在/损坏时回退默认值（先调用 config_set_app_dir 指定目录） */
void config_load(AppConfig *cfg);

/* 保存配置到 config.json（静默失败，不影响主流程） */
void config_save(const AppConfig *cfg);

/* 释放配置占用内存 */
void config_free(AppConfig *cfg);

/* 指定配置目录（exe/源码所在目录，UTF-8）。默认取 exe 目录。 */
void config_set_app_dir(const char *app_dir_utf8);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
