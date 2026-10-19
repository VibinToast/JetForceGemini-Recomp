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
rem    2. downloads the base project (cyscott/dkr-recomp, GPLv3, pinned commit)
rem       and its libraries into the "build" folder next to this file
rem    3. applies the base's dependency patches and the JFG changes
rem    4. builds the recompiler tools and translates the game code from YOUR
rem       ROM to C
rem    5. builds the executable and puts the finished game in the "bin" folder
rem       next to this file (saves and settings are kept there too)
rem
rem  Override the build folder with:  set JFG_PORT_DIR=D:\some\short\path
rem  (worth doing if this folder sits in a very long path: the libraries have
rem  deep folder trees and Windows tools can fail beyond 260 characters)
rem  Override the ROM location with:  set JFG_ROM=D:\path\to\rom.z64
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
if not defined JFG_PORT_DIR set "JFG_PORT_DIR=%PORT_DIR%build"
set "BIN=%PORT_DIR%bin"
set "REAL_WORK=%JFG_PORT_DIR%"
if not exist "%REAL_WORK%" mkdir "%REAL_WORK%"
if not exist "%REAL_WORK%" ( echo [jfg] ERROR: could not create the build folder %REAL_WORK% & exit /b 1 )

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
set "BASE_URL=https://github.com/cyscott/dkr-recomp.git"
set "BASE_COMMIT=eb8151c05837359c9eca0ad006077a14f5bf72b8"

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

rem Keep files byte-identical to the repository (the patches depend on it) and allow long paths.
rem These settings apply only to git commands started by this script.
set GIT_CONFIG_COUNT=2
set GIT_CONFIG_KEY_0=core.autocrlf
set GIT_CONFIG_VALUE_0=false
set GIT_CONFIG_KEY_1=core.longpaths
set GIT_CONFIG_VALUE_1=true

rem ---- 2. base project -------------------------------------------------------
if not exist "%WORK%\.git" (
    echo [jfg] downloading port base...
    rem Not "git clone": the folder already exists and holds the drive-letter note.
    git -C "%WORK%" init -q
    if errorlevel 1 ( echo [jfg] ERROR: git init failed & exit /b 1 )
    git -C "%WORK%" remote add origin "%BASE_URL%"
    git -C "%WORK%" fetch origin
    if errorlevel 1 ( echo [jfg] ERROR: download failed & exit /b 1 )
    git -C "%WORK%" checkout --detach %BASE_COMMIT%
    if errorlevel 1 ( echo [jfg] ERROR: could not check out pinned commit & exit /b 1 )
) else (
    echo [jfg] port base already present
)
echo [jfg] fetching libraries ^(this is the slow part the first time^)...
git -C "%WORK%" submodule update --init --recursive --depth 1
if errorlevel 1 ( echo [jfg] ERROR: submodule download failed & exit /b 1 )

rem ---- 3. patches ------------------------------------------------------------
rem The JFG patches change between revisions of this folder, so first put the files they touch back to the
rem downloaded state (only inside the build folder), then apply everything again.
git -C "%WORK%" checkout -- CMakeLists.txt src assets icons
if errorlevel 1 ( echo [jfg] ERROR: could not restore base files & exit /b 1 )
git -C "%WORK%\lib\rt64" checkout -- .
if errorlevel 1 ( echo [jfg] ERROR: could not restore rt64 files & exit /b 1 )
git -C "%WORK%\lib\rt64" clean -fdq -- src/gbi src/hle src/shaders src/shared
call :apply_once "%WORK%\lib\N64Recomp"                 "%WORK%\submodule-patches\n64recomp-dkr.patch"          || exit /b 1
call :apply_once "%WORK%\lib\N64ModernRuntime"          "%WORK%\submodule-patches\n64modernruntime-dkr.patch"   || exit /b 1
call :apply_once "%WORK%\lib\rt64"                      "%WORK%\submodule-patches\rt64-f3ddkr.patch"            || exit /b 1
call :apply_once "%WORK%\lib\rt64\src\contrib\plume"    "%WORK%\submodule-patches\plume-sdl2-compat.patch"      || exit /b 1
call :apply_once "%WORK%"                               "%PORT_DIR%patches\jfg-base.patch"             || exit /b 1
call :apply_once "%WORK%\lib\rt64"                      "%PORT_DIR%patches\rt64-jfg.patch"                      || exit /b 1
rem The version in version.txt (the release workflow updates it) goes into the build: the launcher shows it.
%PY% "%PORT_DIR%tools\set_version.py" "%PORT_DIR%version.txt" "%WORK%" || exit /b 1

