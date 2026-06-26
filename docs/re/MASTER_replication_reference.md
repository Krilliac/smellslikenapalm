# MASTER Replication Reference — RS2:Vietnam (UE3 EngineVersion 7258)

**THE consolidated ground truth for all replication work on smellslikenapalm.**
Built by reconciling the Decode-fleet findings against the retail client↔server
capture `D:\RE-Tools\rs2_realserver_capture.pcapng` and the UE3 C++ source at
`D:\RE-Tools\UE3-src`. Every claim here is either bit-exact against the capture
`[H]`, source-cited `[S]`, or flagged as unresolved `[?]`.

Companion files (do not duplicate — cite):
- `docs/re/ue3_property_value_codec.md` — source-of-truth value codec
- `docs/re/open_bunch_structure.md` — open-bunch (SerializeNewActor) bit layout (CANONICAL)
- `docs/re/postjoin_packet_timeline.md` — packet burst pacing
- `docs/re/pc_ch2_postjoin_timeline.md` — ch2 RPC sequence
- `docs/re/gri_ch54_properties.md`, `teaminfo_channels_properties.md`,
  `pri_channels_properties.md`, `pawn_spawn_replication.md` — per-actor decodes
- `docs/re/hang_rootcause_teaminfo.md` — the hang analysis (drives `SAFE_STATE_REP_PLAN.md`)
- `docs/re/netfields_all_classes.md` + `tools/netfields_u_<Class>.txt` — full handle tables

---

## 0. Quick constants (memorize these)

| Thing | Value | Source |
|---|---|---|
| S2C decode bound `bd_max` | 12000 (MaxPacket 1500B) | [H] |
| C2S decode bound `bd_max` | 16384 | [H] |
| Bunch chIndex field | `SerializeInt(ci, 1023)` | [H] |
| Bunch chSequence (reliable) | `SerializeInt(sq, 1024)` | [H] |
| `MAX_OBJECT_INDEX` (static ref) | `0x80000000` (1<<31) → 31 ranged bits | [S] UnCoreNet.h:100 |
| Dynamic channel ref / None bound | **1024** (this build) — see §3 caveat | [H] |
| Open flags (actor open) | bControl=1 bOpen=1 bClose=0 bReliable=1 ChType=2 ChSeq=1 | [H] |
| CHTYPE_Control / CHTYPE_Actor | 1 / 2 | [S] UnNet.h:59 |
| maxHandle ROPlayerController | 531 | [H] |
| maxHandle ROGameReplicationInfo | 184 | [H] |
| maxHandle ROTeamInfo | 78 | [H] |
| maxHandle ROPlayerReplicationInfo | 98 | [H] |
| maxHandle ROPawn / ROWeapon | 170 / 99 | [H] |

Static class indices (selector=0 + `SerializeInt(idx, 0x80000000)`, emitted on the
wire LE as `idx<<1`):

| Class | static idx | LE bytes | channel(s) |
|---|---|---|---|
| ROPlayerController | 57520 | `60 c1 01 00` | ch2 |
| ROGameReplicationInfo | 70887 | `ce 29 02 00` | ch54 |
| ROPlayerReplicationInfo | 86701 | `5a a5 02 00` | ch13,14,16,17,…(~65) |
| ROTeamInfo | 90245 | `0a c1 02 00` | ch21, ch56, ch76 |
| ROPawn subclasses | 285994–286464 | — | respawn clusters |
| ROWeapon/Inventory | 82735, 75939, 286097… | — | co-located w/ pawn |

---

## 1. Channel → Actor map (post-Join world burst)

