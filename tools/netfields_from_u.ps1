# Derive EXACT UE3 ClassNetCache handles from compiled .u packages via UELib.
# Ground truth: each UField carries its real engine NetIndex (UObject.NetIndex).
# NetFields(class) = CPF_Net properties + FUNC_Net functions first-declared (Super==null),
#   SORTED BY NetIndex (UClass::Link). Wire handle = FieldsBase(class) + rank-in-sorted.
# maxHandle(ROPlayerController) = sum of NetFields counts over the whole chain.
$ErrorActionPreference = "Stop"
[void][System.Reflection.Assembly]::LoadFrom("D:\RE-Tools\UE-Explorer\Eliot.UELib.dll")
$CPF_Net = [uint32]0x20; $FUNC_Net = [uint32]0x40
$BREW = "D:\rs2dedicatedserver\ROGame\BrewedPCServer"
$chain = @(
  @{ cls="Object";               pkg="Core.u" },
  @{ cls="Actor";                pkg="Engine.u" },
  @{ cls="Controller";           pkg="Engine.u" },
  @{ cls="PlayerController";     pkg="Engine.u" },
  @{ cls="GamePlayerController"; pkg="GameFramework.u" },
  @{ cls="ROPlayerController";   pkg="ROGame.u" }
)
$pkgCache = @{}
function Get-Pkg($name) {
  if (-not $pkgCache.ContainsKey($name)) {
    Write-Host "  loading $name ..."
    $pkgCache[$name] = [UELib.UnrealLoader]::LoadFullPackage((Join-Path $BREW $name), [System.IO.FileAccess]::Read)
  }
  return $pkgCache[$name]
}
function Find-Class($pkg, $name) {
  foreach ($o in $pkg.Objects) {
    if ($o -is [UELib.Core.UClass] -and $o.Name.ToString() -eq $name -and $o.ExportTable -ne $null) { return $o }
  }
  return $null
}
$anc = New-Object System.Collections.Generic.HashSet[string]
$fieldsBase = 0; $results=@(); $globalLines=@(); $gh=0
foreach ($entry in $chain) {
  $pkg = Get-Pkg $entry.pkg
  $cls = Find-Class $pkg $entry.cls
  if ($null -eq $cls) { Write-Host "MISSING $($entry.cls)"; continue }
  $net = New-Object System.Collections.ArrayList
  $allFuncNames = @()
  foreach ($f in $cls.EnumerateFields()) {
    $fn = $f.Name.ToString(); $ni = [int]$f.NetIndex
    if ($f -is [UELib.Core.UFunction]) {
      $allFuncNames += $fn
      if ($f.HasFunctionFlag($FUNC_Net) -and ($null -eq $f.Super) -and (-not $anc.Contains($fn))) {
        [void]$net.Add(@{ ni=$ni; kind="func"; name=$fn })
      }
    } elseif ($f -is [UELib.Core.UProperty]) {
      if ($f.HasPropertyFlag($CPF_Net)) { [void]$net.Add(@{ ni=$ni; kind="prop"; name=$fn }) }
    }
  }
  # SORT by real NetIndex (the engine's GetNetIndex sort key)
  $sorted = $net | Sort-Object { $_.ni }
  $nprops = ($sorted | Where-Object { $_.kind -eq "prop" }).Count
  $nfuncs = ($sorted | Where-Object { $_.kind -eq "func" }).Count
  $results += @{ cls=$entry.cls; nprops=$nprops; nfuncs=$nfuncs; count=$sorted.Count; fb=$fieldsBase }
  foreach ($e in $sorted) { $globalLines += ("{0,4} {1,-22} {2} {3} ni={4}" -f $gh, $entry.cls, $e.kind, $e.name, $e.ni); $gh++ }
  foreach ($n in $allFuncNames) { [void]$anc.Add($n) }
  $fieldsBase += $sorted.Count
}
$globalLines | Set-Content "D:\smellslikenapalm\tools\netfields_u_global.txt" -Encoding utf8
Write-Host ""
Write-Host ("{0,-22} {1,9} {2,9} {3,6} {4,11}" -f "class","netProps","netFuncs","count","FieldsBase")
foreach ($r in $results) { Write-Host ("{0,-22} {1,9} {2,9} {3,6} {4,11}" -f $r.cls,$r.nprops,$r.nfuncs,$r.count,$r.fb) }
Write-Host ""
Write-Host "maxHandle = $fieldsBase"
Write-Host ""
Write-Host "Menu RPCs + anchors:"
foreach ($ln in $globalLines) {
  if ($ln -match "ClientShowTeamSelect|ChangedTeams|ClientShowRoleSelect|ShortClientAdjustPosition|ClientAdjustPosition\b|ServerMove\b|ClientSetHUD") { Write-Host "  $ln" }
}
