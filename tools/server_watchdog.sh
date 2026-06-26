#!/usr/bin/env bash
# Keep rs2v_server alive: if it's not running, (re)start it detached with debug logging.
# Launched via the Bash tool's run_in_background so the harness keeps it alive across turns.
cd /d/smellslikenapalm
while true; do
  if ! tasklist //FI "IMAGENAME eq rs2v_server.exe" 2>/dev/null | grep -qi rs2v_server; then
    powershell.exe -NoProfile -Command "(Get-Content config/server.ini) -replace 'log_level\s*=\s*\w+','log_level=debug' | Set-Content config/server.ini -Encoding utf8; Start-Process -FilePath 'D:\smellslikenapalm\build\Debug\rs2v_server.exe' -ArgumentList '--config','config/server.ini' -WindowStyle Hidden -RedirectStandardOutput 'D:\smellslikenapalm\server_live.log' -RedirectStandardError 'D:\smellslikenapalm\server_live.err.log'" >/dev/null 2>&1
    echo "[watchdog $(date '+%H:%M:%S')] started rs2v_server"
    sleep 5
  fi
  sleep 12
done
