@echo off
setlocal
rem Copy a Kimodo library package into ContraptionFabricator's tree (see
rem docs\SHARED_LIBRARY_PLAN.md, section 8):
rem
rem   the library, header, licences, VERSION.json  ->  <CF>\external\kimodo\<target>\
rem   the weights                                  ->  <CF>\Assets\kimodo\
rem
rem   scripts\build\stage_to_cf.bat --cf <CF folder>
rem   scripts\build\stage_to_cf.bat --cf <CF folder> --target windows
rem   scripts\build\stage_to_cf.bat --cf <CF folder> --package <package folder>
rem   scripts\build\stage_to_cf.bat --cf <CF folder> --no-weights
rem
rem The package is dist\kimodo-<target> unless --package names another; build
rem it first with scripts\build\build_library_<target>.  The library folder
rem is made an exact copy of the package, so a file the package dropped goes
rem from CF too.  The weights are only added and updated: Assets\kimodo may
rem hold CF's own files.  Unchanged files are not copied again.  Close CF
rem first: Windows will not replace a DLL a running program has loaded.

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "CF="
set "TARGET=windows"
set "PACKAGE="
set "WEIGHTS=1"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--cf" (
    if "%~2"=="" goto missing_value
    set "CF=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="--target" (
    if "%~2"=="" goto missing_value
    set "TARGET=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="--package" (
    if "%~2"=="" goto missing_value
    set "PACKAGE=%~2"
    shift
    shift
    goto parse
)
if /i "%~1"=="--no-weights" (
    set "WEIGHTS="
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

if not defined CF (
    echo --cf is required: the ContraptionFabricator folder.
    echo.
    call :usage
    exit /b 2
)
for %%I in ("%CF%") do set "CF=%%~fI"
if not defined PACKAGE set "PACKAGE=%ROOT%\dist\kimodo-%TARGET%"
for %%I in ("%PACKAGE%") do set "PACKAGE=%%~fI"

rem A wrong --cf must not become a folder full of copies: CF has Assets\.
if not exist "%CF%\Assets\" goto not_cf
if not exist "%PACKAGE%\VERSION.json" goto no_package
if not exist "%PACKAGE%\include\kimodo\kimodo_capi.h" goto no_package
if defined WEIGHTS if not exist "%PACKAGE%\weights\" goto no_weights

set "LIBRARY_DEST=%CF%\external\kimodo\%TARGET%"
echo Staging %PACKAGE%
echo     library into %LIBRARY_DEST%
rem Exit codes below 8 are robocopy's successes.  /R:2 /W:1: a locked file
rem fails in seconds instead of robocopy's default of a million retries.
robocopy "%PACKAGE%" "%LIBRARY_DEST%" /MIR /XD "%PACKAGE%\weights" /R:2 /W:1 /NJH /NJS /NDL /NP
if errorlevel 8 goto copy_failed

if not defined WEIGHTS goto staged
echo     weights into %CF%\Assets\kimodo
robocopy "%PACKAGE%\weights" "%CF%\Assets\kimodo" /E /XF .sha256-cache /R:2 /W:1 /NJH /NJS /NDL /NP
if errorlevel 8 goto copy_failed

:staged
echo.
echo Staged.  CF loads %LIBRARY_DEST%\bin\kimodo.dll with the weights in
echo %CF%\Assets\kimodo.
exit /b 0

:usage
echo Usage: scripts\build\stage_to_cf.bat --cf ^<CF folder^> [--target ^<target^>] [--package ^<folder^>] [--no-weights]
echo.
echo   --cf          the ContraptionFabricator folder (it has Assets\)
echo   --target      windows (the default), for dist\kimodo-^<target^>
echo   --package     a package folder other than dist\kimodo-^<target^>
echo   --no-weights  copy the library only
exit /b 0

:missing_value
echo %~1 needs a value.
exit /b 2

:not_cf
echo %CF% has no Assets folder, so it is not a ContraptionFabricator tree.
exit /b 1

:no_package
echo %PACKAGE% is not a Kimodo package.  Build it with
echo     scripts\build\build_library_%TARGET%.bat
exit /b 1

:no_weights
echo %PACKAGE% has no weights.  Build it without --no-weights, or stage with
echo --no-weights.
exit /b 1

:copy_failed
echo Copying failed; see robocopy's report above.  Is CF running with the
echo library loaded?
exit /b 1
