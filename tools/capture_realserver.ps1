[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ServerAddress,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$ServerPort,

    [ValidateNotNullOrEmpty()]
    [string]$Scenario = 'join, select team and role, deploy, move, look, fire, and respawn',

    [string]$OutputPath,

    [ValidateRange(0, 2147483647)]
    [int]$InterfaceIndex = 0,

    [ValidateNotNullOrEmpty()]
    [string]$DumpcapPath = 'C:\Program Files\Wireshark\dumpcap.exe',

    [string]$CapinfosPath,

    [ValidateRange(1, 3600)]
    [int]$MaxDurationSeconds = 60,

    [ValidateRange(64, 2097152)]
    [int]$MaxFileSizeKiB = 262144,

    [ValidateRange(1, 60)]
    [int]$WatchdogGraceSeconds = 10
)

# Bounded, endpoint-specific capture of a real RS2: Vietnam gameplay session.
# Example:
#   powershell -NoProfile -File tools\capture_realserver.ps1 `
#       -ServerAddress 203.0.113.10 -ServerPort 7777 `
#       -Scenario 'South machine-gunner deploy and respawn'
#
# The raw capture and its manifest are intentionally published outside every Git
# work tree. The capture first uses a unique, same-directory .partial.pcapng and
# is renamed only after dumpcap exits cleanly and all integrity checks pass.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8NoBom = New-Object Text.UTF8Encoding($false)
$ownedPartialPath = $null
$ownedManifestPartialPath = $null

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    try {
        return [IO.Path]::GetFullPath($Path)
    }
    catch {
        throw "Invalid path '$Path': $($_.Exception.Message)"
    }
}

function Assert-NoAlternateDataStream {
    param([Parameter(Mandatory = $true)][string]$FullPath, [string]$Label)
    $root = [IO.Path]::GetPathRoot($FullPath)
    if ([string]::IsNullOrWhiteSpace($root)) {
        throw "$Label does not have a rooted filesystem path: $FullPath"
    }
    $remainder = $FullPath.Substring($root.Length)
    if ($remainder.IndexOf(':') -ge 0) {
        throw "$Label must not use an alternate data stream: $FullPath"
    }
}

function Assert-NoReparsePoint {
    param([Parameter(Mandatory = $true)][string]$FullPath, [string]$Label)
    $current = $FullPath
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Label traverses a symlink, junction, or other reparse point: $current"
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent -or
            $parent.FullName.Equals($current, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent.FullName
    }
}

