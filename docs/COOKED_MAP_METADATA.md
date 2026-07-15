# Cooked Map Metadata Extraction

`tools/extract_cooked_map_metadata.ps1` provides a read-only, bounded workflow
for inventorying actor metadata in installed RS2 `.roe` packages with
`Eliot.UELib.dll`. It is intended to replace guessed spawn/objective coordinates
with reproducible evidence before any runtime map data is edited.

The tool opens the package with `FileAccess.Read`. It writes only its requested
JSONL output and a compiled helper under the current user's LocalAppData cache;
it never rewrites the `.roe` file or `config/maps.ini`.

## Prerequisites

- An installed Visual Studio Roslyn C# compiler. The wrapper locates it with
  `vswhere.exe`.
- UE Explorer's `Eliot.UELib.dll` (validated with version 1.12.1), normally at
  `D:\RE-Tools\UE-Explorer\Eliot.UELib.dll`.
- Locally installed RS2 cooked maps, normally below
  `D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps`.

No game-server/CMake build is involved.

## Bulk package-GUID grounding

`tools/ground_cooked_map_guids.py` audits every section in `config/maps.ini`
against the marked `kMapPackageGuids` table in
`src/Network/RetailBootstrap.cpp`. For each configured map absent from that
table, it synchronously invokes the existing extractor with `-ClassesOnly` and
`-MaxClasses 1`. It never runs two package readers concurrently.

Run the RAM preflight, then write a provenance report outside the source tree:

```powershell
powershell -NoProfile -File "$HOME\.claude\scripts\fleet-preflight.ps1"
python tools\ground_cooked_map_guids.py `
  --output "$env:TEMP\rs2-map-guids.jsonl"
```

The helper streams SHA-256 before and after each extraction, the package-summary
GUID, and its 16 wire bytes. The final JSONL record is a compact summary. When
`--output` is supplied, only the summary is also printed to stdout; an existing
output is protected unless `--overwrite` is explicit.

This is an evidence generator, not a source generator: it does not edit the map
config, the `.roe` packages, or C++. Review successful records before adding
them to the constexpr table. It fails closed on source mutation, malformed or
zero GUIDs, duplicate map IDs/paths/GUIDs, resolver-table count drift, unsafe
output paths, and extractor contract violations. Ordinary missing/unreadable
packages are reported individually and produce a nonzero summary after the
remaining configured maps have been checked.

Run its focused stdlib tests without loading retail packages:

```powershell
python -m unittest tools.tests.test_ground_cooked_map_guids
```

## Safe first pass

Run the RAM preflight before opening a large gameplay map:

```powershell
powershell -NoProfile -File "$HOME\.claude\scripts\fleet-preflight.ps1"
```

Inventory matching export classes without initializing/deserializing actors:

```powershell
powershell -NoProfile -File tools\extract_cooked_map_metadata.ps1 `
  'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\Firebase\VNTE-Firebase.roe' `
  -ClassesOnly `
  -OutputPath "$env:TEMP\firebase-classes.jsonl"
```

Then extract selected actor properties on demand:

```powershell
powershell -NoProfile -File tools\extract_cooked_map_metadata.ps1 `
  'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\Firebase\VNTE-Firebase.roe' `
  -OutputPath "$env:TEMP\firebase-actors.jsonl" `
  -MaxActors 250 `
  -MaxProperties 64 `
  -MaxValueChars 1024
```

The default 512 MiB input cap deliberately rejects very large packages. Raise
`-MaxInputMiB` only after a GO preflight and after a class-only pass establishes
that the package is relevant. Do not start with the 1.1 GB Resort package.

Use `-ClassPattern` and `-PropertyPattern` for focused evidence. Both patterns
must match the full class/property name; matching is case-insensitive. To retain
every serialized property tag (still subject to count/value caps), pass
`-AllProperties`.

## Exact cooked role arrays

UELib 1.12.1 does not infer the native element type of
`ROMapInfo.NorthernRoles` or `ROMapInfo.SouthernRoles`; ordinary actor mode
therefore reports `Array type was not detected` for those two fields. Use the
narrow `-RoleInfo` mode instead:

```powershell
powershell -NoProfile -File tools\extract_cooked_map_metadata.ps1 `
  'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\CuChi\VNTE-CuChi.roe' `
  -RoleInfo `
  -MaxInputMiB 300 `
  -OutputPath "$env:TEMP\VNTE-CuChi-roles.jsonl"
