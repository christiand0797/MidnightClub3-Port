@echo off
setlocal
title Check for updates - Midnight Club 3
rem Check only. A newer source commit does not count as a published playable update.
rem Supply the installation explicitly: the packaged helper lives in update\.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0update\Update-MC3.ps1" -Mode Check -InstallDirectory "%~dp0."
set "mc3_update_exit=%errorlevel%"
echo.
echo Checking does not install anything. Download and install are separate choices.
echo See update\README.md for the available actions.
pause
exit /b %mc3_update_exit%
