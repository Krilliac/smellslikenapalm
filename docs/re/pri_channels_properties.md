# PlayerReplicationInfo replication — wire-decoded (RS2:Vietnam, EngineVersion 7258)

Bit-exact decode of `(RO)PlayerReplicationInfo` actor channels from the real-server
capture `D:\RE-Tools\rs2_realserver_capture.pcapng`, **Session A**
(`udp.srcport==7777 && udp.dstport==57867` for S2C). Decoder reuses
`tools/mock_client.py` (`decode_packet(data, bd_max=12000)` for S2C) + a typed
property-block walker. **Every PRI bunch decoded here consumes its `BunchDataBits`
EXACTLY** (e.g. 971/971, 1030/1030, 1243/1243, 316/316, and every delta) — the
strongest possible confirmation the layout below is correct.

Confidence: **[H]** = bit-exact, multiple channels. Companion specs:
`docs/UE3_PropertyReplication.md` (codec source-of-truth) and
`docs/RS2V_ActorReplication_7258.md` (actor-open burst).

---

## 0. TL;DR

- **PRI class NetGUID index = `86701` (0x152ad)**, serialized as the first 4 bytes
  `5a a5 02 00` of every PRI open bunch (`SerializeObject` STATIC: 1 selector bit `0`
  + `SerializeInt(86701, 1<<31)` = 32 bits total). **[H]**
- **PRI open-bunch header (`SerializeNewActor`) = `classRef(32b)` + `Location`
  (`FVector::SerializeCompressed`, 11 bits for a zero vector) = 43 bits.** No Rotation,
  no separate actor-NetGUID field. Property block starts at **bit 43**. Verified
  bit-exact on 6 independent PRI channels (ch13/14/16/17/18/26). **[H]**
- **`maxHandle` for the handle `SerializeInt` = 98** (= `ROPlayerReplicationInfo`
  `GetMaxIndex()`); each property handle is ~7 bits. **[H]**
- **`PRI.Team` (handle 35) is an OBJECT REFERENCE encoded as a DYNAMIC channel ref:**
  1 selector bit = `1` + `SerializeInt(channelIndex, 1024)` = **the TeamInfo actor's
  open channel index**. Observed: ch13(`DodgR`).Team → channel **76**, ch14(`Latte`).Team
  → channel **56** — both are open `ROTeamInfo` channels. The TeamInfo channel must be
  open before Team is replicated, or the ref won't resolve. **[H]**
- **Property send order = UnrealScript declaration order** (Score, Deaths, Ping,
  PlayerName, StartTime, UniqueId, Kills, PlayerID, …), each prefixed by its
  NetIndex-based handle (which runs roughly *opposite*: Score=40 … UniqueId=23). The
  handle is self-describing so order doesn't affect decoding. **[H]**

---

## 1. PRI channels in the capture

Open-bunch class index `86701` identifies a PRI. Session A opens **~65 PRI channels**
(one per connected player) in the f1485–f1530 actor burst. Examples (frame f1485):

| Channel | Player (PlayerName) | open bits | Team (delta) |
|---|---|---|---|
| ch13 | `DodgR`  | 971  | → ch76 |
| ch14 | `Latte`  | 973  | → ch56 |
| ch16 | `Cy`     | 1030 | — |
| ch17 | `Johann The Terrible` | 1243 | — |
| ch18 | `Ace`    | 749  | — |
| ch26 | `Krill`  | 316  | — (local player; spectator/waiting) |

`ch26 = the connecting local player ("Krill")` — its PRI is short (316b) because the
local player just joined and is a spectator (no score yet).

Open-bunch flags for all PRIs: `bControl=1, bOpen=1, bReliable=1, ChType=2, ChSeq=1`.

---

## 2. Open-bunch (`SerializeNewActor`) header — bit-exact

```
[ classRef ]   SerializeObject, STATIC: selector bit 0 + SerializeInt(idx, 1<<31)
               -> idx = 86701 ; occupies exactly 32 bits = bytes "5a a5 02 00"
[ Location ]   FVector::SerializeCompressed:
               SerializeInt(magBits,20) + 3x SerializeInt(comp+Bias, 1<<(magBits+2))
               -> all PRIs sent (0,0,0): magBits=0 -> 11 bits total
=> property block starts at bit 43
```

There is **no Rotation** and **no separate actor-NetGUID** in the PRI open header
(both absent for all 6 channels; including either misaligns the block and breaks the
exact bit-consumption). This pins the `RS2V_ActorReplication_7258.md` §2.2 "[M] open
header field split" open item **for PRI**: header = `class + Location` only. **[H]**

---

## 3. Property-block format

Per `docs/UE3_PropertyReplication.md`: repeated `(handle, value)` with **no terminator**,
running until `BunchDataBits` is exhausted.

```
handle = SerializeInt(FieldNetIndex, maxHandle=98)         ; ~7 bits, LSB-first
if (ArrayDim > 1):  arrayIndex = 8 raw bits                 ; ONLY for array props
value  = <type-specific> (see §4)
```

