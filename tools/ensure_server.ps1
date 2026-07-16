# Scheduled-task compatibility wrapper. The controller owns process identity,
# log rotation, and the explicit-stop sentinel; this file intentionally keeps
# the historical no-argument "ensure" behavior.
$ErrorActionPreference = "Stop"
trap {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    exit 1
}
& (Join-Path $PSScriptRoot "server_control.ps1") -Action Ensure
if (-not $?) {
    exit 1
}
exit 0