| Channel | Actor | TeamIndex / notes |
|---|---|---|
| ch0 | Control channel (NMT only) | not an actor |
| ch2 | ROPlayerController (LOCAL, NetPlayerIndex=0) | the menu driver |
| ch21 | ROTeamInfo | TeamIndex **2** (spectator/neutral, inert; 81-bit open only) |
| ch54 | ROGameReplicationInfo | the big actor (4662-bit open + continuations) |
| ch56 | ROTeamInfo | TeamIndex **1** (playing team) |
| ch76 | ROTeamInfo | TeamIndex **0** (playing team) |
| ch13,14,16,17,18,26,… | ROPlayerReplicationInfo (one per player) | ch26 = local "Krill" |
| ch59+ | rest of burst: more PRIs/pawns/inventory/static actors | |

Burst shape: 52 S2C packets pid 75..126 (f1484..f1559), opening ch2..ch153, ~2.48s,
front-loaded (45 channels in first ~7ms). Each datagram coalesces up to ~1280B,
avg 25 bunches/packet. ch2 open is the FIRST actor bunch; NMT 0x24 (`24 01 00 00 00`)
is the 2nd bunch of that first packet. See `postjoin_packet_timeline.md`.

---

## 2. Open-bunch (SerializeNewActor) layout — CANONICAL

The opening bunch of an actor channel carries, in order:

```
[ classRef       ] 32 bits = 1 selector bit(=0) + SerializeInt(idx, 0x80000000)
                   decode first 4 payload bytes as LE u32 V: selector=V&1, idx=V>>1
[ Location        ] FVector::SerializeCompressed — ALWAYS present (zero vec = 11 bits)
[ Rotation        ] FRotator::SerializeCompressed — ONLY if class bNetInitialRotation
                    (FALSE for PC/GRI/TeamInfo/PRI; true only for projectiles/turrets)
[ NetPlayerIndex  ] 8-bit BYTE — ONLY for PlayerController channels (ch2 = 0) [?width]
[ property block  ] repeat { SerializeInt(handle,maxHandle); typedValue } until
                    BunchDataBits exhausted. NO terminator handle.
```

**There is NO separate per-actor NetGUID.** Identity = channel index. (This RETRACTS
the earlier `gri_ch54_properties.md` claim of a 64-bit `[classNetGUID][actorNetGUID]`
pair and the old `RS2V_ActorReplication_7258.md §6.0` — the supposed 32 "actorNetGUID"
bits are the compressed Location. Removing them makes GRI/TeamInfo/PRI decode
bit-exact.) Verified: TeamInfo and PRI opens consume to exactly 0 bits remaining;
PC header confirmed by NetPlayerIndex==0. `[H]`

`[?]` NetPlayerIndex width: `open_bunch_structure.md` reads it bit-exact as an 8-bit
byte (PlayerController.uc:351); `ActorReplication.h`/`pawn_spawn_replication.md` model
it as `SerializeInt(.,MAX_CHANNELS)`. For value 0 both are indistinguishable. Treat as
**byte 0** for the local PC until a non-zero NetPlayerIndex is captured.

### Compressed Vector (FVector::SerializeCompressed, UnMath.cpp:51) `[S][H]`
```
Bits = Clamp(CeilLogTwo(1+max(|ix|,|iy|,|iz|)),1,20)-1   ; ix=round(x) etc.
SerializeInt(Bits, 20)                                    ; magnitude class
for each axis: SerializeInt(comp + (1<<(Bits+1)), 1<<(Bits+2))   ; Bits+2 bits each
```
Zero vector: Bits=0 → `SerializeInt(0,20)` (5 bits) + 3×`SerializeInt(2,4)` (2 bits) = **11 bits**.
Components are integer-rounded — precision is lost. Decoded PC Location = (-831,4085,292).

### Compressed Rotator (FRotator::SerializeCompressed, UnMath.cpp:84) `[S]`
Per (Pitch,Yaw,Roll): 1 presence bit; if `(angle>>8)!=0`, the 8-bit high byte. Low
byte dropped. Absent for all menu actors (bNetInitialRotation=false).

---

## 3. Object-reference / NetGUID codec (UPackageMap::SerializeObject) `[S][H]`

