# VNTE-CuChi role and squad grounding

This note separates cooked-map evidence, shared `ROGame.u` linker evidence,
runtime squad allocation, and wire PackageMap identity. It does not promote any
value into gameplay solely because nearby indices look plausible.

## Inputs and read-only provenance

Validated installed inputs on 2026-07-14:

| package | bytes | SHA-256 | package GUID |
|---|---:|---|---|
| `Maps\CuChi\VNTE-CuChi.roe` | 283,465,712 | `0410B83DB9E34FB145E0418EE634BA924E3BE41A8570502A8717DE19CEA5E2E2` | `1BE145E5457B54A941963282D62E012B` |
| `BrewedPC\ROGame.u` | 40,986,800 | `34093C828DBB9DA9E709C2845BB9AFF0ED747A7BC954FEC4D76970FF4832305F` | `33EE724F43F851351795FD975E8D5AC1` |

Both before/after hashes remained identical. The map has one `ROMapInfo`
export: linker index 10022, serial offset 36,844,352, serial size 3,002.

Reproduce the exact two role arrays with:

```powershell
powershell -NoProfile -File tools\extract_cooked_map_metadata.ps1 `
  'D:\SteamLibrary\steamapps\common\Rising Storm 2\ROGame\BrewedPC\Maps\CuChi\VNTE-CuChi.roe' `
  -RoleInfo -MaxInputMiB 300 `
  -OutputPath "$env:TEMP\VNTE-CuChi-roles.jsonl"
