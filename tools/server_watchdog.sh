#!/usr/bin/env bash
# Keep rs2v_server alive (it gets killed externally every ~10-40min) AND preserve each
# instance's stdout log so a real client's connection trace is never lost to a restart.
# Run via the Bash tool's run_in_background so the harness keeps it alive across turns.
cd /d/smellslikenapalm
while true; do
  if ! tasklist //FI "IMAGENAME eq rs2v_server.exe" 2>/dev/null | grep -qi rs2v_server; then
    # Preserve the prior instance's log (if it has any client login) before truncating.
    if [ -f server_live.log ] && grep -q "FireClientLoggedIn" server_live.log 2>/dev/null; then
      cp server_live.log "server_live.session_$(date '+%Y%m%d_%H%M%S').log"
    fi
    # Prune: keep only the 6 most recent preserved session logs.
    ls -1t server_live.session_*.log 2>/dev/null | tail -n +7 | xargs -r rm -f
    powershell.exe -NoProfile -Command "(Get-Content config/server.ini) -replace 'log_level\s*=\s*\w+','log_level=debug' | Set-Content config/server.ini -Encoding utf8; Start-Process -FilePath 'D:\smellslikenapalm\build\Debug\rs2v_server.exe' -ArgumentList '--config','config/server.ini' -WindowStyle Hidden -RedirectStandardOutput 'D:\smellslikenapalm\server_live.log' -RedirectStandardError 'D:\smellslikenapalm\server_live.err.log'" >/dev/null 2>&1
    echo "[watchdog $(date '+%H:%M:%S')] (re)started rs2v_server"
    sleep 5
  fi
  sleep 12
done
