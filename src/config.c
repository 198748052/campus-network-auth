/* config.c - 配置读写实现（详见 config.h） */
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "third_party/cjson/cJSON.h"

/* 配置目录（UTF-8）与配置文件路径 */
static char *g_app_dir = NULL;

static void set_default(AppConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->portal_base = xstrdup("http://10.10.1.101");
    cfg->remember_account = true;
    cfg->saved_user_id = xstrdup("");
    cfg->remember_password = false;
    cfg->saved_password = xstrdup("");
    cfg->saved_query_string = xstrdup("");
    cfg->saved_index_url = xstrdup("");
    cfg->auto_start = false;
}

static char *get_config_path(void)
{
    StrBuf sb;
    sb_init(&sb);
    if (g_app_dir)
        sb_append(&sb, g_app_dir);
    else {
        char *dir = get_app_dir_utf8();
        sb_append(&sb, dir ? dir : "");
        free(dir);
    }
    if (sb.len > 0 && sb.buf[sb.len - 1] != '\\')
        sb_append_char(&sb, '\\');
    sb_append(&sb, "config.json");
    return sb_detach(&sb);
}

static void set_str(char **dst, const char *src)
{
    free(*dst);
    *dst = xstrdup(src ? src : "");
}

static bool get_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (v && cJSON_IsString(v)) {
        const char *s = cJSON_GetStringValue(v);
        return s && strcmp(s, "true") == 0;
    }
    return def;
}

static const char *get_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsString(v))
        return cJSON_GetStringValue(v);
    return "";
}

void config_load(AppConfig *cfg)
{
    set_default(cfg);

    char *path = get_config_path();
    if (!path)
        return;
    size_t len = 0;
    char *txt = file_read(path, &len);
    free(path);
    if (!txt)
        return;

    cJSON *root = cJSON_ParseWithLength(txt, len);
    if (root && cJSON_IsObject(root)) {
        set_str(&cfg->portal_base, get_str(root, "portal_base"));
        cfg->remember_account = get_bool(root, "remember_account", true);
        set_str(&cfg->saved_user_id, get_str(root, "saved_user_id"));
        cfg->remember_password = get_bool(root, "remember_password", false);
        set_str(&cfg->saved_password, get_str(root, "saved_password"));
        set_str(&cfg->saved_query_string, get_str(root, "saved_query_string"));
        set_str(&cfg->saved_index_url, get_str(root, "saved_index_url"));
        cfg->auto_start = get_bool(root, "auto_start", false);
        cJSON_Delete(root);
    }
    free(txt);
}

void config_save(const AppConfig *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;
    cJSON_AddStringToObject(root, "portal_base", cfg->portal_base ? cfg->portal_base : "");
    cJSON_AddBoolToObject(root, "remember_account", cfg->remember_account);
    cJSON_AddStringToObject(root, "saved_user_id", cfg->saved_user_id ? cfg->saved_user_id : "");
    cJSON_AddBoolToObject(root, "remember_password", cfg->remember_password);
    cJSON_AddStringToObject(root, "saved_password", cfg->saved_password ? cfg->saved_password : "");
    cJSON_AddStringToObject(root, "saved_query_string", cfg->saved_query_string ? cfg->saved_query_string : "");
    cJSON_AddStringToObject(root, "saved_index_url", cfg->saved_index_url ? cfg->saved_index_url : "");
    cJSON_AddBoolToObject(root, "auto_start", cfg->auto_start);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return;
    char *path = get_config_path();
    if (path) {
        file_write(path, json, strlen(json));
        free(path);
    }
    free(json);
}

void config_free(AppConfig *cfg)
{
    free(cfg->portal_base);
    free(cfg->saved_user_id);
    free(cfg->saved_password);
    free(cfg->saved_query_string);
    free(cfg->saved_index_url);
    memset(cfg, 0, sizeof(*cfg));
}

void config_set_app_dir(const char *app_dir_utf8)
{
    free(g_app_dir);
    g_app_dir = xstrdup(app_dir_utf8 ? app_dir_utf8 : "");
}
