[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$sourceController = [IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "..\client_control.ps1"))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $tempBase (
    "RS2V client control smoke {0}" -f [Guid]::NewGuid().ToString("N"))
$controller = Join-Path $testRoot "tools\client_control.ps1"
$mockSteam = Join-Path $testRoot "mock bin\steam.exe"
$mockClientName = "RS2VCCMock_$([Guid]::NewGuid().ToString('N')).exe"
$mockClient = Join-Path $testRoot "mock game\$mockClientName"
$clientArgsCapture = Join-Path $testRoot "client arguments.txt"
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$powerShellExe = Join-Path $env:SystemRoot `
    "System32\WindowsPowerShell\v1.0\powershell.exe"
$passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
    $script:passed++
}

function ConvertTo-NativeArgument([string]$Argument) {
    if ($Argument.IndexOf('"') -ge 0) {
        throw "Test argument contains an unsupported quote: $Argument"
    }
    if ($Argument -match '[\s]') {
        return '"' + $Argument + '"'
    }
    return $Argument
}

function Invoke-Controller([string]$Action, [string[]]$ExtraArguments = @()) {
    $arguments = @("-NoProfile", "-File", $controller, "-Action", $Action) +
        $ExtraArguments
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $powerShellExe
    $info.Arguments = ($arguments | ForEach-Object {
        ConvertTo-NativeArgument ([string]$_)
    }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) {
        throw "Failed to start isolated controller"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        try { $process.Kill() } catch { }
        throw "Isolated controller timed out"
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $exitCode = $process.ExitCode
    $process.Dispose()
    return [pscustomobject]@{
        ExitCode = $exitCode
        Text = (@($stdout, $stderr) | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        }) -join [Environment]::NewLine
    }
}

function Get-TestClientProcesses {
    $prefix = $testRoot.TrimEnd('\') + '\'
    foreach ($row in @(Get-CimInstance Win32_Process -Filter "Name='$mockClientName'" `
            -ErrorAction SilentlyContinue)) {
        $path = [string]$row.ExecutablePath
        $process = $null
        $startFileTime = [long]0
        try {
            $process = [Diagnostics.Process]::GetProcessById([int]$row.ProcessId)
            if ([string]::IsNullOrWhiteSpace($path)) {
                $path = [string]$process.Path
            }
            $path = [IO.Path]::GetFullPath($path)
            $startFileTime = $process.StartTime.ToUniversalTime().ToFileTimeUtc()
        } catch {
            $path = ""
            $startFileTime = 0
        } finally {
            if ($null -ne $process) { $process.Dispose() }
        }
        if ($path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            [pscustomobject]@{
                ProcessId = [int]$row.ProcessId
                ExecutablePath = $path
                ProcessStartFileTimeUtc = $startFileTime
            }
        }
    }
}

function Stop-TestClients {
    foreach ($row in @(Get-TestClientProcesses)) {
        $process = $null
        try {
            $process = [Diagnostics.Process]::GetProcessById([int]$row.ProcessId)
            $actualPath = [IO.Path]::GetFullPath([string]$process.Path)
            $actualStart = $process.StartTime.ToUniversalTime().ToFileTimeUtc()
            if ($actualStart -ne $row.ProcessStartFileTimeUtc -or
                -not $actualPath.Equals(
                    $row.ExecutablePath, [StringComparison]::OrdinalIgnoreCase) -or
                -not $actualPath.Equals(
                    $mockClient, [StringComparison]::OrdinalIgnoreCase)) {
                Write-Warning "Mock cleanup skipped a changed process identity"
                continue
            }
            $process.Kill()
            [void]$process.WaitForExit(5000)
        } catch {
            Write-Warning "Mock-process cleanup failed: $($_.Exception.Message)"
        } finally {
            if ($null -ne $process) { $process.Dispose() }
        }
    }
}

