# Derive EXACT UE3 ClassNetCache handles + UProperty TYPES from compiled .u packages via UELib.
# For each class chain (super-first): NetFields = CPF_Net properties + FUNC_Net functions
#   first-declared (Super==null), SORTED BY NetIndex (UClass::Link).
#   Wire handle = FieldsBase(class) + rank-in-sorted. maxHandle = total over chain.
# Also resolves each net property's UProperty subclass (type) and element/ref detail.
param([string]$Class = "ALL")
$ErrorActionPreference = "Stop"
[void][System.Reflection.Assembly]::LoadFrom("D:\RE-Tools\UE-Explorer\Eliot.UELib.dll")
$CPF_Net = [uint32]0x20; $FUNC_Net = [uint32]0x40
$BREW = "D:\rs2dedicatedserver\ROGame\BrewedPCServer"

$chains = @{
  "ROPlayerController" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" },
    @{ cls="Controller";pkg="Engine.u" }, @{ cls="PlayerController";pkg="Engine.u" },
    @{ cls="GamePlayerController";pkg="GameFramework.u" }, @{ cls="ROPlayerController";pkg="ROGame.u" })
  "ROGameReplicationInfo" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Info";pkg="Engine.u" },
    @{ cls="ReplicationInfo";pkg="Engine.u" }, @{ cls="GameReplicationInfo";pkg="Engine.u" },
    @{ cls="ROGameReplicationInfo";pkg="ROGame.u" })
  "ROTeamInfo" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Info";pkg="Engine.u" },
    @{ cls="ReplicationInfo";pkg="Engine.u" }, @{ cls="TeamInfo";pkg="Engine.u" },
    @{ cls="ROTeamInfo";pkg="ROGame.u" })
  "ROPlayerReplicationInfo" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Info";pkg="Engine.u" },
    @{ cls="ReplicationInfo";pkg="Engine.u" }, @{ cls="PlayerReplicationInfo";pkg="Engine.u" },
    @{ cls="ROPlayerReplicationInfo";pkg="ROGame.u" })
  "ROPawn" = @(
    @{ cls="Object";pkg="Core.u" }, @{ cls="Actor";pkg="Engine.u" }, @{ cls="Pawn";pkg="Engine.u" },
    @{ cls="GamePawn";pkg="GameFramework.u" }, @{ cls="ROPawn";pkg="ROGame.u" })
}

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
# Try several member names on an object; return first non-null .ToString()
function Try-Member($obj, [string[]]$names) {
  foreach ($n in $names) {
    try {
      $p = $obj.GetType().GetProperty($n)
      if ($p) { $v = $p.GetValue($obj); if ($null -ne $v) { return $v } }
    } catch {}
    try {
      $fld = $obj.GetType().GetField($n)
      if ($fld) { $v = $fld.GetValue($obj); if ($null -ne $v) { return $v } }
    } catch {}
  }
  return $null
}
# Build a readable type string for a UProperty.
function Get-PropTypeCore($p) {
  $t = $p.GetType().Name   # e.g. UIntProperty, UObjectProperty, UStructProperty
  $short = $t -replace '^U','' -replace 'Property$',''
  switch -Regex ($t) {
    'UByteProperty' {
      $e = Try-Member $p @('Enum','EnumObject','Object')
      if ($e) { return "byte(enum $($e.Name))" } else { return "byte" }
    }
    'UBoolProperty'   { return "bool" }
    'UIntProperty'    { return "int" }
    'UFloatProperty'  { return "float" }
    'UStrProperty'    { return "string" }
    'UNameProperty'   { return "name" }
    'UClassProperty'  { $m = Try-Member $p @('MetaClass','Object'); if ($m){return "class<$($m.Name)>"} else {return "class"} }
    'U(Object|Component|Interface)Property' {
      $o = Try-Member $p @('Object'); if ($o){return "obj<$($o.Name)>"} else {return "obj"} }
    'UStructProperty' {
      $s = Try-Member $p @('Struct','StructObject','Object'); if ($s){return "struct $($s.Name)"} else {return "struct" } }
    'UArrayProperty' {
      $inner = Try-Member $p @('InnerProperty','Inner')
      if ($inner) { return "array<" + (Get-PropType $inner) + ">" } else { return "array" } }
    'UDelegateProperty' { return "delegate" }
    default { return $short }
  }
}
# Wrap with static-array dim. ArrayDim>1 => one net handle, but wire sends
# SerializeInt(elemIndex, ArrayDim) right after the handle, before each value.
function Get-PropType($p) {
  $core = Get-PropTypeCore $p
  $dim = 1
  try { $dim = [int]$p.ArrayDim } catch {}
  if ($dim -gt 1) { return "$core[$dim]" } else { return $core }
}

