# capture_realserver.ps1
# Capture a REAL RS2:Vietnam server session so we can reverse-engineer the
# post-spawn actor replication (local PlayerController, pawn spawn/possess,
# ServerMove movement RPCs, property format) that our emulator still lacks.
#
# Usage:  right-click -> Run with PowerShell   (or:  powershell -ExecutionPolicy Bypass -File tools\capture_realserver.ps1)
# Then:   join a real server, pick a team/role, DEPLOY/spawn, move (WASD), mouse-look,
#         sprint, crouch, fire a few shots, maybe die+respawn. ~30-60s is plenty.
# Then:   come back here and press ENTER to stop. Tell Claude the output path.
#
# It auto-detects your default network adapter and captures UDP game traffic
# (ports 7000-8000 + Steam query 27015) to the output file below.

$ErrorActionPreference = 'Stop'
$dumpcap = "C:\Program Files\Wireshark\dumpcap.exe"
$out     = "D:\RE-Tools\rs2_realserver_capture.pcapng"
$filter  = "udp portrange 7000-8000 or udp port 27015"

if (-not (Test-Path $dumpcap)) { Write-Host "dumpcap not found at $dumpcap" -ForegroundColor Red; exit 1 }

# Find the default-route adapter and its dumpcap interface name.
$idx = (Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object RouteMetric | Select-Object -First 1).InterfaceIndex
$ad  = Get-NetAdapter -InterfaceIndex $idx
$iface = "\Device\NPF_$($ad.InterfaceGuid)"
Write-Host "Capturing on: $($ad.Name) ($($ad.InterfaceDescription))" -ForegroundColor Cyan
Write-Host "Filter: $filter" -ForegroundColor Cyan
Write-Host "Output: $out" -ForegroundColor Cyan

if (Test-Path $out) { Remove-Item $out -Force }

# -B 64 = bigger kernel buffer (avoid drops during a busy match)
$p = Start-Process -FilePath $dumpcap -ArgumentList @('-i', $iface, '-f', $filter, '-B', '64', '-w', $out) -NoNewWindow -PassThru
Start-Sleep -Seconds 1
if ($p.HasExited) {
    Write-Host "dumpcap exited immediately (exit $($p.ExitCode)). If it's a permissions error, run this script As Administrator." -ForegroundColor Red
    exit 1
}
Write-Host ""
Write-Host ">>> CAPTURING. Go play on a real server now: join -> team/role -> DEPLOY -> move/look/shoot -> (die/respawn)." -ForegroundColor Green
Write-Host ">>> When done, come back here and press ENTER to stop." -ForegroundColor Green
[void](Read-Host)

Stop-Process -Id $p.Id -Force
Start-Sleep -Milliseconds 500
$sz = (Get-Item $out -ErrorAction SilentlyContinue).Length
Write-Host ""
Write-Host "Stopped. Wrote $out ($([math]::Round($sz/1KB)) KB)." -ForegroundColor Cyan
Write-Host "Tell Claude: capture saved to $out" -ForegroundColor Yellow
