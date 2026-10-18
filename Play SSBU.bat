@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_BIND="
@echo off
rem Starts the native Super Smash Bros. Ultimate (build\ssbu.exe). Its log goes to build\play.log.
cd /d "%~dp0"
start "SSBU log" /min cmd /c "build\ssbu.exe dump\modules --status 30 > build\play.log 2>&1"
