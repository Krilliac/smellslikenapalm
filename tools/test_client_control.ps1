[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$sourceController = Join-Path $PSScriptRoot "client_control.ps1"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempRoot = Join-Path $tempBase (
    "RS2V client control test {0}" -f [Guid]::NewGuid().ToString("N"))
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Assert-Contains([string[]]$Lines, [string]$Pattern, [string]$Message) {
    if (-not ($Lines -match $Pattern)) {
        throw "$Message`nActual output:`n$($Lines -join [Environment]::NewLine)"
    }
}

try {
    $toolsRoot = Join-Path $tempRoot "tools"
    $stateRoot = Join-Path $tempRoot ".client-control"
    New-Item -ItemType Directory -Path $toolsRoot, $stateRoot -Force | Out-Null
    Copy-Item -LiteralPath $sourceController -Destination $toolsRoot
    $controller = Join-Path $toolsRoot "client_control.ps1"

    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $controller, [ref]$null, [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -gt 0) {
        throw "PowerShell parse failed: $($parseErrors[0].Message)"
    }

    $status = @(& $controller -Action Status)
    Assert-Contains $status '^client=(stopped state=missing|running state=unmanaged )' `
        "Missing-state status was not safe with the current global client state"

    $selfTest = @(& $controller -Action SelfTest)
    Assert-Contains $selfTest '^client-control self-test=ok checks=11 lifecycle=untouched$' `
        "Pure self-test did not pass"

    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "state.json"), "{not-json", $utf8NoBom)
    $corrupt = @(& $controller -Action Status)
    Assert-Contains $corrupt '^client=unknown state=corrupt candidates=\d+ ' `
        "Corrupt state was not reported safely"

    $stale = [ordered]@{
        schema = 1
        pid = 2147483000
        process_start_filetime_utc = "1"
        executable = Join-Path $tempRoot "VNGame.exe"
        launch_token = [Guid]::NewGuid().ToString("D")
        launched_utc = [DateTime]::UtcNow.ToString("o")
        steam_app_id = 418460
    }
    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "state.json"),
        (($stale | ConvertTo-Json -Depth 3) + "`n"), $utf8NoBom)
    $staleStatus = @(& $controller -Action Status)
    Assert-Contains $staleStatus '^client=(stopped state=stale |running state=stale-unmanaged )' `
        "Stale state was not reported safely"

    Write-Output "client-control isolated self-test: PASS"
} finally {
    if (Test-Path -LiteralPath $tempRoot) {
        $resolvedTempRoot = [IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = $tempBase.TrimEnd('\') + '\'
        if (-not $resolvedTempRoot.StartsWith(
                $tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            -not [IO.Path]::GetFileName($resolvedTempRoot).StartsWith(
                "RS2V client control test ", [StringComparison]::Ordinal)) {
            throw "Refusing to remove unexpected self-test directory: $resolvedTempRoot"
        }
        Remove-Item -LiteralPath $resolvedTempRoot -Recurse -Force
    }
}