```

`-RoleInfo` exists because UELib 1.12.1 cannot infer the custom native element
type in ordinary actor mode. It parses the source-defined `RORoleCount` tagged
struct directly, resolves each class import, and requires exact consumption of
all 718 bytes per array and 102 bytes per element.

## Cooked map facts

The `ROMapInfo` instance serializes these mode/force fields:

- `DefendingTeam`, `DefendingTeam16`, `DefendingTeam32`, and
  `DefendingTeam64`: `DT_North`.
- `NorthernForce`: `NFOR_NLF`.
- `SouthernForce` has no instance override. `ESouthernForces` zero/default is
  `SFOR_USArmy` in `ROMapInfo.uc`.
- `bIgnoreReverseCountNorth=false` and
  `bIgnoreReverseCountSouth=false`.
- team-leader role instances resolve to Northern and Southern Commander.

The exact cooked base role arrays are:

| side | ordinal | class | count | reverse |
|---|---:|---|---:|---:|
| North | 0 | `RORoleInfoNorthernRifleman` | 255 | 255 |
| North | 1 | `RORoleInfoNorthernScout` | 3 | 5 |
| North | 2 | `RORoleInfoNorthernMachineGunner` | 4 | 5 |
| North | 3 | `RORoleInfoNorthernSniper` | 2 | 2 |
| North | 4 | `RORoleInfoNorthernRPG` | 2 | 3 |
| North | 5 | `RORoleInfoNorthernSapper` | 3 | 3 |
| North | 6 | `RORoleInfoNorthernRadioman` | 2 | 2 |
| South | 0 | `RORoleInfoSouthernRifleman` | 255 | 255 |
| South | 1 | `RORoleInfoSouthernPointman` | 5 | 3 |
| South | 2 | `RORoleInfoSouthernMachineGunner` | 5 | 4 |
| South | 3 | `RORoleInfoSouthernMarksman` | 2 | 2 |
| South | 4 | `RORoleInfoSouthernGrenadier` | 3 | 2 |
| South | 5 | `RORoleInfoSouthernEngineer` | 3 | 3 |
| South | 6 | `RORoleInfoSouthernRadioman` | 2 | 2 |

These class names are the map-authored base array. For Territories,
`ROMapInfo.InitRolesForGametype` replaces each base class by `ClassIndex` and
force before accepting a selection. In Cu Chi's normal, non-reversed first
round:

- North is NLF, so class 0 becomes `RORoleInfoNorthernGuerilla`.
- South is US Army, so class 0 becomes `RORoleInfoSouthernGrunt`.

This substitution comes from the exact `NorthAltRoleClasses[0]` and
`SouthAltRoleClasses[0]` defaults in the installed `ROMapInfo.uc`, not from
numeric adjacency.

## Exact linker identities and conditional wire identities

The installed `ROGame.u` export table gives these class-default-object exports:

| role CDO | export / NetIndex |
|---|---:|
| `Default__RORoleInfoNorthernRifleman` | 47917 |
| `Default__RORoleInfoNorthernGuerilla` | 47919 |
| `Default__RORoleInfoSouthernGrunt` | 48011 |
| `Default__RORoleInfoSouthernGrunt_SK` | 48013 |

The current capture proves `ROGame` PackageMap object base 39479 because both
known pairs reconcile exactly:

```text
39479 + 47917 = 87396  (captured Northern Rifleman)
39479 + 48013 = 87492  (captured Southern Grunt SK)
```

Therefore, **when the connection exports the same `ROGame` package base** used
by the emulator's captured PackageMap bootstrap, Cu Chi class-0 role objects
are exactly:

```text
North / NLF / Guerilla: 39479 + 47919 = 87398
South / US / Grunt:     39479 + 48011 = 87490
```

The PackageMap-base condition matters. A map import `FPackageIndex` such as
`-92` is not the wire object reference, and a linker export index alone is not
the wire object reference. Before wiring these numbers, assert that the
Cu Chi connection still uses `ROGame.ObjectBase == 39479`. The current server
replays one captured bootstrap and carries that exact base in the frozen retail
profile; semantic role grounding rejects every other or unknown base.

## Squads are runtime state, not cooked definitions

`NorthernSquads` and `SouthernSquads` cannot be extracted as fixed map
definitions because there are none. Installed source establishes this sequence:

1. `InitSquadsForGametype` clears both arrays.
2. `GetNumSquads` returns 2, 4, 8, or 10 from maximum players (Skirmish has a
   separate 1/2-squad branch).
3. It constructs fresh `ROSquadInfo` instances for both teams.
4. `ServerAutoSelectSquad` chooses the most populated unlocked squad that has
   fewer than six members; ties retain the first squad encountered.
5. `GetEmptySlot` chooses the first free role slot 0 through 5.

Consequently, captured `SquadIndex=8, RoleIndex=5` or
`SquadIndex=2, RoleIndex=3` values are occupancy snapshots. They must not be
copied to Cu Chi or any other session. The server can reproduce the retail
algorithm exactly from its live roster, squad locks, and configured max-player
count.

## Mapping to `RoleSelectionReplication`

The prior Resort-only implementation had three capture-bounded assumptions that
prevented Cu Chi and other maps from working correctly:

1. `DecodeOne` treated every static object except 87396 and 87492 as malformed.
   The bounded decoder now admits the two exact Cu Chi objects as well, while
   leaving map/team/role authorization to semantic grounding.
2. `ResolveGroundedResortInfantry` binds only `VNTE-Resort` and hard-codes both
   the role object and final PRI squad/role state. The latter is live squad
   state, not map metadata.
3. The Resort South object is `SouthernGrunt_SK`. That is a Skirmish role CDO;
   it must not be generalized to a Territories profile without proving the
   actual game-type context that produced the capture.

The first bounded C++ slice is now wired as an authorization-only path:

1. the decoder admits only the four exact Resort/Cu Chi role object references;
2. `ResolveGroundedCuChiInfantry` requires exact Cu Chi, Territories, server
   team/intent, class-0 CDO, and `ROGame.ObjectBase == 39479` inputs; and
3. valid South/US and North/NLF results deliberately leave `ChangedRole`,
   `SquadIndex`, and `RoleIndex` unset (`255`). `ConnectionManager` rejects the
   request before any `RoleSystem` or deployment mutation.

That last gate is necessary because the emulator's current `RoleSystem` is not
yet an exact substrate for `ServerAutoSelectSquad`: it constructs a fixed eight
squads per team, has no retail squad-lock state or max-player-derived 2/4/8/10
count, and joining the first member of an empty squad changes their combat role
to `SquadLeader`. Reusing the Resort capture's 8/5 or 2/3 occupancy tuple would
be less correct than failing closed.

The next safe implementation slice is therefore to model the source-exact live
squad count, lock, membership, and six role-slot states; allocate the most
populated eligible squad and first empty slot transactionally; then derive the
`ChangedRole` arguments from that authoritative result. A Cu Chi retail or
capture-driven regression must select both sides, close the menu, spawn, and
reconnect without preserving stale squad occupancy before the gate is removed.

Remaining evidence work for full role coverage is mechanical but must stay
exact: extract all `ROGame.u` role CDO exports, apply the source force/game-mode
substitution tables, and confirm each connection's PackageMap base. Vehicle and
pilot roles additionally need tank-selection and spawn-class evidence and are
not unlocked by the infantry table alone.
