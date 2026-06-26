# RS2V Per-Session Actor Replication Spec (EngineVersion 7258)

Reverse-engineered from **two full real-server gameplay captures** where the retail
client actually spawns, deploys, moves and fights:

- `D:\RE-Tools\rs2_realserver_capture.pcapng`, server `93.191.26.237:7777`.
  - Session A (primary): `udp.port==57867 && udp.port==7777` (~59k frames).
  - Session B (corroboration): `udp.port==56400 && udp.port==7777`.

Decoder: `tools/mock_client.py` (`mc.decode_packet(data, bd_max=BD)`), **bd_max=16384
for C->S, bd_max=12000 for S->C** (asymmetric MaxPacket, already RE'd). Bunch framing
and the control-channel handshake are documented in
`docs/RS2V_ControlChannel_WireSpec_7258.md` and
`docs/RS2V_PostJoin_Replication_7258.md`; this document picks up **after the
PackageMap export** and covers the part that turns a frozen, camera-locked client
into a controllable player.

Confidence tags: **[H]** bit-exact from capture and/or reproduced across both
sessions; **[M]** strong inference from capture + source; **[L]** plausible, not yet
bit-pinned (flagged for disassembly follow-up).

---

## 0. TL;DR — the controllable-player milestone

The single most important new finding:

> **The local PlayerController is actor channel `ch2`.** The retail client proves it
> is controllable by sending its first **`ServerMove` RPC on ch2** at **f1522**
> (Session A) / **f60867** (Session B) — with the *byte-identical* opening
> `68 5a 00 00 00 98 72 22 ...` in BOTH sessions. **[H]**

The ordered path from "frozen on map" to "controllable":

```
  (handshake + full PackageMap export already done — see other docs)
1. C->S f1477  ch0 NMT 0x09  "Join / ready"   (client verified all packages)   [H]
2. S->C f1484  ACTOR OPEN BURST begins — server opens ch2 FIRST, then ch3..ch12  [H]
       ch2 = local PlayerController (ChType 2, bOpen, bReliable, 925b initial)
3. S->C f1484..f1521  ~140 actor channels opened + initial property blocks
       (GRI, TeamInfos, PRIs, other players' pawns, world actors)               [H]
4. S->C f1515  ch2 reliable RPC (ClientRestart-class) -> sets Pawn, possession   [M]
5. C->S f1522  ch2 ServerMove RPC stream STARTS  <== CONTROLLABLE MILESTONE      [H]
6. S->C f1525/f1537 ch2 reliable RPCs (0x70, ClientRestart/ClientSetRotation)    [M]
   ... steady state: ServerMove C->S @ ~client tick, property deltas S->C ...
```

**What unlocks the camera / hands control to the player:** the server must (a) spawn
a Pawn server-side and `Possess` it (sets `Pawn.RemoteRole = ROLE_AutonomousProxy`),
then (b) send the reliable **`ClientRestart(Pawn)`** RPC down ch2. `ClientRestart`
(`PlayerController.uc:4059`) does `Pawn = NewPawn; AcknowledgePossession(Pawn);
SetViewTarget(Pawn); ResetCameraMode();` — **that `SetViewTarget(Pawn)` is what
unlocks the frozen camera**, and `AcknowledgePossession` is what makes the client
start sending `ServerMove`. Before that, the client has a PlayerController actor
channel but no possessed pawn, so it floats. **[M]**

Team/role select (the player "deployed") is the C->S reliable RPC
**`SelectRoleByClass(...)`** (`ROGame/ROPlayerController.uc:3790`); readiness is the
replicated `ROPlayerReplicationInfo.bReadyToPlay`. The server reacts to that by
`RestartPlayer` -> `SpawnDefaultPawnFor` -> `Possess` -> `ClientRestart`. **[M]**

---

## 1. The actor-open burst (spawn sequence), bit-decoded

Right after the client's `NMT 0x09` (f1477), the server opens the whole relevant
actor set in a single burst. Channel index is assigned ascending and is the SAME
ChIndex in both directions (UE3 actor channels are bidirectional).

### 1.1 First channels opened (Session A, f1484-1487) **[H]**

| Frame | ChIndex | ct | flags | Bits | Payload[0:12] | Identity |
|-------|---------|----|-------|------|---------------|----------|
| f1484 | **2**  | 2 | O.R sq1 | 925  | `60c101001bccea3f490460e0` | **local PlayerController** (opened FIRST) |
| f1484 | 3  | 2 | O.R sq1 | 995  | `86bb08002bf41834022c7061` | actor (PRI/Pawn class `86bb0800`) |
| f1484 | 4  | 2 | O.R sq1 | 1236 | `86bb0800ab26ffbc052c10c6` | actor |
| f1484 | 5  | 2 | O.R sq1 | 1013 | `86bb08009b505534482c6070` | actor |
| f1484 | 6  | 2 | O.R sq1 | 1250 | `86bb08000cecf5c0f75f8130` | actor (likely the player's own Pawn) |
| f1484 | 7  | 2 | O.R sq1 | 1187 | `86bb08001ce039c2ed5f0197` | actor |
| f1484 | 8  | 2 | O.R sq1 | 213  | `20bc08005d351e143b15fd53` | actor (small) |
| f1484 | 9-12 | 2 | O.R | ~1100-1600 | `8ebb0800.. / acba0800..` | actors |
| f1485 | 13,14 | 2 | O.R | ~970 | `5aa50200404501..` | actor class `5aa50200` (PRI, x65 total) |
| f1485 | 21 | 2 | O.R | 81 | `0ac1020040bd0400000000` | TeamInfo (`0ac10200`, tiny initial) |

Over f1484-f1530 the server opens **~143 actor channels**. Clustering by the
class-NetGUID field shows the dominant classes: `5aa50200` (x65 — PlayerReplicationInfo
shells), `8ebb0800` (x14), `86bb0800` (x9, the Pawn class), plus singletons for GRI,
TeamInfos, etc. **[H]**

### 1.2 Cross-session corroboration **[H]**

Session B (`:56400`) reproduces the exact structure:

```
NMT 0x09 join (C->S)  : f60857
first S->C actor open  : f60859, ChIndex=2   (PlayerController opened first)
first C->S ServerMove  : f60867, payload 685a000000987222...  (byte-identical to A)
```

The byte-identical ServerMove prefix across two independent connections is the
strongest possible confirmation that **ch2 = local PC** and that `68...` is the
fixed ServerMove RPC header.

### 1.3 ch2 traffic, decoded (the PlayerController channel) **[H]**

```
f1484 S->C ch2 O.R sq1 925b : SerializeNewActor(PlayerController) + initial props  (§2)
f1484 S->C ch2 ct0  270b    : 3332f7... unreliable initial property delta
f1487+ S->C ch2 ct0  20-355b: 17.. / 33.. / 40.. periodic PROPERTY deltas to the PC (§3)
f1515 S->C ch2 O.R sq2 1401b: 38.. reliable RPC (tail ASCII "Say" -> a ClientMessage-class RPC)
f1522 C->S ch2 O.R sq1 164b : 68.. FIRST ServerMove (full)            <== controllable
f1522 C->S ch2 O.R sq2..9 33b: 98.. short ServerMoves (one per saved move)
f1522 C->S ch2 O.R sq10 62b : 25.. ServerMove variant (Dual/Old or RO cover move)
f1525 S->C ch2 O.R sq3 614b : 70.. reliable RPC (ClientRestart / possession state)
f1537 S->C ch2 O.R sq4  74b : 70.. reliable RPC (same function index 0x70 as f1525)
```

The two `0x70` reliable RPCs (f1525, f1537) decode to the **same function field
index** — i.e. the same `Client*` function called twice; this is the
`ClientRestart`/possession-class call. The `0x38` RPC at f1515 carries trailing
ASCII `"Say"` so it is a chat/message RPC, not possession. **[M]**

---

## 2. `SerializeNewActor` wire format (opening-actor bunch header)

When an actor channel is opened (`bOpen=1`, `ChType=2`), the bunch payload begins
with UE3's `SerializeNewActor` header, then the initial replicated-property block.

### 2.1 NetGUID / object-reference codec (DISASSEMBLY-CONFIRMED) **[H]**

`UPackageMap::SerializeObject` is **native** (no UnrealScript). The net override is
**`VNGame.exe @ 0x140696070`** (net UPackageMap vtable slot at `0x14101a548`; base
variant `0x1400799e0`). Every object reference / NetGUID on the wire is:

```
1 bit   flag                         ; via network-archive vtable +0x10 (serialize-1-bit)
if flag == 1:  (static / already-known object)
    NetIndex = SerializeInt(value, max = 1023 / 0x3ff)     ; minimal-bit ranged int
    -> object = ObjectTable[ [PackageMap+0x100] + NetIndex*8 + 0xf84 ]
if flag == 0:  (dynamic / export — e.g. a freshly-spawned actor or its class)
    NetIndex = SerializeInt(value, max = 0x80000000)        ; minimal-bit ranged int
    -> resolve/create by index via vtable +0x2a8
```

- The codec is a **1-bit selector + a minimal-bit `SerializeInt`** (the same
  bsr/mask ranged-int scheme as the bunch header), **NOT** a byte-packed varint.
- Load path `0x1406960ad`; static branch `SerializeInt(.,1023)` @`0x1406960d5`;
  dynamic branch `SerializeInt(.,0x80000000)` @`0x14069613a`. Save path mirrors:
  flag write @`0x1406962ce`, `SerializeInt(.,1023)` @`0x140696312`.
- Network-archive bit primitives: vtable **+0x10 = serialize 1 bit**,
  **+0x18 = SerializeInt(value, max)**. Reader prims: `FBitReader::SerializeBits`
  @`0x140078530`, byte read @`0x140076700`; writer `SerializeInt` @`0x1400798e0`.
  FBitReader/Writer offsets: +0x84 buffer, +0x94 bit-pos, +0x98 numbits, +0x38 error.

**Verification against the capture:** the ch2 PlayerController open payload begins
`60 c1 01 00 ...`; byte 0 = `0x60` = LSB-first bits `0,0,0,0,0,1,1,0`, so the **first
(flag) bit = 0 = the dynamic/export path** — exactly what a freshly-spawned actor's
channel-open NetGUID must use. **[H]** (My earlier "even NetGUID" note was reading
this very flag bit; it is the static/dynamic selector, not the low bit of an index.)

### 2.2 Observed open-bunch structure (PlayerController open, ch2 f1484) **[H bytes / M field split]**

```
payload: 60 c1 01 00 | 1b cc ea 3f | 49 04 60 e0 7d 74 11 44 ...
         └ actor NetGUID ┘ └ class NetGUID ┘ └─ initial property block (§3) ─┘
         (flag=0,export)   (ref into PackageMap)
```

- **Actor NetGUID** — flag bit 0 (export) + minimal-bit index (§2.1); new actors are
  exported on first sight. **[H codec; M exact bit count for this index.]**
- **Class NetGUID** — the next object reference (§2.1), indexing a class object in the
  **PackageMap** we sent during the export phase (must resolve against a verified
  package). This is why **65 PlayerReplicationInfo actors share class field
  `5aa50200`** and **9 Pawns share `86bb0800`** — the repeated field IS the shared
  class reference. **[H]**
- **Initial property block** — the `bNetInitial` replicated properties (§3 order).
- **Remaining ambiguity [M]:** whether `SerializeNewActor` reads Location/Rotation
  *inside* the header (after the class ref, before the property loop) or whether they
  arrive as ordinary `Actor` initial properties (handles for `Location, Rotation`,
  §3.2). `UActorChannel::ReceivedBunch` was not pinned (shipping binary, asserts
  stripped, reached only via vtable); it does call SerializeObject `0x140696070`.
  This is the only field-split ambiguity left and does not block Phases 1-4.

### 2.3 Role flagging (how the local PC is made "ours") **[M]**

From `Engine/Actor.uc` `replication{}` (block at lines 519-570) the
**initial-only** (`bNetInitial && Role==ROLE_Authority`) properties include, in this
order: `RemoteRole, Role, bNetOwner, bTearOff` (Pos 0x2BA), and `Owner` (Pos 0x37A,
also gated on `bNetOwner`). `ENetRole`: `ROLE_None=0, ROLE_SimulatedProxy=1,
ROLE_AutonomousProxy=2, ROLE_Authority=3` (`Actor.uc:54-59`).

For the owning client's pawn/PC, the server sends **`RemoteRole = ROLE_AutonomousProxy
(2)`** and **`bNetOwner = true`**; for everyone else's pawns it sends
`ROLE_SimulatedProxy (1)`. UE3 also swaps Role<->RemoteRole on the wire (the client
sees its own pawn as `Role=AutonomousProxy`). `Pawn.PossessedBy` (`Pawn.uc:1029`,
sets `RemoteRole=ROLE_AutonomousProxy` at ~1049 when possessed by a PC and not
Standalone) is what flips this server-side. This + `bNetOwner` is the bit that tells
the client "this is YOUR pawn, send moves for it."

---

## 3. Property-replication wire format

### 3.1 Per-property layout (decoded) **[H]**

Within an actor bunch (initial block or a delta), each replicated property is:

```
Handle = SerializeInt(maxPropertyHandle)     ; the property's index
<property value>                             ; type-specific serialization
```

repeated until the bunch's `BunchDataBits` are consumed. Decoded examples from the
**PlayerController channel ch2** (the unreliable ct0 deltas):

| Frame | Payload | Handle | Note |
|-------|---------|--------|------|
| f1487 | `176a00` | **23** | minimal update of PC property #23 |
| f1490 | `176ad0f30100296000cf116eff7400000000` | **23** | same property #23, new value (a rotation/position vector) |
| f1493 | `33ead02d005c44b6bede7f3550` | **51** | PC property #51 |
| f1517 | `40b5...` | **64** | PC property #64 |

The two handle-23 bunches differ only in their value payload — same field, updated
each tick. The handle is read with `SerializeInt(handle, max)` where **`max` = the
class's `ClassNetCache` replicated-field count — a per-class RUNTIME value, not a
constant** (disasm-confirmed: no constant max appears for the 23/51/64 handles, and
the same applies to the ServerMove function handle 664; the field index and function
index share this per-class indexed namespace). So our emulator computes `maxHandle`
from the class's replicated property+function table, then `SerializeInt`. **[H for
the handle values and the per-class-runtime-max mechanism.]**

### 3.2 Property HANDLE order = VAR DECLARATION order, NOT replication-block order **[H, from source semantics]**

The wire handle of a replicated property is its index in the class's **variable
declaration order** among replicated properties; the `replication{}` block only
supplies the *condition* under which it is sent. The decompiler preserved the native
**`// Pos:0xNNN`** offsets inside each `replication{}` block, which reflect that
order. Authoritative per-class lists (declaration order, replicated subset):

**`Engine/Actor.uc`** (base, every actor) — grouped by replication condition:
```
[bNetInitial+movement] Location, Rotation
[SimProxy]             Base; RelativeLocation, RelativeRotation
[bNetInitial+movement] Physics, Velocity
[Authority+initial]    bHardAttach
[Authority+dirty]      bHidden; bBlockActors, bProjTarget
[Authority+INITIAL]    RemoteRole, Role, bNetOwner, bTearOff      <- the role flags
[Authority+dirty]      Instigator; DrawScale, ReplicatedCollisionType, bCollideActors, bCollideWorld
[bNetOwner+initial]    Owner
[Authority+dirty]      bLoadedFromSavegame
```

**`Engine/PlayerReplicationInfo.uc`** replicated decl order:
`Score, Deaths, Ping, PlayerName, PlayerID, Team, bAdmin, bIsSpectator,
bOnlySpectator, bWaitingPlayer, bReadyToPlay, bOutOfLives, bBot, bIsInactive,
bEgsClient, StartTime, Kills, UniqueId`. (Initial-only: `PlayerID, bBot, bIsInactive`;
`Ping` only when `!bNetOwner`.)

**`ROGame/ROPlayerReplicationInfo.uc`** adds (decl order): `ClassIndex, ClassRank,
DishonorScore, FriendlyFire*, HonorLevel, MatchScore, RoleIndex, SpawnSelection,
SquadIndex, TeamPRIArrayIndex, VOIPStatus, VoicePackIndex, bDead, bIsMember,
bReadyToPlay(...)`, etc. — **`bReadyToPlay` here is the deploy/ready signal**.

**`Engine/Pawn.uc`** replicated: `DrivenVehicle, FlashLocation, HitDamageType,
PlayerReplicationInfo, TakeHitLocation, bIsWalking, bSimulateGravity` (dirty);
`Health` (bNetOwner||bReplicateHealthToAll); `AccelRate, AirControl, AirSpeed,
Controller, GroundSpeed, InvManager, JumpZ, WaterSpeed` (bNetOwner); `FiringMode,
FlashCount, bIsCrouched, RemoteViewPitch, RemoteViewYaw` (!bNetOwner); `bIronSights,
bIsProning, bIsSprinting` (Authority). **Pawn does NOT replicate its own
Location/Rotation/Velocity** — those come from the `Actor` base block above.
`Controller` is repnotify.

**`Engine/TeamInfo.uc`**: replicated `Score` (dirty), `TeamIndex, TeamName`
(initial). **`ROGame/ROTeamInfo.uc`** adds squad/team arrays.

**`Engine/GameReplicationInfo.uc`**: initial `ElapsedTime, GameClass, GoalScore,
RemainingTime, ServerName`; dirty `Winner, bMatchHasBegun, bMatchIsOver,
bStopCountDown, RemainingMinute, TimeLimit`.

**`Engine/PlayerController.uc`**: script-side replicates only `TargetEyeHeight,
TargetViewRotation` (when `ViewTarget != Pawn`); the rest is native
`nativereplication`. So PC handles 23/51/64 we decoded are mostly native/base-actor
+ inherited Controller properties.

---

## 4. `ServerMove` RPC wire format (C->S, the movement RPC)

Signature (`Engine/PlayerController.uc:2512`, RO override `ROPlayerController.uc:15584`):

```unrealscript
unreliable server function ServerMove(
    float  TimeStamp,
    Vector InAccel,      // = Acceleration * 10  (server multiplies by 0.1)
    Vector ClientLoc,    // pawn loc for error-correction; vect(1,2,3) = "skip check"
    byte   MoveFlags,    // CompressedFlags(), see below
    byte   ClientRoll,   // Rotation.Roll = 256 * ClientRoll
    int    View,         // ((Yaw & 0xFFFF) << 16) | (Pitch & 0xFFFF)
    optional int FreeAimRot )   // RS2 free-aim, same Yaw/Pitch packing
```

Variants the client also sends: `DualServerMove` (two moves in one RPC; the first
"pending" move uses `ClientLoc = vect(1,2,3)`), `OldServerMove` (byte-compressed
accel for redundancy), and RO cover/mantle moves (extra cover bits). Dispatcher:
`ROPlayerController.SendServerMove` (`:15901`).

### 4.1 `MoveFlags` (`SavedMove.CompressedFlags()`, `SavedMove.uc:222`) **[H from source]**
```
bits 0-2 : DoubleClickMove (EDoubleClickDir 0..6)
bit  3 (0x08) : bRun
bit  4 (0x10) : bDuck
bit  5 (0x20) : bPressedJump
bit  6 (0x40) : bDoubleJump
bit  7 (0x80) : bPreciseDestination
```
`CompressAccel(C)` (`PlayerController.uc:3222`): `C>=0 -> min(C,127)`, else
`min(C,127)+128` (sign in bit 7) — used only by `OldServerMove`'s byte accel.

### 4.2 Bit layout, decoded from capture **[H for structure, M for exact field widths]**

A *short* steady-state move is **33 bits** total. Six consecutive short moves from
f1522 (sq2..7), all decode to a stable leading field then a varying middle:

```
988e600100 : FieldHandle=664  rest=11000100 000110100000000
980e620100 : FieldHandle=664  rest=11000001 000110100000000
9846620100 : FieldHandle=664  rest=10001001 000110100000000
98ae620100 : FieldHandle=664  rest=11010101 000110100000000
```

- **FieldHandle** = the replicated-function index for `ServerMove`, **stable = 664**
  (read as `SerializeInt`, ~10 bits). Same in every move and in both sessions. **[H]**
- The varying ~8-bit middle = the **TimeStamp** delta (monotonic across moves). **[M]**
- The constant `000110100000000` tail = zero `InAccel`/`ClientLoc` (compressed-out
  when zero) + `MoveFlags=0` + zero roll for a standing-still move. **[M]**

The **first/full** move (f1522 sq1, 164 bits, `685a000000987222fa922a9b7b93a303...`)
carries the full param set (non-zero accel + ClientLoc + View). Exact float/compressed
encoding of `InAccel`/`ClientLoc`/`View` inside the RPC is native-serialized and is
**[L]** until confirmed against `APlayerController::execServerMove` /
`UActorChannel::ReceivedBunch` param read in `VNGame.exe`. The structurally-important,
implementable fact is settled: **moves arrive as reliable bunches on ch2 with
function handle 664, one bunch per saved move, at the client's tick rate.** Session A
shows 35,995 C->S ch2 bunches total (1,118 reliable + the rest move/property data).

### 4.3 Server -> client position replication (the answer move)

The server replicates the pawn back via **property deltas on the pawn's actor
channel** (ChType 0 unreliable bunches, §3): the `Actor` base `Location/Rotation/
Velocity/Physics` handles for SimulatedProxy pawns, and for the **owning** pawn the
server normally relies on client prediction and only sends corrections (it sends the
PC channel ch2 `0x17` updates we decoded — PC view/target state). A position
*correction* would be a reliable `ClientAdjustPosition`-class RPC on ch2 (not observed
in the standing-still window). **[M]**

---

## 5. Implementation plan (prioritized) — minimal controllable player

Goal: a connecting retail client spawns a controllable pawn it owns. Everything here
is **generated per-session by the server**, never replayed.

### PHASE 0 — already working
Handshake (StatelessConnect -> Steam login -> NMT_Welcome) and the full **PackageMap
export** (321 packages) — see `RS2V_PostJoin_Replication_7258.md`. The client will
not open actor channels until it has verified every package and sent **`NMT 0x09`**.

### PHASE 1 — react to `NMT 0x09` (client ready) — REQUIRED
On receiving C->S ch0 `NMT 0x09`, begin the actor-open burst. Do NOT wait for any
team/role RPC to open the basics — the capture opens ch2 immediately on Join.

### PHASE 2 — open the bootstrap actor channels (REQUIRED, in this order)
Each open = `bOpen=1, bReliable=1, ChType=2`, payload = `SerializeNewActor` header
(actor NetGUID + class NetGUID resolving against our PackageMap) + the `bNetInitial`
property block (§3 declaration order, only initial-condition properties).

1. **ch2 = local PlayerController** — open FIRST. Initial block must set the
   inherited `Actor` role fields: `RemoteRole = ROLE_AutonomousProxy(2)`,
   `bNetOwner = true`, `Role = ROLE_Authority(3)` (wire-swapped). This is the channel
   the client will send `ServerMove` on. **REQUIRED for any control.**
2. **ch_GRI = (RO)GameReplicationInfo** — initial: `GameClass, ServerName, GoalScore,
   RemainingTime, ElapsedTime`. The world's root replicated object; HUD/menu need it.
3. **ch_Team... = (RO)TeamInfo** (x2 teams) — initial: `TeamIndex, TeamName`.
   `GRI.Teams[]` must resolve or team-select can't populate.
4. **ch_PRI = (RO)PlayerReplicationInfo** for the local player — `PlayerName, Team,
   PlayerID, UniqueId`, and the RO `bReadyToPlay/RoleInfo`. Other players' PRIs are
   optional for first control.

### PHASE 3 — spawn + possess the pawn (REQUIRED — this unlocks the camera)
1. Server-side: spawn a Pawn (`ROPawn`), `Controller.Possess(Pawn)` →
   `Pawn.PossessedBy` sets `Pawn.RemoteRole = ROLE_AutonomousProxy`.
2. Open the **pawn's** actor channel (ChType 2) with class NetGUID = the ROPawn class
   and an initial block carrying `Actor.Location/Rotation`, `RemoteRole=AutonomousProxy`,
   `bNetOwner=true`, `Pawn.PlayerReplicationInfo`, `Pawn.Health`, `Controller`.
3. Send reliable **`ClientRestart(Pawn)`** down **ch2** (the PC channel): function
   handle for the `Client*` slot, single param = the pawn's NetGUID. This triggers
   client `AcknowledgePossession` -> `SetViewTarget(Pawn)` = **camera unlocks**, and
   the client begins emitting `ServerMove`.

### PHASE 4 — accept `ServerMove` (REQUIRED to move)
Decode reliable/unreliable ch2 bunches whose function handle = the `ServerMove` slot
(observed **664**). Parse `TimeStamp, InAccel, ClientLoc, MoveFlags(§4.1),
ClientRoll, View, [FreeAimRot]`, run movement server-side, and (only on error) send a
correction RPC back on ch2. Steady-state: one move bunch per client tick.

### PHASE 5 — steady-state replication (gameplay, not first-control)
Property deltas on the open actor channels (ChType 0 unreliable, handle+value, §3),
ch0 `NMT 0x23` heartbeat, and opening new actor channels as actors become relevant.

### First observable milestones (in order)
1. **Team-select menu appears** ← client has GRI + TeamInfos replicated (Phase 2).
2. **Camera unlocks / spectator free-look** ← `ClientRestart`/`SetViewTarget` even
   before a pawn (view target = a spawn camera).
3. **Controllable pawn** ← pawn channel open with `RemoteRole=AutonomousProxy` +
   `bNetOwner` + `ClientRestart(Pawn)`. The proof in the capture is the **first
   `ServerMove` on ch2 (f1522 / f60867)**.

### Remaining unknowns (resolved vs. open)
- **RESOLVED [H]:** NetGUID codec = 1 flag bit + minimal-bit `SerializeInt` (max 1023
  static / 0x80000000 export), `SerializeObject @0x140696070` (§2.1). Property/function
  handle = `SerializeInt(handle, max = ClassNetCache field count)` (per-class runtime
  max, §3.1). Bunch `BunchDataBits` max = `[NetConnection+0x10c] << 3` (negotiated
  MaxPacket*8, runtime — read conn+0x10c live; it is **not** a hardcoded 1500/2048).
  Bunch header field maxes: ChIndex `SerializeInt(1023)`, ChSequence `SerializeInt(1024)`,
  ChType `SerializeInt(8)` (`SendRawBunch @0x1404a79d0`).
- **OPEN [M]:** whether `SerializeNewActor` reads Location/Rotation inside the header
  vs. as ordinary `Actor` initial properties (handles for `Location, Rotation`).
  `UActorChannel::ReceivedBunch` is unpinned in the shipping binary (asserts stripped,
  vtable-only) but calls `0x140696070`; trace its callers next to settle this. Does
  not block Phases 1-4.

---

## Appendix: reproduce

```bash
TSHARK="C:\Program Files\Wireshark\tshark.exe"
PCAP="D:\RE-Tools\rs2_realserver_capture.pcapng"
# session A bunch dump:
"$TSHARK" -r "$PCAP" -Y "udp.port==57867 && udp.port==7777" -T fields \
  -e frame.number -e udp.srcport -e data.data
```
```python
import sys; sys.path.insert(0, r"D:\smellslikenapalm\tools"); import mock_client as mc
mc.decode_packet(data, bd_max=16384)   # C->S
mc.decode_packet(data, bd_max=12000)   # S->C  (srcport==7777)
# milestone landmarks (session A): join f1477, actor burst f1484, first ServerMove f1522
# session B: join f60857, burst f60859, first ServerMove f60867
```
```
ServerMove first bytes (BOTH sessions): 68 5a 00 00 00 98 72 22 ...   (ch2, handle 664)
short ServerMove: 98 8e 60 01 00  (33 bits, handle 664 + timestamp + zero move)
PC property delta: 17 6a 00  (handle 23) ; 33 ea d0 ... (handle 51) ; 40 ... (handle 64)
```

---

## 6. Generation templates (regeneration recipe for `MakeOpeningActorBunch`)

Bit-decoded from **Session A** (`udp.port==57867`), the opening-actor burst f1484-1494,
cross-checked against **Session B** (`udp.port==56400`, burst f60859). This section is
the per-class recipe our emulator fills with per-session values and emits.

### 6.0 Decoded wire structure of a SerializeNewActor open (corrected) **[H]**

Each open-bunch payload is, in order:

```
[ classNetGUID ]   ; 1 flag bit + SerializeInt  -> CONSTANT per class family
[ actorNetGUID ]   ; 1 flag bit + SerializeInt  -> per-actor (the new instance)
[ initial property block ]  ; handle(SerializeInt(maxHandle)) + typed value, ascending
```

Empirically confirmed by family clustering: the **leading bytes are constant per class**
(`60c10100`=PlayerController, `5aa50200`=PlayerReplicationInfo ×65, `86bb0800`/`8ebb0800`=
Pawn/ROPawn ×9/×14, `0ac10200`=TeamInfo ×3, `ce290200`=GameReplicationInfo ×1). The
recurring leading block is the **class reference** (the doc's earlier "class field"); the
per-actor NetGUID follows it. **Export NetGUIDs read as `SerializeInt(0x80000000)` =
~31 fixed bits** (so the class+actor header is ~64 bits for an export/export pair; ~43
bits when the class resolves statically via `SerializeInt(1023)`).

> **maxHandle pinning status [M/L]:** the *tiny* opens (TeamInfo) decode with a sub-8-bit
> property region and an all-zero tail (initial TeamIndex only). The **PlayerController /
> Pawn initial blocks are dominated by native-serialized compressed Location/Rotation/View
> vectors** (variable, non-byte-aligned widths), so a single constant `maxHandle` that
> consumes them bit-exactly was **not** pinnable from the capture alone (matches §2.2/§3.1
> open item — the native ReceivedBunch type table is unpinned). **The robust recipe below
> therefore regenerates each initial block VERBATIM per class and patches only the
> per-session fields** (actor NetGUID; FString ServerName / PlayerName). The handle-level
> `maxHandle` only matters for authoring *novel* property values; the verbatim templates
> already carry the correct RemoteRole / bNetOwner / TeamIndex bits.

### 6.1 PlayerController — ch2, open FIRST **[H]**

- **class NetGUID:** `60 c1 01 00` (flag=0 export, idx `0xc160`/57568-class-slot). **[H]**
- **maxHandle:** native (PC is `nativereplication`); not a single constant — see note above.
- **Template payload (Session A, 925b, 116B) — BYTE-IDENTICAL in Session B (first 5 bytes
  `60c101001bccea3f4904…` match exactly):** **[H, cross-session]**
  ```
  60c101001bccea3f490460e07d7411444007160988130040226396c6400000444045070500000000
  405d6520430000440072ab0600000000c02d246d5100004400d95e0700000000c0ed13874c000044
  4068b69f410000440050eb974000004480d586dd400000444085372d82590900b8e9c30c
  ```
- The `1bccea3f` immediately after the class ref is **static** (identical across sessions),
  i.e. the PC actor NetGUID is a fixed/known object — emit it verbatim. The block carries
  the inherited `Actor` initial role fields: per §2.3 this is where `RemoteRole=
  ROLE_AutonomousProxy(2)`, `Role=ROLE_Authority(3)` (wire-swapped), `bNetOwner=true` live.
  Because the bytes are session-invariant, **regenerate verbatim — do not re-author.** **[H
  bytes / M field-level role-bit positions]**
- Flags: `bOpen=1 bReliable=1 ChType=2 ChSeq=1`.

### 6.2 GameReplicationInfo — singleton, opens at f1489 (ch54 in A) **[H]**

- **class NetGUID:** `ce 29 02 00` (flag=0 export). Identify by class ref OR the readable
  **ServerName FString**. **[H]**
- **maxHandle:** GRI replicated set (see §3.2): initial `ElapsedTime, GameClass, GoalScore,
  RemainingTime, ServerName` + dirty timer fields → field count is the SerializeInt bound;
  not bit-pinned (native + arrays). Regenerate verbatim, patch the two FStrings. **[M]**
- **Per-session patch points (FStrings, ANSI, INT-length-prefixed):**
  - **ServerName** at ~**bit 437** of the payload: captured value
    `-=PR=-GAMING #5 | RESORT 24/7 | LOW PING | P-R.ONL…`.
  - **second FString** (MOTD/server-tag) at ~**bit 4206**: `-=PR=- #5 |Resort 24/7 Server`,
    plus an asset URL `http://rs2assets.phantomrebels.com/…png`.
- **Template payload (Session A, 4662b, 583B):** stored as `gri_open.hex` recipe; first
  bytes `ce2902004005f131830601002044f843…` (the `2044f843`/`8b00…` region = the
  GoalScore/RemainingTime/ElapsedTime floats). Emit verbatim, splice ServerName. **[H bytes]**
- Flags: `bOpen=1 bReliable=1 ChType=2 ChSeq=1`.

### 6.3 TeamInfo — ×3 (ch21, ch56, ch76), tiny opens **[H]**

- **class NetGUID:** `0a c1 02 00` (flag=0 export). **[H]**
- **maxHandle:** TeamInfo replicates `Score(dirty), TeamIndex(initial), TeamName(initial)`;
  RS2 sends an **empty TeamName** (no readable string found), so the **initial block is just
  TeamIndex** (a few bits) followed by zero padding. **[H — verified: ch21 has 17 all-zero
  trailing bits after the 64-bit header].**
- **Templates (verbatim, Session A — `0ac1020040bd040000…` byte-identical in Session B):**
  ```
  ch21 (81b , 11B): 0ac1020040bd0400000000                    ; team A (TeamIndex bits differ)
  ch56 (389b, 49B): 0ac1020040c50000007fae00000080e6873f…     ; team B + ROTeamInfo float array
  ch76 (483b, 61B): 0ac1020040bd000000009a1efe4a0000803a…     ; third team/neutral + floats
  ```
  The differing region right after the class ref (`40bd04` vs `40c500` vs `40bd00`) encodes
  the **per-team actor NetGUID + TeamIndex**. The two larger TeamInfos additionally carry
  the ROTeamInfo reinforcement/score float arrays (the `873f`≈0.5 floats); for the menu
  bootstrap the **tiny 81-bit form (TeamIndex only) is sufficient** — open one per team. **[H]**
- Flags: `bOpen=1 bReliable=1 ChType=2 ChSeq=1`.

### 6.4 (local) PlayerReplicationInfo — class `5aa50200` **[H]**

- **class NetGUID:** `5a a5 02 00`. The **local** player's PRI is the one whose PlayerName
  FString matches the connecting player — in BOTH captures that is **"Krill"** (ch26 in A,
  ch4 in B). **[H]**
- **Per-session patch points (decl order §3.2: `Score, Deaths, Ping, PlayerName, PlayerID,
  Team, … UniqueId`):**
  - **PlayerName** FString at ~**bit 81** (after `5aa50200`+actorGUID+Score/Deaths/Ping):
    captured `Krill`. Other PRIs decode cleanly to real names (`DodgR, Latte, Johann The
    Terrible, Bronzon, PANZER_SS, Tanza_RF, …`) confirming the offset. **[H]**
  - **Team** = a NetGUID reference to one of the TeamInfo actors above (must resolve).
  - **PlayerID / UniqueId** = per-session ints/Steam id.
- **Template (Session A local PRI ch26, 316b, 40B):**
  `5aa50200402d0d00000096e4d2d8d80042413f33da03000097dd6df48100008800926b0000a00900`
  (the `Krill` bytes are spliced in; here Session A's `Krill` PRI is ch26). **[H bytes]**
- Flags: `bOpen=1 bReliable=1 ChType=2 ChSeq=1`.

### 6.5 Minimal spectator / team-select-menu bootstrap **[H ordering / M sufficiency]**

Observed open ORDER and timing (Session A):

```
f1477 C->S NMT 0x09 (Join)
f1484 OPEN ch2  PlayerController (60c10100)
f1485 OPEN ch21 TeamInfo #1     (0ac10200)
f1486 OPEN ch26 PRI "Krill"     (5aa50200)   <- the local player's PRI
f1489 OPEN ch54 GRI             (ce290200)   <- ServerName, GameClass
f1489 OPEN ch56 TeamInfo #2
f1494 OPEN ch76 TeamInfo #3
…(140+ more PRIs/Pawns)…
f1522 C->S ch2 first ServerMove (0x68) + ch26 PRI RPC 0x52   <- client now interacting
```

The client does **not** send anything until f1522, after the whole burst — but the bunches
that the **HUD / team-select menu** read are: **the GRI** (root replicated object; the menu
binds to `WorldInfo.GRI`), **the TeamInfo set** (`GRI.Teams[]` must resolve to populate the
team buttons), and **the local PlayerController + its PRI** (so the menu has an owning player
to apply the selection to). Pawn channels are **NOT** required for the menu — the menu is a
pre-spawn/spectator UI. So:

> **MINIMAL menu bootstrap = ch2 PlayerController + GRI + the (≥2) TeamInfos + the local
> player's PRI**, opened in that order, each as a verbatim §6.1-6.4 template with the
> actor NetGUIDs made self-consistent against our PackageMap and the FStrings patched. No
> Pawn channel and none of the other 60 PRIs are needed to reach the menu. **[H that the
> menu is pre-pawn; M that GRI+TeamInfo+PC+localPRI is both necessary and sufficient — it
> is the full singleton set the capture opens before any client interaction.]**

The menu is therefore gated on **PC + GRI + TeamInfo (+ local PRI)** — **not** on the PC
alone, and **not** on a Pawn. Camera-unlock/controllable (§5 Phase 3) is the *next*
milestone after the menu (needs the Pawn open + `ClientRestart`), and is separable.

### 6.6 Emitter checklist for `MakeOpeningActorBunch`

1. On C->S `NMT 0x09`: open **ch2** with the §6.1 PC template (verbatim). 
2. Open the **GRI** (§6.2) with our ServerName/GameClass spliced into the FStrings.
3. Open **2-3 TeamInfos** (§6.3) — the tiny 81-bit form is enough; set TeamIndex 0/1.
4. Open the **local PRI** (§6.4) with our PlayerName + a `Team` ref into a TeamInfo above.
5. Each open: `bControl=1,bOpen=1,bReliable=1,ChType=2`, ascending `ChIndex` from 2,
   `ChSeq=1`. After this set the client should present the team-select menu.
6. (Later, for control) spawn+possess a Pawn, open its channel, send `ClientRestart` on
   ch2 (§5 Phase 3).
```