function Test-IsInsideGitWorkTree {
    param([Parameter(Mandatory = $true)][string]$Directory)
    $current = $Directory
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        $gitMarker = Join-Path $current '.git'
        if (Test-Path -LiteralPath $gitMarker -PathType Leaf) {
            $markerText = [IO.File]::ReadAllText($gitMarker)
            if ($markerText -match '^\s*gitdir\s*:') {
                return $true
            }
        }
        elseif (Test-Path -LiteralPath $gitMarker -PathType Container) {
            # Do not treat an unrelated directory merely named .git as a work
            # tree marker. A real Git directory has HEAD plus object/ref state.
            if ((Test-Path -LiteralPath (Join-Path $gitMarker 'HEAD') -PathType Leaf) -and
                ((Test-Path -LiteralPath (Join-Path $gitMarker 'objects') -PathType Container) -or
                 (Test-Path -LiteralPath (Join-Path $gitMarker 'commondir') -PathType Leaf))) {
                return $true
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent -or
            $parent.FullName.Equals($current, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent.FullName
    }
    return $false
}

function ConvertTo-NativeArgument {
    param([AllowEmptyString()][string]$Argument)
    if ($null -eq $Argument -or $Argument.Length -eq 0) {
        return '""'
    }
    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }

    # Quote according to CommandLineToArgvW rules, including trailing slashes.
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (($slashes * 2) + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) {
            [void]$builder.Append(('\' * $slashes))
            $slashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) {
        [void]$builder.Append(('\' * ($slashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][int]$TimeoutMilliseconds
    )

    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        ConvertTo-NativeArgument ([string]$_)
    }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Could not start $FilePath"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutMilliseconds)
        if ($timedOut) {
            # This is an exceptional watchdog path. Normal capture termination is
            # exclusively dumpcap's duration/filesize autostop and clean exit.
            try {
                $process.Kill()
            }
            catch {
                throw "Timed out and could not terminate the exact dumpcap process: $($_.Exception.Message)"
            }
            if (-not $process.WaitForExit(5000)) {
                throw 'Timed-out dumpcap did not exit after termination.'
            }
        }
        # Required after the timed wait so redirected asynchronous reads flush.
        $process.WaitForExit()
        return [PSCustomObject]@{
            ExitCode = $process.ExitCode
            TimedOut = $timedOut
            StandardOutput = $stdoutTask.GetAwaiter().GetResult()
            StandardError = $stderrTask.GetAwaiter().GetResult()
        }
    }
    finally {
        $process.Dispose()
    }
}

function Get-PacketStatistics {
    param([string]$StandardOutput, [string]$StandardError)
    $combined = @($StandardOutput, $StandardError) -join [Environment]::NewLine
    $captured = $null
    $received = $null
    $dropped = $null

    $capturedMatch = [regex]::Match(
        $combined, 'Packets\s+captured\s*:\s*([0-9]+)',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($capturedMatch.Success) {
        $captured = [int64]$capturedMatch.Groups[1].Value
    }
    $pairMatch = [regex]::Match(
        $combined,
        'Packets\s+received/dropped\s+on\s+interface[^\r\n]*?:\s*([0-9]+)\s*/\s*([0-9]+)',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($pairMatch.Success) {
        $received = [int64]$pairMatch.Groups[1].Value
        $dropped = [int64]$pairMatch.Groups[2].Value
    }
    else {
        $dropMatch = [regex]::Match(
            $combined, 'Packets\s+dropped\s*:\s*([0-9]+)',
            [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($dropMatch.Success) {
            $dropped = [int64]$dropMatch.Groups[1].Value
        }
    }

    $summaryLines = @($combined -split '\r?\n' | Where-Object {
        $_ -match '(?i)packet|drop'
    } | Select-Object -First 32 | ForEach-Object {
        if ($_.Length -gt 512) { $_.Substring(0, 512) } else { $_ }
    })

    return [PSCustomObject]@{
        captured = $captured
        received = $received
        dropped = $dropped
        parse_complete = ($null -ne $captured -and $null -ne $dropped)
        dumpcap_summary_lines = $summaryLines
    }
}

function Write-NewUtf8File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    $stream = $null
    $writer = $null
    try {
        $stream = New-Object IO.FileStream(
            $Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        $writer = New-Object IO.StreamWriter($stream, $utf8NoBom)
        $stream = $null
        $writer.Write($Content)
    }
    finally {
        if ($null -ne $writer) { $writer.Dispose() }
        if ($null -ne $stream) { $stream.Dispose() }
    }
}

function Remove-OwnedPartialFile {
    param([string]$Path, [string]$ExpectedDirectory, [string]$ExpectedSuffix)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path)) {
        return
    }
    try {
        $fullPath = Get-FullPath $Path
        $parent = Split-Path -Parent $fullPath
        $leaf = Split-Path -Leaf $fullPath
        $item = Get-Item -LiteralPath $fullPath -Force
        if (-not $parent.Equals(
                $ExpectedDirectory, [StringComparison]::OrdinalIgnoreCase) -or
            -not $leaf.EndsWith(
                $ExpectedSuffix, [StringComparison]::OrdinalIgnoreCase) -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $item.PSIsContainer) {
            Write-Warning "Refusing to clean an unverified partial path: $fullPath"
            return
        }
        [IO.File]::Delete($fullPath)
    }
    catch {
        Write-Warning "Could not clean owned partial '$Path': $($_.Exception.Message)"
    }
}

function Remove-VerifiedPublishedCapture {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedDirectory,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return
    }
    $fullPath = Get-FullPath $Path
    $parent = Split-Path -Parent $fullPath
    $item = Get-Item -LiteralPath $fullPath -Force
    if (-not $parent.Equals(
            $ExpectedDirectory, [StringComparison]::OrdinalIgnoreCase) -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.PSIsContainer) {
        throw "Refusing rollback of an unverified published path: $fullPath"
    }
    $actualHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedSha256) {
        throw "Refusing rollback because the published capture identity changed: $fullPath"
    }
    [IO.File]::Delete($fullPath)
}

$outputDirectory = $null
try {
    $parsedAddress = $null
    if (-not [Net.IPAddress]::TryParse($ServerAddress, [ref]$parsedAddress) -or
        $parsedAddress.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'ServerAddress must be a literal IPv4 address; hostnames and broad filters are not accepted.'
    }
    if ($parsedAddress.Equals([Net.IPAddress]::Any) -or
        $parsedAddress.Equals([Net.IPAddress]::Broadcast)) {
        throw 'ServerAddress must identify one routable host.'
    }
    $normalizedAddress = $parsedAddress.ToString()

    if ([string]::IsNullOrWhiteSpace($Scenario) -or $Scenario.Length -gt 512 -or
        $Scenario -match '[\x00-\x1F\x7F]') {
        throw 'Scenario must be a nonempty, single-record description of at most 512 characters.'
    }

    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $captureRoot = if (Test-Path -LiteralPath 'D:\') {
            'D:\RE-Tools\RS2V-Captures'
        }
        else {
            Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'RS2V-Captures'
        }
        $uniqueName = 'rs2-realserver-{0}-{1}-{2}.pcapng' -f `
            ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')), $ServerPort,
            ([Guid]::NewGuid().ToString('N').Substring(0, 8))
        $OutputPath = Join-Path $captureRoot $uniqueName
    }

    $fullOutputPath = Get-FullPath $OutputPath
    Assert-NoAlternateDataStream $fullOutputPath 'OutputPath'
    if (-not [IO.Path]::GetExtension($fullOutputPath).Equals(
            '.pcapng', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputPath must end in .pcapng.'
    }
    $outputDirectory = Split-Path -Parent $fullOutputPath
    if ([string]::IsNullOrWhiteSpace($outputDirectory)) {
        throw 'OutputPath must include a parent directory.'
    }
    Assert-NoReparsePoint $outputDirectory 'OutputPath'
    if (Test-IsInsideGitWorkTree $outputDirectory) {
        throw 'Raw captures must be written outside every Git work tree.'
    }
    if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
        [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    }
    Assert-NoReparsePoint $outputDirectory 'OutputPath'

    $manifestPath = $fullOutputPath + '.manifest.json'
    foreach ($reservedPath in @($fullOutputPath, $manifestPath)) {
        if (Test-Path -LiteralPath $reservedPath) {
            throw "Refusing to overwrite or alias an existing output: $reservedPath"
        }
    }

    $fullDumpcapPath = Get-FullPath $DumpcapPath
    Assert-NoAlternateDataStream $fullDumpcapPath 'DumpcapPath'
    Assert-NoReparsePoint $fullDumpcapPath 'DumpcapPath'
    if (-not (Test-Path -LiteralPath $fullDumpcapPath -PathType Leaf)) {
        throw "dumpcap was not found at: $fullDumpcapPath"
    }
    if ([string]::IsNullOrWhiteSpace($CapinfosPath)) {
        $CapinfosPath = Join-Path (Split-Path -Parent $fullDumpcapPath) 'capinfos.exe'
    }
    $fullCapinfosPath = Get-FullPath $CapinfosPath
    Assert-NoAlternateDataStream $fullCapinfosPath 'CapinfosPath'
    Assert-NoReparsePoint $fullCapinfosPath 'CapinfosPath'
    if (-not (Test-Path -LiteralPath $fullCapinfosPath -PathType Leaf)) {
        throw "capinfos was not found at: $fullCapinfosPath"
    }
    if ($fullCapinfosPath.Equals(
            $fullDumpcapPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'dumpcap and capinfos must be distinct executable files.'
    }
    $scriptPath = Get-FullPath $MyInvocation.MyCommand.Path
    foreach ($protectedPath in @(
            $fullDumpcapPath, $fullCapinfosPath, $scriptPath, $manifestPath)) {
        if ($fullOutputPath.Equals(
                $protectedPath, [StringComparison]::OrdinalIgnoreCase)) {
            throw "OutputPath aliases a protected file: $protectedPath"
        }
    }

    $filter = "udp and host $normalizedAddress and port $ServerPort"
    $routeObjects = @(Find-NetRoute -RemoteIPAddress $normalizedAddress)
    $route = @($routeObjects | Where-Object {
        $_.CimClass.CimClassName -eq 'MSFT_NetRoute'
    } | Select-Object -First 1)
    $sourceAddress = @($routeObjects | Where-Object {
        $_.CimClass.CimClassName -eq 'MSFT_NetIPAddress'
    } | Select-Object -First 1)
    if ($route.Count -ne 1 -or $sourceAddress.Count -ne 1 -or
        [string]$route[0].State -ne 'Alive') {
        throw "No active route was found for $normalizedAddress."
    }
    $routeInterfaceIndex = [int]$route[0].InterfaceIndex
    if ($InterfaceIndex -ne 0 -and $InterfaceIndex -ne $routeInterfaceIndex) {
        throw "InterfaceIndex $InterfaceIndex is not the active route for $normalizedAddress (route uses $routeInterfaceIndex)."
    }
    $selectedInterfaceIndex = $routeInterfaceIndex
    $adapter = Get-NetAdapter -InterfaceIndex $selectedInterfaceIndex
    if ($null -eq $adapter -or [string]$adapter.Status -ne 'Up') {
        throw "The routed adapter at interface $selectedInterfaceIndex is not active."
    }
    $adapterGuid = [string]$adapter.InterfaceGuid
    if ([string]::IsNullOrWhiteSpace($adapterGuid)) {
        throw "The routed adapter at interface $selectedInterfaceIndex has no capture GUID."
    }
    $dumpcapInterface = "\Device\NPF_$adapterGuid"

    $dumpcapHashBefore = (Get-FileHash -LiteralPath $fullDumpcapPath -Algorithm SHA256).Hash
    $versionResult = Invoke-BoundedProcess $fullDumpcapPath @('--version') 5000
    if ($versionResult.TimedOut -or $versionResult.ExitCode -ne 0) {
        throw "dumpcap --version failed or timed out (exit $($versionResult.ExitCode))."
    }
    $versionLine = @((@($versionResult.StandardOutput, $versionResult.StandardError) -join "`n") `
        -split '\r?\n' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -First 1)
    if ($versionLine.Count -ne 1) {
        throw 'dumpcap --version did not return an identity line.'
    }
    $capinfosHashBefore = (Get-FileHash `
        -LiteralPath $fullCapinfosPath -Algorithm SHA256).Hash
    $capinfosVersionResult = Invoke-BoundedProcess `
        $fullCapinfosPath @('--version') 5000
    if ($capinfosVersionResult.TimedOut -or
        $capinfosVersionResult.ExitCode -ne 0) {
        throw "capinfos --version failed or timed out (exit $($capinfosVersionResult.ExitCode))."
    }
    $capinfosVersionLine = @((@(
            $capinfosVersionResult.StandardOutput,
            $capinfosVersionResult.StandardError
        ) -join "`n") -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -First 1)
    if ($capinfosVersionLine.Count -ne 1) {
        throw 'capinfos --version did not return an identity line.'
    }

    $captureId = [Guid]::NewGuid().ToString('N')
    $captureLeaf = [IO.Path]::GetFileNameWithoutExtension($fullOutputPath)
    $ownedPartialPath = Join-Path $outputDirectory `
        ('.{0}.{1}.partial.pcapng' -f $captureLeaf, $captureId)
    $ownedManifestPartialPath = Join-Path $outputDirectory `
        ('.{0}.{1}.manifest.partial.json' -f $captureLeaf, $captureId)
    if ((Test-Path -LiteralPath $ownedPartialPath) -or
        (Test-Path -LiteralPath $ownedManifestPartialPath)) {
        throw 'The unique capture staging path unexpectedly already exists.'
    }

    $captureArguments = @(
        '-i', $dumpcapInterface,
        '-f', $filter,
        '-p',
        '-B', '64',
        '-a', "duration:$MaxDurationSeconds",
        '-a', "filesize:$MaxFileSizeKiB",
        '-w', $ownedPartialPath
    )
    $startedUtc = [DateTime]::UtcNow
    Write-Output "Capturing $normalizedAddress`:$ServerPort on $($adapter.Name) for at most $MaxDurationSeconds seconds."
    Write-Output "Filter: $filter"
    Write-Output "Scenario: $Scenario"
    Write-Output "Staging: $ownedPartialPath"

    $watchdogMilliseconds = [int](($MaxDurationSeconds + $WatchdogGraceSeconds) * 1000)
    $captureResult = Invoke-BoundedProcess `
        $fullDumpcapPath $captureArguments $watchdogMilliseconds
    $completedUtc = [DateTime]::UtcNow
    if ($captureResult.TimedOut) {
        throw "dumpcap exceeded its $MaxDurationSeconds-second autostop plus $WatchdogGraceSeconds-second watchdog grace."
    }
    if ($captureResult.ExitCode -ne 0) {
        $diagnostic = (($captureResult.StandardError -split '\r?\n' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -First 4) -join ' | ')
        throw "dumpcap exited with code $($captureResult.ExitCode): $diagnostic"
    }
    if (-not (Test-Path -LiteralPath $ownedPartialPath -PathType Leaf)) {
        throw 'dumpcap exited successfully without creating the staged capture.'
    }
    Assert-NoReparsePoint $ownedPartialPath 'Staged capture'
    $partialItem = Get-Item -LiteralPath $ownedPartialPath -Force
    if ($partialItem.Length -le 0) {
        throw 'dumpcap created an empty staged capture.'
    }

    $dumpcapHashAfter = (Get-FileHash -LiteralPath $fullDumpcapPath -Algorithm SHA256).Hash
    if ($dumpcapHashAfter -ne $dumpcapHashBefore) {
        throw 'dumpcap changed during capture; refusing to publish unverifiable output.'
    }
    $captureHash = (Get-FileHash -LiteralPath $ownedPartialPath -Algorithm SHA256).Hash
    $captureBytes = [int64]$partialItem.Length
    $packetStatistics = Get-PacketStatistics `
        $captureResult.StandardOutput $captureResult.StandardError
    if (-not $packetStatistics.parse_complete) {
        throw 'dumpcap did not report parseable packet/drop statistics; refusing evidence publication.'
    }
    if ([int64]$packetStatistics.captured -le 0) {
        throw 'dumpcap captured zero packets; refusing unusable evidence publication.'
    }
    if ([int64]$packetStatistics.dropped -ne 0) {
        throw "dumpcap reported $($packetStatistics.dropped) dropped packets; refusing incomplete evidence publication."
    }

    $capinfosArguments = @(
        '-C', '-t', '-E', '-c', '-s', '-M', $ownedPartialPath
    )
    $capinfosResult = Invoke-BoundedProcess `
        $fullCapinfosPath $capinfosArguments 10000
    if ($capinfosResult.TimedOut -or $capinfosResult.ExitCode -ne 0) {
        $capinfosDiagnostic = (($capinfosResult.StandardError -split '\r?\n' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -First 4) -join ' | ')
        throw "capinfos structural validation failed (exit $($capinfosResult.ExitCode)): $capinfosDiagnostic"
    }
    $capinfosHashAfter = (Get-FileHash `
        -LiteralPath $fullCapinfosPath -Algorithm SHA256).Hash
    if ($capinfosHashAfter -ne $capinfosHashBefore) {
        throw 'capinfos changed during validation; refusing unverifiable output.'
    }
    $capinfosSummaryLines = @((@(
            $capinfosResult.StandardOutput, $capinfosResult.StandardError
        ) -join "`n") -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -First 32 | ForEach-Object {
            if ($_.Length -gt 512) { $_.Substring(0, 512) } else { $_ }
        })

    foreach ($reservedPath in @($fullOutputPath, $manifestPath)) {
        if (Test-Path -LiteralPath $reservedPath) {
            throw "An output collision appeared during capture; preserving it unchanged: $reservedPath"
        }
    }

    $manifest = [ordered]@{
        schema = 'rs2v.realserver.capture-manifest.v1'
        capture_id = $captureId
        scenario = $Scenario
        started_utc = $startedUtc.ToString('o')
        completed_utc = $completedUtc.ToString('o')
        endpoint = [ordered]@{
            server_address = $normalizedAddress
            server_port = $ServerPort
            capture_filter = $filter
        }
        adapter = [ordered]@{
            interface_index = $selectedInterfaceIndex
            explicit_interface_override = ($InterfaceIndex -ne 0)
            name = [string]$adapter.Name
            description = [string]$adapter.InterfaceDescription
            interface_guid = $adapterGuid
            dumpcap_interface = $dumpcapInterface
        }
        route_validation = [ordered]@{
            validated = $true
            destination = $normalizedAddress
            source_address = [string]$sourceAddress[0].IPAddress
            route_state = [string]$route[0].State
            destination_prefix = [string]$route[0].DestinationPrefix
            next_hop = [string]$route[0].NextHop
            route_interface_index = $routeInterfaceIndex
            route_interface_alias = [string]$route[0].InterfaceAlias
            adapter_status = [string]$adapter.Status
        }
        limits = [ordered]@{
            duration_seconds = $MaxDurationSeconds
            filesize_kib = $MaxFileSizeKiB
            watchdog_grace_seconds = $WatchdogGraceSeconds
            promiscuous_mode = $false
            buffer_mib = 64
        }
        dumpcap = [ordered]@{
            path = $fullDumpcapPath
            version = [string]$versionLine[0]
            sha256 = $dumpcapHashBefore
            sha256_after_capture = $dumpcapHashAfter
            hash_stable = $true
            exit_code = $captureResult.ExitCode
        }
        structural_validation = [ordered]@{
            tool = 'capinfos'
            path = $fullCapinfosPath
            version = [string]$capinfosVersionLine[0]
            sha256 = $capinfosHashBefore
            sha256_after_validation = $capinfosHashAfter
            hash_stable = $true
            exit_code = $capinfosResult.ExitCode
            structurally_valid = $true
            summary_lines = $capinfosSummaryLines
        }
        capture = [ordered]@{
            path = $fullOutputPath
            byte_count = $captureBytes
            sha256 = $captureHash
        }
        packet_statistics = $packetStatistics
    }
    $manifestJson = ($manifest | ConvertTo-Json -Depth 8) + "`n"
    Write-NewUtf8File $ownedManifestPartialPath $manifestJson

    # Both moves stay within one directory. Move refuses existing destinations;
    # it never overwrites a user's capture or manifest.
    if ((Test-Path -LiteralPath $fullOutputPath) -or
        (Test-Path -LiteralPath $manifestPath)) {
        throw 'An output collision appeared before atomic publication.'
    }
    $publishedCaptureOwned = $false
    try {
        [IO.File]::Move($ownedPartialPath, $fullOutputPath)
        $ownedPartialPath = $null
        $publishedCaptureOwned = $true
        $publishedHash = (Get-FileHash `
            -LiteralPath $fullOutputPath -Algorithm SHA256).Hash
        if ($publishedHash -ne $captureHash) {
            throw 'The atomically published capture does not match its staged SHA-256.'
        }
        $publishedValidation = Invoke-BoundedProcess $fullCapinfosPath @(
            '-C', '-t', '-E', '-c', '-s', '-M', $fullOutputPath
        ) 10000
        if ($publishedValidation.TimedOut -or
            $publishedValidation.ExitCode -ne 0) {
            throw 'The atomically published capture failed its final structural validation.'
        }
        [IO.File]::Move($ownedManifestPartialPath, $manifestPath)
        $ownedManifestPartialPath = $null
        $publishedCaptureOwned = $false
    }
    catch {
        $publicationError = $_.Exception
        if ($publishedCaptureOwned) {
            try {
                Remove-VerifiedPublishedCapture `
                    $fullOutputPath $outputDirectory $captureHash
                $publishedCaptureOwned = $false
            }
            catch {
                throw "Publication failed ($($publicationError.Message)); safe capture rollback also failed: $($_.Exception.Message)"
            }
        }
        throw $publicationError
    }

    Write-Output "Capture: $fullOutputPath ($captureBytes bytes, SHA256 $captureHash)"
    Write-Output "Manifest: $manifestPath"
}
finally {
    if ($null -ne $outputDirectory) {
        Remove-OwnedPartialFile `
            $ownedPartialPath $outputDirectory '.partial.pcapng'
        Remove-OwnedPartialFile `
            $ownedManifestPartialPath $outputDirectory '.manifest.partial.json'
    }
}
