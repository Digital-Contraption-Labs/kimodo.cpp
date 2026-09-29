@echo off
setlocal
rem Build the Kimodo text-to-motion service on Windows: the native worker
rem (kmd-generate, with the kimodo and ggml DLLs beside it) and the Go HTTP
rem server that scripts\start-server.bat and scripts\start-demo.bat run.
rem
rem   scripts\build\build_service.bat            release build in build\release
rem   scripts\build\build_service.bat --release  the same
rem   scripts\build\build_service.bat --debug    debug build in build\debug
rem
rem Runs from any shell.  When the Visual Studio x64 toolchain is not already
rem loaded it finds Visual Studio 2022 or later with vswhere and loads it for
rem this script only.  Needs Visual Studio's "Desktop development with C++"
rem workload with its CMake tools (or CMake and Ninja on PATH), the Vulkan
rem SDK, Go and Git.  start-server.bat runs the release build.

rem The repository root, taken before SHIFT moves %0.
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "CONFIG=release"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--release" (
    set "CONFIG=release"
    shift
    goto parse
)
if /i "%~1"=="--debug" (
    set "CONFIG=debug"
    shift
    goto parse
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
if "%~1"=="/?" goto usage
echo Unknown argument: %~1
echo.
call :usage
exit /b 2
:parsed

rem Load the Visual Studio x64 toolchain unless this shell already has it.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto toolchain_ready
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSINSTALLER%\vswhere.exe" goto no_visual_studio
rem vcvars64 runs vswhere by name too, as a developer prompt has it on PATH.
set "PATH=%VSINSTALLER%;%PATH%"
set "VSINSTALL="
for /f "usebackq delims=" %%I in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL goto no_visual_studio
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" goto no_visual_studio
echo Loading the Visual Studio x64 toolchain from %VSINSTALL%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
:toolchain_ready

where cl >nul 2>nul || goto no_visual_studio
where cmake >nul 2>nul || goto no_cmake
where ninja >nul 2>nul || goto no_cmake

rem Locate Go, falling back to the default install path for shells opened
rem before Go was added to PATH.
set "GO=go"
where go >nul 2>nul || set "GO=C:\Program Files\Go\bin\go.exe"
if not exist "%GO%" if not "%GO%"=="go" goto no_go

rem ggml's Vulkan backend, which runs the model on the GPU, needs the SDK.
if not defined VULKAN_SDK goto no_vulkan

rem Windows will not replace a running executable or the DLLs it loaded.
tasklist /fi "imagename eq kimodo-demo.exe" 2>nul | findstr /i /c:"kimodo-demo.exe" >nul
if not errorlevel 1 goto server_running

cd /d "%ROOT%"

rem Check out a missing submodule at the commit this repository pins.  One
rem that is already checked out is left alone, whatever commit it is on.
if exist "ggml\CMakeLists.txt" goto have_ggml
echo Checking out the ggml submodule...
git submodule update --init --recursive -- ggml || goto submodule_failed
:have_ggml
if exist "eigen\Eigen\Sparse" goto have_eigen
echo Checking out the eigen submodule...
git submodule update --init --recursive -- eigen || goto submodule_failed
:have_eigen

echo.
echo Configuring the windows-%CONFIG% preset...
cmake --preset windows-%CONFIG% || goto cmake_failed

echo.
echo Building kmd-generate (%CONFIG%)...
cmake --build --preset windows-%CONFIG% --target kmd-generate || goto cmake_failed

echo.
echo Building the Go server...
"%GO%" build -o build\kimodo-demo.exe .\demo || goto go_failed

echo.
echo Built the Kimodo service (%CONFIG%) in %ROOT%:
echo   build\%CONFIG%\kmd-generate.exe   native worker, with the kimodo and ggml DLLs
echo   build\kimodo-demo.exe            HTTP server
echo.
if "%CONFIG%"=="debug" goto debug_hint
echo Run it with scripts\start-server.bat, or scripts\start-demo.bat for the demo page.
exit /b 0

:debug_hint
echo scripts\start-server.bat runs the release build.  To serve this one, run
echo from %ROOT%:
echo     build\kimodo-demo.exe -addr 127.0.0.1:8094 -generator build/debug/kmd-generate
exit /b 0

:usage
echo Usage: scripts\build\build_service.bat [--release ^| --debug]
echo.
echo   --release  optimised build in build\release (the default); this is the
echo              one scripts\start-server.bat runs
echo   --debug    debug build in build\debug
exit /b 0

:no_visual_studio
echo The Visual Studio x64 C++ toolchain was not found.  Install Visual Studio
echo 2022 or later with the "Desktop development with C++" workload, or run this
echo from an "x64 Native Tools Command Prompt".
exit /b 1

:no_cmake
echo CMake and Ninja were not found.  Add the "C++ CMake tools for Windows"
echo component in the Visual Studio Installer, or install them with:
echo     winget install Kitware.CMake
echo     winget install Ninja-build.Ninja
exit /b 1

:no_go
echo Go was not found.  Install it with: winget install GoLang.Go
exit /b 1

:no_vulkan
echo The Vulkan SDK was not found: VULKAN_SDK is not set.  Install it from
echo https://vulkan.lunarg.com/sdk/home and open a new shell.
exit /b 1

:server_running
echo The Kimodo server is running and holds files this build replaces.
echo Stop it first with scripts\stop-server.bat.
exit /b 1

:submodule_failed
echo git submodule update failed.  This needs Git and a clone of the
echo repository; run "git submodule update --init --recursive" in %ROOT%
echo to see why.
exit /b 1

:cmake_failed
echo The CMake build failed; see the errors above.
exit /b 1

:go_failed
echo go build failed; see the errors above.
exit /b 1
