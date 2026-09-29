@echo off
setlocal
rem Start the Kimodo text-to-motion service on Windows: the HTTP API alone,
rem gallery in memory, nothing written to disk, no browser.  This is what a
rem client such as ContraptionFabricator's Clip Editor talks to.
rem
rem   scripts\start-server.bat                      serve on http://127.0.0.1:8094
rem   scripts\start-server.bat -addr 0.0.0.0:8094   pass extra flags through to the server
rem   scripts\start-server.bat -output demo-output  persist the gallery to disk as well
rem                                                 (start-demo.bat does this and
rem                                                 opens the demo page)
rem
rem Build the native worker first with scripts\build\build_service.bat.  The
rem server runs in this window; press Ctrl+C or close the window to stop it, or
rem run scripts\stop-server.bat from anywhere.  KIMODO_OPEN_URL, when a caller
rem sets it, is opened in the browser once the server listens.  Paths such as
rem -output resolve against the repository root, where this runs.
cd /d "%~dp0.."

set "ADDR=127.0.0.1:8094"
set "GENERATOR=build\release\kmd-generate.exe"
set "SERVER=build\kimodo-demo.exe"

rem Locate Go, falling back to the default install path for shells opened
rem before Go was added to PATH.
set "GO=go"
where go >nul 2>nul || set "GO=C:\Program Files\Go\bin\go.exe"
if not exist "%GO%" if not "%GO%"=="go" (
    echo Go was not found. Install it with: winget install GoLang.Go
    exit /b 1
)

if not exist "%GENERATOR%" (
    echo %GENERATOR% not found.
    echo Build it first with: scripts\build\build_service.bat
    exit /b 1
)
if not exist "tokenizer.gguf" (
    echo Warning: tokenizer.gguf not found in %CD%.
    echo Download weights with: python scripts\download_gguf_weights.py --output . --model soma-rp-v1.1
)

tasklist /fi "imagename eq kimodo-demo.exe" 2>nul | findstr /i /c:"kimodo-demo.exe" >nul
if not errorlevel 1 (
    echo The Kimodo server is already running at http://%ADDR% -- scripts\stop-server.bat stops it.
    if defined KIMODO_OPEN_URL start "" "%KIMODO_OPEN_URL%"
    exit /b 0
)

echo Building server...
"%GO%" build -o "%SERVER%" .\demo
if errorlevel 1 (
    echo go build failed.
    exit /b 1
)

echo.
echo Kimodo service: http://%ADDR%
echo Press Ctrl+C here or run scripts\stop-server.bat to stop it.
echo.
rem A caller that wants the browser (start-demo.bat) gets it once the server
rem has had a moment to start listening.
if defined KIMODO_OPEN_URL start "" /b cmd /c "ping -n 4 127.0.0.1 >nul & start "" "%KIMODO_OPEN_URL%""
"%SERVER%" -addr %ADDR% -generator build/release/kmd-generate %*
