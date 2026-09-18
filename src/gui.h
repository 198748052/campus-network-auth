/* gui.h - 主窗口（Win32 原生 GUI）
 *
 * 布局对应 Python 版 tkinter 界面：
 *   认证设置（门户地址 / 账号密码 / 记住选项 / 登录参数解析 / 开机自启动 / 按钮+状态）
 *   在线信息（登录成功后展示完整用户信息）
 *   日志（实时滚动，同时写入 logs 目录）
 *
 * 线程模型：登录/注销在后台线程执行，通过 PostMessage 与主线程通信
 * （日志走队列由定时器轮询），保证界面不卡顿且无跨线程直接操作控件。
 */
#ifndef GUI_H
#define GUI_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 GUI 主循环。
 * auto_login: --autologin 模式（开机自启动，正常显示窗口并自动登录，认证成功后自动退出）。 */
int gui_run(HINSTANCE hInstance, int nCmdShow, bool auto_login);

#ifdef __cplusplus
}
#endif

#endif /* GUI_H */
