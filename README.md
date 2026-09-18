# 校园网认证客户端（C 版）

Windows 平台下的校园网 ePortal 门户认证客户端，使用 Win32 原生 GUI 编写。
本程序仅用**用户本人账号**通过官方门户的标准认证接口完成登录、保活与注销，零第三方运行时依赖。

## 功能特性

- 门户认证：账号密码登录校园网 ePortal 门户（H3C/锐捷等常见 portal）
- 密码加密：遵循登录页 `security.js` 逻辑，使用服务端下发的 RSA 公钥（Windows CNG/BCrypt）对密码做 PKCS#1 v1.5 加密传输
- 参数解析：粘贴浏览器登录页地址栏 URL，自动解析 `queryString` 接入参数（`wlanuserip` / `wlanacname` / `ssid` / `nasip` / `mac` 等）并校验完整性
- 在线信息：登录成功后展示用户名、账号、IP、MAC、用户组、套餐、费用、剩余时长等
- 保活与注销：按服务端返回的 `keepaliveInterval` 定时保活，支持手动注销
- 开机自启动：支持 `--autologin` 参数（注册表 Run 项），认证成功后自动退出
- 日志：实时滚动显示并写入 `logs\` 目录（按日期分文件）
- 配置持久化：账号、门户地址等保存到 `config.json`（位于 exe 同目录，便于携带）

## 环境要求

- Windows 10 / 11 x64
- 编译需要：Visual Studio 2022/2026 Community + Windows SDK（MSVC x64 工具链）
- 运行无需安装任何运行时库

## 编译

直接运行项目根目录下的 `build.bat`：

```
build.bat
```

成功后生成 `campus_auth.exe`。

> 注意：`build.bat` 中硬编码了本机 MSVC 版本路径（`...\MSVC\14.51.36231`），
> 若本机版本不同，请修改脚本开头的 `VC` / `SDK` / `SDKLIB` 变量，或改用自动探测方式。

## 使用方法

1. 打开浏览器访问校园网门户登录页，在地址栏**复制完整 URL**（含 `queryString` 参数）。
2. 运行 `campus_auth.exe`，将 URL 粘贴到"登录参数"输入框，点击「解析 URL」。
3. 填写账号、密码，按需勾选"记住账号 / 记住密码 / 开机自启动"，点击「登录」。
4. 登录成功后窗口展示在线信息；关闭程序或点击「注销」即可下线。

开机自启动模式（配合注册表 Run 项）：

```
campus_auth.exe --autologin
```

## 配置文件说明

配置文件为 exe 同目录下的 `config.json`（首次运行自动生成）：

| 字段 | 说明 |
|------|------|
| `portal_base` | 门户地址，如 `http://10.10.1.101` |
| `remember_account` | 是否记住账号 |
| `saved_user_id` | 记住的账号 |
| `remember_password` | 是否记住密码（默认关闭） |
| `saved_password` | 记住的密码（**本地明文存储，慎用**） |
| `saved_query_string` | 记住的登录参数 |
| `saved_index_url` | 对应的登录页 URL |
| `auto_start` | 开机自启动标记 |

> 安全提示：`saved_password` 为本地明文存储，仅在勾选"记住密码"时写入，
> 请勿在公共/他人电脑上启用，并妥善保管 `config.json`。

## 目录结构

```
.
├── build.bat            # 构建脚本（MSVC）
├── src/                 # 源码
│   ├── main.c           # 程序入口（--autologin 解析、日志/配置初始化）
│   ├── gui.c            # Win32 主窗口与后台线程
│   ├── client.c         # ePortal 认证协议客户端
│   ├── http.c           # WinINet HTTP 客户端
│   ├── rsa.c            # RSA 加密（Windows CNG/BCrypt）
│   ├── config.c         # config.json 读写
│   ├── logger.c         # 日志模块
│   └── util.c           # 通用工具
└── third_party/cjson/   # 内置的 cJSON 库（已静态编译）
```

## 使用边界与免责声明

- 本程序仅用于**本人账号**在校园网环境下的正常认证，不包含任何破解、绕过、
  暴力破解、未授权访问或攻击性功能。
- 部分校园网规定仅允许官方客户端接入，使用第三方客户端可能违反所在学校
  的**网络管理规定**，可能导致限速或账号受限。请在使用前确认学校政策，
  由此产生的后果由使用者自行承担。
- 请勿将本程序用于任何未授权用途，也不要向 `config.json` 之外提交任何
  真实账号信息（`config.json` 已加入 `.gitignore`）。
