@echo off
setlocal
rem Build the Kimodo shared library for Windows and package everything a host
rem application needs to run it in its own process: kimodo.dll (the engine
rem and ggml in one file, exporting the C API alone), its PDB and import
rem library, kimodo_capi.h, the licences, VERSION.json, and the weights from
rem this repository's cache.  See docs\SHARED_LIBRARY_PLAN.md.
rem
rem   scripts\build\build_library_windows.bat                 release package in dist\kimodo-windows
rem   scripts\build\build_library_windows.bat --debug         debug build (needs the debug C++ runtime,
rem                                                            which comes with Visual Studio)
rem   scripts\build\build_library_windows.bat --out <folder>  package somewhere else
rem   scripts\build\build_library_windows.bat --no-weights    leave the package's weights as they are
rem
rem Runs from any shell: it loads the Visual Studio x64 toolchain itself.
rem Needs Visual Studio 2022 or later with the "Desktop development with C++"
rem workload and its CMake tools, the Vulkan SDK and Git.  The weights come
rem from models\ and the repository root, where
rem scripts\download_gguf_weights.py puts them; an SMPL-X checkpoint is never
rem packaged.  The library is checked on the way: it must export the C API
rem and nothing else, depend on nothing a host would have to ship, and load
rem and answer from the package folder.

rem The repository root and this folder, taken before SHIFT moves %0.
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "HERE=%~dp0"
set "CONFIG=release"
set "OUT="
set "WEIGHTS=ON"

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
if /i "%~1"=="--no-weights" (
    set "WEIGHTS=OFF"
    shift
    goto parse
)
if /i "%~1"=="--out" (
    if "%~2"=="" goto missing_out
    set "OUT=%~2"
    shift
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

rem A relative --out is taken from the folder this was started in.
if not defined OUT set "OUT=%ROOT%\dist\kimodo-windows"
for %%I in ("%OUT%") do set "OUT=%%~fI"

call "%HERE%windows_prepare.bat" "%ROOT%"
if errorlevel 1 exit /b 1

cd /d "%ROOT%"

echo.
echo Configuring the windows-%CONFIG% preset...
cmake --preset windows-%CONFIG% || goto build_failed

echo.
echo Building kimodo.dll (%CONFIG%)...
cmake --build --preset windows-library-%CONFIG% || goto build_failed

echo.
echo Checking and packaging into %OUT%...
cmake -DKIMODO_TARGET=windows "-DKIMODO_SOURCE_DIR=%ROOT%" "-DKIMODO_BUILD_DIR=%ROOT%\build\%CONFIG%" "-DKIMODO_PACKAGE_DIR=%OUT%" -DKIMODO_WEIGHTS=%WEIGHTS% -P "%HERE%package.cmake" || goto package_failed

echo.
echo Loading the packaged library...
"%ROOT%\build\%CONFIG%\kimodo-capi-smoke.exe" --library "%OUT%\bin\kimodo.dll" || goto smoke_failed

echo.
echo Packaged the Kimodo library (%CONFIG%) in %OUT%
echo.
echo To generate a clip through it (a first run takes a minute):
echo     build\%CONFIG%\kimodo-capi-smoke.exe --library "%OUT%\bin\kimodo.dll" ^^
echo         --motion "%OUT%\weights\kimodo-soma-seed-v1.1-f32.gguf" ^^
echo         --text "%OUT%\weights\Llama-3-Kimodo-Q8_0.gguf"
echo To copy it into ContraptionFabricator:
echo     scripts\build\stage_to_cf.bat --cf ^<CF folder^>
exit /b 0

:usage
echo Usage: scripts\build\build_library_windows.bat [--release ^| --debug] [--out ^<folder^>] [--no-weights]
echo.
echo   --release     optimised build (the default)
echo   --debug       debug build; it needs the debug C++ runtime, which comes
echo                 with Visual Studio
echo   --out         the package folder (default dist\kimodo-windows)
echo   --no-weights  leave the package's weights as they are, for a rebuild
echo                 that only changes the library
exit /b 0

:missing_out
echo --out needs a folder.
exit /b 2

:build_failed
echo The CMake build failed; see the errors above.
exit /b 1

:package_failed
echo Packaging failed; see the reason above.
exit /b 1

:smoke_failed
echo The packaged kimodo.dll did not load and answer on its own; see above.
exit /b 1
