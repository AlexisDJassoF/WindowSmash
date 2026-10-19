@echo off
rem Compiles the game's shaders ahead of time (tools\shader_aot.py) for the version you pick: an
rem effect you have not seen yet then shows at once the first time, instead of a moment later.
rem It runs once per game version, from minutes to about two hours, at low priority (the PC stays
rem usable). It can be stopped (close this window) and started again: what is done stays done.
rem Needs Python and the build tools, as for building (docs\BUILDING.md).
cd /d "%~dp0"
setlocal
call tools\find_python.bat || (pause & exit /b 1)
call tools\pick_version.bat || (pause & exit /b 1)
%PY% tools\shader_aot.py --tree "%SSBU_TREE%"
pause
