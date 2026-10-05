@echo off
REM Сборка под Windows через MSVC (cl) без MinGW.
REM Запускать из "Developer Command Prompt for VS".
REM В CI не проверяется (там MinGW), так что это best-effort.
setlocal EnableDelayedExpansion
set SRCS=src\dl_port.c src\dl_core.c src\dl_hist.c src\tls_bear.c src\main_win32.c
for /R third_party\bearssl\src %%f in (*.c) do set SRCS=!SRCS! "%%f"
cl /nologo /O2 /W3 /Ithird_party\bearssl\inc /Ithird_party\bearssl\src !SRCS! /Fe:downloader.exe ws2_32.lib comctl32.lib comdlg32.lib gdi32.lib shell32.lib user32.lib kernel32.lib advapi32.lib crypt32.lib
