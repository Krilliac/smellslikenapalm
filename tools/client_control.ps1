[CmdletBinding()]
param(
    [ValidateSet("Status", "Start", "Stop", "Restart", "Connect", "Recover", "SelfTest")]
    [string]$Action = "Status",

    [string]$SteamPath = "",

    [ValidateLength(1, 255)]
    [string]$ServerAddress = "127.0.0.1",

    [ValidateRange(1, 65535)]
    [int]$ServerPort = 7777,

    [switch]$AllowRemoteAddress,

    [ValidateRange(5, 300)]
    [int]$LaunchTimeoutSeconds = 120,

    [ValidateRange(0, 60)]
    [int]$LateLaunchGraceSeconds = 30,

    [ValidateRange(1, 60)]
    [int]$StopTimeoutSeconds = 15
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 can otherwise report process exit code 0 for an
# unhandled terminating script error. Automation must distinguish a safe no-op
# from a lifecycle failure.
trap {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    exit 1
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$stateRoot = Join-Path $repoRoot ".client-control"
$statePath = Join-Path $stateRoot "state.json"
$utf8NoBom = [Text.UTF8Encoding]::new($false, $true)
$stateSchema = 1
$steamAppId = 418460
$clientProcessName = "VNGame.exe"
$clientRecoveryPathSuffix = "\steamapps\common\Rising Storm 2\Binaries\Win64\VNGame.exe"
$launchTokenPrefix = "-RS2VClientControl="

function Initialize-NativeProcessQuery {
    if ($null -ne ("RS2VClientControl.NativeProcessQuery" -as [type])) {
        return
    }

    # EAC-protected VNGame exposes StartTime but may blank Win32_Process.Path,
    # Win32_Process.CommandLine, and Diagnostics.Process.Path. Windows permits
    # these two identity reads through PROCESS_QUERY_LIMITED_INFORMATION without
    # elevation or process mutation.
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace RS2VClientControl
{
    public static class NativeProcessQuery
    {
        private const uint ProcessQueryLimitedInformation = 0x1000;
        private const int ProcessCommandLineInformation = 60;
        private const int MaximumQueryBytes = 1024 * 1024;

        [StructLayout(LayoutKind.Sequential)]
        private struct UnicodeString
        {
            public ushort Length;
            public ushort MaximumLength;
            public IntPtr Buffer;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(
            uint desiredAccess, bool inheritHandle, int processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool QueryFullProcessImageName(
            IntPtr process, int flags, StringBuilder imagePath, ref int size);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("ntdll.dll")]
        private static extern int NtQueryInformationProcess(
            IntPtr process, int informationClass, IntPtr information,
            int informationLength, out int returnLength);

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

        public static string GetCommandLine(int processId)
        {
            IntPtr process = OpenProcess(
                ProcessQueryLimitedInformation, false, processId);
            if (process == IntPtr.Zero)
                return null;

            IntPtr information = IntPtr.Zero;
            try
            {
                int requiredLength;
                NtQueryInformationProcess(
                    process, ProcessCommandLineInformation, IntPtr.Zero, 0,
                    out requiredLength);
                if (requiredLength <= Marshal.SizeOf(typeof(UnicodeString)) ||
                    requiredLength > MaximumQueryBytes)
                    return null;

                information = Marshal.AllocHGlobal(requiredLength);
                int returnedLength;
                int status = NtQueryInformationProcess(
                    process, ProcessCommandLineInformation, information,
                    requiredLength, out returnedLength);
                if (status != 0 || returnedLength <= 0 ||
                    returnedLength > requiredLength)
                    return null;

                var value = (UnicodeString)Marshal.PtrToStructure(
                    information, typeof(UnicodeString));
                if (value.Buffer == IntPtr.Zero || value.Length == 0 ||
                    (value.Length & 1) != 0 || value.Length > value.MaximumLength)
                    return null;

                long allocationStart = information.ToInt64();
                long allocationEnd = allocationStart + requiredLength;
                long textStart = value.Buffer.ToInt64();
                long textEnd = textStart + value.Length;
                if (allocationEnd < allocationStart || textEnd < textStart ||
                    textStart < allocationStart || textEnd > allocationEnd)
                    return null;

                return Marshal.PtrToStringUni(value.Buffer, value.Length / 2);
            }
            finally
            {
                if (information != IntPtr.Zero)
                    Marshal.FreeHGlobal(information);
                CloseHandle(process);
            }
        }
    }
}
'@
}

function Get-LimitedProcessImagePath([int]$ProcessId) {
    Initialize-NativeProcessQuery
    return [RS2VClientControl.NativeProcessQuery]::GetImagePath($ProcessId)
}

function Get-LimitedProcessCommandLine([int]$ProcessId) {
    Initialize-NativeProcessQuery
    return [RS2VClientControl.NativeProcessQuery]::GetCommandLine($ProcessId)
}

function Test-PathInsideRepo([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $repoRoot.TrimEnd('\') + '\'
    return $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Resolve-RepoPath([string]$Path) {
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

function Assert-NoRepoReparseEscape([string]$Path) {
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
            throw "Refusing reparse-point client-control path: $currentPath"
        }
    }
    return $fullPath
}

function Assert-ControlDirectory([string]$Path) {
    $fullPath = Assert-NoRepoReparseEscape $Path
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if (-not $item.PSIsContainer) {
            throw "Client-control path is not a directory: $fullPath"
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing reparse-point client-control directory: $fullPath"
        }
    } else {
        New-Item -ItemType Directory -Path $fullPath | Out-Null
    }
    return $fullPath
}

function Write-AtomicUtf8([string]$Path, [string]$Text) {
    $fullPath = Assert-NoRepoReparseEscape $Path
    $parent = Split-Path -Parent $fullPath
    Assert-ControlDirectory $parent | Out-Null

    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Refusing unsafe client-control file: $fullPath"
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
    $fullPath = Assert-NoRepoReparseEscape $Path
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Refusing unsafe client-control file: $fullPath"
        }
        Remove-Item -LiteralPath $fullPath -Force
    }
}

function Test-InvalidArgumentText([string]$Value) {
    return $Value.IndexOf('"') -ge 0 -or $Value.IndexOf([char]0) -ge 0 -or
        $Value -match '[\x01-\x1F\x7F]'
}

function Format-ConnectEndpoint([string]$Address, [int]$Port,
                                [bool]$RemoteAllowed) {
    if ([string]::IsNullOrWhiteSpace($Address) -or
        (Test-InvalidArgumentText $Address) -or $Address.IndexOf(' ') -ge 0) {
        throw "Server address contains an invalid command-line character"
    }
    if ($Port -lt 1 -or $Port -gt 65535) {
        throw "Server port must be in the range 1 through 65535"
    }

    if ($Address.Equals("localhost", [StringComparison]::OrdinalIgnoreCase)) {
        # Do not rely on a mutable hosts-file mapping for the default safety
        # boundary; normalize the well-known name to numeric loopback.
        return "127.0.0.1:$Port"
    }

    $ipAddress = $null
    if (-not [Net.IPAddress]::TryParse($Address, [ref]$ipAddress)) {
        throw "Server address must be localhost or a numeric IP address"
    }
    if (-not $RemoteAllowed -and -not [Net.IPAddress]::IsLoopback($ipAddress)) {
        throw "Remote server address requires -AllowRemoteAddress"
    }

    if ($ipAddress.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetworkV6) {
        return "[$($ipAddress.ToString())]:$Port"
    }
    return "$($ipAddress.ToString()):$Port"
}

function Format-SteamLaunchArguments([Guid]$LaunchToken, [string]$Endpoint = "") {
    $arguments = "-applaunch $steamAppId"
    if (-not [string]::IsNullOrWhiteSpace($Endpoint)) {
        if (Test-InvalidArgumentText $Endpoint -or $Endpoint.IndexOf(' ') -ge 0) {
            throw "Connect endpoint contains an invalid command-line character"
        }
        $arguments += " $Endpoint"
    }
    # Keep the UE3 URL (when present) as the first game argument. Unknown dash
    # switches are ignored by the retail client, but placing the ownership token
    # after the URL avoids relying on that parser detail for direct-connect.
    $arguments += " $launchTokenPrefix$($LaunchToken.ToString('D'))"
    return $arguments
}

function Assert-SteamExecutable([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        return $null
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing reparse-point Steam executable: $fullPath"
    }
    if (-not [IO.Path]::GetFileName($fullPath).Equals(
            "steam.exe", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Steam executable must be named steam.exe: $fullPath"
    }
    return $fullPath
}

function Resolve-SteamExecutable {
    if (-not [string]::IsNullOrWhiteSpace($SteamPath)) {
        $explicitPath = Assert-SteamExecutable $SteamPath
        if ($null -eq $explicitPath) {
            throw "Steam executable not found: $([IO.Path]::GetFullPath($SteamPath))"
        }
        return $explicitPath
    }

    $candidates = New-Object Collections.Generic.List[string]
    foreach ($process in @(Get-Process -Name "steam" -ErrorAction SilentlyContinue)) {
        try {
            if (-not [string]::IsNullOrWhiteSpace([string]$process.Path)) {
                $candidates.Add([string]$process.Path)
            }
        } catch { } finally {
            $process.Dispose()
        }
    }

    foreach ($registryPath in @(
            "HKCU:\Software\Valve\Steam",
            "HKLM:\Software\WOW6432Node\Valve\Steam",
            "HKLM:\Software\Valve\Steam")) {
        $entry = Get-ItemProperty -LiteralPath $registryPath -ErrorAction SilentlyContinue
        if ($null -ne $entry) {
            foreach ($propertyName in @("SteamExe", "InstallPath")) {
                $property = $entry.PSObject.Properties[$propertyName]
                if ($null -ne $property -and
                    -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
                    $candidate = [string]$property.Value
                    if ($propertyName -eq "InstallPath") {
                        $candidate = Join-Path $candidate "steam.exe"
                    }
                    $candidates.Add($candidate.Replace('/', '\'))
                }
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $candidates.Add((Join-Path ${env:ProgramFiles(x86)} "Steam\steam.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $candidates.Add((Join-Path $env:ProgramFiles "Steam\steam.exe"))
    }

    foreach ($candidate in @($candidates | Select-Object -Unique)) {
        try {
            $resolved = Assert-SteamExecutable $candidate
            if ($null -ne $resolved) {
                return $resolved
            }
        } catch {
            Write-Warning $_.Exception.Message
        }
    }
    throw "Steam executable not found. Start Steam or pass -SteamPath."
}

function Get-ClientProcessRows {
    $rows = @(Get-CimInstance Win32_Process -Filter "Name='$clientProcessName'" -ErrorAction Stop)
    foreach ($row in $rows) {
        $processId = [int]$row.ProcessId
        $path = [string]$row.ExecutablePath
        $commandLine = [string]$row.CommandLine
        $startFileTime = [long]0
        $identityKnown = $false
        $process = $null
        try {
            $process = [Diagnostics.Process]::GetProcessById($processId)
            if ([string]::IsNullOrWhiteSpace($path)) {
                try { $path = [string]$process.Path } catch { $path = "" }
            }
            if ([string]::IsNullOrWhiteSpace($path)) {
                $path = [string](Get-LimitedProcessImagePath $processId)
            }
            if (-not [string]::IsNullOrWhiteSpace($path)) {
                $path = [IO.Path]::GetFullPath($path)
            }
            $startFileTime = $process.StartTime.ToUniversalTime().ToFileTimeUtc()
            $identityKnown = -not [string]::IsNullOrWhiteSpace($path)
        } catch {
            $path = ""
            $startFileTime = 0
            $identityKnown = $false
        } finally {
            if ($null -ne $process) {
                $process.Dispose()
            }
        }

        if ([string]::IsNullOrWhiteSpace($commandLine)) {
            $commandLine = [string](Get-LimitedProcessCommandLine $processId)
        }

        [pscustomobject]@{
            ProcessId = $processId
            ExecutablePath = $path
            ProcessStartFileTimeUtc = $startFileTime
            CommandLine = $commandLine
            CommandLineKnown = -not [string]::IsNullOrWhiteSpace($commandLine)
            IdentityKnown = $identityKnown
        }
    }
}

function Test-CommandLineLaunchToken([string]$CommandLine, [Guid]$LaunchToken) {
    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $false
    }
    $tokenArgument = "$launchTokenPrefix$($LaunchToken.ToString('D'))"
    $pattern = '(?i)(?:^|\s)"?' + [regex]::Escape($tokenArgument) +
        '"?(?:\s|$)'
    return [regex]::IsMatch($CommandLine, $pattern)
}

function Get-CommandLineLaunchToken([string]$CommandLine) {
    if ([string]::IsNullOrWhiteSpace($CommandLine)) {
        return $null
    }

    $guidPattern =
        '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}'
    # Zero-width argument boundaries allow duplicate-token detection even when
    # adjacent arguments share a single separating space.
    $pattern = '(?i)(?<!\S)"?' + [regex]::Escape($launchTokenPrefix) +
        '(?<token>' + $guidPattern + ')"?(?!\S)'
    $matches = [regex]::Matches($CommandLine, $pattern)
    if ($matches.Count -eq 0) {
        return $null
    }
    if ($matches.Count -ne 1) {
        throw "Client command line contains multiple controller launch tokens"
    }

    $launchToken = [Guid]::Empty
    if (-not [Guid]::TryParseExact(
            $matches[0].Groups['token'].Value, "D", [ref]$launchToken) -or
        $launchToken -eq [Guid]::Empty) {
        return $null
    }
    return $launchToken
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
        $fullStatePath = Assert-NoRepoReparseEscape $statePath
        $item = Get-Item -LiteralPath $fullStatePath -Force
        if (-not $item.PSIsContainer -and
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) {
            $bytes = [IO.File]::ReadAllBytes($fullStatePath)
        } else {
            throw "State path is not a regular file"
        }
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
            $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            throw "State must be UTF-8 without a byte-order mark"
        }
        $state = $utf8NoBom.GetString($bytes) | ConvertFrom-Json -ErrorAction Stop

        $schema = 0
        if (-not [int]::TryParse(
                [string](Get-RequiredStateValue $state "schema"), [ref]$schema) -or
            $schema -ne $stateSchema) {
            throw "Unsupported client-control state schema"
        }

        $processId = 0
        if (-not [int]::TryParse(
                [string](Get-RequiredStateValue $state "pid"), [ref]$processId) -or
            $processId -le 0) {
            throw "State contains an invalid process id"
        }

        $startFileTime = [long]0
        if (-not [long]::TryParse(
                [string](Get-RequiredStateValue $state "process_start_filetime_utc"),
                [ref]$startFileTime) -or $startFileTime -le 0) {
            throw "State contains an invalid process creation time"
        }

        $executable = [IO.Path]::GetFullPath(
            [string](Get-RequiredStateValue $state "executable"))
        if (-not [IO.Path]::GetFileName($executable).Equals(
                $clientProcessName, [StringComparison]::OrdinalIgnoreCase)) {
            throw "State executable is not $clientProcessName"
        }

        $launchToken = [Guid]::Empty
        if (-not [Guid]::TryParseExact(
                [string](Get-RequiredStateValue $state "launch_token"), "D",
                [ref]$launchToken) -or $launchToken -eq [Guid]::Empty) {
            throw "State contains an invalid launch token"
        }

        return [pscustomobject]@{
            Kind = "Parsed"
            Reason = ""
            State = [pscustomobject]@{
                ProcessId = $processId
                ProcessStartFileTimeUtc = $startFileTime
                Executable = $executable
                LaunchToken = $launchToken
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

function Get-ControlledClientStatus {
    $readResult = Read-ControlState
    if ($readResult.Kind -ne "Parsed") {
        return $readResult
    }

    $state = $readResult.State
    $process = $null
    try {
        $process = [Diagnostics.Process]::GetProcessById($state.ProcessId)
        if ($process.HasExited) {
            throw [ArgumentException]::new("Recorded process has exited")
        }
        try { $actualPath = [string]$process.Path } catch { $actualPath = "" }
        if ([string]::IsNullOrWhiteSpace($actualPath)) {
            $actualPath = [string](Get-LimitedProcessImagePath $state.ProcessId)
        }
        if ([string]::IsNullOrWhiteSpace($actualPath)) {
            throw "Could not read the recorded process executable path"
        }
        $actualPath = [IO.Path]::GetFullPath($actualPath)
        $actualStartFileTime = $process.StartTime.ToUniversalTime().ToFileTimeUtc()
    } catch [ArgumentException] {
        if ($null -ne $process) { $process.Dispose() }
        return [pscustomobject]@{
            Kind = "Stale"; Reason = $_.Exception.Message
            State = $state; Process = $null
        }
    } catch [InvalidOperationException] {
        if ($null -ne $process) { $process.Dispose() }
        return [pscustomobject]@{
            Kind = "Stale"; Reason = $_.Exception.Message
            State = $state; Process = $null
        }
    } catch {
        if ($null -ne $process) { $process.Dispose() }
        return [pscustomobject]@{
            Kind = "Indeterminate"
            Reason = "Could not inspect recorded process: $($_.Exception.Message)"
            State = $state; Process = $null
        }
    }

    if ($actualStartFileTime -ne $state.ProcessStartFileTimeUtc -or
        -not $actualPath.Equals(
            $state.Executable, [StringComparison]::OrdinalIgnoreCase)) {
        $process.Dispose()
        return [pscustomobject]@{
            Kind = "IdentityMismatch"
            Reason = "PID was reused or executable identity changed"
            State = $state; Process = $null
        }
    }

    try {
        $row = Get-CimInstance Win32_Process -Filter (
            "ProcessId=$($state.ProcessId)") -ErrorAction Stop
        $commandLine = if ($null -ne $row) { [string]$row.CommandLine } else { "" }
        if ([string]::IsNullOrWhiteSpace($commandLine)) {
            $commandLine = [string](Get-LimitedProcessCommandLine $state.ProcessId)
        }
        if ([string]::IsNullOrWhiteSpace($commandLine)) {
            $process.Dispose()
            return [pscustomobject]@{
                Kind = "Indeterminate"
                Reason = "Could not read the client command line for launch-token verification"
                State = $state; Process = $null
            }
        }
        if (-not (Test-CommandLineLaunchToken $commandLine $state.LaunchToken)) {
            $process.Dispose()
            return [pscustomobject]@{
                Kind = "IdentityMismatch"
                Reason = "Recorded launch token is absent from the client command line"
                State = $state; Process = $null
            }
        }
    } catch {
        $process.Dispose()
        return [pscustomobject]@{
            Kind = "Indeterminate"
            Reason = "Could not verify the client launch token: $($_.Exception.Message)"
            State = $state; Process = $null
        }
    }

    return [pscustomobject]@{
        Kind = "Running"; Reason = ""; State = $state; Process = $process
    }
}

function Move-StateAside([string]$Reason) {
    if (-not (Test-Path -LiteralPath $statePath)) {
        return
    }
    $fullStatePath = Assert-NoRepoReparseEscape $statePath
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $destination = Join-Path $stateRoot (
        "state.invalid_{0}_{1}.json" -f $stamp, [Guid]::NewGuid().ToString("N"))
    Move-Item -LiteralPath $fullStatePath -Destination $destination
    Write-Warning "Moved unusable client-control state aside ($Reason): $destination"
}

function Write-ControlState([object]$ClientRow) {
    $state = [ordered]@{
        schema = $stateSchema
        pid = [int]$ClientRow.ProcessId
        process_start_filetime_utc = [string]$ClientRow.ProcessStartFileTimeUtc
        executable = [string]$ClientRow.ExecutablePath
        launch_token = $ClientRow.LaunchToken.ToString("D")
        launched_utc = [DateTime]::UtcNow.ToString("o")
        steam_app_id = $steamAppId
    }
    Write-AtomicUtf8 $statePath (($state | ConvertTo-Json -Depth 3) + "`n")
}

function Remove-ControlStateIfMatching([object]$ClientRow, [Guid]$LaunchToken) {
    $published = Read-ControlState
    if ($published.Kind -ne "Parsed") {
        return
    }

    $state = $published.State
    if ($state.ProcessId -eq [int]$ClientRow.ProcessId -and
        $state.ProcessStartFileTimeUtc -eq
            [long]$ClientRow.ProcessStartFileTimeUtc -and
        $state.Executable.Equals(
            [string]$ClientRow.ExecutablePath,
            [StringComparison]::OrdinalIgnoreCase) -and
        $state.LaunchToken -eq $LaunchToken) {
        Remove-ControlFile $statePath
    }
}

function Show-ClientStatus {
    $status = Get-ControlledClientStatus
    $candidates = @(Get-ClientProcessRows)
    try {
        switch ($status.Kind) {
            "Running" {
                Write-Output (
                    'client=running state=managed pid={0} path="{1}"' -f
                    $status.State.ProcessId, $status.State.Executable)
            }
            "Missing" {
                if ($candidates.Count -eq 0) {
                    Write-Output "client=stopped state=missing"
                } else {
                    Write-Output (
                        "client=running state=unmanaged pid={0} identity={1}" -f
                        ($candidates.ProcessId -join ','),
                        $(if (@($candidates | Where-Object { -not $_.IdentityKnown }).Count -eq 0) {
                            "known"
                        } else { "unknown" }))
                }
            }
            "Stale" {
                if ($candidates.Count -eq 0) {
                    Write-Output ('client=stopped state=stale reason="{0}"' -f $status.Reason)
                } else {
                    Write-Output (
                        'client=running state=stale-unmanaged pid={0} reason="{1}"' -f
                        ($candidates.ProcessId -join ','), $status.Reason)
                }
            }
            default {
                Write-Output (
                    'client=unknown state={0} candidates={1} reason="{2}"' -f
                    $status.Kind.ToLowerInvariant(), $candidates.Count, $status.Reason)
            }
        }
    } finally {
        if ($status.Kind -eq "Running" -and $null -ne $status.Process) {
            $status.Process.Dispose()
        }
    }
}

function Resolve-StateForStart {
    $status = Get-ControlledClientStatus
    if ($status.Kind -eq "Running") {
        return $status
    }

    $candidates = @(Get-ClientProcessRows)
    switch ($status.Kind) {
        "Missing" { }
        "Stale" {
            if ($candidates.Count -eq 0) {
                Remove-ControlFile $statePath
            }
        }
        "Corrupt" {
            if ($candidates.Count -gt 0) {
                throw "State is corrupt and a client is running; refusing to guess ownership: $($status.Reason)"
            }
            Move-StateAside $status.Reason
        }
        "IdentityMismatch" {
            if ($candidates.Count -gt 0) {
                throw "Recorded client identity changed and a client is running; refusing to guess ownership"
            }
            Move-StateAside $status.Reason
        }
        "Indeterminate" {
            throw "Cannot safely verify recorded client identity: $($status.Reason)"
        }
        default {
            throw "Unexpected client-control state: $($status.Kind)"
        }
    }

    if ($candidates.Count -gt 0) {
        throw "Unmanaged $clientProcessName process detected; refusing to start a duplicate: $($candidates.ProcessId -join ', ')"
    }
    return [pscustomobject]@{ Kind = "Ready"; State = $null; Process = $null }
}

function Recover-OwnedClient {
    Assert-ControlDirectory $stateRoot | Out-Null
    $status = Get-ControlledClientStatus
    if ($status.Kind -eq "Running") {
        try {
            Write-Output "client already managed pid=$($status.State.ProcessId)"
            return
        } finally {
            $status.Process.Dispose()
        }
    }
    if ($status.Kind -notin @("Missing", "Stale")) {
        throw (
            "Cannot recover over client-control state '$($status.Kind)': " +
            "$($status.Reason)")
    }

    $candidates = @(Get-ClientProcessRows)
    if ($candidates.Count -ne 1) {
        throw (
            "Recovery requires exactly one running $clientProcessName process; " +
            "found $($candidates.Count)")
    }

    $candidate = $candidates[0]
    if (-not $candidate.IdentityKnown -or
        [string]::IsNullOrWhiteSpace($candidate.ExecutablePath) -or
        $candidate.ProcessStartFileTimeUtc -le 0) {
        throw "Cannot recover client because its executable identity is inaccessible"
    }
    if (-not [IO.Path]::GetFileName($candidate.ExecutablePath).Equals(
            $clientProcessName, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Recovery candidate executable is not $clientProcessName"
    }
    if (-not $candidate.ExecutablePath.EndsWith(
            $clientRecoveryPathSuffix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Recovery candidate is outside the expected Rising Storm 2 Steam layout"
    }
    if (-not $candidate.CommandLineKnown) {
        throw "Cannot recover client because its command line is inaccessible"
    }

    $launchToken = Get-CommandLineLaunchToken $candidate.CommandLine
    if ($null -eq $launchToken -or
        -not (Test-CommandLineLaunchToken $candidate.CommandLine $launchToken)) {
        throw "Running client has no exact controller launch-token argument"
    }

    $candidate | Add-Member -NotePropertyName LaunchToken -NotePropertyValue $launchToken
    Write-ControlState $candidate

    $verification = Get-ControlledClientStatus
    if ($verification.Kind -ne "Running") {
        Remove-ControlStateIfMatching $candidate $launchToken
        throw (
            "Recovered client failed post-publication identity verification " +
            "($($verification.Kind)): $($verification.Reason)")
    }
    try {
        Write-Output "client recovered pid=$($verification.State.ProcessId) state=managed"
    } finally {
        $verification.Process.Dispose()
    }
}

function Start-OwnedClient([string]$Endpoint = "") {
    Assert-ControlDirectory $stateRoot | Out-Null
    $preflight = Resolve-StateForStart
    if ($preflight.Kind -eq "Running") {
        try {
            Write-Output "client already running pid=$($preflight.State.ProcessId) state=managed"
            return
        } finally {
            $preflight.Process.Dispose()
        }
    }

    $steamExecutable = Resolve-SteamExecutable
    $launchToken = [Guid]::NewGuid()
    $argumentLine = Format-SteamLaunchArguments $launchToken $Endpoint
    $notBefore = [DateTime]::UtcNow.AddSeconds(-2).ToFileTimeUtc()
    $launcher = Start-Process -FilePath $steamExecutable -ArgumentList $argumentLine -PassThru
    if ($null -ne $launcher) {
        $launcher.Dispose()
    }

    $launchWindowSeconds = $LaunchTimeoutSeconds + $LateLaunchGraceSeconds
    $deadline = [DateTime]::UtcNow.AddSeconds($launchWindowSeconds)
    $ownedRow = $null
    $unrelatedProcessIds = [Collections.Generic.HashSet[int]]::new()
    $firstUnverifiedSeenUtc = $null
    $ownershipGraceSeconds = 3
    do {
        $observedRows = @(Get-ClientProcessRows)
        foreach ($row in $observedRows) {
            if ($row.IdentityKnown -and
                $row.ProcessStartFileTimeUtc -ge $notBefore -and
                (Test-CommandLineLaunchToken $row.CommandLine $launchToken)) {
                if ($null -ne $ownedRow -and $ownedRow.ProcessId -ne $row.ProcessId) {
                    throw "Multiple clients reported the same controller launch token"
                }
                $ownedRow = $row
                [void]$unrelatedProcessIds.Remove([int]$row.ProcessId)
            } else {
                [void]$unrelatedProcessIds.Add([int]$row.ProcessId)
            }
        }
        if ($null -ne $ownedRow) {
            break
        }
        if ($observedRows.Count -gt 0) {
            if ($null -eq $firstUnverifiedSeenUtc) {
                $firstUnverifiedSeenUtc = [DateTime]::UtcNow
            } elseif (([DateTime]::UtcNow - $firstUnverifiedSeenUtc).TotalSeconds -ge
                    $ownershipGraceSeconds) {
                $missingCommandLine = @($observedRows | Where-Object {
                    -not $_.CommandLineKnown
                }).Count -gt 0
                $reason = if ($missingCommandLine) {
                    "process command line is inaccessible"
                } else {
                    "exact launch-token argument is absent"
                }
                throw (
                    "Client appeared but ownership could not be verified within " +
                    "$ownershipGraceSeconds seconds ($reason); leaving it unowned")
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    if ($null -eq $ownedRow) {
        $unowned = @(Get-ClientProcessRows)
        if ($unowned.Count -gt 0) {
            $missingCommandLine = @($unowned | Where-Object {
                -not $_.CommandLineKnown
            }).Count -gt 0
            $reason = if ($missingCommandLine) {
                "process command line is inaccessible"
            } else {
                "exact launch-token argument is absent"
            }
            throw "Steam launched a client whose ownership is unverifiable ($reason); leaving it unowned"
        }
        $graceDescription = if ($LateLaunchGraceSeconds -gt 0) {
            " plus a $LateLaunchGraceSeconds-second late-launch grace"
        } else { "" }
        throw (
            "Steam did not launch $clientProcessName within " +
            "$LaunchTimeoutSeconds seconds$graceDescription")
    }

    # Close the observation gap between finding the tagged client and publishing
    # ownership. Controller copies are serialized by an app-global mutex, but a
    # manual launch can still race this invocation.
    foreach ($row in @(Get-ClientProcessRows)) {
        if ($row.ProcessId -ne $ownedRow.ProcessId) {
            [void]$unrelatedProcessIds.Add([int]$row.ProcessId)
        }
    }

    $ownedRow | Add-Member -NotePropertyName LaunchToken -NotePropertyValue $launchToken
    try {
        Write-ControlState $ownedRow
    } catch {
        $publicationError = $_
        # State publication is the ownership handoff. On failure, make a
        # best-effort rollback only after re-verifying every identity component;
        # never turn cleanup into a PID-only termination path.
        $cleanupProcess = $null
        try {
            $cleanupProcess = [Diagnostics.Process]::GetProcessById($ownedRow.ProcessId)
            try { $cleanupPath = [string]$cleanupProcess.Path } catch { $cleanupPath = "" }
            if ([string]::IsNullOrWhiteSpace($cleanupPath)) {
                $cleanupPath = [string](Get-LimitedProcessImagePath $ownedRow.ProcessId)
            }
            if ([string]::IsNullOrWhiteSpace($cleanupPath)) {
                throw "Could not read client path before state-publication rollback"
            }
            $cleanupPath = [IO.Path]::GetFullPath($cleanupPath)
            $cleanupStart = $cleanupProcess.StartTime.ToUniversalTime().ToFileTimeUtc()
            $cleanupRow = Get-CimInstance Win32_Process -Filter (
                "ProcessId=$($ownedRow.ProcessId)") -ErrorAction Stop
            $cleanupCommandLine = if ($null -ne $cleanupRow) {
                [string]$cleanupRow.CommandLine
            } else { "" }
            if ([string]::IsNullOrWhiteSpace($cleanupCommandLine)) {
                $cleanupCommandLine = [string](Get-LimitedProcessCommandLine $ownedRow.ProcessId)
            }
            if ($cleanupStart -ne $ownedRow.ProcessStartFileTimeUtc -or
                -not $cleanupPath.Equals(
                    $ownedRow.ExecutablePath, [StringComparison]::OrdinalIgnoreCase) -or
                -not (Test-CommandLineLaunchToken (
                    $cleanupCommandLine) $launchToken)) {
                throw "Client identity changed before state-publication rollback"
            }
            $cleanupProcess.Kill()
            if (-not $cleanupProcess.WaitForExit(5000)) {
                throw "Client did not exit during state-publication rollback"
            }
        } catch {
            Write-Warning (
                "Could not roll back client after state publication failed: {0}" -f
                $_.Exception.Message)
        } finally {
            if ($null -ne $cleanupProcess) {
                $cleanupProcess.Dispose()
            }
        }
        throw $publicationError
    }

    if ($unrelatedProcessIds.Count -gt 0) {
        throw (
            "Managed client started, but another client appeared concurrently " +
            "(pid=$(@($unrelatedProcessIds) -join ',')); managed state was retained for safe Stop")
    }

    if ([string]::IsNullOrWhiteSpace($Endpoint)) {
        Write-Output "client started pid=$($ownedRow.ProcessId) via=steam appid=$steamAppId"
    } else {
        Write-Output "client started pid=$($ownedRow.ProcessId) via=steam connect=$Endpoint"
    }
}

function Stop-OwnedClient {
    $status = Get-ControlledClientStatus
    switch ($status.Kind) {
        "Missing" {
            $candidates = @(Get-ClientProcessRows)
            if ($candidates.Count -gt 0) {
                throw "Client is running without controller state; refusing to stop it"
            }
            Write-Output "client already stopped state=missing"
            return
        }
        "Stale" {
            Remove-ControlFile $statePath
            $candidates = @(Get-ClientProcessRows)
            if ($candidates.Count -gt 0) {
                Write-Output "managed client is gone; unmanaged client remains pid=$($candidates.ProcessId -join ',')"
            } else {
                Write-Output "client already stopped state=stale"
            }
            return
        }
        "Running" { }
        default {
            throw "Refusing to stop an unverified client ($($status.Kind)): $($status.Reason)"
        }
    }

    $process = $status.Process
    try {
        $process.Kill()
        if (-not $process.WaitForExit($StopTimeoutSeconds * 1000)) {
            throw "Client did not exit within $StopTimeoutSeconds seconds"
        }
    } finally {
        $process.Dispose()
    }
    Remove-ControlFile $statePath
    Write-Output "client stopped pid=$($status.State.ProcessId) state=managed"
}

function Test-ClientControl {
    $checks = 0
    $loopback = Format-ConnectEndpoint "127.0.0.1" 7777 $false
    if ($loopback -ne "127.0.0.1:7777") {
        throw "IPv4 loopback endpoint formatting failed"
    }
    $checks++

    $ipv6 = Format-ConnectEndpoint "::1" 7777 $false
    if ($ipv6 -ne "[::1]:7777") {
        throw "IPv6 loopback endpoint formatting failed"
    }
    $checks++

    try {
        Format-ConnectEndpoint "192.0.2.1" 7777 $false | Out-Null
        throw "Remote endpoint validation unexpectedly succeeded"
    } catch {
        if ($_.Exception.Message -notmatch "requires -AllowRemoteAddress") {
            throw
        }
    }
    $checks++

    $token = [Guid]::NewGuid()
    $arguments = Format-SteamLaunchArguments $token "127.0.0.1:7777"
    $expected = "-applaunch $steamAppId 127.0.0.1:7777 $launchTokenPrefix$($token.ToString('D'))"
    if ($arguments -ne $expected -or
        -not (Test-CommandLineLaunchToken "VNGame.exe $arguments" $token)) {
        throw "Steam launch argument or ownership-token formatting failed"
    }
    $checks++

    $tokenArgument = "$launchTokenPrefix$($token.ToString('D'))"
    if ((Test-CommandLineLaunchToken "VNGame.exe ${tokenArgument}suffix" $token) -or
        (Test-CommandLineLaunchToken "VNGame.exe prefix${tokenArgument}" $token)) {
        throw "Launch-token matching accepted a substring instead of an argument"
    }
    $checks++

    $quotedRetailCommandLine =
        '"D:\SteamLibrary\steamapps\common\Rising Storm 2\Binaries\Win64\VNGame.exe" ' +
        '127.0.0.1:7777 "' + $tokenArgument + '" -nostartupmovies -windowed -console'
    if (-not (Test-CommandLineLaunchToken $quotedRetailCommandLine $token)) {
        throw "Quoted retail launch-token argument was not recognized"
    }
    $checks++

    $parsedToken = Get-CommandLineLaunchToken $quotedRetailCommandLine
    if ($null -eq $parsedToken -or $parsedToken -ne $token) {
        throw "Recovery launch-token extraction failed"
    }
    $checks++

    if ($null -ne (Get-CommandLineLaunchToken (
            "VNGame.exe ${tokenArgument}suffix"))) {
        throw "Recovery launch-token extraction accepted a substring"
    }
    $checks++

    try {
        Get-CommandLineLaunchToken (
            "VNGame.exe $tokenArgument $tokenArgument") | Out-Null
        throw "Recovery launch-token extraction accepted duplicate tokens"
    } catch {
        if ($_.Exception.Message -notmatch "multiple controller launch tokens") {
            throw
        }
    }
    $checks++

    $limitedPath = [string](Get-LimitedProcessImagePath $PID)
    $limitedCommandLine = [string](Get-LimitedProcessCommandLine $PID)
    if ([string]::IsNullOrWhiteSpace($limitedPath) -or
        [string]::IsNullOrWhiteSpace($limitedCommandLine)) {
        throw "Limited-information process identity query failed"
    }
    $checks++

    $stateStatus = Get-ControlledClientStatus
    try {
        if ($stateStatus.Kind -notin @(
                "Missing", "Parsed", "Running", "Stale", "Corrupt",
                "IdentityMismatch", "Indeterminate")) {
            throw "Unexpected current client-control state: $($stateStatus.Kind)"
        }
    } finally {
        if ($stateStatus.Kind -eq "Running" -and $null -ne $stateStatus.Process) {
            $stateStatus.Process.Dispose()
        }
    }
    $checks++

    Write-Output "client-control self-test=ok checks=$checks lifecycle=untouched"
}

function Get-ControlMutexName {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        # VNGame is machine-global, so different checkouts must share the same
        # lifecycle lock rather than racing through repo-scoped mutexes.
        $identity = "RS2V_CLIENT_CONTROL|$steamAppId"
        $bytes = [Text.Encoding]::UTF8.GetBytes($identity)
        $hash = ([BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace("-", "")
        return "Global\RS2VClientControl_$($hash.Substring(0, 24))"
    } finally {
        $sha256.Dispose()
    }
}

switch ($Action) {
    "Status" { Show-ClientStatus; return }
    "SelfTest" { Test-ClientControl; return }
}

$mutex = [Threading.Mutex]::new($false, (Get-ControlMutexName))
$mutexOwned = $false
try {
    try {
        $mutexOwned = $mutex.WaitOne([TimeSpan]::FromSeconds(30))
    } catch [Threading.AbandonedMutexException] {
        $mutexOwned = $true
    }
    if (-not $mutexOwned) {
        throw "Timed out waiting for another client-control operation"
    }

    switch ($Action) {
        "Start" { Start-OwnedClient }
        "Recover" { Recover-OwnedClient }
        "Stop" { Stop-OwnedClient }
        "Restart" {
            $status = Get-ControlledClientStatus
            if ($status.Kind -eq "Running") {
                $status.Process.Dispose()
                Stop-OwnedClient
            } elseif ($status.Kind -notin @("Missing", "Stale")) {
                throw "Cannot restart an unverified client ($($status.Kind)): $($status.Reason)"
            }
            Start-OwnedClient
        }
        "Connect" {
            $endpoint = Format-ConnectEndpoint (
                $ServerAddress) $ServerPort ([bool]$AllowRemoteAddress)
            $status = Get-ControlledClientStatus
            if ($status.Kind -eq "Running") {
                $status.Process.Dispose()
                throw "Client is already running; use the in-game console command: open $endpoint"
            }
            if (@(Get-ClientProcessRows).Count -gt 0) {
                throw "An unmanaged client is already running; use the in-game console command: open $endpoint"
            }
            Start-OwnedClient $endpoint
        }
        default { throw "Unsupported client-control action: $Action" }
    }
} finally {
    if ($mutexOwned) {
        try { $mutex.ReleaseMutex() } catch { }
    }
    $mutex.Dispose()
}
