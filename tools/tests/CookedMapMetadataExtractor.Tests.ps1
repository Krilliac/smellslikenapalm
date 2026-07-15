[CmdletBinding()]
param(
    [string]$UELibPath = 'D:\RE-Tools\UE-Explorer\Eliot.UELib.dll',
    [string]$MapPath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\VNTE-CampaignStart.roe',
    [string]$RoleMapPath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\CuChi\VNTE-CuChi.roe',
    [string]$CollisionMapPath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\Resort\VNTE-Resort.roe',
    [switch]$RequireIntegration
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$wrapper = Join-Path $repoRoot 'tools\extract_cooked_map_metadata.ps1'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("rs2-map-extractor-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($tempRoot) | Out-Null

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

function Invoke-Extractor {
    param([string]$Executable, [string[]]$Arguments, [string]$Label)
    $stdout = Join-Path $tempRoot ($Label + '.stdout')
    $stderr = Join-Path $tempRoot ($Label + '.stderr')
    # Native stderr is expected in the negative-path tests. PowerShell 5.1
    # promotes it to NativeCommandError under Stop, so relax only this call.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 1> $stdout 2> $stderr
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    return [PSCustomObject]@{
        ExitCode = $exitCode
        Stdout = if (Test-Path -LiteralPath $stdout) { [IO.File]::ReadAllText($stdout) } else { '' }
        Stderr = if (Test-Path -LiteralPath $stderr) { [IO.File]::ReadAllText($stderr) } else { '' }
    }
}

try {
    $buildOutput = @(& $wrapper -BuildOnly -UELibPath $UELibPath)
    Assert-True ($LASTEXITCODE -eq 0) 'wrapper compilation must succeed'
    $executable = [string]($buildOutput | Select-Object -Last 1)
    Assert-True (Test-Path -LiteralPath $executable -PathType Leaf) 'compiled extractor must exist'

    $help = Invoke-Extractor $executable @('--help') 'help'
    Assert-True ($help.ExitCode -eq 0) '--help must exit zero'
    Assert-True ($help.Stdout -match 'bounded, read-only') '--help must state the safety contract'

    $roleSchema = Invoke-Extractor $executable @('--self-test-role-schema') 'role-schema'
    Assert-True ($roleSchema.ExitCode -eq 0) `
        'synthetic exact-role-schema checks must succeed'
    Assert-True ($roleSchema.Stdout -match 'rejects unknown fields and nonzero scalar ArrayIndex') `
        'role-schema self-test must cover unknown fields and nonzero scalar ArrayIndex values'

    $missing = Invoke-Extractor $executable @(
        '--input', (Join-Path $tempRoot 'missing.roe'),
        '--uelib', $UELibPath
    ) 'missing'
    Assert-True ($missing.ExitCode -eq 65) 'missing input must be a validation error'
    Assert-True ($missing.Stderr -match 'does not exist') 'missing input error must be explicit'

    if (-not (Test-Path -LiteralPath $UELibPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $MapPath -PathType Leaf)) {
        if ($RequireIntegration) {
            throw 'Integration inputs are required but UELib or the official map is missing.'
        }
        Write-Output 'SKIP: integration inputs are not installed; CLI/compile checks passed.'
        exit 0
    }

    $beforeHash = (Get-FileHash -LiteralPath $MapPath -Algorithm SHA256).Hash

    $wrapperOutput = Join-Path $tempRoot 'wrapper-classes.jsonl'
    & $wrapper $MapPath -UELibPath $UELibPath -ClassesOnly `
        -OutputPath $wrapperOutput -MaxClasses 3
    Assert-True ($LASTEXITCODE -eq 0) 'runtime wrapper invocation must succeed'
    Assert-True (Test-Path -LiteralPath $wrapperOutput -PathType Leaf) 'wrapper must create requested JSONL output'
    $wrapperRecords = @([IO.File]::ReadAllLines($wrapperOutput) | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True (@($wrapperRecords | Where-Object { $_.record -eq 'class' -and $_.class -eq 'ROPlayerStart' }).Count -eq 1) `
        'default class selection must include ROPlayerStart'

    $tooSmall = Invoke-Extractor $executable @(
        '--input', $MapPath,
        '--uelib', $UELibPath,
        '--max-input-bytes', '1'
    ) 'size-limit'
    Assert-True ($tooSmall.ExitCode -eq 65) 'size limit must reject before package parsing'
    Assert-True ($tooSmall.Stderr -match 'exceeding --max-input-bytes') 'size error must name the violated bound'

    $unsafeBoundsClass = Invoke-Extractor $executable @(
        '--input', $MapPath,
        '--uelib', $UELibPath,
        '--mode', 'brush-bounds',
        '--class-pattern', '.*'
    ) 'unsafe-bounds-class'
    Assert-True ($unsafeBoundsClass.ExitCode -eq 65) 'brush-bounds must reject broad class selection'
    Assert-True ($unsafeBoundsClass.Stderr -match 'exact BlockingVolume') `
        'brush-bounds class rejection must explain its narrow contract'

    $classOutput = Join-Path $tempRoot 'classes.jsonl'
    $classes = Invoke-Extractor $executable @(
        '--input', $MapPath,
        '--uelib', $UELibPath,
        '--mode', 'classes',
        '--output', $classOutput,
        '--class-pattern', '.*',
        '--max-classes', '5'
    ) 'classes'
    Assert-True ($classes.ExitCode -eq 0) 'bounded class inventory must succeed'
    $classRecords = @([IO.File]::ReadAllLines($classOutput) | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($classRecords[0].record -eq 'header') 'class JSONL must begin with a header'
    Assert-True (@($classRecords | Where-Object record -eq 'class').Count -le 5) 'class output must honor max-classes'
    Assert-True ($classRecords[-1].record -eq 'summary') 'class JSONL must end with a summary'

    $actorOutput = Join-Path $tempRoot 'actors.jsonl'
    $actors = Invoke-Extractor $executable @(
        '--input', $MapPath,
        '--uelib', $UELibPath,
        '--output', $actorOutput,
        '--class-pattern', 'ROPlayerStart|ROVolumePlayerStartGroup',
        '--property-pattern', 'Location|Rotation|Group',
        '--max-actors', '4',
        '--max-properties', '3',
        '--max-value-chars', '128'
    ) 'actors'
    Assert-True ($actors.ExitCode -eq 0) 'selected actor extraction must succeed'
    $actorRecords = @([IO.File]::ReadAllLines($actorOutput) | ForEach-Object { $_ | ConvertFrom-Json })
    $actualActors = @($actorRecords | Where-Object record -eq 'actor')
    Assert-True ($actualActors.Count -gt 0) 'official campaign shell must expose selected actors'
    Assert-True ($actualActors.Count -le 4) 'actor output must honor max-actors'
    Assert-True (@($actualActors | Where-Object { $null -ne $_.location }).Count -gt 0) 'at least one selected actor must expose a parsed location'
    Assert-True (@($actualActors | Where-Object { $_.path -notmatch '\.TheWorld\.' }).Count -eq 0) 'only world actors may be emitted'
    Assert-True ($actorRecords[-1].record -eq 'summary') 'actor JSONL must end with a summary'
    Assert-True ($actorRecords[-1].errors -eq 0) 'actor extraction must report zero errors'

    if (Test-Path -LiteralPath $RoleMapPath -PathType Leaf) {
        $roleBeforeHash = (Get-FileHash -LiteralPath $RoleMapPath -Algorithm SHA256).Hash
        $roleOutput = Join-Path $tempRoot 'role-info.jsonl'
        & $wrapper $RoleMapPath -UELibPath $UELibPath -RoleInfo `
            -OutputPath $roleOutput -MaxInputMiB 300
        Assert-True ($LASTEXITCODE -eq 0) 'exact ROMapInfo role extraction must succeed'

        $roleRecords = @([IO.File]::ReadAllLines($roleOutput) | ForEach-Object { $_ | ConvertFrom-Json })
        $roles = @($roleRecords | Where-Object record -eq 'role')
        $northRoles = @($roles | Where-Object team -eq 'north')
        $southRoles = @($roles | Where-Object team -eq 'south')
        Assert-True ($roleRecords[0].mode -eq 'role-info') 'role JSONL header must identify its mode'
        Assert-True ($roles.Count -eq 14) 'Cu Chi must expose seven exact roles per team'
        Assert-True ($northRoles.Count -eq 7 -and $southRoles.Count -eq 7) `
            'role output must preserve both team arrays'
        Assert-True (@($roles | Where-Object serializedBytes -ne 102).Count -eq 0) `
            'every Cu Chi RORoleCount element must consume its full tagged 102-byte layout'
        Assert-True ($northRoles[0].roleInfoClassPath -eq "Class'ROGame.RORoleInfoNorthernRifleman'" -and `
                     $northRoles[0].count -eq 255 -and $northRoles[0].reverseCount -eq 255) `
            'Cu Chi North base infantry role/counts must be exact'
        Assert-True ($southRoles[0].roleInfoClassPath -eq "Class'ROGame.RORoleInfoSouthernRifleman'" -and `
                     $southRoles[0].count -eq 255 -and $southRoles[0].reverseCount -eq 255) `
            'Cu Chi South base infantry role/counts must be exact'
        Assert-True ($roleRecords[-1].roleArraysConsumedExactly) `
            'role summary must assert exact role-array consumption'
        Assert-True (-not $roleRecords[-1].runtimeSquadsExtracted) `
            'role extraction must not mislabel runtime-generated squads as cooked data'

        $roleAfterHash = (Get-FileHash -LiteralPath $RoleMapPath -Algorithm SHA256).Hash
        Assert-True ($roleBeforeHash -eq $roleAfterHash) 'the role source .roe must remain byte-identical'
    }
    elseif ($RequireIntegration) {
        throw "Role integration map is required but missing: $RoleMapPath"
    }

    if (Test-Path -LiteralPath $CollisionMapPath -PathType Leaf) {
        $collisionBeforeHash = (Get-FileHash -LiteralPath $CollisionMapPath -Algorithm SHA256).Hash
        $boundsOutput = Join-Path $tempRoot 'brush-bounds.jsonl'
        & $wrapper $CollisionMapPath -UELibPath $UELibPath -BrushBounds `
            -OutputPath $boundsOutput -MaxInputMiB 1200 -MaxActors 4 -MaxErrors 4
        Assert-True ($LASTEXITCODE -eq 0) 'bounded UModel brush-bounds extraction must succeed'

        $boundsRecords = @([IO.File]::ReadAllLines($boundsOutput) | ForEach-Object { $_ | ConvertFrom-Json })
        $bounds = @($boundsRecords | Where-Object record -eq 'brushBounds')
        Assert-True ($boundsRecords[0].mode -eq 'brush-bounds') 'bounds JSONL header must identify its mode'
        Assert-True ($boundsRecords[0].packageGuid -match '^[0-9A-Fa-f]{32}$') 'bounds header must preserve package GUID provenance'
        Assert-True ($bounds.Count -eq 4) 'bounds output must honor max-actors'
        Assert-True ($boundsRecords[-1].record -eq 'summary') 'bounds JSONL must end with a summary'
        Assert-True ($boundsRecords[-1].truncated) 'bounded Resort sample must report truncation'
        Assert-True ($boundsRecords[-1].errors -eq 0) 'bounded Resort sample must report zero errors'

        foreach ($bound in $bounds) {
            Assert-True ($bound.sourceBoundsVerified) 'native UModel bounds must be source-verified'
            Assert-True ($bound.geometryFidelity -eq 'bounds_only') 'UModel output must be explicitly bounds-only'
            Assert-True (-not $bound.collisionGeometryComplete) 'bounds must never claim complete collision geometry'
            Assert-True (-not $bound.worldCollisionComplete) 'bounds must never claim complete world collision'
            Assert-True (-not $bound.safeForGameplayOcclusion) 'bounds must never opt into gameplay occlusion'
            Assert-True ($bound.transform.kind -eq 'translation_only') 'unsupported transforms must not be silently approximated'
            Assert-True ($bound.worldAabb.min.x -le $bound.worldAabb.max.x -and `
                         $bound.worldAabb.min.y -le $bound.worldAabb.max.y -and `
                         $bound.worldAabb.min.z -le $bound.worldAabb.max.z) `
                'world AABB axes must be ordered'
            foreach ($number in @(
                $bound.localBounds.origin.x, $bound.localBounds.origin.y, $bound.localBounds.origin.z,
                $bound.localBounds.extent.x, $bound.localBounds.extent.y, $bound.localBounds.extent.z,
                $bound.localBounds.sphereRadius,
                $bound.worldAabb.min.x, $bound.worldAabb.min.y, $bound.worldAabb.min.z,
                $bound.worldAabb.max.x, $bound.worldAabb.max.y, $bound.worldAabb.max.z)) {
                $value = [double]$number
                Assert-True (-not [double]::IsNaN($value) -and -not [double]::IsInfinity($value)) `
                    'all emitted bounds values must be finite'
                Assert-True ([Math]::Abs($value) -le 100000000.0) `
                    'all emitted bounds values must honor the numeric cap'
            }
        }

        $collisionAfterHash = (Get-FileHash -LiteralPath $CollisionMapPath -Algorithm SHA256).Hash
        Assert-True ($collisionBeforeHash -eq $collisionAfterHash) 'the collision source .roe must remain byte-identical'
    }
    elseif ($RequireIntegration) {
        throw "Collision integration map is required but missing: $CollisionMapPath"
    }

    $afterHash = (Get-FileHash -LiteralPath $MapPath -Algorithm SHA256).Hash
    Assert-True ($beforeHash -eq $afterHash) 'the official .roe input must remain byte-identical'

    Write-Output 'PASS: cooked-map extractor compile, validation, roles, bounds, JSONL, actor metadata, and read-only hash checks.'
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
