# Ensures rs2v_server is running; (re)starts it if not. Registered as a per-minute
# Scheduled Task (RS2VServerWatchdog) so it survives harness/session process reaping
# that kills both the server and any bash-loop watchdog. Preserves the prior instance's
# log (if it captured a client login) before the restart truncates it.
$ErrorActionPreference = "SilentlyContinue"
Set-Location D:\smellslikenapalm
$p = Get-Process rs2v_server -ErrorAction SilentlyContinue
if (-not $p) {
    if ((Test-Path server_live.log) -and (Select-String -Path server_live.log -Pattern "FireClientLoggedIn" -Quiet)) {
        Copy-Item server_live.log ("server_live.session_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss")) -Force
        Get-ChildItem server_live.session_*.log | Sort-Object LastWriteTime -Descending | Select-Object -Skip 6 | Remove-Item -Force
    }
    (Get-Content config/server.ini) -replace 'log_level\s*=\s*\w+','log_level=debug' | Set-Content config/server.ini -Encoding utf8
    Start-Process -FilePath "D:\smellslikenapalm\build\Debug\rs2v_server.exe" -ArgumentList "--config","config/server.ini" -WindowStyle Hidden -RedirectStandardOutput "D:\smellslikenapalm\server_live.log" -RedirectStandardError "D:\smellslikenapalm\server_live.err.log"
}
