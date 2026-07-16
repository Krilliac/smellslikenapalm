[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$sourceController = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\server_control.ps1"))
$powerShellExe = Join-Path $PSHOME "powershell.exe"
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $tempBase ("rs2v server control smoke {0}" -f [Guid]::NewGuid().ToString("N"))
$outsideRoot = Join-Path $tempBase ("rs2v server control outside {0}" -f [Guid]::NewGuid().ToString("N"))
$controller = Join-Path $testRoot "tools\server_control.ps1"
$configPath = Join-Path $testRoot "config\server.ini"
$mockBinary = Join-Path $testRoot "bin\rs2v_server.exe"
$argumentCapture = Join-Path $testRoot "mock arguments.txt"
$bootstrapCapture = Join-Path $testRoot "mock bootstrap environment.txt"
$bootstrapEnvironmentName = "RS2V_REPLICATION_BOOTSTRAP_VARIANT"
$previousBootstrapEnvironment = [Environment]::GetEnvironmentVariable(
    $bootstrapEnvironmentName, [System.EnvironmentVariableTarget]::Process)
$passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
    $script:passed++
}

function Write-TestStage([string]$Stage) {
    [IO.File]::AppendAllText(
        (Join-Path $testRoot "progress.txt"),
        ((Get-Date -Format "HH:mm:ss.fff") + " " + $Stage + [Environment]::NewLine),
        $utf8NoBom)
}

function ConvertTo-TestCommandLine([string[]]$Arguments) {
    return (($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') {
            # Test paths cannot contain a literal quote or end in a directory
            # separator, so simple Windows quoting is sufficient here.
            '"' + $_.Replace('"', '\"') + '"'
        } else {
            $_
        }
    }) -join ' ')
}

function Start-ControllerInvocation([string]$Action,
                                    [string[]]$ExtraArguments = @()) {
    $arguments = @("-NoProfile", "-File", $controller, "-Action", $Action)
    if ($ExtraArguments -notcontains "-Binary") {
        $arguments += @("-Binary", $mockBinary)
    }
    if ($ExtraArguments -notcontains "-Config") {
        $arguments += @("-Config", $configPath)
    }
    if ($ExtraArguments -notcontains "-WaitSeconds") {
        $arguments += @("-WaitSeconds", "5")
    }
    $arguments += $ExtraArguments
    $captureId = [Guid]::NewGuid().ToString("N")
    $stdoutCapture = Join-Path $testRoot "controller_${captureId}.stdout.txt"
    $stderrCapture = Join-Path $testRoot "controller_${captureId}.stderr.txt"
    $process = Start-Process -FilePath $powerShellExe `
        -ArgumentList (ConvertTo-TestCommandLine $arguments) `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutCapture `
        -RedirectStandardError $stderrCapture `
        -PassThru
    # PowerShell 5.1 can expose a null ExitCode after redirected launch unless
    # the Process handle is opened before the child exits.
    $null = $process.Handle
    return [pscustomobject]@{
        Process = $process
        Stdout = $stdoutCapture
        Stderr = $stderrCapture
    }
}

function Read-SharedText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        try {
            return $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Complete-ControllerInvocation([object]$Invocation, [int]$TimeoutSeconds = 30) {
    if (-not $Invocation.Process.WaitForExit($TimeoutSeconds * 1000)) {
        try { $Invocation.Process.Kill() } catch { }
        throw "Controller invocation did not finish within $TimeoutSeconds seconds"
    }
    $Invocation.Process.Refresh()
    $exitCode = [int]$Invocation.Process.ExitCode
    $Invocation.Process.Dispose()
    $stdout = Read-SharedText $Invocation.Stdout
    $stderr = Read-SharedText $Invocation.Stderr
    $text = @($stdout, $stderr) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $text
        Text = $text -join [Environment]::NewLine
    }
}

function Invoke-Controller([string]$Action, [string[]]$ExtraArguments = @()) {
    return Complete-ControllerInvocation (
        Start-ControllerInvocation $Action $ExtraArguments)
}

