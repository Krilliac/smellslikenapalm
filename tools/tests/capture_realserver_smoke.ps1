[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..'))
$captureScript = Join-Path $repoRoot 'tools\capture_realserver.ps1'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $tempBase (
    'RS2V capture smoke {0}' -f [Guid]::NewGuid().ToString('N'))
$outputRoot = Join-Path $testRoot 'capture output'
$fakeDumpcap = Join-Path $testRoot 'fake tools\dumpcap.exe'
$fakeCapinfos = Join-Path $testRoot 'fake tools\capinfos.exe'
$collisionWatcher = Join-Path $testRoot 'fake tools\collision-watcher.exe'
$argumentsLog = Join-Path $testRoot 'fake dumpcap arguments.txt'
$powerShellExe = Join-Path $env:SystemRoot `
    'System32\WindowsPowerShell\v1.0\powershell.exe'
$utf8NoBom = New-Object Text.UTF8Encoding($false)
$serverAddress = '198.51.100.77'
$serverPort = 7777
$passed = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
    $script:passed++
}

function ConvertTo-TestArgument {
    param([AllowEmptyString()][string]$Argument)
    if ($null -eq $Argument -or $Argument.Length -eq 0) {
        return '""'
    }
    if ($Argument.IndexOf('"') -ge 0) {
        throw "The smoke harness does not accept a quote in an argument: $Argument"
    }
    if ($Argument -match '\s') {
        return '"' + $Argument + '"'
    }
    return $Argument
}

function Invoke-Capture {
    param(
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [Parameter(Mandatory = $true)][string]$Mode,
        [string]$CapinfosMode = 'success',
        [string]$Scenario = 'smoke success: deploy, move, fire, respawn',
        [string]$CollisionPath,
        [int]$InterfaceIndex = $script:routeInterfaceIndex
    )

    $env:RS2V_FAKE_DUMPCAP_MODE = $Mode
    $env:RS2V_FAKE_DUMPCAP_ARGS = $argumentsLog
    $env:RS2V_FAKE_CAPINFOS_MODE = $CapinfosMode
    if ([string]::IsNullOrWhiteSpace($CollisionPath)) {
        Remove-Item Env:\RS2V_FAKE_DUMPCAP_COLLISION -ErrorAction SilentlyContinue
    }
    else {
        $env:RS2V_FAKE_DUMPCAP_COLLISION = $CollisionPath
    }
    Remove-Item -LiteralPath $argumentsLog -Force -ErrorAction SilentlyContinue

    $arguments = @(
        '-NoProfile', '-NonInteractive', '-File', $captureScript,
        '-ServerAddress', $serverAddress,
        '-ServerPort', [string]$serverPort,
        '-Scenario', $Scenario,
        '-OutputPath', $OutputPath,
        '-InterfaceIndex', [string]$InterfaceIndex,
        '-DumpcapPath', $fakeDumpcap,
        '-CapinfosPath', $fakeCapinfos,
        '-MaxDurationSeconds', '1',
        '-MaxFileSizeKiB', '64',
        '-WatchdogGraceSeconds', '1'
    )
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $powerShellExe
    $startInfo.Arguments = (($arguments | ForEach-Object {
        ConvertTo-TestArgument ([string]$_)
    }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw 'Could not start the isolated capture script.'
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            try { $process.Kill() } catch { }
            throw 'The isolated capture script exceeded the smoke-test bound.'
        }
        $process.WaitForExit()
        return [PSCustomObject]@{
            ExitCode = $process.ExitCode
            Text = (@(
                $stdoutTask.GetAwaiter().GetResult(),
                $stderrTask.GetAwaiter().GetResult()
            ) | Where-Object {
                -not [string]::IsNullOrWhiteSpace($_)
            }) -join [Environment]::NewLine
        }
    }
    finally {
        $process.Dispose()
    }
}

function Assert-NoPartials {
    param([string]$Directory, [string]$Label)
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        return
    }
    $partials = @(Get-ChildItem -LiteralPath $Directory -Force | Where-Object {
        $_.Name.EndsWith('.partial.pcapng', [StringComparison]::OrdinalIgnoreCase) -or
        $_.Name.EndsWith('.manifest.partial.json', [StringComparison]::OrdinalIgnoreCase)
    })
    Assert-True ($partials.Count -eq 0) `
        "$Label left staging files: $(@($partials | ForEach-Object { $_.Name }) -join ', ')"
}

