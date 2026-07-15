<#
.SYNOPSIS
Build and run the bounded, read-only cooked-map metadata extractor.

.DESCRIPTION
Compiles tools/CookedMapMetadataExtractor.cs with the installed Visual Studio
Roslyn compiler into a content-addressed LocalAppData cache, then inspects one
.roe map or root ROGame.u package through Eliot.UELib.dll. The cache identity
pins the extractor source, wrapper, and compiler bytes. The extractor opens the
package with FileAccess.Read and never writes to the input package.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$InputPath,

    [string]$OutputPath,

    [string]$UELibPath = 'D:\RE-Tools\UE-Explorer\Eliot.UELib.dll',

    [ValidateRange(1, 8192)]
    [int]$MaxInputMiB = 512,

    [ValidateRange(1, 2000000)]
    [int]$MaxExports = 250000,

    [ValidateRange(1, 100000)]
    [int]$MaxActors = 1000,

    [ValidateRange(1, 4096)]
    [int]$MaxProperties = 64,

    [ValidateRange(16, 65536)]
    [int]$MaxValueChars = 1024,

    [ValidateRange(1, 1000)]
    [int]$MaxErrors = 25,

    [ValidateRange(1, 10000)]
    [int]$MaxClasses = 512,

    [string]$ClassPattern,

    [string]$PropertyPattern,

    [switch]$AllProperties,

    [switch]$ClassesOnly,

    [switch]$RoleInfo,

    [switch]$RoleExports,

    [switch]$BrushBounds,

    [string]$ExpectedPackageGuid,

    [string]$ExpectedSha256,

    [string]$ExpectedExecutableSha256,

    [ValidateRange(-1, 2147483647)]
    [long]$ObjectBase = -1,

    [switch]$Overwrite,

    [switch]$ForceRebuild,

    [switch]$BuildOnly,

    [string]$CompilerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Get-DirectoryManifestSha256 {
    param([Parameter(Mandatory = $true)][string]$Root)

    $resolvedRoot = [IO.Path]::GetFullPath($Root)
    $files = @(Get-ChildItem -LiteralPath $resolvedRoot -File -Recurse | Sort-Object FullName)
    if ($files.Count -eq 0) {
        throw "Compiler directory is empty: $resolvedRoot"
    }
    $manifest = New-Object Text.StringBuilder
    foreach ($file in $files) {
        $relativePath = $file.FullName.Substring($resolvedRoot.Length + 1)
        $fileHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
        [void]$manifest.Append($relativePath).Append("`0")
        [void]$manifest.Append($file.Length.ToString([Globalization.CultureInfo]::InvariantCulture)).Append("`0")
        [void]$manifest.Append($fileHash).Append("`n")
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($manifest.ToString())
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '')
    }
    finally {
        $hasher.Dispose()
    }
}

function Publish-AtomicFile {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $backup = $Destination + '.' + [Guid]::NewGuid().ToString('N') + '.bak'
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        try {
            [IO.File]::Replace($Candidate, $Destination, $backup)
        }
        finally {
            if (Test-Path -LiteralPath $backup -PathType Leaf) {
                Remove-Item -LiteralPath $backup -Force
            }
        }
        return
    }
    try {
        [IO.File]::Move($Candidate, $Destination)
    }
    catch [IO.IOException] {
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
            throw
        }
        try {
            [IO.File]::Replace($Candidate, $Destination, $backup)
        }
        finally {
            if (Test-Path -LiteralPath $backup -PathType Leaf) {
                Remove-Item -LiteralPath $backup -Force
            }
        }
    }
}

$sourcePath = Join-Path $PSScriptRoot 'CookedMapMetadataExtractor.cs'
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "Extractor source not found: $sourcePath"
}