**Array properties** send one `(handle, 8-bit index, value)` triple PER element. E.g.
`RecentAchievements` (byte[3]) → three 23-bit entries (7 handle + 8 index + 8 byte);
`PRIAccoladeAccums` (int[11]) → 46-bit entries (7 + 8 + 32); `ReplicatedCharConfig`
(byte[11]) → 23-bit entries.

---

## 4. PRI net-field table (handle → type → wire) — from compiled `.u` + capture

Handles from `tools/netfields_u_ROPlayerReplicationInfo.txt` (built by
`tools/netfields_from_u.ps1`); types from `tools/nettypes_from_u.ps1`. `maxHandle = 98`.
The handle space is base-class-first: Actor 0–22, PlayerReplicationInfo 23–42,
ROPlayerReplicationInfo 43–97.

### 4.1 The replicated PRI properties actually seen on the wire

| Handle | Name | UE type | Wire encoding | Example value (capture) |
|---|---|---|---|---|
| **23** | UniqueId | struct `UniqueNetId` | **64 bits LE** (SteamID64) | `76561198366454879` (DodgR) |
| **24** | Kills | int | 32 bits LE | 11 |
| **25** | StartTime | int | 32 bits LE | 765 |
| **31** | bWaitingPlayer | bool | 1 bit | 1 (Krill) |
| **32** | **bOnlySpectator** | bool | 1 bit | 1 (Krill) |
| **33** | bIsSpectator | bool | 1 bit | 1 (Krill) |
| **35** | **Team** | object → TeamInfo | **1 sel bit `1` + SerializeInt(ch,1024)** | → channel 76 / 56 (§5) |
| **36** | PlayerID | int | 32 bits LE | 803 |
| **37** | **PlayerName** | FString | **int32 len (incl NUL) + ANSI chars** | "DodgR","Latte","Krill",… |
| **38** | Ping | byte | 8 bits | 16 |
| **39** | Deaths | int | 32 bits LE | 10 |
| **40** | Score | float | 32 bits LE IEEE-754 | 164.0 |
| 43 | OwnedDLCPacks | int | 32 LE | 4 |
| 44 | FriendlyFireDamage | int | 32 LE | 10050 |
| 45 | PRIAccoladeAccums | int[11] | (idx8 + int32) per elem | — |
| 46 | DishonorScore | float | 32 LE | 10.0 |
| 47 | MatchScore | float | 32 LE | 50.0 |
| 48 | LastTimeWeDied | int | 32 LE | 1100 |
| 61 | bDead | bool | 1 bit | 1 |
| 62 | ReplicatedCharConfig | byte[11] | (idx8 + byte8) per elem | — |
| 63 | TeamHelicopterSeatIndex | byte | 8 bits | 255 |
| 64 | TeamHelicopterArrayIndex | byte | 8 bits | 5 |
| 67 | TeamPRIArrayIndex | byte | 8 bits | 15 |
| 70 | RecentAchievements | byte[3] | (idx8 + byte8) per elem | — |
| 72 | **ClassRank** | byte | 8 bits | 0 |
| 73 | **HonorLevel** | byte | 8 bits | 51, 99 (player level/rank) |
| 74 | FriendlyFireKillsThisRound | byte | 8 bits | 6 |
| 75 | FriendlyFireKills | byte | 8 bits | 1 |
| 76 | VoicePackIndex | byte | 8 bits | 1 |
| 77 | SpawnSelection | byte | 8 bits | 129 |
| 79 | **ClassIndex** | byte | 8 bits | 1 |
| 80 | **RoleIndex** | byte | 8 bits | 5 |
| 81 | **SquadIndex** | byte | 8 bits | 4 |

**The player's "level/rank" fields are `HonorLevel`(73) + `ClassRank`(72)**; the chosen
role/class/squad are `RoleIndex`(80) / `ClassIndex`(79) / `SquadIndex`(81). All plain
bytes (8 bits).

### 4.2 Notes / gotchas

- **`Team`(35) is NOT in any open block.** It is sent later as a *delta* (it's a
  non-initial replicated prop, and its target TeamInfo channel must exist first). All
  6 open blocks omit Team; ch13/ch14 send it as a delta at f1512/f1537. **[H]**
- **UniqueId** decodes to a valid SteamID64 (`76561198…`) as a flat 64-bit LE value —
  i.e. the `UniqueNetId` struct net-serializes as its single QWORD member, no extra
  framing. **[H]**
- **PlayerName** FString length is `+Num` (ANSI, 1 byte/char) and **includes the
  trailing NUL** ("Krill" → len 6, bytes `K r i l l \0`). **[H]**
- **bOnlySpectator**(32)/bIsSpectator(33)/bWaitingPlayer(31) = 1 for the local joining
  player — it is in the team-select/spectator state. Active players (DodgR/Latte/…)
  have them 0 and carry full Score/Deaths/Kills instead.

---

## 5. `PRI.Team` encoding — the headline (why team buttons were black)

