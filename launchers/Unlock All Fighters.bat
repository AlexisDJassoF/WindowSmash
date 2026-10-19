@echo off
setlocal
cd /d "%~dp0.."
call tools\pick_version.bat || (pause & exit /b 1)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\Unlock-All-Fighters.ps1" -Tree "%SSBU_TREE%" %*
set "SSBU_UNLOCK_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %SSBU_UNLOCK_RESULT%