```

This mode reads the single cooked `ROMapInfo` export and decodes the exact
source-defined native `RORoleCount` tagged-struct layout:

```text
Class<RORoleInfo> RoleInfoClass
byte Count
byte ReverseCount
```

Every array and element must consume its declared byte range exactly. Class
references must resolve through the map package table to an
`ROGame.RORoleInfo*` class, required fields may occur only once, and malformed,
truncated, null, ambiguous, or out-of-family references fail closed. The mode
opens the package and `.roe` payload read-only and emits the map-import
`FPackageIndex`, resolved role-class path, normal/reversed limits, and serialized
element size.

It deliberately does **not** emit squad indices or role-slot indices.
`ROMapInfo.InitSquadsForGametype` clears and constructs `NorthernSquads` and
`SouthernSquads` at runtime from game type and maximum players; later
`ServerAutoSelectSquad` chooses a live squad from occupancy/lock state and
`GetEmptySlot` chooses the first open role slot. Those values are runtime state,
not cooked map definitions. Likewise, the map's import `FPackageIndex` is not a
wire PackageMap object index: a wire role object still requires the agreed
PackageMap package base plus the exact `ROGame.u` role CDO export index.

## Source-verified brush bounds diagnostics

`-BrushBounds` is a deliberately narrow UE3 brush diagnostic. It currently
accepts only the exact `BlockingVolume` class and emits a record only when all
of these invariants are proven from the package:

1. the actor has a finite serialized `Location` and a `BRUSH` reference to a
   `UModel` owned by that actor;
2. no serialized `Rotation`, `DrawScale`, `DrawScale3D`, `PrePivot`,
   `MainScale`, or `PostScale` override is present;
3. the referenced export begins with the cooked UObject `NetIndex` followed by
   the package's exact `FName(None)` tagged-property terminator; and
4. the next seven little-endian floats form a finite, capped, internally
   consistent `FBoxSphereBounds` (`Origin`, nonnegative `BoxExtent`, and
   `SphereRadius`).

The prefix is validated rather than treated as a guessed fixed offset. An
unexpected property tag or transform is an error; the extractor does not scan
forward for plausible floats or approximate rotated/scaled geometry.

```powershell
powershell -NoProfile -File tools\extract_cooked_map_metadata.ps1 `
  'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\Resort\VNTE-Resort.roe' `
  -BrushBounds `
  -MaxInputMiB 1200 `
  -OutputPath "$env:TEMP\VNTE-Resort-brush-bounds.jsonl"
