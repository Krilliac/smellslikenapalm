# GameReplicationInfo (ch54) property replication — decoded from the real-server capture

Capture: `D:\RE-Tools\rs2_realserver_capture.pcapng`
Filter (S2C): `udp.srcport==7777 && udp.dstport==57867`
Decoder: `tools/mock_client.py` (`decode_packet`, S2C `bd_max=12000`)
Class: `ROGameReplicationInfo` (chain `Object<Actor<Info<ReplicationInfo<GameReplicationInfo<ROGameReplicationInfo`)
Authoritative handle map: `tools/netfields_u_ROGameReplicationInfo.txt` (regenerated, now carries the TYPE column) — **maxHandle = 184**.

> Scope note. UE3-7258 (RS2) is the *pre-partial* bunch format: there are **no
> bPartial bits**; a logical bunch larger than a packet is split across **multiple
> reliable bunches reassembled by ChSequence** (see `docs/RS2V_ControlChannel_WireSpec_7258.md` §6,
> `docs/RS2V_PostJoin_Replication_7258.md`). The mock decoder does not reassemble across
> ChSequence and mis-labels continuation bunches, so only the **opening bunch** (the
> bNetInitial block, seq 1) is recovered cleanly here. That is exactly the block the
> task targets.

---

## 1. Where GRI lives on the wire

