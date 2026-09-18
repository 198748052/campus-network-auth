@echo off
rem ============================================================
rem  Campus Auth Client - C version build script
rem  Requires: VS2026 Community MSVC (14.51) + Windows SDK 10.0.26100.0
rem  No third-party runtime deps (cJSON is bundled in third_party\cjson)
rem  Usage: build.bat
rem ============================================================
setlocal enabledelayedexpansion

set "VC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
set "SDKLIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

set "CL_EXE=%VC%\bin\Hostx64\x64\cl.exe"

if not exist "%CL_EXE%" (
    echo [ERROR] MSVC compiler not found: %CL_EXE%
    echo Install VS2022/VS2026 Community, and verify the MSVC version
    echo 14.51.36231 or update the VC path in this script.
    exit /b 1
)

set "INCLUDE=%VC%\include;%SDK%\shared;%SDK%\ucrt;%SDK%\um"
set "LIB=%VC%\lib\x64;%SDKLIB%\ucrt\x64;%SDKLIB%\um\x64"

echo [1/2] Compiling...
"%CL_EXE%" /nologo /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
  /DWIN32_LEAN_AND_MEAN ^
  /I. /Isrc /Ithird_party\cjson ^
  src\main.c src\gui.c src\client.c src\http.c src\rsa.c src\logger.c src\config.c src\util.c third_party\cjson\cJSON.c ^
  /Fe:campus_auth.exe ^
  /link /SUBSYSTEM:WINDOWS ^
  wininet.lib bcrypt.lib ws2_32.lib iphlpapi.lib shell32.lib user32.lib gdi32.lib advapi32.lib ole32.lib

if errorlevel 1 (
    echo [ERROR] Build failed. Fix the errors above and retry.
    exit /b 1
)

echo [2/2] Done: campus_auth.exe
endlocal
