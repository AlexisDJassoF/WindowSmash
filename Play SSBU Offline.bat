@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_BIND="
set "SSBU_NET_ROOM="
set "SSBU_NET_NAME="
@echo off
rem Starts the native Super Smash Bros. Ultimate offline (Play SSBU.bat plays online). Every game
rem version built here has a folder of its own with its own save (the port's folder: build\, save\;
rem others: v13.0.5\ and so on - see docs\VERSIONS.md); with more than one, this asks which to
rem start. Its log goes to <version folder>\build\play.log.
cd /d "%~dp0"
call tools\pick_version.bat || (pause & exit /b 1)
start "SSBU log" /min cmd /c "%SSBU_TREE%\build\ssbu.exe %SSBU_TREE%\dump\modules --status 30 > %SSBU_TREE%\build\play.log 2>&1"