| Fact | Value |
|---|---|
| Channel index | **ch54** (Session A) |
| Open bunch | **frame 1489**, pid 80, `bOpen=1 bReliable=1 ChType=2 (CHTYPE_Actor) ChSeq=1`, **4662 payload bits (583 B)** |
| Class identity | classNetGUID = **static/packagemap index 70887** = the `ROGameReplicationInfo` class static index (matches the known GRI class index 70887) |
| Singleton | exactly **one** reliable ch54 bunch in the whole capture (the open). No clean reliable ch54 *update* bunch was isolable (decoder can't reassemble multi-bunch packets) |
| Continuation bunches | the GRI initial block overflows one packet. Its tail lands in **frame 1494 ch54 (7096 b — carries `ServerMOTD`)** and **frame 1527 ch54 (7190 b)**. The mock decoder shows them as `bReliable=0` because it does not parse the reliable continuation header — they are GRI ch54 ChSequence continuations, not separate logical bunches. |

The class clustering also identifies the actor: leading bytes `ce 29 02 00` are constant
for the GRI class family (one instance), per `docs/RS2V_ActorReplication_7258.md` §6.0/§6.2.

---

## 2. Open-bunch (`SerializeNewActor`) header — decoded bit-exactly

The 4662-bit payload begins with the UE3 `SerializeNewActor` header, **then** the
property block. Object refs use `UPackageMapLevel::SerializeObject`: `1 selector bit`,
then `flag==0 → SerializeInt(value, 0x80000000)` (≈31 fixed bits) / `flag==1 →
SerializeInt(value, 1024)` (`docs/UE3_PropertyReplication.md` §6).

```
bit   0..31   classNetGUID  : selector=0 (static)  SerializeInt(.,0x80000000) = 70887   (= GRI class)
bit  32..63   actorNetGUID  : selector=0 (export)  SerializeInt(.,0x80000000) = 418939552 (this GRI instance)
bit  64..220  NATIVE open-header (157 bits) — Actor base bNetInitial block, NOT handle/value pairs
              (compressed Location/Rotation/Velocity + Role/RemoteRole/bNetOwner native serialize).
              NOT bit-pinned (native serialization + enum widths) — matches the standing open item in
              docs/RS2V_ActorReplication_7258.md §2.2/§6.0 and docs/UE3_PropertyReplication.md §8.
bit 221..     handle-encoded property block begins  (first handle = SerializeInt(.,184))
```

- class+actor NetGUID pair = exactly **64 bits** (two 31-bit exports + 2 selector
  bits), confirming `docs/RS2V_ActorReplication_7258.md` §6.0 `[classNetGUID][actorNetGUID]` order.
- The first **handle-encoded** property is **`ServerName` (handle 24)** whose
  `SerializeInt(handle,184)` lands at **bit 221** and whose `INT len` field is at bit 229.
  Handles 1–23 (Actor base + `GameReplicationInfo.Winner`) are carried inside the
  native open-header region or were default/unsent; they do not appear as discrete
  handle/value pairs in the recovered stream.

### Per-property wire layout (confirmed against the capture)
`handle = SerializeInt(handle, 184)` (LSB-first minimal-bit; in-range values here use 8 bits),
then, **for `ArrayDim>1` an 8-bit element index**, then the typed value:
`bool=1b`, `byte=8b` (enum byte = `ceilLog2(NumEnums-1)` bits), `int=32b LE`,
`float=32b LE`, `string=INT32 len (incl NUL) + len bytes ANSI`, `object/class =
SerializeObject`. No terminator. (`docs/UE3_PropertyReplication.md` §3–§5.)

---

## 3. GRI properties decoded with concrete values (Session A)

These are the property VALUES actually pulled out of the ch54 stream:

| Handle | Property | Type | Decoded value | Where |
|---:|---|---|---|---|
| 24 | **ServerName** | string | `"    -=PR=-GAMING #5 \| RESORT 24/7 \| LOW PING \| P-R.ONL"` (len 55 incl NUL) | f1489 bit 229 |
| 43 | **ServerMOTD** | string | `"Welcome Soldier! This is the -=PR=- #5 Resort 24/7 server@nl@All -=PR=- servers are ranked. Discord: dsc.gg/joinpr@nl@…"` (865 chars) | f1494 ch54 (continuation), byte 1 |
| 44 | **ServerAdInfo** | struct PreGameServerAdInfo | banner image URL `http://rs2assets.phantomrebels.com/Phantom_Rebels_Artwork_more_prominent.png`; ad title `=PR=- #5 \|Resort 24/7 Server`; website `p-r.onl` | f1489 bits 3526 / 4174 / 4534 |

`@nl@` is the literal newline token the server stores in the MOTD string (decoded ANSI, not a control char).

The numeric scalar fields (`MaxPlayers` h182, `GoalScore` h26, `TimeLimit` h25,
`bMatchHasBegun` h31, `GameClass` h33, the per-team `byte[2]` arrays such as
`PlayersAliveCount` h129 / `bHasHelicopters` h128, the `ObjectiveStatus`/`Objective*`
`byte[16]` arrays h172–179, etc.) are present in the same 4662-bit bunch and the two
continuations, but could **not** be individually bit-pinned because (a) the
`ServerAdInfo` struct and the many static arrays are native-serialized, (b) enum-byte
widths are sub-8-bit, and (c) the value region is reassembled across the 3 ChSequence
bunches the mock decoder cannot stitch. This is the same native-serialization wall
documented in `docs/RS2V_ActorReplication_7258.md` §6.0 and `docs/UE3_PropertyReplication.md` §8.
The strings above are the load-bearing, human-meaningful per-session values; the rest
should be authored from the class defaults + game state per the handle/type table in §4.

---

## 4. Full GRI net-field handle / type table (maxHandle = 184)

Authoritative source `tools/netfields_u_ROGameReplicationInfo.txt`. Handles are the
`SerializeInt(handle,184)` wire values. `func` rows are RPCs sharing the handle space
(handle 0 `DemoRecordSound`, handle 183 `ServerZeroReinforcements`) and never appear as
replicated values. `[N]` = static `ArrayDim` (emits an 8-bit element index before each value).

Actor base (handles 0–22): `1 RelativeRotation` rotator, `2 RelativeLocation` vector,
`3 Velocity` vector, `4 Instigator` obj<Pawn>, `5 Base` obj, `6 Owner` obj,
`7 ReplicatedCollisionType` byte-enum, `8 Role` byte-enum(ENetRole), `9 RemoteRole`
byte-enum(ENetRole), `10 Physics` byte-enum, `11 DrawScale` float, `12 Rotation`
rotator, `13 Location` vector, `14 bNetOwner`…`22 bLoadedFromSavegame` bool.

GameReplicationInfo (23–33):
| h | name | type |
|---:|---|---|
| 23 | Winner | obj<Actor> |
| 24 | ServerName | string |
| 25 | TimeLimit | int |
| 26 | GoalScore | int |
| 27 | RemainingMinute | int |
| 28 | ElapsedTime | int |
| 29 | RemainingTime | int |
| 30 | bMatchIsOver | bool |
| 31 | bMatchHasBegun | bool |
| 32 | bStopCountDown | bool |
| 33 | GameClass | class<GameInfo> |

ROGameReplicationInfo (34–182), notable handles:
| h | name | type |
|---:|---|---|
| 34/35/36 | WeaponXPMultiplier / ClassXPMultiplier / XPMultiplier | float |
| 37 | AllSpawnWindowsStaticDontUse | int[20] |
| 38 | SpawnWindowCloseTime | int[2] |
| 39/40/41 | PlayedRoundsCount / RoundTeamScoreLimit / RoundLimit | int |
| 42 | NoTunnelVolumes | obj<ROVolumeNoTunnels>[16] |
| 43 | **ServerMOTD** | string |
| 44 | **ServerAdInfo** | struct PreGameServerAdInfo |
| 45 | WinStrengthPoints | int |
| 62 | ArtySpawn | struct Vector |
| 68 | TeamObjectivesCaptured | int[2] |
| 69 | TeamsGrandScore | int[2] |
| 70 | TeamsStrengthPoints | int[2] |
| 77 | CampaignTeamCombatPower | float[2] |
| 78–119 | b* match-rule flags (bIsRanked, bBalanceTeams, bMatchHasBegun-class options…) | bool |
| 120 | AlliesSpawnProtection | obj<ROVolumeSpawnProtection>[10] |
| 121 | AxisSpawnProtection | obj<ROVolumeSpawnProtection>[10] |
| 122 | MapBoundaries | obj<ROVolumeMapBoundary>[10] |
| 123 | ResupplyAreas | struct ResupplyInfo[16] |
| 124 | ObjCappers | struct ObjCappersInfo[16] |
| 125–137 | Proximity3DVOIPSetting…RealismMode | byte / byte-enum |
| 127 | SquadSpawnMethod | byte(enum EROSquadSpawnMethod)[2] |
| 128 | bHasHelicopters | byte[2] |
| 129 | PlayersAliveCount | byte[2] |
| 130 | NoTunnelsEnabled | byte[16] |
| 142 | FriendlyPlayerNames | byte |
| 143 | DefendingTeam | byte |
| 144 | CutOffTimer | byte |
| 145/146 | CampaignAttackingTeamPerTurn / CampaignWinningTeamPerTurn | byte[23] |
| 172–179 | CampaignRegionOwners / MiniObjectiveStatus / ObjectiveConnectedToBase / ObjectiveSatchelProgress / ObjectiveCapProgress / ObjectiveForceRatio / ObjectiveStatus / ObjectiveRepIndices | byte[16] / byte[12] |
| 180 | RoleInfoItemsIdx | byte |
| 181 | MaxTeamDifference | byte |
| 182 | **MaxPlayers** | byte |
| 183 | ServerZeroReinforcements | (rpc) |

The complete row-by-row list (all 184 handles, with class, kind, type, ArrayDim and the
real engine `NetIndex`) lives in `tools/netfields_u_ROGameReplicationInfo.txt`.

> GRI has **no `Teams[]` property** in its replicated set — the team objects are
> separate `ROTeamInfo` actor channels (ch56/76/… in this capture). The team-select
> "black squares" are a *TeamInfo* value-replication problem, not a GRI ch54 one.

---

## 5. Reproduce

```
# handle/type table (regenerates tools/netfields_u_ROGameReplicationInfo.txt)
powershell tools/netfields_from_u.ps1 -Class ROGameReplicationInfo
powershell tools/netfield_types.ps1   -Class ROGameReplicationInfo   # UProperty subclasses

# extract + decode (scratchpad scripts; run via Bash with dangerouslyDisableSandbox)
python extract_ch54b.py   # the single reliable ch54 open bunch (frame 1489)
python bit_fstr.py        # bit-level FString recovery: ServerName / ServerMOTD / ServerAdInfo
python fwd.py             # forward property decode anchored at ServerName(h24)@bit221
```

## 6. Open items (unchanged from prior RE)
1. **Native open-header split** (bits 64–220): which of Location/Rotation/Velocity/Role
   bits are present and their exact widths — native `UActorChannel::SerializeNewActor`,
   not pinned from the capture.
2. **Per-scalar numeric values** (MaxPlayers, GoalScore, the byte[2]/byte[16] arrays):
   blocked by native struct/array serialization + multi-bunch ChSequence reassembly the
   current `mock_client` decoder does not perform.
3. **GRI delta updates** over time (ElapsedTime/RemainingTime ticking): not isolable —
   they ride as 2nd+ bunches in multi-bunch packets where the decoder desyncs. Fixing
   `mock_client` to reassemble reliable bunches by ChSequence would unlock both (2) and (3).
