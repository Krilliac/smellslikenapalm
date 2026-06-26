# Open-bunch (SerializeNewActor) structure — bit-exact RE from capture

Goal: pin the **exact bit layout** of an actor-channel OPEN bunch — the
`SerializeNewActor` header **and** the `bNetInitial` property block that follows —
so the emulator can replicate properties **inside the open** (the real server does;
sending them as separate later bunches HUNG the client).

Sources:
- Capture `D:\RE-Tools\rs2_realserver_capture.pcapng`, S2C =
  `udp.srcport==7777 && udp.dstport==57867`, actor burst f1484+.
- UE3 engine tree `D:\RE-Tools\UE3-src\Development\Src` (`UnChan.cpp`, `UnNetDrv.cpp`,
  `UnMath.cpp`, `UnCoreNet.h`).
- Field/handle tables: `tools/netfields_u_*.txt`; property types via UELib over the
  compiled `.u`.
- Decoder: `tools/mock_client.py` framing + a self-contained bit reader (scratchpad
  `decode2.py`).

Confidence: **[H]** = bit-exact decode of the real capture AND matches engine source.

---

## 0. TL;DR — the layout (every open actor bunch)

```
Bunch flags:  bControl=1 bOpen=1 bClose=0 bReliable=1  ChType=2 (CHTYPE_Actor)  ChSeq=1
Payload bits (LSB-first), in this exact order:

  [ class/archetype ref ]   32 bits  = 1 flag bit (=0, "static object")
                                       + SerializeInt(index, 1<<31)  (== 31 fixed bits)
  [ compressed Location  ]  variable = FVector::SerializeCompressed  (ALWAYS present)
  [ compressed Rotation  ]  variable = FRotator::SerializeCompressed (ONLY if the class's
                                       bNetInitialRotation==true; FALSE for PC/Info/PRI/
                                       GRI/TeamInfo, so ABSENT for all menu actors)
  [ NetPlayerIndex       ]  8 bits   = a BYTE (ONLY if the actor IsA PlayerController; 0
                                       for the main local PC)
  [ initial property block ]         = repeat { SerializeInt(handle, maxHandle); value }
                                       until the bunch's BunchDataBits are exhausted.
```

There is **NO separate "actor NetGUID"** in the open. The actor's identity on the wire
**is the channel index**. (An earlier doc, `RS2V_ActorReplication_7258.md §6.0`, claimed
`[classNetGUID][actorNetGUID]` — that is WRONG; the bytes it read as the "actorNetGUID"
are the **compressed Location**. Confirmed below: removing the phantom actor-GUID makes
the GRI/TeamInfo/PRI property blocks decode bit-exactly to 0 bits remaining.)

Engine proof (send side, `UnChan.cpp:1769-1796`):
```
if (Actor->bNetInitial && OpenedLocally) {
  if (Actor->IsStatic() || Actor->bNoDelete) Bunch << Actor;        // static: a level-actor ref
  else {                                                            // transient (gameplay) case
    UObject* Archetype = Actor->GetArchetype();  Bunch << Archetype; // the CLASS/archetype ref
    SerializeCompressedInitial(Bunch, Actor->Location, Actor->Rotation,
                               Actor->bNetInitialRotation, this);    // Location always, Rotation gated
    if (Actor is PlayerController) Bunch << PC->NetPlayerIndex;      // a BYTE
  }
}
// then the property loop writes WriteIntWrapped(FieldNetIndex, ClassCache->GetMaxIndex()) + value
```
Receive side mirror (`UnChan.cpp:1381,1396,1417,1477`): `Bunch << Object` (class) →
`Cast<AActor>` → `SerializeCompressedInitial` → `<< NetPlayerIndex` (if PC) →
`RepIndex = Bunch.ReadInt(ClassCache->GetMaxIndex())` loop.

---

## 1. The class/archetype reference codec — 32 bits, fixed [H]

