[CmdletBinding()]
param(
    [ValidateSet("Status", "Start", "Stop", "Restart", "Ensure", "Logs", "Watch", "SelfTest")]
    [string]$Action = "Status",

    [string]$Binary = "",
    [string]$Config = "config/server.ini",
    [ValidateLength(0, 128)]
    [string]$Map = "",
    [ValidateRange(0, 65535)]
    [int]$Port = 0,
    [ValidateRange(0, 65535)]
    [int]$EacPort = 0,

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$BuildConfig = "RelWithDebInfo",

    [ValidateSet('canonical', 'installed')]
    [string]$ReplicationBootstrapVariant = 'installed',

    [ValidateRange(1, 60)]
    [int]$WaitSeconds = 10,

    [ValidateRange(1, 10000)]
    [int]$TailLines = 40,

    [ValidateRange(1, 60)]
    [int]$WatchIntervalSeconds = 2
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 can return process exit code 0 for an otherwise
# unhandled terminating script error. Scheduled Task history must be able to
# distinguish a safe no-op from a failed control action.
trap {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    exit 1
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$stateRoot = Join-Path $repoRoot ".server-control"
$logRoot = Join-Path $stateRoot "logs"
$disabledPath = Join-Path $stateRoot "disabled"
$statePath = Join-Path $stateRoot "state.json"
$stdoutPath = Join-Path $stateRoot "server.stdout.log"
$stderrPath = Join-Path $stateRoot "server.stderr.log"
$utf8NoBom = [Text.UTF8Encoding]::new($false, $true)
$stateSchema = 1
$maxArchivedLogSets = 20
$replicationBootstrapEnvironmentName = "RS2V_REPLICATION_BOOTSTRAP_VARIANT"

function Initialize-NativeProcessQuery {
    if ($null -ne ("RS2VServerControl.NativeProcessQuery" -as [type])) {
        return
    }

    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace RS2VServerControl
{
    public static class NativeProcessQuery
    {
        private const uint ProcessQueryLimitedInformation = 0x1000;

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(
            uint desiredAccess, bool inheritHandle, int processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool QueryFullProcessImageName(
            IntPtr process, int flags, StringBuilder imagePath, ref int size);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);

        public static string GetImagePath(int processId)
        {
            IntPtr process = OpenProcess(
                ProcessQueryLimitedInformation, false, processId);
            if (process == IntPtr.Zero)
                return null;
            try
            {
                var path = new StringBuilder(32768);
                int length = path.Capacity;
                return QueryFullProcessImageName(process, 0, path, ref length)
                    ? path.ToString()
                    : null;
            }
            finally
            {
                CloseHandle(process);
            }
        }
    }
}
'@
}

function Get-ProcessExecutablePath([Diagnostics.Process]$Process,
                                   [int]$RetryMilliseconds = 0) {
    if ($null -eq $Process) {
        throw "Process identity query requires a process"
    }
    if ($RetryMilliseconds -lt 0 -or $RetryMilliseconds -gt 10000) {
        throw "Process path retry must be between 0 and 10000 milliseconds"
    }

    Initialize-NativeProcessQuery
    $deadline = [DateTime]::UtcNow.AddMilliseconds($RetryMilliseconds)
    do {
        if ($Process.HasExited) {
            throw [InvalidOperationException]::new("Process exited before its path was available")
        }

        $path = ""
        try { $path = [string]$Process.Path } catch { $path = "" }
        if ([string]::IsNullOrWhiteSpace($path)) {
            $path = [string][RS2VServerControl.NativeProcessQuery]::GetImagePath(
                $Process.Id)
        }
        if (-not [string]::IsNullOrWhiteSpace($path)) {
            return [IO.Path]::GetFullPath($path)
        }

        if ([DateTime]::UtcNow -ge $deadline) {
            break
        }
        Start-Sleep -Milliseconds 25
        try { $Process.Refresh() } catch { }
    } while ($true)

    throw "Process executable path remained unavailable after $RetryMilliseconds milliseconds"
}

function Test-PathInsideRepo([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $repoRoot.TrimEnd('\') + '\'
    return $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Resolve-RepoPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Repository path cannot be empty"
    }

    $fullPath = if ([IO.Path]::IsPathRooted($Path)) {
        [IO.Path]::GetFullPath($Path)
    } else {
        [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
    }

    if (-not (Test-PathInsideRepo $fullPath)) {
        throw "Refusing path outside repository: $fullPath"
    }
    return $fullPath
}

function Format-ServerArgumentLine([string]$ConfigPath, [string]$MapName = "",
                                   [int]$PortOverride = 0,
                                   [int]$EacPortOverride = 0) {
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        throw "Server config path cannot be empty"
    }
    if ($ConfigPath.IndexOf('"') -ge 0 -or $ConfigPath.IndexOf([char]0) -ge 0) {
        throw "Server config path contains an invalid command-line character"
    }
    # Start-Process flattens ArgumentList into one native command line on Windows.
    # Preserve explicit quotes so paths containing spaces arrive as one argv value.
    $arguments = '--config "{0}"' -f $ConfigPath
    if (-not [string]::IsNullOrWhiteSpace($MapName)) {
        if ($MapName.Length -gt 128 -or $MapName.IndexOf('"') -ge 0 -or
            $MapName.IndexOf([char]0) -ge 0 -or $MapName -match '[\x01-\x1F\x7F]') {
            throw "Server map name contains an invalid command-line character"
        }
        $arguments += ' --map "{0}"' -f $MapName
    }
    if ($PortOverride -lt 0 -or $PortOverride -gt 65535) {
        throw "Server port override must be zero or in the range 1 through 65535"
    }
    if ($PortOverride -gt 0) {
        $arguments += ' --port {0}' -f $PortOverride
    }
    if ($EacPortOverride -lt 0 -or $EacPortOverride -gt 65535) {
        throw "EAC port override must be zero or in the range 1 through 65535"
    }
    if ($EacPortOverride -gt 0) {
        $arguments += ' --eac-port {0}' -f $EacPortOverride
    }
    return $arguments
}

function Resolve-ReplicationBootstrapEnvironmentValue([string]$Variant) {
    switch ($Variant) {
        "installed" { return "installed" }
        # Canonical is the server's no-override path. Passing the literal value
        # "canonical" would instead select a nonempty override and fail closed.
        "canonical" { return $null }
        default { throw "Unsupported replication bootstrap variant: $Variant" }
    }
}

function Assert-NoReparseEscape([string]$Path) {
    $fullPath = Resolve-RepoPath $Path
    $prefix = $repoRoot.TrimEnd('\') + '\'
    $relativePath = $fullPath.Substring($prefix.Length)
    $currentPath = $repoRoot

    foreach ($segment in $relativePath.Split(@('\', '/'),
            [StringSplitOptions]::RemoveEmptyEntries)) {
        $currentPath = Join-Path $currentPath $segment
        $item = Get-Item -LiteralPath $currentPath -Force -ErrorAction SilentlyContinue
        if ($null -eq $item) {
            break
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing reparse-point path inside repository: $currentPath"
        }
    }
    return $fullPath
}

function Assert-RepoFile([string]$Path, [string]$Purpose) {
    $fullPath = Assert-NoReparseEscape $Path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "${Purpose} not found: $fullPath"
    }
    return $fullPath
}

function Assert-ControlDirectory([string]$Path) {
    $fullPath = Resolve-RepoPath $Path
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if (-not $item.PSIsContainer) {
            throw "Server-control path is not a directory: $fullPath"
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing reparse-point server-control directory: $fullPath"
        }
    } else {
        New-Item -ItemType Directory -Path $fullPath | Out-Null
        $item = Get-Item -LiteralPath $fullPath -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing reparse-point server-control directory: $fullPath"
        }
    }
    return $fullPath
}

function Initialize-ControlDirectories {
    Assert-ControlDirectory $stateRoot | Out-Null
    Assert-ControlDirectory $logRoot | Out-Null
}

function Write-AtomicUtf8([string]$Path, [string]$Text) {
    $fullPath = Assert-NoReparseEscape $Path
    $parent = Split-Path -Parent $fullPath
    Assert-ControlDirectory $parent | Out-Null

    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Refusing unsafe server-control file: $fullPath"
        }
    }

    $temporaryPath = Join-Path $parent (
        ".{0}.{1}.{2}.tmp" -f ([IO.Path]::GetFileName($fullPath)), $PID,
        [Guid]::NewGuid().ToString("N"))
    try {
        [IO.File]::WriteAllText($temporaryPath, $Text, $utf8NoBom)
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            [IO.File]::Replace($temporaryPath, $fullPath, $null)
        } else {
            [IO.File]::Move($temporaryPath, $fullPath)
        }
    } finally {
        Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
    }
}

