@echo off
rem ============================================================================
rem  Jet Force Gemini: Recompiled - Windows build script (double-click to run).
rem  Writes everything it prints to build_windows.log next to this file.
rem
rem  Needs: your Jet Force Gemini (US) ROM as baserom.us.z64 next to this file,
rem  Visual Studio 2022 (C++ desktop workload + "C++ Clang tools"), Git, Python 3.
rem  See README.md.
rem
rem  What it does:
rem    1. finds Visual Studio (MSVC + clang-cl + CMake + Ninja), Git and Python
rem    2. downloads the libraries (git submodules: the port's forks of N64Recomp,
rem       N64ModernRuntime and RT64, each pinned to a commit on its jfg branch,
rem       and the base project's other libraries)
rem    3. builds the recompiler tools and translates the game code from YOUR
rem       ROM to C (skipped when nothing it is made from changed)
rem    4. builds the executable and puts the finished game in the "bin" folder
rem       next to this file (saves and settings are kept there too)
rem
rem  The game is built in this folder (build-tools, build-windows,
rem  RecompiledFuncs; none of it is committed).
rem  Override the ROM location with:  set JFG_ROM=D:\path\to\rom.z64
rem  Leave lib\ as it is (no checkout to the pinned commits, for work inside a
rem  library) with:                   set JFG_KEEP_LIBS=1
rem  Generate the game code even if nothing changed: set JFG_REGENERATE=1
rem  Nothing here pushes or uploads anything. Git is used for downloads only.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

if /i not "%~1"=="__run" (
    cmd /d /c ""%~f0" __run 2>&1" | powershell -NoProfile -Command "$input | Tee-Object -FilePath '%~dp0build_windows.log'"
    rem The build maps its folder to a temporary drive letter (see below) and notes it here; remove the mapping again.
    if exist "%~dp0build_drive.tmp" (
        for /f "usebackq delims=" %%d in ("%~dp0build_drive.tmp") do subst %%d /d >nul 2>&1
        del "%~dp0build_drive.tmp" >nul 2>&1
    )
    echo.
    echo Log saved to "%~dp0build_windows.log"
    if not defined JFG_NO_PAUSE pause
    exit /b
)

set "PORT_DIR=%~dp0"
set "BIN=%PORT_DIR%bin"
rem The build folder is this folder (without the trailing backslash).
set "REAL_WORK=%PORT_DIR:~0,-1%"

rem The libraries' build steps break on paths that contain spaces or are very long, and this folder may be anywhere.
rem So the folder holding the build folder is mapped to a spare drive letter for the duration of the build (subst; nothing is written
rem outside the folder) and everything below works through that letter. The letter used last time is remembered in
rem the build folder, because CMake's files refer to it. Set JFG_NO_SUBST=1 to build through the real path instead.
set "WORK=%REAL_WORK%"
if defined JFG_NO_SUBST goto work_ready
rem A mapping left behind by a build that was closed half-way is removed first.
if exist "%PORT_DIR%build_drive.tmp" (
    for /f "usebackq delims=" %%d in ("%PORT_DIR%build_drive.tmp") do subst %%d /d >nul 2>&1
    del "%PORT_DIR%build_drive.tmp" >nul 2>&1
)
set "DRV="
set "LAST_DRV="
if exist "%REAL_WORK%\.jfg_drive" set /p LAST_DRV=<"%REAL_WORK%\.jfg_drive"
if defined LAST_DRV if not exist "%LAST_DRV%\" set "DRV=%LAST_DRV%"
if not defined DRV for %%L in (J K L M N O P Q R S T U V W) do if not defined DRV if not exist "%%L:\" set "DRV=%%L:"
if not defined DRV ( echo [jfg] WARNING: no spare drive letter found - building through the real path & goto work_ready )
rem The letter is mapped to the build folder's parent, so the build folder keeps a normal name on it.
for %%I in ("%REAL_WORK%") do ( set "WORK_PARENT=%%~dpI" & set "WORK_NAME=%%~nxI" )
if not "%WORK_PARENT:~3,1%"=="" set "WORK_PARENT=%WORK_PARENT:~0,-1%"
subst %DRV% "%WORK_PARENT%"
if errorlevel 1 ( echo [jfg] WARNING: could not map %DRV% - building through the real path & goto work_ready )
>"%PORT_DIR%build_drive.tmp" echo %DRV%
if defined LAST_DRV if /i not "%LAST_DRV%"=="%DRV%" (
    echo [jfg] the drive letter changed ^(%LAST_DRV% -^> %DRV%^): CMake has to configure again
    if exist "%REAL_WORK%\build-windows\CMakeCache.txt" del "%REAL_WORK%\build-windows\CMakeCache.txt"
    if exist "%REAL_WORK%\build-tools\CMakeCache.txt" del "%REAL_WORK%\build-tools\CMakeCache.txt"
)
>"%REAL_WORK%\.jfg_drive" echo %DRV%
set "WORK=%DRV%\%WORK_NAME%"
:work_ready
if not defined JFG_ROM set "JFG_ROM=%PORT_DIR%baserom.us.z64"
set "ROM_SHA1=493ced9008dbe932d6e91179b68e8630cf23a023"

echo [jfg] build started %DATE% %TIME%
echo [jfg] port folder : %PORT_DIR%
echo [jfg] build folder: %REAL_WORK%
if /i not "%WORK%"=="%REAL_WORK%" echo [jfg]               ^(mapped to %WORK% while building^)
echo [jfg] game folder : %BIN%

rem ---- 0. ROM ----------------------------------------------------------------
if not exist "%JFG_ROM%" ( echo [jfg] ERROR: ROM not found: %JFG_ROM% & echo [jfg] Put your Jet Force Gemini US ROM there as baserom.us.z64 - see README.md & exit /b 1 )
set "HAVE_SHA1="
for /f "skip=1 tokens=* delims=" %%h in ('certutil -hashfile "%JFG_ROM%" SHA1') do if not defined HAVE_SHA1 set "HAVE_SHA1=%%h"
set "HAVE_SHA1=%HAVE_SHA1: =%"
if /i not "%HAVE_SHA1%"=="%ROM_SHA1%" (
    echo [jfg] ERROR: wrong ROM. SHA-1 is %HAVE_SHA1%
    echo [jfg]        expected          %ROM_SHA1%  ^(US version, .z64 / big-endian format^)
    exit /b 1
)
echo [jfg] ROM ok: %JFG_ROM%

rem ---- 1. toolchain ----------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [jfg] ERROR: vswhere.exe not found - is Visual Studio installed? & exit /b 1 )
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo [jfg] ERROR: no Visual Studio installation found & exit /b 1 )
echo [jfg] Visual Studio: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [jfg] ERROR: vcvars64.bat failed & exit /b 1 )
if exist "%VSPATH%\VC\Tools\Llvm\x64\bin\clang-cl.exe" set "PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%"