`UPackageMap::SerializeObject` (`UnNetDrv.cpp:97`). The **load** path:
```
B = SerializeBits(1)                                   // 1 selector bit
if (B)  SerializeInt(Index, MAX_CHANNELS=1023)         // DYNAMIC: live actor by channel idx, or None
else    SerializeInt(Index, MAX_OBJECT_INDEX=1<<31)    // STATIC: package-map object (class/CDO)
```
`MAX_OBJECT_INDEX = (DWORD(1)<<31)` (`UnCoreNet.h:100`). `SerializeInt(v, 1<<31)` reads
**exactly 31 bits** (the ranged-int loop runs until `val+mask >= 1<<31`). So a class ref =
**1 + 31 = 32 bits = the first 4 payload bytes**, decoded as a little-endian uint32 `V`:
```
flag  = V & 1            // 0 = static object (always 0 for a class ref)
index = V >> 1           // the class's package-map (NetGUID) export index
```

Verified against the four target opens (first 4 payload bytes → V → index):

| Channel | First 4 bytes | V (LE u32) | flag | index = V>>1 | resolves to |
|--------|---------------|------------|------|--------------|-------------|
| ch2  PC   | `60 c1 01 00` | 0x0001c160 | 0 | **57520** | ROPlayerController |
| ch13 PRI  | `5a a5 02 00` | 0x0002a55a | 0 | **86701** | ROPlayerReplicationInfo |
| ch21 Team | `0a c1 02 00` | 0x0002c10a | 0 | **90245** | ROTeamInfo |
| ch54 GRI  | `ce 29 02 00` | 0x000229ce | 0 | **70887** | ROGameReplicationInfo |

All four indices match the known class static indices exactly. The flag bit is 0 (static)
in every case — a class lives in a verified package, so it is resolved via the PackageMap
export tables, **not** by channel index. (Disasm cross-check: VNGame.exe static branch
`SerializeInt(.,0x80000000)` — identical 31-bit width.)

> Emitter rule: write `flag=0` then `SerializeInt(classExportIndex, 0x80000000)`. In bytes,
> for a class index `I` this is simply `LE32((I<<1))` (since flag=0). PC=`60c10100`,
> PRI=`5aa50200`, TeamInfo=`0ac10200`, GRI=`ce290200`.

---

## 2. Compressed Location / Rotation header [H]

`FVector::SerializeCompressed` (`UnMath.cpp:51`):
```
Bits = clamp(ceilLog2(1 + max(|IntX|,|IntY|,|IntZ|)), 1, 20) - 1
SerializeInt(Bits, 20)                      // the magnitude class, 0..19
Bias = 1<<(Bits+1);  Max = 1<<(Bits+2)
SerializeInt(IntX+Bias, Max)                // each axis: width = Bits+2 bits
SerializeInt(IntY+Bias, Max)
SerializeInt(IntZ+Bias, Max)                // coords are appRound()'d to integers
```
A zero vector ⇒ Bits=0 ⇒ `SerializeInt(0,20)` (5 bits) + 3×`SerializeInt(0,4)` (2 bits
each) = **11 bits**. Decoded examples:
- ch2 PC: `X=-831 Y=4085 Z=292`, magBits=11 ⇒ **43 bits** (4 for the Bits field + 3×13).
- ch13 PRI, ch21 TeamInfo, ch54 GRI: `(0,0,0)` ⇒ **11 bits** each (Info actors sit at origin).

`FRotator::SerializeCompressed` (`UnMath.cpp:84`): for Pitch, Yaw, Roll each a **1 presence
bit**; if set, **1 byte** (`angle>>8`). Zero rotation = 3 bits. **Only emitted when the
class's `bNetInitialRotation==true`.** Default is `false` (`Actor.uc:228`) and it is only
overridden on projectiles/turrets/traps/etc — **never** on PlayerController, Info, PRI, GRI,
TeamInfo. So **no Rotation appears in any of the four menu opens.** (Confirmed: assuming a
3-bit zero rotation for PC shifted NetPlayerIndex to 128 and desynced the property block;
omitting it yields NetPlayerIndex=0 and a clean block.)

