@echo off
REM Сборка под Windows через MSVC (cl) без MinGW.
REM Запускать из "Developer Command Prompt for VS".
cl /nologo /O2 /W3 src\dl_port.c src\dl_core.c src\dl_hist.c src\main_win32.c /Fe:downloader.exe ws2_32.lib comctl32.lib comdlg32.lib gdi32.lib shell32.lib user32.lib kernel32.lib
