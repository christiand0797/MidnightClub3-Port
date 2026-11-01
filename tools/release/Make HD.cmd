@echo off
rem Makes the game HD on this PC from your own disc (enhance\tools\enhance\build_hd.py). Safe to close and run again.
title Make HD - Midnight Club 3
cd /d "%~dp0"
if not exist "enhance\python\python.exe" (
    echo The enhance folder is missing. Extract the whole ZIP again, then double-click Make HD.
    pause
    exit /b 1
)
start "" /b /wait /belownormal "enhance\python\python.exe" -I -X utf8 "enhance\tools\enhance\build_hd.py" %*
echo.
pause