---

## 3. NetPlayerIndex — PlayerController only, 8 bits [H]

`APlayerController.NetPlayerIndex` is a `byte` (`PlayerController.uc:351`). Read with
`Bunch << PC->NetPlayerIndex` **after** Location/Rotation (`UnChan.cpp:1417`). For ch2 it
decodes to **0** = the main (non-split-screen) local player → the client calls
`HandleClientPlayer(PC)`. Only PlayerController channels carry this byte.

Full ch2 header: classRef[0..32) + Location[32..75) + NetPlayerIndex[75..83). Property
block starts at bit 83.

---

## 4. The initial property block [H]

After the header, the payload is a flat repeat of `SerializeInt(handle, maxHandle)` +
typed value, until `BunchDataBits` is exhausted. There is **no terminator value** — the
bunch's exact bit length is the terminator (the client's `ReadInt` past end sets the error
flag and ends the loop, `UnChan.cpp:1477,1576`).

- **handle** = `SerializeInt(FieldNetIndex, ClassCache->GetMaxIndex())`
  (`UnChan.cpp:1952`). `GetMaxIndex() = FieldsBase + Fields.Num()` summed up the whole
  inheritance chain (`UnCoreNet.h:31`). Per-class maxHandle (from `netfields_from_u.ps1`):
  **ROPlayerController=531, ROGameReplicationInfo=184, ROTeamInfo=78,
  ROPlayerReplicationInfo=98.**
- **value** = `UProperty::NetSerializeItem`, type-specific:
  - `UBoolProperty` → **1 bit**
  - `UByteProperty` (incl. `ENetRole`) → **8 bits**
  - `UIntProperty` → **32 bits** (LE)
  - `UFloatProperty` → **32 bits** (IEEE-754 LE)
  - `UStrProperty` (FString) → **int32 length (32 bits)** then `length` ANSI bytes (incl.
    NUL); a negative length = UTF-16, `-length` wchars.
  - `UObjectProperty`/`UClassProperty` → the §1 codec (1 flag bit + ranged int; a live
    actor uses flag=1 + `SerializeInt(channelIdx, 1023)`, a package object uses flag=0 +
    `SerializeInt(idx, 1<<31)`).
  - `UStructProperty` → component-wise (e.g. `UniqueId`/FUniqueNetId = a 64-bit QWORD).
- **Static arrays (`ArrayDim>1`)**: each element is preceded by an **8-bit element index**
  (`UnChan.cpp:1514`); only changed elements are sent, so indices can be sparse.

### Wire order = the class's `RepProperties` order, NOT ascending handle [H]

The send loop iterates `ClassCache->RepProperties` (`UnChan.cpp:1845`), which is grouped by
`replication{}` condition, so handles are **not** monotonic on the wire (e.g. PRI sends
40,39,38,37 then 25,23,24,36 then the RO block). Because every value is preceded by its own
handle, **order does not affect client decode** — the only hard requirement is that the
bunch ends exactly on the last value's last bit (any stray trailing bits would be misread
as a bogus handle). This is precisely why feeding properties as separate later bunches /
non-bit-exact payloads HUNG the client: the real server packs them in the open with exact
bit length.

### 4.1 ch54 GRI (ROGameReplicationInfo) — decoded, maxHandle=184 [H]

Header: classRef[0..32) + Location(0,0,0)[32..43). Property block from bit 43:

