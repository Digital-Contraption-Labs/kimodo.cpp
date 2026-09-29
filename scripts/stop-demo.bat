@echo off
rem Stop the Kimodo demo server started by start-demo.bat.  It is the same
rem server start-server.bat starts, so stop-server.bat does the work.
call "%~dp0stop-server.bat"