function Remove-ControlFile([string]$Path) {
    $fullPath = Assert-NoReparseEscape $Path
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Refusing unsafe server-control file: $fullPath"
        }
        Remove-Item -LiteralPath $fullPath -Force
    }
}

function Test-StopSentinel {
    if (-not (Test-Path -LiteralPath $disabledPath)) {
        return $false
    }
    Assert-RepoFile $disabledPath "Explicit-stop sentinel" | Out-Null
    return $true
}

function Write-StopSentinel {
    Write-AtomicUtf8 $disabledPath ([DateTime]::UtcNow.ToString("o"))
}

function Resolve-ServerBinary {
    $candidates = New-Object Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Binary)) {
        $candidates.Add((Resolve-RepoPath $Binary))
    } else {
        $candidates.Add((Join-Path $repoRoot "build-merge\$BuildConfig\rs2v_server.exe"))
        $candidates.Add((Join-Path $repoRoot "build\$BuildConfig\rs2v_server.exe"))
        if ($BuildConfig -eq "Debug") {
            $candidates.Add((Join-Path $repoRoot "out\build\x64-Debug\rs2v_server.exe"))
        }
    }

    foreach ($candidate in $candidates) {
        $fullPath = Assert-NoReparseEscape $candidate
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            if (-not [IO.Path]::GetFileName($fullPath).Equals(
                    "rs2v_server.exe", [StringComparison]::OrdinalIgnoreCase)) {
                throw "Server binary must be named rs2v_server.exe: $fullPath"
            }
            return $fullPath
        }
    }
    throw "No server binary found. Build rs2v_server ($BuildConfig) or pass -Binary. Tried: $($candidates -join ', ')"
}