rem ---- 4. JFG files ----------------------------------------------------------
rem src\jfg holds only the port's files: empty it first, so a file removed or renamed here does not stay in the build.
if exist "%WORK%\src\jfg" rmdir /s /q "%WORK%\src\jfg"
mkdir "%WORK%\src\jfg"
copy /y "%PORT_DIR%src\jfg\*.cpp" "%WORK%\src\jfg\" >nul || ( echo [jfg] ERROR: copying src\jfg failed & exit /b 1 )
copy /y "%PORT_DIR%src\jfg\*.h" "%WORK%\src\jfg\" >nul || ( echo [jfg] ERROR: copying src\jfg headers failed & exit /b 1 )
copy /y "%PORT_DIR%src\jfg\*.inc" "%WORK%\src\jfg\" >nul || ( echo [jfg] ERROR: copying src\jfg\*.inc failed & exit /b 1 )
rem The port's tabs of the Select menu (generated by tools\gen_jfg_config_rml.py).
copy /y "%PORT_DIR%assets\config_menu\jfg_*.rml" "%WORK%\assets\config_menu\" >nul || ( echo [jfg] ERROR: copying assets\config_menu failed & exit /b 1 )
rem The launcher's fonts and images (assets\launcher.rml; the VibinToast icon is made by tools\gen_jfg_launcher_sheet.py).
if not exist "%WORK%\assets\jfg_launcher" mkdir "%WORK%\assets\jfg_launcher"
copy /y "%PORT_DIR%assets\jfg_launcher\*.*" "%WORK%\assets\jfg_launcher\" >nul || ( echo [jfg] ERROR: copying assets\jfg_launcher failed & exit /b 1 )
rem The changelog the launcher shows (About > Changelog) ships with the assets, so it also updates with the game.
copy /y "%PORT_DIR%CHANGELOG.md" "%WORK%\assets\jfg_launcher\CHANGELOG.md" >nul || ( echo [jfg] ERROR: copying CHANGELOG.md failed & exit /b 1 )
if not exist "%WORK%\include" mkdir "%WORK%\include"
copy /y "%PORT_DIR%include\*.h" "%WORK%\include\" >nul || ( echo [jfg] ERROR: copying include failed & exit /b 1 )
copy /y "%PORT_DIR%icons\*.*" "%WORK%\icons\" >nul || ( echo [jfg] ERROR: copying icons failed & exit /b 1 )
copy /y "%PORT_DIR%jfg.us.toml" "%WORK%\" >nul
copy /y "%PORT_DIR%jfg.us.syms.toml" "%WORK%\" >nul
copy /y "%PORT_DIR%rsp-aspMain.jfg.toml" "%WORK%\" >nul
if not exist "%WORK%\tools" mkdir "%WORK%\tools"
copy /y "%PORT_DIR%tools\*.py" "%WORK%\tools\" >nul || ( echo [jfg] ERROR: copying tools failed & exit /b 1 )
copy /y "%PORT_DIR%tools\jfg.us.dynamic_relocs.json" "%WORK%\tools\" >nul

rem ---- 4b. game code ---------------------------------------------------------
if exist "%WORK%\RecompiledFuncs" rmdir /s /q "%WORK%\RecompiledFuncs"
if exist "%PORT_DIR%generated\RecompiledFuncs.tgz" goto use_pregenerated