for %%t in (git cmake ninja clang-cl tar) do (
    where %%t >nul 2>&1
    if errorlevel 1 ( echo [jfg] ERROR: %%t not found on PATH & exit /b 1 )
)
set "PY="
py -3 --version >nul 2>&1 && set "PY=py -3"
if not defined PY ( python --version >nul 2>&1 && set "PY=python" )
if not defined PY ( echo [jfg] ERROR: Python 3 not found - install it from python.org & exit /b 1 )
%PY% --version
git --version
cmake --version | findstr /i version
clang-cl --version | findstr /i version

rem Keep files byte-identical to the repository and allow long paths.
rem These settings apply only to git commands started by this script.
set GIT_CONFIG_COUNT=2
set GIT_CONFIG_KEY_0=core.autocrlf
set GIT_CONFIG_VALUE_0=false
set GIT_CONFIG_KEY_1=core.longpaths
set GIT_CONFIG_VALUE_1=true

rem ---- 2. libraries ----------------------------------------------------------
if defined JFG_KEEP_LIBS (
    echo [jfg] JFG_KEEP_LIBS is set: lib\ is left as it is
    goto libs_ready
)
echo [jfg] fetching libraries ^(this is the slow part the first time^)...
git -C "%WORK%" submodule update --init --recursive
if errorlevel 1 ( echo [jfg] ERROR: submodule download failed & exit /b 1 )
:libs_ready