`PRI.Team` (handle 35) is a `UObjectProperty` (`->TeamInfo`). Per
`UPackageMap::SerializeObject` (`docs/UE3_PropertyReplication.md` §6), an object that
is a **dynamic actor with an open channel** is sent as:

```
handle  = SerializeInt(35, 98)            ; ~7 bits
selector= 1 bit, value 1                  ; 1 = DYNAMIC (actor has a channel)
chIndex = SerializeInt(channelIndex, 1024); the TeamInfo actor's OPEN channel index
```

Decoded deltas (Session A):

```
ch13 (DodgR) f1512/f1537:  h35 Team -> channel 76    (ROTeamInfo ch76)
ch14 (Latte) f1512/f1537:  h35 Team -> channel 56    (ROTeamInfo ch56)
```

`ch56` and `ch76` are the open `ROTeamInfo` channels (class idx 90245) from the actor
burst (matches `RS2V_ActorReplication_7258.md` §6.3: TeamInfos at ch21/ch56/ch76). So
**`PRI.Team` literally carries the channel number of an already-open TeamInfo actor**;
the client resolves channel→actor to bind the reference.

### Implication for the emulator (black team buttons)
The team-select menu binds team buttons to `GRI.Teams[]` / `PRI.Team` → `ROTeamInfo`.
For the ref to resolve and the button to render its team, the server must:
1. **Open the TeamInfo channel(s) FIRST** (class idx 90245), with a valid initial
   `TeamIndex` (ROTeamInfo open header = `classRef(32) + Location(11)` then handle
   `TeamIndex`; see the sibling TeamInfo decode), and
2. **Then replicate `PRI.Team`(35)** as a dynamic ref = selector bit 1 +
   `SerializeInt(thatTeamInfoChannel, 1024)`.

If Team is sent before the TeamInfo channel is open, or as a static/None ref, the
client gets an unresolved/None team → **black square**. (An earlier attempt that "hung
the client" was likely a malformed Team ref or an out-of-range channel index causing a
read overrun.)

---

## 6. Worked open-block decodes (bit offsets)

### ch26 — local player "Krill" (316 bits, 316/316 exact)
```
@0   classRef static idx=86701            (32b)
@32  Location (0,0,0) magBits=0           (11b)
@43  h37 PlayerName      = "Krill"        FString
@129 h33 bIsSpectator    = 1
@137 h32 bOnlySpectator  = 1
@145 h31 bWaitingPlayer  = 1
@153 h25 StartTime       = 986            int
@192 h23 UniqueId        = 76561198025857979   (SteamID64)
@263 h36 PlayerID        = 860            int
@301 h77 SpawnSelection  = 0              byte
```

### ch13 — "DodgR" (971 bits, 971/971 exact) — abridged
```
@43  h40 Score=164.0  @81 h39 Deaths=10  @119 h38 Ping=16  @133 h37 PlayerName="DodgR"
@219 h25 StartTime=765  @258 h23 UniqueId=76561198366454879  @329 h24 Kills=11
@368 h36 PlayerID=803  @406 h81 SquadIndex=0  @421 h80 RoleIndex=1  @436 h79 ClassIndex=1
@466 h47 MatchScore=50.0  @504 h46 DishonorScore=10.0  @557 h73 HonorLevel=51  @572 h72 ClassRank=0
... h70 RecentAchievements[3], h43 OwnedDLCPacks, h67 TeamPRIArrayIndex, h62 ReplicatedCharConfig[5],
    h45 PRIAccoladeAccums[3] ... -> 971/971
```

Note the **send order = declaration order** (Score, Deaths, Ping, PlayerName, StartTime,
UniqueId, Kills, PlayerID, then ROPRI fields), while the *handles* are NetIndex-based.

---

## 7. Reproduce

```bash
TSHARK="C:\Program Files\Wireshark\tshark.exe"; PCAP="D:\RE-Tools\rs2_realserver_capture.pcapng"
# dump S2C, decode a PRI open/delta with mock_client + the typed walker
"$TSHARK" -r "$PCAP" -Y "udp.srcport==7777 && udp.dstport==57867" -T fields \
   -e frame.number -e data.data
```
```python
import sys; sys.path.insert(0,r"D:\smellslikenapalm\tools"); import mock_client as mc
# header: classRef = 1 bit(0)+SerializeInt(1<<31); Location = SerializeInt(20)+3*SerializeInt(1<<(b+2))
# then loop: handle=SerializeInt(98); value per type (bool 1b, byte 8b, int/float 32b LE,
#            FString int32 len+chars, object 1 selbit + (sel? SerializeInt(1024): SerializeInt(1<<31)),
#            UniqueId 64b, array prop adds 8-bit index before value)
mc.decode_packet(data, bd_max=12000)   # S2C bunches
# landmarks (Session A): PRI opens at f1485; class idx 86701; ch26="Krill"
```
Tools added by this pass: `tools/nettypes_from_u.ps1` (UELib net-field TYPE dump).
```
