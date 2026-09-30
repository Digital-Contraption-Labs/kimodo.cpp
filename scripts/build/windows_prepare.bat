@echo off
rem Shared by the Windows build scripts in this folder, which CALL it:
rem
rem     call "%HERE%windows_prepare.bat" "<repository root>"
rem     if errorlevel 1 exit /b 1
rem
rem Loads the Visual Studio x64 toolchain unless the shell already has it,
rem checks for CMake, Ninja and the Vulkan SDK, and checks out a missing ggml
rem or eigen submodule.  It has no SETLOCAL of its own, so what it sets lands
rem in the caller's environment, which the caller's SETLOCAL confines.  On a
rem missing prerequisite it names it, says how to install it and exits 1.

if "%~1"=="" (
    echo windows_prepare.bat needs the repository root as its argument.
    exit /b 1
)
set "KIMODO_PREPARE_ROOT=%~f1"

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

rem ggml's Vulkan backend, which runs the model on the GPU, needs the SDK.
if not defined VULKAN_SDK goto no_vulkan

rem Check out a missing submodule at the commit this repository pins.  One
rem that is already checked out is left alone, whatever commit it is on.
if exist "%KIMODO_PREPARE_ROOT%\ggml\CMakeLists.txt" goto have_ggml
echo Checking out the ggml submodule...
git -C "%KIMODO_PREPARE_ROOT%" submodule update --init --recursive -- ggml || goto submodule_failed
:have_ggml
if exist "%KIMODO_PREPARE_ROOT%\eigen\Eigen\Sparse" goto have_eigen
echo Checking out the eigen submodule...
git -C "%KIMODO_PREPARE_ROOT%" submodule update --init --recursive -- eigen || goto submodule_failed
:have_eigen
rem The Vulkan and SPIR-V headers Android builds compile against.
if exist "%KIMODO_PREPARE_ROOT%\vulkan-headers\include\vulkan\vulkan.hpp" goto have_vulkan_headers
echo Checking out the vulkan-headers submodule...
git -C "%KIMODO_PREPARE_ROOT%" submodule update --init -- vulkan-headers || goto submodule_failed
:have_vulkan_headers
if exist "%KIMODO_PREPARE_ROOT%\spirv-headers\include\spirv\unified1\spirv.hpp" goto have_spirv_headers
echo Checking out the spirv-headers submodule...
git -C "%KIMODO_PREPARE_ROOT%" submodule update --init -- spirv-headers || goto submodule_failed
:have_spirv_headers
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

:no_vulkan
echo The Vulkan SDK was not found: VULKAN_SDK is not set.  Install it from
echo https://vulkan.lunarg.com/sdk/home and open a new shell.
exit /b 1

:submodule_failed
echo git submodule update failed.  This needs Git and a clone of the
echo repository; run "git submodule update --init --recursive" in
echo %KIMODO_PREPARE_ROOT% to see why.
exit /b 1