```
[ selector bit ]
  selector == 0  -> STATIC object: SerializeInt(index, 0x80000000)   (~31 bits)
                    index = package.ObjectBase + Object->NetIndex (deterministic)
  selector == 1  -> DYNAMIC actor:  SerializeInt(channelIndex, 1024)  (~10 bits)
                    value = the target actor's OPEN channel index; <=0 means None/NULL
None = selector 1 + SerializeInt(0,1024)  (all-zero ~10 bits)
```

Verified polarity: GRI class ref `ce 29 02 00` → strip selector bit → 70887. PRI.Team
(handle 35) decoded as selector=1 + `SerializeInt(ch,1024)` → ch76/ch56 (open TeamInfo
channels), bit-exact. `[H]`

**`[?]` CHANNEL-BOUND DISCREPANCY (high-risk, one bit shifts everything downstream):**
generic UE3 source defines `MAX_NET_CHANNELS=2048` (UnConn.h:143 → 11-bit channel/None
field) but every BIT-EXACT decode in this capture (PRI.Team, bunch chIndex, None refs)
reproduces only with **1024** (10-bit). `ActorReplication.h kDynamicChannelMax=1024`
matches the capture. **Use 1024 for this build.** If a future re-cook desyncs object
refs by one bit, this is the first suspect.

---

## 4. Property-value codec (per UProperty type) `[S]` ue3_property_value_codec.md

Per replicated property on the wire:
```
handle = SerializeInt(FieldNetIndex, maxHandle)        ; LSB-first ranged, ~ceil(log2(maxHandle)) bits
if (property->ArrayDim != 1):  elementIndex = 8 RAW bits   ; fixed C arrays ONLY (UnChan.cpp:1514)
value  = NetSerializeItem(...)                         ; see table
```
No count, no terminator: the property loop ends when the bunch runs out of bits (or a
handle fails to resolve). **Static-array element index is a RAW 8-bit byte** — NOT
`SerializeInt(elem,ArrayDim)` (corrects older notes; both PRI and TeamInfo decodes confirm).

| Type | Wire encoding |
|---|---|
| bool | exactly 1 bit (each replicated bool is its own handle) |
| byte | 8 raw bits |
| byte(enum E) | `ceil(log2(NumEnums-1))` bits, NumEnums INCLUDES autogen `_MAX` (4 values→5→2 bits) |
| int | 32 raw bits LE |
| float | 32 raw bits IEEE-754 LE |
| FString | int32 SaveNum (raw LE) + chars. `SaveNum>0`→1-byte ANSI, `<0`→2-byte UTF-16LE. **\|SaveNum\| INCLUDES the trailing NUL** ("Krill"→6, ""→0) |
| object/class ref | §3 codec (selector bit + ranged int) |
| struct UniqueNetId | 1 QWORD = 64 bits LE (SteamID64 verified) |
| struct Vector | FVector::SerializeCompressed (§2) |
| struct Rotator | FRotator::SerializeCompressed (§2) |
| struct (other) | members in declaration order, no framing |
| dynamic array (TArray) | **NO-OP** — UArrayProperty::NetSerializeItem writes nothing (UnProp.cpp:3365). Dynamic-array CONTENTS are not transmitted via the property path |

Wire ORDER of properties = class RepProperties declaration order (NOT ascending
handle). Decode is order-independent because each value carries its handle. The ONLY
hard requirement: **the bunch must END exactly on the last value's last bit** — stray
pad bits get misread as a bogus handle. (This is why non-bit-exact payloads HUNG the
client.)

---

## 5. Per-class net-field tables (handles + types)

Full tables: `tools/netfields_u_<Class>.txt`. Handle space is base-class-first; each
property occupies ONE handle regardless of ArrayDim. Below = the handles that matter
for menu/state replication.

