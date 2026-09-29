@echo off
setlocal
rem Start the Kimodo text-to-motion demo on Windows: the service with its
rem gallery persisted to demo-output\ (records, raw streams and GLBs, reloaded
rem next time) and the demo page opened in the browser.  For the service
rem alone -- in memory, no files, no browser -- run scripts\start-server.bat.
rem
rem   scripts\start-demo.bat                     serve on http://127.0.0.1:8094 and open it
rem   scripts\start-demo.bat -addr 0.0.0.0:8094  pass extra flags through to the server
rem
rem The server runs in this window; press Ctrl+C or close the window to stop
rem it, or run scripts\stop-demo.bat from anywhere.  Build the native worker
rem first with scripts\build\build_service.bat.
set "KIMODO_OPEN_URL=http://127.0.0.1:8094"
call "%~dp0start-server.bat" -output demo-output %*