if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "Visual Studio locator not found: $vswhere"
    }
    $CompilerPath = @(
        & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.Roslyn.Compiler `
            -find 'MSBuild\Current\Bin\Roslyn\csc.exe'
    ) | Select-Object -First 1
}

if ([string]::IsNullOrWhiteSpace($CompilerPath) -or
    -not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw 'The installed Visual Studio Roslyn compiler (csc.exe) was not found.'
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedExecutableSha256)) {
    $ExpectedExecutableSha256 = $ExpectedExecutableSha256.Trim().ToUpperInvariant()
    if ($ExpectedExecutableSha256 -notmatch '^[0-9A-F]{64}$') {
        throw 'ExpectedExecutableSha256 must contain exactly 64 hexadecimal digits.'
    }
}

$sourceSha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToUpperInvariant()
$wrapperSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToUpperInvariant()
$compilerRoot = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($CompilerPath))
$compilerManifestSha256 = Get-DirectoryManifestSha256 -Root $compilerRoot
$cacheIdentity = [Text.Encoding]::UTF8.GetBytes(
    "cooked-map-metadata-v3`n$sourceSha256`n$wrapperSha256`n$compilerManifestSha256"
)
$cacheHasher = [Security.Cryptography.SHA256]::Create()
try {
    $cacheKey = ([BitConverter]::ToString($cacheHasher.ComputeHash($cacheIdentity))).Replace('-', '')
}
finally {
    $cacheHasher.Dispose()
}
$cacheBaseRoot = Join-Path $env:LOCALAPPDATA 'smellslikenapalm\tools\cooked-map-metadata\v3'
$cacheRoot = Join-Path $cacheBaseRoot $cacheKey
$executablePath = Join-Path $cacheRoot 'CookedMapMetadataExtractor.exe'
$executableHashPath = $executablePath + '.sha256'
$needsBuild = $ForceRebuild -or
    -not (Test-Path -LiteralPath $executablePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $executableHashPath -PathType Leaf)
if (-not $needsBuild) {
    $recordedExecutableSha256 = [IO.File]::ReadAllText($executableHashPath).Trim().ToUpperInvariant()
    $actualExecutableSha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToUpperInvariant()
    $needsBuild = $recordedExecutableSha256 -notmatch '^[0-9A-F]{64}$' -or
        $actualExecutableSha256 -ne $recordedExecutableSha256
}

if ($needsBuild) {
    [IO.Directory]::CreateDirectory($cacheRoot) | Out-Null
    $temporaryBuildRoot = Join-Path $cacheRoot (
        'build-' + [Guid]::NewGuid().ToString('N')
    )
    [IO.Directory]::CreateDirectory($temporaryBuildRoot) | Out-Null
    $temporaryExecutablePath = Join-Path $temporaryBuildRoot 'CookedMapMetadataExtractor.exe'
    $temporaryHashPath = $temporaryExecutablePath + '.sha256'
    $compilerArguments = @(
        '/nologo',
        '/target:exe',
        '/optimize+',
        '/deterministic+',
        '/warn:4',
        '/warnaserror+',
        '/langversion:7.3',
        '/reference:Microsoft.CSharp.dll',
        "/out:$temporaryExecutablePath",
        $sourcePath
    )
    try {
        & $CompilerPath @compilerArguments
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $temporaryExecutablePath -PathType Leaf)) {
            throw "Cooked-map extractor compilation failed with exit code $LASTEXITCODE."
        }

        $sourceSha256AfterCompile = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToUpperInvariant()
        $wrapperSha256AfterCompile = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToUpperInvariant()
        $compilerManifestAfterCompile = Get-DirectoryManifestSha256 -Root $compilerRoot
        if ($sourceSha256AfterCompile -ne $sourceSha256 -or
            $wrapperSha256AfterCompile -ne $wrapperSha256 -or
            $compilerManifestAfterCompile -ne $compilerManifestSha256) {
            throw 'Extractor source, wrapper, or compiler closure changed during compilation.'
        }

        $builtExecutableSha256 = (Get-FileHash -LiteralPath $temporaryExecutablePath -Algorithm SHA256).Hash.ToUpperInvariant()
        if (-not [string]::IsNullOrWhiteSpace($ExpectedExecutableSha256) -and
            $builtExecutableSha256 -ne $ExpectedExecutableSha256) {
            throw "Compiled extractor SHA-256 mismatch before publication: expected $ExpectedExecutableSha256 but found $builtExecutableSha256."
        }
        [IO.File]::WriteAllText(
            $temporaryHashPath,
            $builtExecutableSha256 + "`n",
            (New-Object Text.UTF8Encoding($false))
        )
        Publish-AtomicFile -Candidate $temporaryExecutablePath -Destination $executablePath
        Publish-AtomicFile -Candidate $temporaryHashPath -Destination $executableHashPath
    }
    finally {
        if (Test-Path -LiteralPath $temporaryExecutablePath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryExecutablePath -Force
        }
        if (Test-Path -LiteralPath $temporaryHashPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryHashPath -Force
        }
        if (Test-Path -LiteralPath $temporaryBuildRoot -PathType Container) {
            Remove-Item -LiteralPath $temporaryBuildRoot -Force
        }
    }
}

