@echo off
setlocal
rem Starts the native Super Smash Bros. Ultimate in LAN mode: no lobby or relay - the game uses this
rem PC's own network, where a Switch on the same network plays with it in the game's LAN mode
rem (Local Wireless, then L+R+left stick). See NETPLAY.md. Every game version built here has a folder
rem of its own; with more than one, this asks which to start. Its log goes to
rem <version folder>\build\play.log.
cd /d "%~dp0"
call tools\pick_version.bat || (pause & exit /b 1)
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_LAN=1"
start "SSBU log" /min cmd /c "%SSBU_TREE%\build\ssbu.exe %SSBU_TREE%\dump\modules --status 30 > %SSBU_TREE%\build\play.log 2>&1"