function Get-RepoServerProcesses([int]$ExcludePid = 0) {
    $rows = @(Get-CimInstance Win32_Process -Filter "Name='rs2v_server.exe'" -ErrorAction Stop)
    foreach ($row in $rows) {
        $processId = [int]$row.ProcessId
        if ($processId -eq $ExcludePid) {
            continue
        }

        $path = [string]$row.ExecutablePath
        if ([string]::IsNullOrWhiteSpace($path)) {
            try {
                $path = [string]([Diagnostics.Process]::GetProcessById($processId).Path)
            } catch {
                $path = ""
            }
        }

        if ([string]::IsNullOrWhiteSpace($path)) {
            # An inaccessible same-name process cannot safely be ruled out.
            [pscustomobject]@{
                ProcessId = $processId
                ExecutablePath = ""
                IdentityKnown = $false
            }
            continue
        }

        try {
            $fullPath = [IO.Path]::GetFullPath($path)
        } catch {
            [pscustomobject]@{
                ProcessId = $processId
                ExecutablePath = ""
                IdentityKnown = $false
            }
            continue
        }

        if (Test-PathInsideRepo $fullPath) {
            [pscustomobject]@{
                ProcessId = $processId
                ExecutablePath = $fullPath
                IdentityKnown = $true
            }
        }
    }
}

function Get-RequiredStateValue([object]$State, [string]$Name) {
    $property = $State.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value -or
        [string]::IsNullOrWhiteSpace([string]$property.Value)) {
        throw "State is missing required field '$Name'"
    }
    return $property.Value
}

function Read-ControlState {
    if (-not (Test-Path -LiteralPath $statePath)) {
        return [pscustomobject]@{ Kind = "Missing"; Reason = ""; State = $null }
    }

    try {
        Assert-RepoFile $statePath "Server-control state" | Out-Null
        $bytes = [IO.File]::ReadAllBytes($statePath)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
            $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            throw "State must be UTF-8 without a byte-order mark"
        }
        $jsonText = $utf8NoBom.GetString($bytes)
        $state = $jsonText | ConvertFrom-Json -ErrorAction Stop

        $schemaProperty = $state.PSObject.Properties["schema"]
        $legacyState = $null -eq $schemaProperty
        if (-not $legacyState) {
            $schema = 0
            if (-not [int]::TryParse([string]$schemaProperty.Value, [ref]$schema) -or
                $schema -ne $stateSchema) {
                throw "Unsupported server-control state schema"
            }
        }

        $processId = 0
        if (-not [int]::TryParse(
                [string](Get-RequiredStateValue $state "pid"), [ref]$processId) -or
            $processId -le 0) {
            throw "State contains an invalid process id"
        }

        $startFileTime = [long]0
        if ($legacyState) {
            # Pre-schema controller states were recorded after the startup probe and
            # therefore only have an approximate started_utc value. Accepting them
            # keeps a live managed server stoppable during an in-place script upgrade.
            $legacyStart = [DateTimeOffset]::MinValue
            if (-not [DateTimeOffset]::TryParse(
                    [string](Get-RequiredStateValue $state "started_utc"),
                    [ref]$legacyStart)) {
                throw "Legacy state contains an invalid process creation time"
            }
            $startFileTime = $legacyStart.UtcDateTime.ToFileTimeUtc()
        } elseif (-not [long]::TryParse(
                [string](Get-RequiredStateValue $state "process_start_filetime_utc"),
                [ref]$startFileTime) -or $startFileTime -le 0) {
            throw "State contains an invalid process creation time"
        }

        $executable = Assert-NoReparseEscape (
            [string](Get-RequiredStateValue $state "executable"))
        if (-not [IO.Path]::GetFileName($executable).Equals(
                "rs2v_server.exe", [StringComparison]::OrdinalIgnoreCase)) {
            throw "State executable is not an rs2v_server binary"
        }
        $configPath = Assert-NoReparseEscape (
            [string](Get-RequiredStateValue $state "config"))

        return [pscustomobject]@{
            Kind = "Parsed"
            Reason = ""
            State = [pscustomobject]@{
                ProcessId = $processId
                ProcessStartFileTimeUtc = $startFileTime
                LegacyStartTime = $legacyState
                Executable = $executable
                Config = $configPath
            }
        }
    } catch {
        return [pscustomobject]@{
            Kind = "Corrupt"
            Reason = $_.Exception.Message
            State = $null
        }
    }
}

