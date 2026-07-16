[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

try {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $rootCMakePath = Join-Path $repoRoot 'CMakeLists.txt'
    $testsCMakePath = Join-Path $repoRoot 'tests\CMakeLists.txt'
    $rootCMake = [System.IO.File]::ReadAllText($rootCMakePath)
    $testsCMake = [System.IO.File]::ReadAllText($testsCMakePath)

    $trackedCMake = @(
        & git -C $repoRoot ls-files -- `
            'CMakeLists.txt' `
            ':(glob)**/CMakeLists.txt' `
            ':(glob)**/*.cmake'
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique
    Assert-True ($LASTEXITCODE -eq 0) `
        'git ls-files failed while inventorying source-controlled CMake files'
    Assert-True ($trackedCMake.Count -gt 0) `
        'no source-controlled CMake files were found'

    $globPattern = '(?is)\bfile\s*\(\s*GLOB(?:_RECURSE)?\b.*?\)'
    $globCommands = @()
    foreach ($relativePath in $trackedCMake) {
        $absolutePath = Join-Path $repoRoot $relativePath
        $content = [System.IO.File]::ReadAllText($absolutePath)
        foreach ($match in [regex]::Matches($content, $globPattern)) {
            $globCommands += [pscustomobject]@{
                File = $relativePath
                Text = $match.Value
            }
        }
    }

    Assert-True ($globCommands.Count -gt 0) `
        'expected at least one source-discovery glob'
    foreach ($command in $globCommands) {
        Assert-True ($command.Text -match '(?i)\bCONFIGURE_DEPENDS\b') `
            ("source glob lacks CONFIGURE_DEPENDS in {0}: {1}" -f `
                $command.File, ($command.Text -replace '\s+', ' '))
    }

    foreach ($variable in @(
        'CORE_SOURCES', 'SCRIPTING_SOURCES', 'TELEMETRY_SOURCES')) {
        $expected = '(?is)file\s*\(\s*GLOB_RECURSE\s+' +
            [regex]::Escape($variable) + '\s+CONFIGURE_DEPENDS\b'
        Assert-True ($rootCMake -match $expected) `
            ("{0} must use GLOB_RECURSE CONFIGURE_DEPENDS" -f $variable)
    }

    Assert-True ($rootCMake -match '(?i)src/Physics/\*\.cpp') `
        'CORE_SOURCES must continue covering newly added Physics sources'
    Assert-True ($rootCMake -match `
        '(?is)add_library\s*\(\s*rs2v_core\s+STATIC\s+\$\{CORE_SOURCES\}') `
        'rs2v_core must continue consuming CORE_SOURCES'

    # Test translation units intentionally remain an explicit allow-list. A
    # broad test glob would silently compile known-incompatible legacy suites.
    Assert-True ($testsCMake -notmatch $globPattern) `
        'tests/CMakeLists.txt must remain an explicit allow-list, not a glob'
    Assert-True ($testsCMake -match `
        '(?i)rs2v_add_test\s*\(\s*MovementValidationTests\s*\)') `
        'MovementValidationTests must remain in the explicit test allow-list'
    Assert-True ($testsCMake -match `
        '(?is)target_link_libraries\s*\(\s*\$\{tgt\}\s+PRIVATE\s+rs2v_core\b') `
        'test targets must continue linking rs2v_core'

    Write-Output (
        'PASS: {0} tracked CMake files; {1} source globs are configure-dependent' `
        -f $trackedCMake.Count, $globCommands.Count)
    exit 0
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}
