# VNTE-CuChi role and squad grounding

This note separates cooked-map evidence, shared `ROGame.u` linker evidence,
runtime squad allocation, and wire PackageMap identity. It does not promote any
value into gameplay solely because nearby indices look plausible.

## Inputs and read-only provenance

Validated read-only inputs on 2026-07-14 and 2026-07-15:

| package | bytes | SHA-256 | package GUID |
|---|---:|---|---|
| `Maps\CuChi\VNTE-CuChi.roe` | 283,465,712 | `F5D5E9DB687DC45889CC8C2C795904C467FF81285B06338E4F176A09C011687E` | `1BE145E5457B54A941963282D62E012B` |
| capture-compatible `BrewedPCServer\ROGame.u` | 40,986,800 | `34093C828DBB9DA9E709C2845BB9AFF0ED747A7BC954FEC4D76970FF4832305F` | `33EE724F43F851351795FD975E8D5AC1` |
| current retail-client `BrewedPC\ROGame.u` | 40,989,134 | `AED4E60D406880D048EB579A082F4A44BE3D0B39CFEC47F9FCEF828A40C44961` | `16A6CC8D446C4A9FD5B688B3210DCC82` |

Read-only inspection left all input hashes unchanged. The map has one `ROMapInfo`
export: linker index 10022, serial offset 36,844,352, serial size 3,002.
The evidence audit also pins UELib's only non-framework runtime dependency,
`System.Runtime.CompilerServices.Unsafe.dll` 6.0.3.0, at 19,256 bytes and
SHA-256 `08CBD7278B66F1E68425A82D4B97181A4130D93E3DD91831407ABA7212CCDACF`.
The wrapper and C# extractor source are identity-pinned before and after the
audit, every extraction forces a fresh content-keyed compile, and the CLR must
load that exact Unsafe identity from that exact sibling path.

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

## Exact linker identities and artifact-specific wire identities

The capture-compatible server package has these relevant class-default-object
exports:

| role CDO | export index |
|---|---:|
| `Default__RORoleInfoNorthernRifleman` | 47917 |
| `Default__RORoleInfoNorthernGuerilla` | 47919 |
| `Default__RORoleInfoSouthernGrunt` | 48011 |
| `Default__RORoleInfoSouthernGrunt_SK` | 48013 |

The canonical compatibility profile remains frozen to those capture-era role
object indices and historical role-registry token 39479. Both known capture
pairs reconcile exactly:

```text
39479 + 47917 = 87396  (captured Northern Rifleman)
39479 + 48013 = 87492  (captured Southern Grunt SK)
```

Therefore, **when role grounding uses the same historical +1 registry token**
recovered from the captured bootstrap, canonical Cu Chi class-0 legacy role
object references are exactly:

```text
North / NLF / Guerilla: 39479 + 47919 = 87398
South / US / Grunt:     39479 + 48011 = 87490
```

The current retail-client `ROGame.u` has different UClass NetIndices for the
same two non-Skirmish role classes. The installed PackageMap uses actual
`ROGame.ObjectBase` 39478:

| role UClass | current NetIndex | installed wire reference |
|---|---:|---:|
| `RORoleInfoNorthernGuerilla` | 47921 | 87399 |
| `RORoleInfoSouthernGrunt` | 48013 | 87491 |

These installed identities are not justified by arithmetic alone. The same
base maps the adjacent live Compound `_SK` role references at NetIndices
47923/48015 to 87401/87493, matching the already grounded installed Compound
h175 requests. Cu Chi source independently selects the non-`_SK` Guerilla and
Grunt classes for Territories. Together, source selection, exact current
UClass NetIndices, the frozen installed base, and those adjacent live refs
ground 87399/87491. The resulting h175 payloads are source-exact constructions,
not Cu Chi live-capture observations.

The artifact-pinned table audit now resolves all fourteen effective first-round
infantry UClasses without promoting them into gameplay. Reproduce it with
`python tools\audit_installed_role_refs.py`; the exact JSONL result is
`data/installed_cuchi_role_refs.jsonl`.

