@echo off
setlocal
cd /d "%~dp0.."
set "SSBU_BUNDLER=tools\bundle_ssbu.py"
if not exist "%SSBU_BUNDLER%" (
    echo Cannot find tools\bundle_ssbu.py.
    pause
    exit /b 1
)
call tools\find_python.bat || (pause & exit /b 1)
%PY% "%SSBU_BUNDLER%" %*
set "SSBU_BUNDLE_RESULT=%ERRORLEVEL%"
echo.
if not "%SSBU_BUNDLE_RESULT%"=="0" echo Bundle failed. See the error above.
pause
exit /b %SSBU_BUNDLE_RESULT%
