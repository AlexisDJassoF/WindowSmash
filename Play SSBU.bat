@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_BIND="
set "SSBU_NET_ROOM="
set "SSBU_NET_NAME="
set "SSBU_NET_DIRECT="
set "SSBU_NET_PORT="
rem Starts the native Super Smash Bros. Ultimate, online: the game joins the lobby named in
rem online_server.txt, where Local Wireless lists everyone's rooms (see docs\MULTIPLAYER.md). If the lobby
rem does not answer, the game plays offline (launchers\Play SSBU Offline.bat never goes online). With
rem --online the game reads online_server.txt, online_name.txt (the name it picks the first time) and
rem online_direct.txt itself ("off": no direct connections, so the players in your rooms don't learn
rem your internet address); its settings menu (Esc) changes them. Every game version built here has
rem a folder of its own with its own save (the port's folder: build\, save\; others: v13.0.5\ and so
rem on - see docs\VERSIONS.md). Its log goes to <version folder>\build\play.log.
rem
rem First a window (tools\launcher.ps1) shows the game version and the settings, and its Play button
rem starts the game. With "start the game directly" ticked there (launcher.ini: show_launcher = 0),
rem the game starts at once with the version picked last; hold Shift while starting to see the window
rem again. If the window cannot open (exit code 2), the game starts as before: with more than one
rem version built, this asks which one in this window (tools\pick_version.bat).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\launcher.ps1 -Auto
if %errorlevel%==2 goto direct
if errorlevel 1 (pause & exit /b 1)
exit /b 0
:direct
call tools\pick_version.bat || (pause & exit /b 1)
start "SSBU log" /min cmd /c "%SSBU_TREE%\build\ssbu.exe %SSBU_TREE%\dump\modules --status 30 --online > %SSBU_TREE%\build\play.log 2>&1"
exit /b 0