function Get-ControlledServerStatus {
    $readResult = Read-ControlState
    if ($readResult.Kind -ne "Parsed") {
        return $readResult
    }

    $state = $readResult.State
    try {
        $process = [Diagnostics.Process]::GetProcessById($state.ProcessId)
    } catch [ArgumentException] {
        return [pscustomobject]@{
            Kind = "Stale"
            Reason = "Recorded process no longer exists"
            State = $state
            Process = $null
        }
    } catch {
        return [pscustomobject]@{
            Kind = "Indeterminate"
            Reason = "Could not inspect recorded process: $($_.Exception.Message)"
            State = $state
            Process = $null
        }
    }

    try {
        if ($process.HasExited) {
            $process.Dispose()
            return [pscustomobject]@{
                Kind = "Stale"
                Reason = "Recorded process has exited"
                State = $state
                Process = $null
            }
        }
        $actualStartFileTime = $process.StartTime.ToUniversalTime().ToFileTimeUtc()
        $actualExecutable = Get-ProcessExecutablePath $process
    } catch [InvalidOperationException] {
        $process.Dispose()
        return [pscustomobject]@{
            Kind = "Stale"
            Reason = "Recorded process exited during identity verification"
            State = $state
            Process = $null
        }
    } catch {
        $process.Dispose()
        return [pscustomobject]@{
            Kind = "Indeterminate"
            Reason = "Could not verify recorded process identity: $($_.Exception.Message)"
            State = $state
            Process = $null
        }
    }

    $startTimeMatches = if ($state.LegacyStartTime) {
        $actualStart = [DateTime]::FromFileTimeUtc($actualStartFileTime)
        $recordedStart = [DateTime]::FromFileTimeUtc($state.ProcessStartFileTimeUtc)
        [Math]::Abs(($actualStart - $recordedStart).TotalSeconds) -le 5.0
    } else {
        $actualStartFileTime -eq $state.ProcessStartFileTimeUtc
    }

    if (-not $startTimeMatches -or
        -not $actualExecutable.Equals(
            $state.Executable, [StringComparison]::OrdinalIgnoreCase)) {
        $process.Dispose()
        return [pscustomobject]@{
            Kind = "IdentityMismatch"
            Reason = "PID was reused or executable identity changed"
            State = $state
            Process = $null
        }
    }

    return [pscustomobject]@{
        Kind = "Running"
        Reason = ""
        State = $state
        Process = $process
    }
}

function Move-StateAside([string]$Reason) {
    if (-not (Test-Path -LiteralPath $statePath)) {
        return
    }
    Assert-RepoFile $statePath "Server-control state" | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $destination = Join-Path $stateRoot (
        "state.invalid_{0}_{1}.json" -f $stamp, [Guid]::NewGuid().ToString("N"))
    Move-Item -LiteralPath $statePath -Destination $destination
    Write-Warning "Moved unusable server-control state aside ($Reason): $destination"
}

function Resolve-StateForStart {
    $status = Get-ControlledServerStatus
    if ($status.Kind -eq "Running") {
        return $status
    }

    $unmanaged = @(Get-RepoServerProcesses)
    switch ($status.Kind) {
        "Missing" { }
        "Stale" {
            Remove-ControlFile $statePath
        }
        "Corrupt" {
            if ($unmanaged.Count -gt 0) {
                throw "State is corrupt and a candidate server is running; refusing to guess ownership: $($status.Reason)"
            }
            Move-StateAside $status.Reason
        }
        "IdentityMismatch" {
            if ($unmanaged.Count -gt 0) {
                throw "Recorded PID identity changed and a candidate server is running; refusing to guess ownership"
            }
            Move-StateAside $status.Reason
        }
        "Indeterminate" {
            throw "Cannot safely verify recorded server identity: $($status.Reason)"
        }
        default {
            throw "Unexpected server-control state: $($status.Kind)"
        }
    }

    if ($unmanaged.Count -gt 0) {
        throw "Unmanaged rs2v_server process detected; refusing to start a duplicate: $($unmanaged.ProcessId -join ', ')"
    }
    return [pscustomobject]@{ Kind = "Ready"; State = $null; Process = $null }
}