| side | ordinal | effective Territories class | class index | normal limit | current UClass NetIndex | installed static ref |
|---|---:|---|---:|---:|---:|---:|
| North | 0 | `RORoleInfoNorthernGuerilla` | 0 | 255 | 47921 | 87399 |
| North | 1 | `RORoleInfoNorthernScoutNLF` | 1 | 3 | 47963 | 87441 |
| North | 2 | `RORoleInfoNorthernMachineGunnerNLF` | 2 | 4 | 47929 | 87407 |
| North | 3 | `RORoleInfoNorthernSniperNLF` | 3 | 2 | 47969 | 87447 |
| North | 4 | `RORoleInfoNorthernRPGNLF` | 6 | 2 | 47945 | 87423 |
| North | 5 | `RORoleInfoNorthernSapperNLF` | 4 | 3 | 47955 | 87433 |
| North | 6 | `RORoleInfoNorthernRadiomanNLF` | 7 | 2 | 47935 | 87413 |
| South | 0 | `RORoleInfoSouthernGrunt` | 0 | 255 | 48013 | 87491 |
| South | 1 | `RORoleInfoSouthernPointman` | 1 | 5 | 48053 | 87531 |
| South | 2 | `RORoleInfoSouthernMachineGunner` | 2 | 5 | 48019 | 87497 |
| South | 3 | `RORoleInfoSouthernMarksman` | 3 | 2 | 48031 | 87509 |
| South | 4 | `RORoleInfoSouthernGrenadier` | 5 | 3 | 47999 | 87477 |
| South | 5 | `RORoleInfoSouthernEngineer` | 4 | 3 | 47985 | 87463 |
| South | 6 | `RORoleInfoSouthernRadioman` | 7 | 2 | 48065 | 87543 |

The RPG/Sapper and Grenadier/Engineer rows demonstrate why cooked ordinal must
not be treated as protocol class index. `InitRolesForGametype` selects the force
substitute by the role's source `ClassIndex`. The audit validates exact
UClass/CDO pairing, package identity, generation metadata, checked reference
arithmetic, uniqueness, and the two live-adjacent class-0 cross-checks. The
report authorizes no gameplay by itself (`authorizedByReport=false`), while
correctly marking the two class-0 rows as already supported. The other twelve
remain blocked because no non-class-0 Cu Chi live h175 or role/loadout-specific
owning-pawn graph has been captured.

The identity source is artifact-specific. A map import `FPackageIndex` such as
`-92` is not a wire object reference, and a linker NetIndex without the frozen
PackageMap base is not one either. The canonical package's actual
`ROGame.ObjectBase` is also 39478, but its legacy 39479 role-registry token must
not be silently migrated. Semantic grounding requires the exact
map/mode/team/artifact combination and rejects cross-layout or unknown values.

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

Bots participate in that same grid. `ROGameInfo.uc:2970-2982` joins each bot to
its team, chooses a role, and then calls `ChooseSquad`; `ROAIController.uc:6202-
6265` runs the same fullest-unlocked scan, and `ROSquadInfo.uc:65-84` counts any
non-null Controller owner without distinguishing humans from AI. Emulator squad
owners are therefore tagged `ParticipantId` values so `Human(1)` and `Bot(1)`
remain distinct. Bot death/respawn preserves membership; fill eviction releases
the bot synchronously before a same-packet human h175 can allocate its slot.
That synchronous eviction is the emulator's deterministic fixed-fill policy,
not a retail timing or bot-identity claim: retail `JoinTeam` does not identify a
bot to evict, and `TooManyBots` is evaluated later from bot restart.

Consequently, captured `SquadIndex=8, RoleIndex=5` or
`SquadIndex=2, RoleIndex=3` values are occupancy snapshots. They must not be
copied to Cu Chi or any other session. The server can reproduce the retail
algorithm exactly from its live roster, squad locks, and configured max-player
count.

## Mapping to `RoleSelectionReplication`

The prior Resort-only implementation had three capture-bounded assumptions that
prevented Cu Chi and other maps from working correctly:

1. `DecodeOne` treated every static object except 87396 and 87492 as malformed.
   The structural decoder now admits any nonzero static object reference; exact
   map/team/artifact/role authorization remains a separate semantic grounding
   step, so unknown values still fail before authority mutation.
2. `ResolveGroundedResortInfantry` binds only `VNTE-Resort` and hard-codes both
   the legacy role object and final PRI squad/role state. The latter is live squad
   state, not map metadata.
3. The Resort South object is `Default__RORoleInfoSouthernGrunt_SK`. That is a
   Skirmish role class-default object;
   it must not be generalized to a Territories profile without proving the
   actual game-type context that produced the capture.

The bounded artifact-specific C++ path now carries the request through live
squad and spawn-selection authority:

1. the decoder structurally admits a nonzero static role object reference, then
   the semantic resolver admits only an exact grounded Resort/Cu Chi identity;
