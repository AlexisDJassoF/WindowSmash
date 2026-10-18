@echo off
rem Picks the game version to start, for the launchers: sets SSBU_TREE to its folder - the port's
rem own (.) or a version's own (v13.0.5, ...: tools\install_version.py, docs\VERSIONS.md). With one
rem version built it is that one; with several it lists them (each folder's version.txt) and
rem asks, offering the one picked last time (last_version.txt). SSBU_TREE set beforehand to a
rem built folder skips the question. Run from the port's folder; exits with 1 if nothing is built.
if defined SSBU_TREE if exist "%SSBU_TREE%\build\ssbu.exe" exit /b 0
setlocal EnableDelayedExpansion
set count=0
if exist "build\ssbu.exe" call :add "."
for /d %%d in (*) do if exist "%%d\build\ssbu.exe" if exist "%%d\dump\modules\romfs.cfg" call :add "%%d"
if %count%==0 (
    echo No game version is built here yet - see docs\VERSIONS.md.
    endlocal
    exit /b 1
)
set pick=%count%
set "last="
if exist last_version.txt set /p last=<last_version.txt
for /l %%i in (1,1,%count%) do if /i "!tree%%i!"=="!last!" set pick=%%i
if %count%==1 goto chosen
echo Game versions built here:
for /l %%i in (1,1,%count%) do echo   %%i. !label%%i!
:ask
set "answer="
set /p "answer=Which one [%pick%]: "
if not defined answer goto chosen
set /a n=answer 2>nul
if !n! geq 1 if !n! leq %count% (
    set pick=!n!
    goto chosen
)
echo Type one of the numbers above.
goto ask
:chosen
set "chosen=!tree%pick%!"
> last_version.txt echo !chosen!
echo Game version !label%pick%!
endlocal & set "SSBU_TREE=%chosen%"
exit /b 0

:add
set /a count+=1
set "tree%count%=%~1"
set "ver=%~1"
set "got="
if exist "%~1\version.txt" for /f "usebackq delims=" %%v in ("%~1\version.txt") do if not defined got (
    set "ver=%%v"
    set got=1
)
if "%~1"=="." (set "label%count%=!ver!  - build\, save\") else (set "label%count%=!ver!  - %~1\")
exit /b 0