try {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $fakeDumpcap)) | Out-Null
    [IO.Directory]::CreateDirectory($outputRoot) | Out-Null

    $fakeSource = @'
using System;
using System.IO;
using System.Text;
using System.Threading;

public static class FakeDumpcap
{
    public static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--version")
        {
            Console.WriteLine("Dumpcap (Fake) 9.9.9-test");
            return 0;
        }

        string log = Environment.GetEnvironmentVariable("RS2V_FAKE_DUMPCAP_ARGS");
        if (!String.IsNullOrEmpty(log))
            File.WriteAllLines(log, args, new UTF8Encoding(false));

        string output = null;
        for (int index = 0; index + 1 < args.Length; ++index)
            if (args[index] == "-w")
                output = args[index + 1];
        if (String.IsNullOrEmpty(output))
            return 41;

        Directory.CreateDirectory(Path.GetDirectoryName(output));
        File.WriteAllBytes(output, Encoding.ASCII.GetBytes(
            "FAKE-PCAPNG\0endpoint-specific\0"));

        string collision = Environment.GetEnvironmentVariable(
            "RS2V_FAKE_DUMPCAP_COLLISION");
        if (!String.IsNullOrEmpty(collision))
            File.WriteAllText(collision, "RACED USER DATA", new UTF8Encoding(false));

        string mode = Environment.GetEnvironmentVariable("RS2V_FAKE_DUMPCAP_MODE");
        if (mode == "timeout")
            Thread.Sleep(30000);

        Console.Error.WriteLine(mode == "zero" ?
            "Packets captured: 0" : "Packets captured: 7");
        if (mode == "nostats")
        {
            // Intentionally omit receive/drop accounting.
        }
        else if (mode == "drops")
            Console.Error.WriteLine(
                "Packets received/dropped on interface 'fake': 9/2 (pcap:9/2)");
        else if (mode == "zero")
            Console.Error.WriteLine(
                "Packets received/dropped on interface 'fake': 0/0 (pcap:0/0)");
        else
            Console.Error.WriteLine(
                "Packets received/dropped on interface 'fake': 7/0 (pcap:7/0)");
        if (mode == "failure")
        {
            Console.Error.WriteLine("synthetic capture failure");
            return 9;
        }
        return 0;
    }
}
'@
    Add-Type -TypeDefinition $fakeSource -Language CSharp `
        -OutputType ConsoleApplication -OutputAssembly $fakeDumpcap

    $capinfosSource = @'
using System;
using System.IO;
using System.Threading;

public static class FakeCapinfos
{
    public static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--version")
        {
            Console.WriteLine("Capinfos (Fake) 9.9.9-test");
            return 0;
        }
        string mode = Environment.GetEnvironmentVariable(
            "RS2V_FAKE_CAPINFOS_MODE");
        if (mode == "failure")
        {
            Console.Error.WriteLine("synthetic malformed pcapng");
            return 12;
        }
        if (args.Length == 0 || !File.Exists(args[args.Length - 1]))
            return 13;
        if (mode == "manifest-race" &&
            !args[args.Length - 1].EndsWith(
                ".partial.pcapng", StringComparison.OrdinalIgnoreCase))
            Thread.Sleep(750);
        Console.WriteLine("File type: Wireshark/... - pcapng");
        Console.WriteLine("Number of packets: 7");
        Console.WriteLine("File size: " +
            new FileInfo(args[args.Length - 1]).Length + " bytes");
        return 0;
    }
}
'@
    Add-Type -TypeDefinition $capinfosSource -Language CSharp `
        -OutputType ConsoleApplication -OutputAssembly $fakeCapinfos

    $watcherSource = @'
using System;
using System.IO;
using System.Text;
using System.Threading;