### 5.1 ROGameReplicationInfo (ch54) — maxHandle 184
| h | name | type | note |
|---|---|---|---|
| 24 | ServerName | FString | menu-critical |
| 25 | TimeLimit | int | |
| 26 | GoalScore | int | |
| 28 | ElapsedTime | int | |
| 29 | RemainingTime | int | |
| 31 | bMatchHasBegun | bool | menu-critical |
| 32 | bStopCountDown | bool | |
| 33 | GameClass | class<GameInfo> | static class ref |
| 43 | ServerMOTD | FString | overflows to continuation bunch |
| 44 | ServerAdInfo | struct PreGameServerAdInfo | banner URL/title/website |
| 128 | bHasHelicopters | byte[2] | array (8-bit elem idx) |
| 129 | PlayersAliveCount | byte[2] | array |
| 172–179 | Objective* | byte[16] | arrays |
| 182 | MaxPlayers | byte | menu-critical |
| 183 | ServerZeroReinforcements | (RPC) | handle 183 |
| 0 | DemoRecordSound | (RPC) | Actor root |
**GRI has NO replicated Teams[] array** — clients rebuild it from individual ROTeamInfo channels.

### 5.2 ROTeamInfo (ch21/56/76) — maxHandle 78
| h | name | type | note |
|---|---|---|---|
| 23 | TeamIndex | int (32b) | **the team-button identity**; default -1 |
| 24 | Score | float | |
| 25 | TeamName | FString | NEVER replicated by real server (stays default empty) |
| 42 | SavedArtilleryCoords | struct Vector | |
| 55 | TeamLocationArray | struct Vector[32] | array |
| 62 | ReinforcementsRemaining | int | |
| 65 | SelectedArtyIndex | byte | |
| 77 | NumPlayers | byte | |
TeamInfo block starts at h23 (TeamInfo super at FieldsBase 23). ROTeamInfo own props h26–77.

### 5.3 ROPlayerReplicationInfo (ch13,14,…) — maxHandle 98
| h | name | type | note |
|---|---|---|---|
| 23 | UniqueId | struct UniqueNetId | 64-bit SteamID64 |
| 31 | bWaitingPlayer | bool | 1 for joining/spectating player |
| 32 | bOnlySpectator | bool | |
| 33 | bIsSpectator | bool | |
| 35 | **Team** | object→TeamInfo | **selector=1 + SerializeInt(teamInfoChannel,1024)**; sent as DELTA after TeamInfo open |
| 36 | PlayerID | int | |
| 37 | PlayerName | FString | menu-critical |
| 38 | Ping | byte | |
| 39 | Deaths | int | |
| 40 | Score | float | |
| 72/73 | ClassRank/HonorLevel | byte | player level |
| 79/80/81 | ClassIndex/RoleIndex/SquadIndex | byte | selection |
Send order = UnrealScript declaration order (Score,Deaths,Ping,PlayerName,StartTime,UniqueId,…).

### 5.4 ROPlayerController (ch2) — maxHandle 531
RPC handles used in the post-Join flow (full table `tools/netfields_u_global.txt`,
handle index = column 0):
| h | name | dir | role |
|---|---|---|---|
| 23 | PlayerReplicationInfo | prop | — |
| 24 | **Pawn** | prop (obj ref) | points at local pawn channel (CORRECTED: was wrongly 23) |
| 41 | ClientGotoState | RPC | enters menu/spectate state |
| 45 | ClientSetHUD | RPC | (NOT sent in menu phase) |
| 85 | ClientRestart | RPC | accept pawn, leave menu (spawn flow) |
| 150 | ClientOnPossess | RPC | |
| 172 | ChangedTeams | RPC | post-pick confirmation (NOT the menu opener) |
| 206 | **ClientShowTeamSelect** | RPC | **opens the team-select menu** (handle-only, 9-bit bunch) |
| 210 | ChangedRole | RPC | |