function Get-TestState {
    return (Get-Content -LiteralPath (Join-Path $testRoot ".server-control\state.json") -Raw |
        ConvertFrom-Json)
}

function Stop-TestProcesses {
    foreach ($process in @(Get-Process -Name "rs2v_server" -ErrorAction SilentlyContinue)) {
        try {
            $path = [IO.Path]::GetFullPath([string]$process.Path)
            $prefix = $testRoot.TrimEnd('\') + '\'
            if ($path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $process.Kill()
                $process.WaitForExit(5000) | Out-Null
            }
        } catch {
            Write-Warning "Mock-process cleanup failed: $($_.Exception.Message)"
        } finally {
            $process.Dispose()
        }
    }
}

try {
    New-Item -ItemType Directory -Path (Split-Path -Parent $controller) -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $configPath) -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $mockBinary) -Force | Out-Null
    New-Item -ItemType Directory -Path $outsideRoot -Force | Out-Null
    Copy-Item -LiteralPath $sourceController -Destination $controller
    [IO.File]::WriteAllText($configPath, "[server]`n", $utf8NoBom)

    $mockSource = @'
using System;
using System.IO;
using System.Text;
using System.Threading;

public static class MockServer
{
    public static int Main(string[] args)
    {
        string capture = Environment.GetEnvironmentVariable("RS2V_TEST_ARGS_FILE");
        if (!String.IsNullOrEmpty(capture))
            File.WriteAllLines(capture, args, new UTF8Encoding(false));

        string bootstrapCapture = Environment.GetEnvironmentVariable(
            "RS2V_TEST_BOOTSTRAP_FILE");
        if (!String.IsNullOrEmpty(bootstrapCapture))
        {
            string bootstrap = Environment.GetEnvironmentVariable(
                "RS2V_REPLICATION_BOOTSTRAP_VARIANT");
            File.WriteAllText(
                bootstrapCapture, bootstrap ?? "<missing>",
                new UTF8Encoding(false));
        }

        int exitCode;
        if (Int32.TryParse(Environment.GetEnvironmentVariable("RS2V_TEST_EXIT_CODE"), out exitCode))
            return exitCode;

        while (true)
            Thread.Sleep(1000);
    }
}
'@
    Add-Type -TypeDefinition $mockSource -Language CSharp -OutputType ConsoleApplication `
        -OutputAssembly $mockBinary
    Copy-Item -LiteralPath $mockBinary -Destination (Join-Path $outsideRoot "rs2v_server.exe")

    $env:RS2V_TEST_ARGS_FILE = $argumentCapture
    $env:RS2V_TEST_BOOTSTRAP_FILE = $bootstrapCapture
    [Environment]::SetEnvironmentVariable(
        $bootstrapEnvironmentName, "parent-sentinel",
        [System.EnvironmentVariableTarget]::Process)
    Remove-Item Env:\RS2V_TEST_EXIT_CODE -ErrorAction SilentlyContinue

    Write-TestStage "outside-path"
    $outside = Invoke-Controller "Start" @("-Binary", (Join-Path $outsideRoot "rs2v_server.exe"))
    Assert-True ($outside.ExitCode -ne 0) `
        "an outside-repository binary must be rejected (exit=$($outside.ExitCode)): $($outside.Text)"

    Write-TestStage "junction-path"
    $junctionPath = Join-Path $testRoot "bin\escape"
    New-Item -ItemType Junction -Path $junctionPath -Target $outsideRoot | Out-Null
    try {
        $reparse = Invoke-Controller "Start" @(
            "-Binary", (Join-Path $junctionPath "rs2v_server.exe"))
        Assert-True ($reparse.ExitCode -ne 0) "a junction escape must be rejected"
        Assert-True ($reparse.Text -match "reparse-point") `
            "junction rejection should be explicit: $($reparse.Text)"
    } finally {
        if (Test-Path -LiteralPath $junctionPath) {
            # Windows PowerShell 5.1 can throw NullReferenceException when its
            # Remove-Item provider removes a directory junction.
            [IO.Directory]::Delete($junctionPath)
        }
    }

    Write-TestStage "initial-start"
    $start = Invoke-Controller "Start"
    Assert-True ($start.ExitCode -eq 0) `
        "mock server should start (exit=$($start.ExitCode)): $($start.Text)"
    Write-TestStage "initial-state-assertions"
    $state = Get-TestState
    $mockPid = [int]$state.pid
    Assert-True ($null -ne (Get-Process -Id $mockPid -ErrorAction SilentlyContinue)) `
        "recorded mock process must be alive"
    Assert-True ([string]$state.executable -eq $mockBinary) "state must record the exact executable"
    Assert-True ([string]$state.config -eq $configPath) "state must record the exact config path"

    $stateBytes = [IO.File]::ReadAllBytes((Join-Path $testRoot ".server-control\state.json"))
    $hasBom = $stateBytes.Length -ge 3 -and $stateBytes[0] -eq 0xEF -and
        $stateBytes[1] -eq 0xBB -and $stateBytes[2] -eq 0xBF
    Assert-True (-not $hasBom) "state JSON must be UTF-8 without a BOM"

    Write-TestStage "argument-assertions"
    $capturedArguments = @(Get-Content -LiteralPath $argumentCapture)
    Assert-True ($capturedArguments.Count -eq 2) "mock should receive exactly two arguments"
    Assert-True ($capturedArguments[0] -eq "--config") "first mock argument must be --config"
    Assert-True ($capturedArguments[1] -eq $configPath) `
        "config path containing spaces must remain one argument"
    Assert-True ((Get-Content -LiteralPath $bootstrapCapture -Raw) -eq "installed") `
        "default Start must expose the installed bootstrap variant to the child"

    # A concurrent Ensure/Stop pair must always converge on stopped+disabled,
    # regardless of which operation acquires the cross-process mutex first.
    $ensureInvocation = Start-ControllerInvocation "Ensure"
    $stopInvocation = Start-ControllerInvocation "Stop"
    Write-TestStage "concurrent-processes-wait"
    $jobResults = @(
        Complete-ControllerInvocation $ensureInvocation 20
        Complete-ControllerInvocation $stopInvocation 20
    )
    Write-TestStage "concurrent-processes-done"
    Assert-True (@($jobResults | Where-Object { $_.ExitCode -ne 0 }).Count -eq 0) `
        "concurrent Ensure/Stop operations must both complete safely"
    Assert-True (Test-Path -LiteralPath (Join-Path $testRoot ".server-control\disabled")) `
        "explicit Stop must leave the sentinel"
    Assert-True ($null -eq (Get-Process -Id $mockPid -ErrorAction SilentlyContinue)) `
        "concurrent Stop must terminate the managed mock"

    Write-TestStage "canonical-bootstrap-start"
    $canonicalStart = Invoke-Controller "Start" @(
        "-ReplicationBootstrapVariant", "canonical")
    Assert-True ($canonicalStart.ExitCode -eq 0) `
        "canonical bootstrap mock server should start: $($canonicalStart.Text)"
    Assert-True ((Get-Content -LiteralPath $bootstrapCapture -Raw) -eq "<missing>") `
        "canonical Start must omit the bootstrap environment variable from the child"
    $canonicalState = Get-TestState
    $canonicalPid = [int]$canonicalState.pid
    $canonicalStop = Invoke-Controller "Stop"
    Assert-True ($canonicalStop.ExitCode -eq 0) `
        "canonical bootstrap mock server should stop cleanly: $($canonicalStop.Text)"
    Assert-True ($null -eq (Get-Process -Id $canonicalPid -ErrorAction SilentlyContinue)) `
        "canonical bootstrap Stop must terminate the managed mock"

    $suppressed = Invoke-Controller "Ensure"
    Assert-True ($suppressed.ExitCode -eq 0) "suppressed Ensure should be a successful no-op"
    Assert-True ($suppressed.Text -match "suppressed by explicit-stop sentinel") `
        "suppressed Ensure should explain why it did not start"

    $restartByStart = Invoke-Controller "Start"
    Assert-True ($restartByStart.ExitCode -eq 0) "manual Start should re-enable Ensure"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $testRoot ".server-control\disabled"))) `
        "manual Start must clear the sentinel"
    $state = Get-TestState
    $mockPid = [int]$state.pid
    Assert-True ((Get-Content -LiteralPath $bootstrapCapture -Raw) -eq "installed") `
        "a later default Start must restore the installed child bootstrap variant"

    # Corrupt just the creation identity. Stop must write the sentinel but leave the
    # now-unproven process untouched, demonstrating PID-reuse protection.
    $state.process_start_filetime_utc = "1"
    [IO.File]::WriteAllText(
        (Join-Path $testRoot ".server-control\state.json"),
        ($state | ConvertTo-Json -Depth 3), $utf8NoBom)
    $mismatchStop = Invoke-Controller "Stop"
    Assert-True ($mismatchStop.ExitCode -ne 0) "identity mismatch must fail closed"
    Assert-True ($null -ne (Get-Process -Id $mockPid -ErrorAction SilentlyContinue)) `
        "identity mismatch must not terminate the process"
    Assert-True (Test-Path -LiteralPath (Join-Path $testRoot ".server-control\disabled")) `
        "failed explicit Stop must still suppress Ensure"
    Stop-TestProcesses

    # With no candidate server, malformed state can be quarantined safely. Seed more
    # than 20 paired archives and verify the next Start retains complete session sets.
    [IO.File]::WriteAllText(
        (Join-Path $testRoot ".server-control\state.json"), "{not json", $utf8NoBom)
    $archiveRoot = Join-Path $testRoot ".server-control\logs"
    [IO.File]::WriteAllText(
        (Join-Path $testRoot ".server-control\server.stdout.log"), "active", $utf8NoBom)
    [IO.File]::WriteAllText(
        (Join-Path $testRoot ".server-control\server.stderr.log"), "", $utf8NoBom)
    0..21 | ForEach-Object {
        $stamp = "20260101_0000{0:D2}_000" -f $_
        [IO.File]::WriteAllText((Join-Path $archiveRoot "server_${stamp}.stdout.log"), "out", $utf8NoBom)
        [IO.File]::WriteAllText((Join-Path $archiveRoot "server_${stamp}.stderr.log"), "err", $utf8NoBom)
    }
    $recovered = Invoke-Controller "Start"
    Assert-True ($recovered.ExitCode -eq 0) "corrupt state without a candidate may be quarantined"
    $sets = @(Get-ChildItem -LiteralPath $archiveRoot -File |
        Where-Object { $_.Name -match '^server_(\d{8}_\d{6}_\d{3})\.(stdout|stderr)\.log$' } |
        Group-Object { [regex]::Match($_.Name, '^server_(\d{8}_\d{6}_\d{3})\.').Groups[1].Value })
    Assert-True ($sets.Count -eq 20) "log rotation must retain exactly 20 session sets"
    Assert-True (@($sets | Where-Object { $_.Count -ne 2 }).Count -eq 0) `
        "log rotation must not split stdout/stderr pairs"
    $state = Get-TestState
    $mockPid = [int]$state.pid
    $finalStop = Invoke-Controller "Stop"
    Assert-True ($finalStop.ExitCode -eq 0) "final managed Stop should succeed"

    # Terminate a helper while the controller is already waiting on the helper-owned
    # mutex; the waiter should receive and recover AbandonedMutexException.
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $rootBytes = [Text.Encoding]::UTF8.GetBytes(
            ([IO.Path]::GetFullPath($testRoot)).ToUpperInvariant())
        $hash = ([BitConverter]::ToString($sha256.ComputeHash($rootBytes))).Replace("-", "").Substring(0, 24)
    } finally {
        $sha256.Dispose()
    }
    $mutexName = "Global\RS2VServerControl_$hash"
    $mutexOwnerScript = Join-Path $testRoot "abandon mutex owner.ps1"
    $mutexReady = Join-Path $testRoot "abandon mutex ready.txt"
    [IO.File]::WriteAllText($mutexOwnerScript, @'
$ErrorActionPreference = "Stop"
$mutex = [Threading.Mutex]::new($false, $env:RS2V_TEST_MUTEX_NAME)
[void]$mutex.WaitOne()
[IO.File]::WriteAllText($env:RS2V_TEST_MUTEX_READY, $env:RS2V_TEST_MUTEX_NAME)
Start-Sleep -Seconds 60
'@, $utf8NoBom)
    $env:RS2V_TEST_MUTEX_NAME = $mutexName
    $env:RS2V_TEST_MUTEX_READY = $mutexReady
    $owner = Start-Process -FilePath $powerShellExe `
        -ArgumentList (ConvertTo-TestCommandLine @(
            "-NoProfile", "-File", $mutexOwnerScript)) `
        -WindowStyle Hidden -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while (-not (Test-Path -LiteralPath $mutexReady) -and
               [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 25
        }
        Assert-True (Test-Path -LiteralPath $mutexReady) `
            "mutex-abandonment helper should acquire the mutex"
        Assert-True ((Get-Content -LiteralPath $mutexReady -Raw) -eq $mutexName) `
            "mutex-abandonment helper must use the expected mutex name"
        Assert-True (-not $owner.HasExited) `
            "mutex-abandonment helper must still own the mutex"
        # Ensure is a mutex-protected lifecycle action. The sentinel left by the
        # final Stop makes it a non-mutating no-op after lock acquisition.
        $waitingController = Start-ControllerInvocation "Ensure"
        Start-Sleep -Milliseconds 300
        Assert-True (-not $waitingController.Process.HasExited) `
            "controller must wait while another process owns its mutex"
        $owner.Kill()
        [void]$owner.WaitForExit(5000)
        $abandoned = Complete-ControllerInvocation $waitingController 10
        Assert-True ($abandoned.ExitCode -eq 0) "controller must recover an abandoned mutex"
        Assert-True ($abandoned.Text -match "suppressed by explicit-stop sentinel") `
            "controller should complete the waiting action after owner termination"
    } finally {
        try {
            if (-not $owner.HasExited) {
                $owner.Kill()
                [void]$owner.WaitForExit(5000)
            }
        } catch { }
        $owner.Dispose()
        Remove-Item Env:\RS2V_TEST_MUTEX_NAME -ErrorAction SilentlyContinue
        Remove-Item Env:\RS2V_TEST_MUTEX_READY -ErrorAction SilentlyContinue
    }

    # A child that exits during the startup probe must never leave state or an orphan.
    Remove-Item -LiteralPath (Join-Path $testRoot ".server-control\disabled") `
        -Force -ErrorAction SilentlyContinue
    $env:RS2V_TEST_EXIT_CODE = "23"
    $failedStart = Invoke-Controller "Start"
    Assert-True ($failedStart.ExitCode -ne 0) "early child exit must fail Start"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $testRoot ".server-control\state.json"))) `
        "failed Start must not publish managed state"
    Assert-True (Test-Path -LiteralPath (Join-Path $testRoot ".server-control\disabled") `
        -PathType Leaf) "failed Start must restore the explicit-stop sentinel"
    Assert-True (@(Get-Process -Name "rs2v_server" -ErrorAction SilentlyContinue |
        Where-Object { ([string]$_.Path).StartsWith($testRoot, [StringComparison]::OrdinalIgnoreCase) }).Count -eq 0) `
        "failed Start must not leave an orphaned mock process"

    Write-Output "server_control smoke tests passed: $passed assertions"
} finally {
    Remove-Item Env:\RS2V_TEST_ARGS_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_TEST_BOOTSTRAP_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:\RS2V_TEST_EXIT_CODE -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable(
        $bootstrapEnvironmentName, $previousBootstrapEnvironment,
        [System.EnvironmentVariableTarget]::Process)
    Stop-TestProcesses

    foreach ($path in @($testRoot, $outsideRoot)) {
        if (Test-Path -LiteralPath $path) {
            $fullPath = [IO.Path]::GetFullPath($path)
            if (-not $fullPath.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to clean test path outside the temp directory: $fullPath"
            }
            Remove-Item -LiteralPath $fullPath -Recurse -Force
        }
    }
}
