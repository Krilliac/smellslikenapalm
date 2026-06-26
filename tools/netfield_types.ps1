# Dump each CPF_Net property's UProperty subclass (type) for a class chain.
param([string]$Class = "ROGameReplicationInfo")
$ErrorActionPreference = "Stop"
[void][System.Reflection.Assembly]::LoadFrom("D:\RE-Tools\UE-Explorer\Eliot.UELib.dll")
$CPF_Net = [uint32]0x20
$BREW = "D:\rs2dedicatedserver\ROGame\BrewedPCServer"
$chains = @{
  "ROGameReplicationInfo" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Info";pkg="Engine.u" },
    @{ cls="ReplicationInfo";pkg="Engine.u" }, @{ cls="GameReplicationInfo";pkg="Engine.u" },
    @{ cls="ROGameReplicationInfo";pkg="ROGame.u" })
  "ROTeamInfo" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Info";pkg="Engine.u" },
    @{ cls="ReplicationInfo";pkg="Engine.u" }, @{ cls="TeamInfo";pkg="Engine.u" },
    @{ cls="ROTeamInfo";pkg="ROGame.u" })
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
function Prop-Type($p) {
  $t = $p.GetType().Name   # e.g. UIntProperty, UByteProperty...
  $extra = ""
  try {
    if ($t -eq "UArrayProperty") {
      $inner = $p.InnerProperty
      if ($inner) { $extra = "<" + $inner.GetType().Name + ">" }
    } elseif ($t -eq "UByteProperty") {
      $en = $p.EnumObject
      if ($en) { $extra = "(enum " + $en.Name.ToString() + ")" }
    } elseif ($t -eq "UStructProperty") {
      $st = $p.StructObject
      if ($st) { $extra = "(" + $st.Name.ToString() + ")" }
    } elseif ($t -eq "UObjectProperty" -or $t -eq "UComponentProperty" -or $t -eq "UClassProperty" -or $t -eq "UInterfaceProperty") {
      $oc = $p.Object   # PropertyClass-ish
      if ($oc) { $extra = "(" + $oc.GetType().Name + ")" }
    }
  } catch {}
  return "$t$extra"
}
$lines = @()
$h = 0
foreach ($entry in $chain) {
  $pkg = Get-Pkg $entry.pkg
  $cls = Find-Class $pkg $entry.cls
  if ($null -eq $cls) { continue }
  $net = New-Object System.Collections.ArrayList
  foreach ($f in $cls.EnumerateFields()) {
    if ($f -is [UELib.Core.UProperty]) {
      if ($f.HasPropertyFlag($CPF_Net)) {
        [void]$net.Add(@{ ni=[int]$f.NetIndex; name=$f.Name.ToString(); p=$f; arr=[int]$f.ArrayDim })
      }
    }
  }
  $sorted = $net | Sort-Object { $_.ni }
  foreach ($e in $sorted) {
    $ty = Prop-Type $e.p
    $ad = if ($e.arr -gt 1) { " ArrayDim=$($e.arr)" } else { "" }
    $lines += ("{0,4}  {1,-30} {2,-34} ni={3}{4}" -f $h, $e.name, $ty, $e.ni, $ad)
    $h++
  }
}
$lines | Set-Content "D:\smellslikenapalm\tools\netfield_types_$Class.txt" -Encoding utf8
$lines | ForEach-Object { Write-Host $_ }
Write-Host "maxHandle = $h"
