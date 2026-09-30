@echo off
setlocal
rem Build the Kimodo text-to-motion service on Windows: the native worker
rem (kmd-generate, one self-contained executable) and the Go HTTP server that
rem scripts\start-server.bat and scripts\start-demo.bat run.
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

rem The repository root and this folder, taken before SHIFT moves %0.
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "HERE=%~dp0"
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

rem The toolchain, CMake, Ninja, the Vulkan SDK and the submodules.
call "%HERE%windows_prepare.bat" "%ROOT%"
if errorlevel 1 exit /b 1

rem Locate Go, falling back to the default install path for shells opened
rem before Go was added to PATH.
set "GO=go"
where go >nul 2>nul || set "GO=C:\Program Files\Go\bin\go.exe"
if not exist "%GO%" if not "%GO%"=="go" goto no_go

rem Windows will not replace a running executable or the DLLs it loaded.
tasklist /fi "imagename eq kimodo-demo.exe" 2>nul | findstr /i /c:"kimodo-demo.exe" >nul
if not errorlevel 1 goto server_running

cd /d "%ROOT%"

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
echo   build\%CONFIG%\kmd-generate.exe   native worker
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

:no_go
echo Go was not found.  Install it with: winget install GoLang.Go
exit /b 1

:server_running
echo The Kimodo server is running and holds files this build replaces.
echo Stop it first with scripts\stop-server.bat.
exit /b 1

:cmake_failed
echo The CMake build failed; see the errors above.
exit /b 1

:go_failed
echo go build failed; see the errors above.
exit /b 1
