# Dump net-field TYPES for a class chain (companion to netfields_from_u.ps1).
param([string]$Class = "ROPlayerReplicationInfo")
$ErrorActionPreference = "Stop"
[void][System.Reflection.Assembly]::LoadFrom("D:\RE-Tools\UE-Explorer\Eliot.UELib.dll")
$CPF_Net = [uint32]0x20; $FUNC_Net = [uint32]0x40
$BREW = "D:\rs2dedicatedserver\ROGame\BrewedPCServer"
$chains = @{
  "ROPlayerReplicationInfo" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Info";pkg="Engine.u" },
    @{ cls="ReplicationInfo";pkg="Engine.u" }, @{ cls="PlayerReplicationInfo";pkg="Engine.u" },
    @{ cls="ROPlayerReplicationInfo";pkg="ROGame.u" })
}
$chain = $chains[$Class]
$pkgCache = @{}
function Get-Pkg($name) {
  if (-not $pkgCache.ContainsKey($name)) {
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
$gh=0
foreach ($entry in $chain) {
  $pkg = Get-Pkg $entry.pkg
  $cls = Find-Class $pkg $entry.cls
  if ($null -eq $cls) { continue }
  $net = New-Object System.Collections.ArrayList
  $allFuncNames = @()
  foreach ($f in $cls.EnumerateFields()) {
    $fn = $f.Name.ToString(); $ni = [int]$f.NetIndex
    if ($f -is [UELib.Core.UFunction]) {
      $allFuncNames += $fn
      if ($f.HasFunctionFlag($FUNC_Net) -and ($null -eq $f.Super) -and (-not $anc.Contains($fn))) {
        [void]$net.Add(@{ ni=$ni; kind="func"; name=$fn; type="(func)"; obj=$f })
      }
    } elseif ($f -is [UELib.Core.UProperty]) {
      if ($f.HasPropertyFlag($CPF_Net)) {
        $tn = $f.GetType().Name
        $extra = ""
        $arrdim = 1
        try { $arrdim = [int]$f.ArrayDim } catch {}
        if ($f -is [UELib.Core.UArrayProperty]) {
          try { $extra = "<" + $f.InnerProperty.GetType().Name + ">" } catch {}
        }
        if ($f -is [UELib.Core.UStructProperty]) {
          try { $extra = "{" + $f.StructObject.Name.ToString() + "}" } catch {}
        }
        if ($f -is [UELib.Core.UByteProperty]) {
          try { if ($f.EnumObject -ne $null) { $extra = "enum:" + $f.EnumObject.Name.ToString() } } catch {}
        }
        if ($f -is [UELib.Core.UObjectProperty]) {
          try { $extra = "->" + $f.Object.Name.ToString() } catch {}
        }
        [void]$net.Add(@{ ni=$ni; kind="prop"; name=$fn; type=($tn+$extra); obj=$f; arrdim=$arrdim })
      }
    }
  }
  $sorted = $net | Sort-Object { $_.ni }
  foreach ($e in $sorted) {
    $ad = ""
    if ($e.kind -eq "prop" -and $e.arrdim -gt 1) { $ad = " [ArrayDim=$($e.arrdim)]" }
    Write-Host ("{0,4} {1,-22} {2,-5} {3,-28} {4}{5}" -f $gh, $entry.cls, $e.kind, $e.name, $e.type, $ad)
    $gh++
  }
  foreach ($n in $allFuncNames) { [void]$anc.Add($n) }
}
Write-Host ""
Write-Host "maxHandle = $gh"