function Rotate-ServerLogs {
    Assert-ControlDirectory $logRoot | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    while ((Test-Path -LiteralPath (Join-Path $logRoot "server_${stamp}.stdout.log")) -or
           (Test-Path -LiteralPath (Join-Path $logRoot "server_${stamp}.stderr.log"))) {
        Start-Sleep -Milliseconds 2
        $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    }

    $activeLogs = @(
        @{ Source = $stdoutPath; Name = "server_${stamp}.stdout.log" },
        @{ Source = $stderrPath; Name = "server_${stamp}.stderr.log" }
    )
    $hasSessionOutput = $false
    foreach ($entry in $activeLogs) {
        if (Test-Path -LiteralPath $entry.Source) {
            Assert-RepoFile $entry.Source "Server log" | Out-Null
            if ((Get-Item -LiteralPath $entry.Source).Length -gt 0) {
                $hasSessionOutput = $true
            }
        }
    }

    foreach ($entry in $activeLogs) {
        if ($hasSessionOutput) {
            $destination = Join-Path $logRoot $entry.Name
            if (Test-Path -LiteralPath $entry.Source -PathType Leaf) {
                Move-Item -LiteralPath $entry.Source -Destination $destination
            } else {
                # Keep stdout/stderr archives paired even when the process never
                # created one stream or left it empty.
                [IO.File]::WriteAllText($destination, "", $utf8NoBom)
            }
        } else {
            Remove-ControlFile $entry.Source
        }
    }

    $archives = @(Get-ChildItem -LiteralPath $logRoot -File -ErrorAction Stop |
        Where-Object { $_.Name -match '^server_(\d{8}_\d{6}_\d{3})\.(stdout|stderr)\.log$' } |
        Group-Object { [regex]::Match(
            $_.Name, '^server_(\d{8}_\d{6}_\d{3})\.').Groups[1].Value } |
        Sort-Object Name -Descending)
    foreach ($set in @($archives | Select-Object -Skip $maxArchivedLogSets)) {
        foreach ($file in $set.Group) {
            Remove-Item -LiteralPath $file.FullName -Force
        }
    }
}

function Write-ControlState([Diagnostics.Process]$Process, [string]$Executable,
                            [string]$ConfigPath) {
    $state = [ordered]@{
        schema = $stateSchema
        pid = $Process.Id
        process_start_filetime_utc = [string](
            $Process.StartTime.ToUniversalTime().ToFileTimeUtc())
        executable = $Executable
        config = $ConfigPath
        recorded_utc = [DateTime]::UtcNow.ToString("o")
        stdout = $stdoutPath
        stderr = $stderrPath
    }
    Write-AtomicUtf8 $statePath ($state | ConvertTo-Json -Depth 3)
}

function Show-ServerStatus {
    $status = Get-ControlledServerStatus
    $disabled = Test-StopSentinel
    $ensureState = if ($disabled) { "disabled" } else { "enabled" }
    $managedPid = if ($status.Kind -eq "Running") {
        $status.State.ProcessId
    } else { 0 }

    switch ($status.Kind) {
        "Running" {
            Write-Output ("server=running ownership=managed pid={0} executable={1} ensure={2}" -f
                $status.State.ProcessId, $status.State.Executable, $ensureState)
            $status.Process.Dispose()
        }
        "Missing" {
            Write-Output ("server=stopped state=missing ensure={0}" -f $ensureState)
        }
        "Stale" {
            Write-Output ("server=stopped state=stale ensure={0} detail={1}" -f
                $ensureState, $status.Reason)
        }
        default {
            Write-Output ("server=unknown state={0} ensure={1} detail={2}" -f
                $status.Kind.ToLowerInvariant(), $ensureState, $status.Reason)
        }
    }

    foreach ($process in @(Get-RepoServerProcesses $managedPid)) {
        $path = if ($process.IdentityKnown) { $process.ExecutablePath } else { "unknown" }
        Write-Output ("server=running ownership=unmanaged pid={0} executable={1} ensure={2}" -f
            $process.ProcessId, $path, $ensureState)
    }
}

