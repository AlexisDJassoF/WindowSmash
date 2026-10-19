@echo off
rem Sets WindowSmash up on this PC in one go: checks the PC, installs what is missing (Python,
rem the Visual Studio build tools, SDL3), asks where your game files, keys and firmware are
rem (found by itself when yuzu or Ryujinx has them), builds the game, and puts a shortcut on the
rem desktop. Nothing but Windows itself is needed to start it. A step that is already done is
rem skipped, so running it again carries on where it stopped. tools\install_windowsmash.ps1 does
rem the work; options such as -DryRun are passed on to it (see its header).
setlocal
cd /d "%~dp0"
rem Files downloaded from the internet are marked, and Windows then warns about each one it
rem runs: take the mark off every file in this folder first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0.' -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install_windowsmash.ps1" %*
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
