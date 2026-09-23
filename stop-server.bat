@echo off
rem Stop the Kimodo server started by start-server.bat or start-demo.bat.
rem
rem Also stops the kmd-generate worker the server spawned, since that process
rem is what holds the text encoder and motion model in VRAM. Other
rem kmd-generate processes (for example a command-line run in another window)
rem are left alone.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$servers = @(Get-Process kimodo-demo -ErrorAction SilentlyContinue);" ^
  "if ($servers.Count -eq 0) { Write-Host 'The Kimodo server is not running.'; exit 0 };" ^
  "foreach ($server in $servers) {" ^
  "  Get-CimInstance Win32_Process -Filter ('Name=''kmd-generate.exe'' AND ParentProcessId=' + $server.Id) | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue };" ^
  "  Stop-Process -Id $server.Id -Force" ^
  "};" ^
  "Write-Host ('Stopped the Kimodo server (PID ' + ($servers.Id -join ', ') + ').')"