### 5.5 ROPawn (maxHandle 170) / ROWeapon (maxHandle 99)
Spawn wiring handles — ROPawn: 4 Instigator, 6 Owner, 8/9 Role/RemoteRole (owner gets
RemoteRole=AutonomousProxy=2, others SimulatedProxy=1), 14 bNetOwner, 27 InvManager,
32 PlayerReplicationInfo(→PRI ch), 52 Controller(→ch2). ROWeapon: 23 InvManager,
24 Inventory(next item), 89/91/92 ammo. See `pawn_spawn_replication.md`.

---

## 6. Post-Join RPC / property timeline (ch2, reliable chSeq order) `[H]`

```
chSeq 1  = ch2 OPEN (925-bit: ROPC class + Location + NetPlayerIndex=0 + initial props)
        + NMT 0x24 (24 01 00 00 00) as 2nd bunch of same packet
chSeq 2  = TeamMessage (h56)
chSeq 3-7= ClientMutePlayer (h112) ×5
chSeq 8  = ClientShowTeamSelect (h206)   <-- OPENS THE TEAM MENU (payload 'ce 00', 9 bits)
chSeq 9  = ClientGotoState (h41)         <-- enters menu/spectate state
chSeq 10 = ForceEscapeSceneUpdate (h405)
... Mute/Unmute spam + periodic ClientGotoState ...
chSeq 21 = ClientUpdateSpectatorCameraSpeed (h179)
chSeq 29 = ClientUpdateBestNextHosts (h36)
chSeq 30 = ChangedRole (h210)
chSeq 31-33 = ClientSetViewTarget (h87)
chSeq 44 = ClientSetCameraMode (h61)
```
Minimal causal chain to open the menu shell = **ClientShowTeamSelect(206) → ClientGotoState(41)**.
ChangedTeams/ClientSetHUD/ClientRestart are NEVER sent in the menu phase.
Unreliable ch2 background (chSeq=0): WwiseClientHearSound(51), PRI ref(23), RepHitInfo(320), etc.

Possession onset (local pawn): C2S ch2 switches from ~52-bit menu RPCs to ~205-bit
ServerMove at **t≈146 (f4266)**. team/role select occupies t103→t146.

Control-channel heartbeat: NMT 0x23 = byte 0x23 + int32 len 0x18 + 24 opaque bytes,
reliable, BIDIRECTIONAL, rolling trailing nonce.

---

## 7. Known reconciliations / corrections (read before trusting old docs)

1. **No actorNetGUID** in open header (§2). Supersedes gri_ch54 64-bit pair claim.
2. **ch2 Pawn = handle 24**, not 23 (handle 23 = PlayerReplicationInfo).
3. **Team menu opener = ClientShowTeamSelect(206)**, not ChangedTeams(172).
4. **Static-array element index = raw 8-bit byte**, not SerializeInt(elem,ArrayDim).
5. **Dynamic channel bound = 1024** in this build (source says 2048 — §3 caveat).
6. **TeamIndex rides the OPEN bunch** as a bNetInitial property — never a standalone later bunch (the hang root cause; see SAFE_STATE_REP_PLAN.md).

---

## 8. Tooling / reproduce
- `tools/mock_client.py` — `decode_packet(data, bd_max=12000 S2C / 16384 C2S)`.
- `tools/netfields_all.ps1 -Class <ALL|Class>` — regenerate handle+type tables.
- `tools/gen_actor_bootstrap.py` — capture→`data/actor_bootstrap.bin` (record:
  `u16 chIndex | u8 chType | u8 flags(b0 bOpen,b1 bClose,b2 bReliable,b3 bControl) | u16 chSeq | u32 bunchDataBits | payload`).
- Consumer: `ConnectionManager::SendActorBootstrap` (src/Network/ConnectionManager.cpp:631).
- Builders: `ActorRepl::WriteActorOpenHeader` + `WriteProp*` (src/Network/ActorReplication.h).
- Wire filters: S2C `udp.srcport==7777 && udp.dstport==57867`; C2S swap.