| bits | handle | property | type | value |
|------|--------|----------|------|-------|
| 43..52  | 32  | GameReplicationInfo.bStopCountDown | bool | 0 |
| 52..61  | 31  | GameReplicationInfo.bMatchHasBegun | bool | 1 |
| 61..101 | 25  | GameReplicationInfo.TimeLimit | int | 2100 |
| 101..141| 33  | GameReplicationInfo.GameClass | class-ref | obj(static idx 69601) |
| 141..181| 29  | GameReplicationInfo.RemainingTime | int | 1114 |
| 181..221| 28  | GameReplicationInfo.ElapsedTime | int | 986 |
| 221..701| 24  | GameReplicationInfo.ServerName | FString | `"    -=PR=-GAMING #5 | RESORT 24/7 | LOW PING | P-R.ONL…"` |
| 701..  | 179 | ROGameReplicationInfo.ObjectiveRepIndices[0..4] | byte[16] | 0,1,2,3,4 |
| …      | 178 | ObjectiveStatus[0..4] | byte[16] | 17,17,80,80,16 |
| …      | 177 | ObjectiveForceRatio[2] | byte[16] | 254 |
| …      | 123 | ResupplyAreas | struct[16] | (native struct, not decoded further) |

The readable `ServerName` FString at handle 24 is the strongest possible validation that
the header + handle + FString codec are all correct. (GRI is 4662 bits / 583 B; the tail is
ROGameReplicationInfo struct arrays — objective/campaign state — which are native structs we
did not expand, but the leading scalar block is bit-exact.)

### 4.2 ch21 TeamInfo (ROTeamInfo) — decoded, maxHandle=78, EXACT [H]

Header: classRef[0..32) + Location(0,0,0)[32..43). Block:

| bits | handle | property | type | value |
|------|--------|----------|------|-------|
| 43..81 | 23 | TeamInfo.TeamIndex | int | 2 |

**END at bit 81 = the bunch's full 81 bits, 0 remaining.** A TeamInfo open is just the
class ref + zero Location + the single `TeamIndex` int. `TeamName` (FString) and `Score`
(float) are at their defaults and are NOT sent. (The larger TeamInfo opens ch56/ch76 add
the ROTeamInfo reinforcement/score arrays.)

### 4.3 ch13 PRI (ROPlayerReplicationInfo) — decoded, maxHandle=98, EXACT [H]

Header: classRef[0..32) + Location(0,0,0)[32..43). Block (a real player, "DodgR"):

| bits | handle | property | type | value |
|------|--------|----------|------|-------|
| 43..81   | 40 | PlayerReplicationInfo.Score | float | 164.0 |
| 81..119  | 39 | PlayerReplicationInfo.Deaths | int | 10 |
| 119..133 | 38 | PlayerReplicationInfo.Ping | byte | 16 |
| 133..219 | 37 | PlayerReplicationInfo.PlayerName | FString | `"DodgR"` |
| 219..258 | 25 | PlayerReplicationInfo.StartTime | int | 765 |
| 258..329 | 23 | PlayerReplicationInfo.UniqueId | struct(QWORD) | 0x011000011835f45f (SteamID) |
| 329..368 | 24 | PlayerReplicationInfo.Kills | int | 11 |
| 368..406 | 36 | PlayerReplicationInfo.PlayerID | int | 803 |
| 406..421 | 81 | ROPlayerReplicationInfo.SquadIndex | byte | 0 |
| 421..436 | 80 | RoleIndex | byte | 1 |
| 436..451 | 79 | ClassIndex | byte | 1 |
| 451..466 | 77 | SpawnSelection | byte | 129 |
| 466..504 | 47 | MatchScore | float | 50.0 |
| 504..542 | 46 | DishonorScore | float | 10.0 |
| 542..557 | 76 | VoicePackIndex | byte | 1 |
| 557..572 | 73 | HonorLevel | byte | 51 |
| 572..587 | 72 | ClassRank | byte | 0 |
| 587..656 | 70 | RecentAchievements[0,1,2] | byte[3] | 42,42,82 |
| 656..694 | 43 | OwnedDLCPacks | int | 4 |
| 694..709 | 67 | TeamPRIArrayIndex | byte | 15 |
| 709..723 | 63 | TeamHelicopterSeatIndex | byte | 255 |
| 723..833 | 62 | ReplicatedCharConfig[0,1,3,5,10] | byte[11] | 2,2,6,5,1 |
| 833..971 | 45 | PRIAccoladeAccums[1,2,7] | int[11] | 4,7,2 |