2. `ResolveGroundedCuChiInfantry` requires exact Cu Chi, Territories, server
   team/intent, class-0 artifact identity, and either canonical historical-token or
   installed PackageMap provenance;
3. map activation derives the active 2/4/8/10 squad prefix from the configured
   maximum players while retaining ten stable replication entries per team;
4. `AutoAssignRetailSquad` transactionally chooses the most populated active,
   unlocked, non-full squad and its first free slot among the six retail slots;
5. production bot creation/removal participates synchronously in the same tagged
   squad grid, including h170 reconciliation before a following h175;
6. `ConnectionManager` maps Cu Chi class 0 to Rifleman, publishes owner-PRI h79,
   derives h210+h211 from the live assignment, publishes the owner-PRI squad and
   role properties, and advances spawn selection only after every mutation and
   publication precondition succeeds; and
7. map activation advances the squad generation, clears locks and membership,
   then recreates the map-local bot manager so reconnects cannot inherit stale
   occupancy.

The canonical empty-roster Cu Chi final requests are exact 57-bit
`af265c150080c301` (South/US Grunt, object 87490) and
`af6456150080c301` (North/NLF Guerilla, object 87398). The installed
source-constructed forms are exact 57-bit `af365c150080c301` (South, object
87491) and `af7456150080c301` (North, object 87399). The first live assignment
is squad 0, slot 0, so its combined ChangedRole/ChangedSquad payload is the
32-bit `d2fe731a`; Resort's captured 8/5 and 2/3 occupancy values are never
reused.

The emulator's default server configuration is not an empty roster: its fixed
fill is eight bots per side. Six occupy squad 0 and two occupy squad 1. Under
the emulator's deterministic policy, the first human admission removes the
highest-id fill bot before role selection, leaving seven bots; the human then
joins the fullest non-full squad at 1/1. That exact removal identity and timing
are emulator invariants, not claimed retail behavior. This increment grounds
shared squad occupancy only. Headless bots still lack the cooked
`bBotSelectable` role tables and AI role-choice policy, so it does not claim a
source-exact bot class or commander assignment.

Leave `RS2V_REPLICATION_BOOTSTRAP_VARIANT` unset for canonical compatibility or
set it to exact `installed` for the current retail-client layout. The installed
artifact's broad `roleRegistryGrounded` flag remains false: this narrow Cu Chi
class-0 exception does not claim that a complete installed role registry is
known. Installed Resort, both Hue City layouts, canonical Compound, non-class-0
Cu Chi roles, and cross-layout role references still fail before role, squad,
PRI, or deployment state changes.

## Canonical live verification (pre-bot-occupancy correction)

On 2026-07-15, a Debug server loaded the installed `VNTE-CuChi.roe` read-only
under the canonical artifact, activated Territories with US Army/NLFSV, loaded
all seven cooked objective identities and ten deployment starts, and configured
ten active squads for a 64-player server. The retail-protocol mock then passed:

- South class-0 h175 -> live squad/slot 0/0 -> exact owning pawn/loadout graph;
- North class-0 h175 -> live squad/slot 0/0 -> exact 32-bit h210+h211 transition
  plus owning pawn/loadout graph;
- a two-session same-UDP-endpoint reconnect with packet/channel cursors reset;
  and
- a repeated South role/spawn after disconnect, again assigned squad/slot 0/0.

Server logs froze the canonical artifact with actual ROGame ObjectBase 39478,
accepted clients 1, 2, and 5 at squad/slot 0/0, and removed each disconnected
session before the next selection. This is direct evidence that captured Resort
occupancy is not reused and stale Cu Chi membership does not survive reconnect.
It is not evidence for production fill allocation: those 0/0 results predate
tagged bot participation and reflected the emulator's then-missing AI squad
owners. Current deterministic coverage pins the default filled-roster result at
1/1 and separately keeps the empty-roster 0/0 fixture for wire encoding.

Remaining evidence work for full role coverage is now capture-bound rather than
index-bound. Each non-class-0 infantry role needs an exact accepted h175 and a
role/loadout-keyed owning-pawn graph before runtime authorization. The current
spawn path selects one fixed graph per team, so accepting a different role from
UClass identity alone would publish the wrong weapons. South MachineGunner is
the smallest next candidate (class 2, limit 5, default M60 plus M61), but still
needs its M60 graph captured and implemented. Commander, vehicle, and pilot
roles additionally need their separate ownership, tank-selection, seat,
possession, and spawn-class evidence and are not unlocked by the infantry
table.
