@echo off
setlocal
rem Build the Kimodo shared library for Android (arm64-v8a) on Windows and
rem package it for an app: libkimodo.so (the engine and ggml in one file,
rem exporting the C API alone; API 29, the static C++ runtime, aligned for
rem 16 KB pages), its unstripped copy for crash reports, kimodo_capi.h, the
rem licences, VERSION.json and the motion models.  The 8 GB text encoder is
rem left out: no phone holds it beside an app, so the library takes prompt
rem embeddings made elsewhere.  See docs\SHARED_LIBRARY_PLAN.md.
rem
rem   scripts\build\build_library_android.bat                  release package in dist\kimodo-android
rem   scripts\build\build_library_android.bat --debug          debug build
rem   scripts\build\build_library_android.bat --out <folder>   package somewhere else
rem   scripts\build\build_library_android.bat --no-weights     leave the package's weights as they are
rem   scripts\build\build_library_android.bat --ndk <folder>   an NDK other than the one found
rem
rem Needs the Android NDK 27.2.12479018 (ContraptionFabricator's), found in
rem ANDROID_NDK_HOME, ANDROID_NDK_ROOT or the SDK's ndk\27.2.12479018; and, as
rem build_service.bat does, Visual Studio's C++ tools (ggml builds a shader
rem compiler for this machine), CMake, Ninja, the Vulkan SDK (its glslc) and
rem Git.  The library cannot be run here; the package's tools\ folder has a
rem program that checks it on a device through adb (printed at the end).

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "HERE=%~dp0"
set "CONFIG=release"
set "OUT="
set "WEIGHTS=ON"
set "NDK="
set "NDK_VERSION=27.2.12479018"

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
    if "%~2"=="" goto missing_value
    set "OUT=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="--ndk" (
    if "%~2"=="" goto missing_value
    set "NDK=%~2"
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

if not defined OUT set "OUT=%ROOT%\dist\kimodo-android"
for %%I in ("%OUT%") do set "OUT=%%~fI"

rem The NDK: --ndk, then the environment, then the SDK's side-by-side folder.
if not defined NDK if defined ANDROID_NDK_HOME set "NDK=%ANDROID_NDK_HOME%"
if not defined NDK if defined ANDROID_NDK_ROOT set "NDK=%ANDROID_NDK_ROOT%"
if not defined NDK if defined ANDROID_HOME set "NDK=%ANDROID_HOME%\ndk\%NDK_VERSION%"
if not defined NDK set "NDK=%LOCALAPPDATA%\Android\Sdk\ndk\%NDK_VERSION%"
if not exist "%NDK%\build\cmake\android.toolchain.cmake" goto no_ndk
for %%I in ("%NDK%") do set "NDK=%%~fI"
set "NDK_FOUND="
for /f "tokens=2 delims== " %%V in ('findstr /b /c:"Pkg.Revision" "%NDK%\source.properties"') do set "NDK_FOUND=%%V"
if "%NDK_FOUND%"=="%NDK_VERSION%" goto ndk_version_ok
echo Warning: NDK %NDK_FOUND% in %NDK%; ContraptionFabricator builds with %NDK_VERSION%.
:ndk_version_ok
rem The presets read it from here.
set "ANDROID_NDK_HOME=%NDK%"
set "NDK_BIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin"

call "%HERE%windows_prepare.bat" "%ROOT%"
if errorlevel 1 exit /b 1

cd /d "%ROOT%"

echo.
echo Configuring the android-arm64-%CONFIG% preset with the NDK in %NDK%...
cmake --preset android-arm64-%CONFIG% || goto build_failed

echo.
echo Building libkimodo.so for arm64-v8a (%CONFIG%)...
cmake --build --preset android-arm64-library-%CONFIG% || goto build_failed

echo.
echo Checking and packaging into %OUT%...
cmake -DKIMODO_TARGET=android -DKIMODO_ANDROID_ABI=arm64-v8a "-DKIMODO_SOURCE_DIR=%ROOT%" ^
      "-DKIMODO_BUILD_DIR=%ROOT%\build\android-arm64-v8a-%CONFIG%" "-DKIMODO_PACKAGE_DIR=%OUT%" ^
      -DKIMODO_WEIGHTS=%WEIGHTS% -DKIMODO_TEXT_WEIGHTS=OFF ^
      "-DKIMODO_NM=%NDK_BIN%\llvm-nm.exe" "-DKIMODO_READELF=%NDK_BIN%\llvm-readelf.exe" "-DKIMODO_STRIP=%NDK_BIN%\llvm-strip.exe" ^
      -P "%HERE%package.cmake" || goto package_failed

echo.
echo Packaged the Kimodo library for Android (%CONFIG%) in %OUT%
echo.
echo To check it on a phone or headset (USB debugging on, adb from the SDK's
echo platform-tools): make a prompt's embedding here, then generate from it there:
echo     build\release\kimodo-capi-smoke.exe --library build\release\kimodo.dll ^^
echo         --data dist\kimodo-windows\weights --save-embedding walk.f32
echo     adb push "%OUT%\lib\arm64-v8a\libkimodo.so" "%OUT%\tools\arm64-v8a\kimodo-capi-smoke" walk.f32 ^^
echo         "%OUT%\weights\kimodo-soma-seed-v1.1-f32.gguf" /data/local/tmp/
echo     adb shell "cd /data/local/tmp && chmod +x kimodo-capi-smoke && ./kimodo-capi-smoke --library ./libkimodo.so --data . --no-text --embedding-file walk.f32"
exit /b 0

:usage
echo Usage: scripts\build\build_library_android.bat [--release ^| --debug] [--out ^<folder^>] [--no-weights] [--ndk ^<folder^>]
echo.
echo   --release     optimised build (the default)
echo   --debug       debug build
echo   --out         the package folder (default dist\kimodo-android)
echo   --no-weights  leave the package's weights as they are
echo   --ndk         the Android NDK (default: ANDROID_NDK_HOME, ANDROID_NDK_ROOT,
echo                 or the SDK's ndk\%NDK_VERSION%)
exit /b 0

:missing_value
echo %~1 needs a folder.
exit /b 2

:no_ndk
echo The Android NDK was not found at %NDK%.  Install NDK %NDK_VERSION% with
echo Android Studio's SDK Manager (SDK Tools, "NDK (Side by side)", show package
echo details), or: sdkmanager "ndk;%NDK_VERSION%"
echo and set ANDROID_NDK_HOME, or pass --ndk ^<folder^>.
exit /b 1

:build_failed
echo The CMake build failed; see the errors above.
exit /b 1

:package_failed
echo Packaging failed; see the reason above.
exit /b 1