echo [jfg] building the recompiler tools...
cmake -S "%WORK%\lib\N64Recomp" -B "%WORK%\build-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 ( echo [jfg] ERROR: configuring the recompiler tools failed & exit /b 1 )
cmake --build "%WORK%\build-tools" --parallel --target N64RecompCLI RSPRecomp
if errorlevel 1 ( echo [jfg] ERROR: building the recompiler tools failed & exit /b 1 )
echo [jfg] translating the game code ^(your ROM -^> C^)...
copy /y "%JFG_ROM%" "%WORK%\baserom.us.z64" >nul || ( echo [jfg] ERROR: copying the ROM failed & exit /b 1 )
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
%PY% tools\fix_dynamic_relocs.py RecompiledFuncs tools\jfg.us.dynamic_relocs.json baserom.us.recomp.z64 jfg.us.syms.toml
if errorlevel 1 ( popd & echo [jfg] ERROR: fix_dynamic_relocs.py failed & exit /b 1 )
%PY% tools\refuse_mod_hooks.py RecompiledFuncs tools\jfg.us.dynamic_relocs.json jfg.us.syms.toml
if errorlevel 1 ( popd & echo [jfg] ERROR: refuse_mod_hooks.py failed & exit /b 1 )
popd
goto build

:use_pregenerated
rem Local shortcut, not part of the published project: game code generated earlier on another machine.
echo [jfg] unpacking generated\RecompiledFuncs.tgz ^(delete that file to generate from the ROM instead^)...
tar -xzf "%PORT_DIR%generated\RecompiledFuncs.tgz" -C "%WORK%"
if errorlevel 1 ( echo [jfg] ERROR: unpacking generated code failed & exit /b 1 )

:build
rem ---- 5. build --------------------------------------------------------------
echo [jfg] configuring...
cmake -S "%WORK%" -B "%WORK%\build-windows" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 ( echo [jfg] ERROR: CMake configure failed & exit /b 1 )
echo [jfg] building ^(10-40 minutes the first time^)...
rem -k 0: keep going after errors so one run reports all of them
cmake --build "%WORK%\build-windows" --parallel -- -k 0
if errorlevel 1 ( echo [jfg] ERROR: build failed - see the errors above & exit /b 1 )

if not exist "%WORK%\build-windows\JetForceGeminiRecompiled.exe" ( echo [jfg] ERROR: build reported success but the executable is missing & exit /b 1 )

rem ---- 6. game folder --------------------------------------------------------
rem Everything the game needs to run, in one place. Saves and settings are written here as well.
if not exist "%BIN%" mkdir "%BIN%"
copy /y "%WORK%\build-windows\JetForceGeminiRecompiled.exe" "%BIN%\" >nul || ( echo [jfg] ERROR: could not copy the executable to %BIN% - is the game still running? & exit /b 1 )
copy /y "%WORK%\build-windows\*.dll" "%BIN%\" >nul || ( echo [jfg] ERROR: copying the DLLs failed & exit /b 1 )
copy /y "%WORK%\recompcontrollerdb.txt" "%BIN%\" >nul || ( echo [jfg] ERROR: copying recompcontrollerdb.txt failed & exit /b 1 )
xcopy "%WORK%\assets" "%BIN%\assets\" /e /i /y /q >nul || ( echo [jfg] ERROR: copying assets failed & exit /b 1 )
rem Empty folders for mods and texture packs (.rtz files or folders); existing ones, and what is in them, are left alone.
if not exist "%BIN%\mods" mkdir "%BIN%\mods"
if not exist "%BIN%\texture_packs" mkdir "%BIN%\texture_packs"
echo [jfg] BUILD OK: %BIN%\JetForceGeminiRecompiled.exe
echo [jfg] next: double-click run_windows.bat
echo [jfg] build finished %DATE% %TIME%
exit /b 0

:apply_once
rem %1 = repository folder, %2 = patch file. Skips patches that are already applied.
git -C %1 apply --recount --reverse --check %2 >nul 2>&1
if not errorlevel 1 ( echo [jfg] already applied: %~nx2 & exit /b 0 )
git -C %1 apply --recount %2
if errorlevel 1 ( echo [jfg] ERROR: could not apply %~nx2 & exit /b 1 )
echo [jfg] applied: %~nx2
exit /b 0