public static class CollisionWatcher
{
    public static int Main()
    {
        string finalCapture = Environment.GetEnvironmentVariable(
            "RS2V_WATCH_FINAL_CAPTURE");
        string manifest = Environment.GetEnvironmentVariable(
            "RS2V_WATCH_MANIFEST");
        DateTime deadline = DateTime.UtcNow.AddSeconds(15);
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists(finalCapture))
            {
                File.WriteAllText(
                    manifest, "RACED MANIFEST DATA", new UTF8Encoding(false));
                return 0;
            }
            Thread.Sleep(1);
        }
        return 21;
    }
}
'@
    Add-Type -TypeDefinition $watcherSource -Language CSharp `
        -OutputType ConsoleApplication -OutputAssembly $collisionWatcher

    $routeObjects = @(Find-NetRoute -RemoteIPAddress $serverAddress)
    $route = @($routeObjects | Where-Object {
        $_.CimClass.CimClassName -eq 'MSFT_NetRoute' -and $_.State -eq 'Alive'
    } | Select-Object -First 1)
    Assert-True ($route.Count -eq 1) `
        'The smoke target must have one active route for validation.'
    $script:routeInterfaceIndex = [int]$route[0].InterfaceIndex
    $adapter = Get-NetAdapter -InterfaceIndex $script:routeInterfaceIndex
    Assert-True ([string]$adapter.Status -eq 'Up') `
        'The smoke route adapter must be active.'

    # Clean success publishes the capture and manifest only after dumpcap exits.
    $successPath = Join-Path $outputRoot 'success capture.pcapng'
    $successScenario = 'South machine-gunner: deploy, move, fire, respawn'
    $success = Invoke-Capture `
        -OutputPath $successPath -Mode success -Scenario $successScenario
    Assert-True ($success.ExitCode -eq 0) `
        "Success capture failed: $($success.Text)"
    $successManifestPath = $successPath + '.manifest.json'
    Assert-True (Test-Path -LiteralPath $successPath -PathType Leaf) `
        'Success did not publish the final capture.'
    Assert-True (Test-Path -LiteralPath $successManifestPath -PathType Leaf) `
        'Success did not publish the final manifest.'
    Assert-NoPartials $outputRoot 'Success'

    $manifestBytes = [IO.File]::ReadAllBytes($successManifestPath)
    $hasBom = $manifestBytes.Length -ge 3 -and
        $manifestBytes[0] -eq 0xEF -and $manifestBytes[1] -eq 0xBB -and
        $manifestBytes[2] -eq 0xBF
    Assert-True (-not $hasBom) 'Manifest must be UTF-8 without a BOM.'
    $manifest = [IO.File]::ReadAllText($successManifestPath) | ConvertFrom-Json
    $captureHash = (Get-FileHash -LiteralPath $successPath -Algorithm SHA256).Hash
    $dumpcapHash = (Get-FileHash -LiteralPath $fakeDumpcap -Algorithm SHA256).Hash
    Assert-True ([string]$manifest.schema -eq `
        'rs2v.realserver.capture-manifest.v1') 'Manifest schema drifted.'
    Assert-True ([string]$manifest.scenario -eq $successScenario) `
        'Manifest did not preserve the scenario.'
    Assert-True ([string]$manifest.endpoint.server_address -eq $serverAddress -and
        [int]$manifest.endpoint.server_port -eq $serverPort) `
        'Manifest endpoint drifted.'
    Assert-True ([string]$manifest.endpoint.capture_filter -eq `
        "udp and host $serverAddress and port $serverPort") `
        'Manifest capture filter is not endpoint-specific.'
    Assert-True ([bool]$manifest.route_validation.validated -and
        [string]$manifest.route_validation.route_state -eq 'Alive' -and
        [int]$manifest.route_validation.route_interface_index -eq
            $script:routeInterfaceIndex) 'Manifest route validation is incomplete.'
    Assert-True ([bool]$manifest.adapter.explicit_interface_override -and
        [int]$manifest.adapter.interface_index -eq $script:routeInterfaceIndex) `
        'Explicit interface override was not recorded.'
    Assert-True (-not [bool]$manifest.limits.promiscuous_mode -and
        [int]$manifest.limits.duration_seconds -eq 1 -and
        [int]$manifest.limits.filesize_kib -eq 64) `
        'Manifest did not record the capture bounds or non-promiscuous mode.'
    Assert-True ([string]$manifest.dumpcap.path -eq $fakeDumpcap -and
        [string]$manifest.dumpcap.version -eq 'Dumpcap (Fake) 9.9.9-test' -and
        [string]$manifest.dumpcap.sha256 -eq $dumpcapHash -and
        [bool]$manifest.dumpcap.hash_stable) `
        'Manifest dumpcap identity is incomplete.'
    Assert-True ([int64]$manifest.capture.byte_count -eq
            (Get-Item -LiteralPath $successPath).Length -and
        [string]$manifest.capture.sha256 -eq $captureHash) `
        'Manifest capture size/hash does not match the published bytes.'
    Assert-True ([int64]$manifest.packet_statistics.captured -eq 7 -and
        [int64]$manifest.packet_statistics.received -eq 7 -and
        [int64]$manifest.packet_statistics.dropped -eq 0 -and
        [bool]$manifest.packet_statistics.parse_complete) `
        'Manifest packet/drop statistics were not parsed.'
    Assert-True ([bool]$manifest.structural_validation.structurally_valid -and
        [string]$manifest.structural_validation.path -eq $fakeCapinfos -and
        [string]$manifest.structural_validation.version -eq
            'Capinfos (Fake) 9.9.9-test' -and
        [bool]$manifest.structural_validation.hash_stable) `
        'Manifest did not record structural capture validation.'

    $fakeArguments = @([IO.File]::ReadAllLines($argumentsLog))
    $filterIndex = [Array]::IndexOf($fakeArguments, '-f')
    $writeIndex = [Array]::IndexOf($fakeArguments, '-w')
    $interfaceArgumentIndex = [Array]::IndexOf($fakeArguments, '-i')
    Assert-True ($filterIndex -ge 0 -and
        $fakeArguments[$filterIndex + 1] -eq
            "udp and host $serverAddress and port $serverPort") `
        'dumpcap did not receive one exact endpoint filter argument.'
    Assert-True ($fakeArguments -contains '-p') `
        'dumpcap did not receive non-promiscuous mode.'
    Assert-True ($fakeArguments -contains 'duration:1' -and
        $fakeArguments -contains 'filesize:64') `
        'dumpcap did not receive both autostop bounds.'
    Assert-True ($interfaceArgumentIndex -ge 0 -and
        $fakeArguments[$interfaceArgumentIndex + 1] -eq
            "\Device\NPF_$($adapter.InterfaceGuid)") `
        'dumpcap did not receive the routed NPF interface.'
    Assert-True ($writeIndex -ge 0 -and
        $fakeArguments[$writeIndex + 1].EndsWith(
            '.partial.pcapng', [StringComparison]::OrdinalIgnoreCase) -and
        -not $fakeArguments[$writeIndex + 1].Equals(
            $successPath, [StringComparison]::OrdinalIgnoreCase)) `
        'dumpcap wrote directly to the final output instead of unique staging.'

    # Nonzero dumpcap exit leaves no final or staging capture.
    $failurePath = Join-Path $outputRoot 'failure capture.pcapng'
    $failure = Invoke-Capture -OutputPath $failurePath -Mode failure
    Assert-True ($failure.ExitCode -ne 0 -and
        $failure.Text -match 'exited with code 9') `
        "Synthetic dumpcap failure was not surfaced: $($failure.Text)"
    Assert-True (-not (Test-Path -LiteralPath $failurePath) -and
        -not (Test-Path -LiteralPath ($failurePath + '.manifest.json'))) `
        'Failed dumpcap published a final artifact.'
    Assert-NoPartials $outputRoot 'Failure'

    # Clean exit with unknown or nonzero loss is not acceptable evidence.
    $dropsPath = Join-Path $outputRoot 'dropped packets.pcapng'
    $drops = Invoke-Capture -OutputPath $dropsPath -Mode drops
    Assert-True ($drops.ExitCode -ne 0 -and
        $drops.Text -match 'reported 2 dropped packets') `
        "Dropped packets did not fail closed: $($drops.Text)"
    Assert-True (-not (Test-Path -LiteralPath $dropsPath) -and
        -not (Test-Path -LiteralPath ($dropsPath + '.manifest.json'))) `
        'Lossy capture was published as complete evidence.'
    Assert-NoPartials $outputRoot 'Dropped packets'

    $unknownStatsPath = Join-Path $outputRoot 'unknown drops.pcapng'
    $unknownStats = Invoke-Capture -OutputPath $unknownStatsPath -Mode nostats
    Assert-True ($unknownStats.ExitCode -ne 0 -and
        $unknownStats.Text -match 'parseable packet/drop statistics') `
        "Unknown packet loss did not fail closed: $($unknownStats.Text)"
    Assert-True (-not (Test-Path -LiteralPath $unknownStatsPath) -and
        -not (Test-Path -LiteralPath ($unknownStatsPath + '.manifest.json'))) `
        'Capture with unknown packet loss was published as evidence.'
    Assert-NoPartials $outputRoot 'Unknown drops'

    $zeroPath = Join-Path $outputRoot 'zero packets.pcapng'
    $zero = Invoke-Capture -OutputPath $zeroPath -Mode zero
    Assert-True ($zero.ExitCode -ne 0 -and
        $zero.Text -match 'captured zero packets') `
        "Zero-packet capture did not fail closed: $($zero.Text)"
    Assert-True (-not (Test-Path -LiteralPath $zeroPath) -and
        -not (Test-Path -LiteralPath ($zeroPath + '.manifest.json'))) `
        'Zero-packet capture was published as evidence.'
    Assert-NoPartials $outputRoot 'Zero packets'

    # A structurally invalid pcapng never reaches its final filename.
    $invalidPath = Join-Path $outputRoot 'invalid structure.pcapng'
    $invalid = Invoke-Capture -OutputPath $invalidPath -Mode success `
        -CapinfosMode failure
    Assert-True ($invalid.ExitCode -ne 0 -and
        $invalid.Text -match 'capinfos structural validation failed') `
        "Structural validation failure was not surfaced: $($invalid.Text)"
    Assert-True (-not (Test-Path -LiteralPath $invalidPath) -and
        -not (Test-Path -LiteralPath ($invalidPath + '.manifest.json'))) `
        'Structurally invalid capture was published.'
    Assert-NoPartials $outputRoot 'Structural failure'

    # A dumpcap that ignores its own duration is killed only by the exceptional
    # watchdog path; publication still remains atomic.
    $timeoutPath = Join-Path $outputRoot 'timeout capture.pcapng'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $timeout = Invoke-Capture -OutputPath $timeoutPath -Mode timeout
    $timer.Stop()
    Assert-True ($timeout.ExitCode -ne 0 -and
        $timeout.Text -match 'exceeded its 1-second autostop') `
        "Synthetic timeout was not explicit: $($timeout.Text)"
    Assert-True ($timer.Elapsed.TotalSeconds -lt 10) `
        "Timeout was not bounded: $($timer.Elapsed)"
    Assert-True (-not (Test-Path -LiteralPath $timeoutPath) -and
        -not (Test-Path -LiteralPath ($timeoutPath + '.manifest.json'))) `
        'Timed-out dumpcap published a final artifact.'
    Assert-NoPartials $outputRoot 'Timeout'

    # A final path raced in after validation is preserved byte-for-byte.
    $collisionPath = Join-Path $outputRoot 'collision capture.pcapng'
    $collision = Invoke-Capture -OutputPath $collisionPath -Mode success `
        -CollisionPath $collisionPath
    Assert-True ($collision.ExitCode -ne 0 -and
        $collision.Text -match 'collision appeared during capture') `
        "Output collision was not rejected: $($collision.Text)"
    Assert-True ([IO.File]::ReadAllText($collisionPath) -eq 'RACED USER DATA') `
        'Output collision bytes were overwritten or removed.'
    Assert-True (-not (Test-Path -LiteralPath ($collisionPath + '.manifest.json'))) `
        'Collision failure published a manifest.'
    Assert-NoPartials $outputRoot 'Collision'

    # If a manifest collision appears after capture promotion, publication
    # rolls back only the hash-verified capture it just published.
    $manifestRacePath = Join-Path $outputRoot 'manifest race.pcapng'
    $manifestRaceManifest = $manifestRacePath + '.manifest.json'
    $env:RS2V_WATCH_FINAL_CAPTURE = $manifestRacePath
    $env:RS2V_WATCH_MANIFEST = $manifestRaceManifest
    $watcher = Start-Process -FilePath $collisionWatcher -PassThru
    try {
        $manifestRace = Invoke-Capture -OutputPath $manifestRacePath `
            -Mode success -CapinfosMode manifest-race
        if (-not $watcher.WaitForExit(5000)) {
            $watcher.Kill()
            throw 'Manifest-collision watcher did not exit.'
        }
        Assert-True ($watcher.ExitCode -eq 0) `
            'Manifest-collision watcher never observed capture promotion.'
        Assert-True ($manifestRace.ExitCode -ne 0 -and
            $manifestRace.Text -match 'already exists') `
            "Post-promotion manifest collision was not surfaced: $($manifestRace.Text)"
        Assert-True (-not (Test-Path -LiteralPath $manifestRacePath)) `
            'Manifest failure left an orphan capture after safe rollback.'
        Assert-True ([IO.File]::ReadAllText($manifestRaceManifest) -eq
            'RACED MANIFEST DATA') `
            'Manifest collision bytes were overwritten or removed.'
        Assert-NoPartials $outputRoot 'Manifest rollback'
    }
    finally {
        if (-not $watcher.HasExited) {
            $watcher.Kill()
            [void]$watcher.WaitForExit(5000)
        }
        $watcher.Dispose()
        Remove-Item Env:\RS2V_WATCH_FINAL_CAPTURE -ErrorAction SilentlyContinue
        Remove-Item Env:\RS2V_WATCH_MANIFEST -ErrorAction SilentlyContinue
    }

    # Raw packet data is never allowed under the source Git work tree.
    $repoCapture = Join-Path $repoRoot `
        ('capture-realserver-smoke-{0}.pcapng' -f [Guid]::NewGuid().ToString('N'))
    $repoRejected = Invoke-Capture -OutputPath $repoCapture -Mode success
    Assert-True ($repoRejected.ExitCode -ne 0 -and
        $repoRejected.Text -match 'outside every Git work tree') `
        "Repository output was not rejected: $($repoRejected.Text)"
    Assert-True (-not (Test-Path -LiteralPath $repoCapture) -and
        -not (Test-Path -LiteralPath ($repoCapture + '.manifest.json'))) `
        'Repository rejection left raw capture artifacts.'

    # Reparse-point parents must not redirect output publication elsewhere.
    $junctionTarget = Join-Path $testRoot 'junction target'
    $junctionPath = Join-Path $testRoot 'junction output'
    [IO.Directory]::CreateDirectory($junctionTarget) | Out-Null
    New-Item -ItemType Junction -Path $junctionPath -Target $junctionTarget |
        Out-Null
    $junctionOutput = Join-Path $junctionPath 'redirected.pcapng'
    $junctionRejected = Invoke-Capture -OutputPath $junctionOutput -Mode success
    Assert-True ($junctionRejected.ExitCode -ne 0 -and
        $junctionRejected.Text -match 'reparse point') `
        "Junction output was not rejected: $($junctionRejected.Text)"
    Assert-True (-not (Test-Path -LiteralPath `
        (Join-Path $junctionTarget 'redirected.pcapng'))) `
        'Junction rejection wrote into the target directory.'

    # C0 controls cannot create multiline or ambiguous manifest records.
    $controlPath = Join-Path $outputRoot 'control scenario.pcapng'
    $controlRejected = Invoke-Capture -OutputPath $controlPath -Mode success `
        -Scenario "deploy`tunexpected"
    Assert-True ($controlRejected.ExitCode -ne 0 -and
        $controlRejected.Text -match 'single-record description') `
        "Control-character scenario was not rejected: $($controlRejected.Text)"
    Assert-True (-not (Test-Path -LiteralPath $controlPath) -and
        -not (Test-Path -LiteralPath ($controlPath + '.manifest.json'))) `
        'Control-character rejection published an artifact.'

    Write-Output "capture_realserver smoke tests passed: $passed assertions"
}
finally {
    Remove-Item Env:\RS2V_FAKE_DUMPCAP_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_FAKE_DUMPCAP_ARGS -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_FAKE_DUMPCAP_COLLISION -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_FAKE_CAPINFOS_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_WATCH_FINAL_CAPTURE -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_WATCH_MANIFEST -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $testRoot) {
        $fullTestRoot = [IO.Path]::GetFullPath($testRoot)
        $tempPrefix = $tempBase.TrimEnd('\') + '\'
        if (-not $fullTestRoot.StartsWith(
                $tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean a test path outside temp: $fullTestRoot"
        }
        Remove-Item -LiteralPath $fullTestRoot -Recurse -Force
    }
}
