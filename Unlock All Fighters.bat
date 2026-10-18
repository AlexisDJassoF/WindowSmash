@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Unlock-All-Fighters.ps1" %*
set "SSBU_UNLOCK_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %SSBU_UNLOCK_RESULT%