function Process-Class([string]$ClassName) {
  $chain = $chains[$ClassName]
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
          [void]$net.Add(@{ ni=$ni; kind="func"; name=$fn; type="(rpc)" })
        }
      } elseif ($f -is [UELib.Core.UProperty]) {
        if ($f.HasPropertyFlag($CPF_Net)) {
          $ty = Get-PropType $f
          [void]$net.Add(@{ ni=$ni; kind="prop"; name=$fn; type=$ty })
        }
      }
    }
    $sorted = $net | Sort-Object { $_.ni }
    $nprops = ($sorted | Where-Object { $_.kind -eq "prop" }).Count
    $nfuncs = ($sorted | Where-Object { $_.kind -eq "func" }).Count
    $results += @{ cls=$entry.cls; nprops=$nprops; nfuncs=$nfuncs; count=$sorted.Count; fb=$fieldsBase }
    foreach ($e in $sorted) {
      $globalLines += ("{0,4}  {1,-22} {2,-4} {3,-34} {4,-22} ni={5}" -f $gh, $entry.cls, $e.kind, $e.name, $e.type, $e.ni)
      $gh++
    }
    foreach ($n in $allFuncNames) { [void]$anc.Add($n) }
    $fieldsBase += $sorted.Count
  }
  $outName = "netfields_u_$ClassName.txt"
  $header = @()
  $header += "# $ClassName  maxHandle=$fieldsBase"
  $header += ("{0,4}  {1,-22} {2,-4} {3,-34} {4,-22} {5}" -f "h","class","kind","name","type","netindex")
  ($header + $globalLines) | Set-Content "D:\smellslikenapalm\tools\$outName" -Encoding utf8
  Write-Host ""
  Write-Host "===== $ClassName ====="
  Write-Host ("{0,-22} {1,9} {2,9} {3,6} {4,11}" -f "class","netProps","netFuncs","count","FieldsBase")
  foreach ($r in $results) { Write-Host ("{0,-22} {1,9} {2,9} {3,6} {4,11}" -f $r.cls,$r.nprops,$r.nfuncs,$r.count,$r.fb) }
  Write-Host "maxHandle($ClassName) = $fieldsBase"
  return [pscustomobject]@{ name=$ClassName; maxHandle=$fieldsBase; results=$results; lines=$globalLines }
}

$targets = if ($Class -eq "ALL") { @("ROPlayerController","ROGameReplicationInfo","ROTeamInfo","ROPlayerReplicationInfo","ROPawn") } else { @($Class) }
$all = @()
foreach ($t in $targets) { $all += (Process-Class $t) }

# Emit master markdown
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Net-field master reference (handle + type) for RS2V replication")
$md.Add("")
$md.Add("Generated by tools/netfields_all.ps1 from compiled .u (UELib over Eliot.UELib.dll).")
$md.Add("Handle = SerializeInt(handle, maxHandle); ordering = ClassNetCache (super-first, each class's net fields sorted by engine NetIndex).")
$md.Add("kind: prop = replicated UProperty (CPF_Net); func = replicated function / RPC (FUNC_Net, first-declared).")
$md.Add("")
$md.Add("### Wire / decode gotchas")
$md.Add("")
$md.Add("- Each net field is ONE handle even when it is a static array (type shown as ``T[N]``).")
$md.Add("  For a static array, the wire order is: ``SerializeInt(handle, maxHandle)`` then")
$md.Add("  ``SerializeInt(elemIndex, ArrayDim=N)`` then the element value. (maxHandle counts the")
$md.Add("  property once, NOT once per element - verified: handle totals match the known maxHandles.)")
$md.Add("- Scalar (non-array, N=1) fields: handle then value directly, no index.")
$md.Add("- Value encodings: byte=8b, bool=1b, int/float=32b, string=int32 len + chars,")
$md.Add("  name=SerializeName, struct=member-by-member, obj ref=1 selector bit +")
$md.Add("  SerializeInt(idx, 0x80000000 static / NetIndexCount dynamic).")
$md.Add("- ``struct Vector``/``Rotator`` replicate as compressed vectors/rotators, not 3x raw float.")
$md.Add("- GameReplicationInfo has NO replicated ``Teams``/``PRIArray`` net property: clients rebuild")
$md.Add("  those arrays from the individually-replicated ROTeamInfo and (RO)PlayerReplicationInfo")
$md.Add("  actors. Team-button data lives in ROTeamInfo net props (TeamIndex, etc.), not in GRI.")
$md.Add("")
$md.Add("## maxHandle summary")
$md.Add("")
$md.Add("| Class | maxHandle |")
$md.Add("|---|---|")
foreach ($a in $all) { $md.Add("| $($a.name) | $($a.maxHandle) |") }
$md.Add("")
foreach ($a in $all) {
  $md.Add("## $($a.name)  (maxHandle = $($a.maxHandle))")
  $md.Add("")
  $md.Add("Per-class net counts:")
  $md.Add("")
  $md.Add("| class | netProps | netFuncs | count | FieldsBase |")
  $md.Add("|---|---|---|---|---|")
  foreach ($r in $a.results) { $md.Add("| $($r.cls) | $($r.nprops) | $($r.nfuncs) | $($r.count) | $($r.fb) |") }
  $md.Add("")
  $md.Add("Full ordered field table:")
  $md.Add("")
  $md.Add("| handle | class | kind | name | type | netIndex |")
  $md.Add("|---|---|---|---|---|---|")
  foreach ($ln in $a.lines) {
    if ($ln -match '^\s*(\d+)\s+(\S+)\s+(prop|func)\s+(\S+)\s+(.+?)\s+ni=(\d+)\s*$') {
      $md.Add("| $($matches[1]) | $($matches[2]) | $($matches[3]) | $($matches[4]) | $($matches[5].Trim()) | $($matches[6]) |")
    }
  }
  $md.Add("")
}
$null = New-Item -ItemType Directory -Force "D:\smellslikenapalm\docs\re"
$md -join "`r`n" | Set-Content "D:\smellslikenapalm\docs\re\netfields_all_classes.md" -Encoding utf8
Write-Host ""
Write-Host "Wrote D:\smellslikenapalm\docs\re\netfields_all_classes.md"