function Show-ServerLogs {
    foreach ($entry in @(
        @{ Label = "stdout"; Path = $stdoutPath },
        @{ Label = "stderr"; Path = $stderrPath }
    )) {
        Write-Output ("--- server {0}: {1} ---" -f $entry.Label, $entry.Path)
        if (Test-Path -LiteralPath $entry.Path -PathType Leaf) {
            Assert-RepoFile $entry.Path "Server log" | Out-Null
            Get-Content -LiteralPath $entry.Path -Tail $TailLines
        } else {
            Write-Output "(log not present)"
        }
    }
}

function Get-LogLength([string]$Path) {
    try {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            return [long]0
        }
        Assert-RepoFile $Path "Server log" | Out-Null
        return [long](Get-Item -LiteralPath $Path).Length
    } catch [IO.IOException] {
        return [long]0
    }
}

function Read-LogDelta([string]$Path, [long]$Offset) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{ Offset = [long]0; Text = "" }
    }

    try {
        Assert-RepoFile $Path "Server log" | Out-Null
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open(
            $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
        try {
            if ($stream.Length -lt $Offset) {
                # Start/Restart rotated or truncated the active log.
                $Offset = 0
            }
            $stream.Seek($Offset, [IO.SeekOrigin]::Begin) | Out-Null
            $reader = [IO.StreamReader]::new(
                $stream, [Text.UTF8Encoding]::new($false, $true), $true)
            try {
                $text = $reader.ReadToEnd()
                $newOffset = $stream.Position
            } finally {
                $reader.Dispose()
            }
            return [pscustomobject]@{ Offset = [long]$newOffset; Text = $text }
        } finally {
            $stream.Dispose()
        }
    } catch [IO.IOException] {
        # A concurrent restart may briefly move/recreate the file. Resetting to zero
        # makes the next poll read the replacement instead of skipping its prefix.
        return [pscustomobject]@{ Offset = [long]0; Text = "" }
    }
}

function Watch-Server {
    Write-Output ("watching server every {0}s; press Ctrl+C to stop watching" -f
        $WatchIntervalSeconds)
    $lastStatus = (@(Show-ServerStatus) -join [Environment]::NewLine)
    Write-Output $lastStatus
    Show-ServerLogs
    $stdoutOffset = Get-LogLength $stdoutPath
    $stderrOffset = Get-LogLength $stderrPath

    while ($true) {
        Start-Sleep -Seconds $WatchIntervalSeconds
        $statusText = (@(Show-ServerStatus) -join [Environment]::NewLine)
        if ($statusText -ne $lastStatus) {
            Write-Output $statusText
            $lastStatus = $statusText
            Show-ServerLogs
            $stdoutOffset = Get-LogLength $stdoutPath
            $stderrOffset = Get-LogLength $stderrPath
            continue
        }

        $stdoutDelta = Read-LogDelta $stdoutPath $stdoutOffset
        $stdoutOffset = $stdoutDelta.Offset
        if (-not [string]::IsNullOrEmpty($stdoutDelta.Text)) {
            [Console]::Out.Write($stdoutDelta.Text)
        }

        $stderrDelta = Read-LogDelta $stderrPath $stderrOffset
        $stderrOffset = $stderrDelta.Offset
        if (-not [string]::IsNullOrEmpty($stderrDelta.Text)) {
            [Console]::Error.Write($stderrDelta.Text)
        }
    }
}

