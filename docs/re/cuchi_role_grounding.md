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

Therefore, **when role grounding uses the same historical +1 registry token**
recovered from the captured bootstrap, Cu Chi class-0 role objects are exactly:

```text
North / NLF / Guerilla: 39479 + 47919 = 87398
South / US / Grunt:     39479 + 48011 = 87490
```

The registry-token condition matters. A map import `FPackageIndex` such as
`-92` is not the wire object reference, and a linker export index alone is not
the wire object reference. The canonical package's actual `ROGame.ObjectBase`
is 39478; 39479 is the separately pinned historical +1 token against which the
role decoder was grounded. The frozen retail profile must retain that token,
and semantic role grounding rejects every other or unknown value. Do not
replace it with an artifact layout base without migrating the registry.

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
   the role object and final PRI squad/role state. The latter is live squad
   state, not map metadata.
3. The Resort South object is `SouthernGrunt_SK`. That is a Skirmish role CDO;
   it must not be generalized to a Territories profile without proving the
   actual game-type context that produced the capture.

The bounded canonical C++ path now carries the request through live squad and
spawn-selection authority:

1. the decoder structurally admits a nonzero static role object reference, then
   the semantic resolver admits only an exact grounded Resort/Cu Chi identity;
2. `ResolveGroundedCuChiInfantry` requires exact Cu Chi, Territories, server
   team/intent, class-0 CDO, and historical role-registry token 39479 inputs;
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

The exact empty-roster Cu Chi final requests are 57-bit
`af265c150080c301` (South/US Grunt, object 87490) and
`af6456150080c301` (North/NLF Guerilla, object 87398). The first live assignment
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

This path is deliberately **canonical-only**. Leave
`RS2V_REPLICATION_BOOTSTRAP_VARIANT` unset when exercising Cu Chi. The installed
artifact currently advertises `roleRegistryGrounded=false`, so an installed
Cu Chi h175 request fails closed before role, squad, PRI, or deployment state
changes. Installed support requires an independently grounded PackageMap role
registry; export-index arithmetic alone is not sufficient evidence.

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

Remaining evidence work for full role coverage is mechanical but must stay
exact: extract all `ROGame.u` role CDO exports, apply the source force/game-mode
substitution tables, and confirm each connection's PackageMap base. Vehicle and
pilot roles additionally need tank-selection and spawn-class evidence and are
not unlocked by the infantry table alone.
