<#
.SYNOPSIS
Build and run the bounded, read-only cooked-map metadata extractor.

.DESCRIPTION
Compiles tools/CookedMapMetadataExtractor.cs with the installed Visual Studio
Roslyn compiler into the user's LocalAppData cache, then inspects one .roe file
through Eliot.UELib.dll. The extractor opens the package with FileAccess.Read and
never writes to the input package.
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

    [switch]$BrushBounds,

    [switch]$Overwrite,

    [switch]$ForceRebuild,

    [switch]$BuildOnly,

    [string]$CompilerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

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

$cacheRoot = Join-Path $env:LOCALAPPDATA 'smellslikenapalm\tools\cooked-map-metadata'
$executablePath = Join-Path $cacheRoot 'CookedMapMetadataExtractor.exe'
$needsBuild = $ForceRebuild -or -not (Test-Path -LiteralPath $executablePath -PathType Leaf)
if (-not $needsBuild) {
    $needsBuild = (Get-Item -LiteralPath $sourcePath).LastWriteTimeUtc -gt
        (Get-Item -LiteralPath $executablePath).LastWriteTimeUtc
}

if ($needsBuild) {
    [IO.Directory]::CreateDirectory($cacheRoot) | Out-Null
    $compilerArguments = @(
        '/nologo',
        '/target:exe',
        '/optimize+',
        '/deterministic+',
        '/warn:4',
        '/warnaserror+',
        '/langversion:7.3',
        '/reference:Microsoft.CSharp.dll',
        "/out:$executablePath",
        $sourcePath
    )
    & $CompilerPath @compilerArguments
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Cooked-map extractor compilation failed with exit code $LASTEXITCODE."
    }
}

if ($BuildOnly) {
    Write-Output $executablePath
    exit 0
}

if ([string]::IsNullOrWhiteSpace($InputPath)) {
    throw 'InputPath is required unless -BuildOnly is used.'
}
$exclusiveModes = @(@($ClassesOnly, $RoleInfo, $BrushBounds) | Where-Object { $_ })
if ($exclusiveModes.Count -gt 1) {
    throw '-ClassesOnly, -RoleInfo, and -BrushBounds are mutually exclusive.'
}

# ValidateRange caps this multiplication at 8 GiB, well inside Int64.
$maxInputBytes = [long]$MaxInputMiB * 1024L * 1024L
$extractorArguments = @(
    '--input', $InputPath,
    '--uelib', $UELibPath,
    '--mode', $(if ($ClassesOnly) { 'classes' } elseif ($RoleInfo) { 'role-info' } elseif ($BrushBounds) { 'brush-bounds' } else { 'actors' }),
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
if ($Overwrite) {
    $extractorArguments += '--overwrite'
}

& $executablePath @extractorArguments
exit $LASTEXITCODE