rem ---- 3. game code ----------------------------------------------------------
if exist "%PORT_DIR%generated\RecompiledFuncs.tgz" goto use_pregenerated

echo [jfg] building the recompiler tools...
cmake -S "%WORK%\lib\N64Recomp" -B "%WORK%\build-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 ( echo [jfg] ERROR: configuring the recompiler tools failed & exit /b 1 )
cmake --build "%WORK%\build-tools" --parallel --target N64RecompCLI RSPRecomp
if errorlevel 1 ( echo [jfg] ERROR: building the recompiler tools failed & exit /b 1 )
rem The game code is generated again only when something it is made from changed (the hash below, saved with it), so
rem a rebuild after a change in src or lib compiles only that.
set "STAMP=%WORK%\RecompiledFuncs\.inputs_hash"
call :inputs_hash || exit /b 1
set "OLD_HASH="
if exist "%STAMP%" set /p OLD_HASH=<"%STAMP%"
if not defined JFG_REGENERATE if defined OLD_HASH if exist "%WORK%\rsp\aspMain.cpp" if "%OLD_HASH%"=="%INPUTS_HASH%" (
    echo [jfg] game code is up to date ^(ROM, recompiler configs and tools unchanged^): not generated again
    goto build
)
if exist "%WORK%\RecompiledFuncs" rmdir /s /q "%WORK%\RecompiledFuncs"
echo [jfg] translating the game code ^(your ROM -^> C^)...
if /i not "%JFG_ROM%"=="%PORT_DIR%baserom.us.z64" copy /y "%JFG_ROM%" "%WORK%\baserom.us.z64" >nul || ( echo [jfg] ERROR: copying the ROM failed & exit /b 1 )
pushd "%WORK%"
rem Recompiler input only: a copy of the ROM with the overlays pre-linked at private addresses. Never used at run time.
%PY% tools\prepare_overlays.py baserom.us.z64 - baserom.us.recomp.z64 - tools\jfg.us.dynamic_relocs.json
if errorlevel 1 ( popd & echo [jfg] ERROR: prepare_overlays.py failed & exit /b 1 )
mkdir RecompiledFuncs
build-tools\N64Recomp.exe jfg.us.toml > recomp.log 2>&1
if errorlevel 1 ( popd & echo [jfg] ERROR: N64Recomp failed - see %WORK%\recomp.log & exit /b 1 )
build-tools\RSPRecomp.exe rsp-aspMain.jfg.toml
if errorlevel 1 ( popd & echo [jfg] ERROR: RSPRecomp failed & exit /b 1 )
%PY% tools\merge_gen_anim_data.py RecompiledFuncs
if errorlevel 1 ( popd & echo [jfg] ERROR: merge_gen_anim_data.py failed & exit /b 1 )
%PY% tools\fix_dynamic_relocs.py RecompiledFuncs tools\jfg.us.dynamic_relocs.json baserom.us.recomp.z64 JFGRecompSyms\jfg.us.syms.toml
if errorlevel 1 ( popd & echo [jfg] ERROR: fix_dynamic_relocs.py failed & exit /b 1 )
%PY% tools\refuse_mod_hooks.py RecompiledFuncs tools\jfg.us.dynamic_relocs.json JFGRecompSyms\jfg.us.syms.toml jfg.us.toml
if errorlevel 1 ( popd & echo [jfg] ERROR: refuse_mod_hooks.py failed & exit /b 1 )
popd
rem Saved last, so a run that stopped half-way generates the code again next time.
call :inputs_hash || exit /b 1
>"%STAMP%" echo %INPUTS_HASH%
goto build

