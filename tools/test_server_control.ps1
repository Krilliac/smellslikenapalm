[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$sourceController = Join-Path $PSScriptRoot "server_control.ps1"
$sourceEnsure = Join-Path $PSScriptRoot "ensure_server.ps1"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempRoot = Join-Path $tempBase (
    "RS2V server control test {0}" -f [Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object Text.UTF8Encoding($false)

function Assert-Contains([string[]]$Lines, [string]$Pattern, [string]$Message) {
    if (-not ($Lines -match $Pattern)) {
        throw "$Message`nActual output:`n$($Lines -join [Environment]::NewLine)"
    }
}

try {
    $toolsRoot = Join-Path $tempRoot "tools"
    $configRoot = Join-Path $tempRoot "config"
    $binaryRoot = Join-Path $tempRoot "build-merge\RelWithDebInfo"
    $stateRoot = Join-Path $tempRoot ".server-control"
    New-Item -ItemType Directory -Path $toolsRoot, $configRoot, $binaryRoot,
        $stateRoot -Force | Out-Null

    Copy-Item -LiteralPath $sourceController -Destination $toolsRoot
    Copy-Item -LiteralPath $sourceEnsure -Destination $toolsRoot
    [IO.File]::WriteAllText(
        (Join-Path $configRoot "server.ini"), "[Server]`n", $utf8NoBom)
    [IO.File]::WriteAllBytes(
        (Join-Path $binaryRoot "rs2v_server.exe"), (New-Object byte[] 0))

    $controller = Join-Path $toolsRoot "server_control.ps1"
    $ensure = Join-Path $toolsRoot "ensure_server.ps1"
    foreach ($script in @($controller, $ensure)) {
        $parseErrors = $null
        [Management.Automation.Language.Parser]::ParseFile(
            $script, [ref]$null, [ref]$parseErrors) | Out-Null
        if ($parseErrors.Count -gt 0) {
            throw "PowerShell parse failed for ${script}: $($parseErrors[0].Message)"
        }
    }

    $status = @(& $controller -Action Status)
    Assert-Contains $status 'server=stopped state=missing ensure=enabled' `
        "Missing-state status was not deterministic"

    $selfTest = @(& $controller -Action SelfTest)
    Assert-Contains $selfTest 'self-test=ok .*state=missing quoting=ok' `
        "Self-test did not validate the path-with-spaces fixture"

    $stopOutput = @(& $controller -Action Stop)
    Assert-Contains $stopOutput 'server stopped ensure=disabled' `
        "Explicit Stop did not create its sentinel in the isolated repository"
    $ensureOutput = @(& powershell.exe -NoProfile -File $ensure 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Ensure wrapper returned exit code $LASTEXITCODE"
    }
    Assert-Contains $ensureOutput 'ensure suppressed by explicit-stop sentinel' `
        "Ensure ignored the explicit-stop sentinel"

    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "state.json"), "{not-json", $utf8NoBom)
    $corruptStatus = @(& $controller -Action Status)
    Assert-Contains $corruptStatus 'server=unknown state=corrupt' `
        "Corrupt state was not reported safely"

    $staleState = [ordered]@{
        schema = 1
        pid = 2147483000
        process_start_filetime_utc = "1"
        executable = Join-Path $binaryRoot "rs2v_server.exe"
        config = Join-Path $configRoot "server.ini"
    }
    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "state.json"),
        ($staleState | ConvertTo-Json -Depth 3), $utf8NoBom)
    $staleStatus = @(& $controller -Action Status)
    Assert-Contains $staleStatus 'server=stopped state=stale' `
        "Stale state was not reported safely"

    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "server.stdout.log"), "first`nsecond`n", $utf8NoBom)
    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "server.stderr.log"), "error-one`n", $utf8NoBom)
    $logs = @(& $controller -Action Logs -TailLines 1)
    Assert-Contains $logs '^second$' "Stdout tail returned the wrong line"
    Assert-Contains $logs '^error-one$' "Stderr tail returned the wrong line"
    if ($logs -match '^first$') {
        throw "Log tail returned more lines than requested"
    }

    Remove-Item -LiteralPath (Join-Path $stateRoot "disabled") -Force
    [IO.File]::WriteAllText(
        (Join-Path $stateRoot "state.json"), "{not-json", $utf8NoBom)
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $failedEnsureOutput = @(& powershell.exe -NoProfile -File $ensure 2>&1)
        $failedEnsureCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($failedEnsureCode -eq 0) {
        throw "Ensure wrapper returned success after its isolated server start failed"
    }
    if ($failedEnsureOutput.Count -eq 0) {
        throw "Ensure wrapper failure did not emit a diagnostic"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $stateRoot "disabled") -PathType Leaf)) {
        throw "Failed startup did not restore the explicit-stop sentinel"
    }

    Write-Output "server-control isolated self-test: PASS"
} finally {
    if (Test-Path -LiteralPath $tempRoot) {
        $resolvedTempRoot = [IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = $tempBase.TrimEnd('\') + '\'
        if (-not $resolvedTempRoot.StartsWith(
                $tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            -not [IO.Path]::GetFileName($resolvedTempRoot).StartsWith(
                "RS2V server control test ", [StringComparison]::Ordinal)) {
            throw "Refusing to remove unexpected self-test directory: $resolvedTempRoot"
        }
        Remove-Item -LiteralPath $resolvedTempRoot -Recurse -Force
    }
}