function Test-ServerControl {
    $resolvedBinary = Resolve-ServerBinary
    $resolvedConfig = Assert-RepoFile (Resolve-RepoPath $Config) "Server config"
    $argumentProbePath = Join-Path $repoRoot "path with spaces\server.ini"
    $argumentProbe = Format-ServerArgumentLine $argumentProbePath "VNTE-Resort" 17777 17957
    if ($argumentProbe -ne ('--config "{0}" --map "VNTE-Resort" --port 17777 --eac-port 17957' -f
            $argumentProbePath)) {
        throw "Native config/map/game-port/EAC-port argument quoting self-test failed"
    }
    if ((Resolve-ReplicationBootstrapEnvironmentValue "installed") -ne
            "installed" -or
        $null -ne (Resolve-ReplicationBootstrapEnvironmentValue "canonical")) {
        throw "Replication-bootstrap environment mapping self-test failed"
    }

    $selfProcess = [Diagnostics.Process]::GetProcessById($PID)
    try {
        $managedPath = Get-ProcessExecutablePath $selfProcess 250
        $limitedPath =
            [RS2VServerControl.NativeProcessQuery]::GetImagePath($selfProcess.Id)
        if ([string]::IsNullOrWhiteSpace($limitedPath) -or
            -not $managedPath.Equals(
                [IO.Path]::GetFullPath($limitedPath),
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Limited-information process-path self-test failed"
        }
    } finally {
        $selfProcess.Dispose()
    }

    foreach ($path in @($statePath, $disabledPath, $stdoutPath, $stderrPath)) {
        if (Test-Path -LiteralPath $path) {
            Assert-RepoFile $path "Server-control artifact" | Out-Null
        }
    }

    $status = Get-ControlledServerStatus
    if ($status.Kind -eq "Running") {
        $status.Process.Dispose()
    } elseif ($status.Kind -in @("Corrupt", "IdentityMismatch", "Indeterminate")) {
        throw "Server-control state self-test failed ($($status.Kind)): $($status.Reason)"
    }

    Write-Output ("self-test=ok binary={0} config={1} state={2} quoting=ok process_path=ok bootstrap_environment=ok variant={3}" -f
        $resolvedBinary, $resolvedConfig, $status.Kind.ToLowerInvariant(),
        $ReplicationBootstrapVariant)
}

function Start-OwnedServer {
    $existing = Resolve-StateForStart
    if ($existing.Kind -eq "Running") {
        try {
            # A manual Start is also an explicit request to re-enable Ensure, even
            # when the managed process is already alive after a previously failed Stop.
            Remove-ControlFile $disabledPath
            Write-Output ("server already running pid={0}" -f $existing.State.ProcessId)
        } finally {
            $existing.Process.Dispose()
        }
        return
    }

    $executable = Resolve-ServerBinary
    $configPath = Assert-RepoFile (Resolve-RepoPath $Config) "Server config"

    $process = $null
    $stateWritten = $false
    try {
        # A manual Start/Restart explicitly opts back into automatic Ensure. Keep
        # sentinel removal inside the startup failure boundary so any later
        # launch/identity/state error restores suppression before returning.
        Remove-ControlFile $disabledPath
        Rotate-ServerLogs

        $arguments = Format-ServerArgumentLine $configPath $Map $Port $EacPort
        $bootstrapEnvironmentValue =
            Resolve-ReplicationBootstrapEnvironmentValue $ReplicationBootstrapVariant
        $previousBootstrapEnvironmentValue =
            [Environment]::GetEnvironmentVariable(
                $replicationBootstrapEnvironmentName,
                [System.EnvironmentVariableTarget]::Process)
        try {
            [Environment]::SetEnvironmentVariable(
                $replicationBootstrapEnvironmentName,
                $bootstrapEnvironmentValue,
                [System.EnvironmentVariableTarget]::Process)
            $process = Start-Process -FilePath $executable `
                -ArgumentList $arguments `
                -WorkingDirectory $repoRoot `
                -WindowStyle Hidden `
                -RedirectStandardOutput $stdoutPath `
                -RedirectStandardError $stderrPath `
                -PassThru
        } finally {
            # Windows PowerShell 5.1 has no per-child Start-Process environment
            # parameter. Limit the inherited override to process creation and
            # restore the controller exactly even when Start-Process throws.
            [Environment]::SetEnvironmentVariable(
                $replicationBootstrapEnvironmentName,
                $previousBootstrapEnvironmentValue,
                [System.EnvironmentVariableTarget]::Process)
        }

        $process.Refresh()
        $pathRetryMilliseconds = [Math]::Min(2000, $WaitSeconds * 1000)
        $actualExecutable = Get-ProcessExecutablePath $process $pathRetryMilliseconds
        if (-not $actualExecutable.Equals(
                $executable, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Started process executable does not match requested server binary"
        }
        # Record process identity before the startup grace period. If this controller
        # is interrupted during that wait, a later Stop can still prove ownership and
        # hard-kill the exact process instead of leaving an untracked server behind.
        Write-ControlState $process $executable $configPath
        $stateWritten = $true

        if ($process.WaitForExit(750)) {
            $stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
                (Get-Content -LiteralPath $stderrPath -Tail 40) -join [Environment]::NewLine
            } else { "" }
            throw "Server exited during startup with code $($process.ExitCode). $stderr"
        }

        Write-Output ("server started pid={0} executable={1}" -f $process.Id, $executable)
    } catch {
        $startError = $_
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    $process.Kill()
                    $process.WaitForExit([Math]::Max(1000, $WaitSeconds * 1000)) | Out-Null
                }
            } catch {
                Write-Warning "Could not clean up failed server start: $($_.Exception.Message)"
            }
        }
        if ($stateWritten) {
            try {
                Remove-ControlFile $statePath
            } catch {
                Write-Warning "Could not remove failed-start state: $($_.Exception.Message)"
            }
        }
        try {
            Write-StopSentinel
        } catch {
            throw (
                "Server start failed: {0}. Automatic Ensure could not be disabled: {1}" -f
                $startError.Exception.Message, $_.Exception.Message)
        }
        throw $startError
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
    }
}

