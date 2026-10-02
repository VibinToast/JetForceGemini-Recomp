@echo off
rem Starts the game built by build_windows.bat and closes this window straight away.
rem Keyboard: Enter = START, Space = A, X or Left Shift = B, Q = Z, E = L, R = R, WASD = stick, arrows = C buttons, IJKL = D-pad.
rem F9 opens the debug menu. Controllers are picked up automatically.
rem The game, its saves and its settings are all in the bin folder; the executable there can also be started directly.
rem
rem For troubleshooting, run "run_windows.bat log" from a command prompt: the window then stays open until the game
rem is closed and the game's output is saved to run_windows.log next to this file.
setlocal EnableExtensions
set "PORT_DIR=%~dp0"
if not defined JFG_ROM set "JFG_ROM=%PORT_DIR%baserom.us.z64"
set "EXE=%PORT_DIR%bin\JetForceGeminiRecompiled.exe"
if not exist "%EXE%" ( echo Executable not found: %EXE% - run build_windows.bat first. & pause & exit /b 1 )
if not exist "%JFG_ROM%" ( echo ROM not found: %JFG_ROM% & pause & exit /b 1 )
cd /d "%PORT_DIR%bin"
if /i "%~1"=="log" goto with_log
start "" "%EXE%" --rom "%JFG_ROM%" --skip-launcher
exit /b 0

:with_log
echo Running... output goes to "%PORT_DIR%run_windows.log"
"%EXE%" --rom "%JFG_ROM%" --skip-launcher > "%PORT_DIR%run_windows.log" 2>&1
echo Exit code: %ERRORLEVEL% >> "%PORT_DIR%run_windows.log"
echo Finished with exit code %ERRORLEVEL%. Log saved.
pause