:use_pregenerated
rem Local shortcut, not part of the published project: game code generated earlier on another machine.
echo [jfg] unpacking generated\RecompiledFuncs.tgz ^(delete that file to generate from the ROM instead^)...
if exist "%WORK%\RecompiledFuncs" rmdir /s /q "%WORK%\RecompiledFuncs"
tar -xzf "%PORT_DIR%generated\RecompiledFuncs.tgz" -C "%WORK%"
if errorlevel 1 ( echo [jfg] ERROR: unpacking generated code failed & exit /b 1 )

:build
rem ---- 4. build --------------------------------------------------------------
echo [jfg] configuring...
cmake -S "%WORK%" -B "%WORK%\build-windows" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 ( echo [jfg] ERROR: CMake configure failed & exit /b 1 )
echo [jfg] building ^(10-40 minutes the first time^)...
rem -k 0: keep going after errors so one run reports all of them
cmake --build "%WORK%\build-windows" --parallel -- -k 0
if errorlevel 1 ( echo [jfg] ERROR: build failed - see the errors above & exit /b 1 )

if not exist "%WORK%\build-windows\JetForceGeminiRecompiled.exe" ( echo [jfg] ERROR: build reported success but the executable is missing & exit /b 1 )

rem ---- 5. game folder --------------------------------------------------------
rem Everything the game needs to run, in one place. Saves and settings are written here as well.
if not exist "%BIN%" mkdir "%BIN%"
copy /y "%WORK%\build-windows\JetForceGeminiRecompiled.exe" "%BIN%\" >nul || ( echo [jfg] ERROR: could not copy the executable to %BIN% - is the game still running? & exit /b 1 )
copy /y "%WORK%\build-windows\*.dll" "%BIN%\" >nul || ( echo [jfg] ERROR: copying the DLLs failed & exit /b 1 )
copy /y "%WORK%\recompcontrollerdb.txt" "%BIN%\" >nul || ( echo [jfg] ERROR: copying recompcontrollerdb.txt failed & exit /b 1 )
xcopy "%WORK%\assets" "%BIN%\assets\" /e /i /y /q >nul || ( echo [jfg] ERROR: copying assets failed & exit /b 1 )
rem The changelog the launcher shows (About > Changelog) ships with the assets, so it also updates with the game.
copy /y "%PORT_DIR%CHANGELOG.md" "%BIN%\assets\jfg_launcher\CHANGELOG.md" >nul || ( echo [jfg] ERROR: copying CHANGELOG.md failed & exit /b 1 )
rem Empty folders for mods and texture packs (.rtz files or folders); existing ones, and what is in them, are left alone.
if not exist "%BIN%\mods" mkdir "%BIN%\mods"
if not exist "%BIN%\texture_packs" mkdir "%BIN%\texture_packs"
echo [jfg] BUILD OK: %BIN%\JetForceGeminiRecompiled.exe
echo [jfg] next: double-click run_windows.bat
echo [jfg] build finished %DATE% %TIME%
exit /b 0

:inputs_hash
rem One hash of everything the game code is made from (tools\recomp_inputs_hash.py), in INPUTS_HASH.
set "INPUTS_HASH="
for /f "usebackq delims=" %%h in (`%PY% "%WORK%\tools\recomp_inputs_hash.py" "%JFG_ROM%" "%WORK%\jfg.us.toml" "%WORK%\JFGRecompSyms\jfg.us.syms.toml" "%WORK%\rsp-aspMain.jfg.toml" "%WORK%\tools\*.py" "%WORK%\tools\jfg.us.dynamic_relocs.json" "%WORK%\build-tools\N64Recomp.exe" "%WORK%\build-tools\RSPRecomp.exe"`) do set "INPUTS_HASH=%%h"
if not defined INPUTS_HASH ( echo [jfg] ERROR: recomp_inputs_hash.py failed & exit /b 1 )
exit /b 0
