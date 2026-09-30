@echo off
setlocal
rem Build the Linux package from Windows: runs build_library_linux.sh in a
rem WSL distribution, Debian 12 by default, whose glibc (2.36) is then the
rem oldest the library runs on.  The package lands in dist\kimodo-linux.
rem
rem   scripts\build\build_library_linux.bat                  release package
rem   scripts\build\build_library_linux.bat --debug          debug build
rem   scripts\build\build_library_linux.bat --out <folder>   package somewhere else
rem   scripts\build\build_library_linux.bat --no-weights     leave the package's weights as they are
rem   scripts\build\build_library_linux.bat --distro <name>  another WSL distribution
rem
rem The distribution needs the packages build_library_linux.sh names; in
rem Debian 12:
rem     wsl -d Debian12 -u root -- apt-get install -y cmake ninja-build g++ glslc libvulkan-dev git binutils
rem WSL has no real GPU, so the library can be built and run there on the CPU
rem only; a GPU run needs a Linux machine.

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "DISTRO=Debian12"
set "OUT="
set "PASS="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--distro" (
    if "%~2"=="" goto missing_value
    set "DISTRO=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="--out" (
    if "%~2"=="" goto missing_value
    set "OUT=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
if "%~1"=="/?" goto usage
if /i "%~1"=="--release" goto pass
if /i "%~1"=="--debug" goto pass
if /i "%~1"=="--no-weights" goto pass
echo Unknown argument: %~1
echo.
call :usage
exit /b 2
:pass
set "PASS=%PASS% %~1"
shift
goto parse
:parsed

where wsl >nul 2>nul || goto no_wsl
wsl -d %DISTRO% -e true >nul 2>nul || goto no_distro

rem The repository and the package folder as the distribution sees them.
set "ROOT_WSL="
for /f "usebackq delims=" %%I in (`wsl -d %DISTRO% -e wslpath -a "%ROOT%"`) do set "ROOT_WSL=%%I"
if not defined ROOT_WSL goto no_path
if not defined OUT goto run
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
set "OUT_WSL="
for /f "usebackq delims=" %%I in (`wsl -d %DISTRO% -e wslpath -a "%OUT%"`) do set "OUT_WSL=%%I"
if not defined OUT_WSL goto no_path
set "PASS=%PASS% --out "%OUT_WSL%""

:run
echo Building in WSL (%DISTRO%): %ROOT_WSL%
wsl -d %DISTRO% -e bash "%ROOT_WSL%/scripts/build/build_library_linux.sh"%PASS%
exit /b %errorlevel%

:usage
echo Usage: scripts\build\build_library_linux.bat [--release ^| --debug] [--out ^<folder^>] [--no-weights] [--distro ^<name^>]
echo.
echo   --release     optimised build (the default)
echo   --debug       debug build
echo   --out         the package folder (default dist\kimodo-linux)
echo   --no-weights  leave the package's weights as they are
echo   --distro      the WSL distribution to build in (default Debian12)
exit /b 0

:missing_value
echo %~1 needs a value.
exit /b 2

:no_wsl
echo WSL was not found.  Install it and Debian 12 with:
echo     wsl --install -d Debian
exit /b 1

:no_distro
echo The WSL distribution %DISTRO% was not found; wsl -l lists those installed.
echo Install Debian with: wsl --install -d Debian
exit /b 1

:no_path
echo WSL could not translate %ROOT% into a Linux path.
exit /b 1
