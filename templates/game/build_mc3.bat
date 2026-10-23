@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_mc3.ps1" %*
exit /b %errorlevel%
