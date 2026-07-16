[CmdletBinding()]
param(
    [string]$UELibPath = 'D:\RE-Tools\UE-Explorer\Eliot.UELib.dll',
    [string]$MapPath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\VNTE-CampaignStart.roe',
    [string]$RoleMapPath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\CuChi\VNTE-CuChi.roe',
    [string]$CollisionMapPath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\Resort\VNTE-Resort.roe',
    [string]$RolePackagePath = 'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\ROGame.u',
    [string]$ClassArtifactPackageRoot = 'D:\rs2dedicatedserver\ROGame\BrewedPCServer',
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

function Assert-NoForbiddenClassArtifactKeys {
    param([object]$Value, [string]$Path = '$')
    if ($null -eq $Value -or $Value -is [string] -or
        $Value -is [ValueType]) {
        return
    }
    if ($Value -is [Collections.IDictionary]) {
        foreach ($key in $Value.Keys) {
            Assert-True (@('objectBase', 'staticReference', 'wireReference') -notcontains [string]$key) `
                "class-artifact JSON must not contain forbidden key $Path.$key"
            Assert-NoForbiddenClassArtifactKeys $Value[$key] "$Path.$key"
        }
        return
    }
    if ($Value -is [Collections.IEnumerable]) {
        $index = 0
        foreach ($item in $Value) {
            Assert-NoForbiddenClassArtifactKeys $item "$Path[$index]"
            $index++
        }
        return
    }
    foreach ($property in $Value.PSObject.Properties) {
        if ($property.MemberType -notin @('NoteProperty', 'Property')) {
            continue
        }
        Assert-True (@('objectBase', 'staticReference', 'wireReference') -notcontains $property.Name) `
            "class-artifact JSON must not contain forbidden key $Path.$($property.Name)"
        Assert-NoForbiddenClassArtifactKeys $property.Value "$Path.$($property.Name)"
    }
}

function Assert-ExactJsonPropertySet {
    param(
        [object]$Value,
        [string[]]$Expected,
        [string]$Label
    )
    $actualNames = @(
        $Value.PSObject.Properties |
            Where-Object MemberType -eq 'NoteProperty' |
            ForEach-Object Name |
            Sort-Object
    )
    $expectedNames = @($Expected | Sort-Object)
    Assert-True ($actualNames.Count -eq $expectedNames.Count -and
        (($actualNames -join ',') -eq ($expectedNames -join ','))) `
        "$Label JSON keys must be exact; actual=$($actualNames -join ',')"
}

try {
    $buildOutput = @(& $wrapper -BuildOnly -UELibPath $UELibPath)
    Assert-True ($LASTEXITCODE -eq 0) 'wrapper compilation must succeed'
    $executable = [string]($buildOutput | Select-Object -Last 1)
    Assert-True (Test-Path -LiteralPath $executable -PathType Leaf) 'compiled extractor must exist'
    $cacheKey = Split-Path -Leaf (Split-Path -Parent $executable)
    Assert-True ($cacheKey -match '^[0-9A-F]{64}$') `
        'compiled extractor cache must be content-addressed'
    $secondBuildOutput = @(& $wrapper -BuildOnly -UELibPath $UELibPath)
    Assert-True ([string]($secondBuildOutput | Select-Object -Last 1) -eq $executable) `
        'identical source, wrapper, and compiler bytes must reuse one cache identity'
    $executableHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    $extractorAssemblyIdentity =
        [Reflection.AssemblyName]::GetAssemblyName($executable).FullName
    $extractorBytes = (Get-Item -LiteralPath $executable).Length
    $hashManifest = $executable + '.sha256'
    Assert-True (Test-Path -LiteralPath $hashManifest -PathType Leaf) `
        'compiled extractor cache must include an executable-hash manifest'
    Assert-True ([IO.File]::ReadAllText($hashManifest).Trim() -eq $executableHash) `
        'compiled extractor cache manifest must match the executable bytes'
    $forcedBuildOutput = @(
        & $wrapper -BuildOnly -ForceRebuild -UELibPath $UELibPath `
            -ExpectedExecutableSha256 $executableHash
    )
    Assert-True ([string]($forcedBuildOutput | Select-Object -Last 1) -eq $executable) `
        'forced build must atomically republish the same deterministic executable'
    Assert-True (@(Get-ChildItem -LiteralPath (Split-Path -Parent $executable) `
            -Directory -Filter 'build-*').Count -eq 0) `
        'successful cache publication must leave no staging directories'
    [IO.File]::WriteAllText(
        $hashManifest,
        ('0' * 64) + "`n",
        (New-Object Text.UTF8Encoding($false))
    )
    $repairedBuildOutput = @(
        & $wrapper -BuildOnly -UELibPath $UELibPath `
            -ExpectedExecutableSha256 $executableHash
    )
    Assert-True ([string]($repairedBuildOutput | Select-Object -Last 1) -eq $executable) `
        'a stale executable-hash manifest must trigger a known-good rebuild'
    Assert-True ([IO.File]::ReadAllText($hashManifest).Trim() -eq $executableHash) `
        'cache rebuild must repair the executable-hash manifest'
    $wrongExecutableHash = Invoke-Extractor 'powershell' @(
        '-NoProfile',
        '-File', $wrapper,
        '-BuildOnly',
        '-ForceRebuild',
        '-ExpectedExecutableSha256', ('F' * 64)
    ) 'wrong-executable-hash'
    Assert-True ($wrongExecutableHash.ExitCode -ne 0) `
        'forced build must reject an unexpected executable SHA-256'
    Assert-True ($wrongExecutableHash.Stderr -match 'mismatch before publication') `
        'unexpected executable rejection must occur before cache publication'
    Assert-True ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -eq $executableHash) `
        'unexpected staged executable must not replace the known-good cache entry'

    $help = Invoke-Extractor $executable @('--help') 'help'
    Assert-True ($help.ExitCode -eq 0) '--help must exit zero'
    Assert-True ($help.Stdout -match 'bounded, read-only') '--help must state the safety contract'

    $roleSchema = Invoke-Extractor $executable @('--self-test-role-schema') 'role-schema'
    Assert-True ($roleSchema.ExitCode -eq 0) `
        'synthetic exact-role-schema checks must succeed'
    Assert-True ($roleSchema.Stdout -match 'rejects unknown fields and nonzero scalar ArrayIndex') `
        'role-schema self-test must cover unknown fields and nonzero scalar ArrayIndex values'

    $roleExportSchema = Invoke-Extractor $executable @('--self-test-role-export-schema') 'role-export-schema'
    Assert-True ($roleExportSchema.ExitCode -eq 0) `
        'synthetic exact-role-export checks must succeed'
    Assert-True ($roleExportSchema.Stdout -match 'checked static references') `
        'role-export self-test must cover checked PackageMap reference arithmetic'

    $classArtifactSchema = Invoke-Extractor $executable `
        @('--self-test-class-artifact-schema') 'class-artifact-schema'
    Assert-True ($classArtifactSchema.ExitCode -eq 0) `
        'synthetic exact class-artifact checks must succeed'
    Assert-True ($classArtifactSchema.Stdout -match `
        'anchored class regex, exact count, non-adjacent CDO linkage') `
        'class-artifact self-test must cover anchored selection, exact counts, and direct non-adjacent linkage'
    Assert-True ($classArtifactSchema.Stdout -match 'forbidden key absence') `
        'class-artifact self-test must recursively exclude runtime-reference keys'

    $exclusiveClassModes = Invoke-Extractor 'powershell' @(
        '-NoProfile', '-File', $wrapper, '-BuildOnly',
        '-ClassesOnly', '-ClassArtifacts'
    ) 'exclusive-class-modes'
    Assert-True ($exclusiveClassModes.ExitCode -ne 0) `
        'wrapper must reject class-artifacts combined with another extraction mode'
    Assert-True ($exclusiveClassModes.Stderr -match 'mutually exclusive') `
        'wrapper mode rejection must explain exclusivity'

    $classIdentityArguments = @(
        '--input', (Join-Path $tempRoot 'missing.u'),
        '--uelib', $UELibPath,
        '--mode', 'class-artifacts',
        '--class-pattern', '^(Actor)$',
        '--expected-package-guid', ('1' * 32),
        '--expected-sha256', ('A' * 64)
    )
    $missingClassCount = Invoke-Extractor $executable `
        $classIdentityArguments 'class-artifact-missing-count'
    Assert-True ($missingClassCount.ExitCode -eq 65) `
        'class-artifacts must reject a missing exact class count before opening a package'
    Assert-True ($missingClassCount.Stderr -match 'requires --expected-class-count') `
        'missing class count rejection must identify the required argument'

    $unanchoredClassPattern = Invoke-Extractor $executable `
        @($classIdentityArguments + @(
            '--expected-class-count', '1',
            '--class-pattern', 'Actor'
        )) 'class-artifact-unanchored'
    Assert-True ($unanchoredClassPattern.ExitCode -eq 65) `
        'class-artifacts must reject an unanchored regex before opening a package'
    Assert-True ($unanchoredClassPattern.Stderr -match 'explicitly anchored') `
        'unanchored regex rejection must identify the exact selection contract'

    $classObjectBase = Invoke-Extractor $executable `
        @($classIdentityArguments + @(
            '--expected-class-count', '1',
            '--object-base', '39478'
        )) 'class-artifact-object-base'
    Assert-True ($classObjectBase.ExitCode -eq 65) `
        'class-artifacts must reject object-base derivation before opening a package'
    Assert-True ($classObjectBase.Stderr -match 'rejects --object-base') `
        'object-base rejection must state that no static references are derived'

    foreach ($forbiddenReferenceArgument in @(
        '--static-reference', '--wire-reference')) {
        $forbiddenReference = Invoke-Extractor $executable `
            @($classIdentityArguments + @(
                '--expected-class-count', '1',
                $forbiddenReferenceArgument
            )) ('class-artifact-' + $forbiddenReferenceArgument.TrimStart('-'))
        Assert-True ($forbiddenReference.ExitCode -eq 64) `
            "class-artifacts must reject $forbiddenReferenceArgument as an unknown CLI argument"
        Assert-True ($forbiddenReference.Stderr -match `
            [Regex]::Escape("unknown argument: $forbiddenReferenceArgument")) `
            "$forbiddenReferenceArgument rejection must occur at CLI parsing"
    }

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

    $uelibAssemblyName = [Reflection.AssemblyName]::GetAssemblyName($UELibPath)
    $uelibAssemblyIdentity = $uelibAssemblyName.FullName
    $uelibAssemblyVersion = $uelibAssemblyName.Version.ToString()
    $uelibProductVersion =
        [Diagnostics.FileVersionInfo]::GetVersionInfo($UELibPath).ProductVersion
    $uelibBytes = (Get-Item -LiteralPath $UELibPath).Length
    $uelibSha256 = (Get-FileHash -LiteralPath $UELibPath -Algorithm SHA256).Hash
    $unsafePath = Join-Path (Split-Path -Parent $UELibPath) `
        'System.Runtime.CompilerServices.Unsafe.dll'
    Assert-True (Test-Path -LiteralPath $unsafePath -PathType Leaf) `
        'pinned UELib Unsafe dependency must exist'
    $unsafeAssemblyIdentity =
        [Reflection.AssemblyName]::GetAssemblyName($unsafePath).FullName
    $unsafeBytes = (Get-Item -LiteralPath $unsafePath).Length
    $unsafeSha256 = (Get-FileHash -LiteralPath $unsafePath -Algorithm SHA256).Hash

    $beforeHash = (Get-FileHash -LiteralPath $MapPath -Algorithm SHA256).Hash

    $wrongDependencyRoot = Join-Path $tempRoot 'wrong-dependency'
    [IO.Directory]::CreateDirectory($wrongDependencyRoot) | Out-Null
    $copiedUELib = Join-Path $wrongDependencyRoot 'Eliot.UELib.dll'
    $wrongUnsafe = Join-Path $wrongDependencyRoot 'System.Runtime.CompilerServices.Unsafe.dll'
    Copy-Item -LiteralPath $UELibPath -Destination $copiedUELib
    Copy-Item -LiteralPath $UELibPath -Destination $wrongUnsafe
    $wrongDependency = Invoke-Extractor $executable @(
        '--input', $MapPath,
        '--uelib', $copiedUELib,
        '--max-actors', '1'
    ) 'wrong-dependency'
    Assert-True ($wrongDependency.ExitCode -eq 65) `
        'extractor must reject a sibling dependency with the wrong assembly identity'
    Assert-True ($wrongDependency.Stderr -match 'exact pinned UELib dependency') `
        'dependency rejection must identify the pinned-load contract'

    $unsafeSource = Join-Path (Split-Path -Parent $UELibPath) `
        'System.Runtime.CompilerServices.Unsafe.dll'
    Assert-True (Test-Path -LiteralPath $unsafeSource -PathType Leaf) `
        'installed UELib dependency must exist for integration checks'
    $protectedDependencyRoot = Join-Path $tempRoot 'protected-dependency'
    [IO.Directory]::CreateDirectory($protectedDependencyRoot) | Out-Null
    $protectedUELib = Join-Path $protectedDependencyRoot 'Eliot.UELib.dll'
    $protectedUnsafe = Join-Path $protectedDependencyRoot `
        'System.Runtime.CompilerServices.Unsafe.dll'
    Copy-Item -LiteralPath $UELibPath -Destination $protectedUELib
    Copy-Item -LiteralPath $unsafeSource -Destination $protectedUnsafe
    $protectedUnsafeHash = (Get-FileHash -LiteralPath $protectedUnsafe -Algorithm SHA256).Hash
    $dependencyOutput = Invoke-Extractor $executable @(
        '--input', $MapPath,
        '--uelib', $protectedUELib,
        '--output', $protectedUnsafe,
        '--overwrite',
        '--max-actors', '1'
    ) 'dependency-output'
    Assert-True ($dependencyOutput.ExitCode -eq 65) `
        'extractor output must not alias the pinned UELib dependency'
    Assert-True ($dependencyOutput.Stderr -match 'UELib dependency file') `
        'dependency output rejection must identify the protected input'
    Assert-True ((Get-FileHash -LiteralPath $protectedUnsafe -Algorithm SHA256).Hash -eq $protectedUnsafeHash) `
        'dependency output rejection must leave the pinned bytes unchanged'

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

    if (Test-Path -LiteralPath $RolePackagePath -PathType Leaf) {
        $rolePackageBeforeHash = (Get-FileHash -LiteralPath $RolePackagePath -Algorithm SHA256).Hash
        $expectedRolePackageHash = 'AED4E60D406880D048EB579A082F4A44BE3D0B39CFEC47F9FCEF828A40C44961'
        Assert-True ($rolePackageBeforeHash -eq $expectedRolePackageHash) `
            'installed ROGame.u must match the pinned role-export artifact'

        $roleExportOutput = Join-Path $tempRoot 'role-exports.jsonl'
        & $wrapper $RolePackagePath -UELibPath $UELibPath -RoleExports `
            -ExpectedPackageGuid '16A6CC8D446C4A9FD5B688B3210DCC82' `
            -ExpectedSha256 $expectedRolePackageHash -ObjectBase 39478 `
            -MaxInputMiB 64 -OutputPath $roleExportOutput
        Assert-True ($LASTEXITCODE -eq 0) 'pinned ROGame role-export audit must succeed'

        $roleExportRecords = @([IO.File]::ReadAllLines($roleExportOutput) | ForEach-Object { $_ | ConvertFrom-Json })
        $roleExports = @($roleExportRecords | Where-Object record -eq 'roleExport')
        Assert-True ($roleExportRecords[0].inputSha256 -eq $expectedRolePackageHash) `
            'role-export header must preserve the verified artifact SHA-256'
        Assert-True ($roleExportRecords[0].sha256StableAcrossTableRead) `
            'role-export audit must recheck SHA-256 after reading package tables'
        Assert-True ($roleExportRecords[0].packageGuid -eq '16A6CC8D446C4A9FD5B688B3210DCC82') `
            'role-export header must preserve the exact package GUID'
        Assert-True ($roleExportRecords[0].packageName -eq 'ROGame') `
            'role-export header must preserve the exact internal package name'
        Assert-True ($roleExportRecords[0].packageFlags -eq '0x20204001' -and $roleExportRecords[0].finalGenerationNetObjects -eq 64476) `
            'role-export header must preserve exact script/generation provenance'
        Assert-True ($roleExports.Count -eq 94) `
            'pinned ROGame.u must expose 94 exact UClass/CDO role pairs'
        $northGuerilla = @($roleExports | Where-Object roleClass -eq 'RORoleInfoNorthernGuerilla')
        $southGrunt = @($roleExports | Where-Object roleClass -eq 'RORoleInfoSouthernGrunt')
        $southMachineGunner = @($roleExports | Where-Object roleClass -eq 'RORoleInfoSouthernMachineGunner')
        Assert-True ($northGuerilla.Count -eq 1 -and $northGuerilla[0].uclassLinkerIndex -eq 47921 -and $northGuerilla[0].uclassStaticReference -eq 87399) `
            'North Cu Chi class-0 UClass identity must remain exact'
        Assert-True ($southGrunt.Count -eq 1 -and $southGrunt[0].uclassLinkerIndex -eq 48013 -and $southGrunt[0].uclassStaticReference -eq 87491) `
            'South Cu Chi class-0 UClass identity must remain exact'
        Assert-True ($southMachineGunner.Count -eq 1 -and $southMachineGunner[0].uclassLinkerIndex -eq 48019 -and $southMachineGunner[0].uclassStaticReference -eq 87497) `
            'South MachineGunner evidence candidate must remain exact but disabled'
        Assert-True (@($roleExports | Where-Object authorizedByExtractor).Count -eq 0) `
            'evidence extraction must never authorize runtime role support'
        Assert-True ($roleExportRecords[-1].pairsValidatedExactly -and -not $roleExportRecords[-1].runtimeRolesAuthorized) `
            'role-export summary must preserve exact pairing and fail-closed runtime state'

        $badGuid = Invoke-Extractor $executable @(
            '--input', $RolePackagePath,
            '--uelib', $UELibPath,
            '--mode', 'role-exports',
            '--expected-package-guid', '11111111111111111111111111111111',
            '--expected-sha256', $expectedRolePackageHash
        ) 'role-export-bad-guid'
        Assert-True ($badGuid.ExitCode -eq 65) `
            'role-export audit must fail closed on package GUID drift'
        Assert-True ($badGuid.Stderr -match 'package GUID mismatch') `
            'role-export GUID rejection must identify the drift'

        $rolePackageAfterHash = (Get-FileHash -LiteralPath $RolePackagePath -Algorithm SHA256).Hash
        Assert-True ($rolePackageBeforeHash -eq $rolePackageAfterHash) `
            'the audited ROGame.u source must remain byte-identical'
    }
    elseif ($RequireIntegration) {
        throw "Role package integration input is required but missing: $RolePackagePath"
    }

    $classArtifactCases = @(
        [PSCustomObject]@{
            Package = 'Core'; Pattern = '^(Object)$'; Count = 1
            Bytes = 226734; PackageFlags = '0x20204000'
            PackageVersion = 765; LicenseeVersion = 771; EngineVersion = 7258
            Generations = @('1535/803/1535', '1535/803/1535')
            Sha256 = '9F48070EEFF458478792677B6E3D3CCB6A94678A070EB602D6FE00B51EBE1E6A'
            Guid = '4E98E1A84B17D0773382759988198719'
            RawGuid = 'A8E1984E77D0174B9975823319871988'
            Targets = @()
        },
        [PSCustomObject]@{
            Package = 'Engine'; Pattern = '^(Actor|Inventory|Weapon)$'; Count = 3
            Bytes = 196406917; PackageFlags = '0x20204000'
            PackageVersion = 765; LicenseeVersion = 771; EngineVersion = 7258
            Generations = @('37942/22296/37942', '37943/23123/37943')
            Sha256 = '068946B520AA5DC81F22E0DBB6CE78B096F171E88A2E25608FB49D261E48D98E'
            Guid = '7AE12CB344747678743E5ABD12E28DA7'
            RawGuid = 'B32CE17A78767444BD5A3E74A78DE212'
            Targets = @([PSCustomObject]@{
                Class = 'Actor'; UClassNetIndex = $null; CdoNetIndex = $null
            })
        },
        [PSCustomObject]@{
            Package = 'ROGame'
            Pattern = '^(ROWeapon|ROProjectileWeapon|ROBipodWeapon|ROMGWeapon|ROWeap_M60_GPMG|ROOneShotWeapon|ROExplosiveWeapon|ROEggGrenadeWeapon|ROWeap_M61_Grenade)$'
            Count = 9
            Bytes = 26584752; PackageFlags = '0x20204001'
            PackageVersion = 765; LicenseeVersion = 771; EngineVersion = 7258
            Generations = @('64471/48070/64471', '64472/48357/64472')
            Sha256 = '06D63FF85F2C9BC740E50FC127AE5DF4DFF8AD2678DA615C44184EF4D9D71A02'
            Guid = '33EE724F43F851351795FD975E8D5AC1'
            RawGuid = '4F72EE333551F84397FD9517C15A8D5E'
            Targets = @()
        },
        [PSCustomObject]@{
            Package = 'ROGameContent'
            Pattern = '^(ROWeap_M60_GPMG_Content|ROWeap_M61_Grenade_Content|ROWeap_M61_Grenade_ContentSingle)$'
            Count = 3
            Bytes = 23934851; PackageFlags = '0x20204001'
            PackageVersion = 765; LicenseeVersion = 771; EngineVersion = 7258
            Generations = @('2351/3800/2351', '2352/4093/2352')
            Sha256 = '2D6433144F00EB130D15193C414A4F401FCA40806AEFC9A6342B7670E376F992'
            Guid = 'FE4B4F2F4B3128C42FE5098ACAB560C8'
            RawGuid = '2F4F4BFEC428314B8A09E52FC860B5CA'
            Targets = @(
                [PSCustomObject]@{
                    Class = 'ROWeap_M60_GPMG_Content'
                    UClassNetIndex = 512; CdoNetIndex = 513
                },
                [PSCustomObject]@{
                    Class = 'ROWeap_M61_Grenade_ContentSingle'
                    UClassNetIndex = 529; CdoNetIndex = 530
                }
            )
        }
    )
    $missingClassArtifactPackages = @(
        $classArtifactCases | Where-Object {
            -not (Test-Path -LiteralPath `
                (Join-Path $ClassArtifactPackageRoot ($_.Package + '.u')) `
                -PathType Leaf)
        }
    )
    if ($missingClassArtifactPackages.Count -eq 0) {
        foreach ($case in $classArtifactCases) {
            $packagePath = Join-Path $ClassArtifactPackageRoot ($case.Package + '.u')
            $beforeClassArtifactHash = `
                (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
            Assert-True ((Get-Item -LiteralPath $packagePath).Length -eq $case.Bytes) `
                "$($case.Package).u byte length must remain exactly pinned"
            Assert-True ($beforeClassArtifactHash -eq $case.Sha256) `
                "$($case.Package).u must match the capture-compatible server artifact"
            $classArtifactOutput = Join-Path $tempRoot `
                ('class-artifacts-' + $case.Package + '.jsonl')
            & $wrapper $packagePath -UELibPath $UELibPath -ClassArtifacts `
                -ClassPattern $case.Pattern -ExpectedClassCount $case.Count `
                -ExpectedPackageGuid $case.Guid -ExpectedSha256 $case.Sha256 `
                -OutputPath $classArtifactOutput
            Assert-True ($LASTEXITCODE -eq 0) `
                "$($case.Package).u exact class-artifact extraction must succeed"

            $rawClassArtifactJson = [IO.File]::ReadAllText($classArtifactOutput)
            $classArtifactRecords = @(
                [IO.File]::ReadAllLines($classArtifactOutput) |
                    ForEach-Object { $_ | ConvertFrom-Json }
            )
            $classRecords = @(
                $classArtifactRecords | Where-Object record -eq 'class'
            )
            $classHeader = $classArtifactRecords[0]
            $classPackage = @(
                $classArtifactRecords | Where-Object record -eq 'package'
            )
            Assert-True ($classArtifactRecords[-1].record -eq 'summary' -and
                @($classArtifactRecords | Where-Object {
                    $_.record -notin @('header', 'package', 'class', 'summary')
                }).Count -eq 0) `
                'class-artifact JSONL must contain only the frozen record kinds'
            Assert-ExactJsonPropertySet $classHeader @(
                'record', 'schema', 'mode', 'classPattern',
                'expectedClassCount', 'artifactBytes', 'artifactSha256',
                'extractorAssemblyIdentity', 'extractorBytes',
                'extractorSha256', 'uelibAssemblyIdentity',
                'uelibAssemblyVersion', 'uelibProductVersion', 'uelibBytes',
                'uelibSha256', 'unsafeAssemblyIdentity', 'unsafeBytes',
                'unsafeSha256', 'wireStaticReferencesDerived',
                'reportAuthorizesRuntime'
            ) 'class-artifact header'
            Assert-True ($classHeader.record -eq 'header' -and
                $classHeader.schema -eq 'rs2.cooked-class-artifacts.raw.v1' -and
                $classHeader.mode -eq 'class-artifacts') `
                'class-artifact JSONL must start with the exact raw schema header'
            Assert-True ($classHeader.classPattern -eq $case.Pattern -and
                $classHeader.expectedClassCount -eq $case.Count) `
                'class-artifact header must preserve the exact anchored selector and count'
            Assert-True ($classHeader.artifactBytes -eq $case.Bytes -and
                $classHeader.artifactSha256 -eq $case.Sha256) `
                'class-artifact header must pin the exact artifact bytes'
            Assert-True ($classHeader.extractorAssemblyIdentity -eq
                    $extractorAssemblyIdentity -and
                $classHeader.extractorBytes -eq $extractorBytes -and
                $classHeader.extractorSha256 -eq $executableHash) `
                'class-artifact header must pin the exact invoked extractor identity'
            Assert-True ($classHeader.uelibAssemblyIdentity -eq
                    $uelibAssemblyIdentity -and
                $classHeader.uelibAssemblyVersion -eq $uelibAssemblyVersion -and
                $classHeader.uelibProductVersion -eq $uelibProductVersion -and
                $classHeader.uelibBytes -eq $uelibBytes -and
                $classHeader.uelibSha256 -eq $uelibSha256) `
                'class-artifact header must pin the exact loaded UELib identity'
            Assert-True ($classHeader.unsafeAssemblyIdentity -eq
                    $unsafeAssemblyIdentity -and
                $classHeader.unsafeBytes -eq $unsafeBytes -and
                $classHeader.unsafeSha256 -eq $unsafeSha256) `
                'class-artifact header must pin the exact loaded Unsafe identity'
            Assert-True (-not $classHeader.wireStaticReferencesDerived -and
                -not $classHeader.reportAuthorizesRuntime) `
                'class-artifact header must fail closed on wire and runtime claims'
            Assert-True ($classHeader.wireStaticReferencesDerived -is [bool] -and
                $classHeader.reportAuthorizesRuntime -is [bool] -and
                $classHeader.expectedClassCount -is [ValueType] -and
                $classHeader.artifactBytes -is [ValueType]) `
                'class-artifact header booleans and counts must retain JSON scalar types'
            foreach ($stringIdentityField in @(
                'record', 'schema', 'mode', 'classPattern', 'artifactSha256',
                'extractorAssemblyIdentity', 'extractorSha256',
                'uelibAssemblyIdentity', 'uelibAssemblyVersion',
                'uelibProductVersion', 'uelibSha256',
                'unsafeAssemblyIdentity', 'unsafeSha256')) {
                Assert-True ($classHeader.$stringIdentityField -is [string]) `
                    "header $stringIdentityField must retain its JSON string type"
            }
            foreach ($numericIdentityField in @(
                'expectedClassCount', 'artifactBytes', 'extractorBytes',
                'uelibBytes', 'unsafeBytes')) {
                Assert-True ($classHeader.$numericIdentityField -is [ValueType]) `
                    "header $numericIdentityField must retain its JSON numeric type"
            }
            Assert-ExactJsonPropertySet $classPackage[0] @(
                'record', 'package', 'artifactBytes', 'artifactSha256',
                'rawGuid', 'packageGuid', 'packageFlags', 'packageVersion',
                'licenseeVersion', 'engineVersion', 'generationCount',
                'generations'
            ) 'class-artifact package'
            Assert-True ($classPackage.Count -eq 1 -and
                $classPackage[0].package -eq $case.Package -and
                $classPackage[0].artifactBytes -eq $case.Bytes -and
                $classPackage[0].artifactSha256 -eq $case.Sha256 -and
                $classPackage[0].packageGuid -eq $case.Guid -and
                $classPackage[0].rawGuid -eq $case.RawGuid) `
                'package record must preserve exact artifact and GUID identities'
            Assert-True ($classPackage[0].package -is [string] -and
                $classPackage[0].artifactBytes -is [ValueType] -and
                $classPackage[0].artifactSha256 -is [string] -and
                $classPackage[0].rawGuid -is [string] -and
                $classPackage[0].packageGuid -is [string] -and
                $classPackage[0].packageFlags -is [string] -and
                $classPackage[0].packageVersion -is [ValueType] -and
                $classPackage[0].licenseeVersion -is [ValueType] -and
                $classPackage[0].engineVersion -is [ValueType] -and
                $classPackage[0].generationCount -is [ValueType]) `
                'package identity fields must retain exact string and numeric JSON types'
            Assert-True ($classPackage[0].packageFlags -eq $case.PackageFlags -and
                $classPackage[0].packageVersion -eq $case.PackageVersion -and
                $classPackage[0].licenseeVersion -eq $case.LicenseeVersion -and
                $classPackage[0].engineVersion -eq $case.EngineVersion) `
                'package record must preserve exact flags and package/licensee/engine versions'
            $actualGenerationTuples = @()
            for ($generationIndex = 0;
                 $generationIndex -lt @($classPackage[0].generations).Count;
                 $generationIndex++) {
                $generation = @($classPackage[0].generations)[$generationIndex]
                Assert-ExactJsonPropertySet $generation @(
                    'ordinal', 'exports', 'names', 'netObjects'
                ) 'class-artifact generation'
                Assert-True ($generation.ordinal -eq $generationIndex -and
                    $generation.ordinal -is [ValueType] -and
                    $generation.exports -is [ValueType] -and
                    $generation.names -is [ValueType] -and
                    $generation.netObjects -is [ValueType]) `
                    'generation ordinal must equal its array index and all counts must be numeric'
                $actualGenerationTuples += '{0}/{1}/{2}' -f `
                    $generation.exports, $generation.names, $generation.netObjects
            }
            Assert-True ($classPackage[0].generationCount -eq $case.Generations.Count -and
                (($actualGenerationTuples -join ',') -eq
                    ($case.Generations -join ','))) `
                'package record must preserve every exact generation tuple in order'
            Assert-True ($classRecords.Count -eq $case.Count) `
                "$($case.Package).u must emit exactly $($case.Count) selected classes"
            Assert-True (@($classRecords | Where-Object {
                -not $_.CDO.classLinkVerified
            }).Count -eq 0) `
                'every emitted CDO must verify its direct loaded UClass/table linkage'
            foreach ($classRecord in $classRecords) {
                Assert-ExactJsonPropertySet $classRecord @(
                    'record', 'classPath', 'superClassPath',
                    'superTableReference', 'UClass', 'CDO',
                    'declaredNetworkMembers', 'directBNetInitialRotationTags'
                ) 'class-artifact class'
                Assert-ExactJsonPropertySet $classRecord.UClass @(
                    'exportIndex', 'uobjectNetIndex'
                ) 'class-artifact UClass identity'
                Assert-ExactJsonPropertySet $classRecord.CDO @(
                    'path', 'exportIndex', 'uobjectNetIndex',
                    'classTableReference', 'classLinkVerified', 'serialOffset',
                    'serialSize', 'objectFlags'
                ) 'class-artifact CDO identity'
                Assert-True ($classRecord.CDO.classLinkVerified -is [bool] -and
                    $classRecord.classPath -is [string] -and
                    ($null -eq $classRecord.superClassPath -or
                        $classRecord.superClassPath -is [string]) -and
                    $classRecord.superTableReference -is [ValueType] -and
                    $classRecord.UClass.exportIndex -is [ValueType] -and
                    $classRecord.UClass.uobjectNetIndex -is [ValueType] -and
                    $classRecord.CDO.path -is [string] -and
                    $classRecord.CDO.exportIndex -is [ValueType] -and
                    $classRecord.CDO.uobjectNetIndex -is [ValueType] -and
                    $classRecord.CDO.classTableReference -is [ValueType] -and
                    $classRecord.CDO.serialOffset -is [ValueType] -and
                    $classRecord.CDO.serialSize -is [ValueType] -and
                    $classRecord.CDO.objectFlags -is [string]) `
                    'class/CDO identities must retain exact boolean and numeric JSON types'
                foreach ($requiredClassKey in @(
                    'classPath', 'superClassPath', 'superTableReference',
                    'UClass', 'CDO', 'declaredNetworkMembers',
                    'directBNetInitialRotationTags')) {
                    Assert-True ($classRecord.PSObject.Properties.Name -contains
                        $requiredClassKey) `
                        "class-artifact record must retain $requiredClassKey"
                }
                foreach ($member in @($classRecord.declaredNetworkMembers)) {
                    Assert-ExactJsonPropertySet $member @(
                        'name', 'kind', 'uobjectNetIndex', 'propertyFlags',
                        'functionFlags', 'uelibType', 'arrayDim',
                        'functionSuperPresent', 'specialDeclaration'
                    ) 'class-artifact declared member'
                    Assert-True ($member.kind -in @('property', 'function') -and
                        $member.name -is [string] -and
                        $member.uelibType -is [string] -and
                        $member.uobjectNetIndex -is [ValueType] -and
                        $member.specialDeclaration -is [bool]) `
                        'declared member discriminator, index, and special marker types must be exact'
                    Assert-True (($member.kind -eq 'property' -and
                            $member.propertyFlags -is [string] -and
                            $null -eq $member.functionFlags -and
                            $member.arrayDim -is [ValueType] -and
                            $null -eq $member.functionSuperPresent) -or
                        ($member.kind -eq 'function' -and
                            $null -eq $member.propertyFlags -and
                            $member.functionFlags -is [string] -and
                            $null -eq $member.arrayDim -and
                            $member.functionSuperPresent -is [bool])) `
                        'member kind must select the exact property/function nullable fields'
                }
                foreach ($tag in @($classRecord.directBNetInitialRotationTags)) {
                    Assert-ExactJsonPropertySet $tag @(
                        'propertyPath', 'value', 'valueState', 'sourceOffset'
                    ) 'class-artifact direct CDO bool tag'
                    Assert-True ($tag.propertyPath -is [string] -and
                        $tag.valueState -in @(
                            'explicit', 'unknown',
                            'duplicate-explicit', 'duplicate-unknown') -and
                        ($null -eq $tag.value -or $tag.value -is [bool]) -and
                        $tag.sourceOffset -is [ValueType]) `
                        'direct CDO bool-tag value, state, and offset types must be exact'
                }
                Assert-True (@($classRecord.directBNetInitialRotationTags).Count -eq 0) `
                    'selected capture-compatible CDOs must pin direct bNetInitialRotation tag absence'
            }
            Assert-ExactJsonPropertySet $classArtifactRecords[-1] @(
                'record', 'schema', 'mode', 'classesEmitted',
                'classLinksVerifiedDirectly', 'wireStaticReferencesDerived',
                'reportAuthorizesRuntime'
            ) 'class-artifact summary'
            $classSummary = $classArtifactRecords[-1]
            Assert-True ($classSummary.record -eq 'summary' -and
                $classSummary.schema -eq 'rs2.cooked-class-artifacts.raw.v1' -and
                $classSummary.mode -eq 'class-artifacts' -and
                $classSummary.classesEmitted -eq $case.Count -and
                $classSummary.classesEmitted -is [ValueType] -and
                $classSummary.classLinksVerifiedDirectly -is [bool] -and
                $classSummary.classLinksVerifiedDirectly -and
                $classSummary.wireStaticReferencesDerived -is [bool] -and
                -not $classSummary.wireStaticReferencesDerived -and
                $classSummary.reportAuthorizesRuntime -is [bool] -and
                -not $classSummary.reportAuthorizesRuntime) `
                'class-artifact summary must pin verified links and fail closed on wire/runtime claims'
            foreach ($record in $classArtifactRecords) {
                Assert-NoForbiddenClassArtifactKeys $record
            }
            Assert-True ($rawClassArtifactJson -notmatch `
                '"(objectBase|staticReference|wireReference)"\s*:') `
                'raw class-artifact JSON must contain no forbidden runtime-reference key'

            foreach ($targetSpec in @($case.Targets)) {
                $target = @($classRecords | Where-Object {
                    $_.classPath -match ("\." + [Regex]::Escape($targetSpec.Class) + "'$")
                })
                Assert-True ($target.Count -eq 1) `
                    "$($case.Package).$($targetSpec.Class) must be emitted exactly once"
                if ($null -ne $targetSpec.UClassNetIndex) {
                    Assert-True ($target[0].UClass.uobjectNetIndex -eq
                            $targetSpec.UClassNetIndex -and
                        $target[0].CDO.uobjectNetIndex -eq
                            $targetSpec.CdoNetIndex) `
                        "$($targetSpec.Class) UClass/CDO UObject.NetIndex identities must remain exact"
                }
                if ($targetSpec.Class -eq 'Actor') {
                    $rotationDeclaration = @(
                        $target[0].declaredNetworkMembers | Where-Object {
                            $_.name -eq 'bNetInitialRotation' -and
                            $_.kind -eq 'property' -and $_.specialDeclaration
                        }
                    )
                    Assert-True ($rotationDeclaration.Count -eq 1) `
                        'Actor.bNetInitialRotation must be emitted as its exact special declaration'
                    Assert-True ($rotationDeclaration[0].uelibType -eq
                            'UELib.Core.UBoolProperty' -and
                        $rotationDeclaration[0].arrayDim -eq 1 -and
                        $rotationDeclaration[0].propertyFlags -eq
                            '0x0000000000000002' -and
                        (([Convert]::ToUInt64(
                            $rotationDeclaration[0].propertyFlags.Substring(2),
                            16) -band 0x20) -eq 0) -and
                        $null -eq $rotationDeclaration[0].functionFlags -and
                        $null -eq $rotationDeclaration[0].functionSuperPresent) `
                        'Actor.bNetInitialRotation must preserve its exact Const/non-CPF_Net UBoolProperty declaration facts'
                }
            }
            Assert-True ((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -eq
                $beforeClassArtifactHash) `
                "$($case.Package).u must remain byte-identical after extraction"
        }
    }
    elseif ($RequireIntegration) {
        throw ('Class-artifact integration packages are required but missing: ' +
            (($missingClassArtifactPackages | ForEach-Object Package) -join ', '))
    }
    else {
        Write-Output 'SKIP: capture-compatible class-artifact package set is not installed.'
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

    Write-Output 'PASS: cooked-map extractor compile, class artifacts, validation, roles, bounds, JSONL, actor metadata, and read-only hash checks.'
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