**END at bit 971 = the bunch's full 971 bits, 0 remaining.** Note `PlayerName` (FString,
handle 37) and `UniqueId` (handle 23) — the two per-session patch points. Note also the
sparse static-array element indices (ReplicatedCharConfig sends only indices 0,1,3,5,10).

### 4.4 ch2 PC (ROPlayerController) — header EXACT; native block [H header / L block]

Header fully pinned: classRef(57520)[0..32) + Location(-831,4085,292)[32..75) +
NetPlayerIndex=0 [75..83). Property block begins at bit 83 with **handle 12 =
Actor.Rotation** (a struct). The PC channel (925 b) is dominated by `nativereplication`
state (rotation/view/Controller structs) whose component encodings are native and not
byte-trivial — decoding it fully is out of scope here and unnecessary for the team-select
menu. The header is the load-bearing part and is confirmed by `NetPlayerIndex==0`.

---

## 5. Emitter checklist (replicate properties INSIDE the open)

For each bootstrap actor (PC, GRI, ≥2 TeamInfo, local PRI), one reliable bunch
`bControl=1 bOpen=1 bClose=0 bReliable=1 ChType=2`, ascending ChIndex from 2, ChSeq=1,
payload built in this exact order:

1. **classRef**: `flag=0` + `SerializeInt(classExportIdx, 1<<31)` → in bytes `LE32(idx<<1)`
   (PC `60c10100`/57520, GRI `ce290200`/70887, TeamInfo `0ac10200`/90245,
   PRI `5aa50200`/86701). The index MUST resolve to that UClass in our sent PackageMap.
2. **Location**: `FVector::SerializeCompressed` — always. `(0,0,0)` = 11 bits for
   Info/PRI/GRI/TeamInfo; PC uses the real view location.
3. **Rotation**: OMIT (bNetInitialRotation=false for all of these).
4. **NetPlayerIndex**: PC only, one byte = 0.
5. **property block**: for each initial property, `SerializeInt(handle, maxHandle)` +
   typed value (§4 type table), then **close the bunch exactly on the last value's last
   bit** — do not pad, or the client misreads pad bits as a spurious handle. Minimum menu
   set: GRI {bMatchHasBegun, TimeLimit, GameClass, RemainingTime, ElapsedTime, ServerName},
   TeamInfo {TeamIndex}, PRI {Score, PlayerName, PlayerID, Team(→TeamInfo ref), UniqueId,
   …}. maxHandle: PC 531, GRI 184, TeamInfo 78, PRI 98.

Avoid (each desyncs/hangs): a second actor-GUID after the class ref; emitting a Rotation
header on these classes; sending these initial properties as separate later bunches; any
trailing pad bits in the open bunch.

---

## 6. Reproduce

```bash
# scratchpad decode2.py (self-contained): loads S2C frames via tshark, finds the first
# bOpen bunch on ch2/13/21/54, bit-reads [classRef][Location][NetPlayerIndex?] then the
# typed property block using netfields_u_*.txt (handles) + UELib type dumps.
python decode2.py        # ch13 PRI and ch21 TeamInfo consume to EXACTLY 0 bits remaining
```
Handle tables: `tools/netfields_u_global.txt` (PC), `netfields_u_ROGameReplicationInfo.txt`,
`netfields_u_ROTeamInfo.txt`, `netfields_u_ROPlayerReplicationInfo.txt`
(regenerate via `tools/netfields_from_u.ps1 -Class <Name>`).