function Stop-ControlledProcess([object]$Status) {
    $process = $Status.Process
    try {
        # Get-ControlledServerStatus opened this exact process and verified both its
        # creation FILETIME and executable before this handle is used to terminate it.
        if (-not $process.HasExited) {
            $process.Kill()
        }
        if (-not $process.WaitForExit($WaitSeconds * 1000)) {
            throw "Timed out stopping managed server pid $($Status.State.ProcessId)"
        }
    } finally {
        $process.Dispose()
    }
}

function Stop-OwnedServer([bool]$DisableEnsure) {
    if ($DisableEnsure) {
        # Write this before inspecting process state. Even an ownership error must not
        # let the scheduled Ensure race in and restart another instance.
        Write-StopSentinel
    }

    $status = Get-ControlledServerStatus
    switch ($status.Kind) {
        "Running" {
            Stop-ControlledProcess $status
            Remove-ControlFile $statePath
        }
        "Missing" { }
        "Stale" {
            Remove-ControlFile $statePath
        }
        "Corrupt" {
            $unmanaged = @(Get-RepoServerProcesses)
            if ($unmanaged.Count -gt 0) {
                throw "State is corrupt and a candidate server is running; explicit-stop sentinel is set, but ownership cannot be proven"
            }
            Move-StateAside $status.Reason
        }
        "IdentityMismatch" {
            $unmanaged = @(Get-RepoServerProcesses)
            if ($unmanaged.Count -gt 0) {
                throw "Recorded PID identity changed; explicit-stop sentinel is set, but refusing to terminate an unproven process"
            }
            Move-StateAside $status.Reason
        }
        "Indeterminate" {
            throw "Explicit-stop sentinel is set, but server identity cannot be verified: $($status.Reason)"
        }
        default {
            throw "Unexpected server-control state: $($status.Kind)"
        }
    }

    $remaining = @(Get-RepoServerProcesses)
    if ($remaining.Count -gt 0) {
        $safetyState = if (Test-StopSentinel) {
            "Explicit-stop sentinel is set"
        } else {
            "Automatic Ensure remains enabled"
        }
        throw "$safetyState, but unmanaged server process(es) were not terminated: $($remaining.ProcessId -join ', ')"
    }
    $ensureState = if (Test-StopSentinel) { "disabled" } else { "enabled" }
    Write-Output ("server stopped ensure={0}" -f $ensureState)
}

switch ($Action) {
    "Status" { Show-ServerStatus; return }
    "Logs" { Show-ServerLogs; return }
    "Watch" { Watch-Server; return }
    "SelfTest" { Test-ServerControl; return }
}

# Only lifecycle mutations need directories and the cross-session mutex. Keeping
# Status/Logs/Watch outside this lock means an operator can stop or restart a server
# while another shell is continuously watching it.
Initialize-ControlDirectories

$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $rootBytes = [Text.Encoding]::UTF8.GetBytes($repoRoot.ToUpperInvariant())
    $mutexHash = ([BitConverter]::ToString($sha256.ComputeHash($rootBytes))).Replace("-", "").Substring(0, 24)
} finally {
    $sha256.Dispose()
}

# Global\ is intentional: the Scheduled Task and the interactive shell may be in
# different Windows sessions. The repository hash prevents unrelated clones from
# blocking each other.
Write-Verbose "server-control repository=$repoRoot mutex=Global\RS2VServerControl_$mutexHash"
$mutex = [Threading.Mutex]::new($false, "Global\RS2VServerControl_$mutexHash")
$lockTaken = $false
try {
    try {
        $lockTaken = $mutex.WaitOne([TimeSpan]::FromSeconds($WaitSeconds))
    } catch [Threading.AbandonedMutexException] {
        # WaitOne grants ownership when it reports an abandoned mutex.
        $lockTaken = $true
        Write-Warning "Recovered abandoned server-control mutex"
    }
    if (-not $lockTaken) {
        throw "Timed out waiting for the server-control mutex"
    }

    switch ($Action) {
        "Start" { Start-OwnedServer; Show-ServerStatus }
        "Stop" { Stop-OwnedServer $true; Show-ServerStatus }
        "Restart" {
            Stop-OwnedServer $false
            Start-OwnedServer
            Show-ServerStatus
        }
        "Ensure" {
            if (Test-StopSentinel) {
                Write-Output "server ensure suppressed by explicit-stop sentinel"
                Show-ServerStatus
            } else {
                Start-OwnedServer
                Show-ServerStatus
            }
        }
        default { throw "Unsupported mutating server-control action: $Action" }
    }
} finally {
    if ($lockTaken) {
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
}