```

The installed Resort package is in `BrewedPC\Maps\Resort`; the similarly named
`ROGame\CookedPC\VNTE-Resort.roe` path does not exist on the validated machine.
The validated package identity is:

- size: `1,162,107,869` bytes;
- SHA-256: `58193C282D27B3D8E2A47B16C919618BBAE6EE2F43B64CCC738DA6FFEA7055AF`;
- package GUID: `C75E786345B77AA5243259ABAF16C294`;
- package/licensee versions: `765` / `771`.

The complete Resort run emitted all 874 `BlockingVolume` bounds with zero
errors. Each actor serialized only `BRUSH` and `Location`, so its world AABB is
the source UModel box translated by the actor location and identity Actor class
defaults. The extractor still labels every record:

```text
geometryFidelity=bounds_only
collisionGeometryComplete=false
worldCollisionComplete=false
safeForGameplayOcclusion=false
```

Those flags are part of the safety contract. `FBoxSphereBounds` is an exact
source bounding volume, not the brush's convex collision hull. Treating it as
an occluder can block shots through empty portions of an irregular or slanted
brush. Resort also contains 15,851 `StaticMeshActor` exports and two `Terrain`
actors (88 `TerrainComponent` exports) whose complete collision is not supplied
by this workflow. No runtime combat, bot, projectile, grenade, or ground-height
query consumes these diagnostic AABBs.

The binary layout is grounded in the installed UE3 source:

- `Development\Src\Core\Src\UnObj.cpp`: `UObject::Serialize` writes NetIndex
  and tagged properties;
- `Development\Src\Core\Inc\UnMath.h`: `FBoxSphereBounds` serializes Origin,
  BoxExtent, and SphereRadius;
- `Development\Src\Engine\Src\UnModel.cpp`: `UModel::Serialize` writes Bounds
  immediately after `Super::Serialize`;
- `Development\Src\Engine\Src\UnBrushComponent.cpp`: runtime brush collision
  uses `BrushAggGeom`, not the UModel box.

## JSONL contract

The output is UTF-8 JSON Lines so consumers can stream it without holding the
whole map inventory in memory:

- `header`: input identity, UELib/package/engine versions, table counts, filters,
  and active bounds.
- `class`: export count for one matching class (`-ClassesOnly`).
- `role`: exact `ROMapInfo.RORoleCount` team/ordinal, map import reference,
  resolved role class, normal/reversed limits, and per-element serialized size
  (`-RoleInfo`).
- `actor`: class name/path, object name/path, export offset/size, selected
  serialized property tags, and a parsed `{x,y,z}` convenience field when a
  valid `Location` struct is present.
- `brushBounds`: UModel/export provenance, the validated native layout, actor
  translation, exact local box/sphere bounds, translated world AABB, and
  mandatory bounds-only/incomplete/unsafe flags (`-BrushBounds`).
- `error`: a bounded per-actor deserialization error.
- `summary`: candidate/emitted/truncated/error counts. It is always the final
  record after a successful package-table read.

The default selection covers player starts and start-group volumes, spawn
actors/volumes, objectives and their capture volumes, territory/capture actors,
map bounds, team actors, and `WorldInfo`. Useful RS2 property tags observed in
official maps include `Location`, `Rotation`, `Group`, `ObjName`, `ObjIndex`,
`ObjRepIndex`, `InitialObjState`, `ObjVolume`, `AlliesPriority`, `AxisPriority`,
`MinimumCaptureTime`, `SpawnProtectionVolume`, and `SpawnVolumeCamera`.

UELib calls these serialized instance tags `DefaultProperties`. They are not a
fully materialized Unreal inheritance tree: omitted values may still come from a
class default object or archetype. Normal actor mode leaves brush/collision
geometry as bounded property/reference text. Brush-bounds mode reads only the
validated UModel box described above; it does not reconstruct UE3 world
collision. UELib 1.12.1 deserializes RS2's `UModel` using an incompatible legacy
`UPrimitive` box/sphere layout, leaves `Polys` unavailable, and throws while
rendering `BrushAggGeom` convex arrays. It also does not expose authoritative
static-mesh `BodySetup` collision or terrain height samples from this package.
UELib 1.12.1 cannot infer the custom element type of Firebase's
`ValidLocationsForPawns` array, so that tag is not in the default property
filter; do not treat its decompiled value from `-AllProperties` as authoritative.

## Bounds and failures

Defaults are 512 MiB input, 250,000 exports, 1,000 actors, 64 properties per
actor, 1,024 characters per value, 25 actor errors, and 512 class records. The
tool refuses to overwrite an output file unless `-Overwrite` is explicit, and it
refuses any output path equal to the map or UELib input.

Exit codes are explicit:

- `0`: complete extraction with no actor errors.
- `64`: invalid command-line usage.
- `65`: path, regex, count, size, or overwrite validation failure.
- `66`: package-table/object initialization failure.
- `67`: JSONL is partial because one or more selected actors failed to deserialize.
- `70`: unexpected internal failure.

Run the focused smoke/integration suite (it hashes the official input before and
after extraction):

```powershell
powershell -NoProfile -File tools\tests\CookedMapMetadataExtractor.Tests.ps1 `
  -RequireIntegration
```
