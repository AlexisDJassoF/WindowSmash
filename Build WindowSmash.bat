@echo off
rem Builds the game from your own files in one go: the setup guide's steps from installing the
rem Python packages to the build itself (docs\BUILDING.md), one after the other. It asks where your
rem game files, keys and firmware are, and stops with a message at the first step that fails. A
rem step that is already done is skipped, so running it again carries on where it stopped. Needs
rem Python 3.14 and Visual Studio's Clang tools (the guide's first steps). tools\build_game.py does
rem the work; options such as --jobs 4 are passed on to it.
setlocal
cd /d "%~dp0"
set "PY="
python -c "import compression.zstd" >nul 2>&1 && set "PY=python"
if not defined PY (py -3.14 -c "import compression.zstd" >nul 2>&1 && set "PY=py -3.14")
if not defined PY (
    echo Python 3.14 was not found. Install it as the setup guide says ^(docs\BUILDING.md^), with
    echo "Add python.exe to PATH" ticked, then run this again.
    echo.
    pause
    exit /b 1
)
%PY% tools\build_game.py %*
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
