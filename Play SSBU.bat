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
rem Starts the native Super Smash Bros. Ultimate, online: the game joins the lobby named in
rem online_server.txt, where Local Wireless lists everyone's rooms (see NETPLAY.md). If the lobby
rem does not answer, the game plays offline (Play SSBU Offline.bat never goes online). The word off
rem in online_direct.txt keeps all of a room's traffic going through the lobby (no direct
rem connections, so the players in your rooms don't learn your internet address). Every game
rem version built here has a folder of its own with its own save (the port's folder: build\, save\;
rem others: v13.0.5\ and so on - see docs\VERSIONS.md); with more than one, this asks which to
rem start. Its log goes to <version folder>\build\play.log.
cd /d "%~dp0"
call tools\pick_version.bat || (pause & exit /b 1)
if exist online_server.txt for /f "usebackq eol=# tokens=1,2" %%a in ("online_server.txt") do if not defined SSBU_NET_RELAY (
    set "SSBU_NET_RELAY=%%a"
    if not "%%b"=="" set "SSBU_NET_ROOM=%%b"
)
if /i "%SSBU_NET_RELAY%"=="off" set "SSBU_NET_RELAY="
if defined SSBU_NET_RELAY (
    if not exist online_name.txt call :ask_name
    for /f "usebackq delims=" %%a in ("online_name.txt") do if not defined SSBU_NET_NAME set "SSBU_NET_NAME=%%a"
)
if exist online_direct.txt for /f "usebackq eol=# tokens=1" %%a in ("online_direct.txt") do if /i "%%a"=="off" set "SSBU_NET_DIRECT=0"
start "SSBU log" /min cmd /c "%SSBU_TREE%\build\ssbu.exe %SSBU_TREE%\dump\modules --status 30 > %SSBU_TREE%\build\play.log 2>&1"
exit /b 0

:ask_name
set /a "number=%RANDOM% %% 9000 + 1000"
set "pick=Player%number%"
echo Online, other players see your name (it is kept in online_name.txt).
set /p "pick=Your online name [%pick%]: "
>online_name.txt echo %pick%
exit /b 0
