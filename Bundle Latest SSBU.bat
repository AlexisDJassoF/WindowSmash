@echo off
setlocal
set "SSBU_BUNDLER=%~dp0tools\bundle_ssbu.py"
if not exist "%SSBU_BUNDLER%" set "SSBU_BUNDLER=%~dp0bundle_ssbu.py"
if not exist "%SSBU_BUNDLER%" (
    echo Cannot find bundle_ssbu.py beside this launcher or in tools.
    pause
    exit /b 1
)
where py >nul 2>&1
if not errorlevel 1 (
    py -3 "%SSBU_BUNDLER%" %*
) else (
    python "%SSBU_BUNDLER%" %*
)
set "SSBU_BUNDLE_RESULT=%ERRORLEVEL%"
echo.
if not "%SSBU_BUNDLE_RESULT%"=="0" echo Bundle failed. See the error above.
pause
exit /b %SSBU_BUNDLE_RESULT%
