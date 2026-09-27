@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "RESOBJ="

where windres >nul 2>nul
if errorlevel 1 (
    echo [warn] windres not found -- building without manifest.
) else (
    if not exist screenshot.rc (
        echo [warn] screenshot.rc missing -- building without manifest.
    ) else (
        windres -i screenshot.rc -o screenshot_res.o
        if errorlevel 1 (
            echo [warn] windres failed -- building without manifest.
            del /q screenshot_res.o >nul 2>nul
        ) else (
            set "RESOBJ=screenshot_res.o"
        )
    )
)

if defined RESOBJ echo [info] embedding manifest from screenshot.rc

gcc -o run.exe screenshot.c %RESOBJ% ^
    -O2 -s -Wall -Wextra ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    -municode -lgdi32 -luser32 -mwindows

if errorlevel 1 (
    echo [error] build failed.
    exit /b 1
)

echo [ok] build succeeded.
for %%F in (run.exe) do echo        run.exe  %%~zF bytes
exit /b 0
