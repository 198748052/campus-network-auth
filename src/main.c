/* main.c - 程序入口
 *
 * 职责：
 *   1. 解析命令行参数（--autologin 开机自启动模式）
 *   2. 初始化日志（logs 目录）与配置（config.json）
 *   3. 进入 GUI 主循环
 */
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

#include "util.h"
#include "logger.h"
#include "config.h"
#include "gui.h"

/* 程序入口（GUI 子系统，宽字符命令行） */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    /* 解析 --autologin 参数（开机自启动时由注册表 Run 项传入） */
    bool auto_login = false;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && wcscmp(argv[i], L"--autologin") == 0) {
                auto_login = true;
                break;
            }
        }
        LocalFree(argv);
    }

    /* 应用目录：exe 所在目录（开发调试时即源码目录），日志与配置均放于此 */
    char *app_dir = get_app_dir_utf8();
    logger_init(app_dir);
    config_set_app_dir(app_dir);
    free(app_dir);

    LOG_INFO("========== 校园网认证客户端启动%s ==========",
             auto_login ? "（自动登录模式）" : "");

    /* 进入 GUI 主循环（内部完成配置加载，退出后返回） */
    int rc = gui_run(hInstance, nCmdShow, auto_login);

    LOG_INFO("程序退出，返回码 %d", rc);
    logger_close();
    return rc;
}