$sourceSha256AfterBuild = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToUpperInvariant()
$wrapperSha256AfterBuild = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToUpperInvariant()
$compilerManifestSha256AfterBuild = Get-DirectoryManifestSha256 -Root $compilerRoot
if ($sourceSha256AfterBuild -ne $sourceSha256 -or
    $wrapperSha256AfterBuild -ne $wrapperSha256 -or
    $compilerManifestSha256AfterBuild -ne $compilerManifestSha256) {
    throw 'Extractor source, wrapper, or compiler closure changed while the cached executable was selected or built.'
}
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $executableHashPath -PathType Leaf)) {
    throw 'Compiled extractor cache entry is incomplete.'
}
$recordedExecutableSha256 = [IO.File]::ReadAllText($executableHashPath).Trim().ToUpperInvariant()
$actualExecutableSha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToUpperInvariant()
if ($recordedExecutableSha256 -notmatch '^[0-9A-F]{64}$' -or
    $actualExecutableSha256 -ne $recordedExecutableSha256) {
    throw 'Compiled extractor cache entry failed its executable hash manifest.'
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedExecutableSha256)) {
    if ($actualExecutableSha256 -ne $ExpectedExecutableSha256) {
        throw "Compiled extractor SHA-256 mismatch: expected $ExpectedExecutableSha256 but found $actualExecutableSha256."
    }
}

if ($BuildOnly) {
    Write-Output $executablePath
    exit 0
}

if ([string]::IsNullOrWhiteSpace($InputPath)) {
    throw 'InputPath is required unless -BuildOnly is used.'
}
$exclusiveModes = @(@($ClassesOnly, $RoleInfo, $RoleExports, $BrushBounds) | Where-Object { $_ })
if ($exclusiveModes.Count -gt 1) {
    throw '-ClassesOnly, -RoleInfo, -RoleExports, and -BrushBounds are mutually exclusive.'
}

# ValidateRange caps this multiplication at 8 GiB, well inside Int64.
$maxInputBytes = [long]$MaxInputMiB * 1024L * 1024L
$extractorArguments = @(
    '--input', $InputPath,
    '--uelib', $UELibPath,
    '--mode', $(if ($ClassesOnly) { 'classes' } elseif ($RoleInfo) { 'role-info' } elseif ($RoleExports) { 'role-exports' } elseif ($BrushBounds) { 'brush-bounds' } else { 'actors' }),
    '--max-input-bytes', $maxInputBytes.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-exports', $MaxExports.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-actors', $MaxActors.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-properties', $MaxProperties.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-value-chars', $MaxValueChars.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-errors', $MaxErrors.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-classes', $MaxClasses.ToString([Globalization.CultureInfo]::InvariantCulture)
)

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $extractorArguments += @('--output', $OutputPath)
}
if (-not [string]::IsNullOrWhiteSpace($ClassPattern)) {
    $extractorArguments += @('--class-pattern', $ClassPattern)
}
if ($AllProperties) {
    $extractorArguments += @('--property-pattern', '.*')
} elseif (-not [string]::IsNullOrWhiteSpace($PropertyPattern)) {
    $extractorArguments += @('--property-pattern', $PropertyPattern)
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedPackageGuid)) {
    $extractorArguments += @('--expected-package-guid', $ExpectedPackageGuid)
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
    $extractorArguments += @('--expected-sha256', $ExpectedSha256)
}
if ($ObjectBase -ge 0) {
    $extractorArguments += @(
        '--object-base',
        $ObjectBase.ToString([Globalization.CultureInfo]::InvariantCulture)
    )
}
if ($Overwrite) {
    $extractorArguments += '--overwrite'
}

& $executablePath @extractorArguments
exit $LASTEXITCODE