try {
    New-Item -ItemType Directory -Path (Split-Path -Parent $controller),
        (Split-Path -Parent $mockSteam), (Split-Path -Parent $mockClient) `
        -Force | Out-Null
    Copy-Item -LiteralPath $sourceController -Destination $controller
    $controllerText = [IO.File]::ReadAllText($controller)
    $productionDeclaration = '$clientProcessName = "VNGame.exe"'
    $testDeclaration = '$clientProcessName = "' + $mockClientName + '"'
    if ($controllerText.IndexOf($productionDeclaration, [StringComparison]::Ordinal) -lt 0) {
        throw "Could not find the client-process declaration in the controller fixture"
    }
    $controllerText = $controllerText.Replace($productionDeclaration, $testDeclaration)
    $productionRecoverySuffix =
        '$clientRecoveryPathSuffix = "\steamapps\common\Rising Storm 2\Binaries\Win64\VNGame.exe"'
    $testRecoverySuffix = '$clientRecoveryPathSuffix = "\mock game\' +
        $mockClientName + '"'
    if ($controllerText.IndexOf(
            $productionRecoverySuffix, [StringComparison]::Ordinal) -lt 0) {
        throw "Could not find the recovery-path declaration in the controller fixture"
    }
    $controllerText = $controllerText.Replace(
        $productionRecoverySuffix, $testRecoverySuffix)
    [IO.File]::WriteAllText($controller, $controllerText, $utf8NoBom)

    $steamSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;

public static class MockSteam
{
    public static int Main(string[] args)
    {
        string game = Environment.GetEnvironmentVariable("RS2V_TEST_CLIENT_EXE");
        if (String.IsNullOrEmpty(game))
            return 31;
        bool stripToken = Environment.GetEnvironmentVariable(
            "RS2V_TEST_STRIP_TOKEN") == "1";
        bool quoteToken = Environment.GetEnvironmentVariable(
            "RS2V_TEST_QUOTE_TOKEN") == "1";
        int delayMs;
        if (Int32.TryParse(Environment.GetEnvironmentVariable(
                "RS2V_TEST_LAUNCH_DELAY_MS"), out delayMs) && delayMs > 0)
            System.Threading.Thread.Sleep(delayMs);
        var forwarded = new List<string>();
        foreach (string arg in args)
        {
            bool isToken = arg.StartsWith(
                "-RS2VClientControl=", StringComparison.OrdinalIgnoreCase);
            if (stripToken && isToken)
                continue;
            forwarded.Add(quoteToken && isToken ? "\"" + arg + "\"" : arg);
        }
        ProcessStartInfo info = new ProcessStartInfo();
        info.FileName = game;
        info.Arguments = String.Join(" ", forwarded.ToArray());
        info.UseShellExecute = false;
        Process.Start(info);
        return 0;
    }
}
'@
    $clientSource = @'
using System;
using System.IO;
using System.Text;
using System.Threading;

public static class MockClient
{
    public static int Main(string[] args)
    {
        string capture = Environment.GetEnvironmentVariable("RS2V_TEST_CLIENT_ARGS");
        if (!String.IsNullOrEmpty(capture))
            File.WriteAllLines(capture, args, new UTF8Encoding(false));
        while (true)
            Thread.Sleep(1000);
    }
}
'@
    Add-Type -TypeDefinition $steamSource -Language CSharp -OutputType ConsoleApplication `
        -OutputAssembly $mockSteam
    Add-Type -TypeDefinition $clientSource -Language CSharp -OutputType ConsoleApplication `
        -OutputAssembly $mockClient
    $env:RS2V_TEST_CLIENT_EXE = $mockClient
    $env:RS2V_TEST_CLIENT_ARGS = $clientArgsCapture
    $env:RS2V_TEST_QUOTE_TOKEN = "1"

    $start = Invoke-Controller "Start" @(
        "-SteamPath", $mockSteam, "-LaunchTimeoutSeconds", "10")
    Assert-True ($start.ExitCode -eq 0) "Mock Steam start failed: $($start.Text)"
    Assert-True ($start.Text -match 'client started pid=\d+ via=steam appid=418460') `
        "Start output did not identify the Steam launch: $($start.Text)"
    $statePath = Join-Path $testRoot ".client-control\state.json"
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    $mockPid = [int]$state.pid
    Assert-True ($null -ne (Get-Process -Id $mockPid -ErrorAction SilentlyContinue)) `
        "Recorded mock client is not running"
    Assert-True ([string]$state.executable -eq $mockClient) `
        "State did not record the exact mock client path"
    Assert-True ([Guid]::Parse([string]$state.launch_token) -ne [Guid]::Empty) `
        "State did not record a launch token"

    $stateBytes = [IO.File]::ReadAllBytes($statePath)
    $hasBom = $stateBytes.Length -ge 3 -and $stateBytes[0] -eq 0xEF -and
        $stateBytes[1] -eq 0xBB -and $stateBytes[2] -eq 0xBF
    Assert-True (-not $hasBom) "State JSON must be UTF-8 without a BOM"

    $status = Invoke-Controller "Status"
    Assert-True ($status.ExitCode -eq 0 -and $status.Text -match
        "client=running state=managed pid=$mockPid") `
        "Managed status was not deterministic: $($status.Text)"

    $secondStart = Invoke-Controller "Start" @("-SteamPath", $mockSteam)
    Assert-True ($secondStart.ExitCode -eq 0 -and
        $secondStart.Text -match "client already running pid=$mockPid") `
        "Repeated Start was not idempotent: $($secondStart.Text)"
    Assert-True (@(Get-TestClientProcesses).Count -eq 1) `
        "Repeated Start launched a duplicate client"

    $stop = Invoke-Controller "Stop"
    Assert-True ($stop.ExitCode -eq 0 -and
        $stop.Text -match "client stopped pid=$mockPid state=managed") `
        "Managed Stop failed: $($stop.Text)"
    Assert-True (@(Get-TestClientProcesses).Count -eq 0) `
        "Managed Stop left the mock client running"

    $env:RS2V_TEST_LAUNCH_DELAY_MS = "6000"
    try {
        $lateTimer = [Diagnostics.Stopwatch]::StartNew()
        $lateStart = Invoke-Controller "Start" @(
            "-SteamPath", $mockSteam, "-LaunchTimeoutSeconds", "5",
            "-LateLaunchGraceSeconds", "3")
        $lateTimer.Stop()
        Assert-True ($lateStart.ExitCode -eq 0) `
            "Late Steam launch was not recovered inside the bounded grace: $($lateStart.Text)"
        Assert-True ($lateTimer.Elapsed.TotalSeconds -ge 5 -and
            $lateTimer.Elapsed.TotalSeconds -lt 12) `
            "Late-launch grace timing was outside the expected window: $($lateTimer.Elapsed)"
        $lateStop = Invoke-Controller "Stop"
        Assert-True ($lateStop.ExitCode -eq 0) `
            "Late-launch mock client did not stop: $($lateStop.Text)"
    } finally {
        Remove-Item Env:\RS2V_TEST_LAUNCH_DELAY_MS -ErrorAction SilentlyContinue
        Stop-TestClients
    }

    Remove-Item -LiteralPath $clientArgsCapture -Force -ErrorAction SilentlyContinue
    $connect = Invoke-Controller "Connect" @(
        "-SteamPath", $mockSteam, "-ServerAddress", "127.0.0.1",
        "-ServerPort", "17777", "-LaunchTimeoutSeconds", "10")
    Assert-True ($connect.ExitCode -eq 0 -and
        $connect.Text -match 'connect=127\.0\.0\.1:17777') `
        "Connect did not launch through Steam: $($connect.Text)"
    $captured = @(Get-Content -LiteralPath $clientArgsCapture)
    Assert-True ($captured.Count -eq 4) `
        "Mock client received the wrong argument count: $($captured -join ', ')"
    Assert-True ($captured[0] -eq "-applaunch" -and $captured[1] -eq "418460") `
        "Steam app launch arguments were malformed: $($captured -join ', ')"
    Assert-True ($captured[2] -eq "127.0.0.1:17777") `
        "Direct-connect endpoint was not passed as the UE3 URL argument"
    Assert-True ($captured[3] -match '^-RS2VClientControl=[0-9a-f-]{36}$') `
        "Launch ownership token was not preserved: $($captured[3])"
    $connectStop = Invoke-Controller "Stop"
    Assert-True ($connectStop.ExitCode -eq 0) `
        "Connected mock client did not stop: $($connectStop.Text)"

    $remoteRejected = Invoke-Controller "Connect" @(
        "-SteamPath", $mockSteam, "-ServerAddress", "192.0.2.1",
        "-LaunchTimeoutSeconds", "10")
    Assert-True ($remoteRejected.ExitCode -ne 0 -and
        $remoteRejected.Text -match 'requires -AllowRemoteAddress') `
        "Remote endpoint was not rejected by default: $($remoteRejected.Text)"
    Assert-True (@(Get-TestClientProcesses).Count -eq 0) `
        "Rejected remote endpoint launched a client"

    $env:RS2V_TEST_STRIP_TOKEN = "1"
    try {
        $unverifiableTimer = [Diagnostics.Stopwatch]::StartNew()
        $unverifiable = Invoke-Controller "Start" @(
            "-SteamPath", $mockSteam, "-LaunchTimeoutSeconds", "10")
        $unverifiableTimer.Stop()
        Assert-True ($unverifiable.ExitCode -ne 0 -and
            $unverifiable.Text -match 'exact launch-token argument is absent') `
            "Tokenless client was not classified explicitly: $($unverifiable.Text)"
        Assert-True ($unverifiableTimer.Elapsed.TotalSeconds -lt 8) `
            "Tokenless client consumed the full launch timeout: $($unverifiableTimer.Elapsed)"
        Assert-True (@(Get-TestClientProcesses).Count -eq 1) `
            "Unverifiable launch should leave exactly one unowned mock client"
        $tokenlessRecovery = Invoke-Controller "Recover"
        Assert-True ($tokenlessRecovery.ExitCode -ne 0 -and
            $tokenlessRecovery.Text -match 'no exact controller launch-token argument') `
            "Recover accepted a tokenless client: $($tokenlessRecovery.Text)"
        Assert-True (@(Get-TestClientProcesses).Count -eq 1) `
            "Rejected recovery mutated the tokenless client"
    } finally {
        Remove-Item Env:\RS2V_TEST_STRIP_TOKEN -ErrorAction SilentlyContinue
        Stop-TestClients
    }

    $recoveryToken = [Guid]::NewGuid()
    $recoveryArgument = "-RS2VClientControl=$($recoveryToken.ToString('D'))"
    $recoverable = Start-Process -FilePath $mockClient `
        -ArgumentList $recoveryArgument -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while (@(Get-TestClientProcesses).Count -eq 0 -and
               [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 50
        }
        $recovery = Invoke-Controller "Recover"
        Assert-True ($recovery.ExitCode -eq 0 -and
            $recovery.Text -match "client recovered pid=$($recoverable.Id) state=managed") `
            "Tagged client recovery failed: $($recovery.Text)"
        $recoveredState = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        Assert-True ([int]$recoveredState.pid -eq $recoverable.Id -and
            [Guid]::Parse([string]$recoveredState.launch_token) -eq $recoveryToken) `
            "Recovery published the wrong process identity or token"
        $recoveryStop = Invoke-Controller "Stop"
        Assert-True ($recoveryStop.ExitCode -eq 0) `
            "Recovered mock client did not stop: $($recoveryStop.Text)"
        Assert-True (@(Get-TestClientProcesses).Count -eq 0) `
            "Recovered client remained alive after managed Stop"
    } finally {
        if (-not $recoverable.HasExited) {
            $recoverable.Kill()
            [void]$recoverable.WaitForExit(5000)
        }
        $recoverable.Dispose()
        Stop-TestClients
    }

    $unmanaged = Start-Process -FilePath $mockClient -ArgumentList "-unmanaged" -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while (@(Get-TestClientProcesses).Count -eq 0 -and
               [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 50
        }
        $unmanagedStop = Invoke-Controller "Stop"
        Assert-True ($unmanagedStop.ExitCode -ne 0 -and
            $unmanagedStop.Text -match 'without controller state; refusing to stop') `
            "Unmanaged client Stop did not fail closed: $($unmanagedStop.Text)"
        Assert-True (-not $unmanaged.HasExited) `
            "Controller terminated an unmanaged client"
    } finally {
        if (-not $unmanaged.HasExited) {
            $unmanaged.Kill()
            [void]$unmanaged.WaitForExit(5000)
        }
        $unmanaged.Dispose()
    }

    $mismatchStart = Invoke-Controller "Start" @(
        "-SteamPath", $mockSteam, "-LaunchTimeoutSeconds", "10")
    Assert-True ($mismatchStart.ExitCode -eq 0) `
        "Identity-mismatch fixture failed to start: $($mismatchStart.Text)"
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    $mismatchPid = [int]$state.pid
    $state.process_start_filetime_utc = "1"
    [IO.File]::WriteAllText(
        $statePath, (($state | ConvertTo-Json -Depth 3) + "`n"), $utf8NoBom)
    $mismatchStop = Invoke-Controller "Stop"
    Assert-True ($mismatchStop.ExitCode -ne 0 -and
        $mismatchStop.Text -match 'Refusing to stop an unverified client') `
        "Identity mismatch did not fail closed: $($mismatchStop.Text)"
    Assert-True ($null -ne (Get-Process -Id $mismatchPid -ErrorAction SilentlyContinue)) `
        "Identity mismatch terminated the live mock client"

    Write-Output "client_control smoke tests passed: $passed assertions"
} finally {
    Remove-Item Env:\RS2V_TEST_CLIENT_EXE -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_TEST_CLIENT_ARGS -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_TEST_QUOTE_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_TEST_STRIP_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_TEST_LAUNCH_DELAY_MS -ErrorAction SilentlyContinue
    Stop-TestClients

    foreach ($path in @($testRoot)) {
        if (Test-Path -LiteralPath $path) {
            $fullPath = [IO.Path]::GetFullPath($path)
            $prefix = $tempBase.TrimEnd('\') + '\'
            if (-not $fullPath.StartsWith(
                    $prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to clean test path outside the temp directory: $fullPath"
            }
            Remove-Item -LiteralPath $fullPath -Recurse -Force
        }
    }
}
