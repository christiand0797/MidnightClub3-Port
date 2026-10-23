@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-MC3.ps1" %*
exit /b %ERRORLEVEL%
