# ROTeamInfo channels (ch21, ch56, ch76) — full property decode (S2C, real server)

Source capture: `D:\RE-Tools\rs2_realserver_capture.pcapng`, Session A
(`udp.srcport==7777 && udp.dstport==57867`, S2C). Decoder: `tools/mock_client.py`
framing (`decode_packet(data, bd_max=12000)` for S2C, MaxPacket=1500).
Handle map + types: `tools/netfields_u_ROTeamInfo.txt` / `docs/re/netfields_all_classes.md`
(**ROTeamInfo maxHandle = 78**, ClassNetCache field count). Net-field TYPES taken from a
UELib pass over the compiled `.u` (UProperty subclass per CPF_Net field).

Confidence: **[H]** = bit-exact from capture, re-derived, and self-consistent (handle+value
consume exactly the right bits, next handle lands on a valid in-range field). Open headers and
the three OPEN property blocks below are [H]. Struct/array-heavy *delta* bunches are only
partially decodable (see §5) and are tagged [M].

---

## 0. TL;DR — what the real server replicates on a TeamInfo

> Three `ROTeamInfo` actor channels are opened (one per team object the GameInfo created):
> **ch21 = TeamIndex 2, ch56 = TeamIndex 1, ch76 = TeamIndex 0.** Each open is a normal UE3
> dynamic-actor open: `archetype ref (0ac10200 = ROTeamInfo class, static idx 90245)` +
> `compressed Location (0,0,0)` + initial replicated-property block. **TeamName (handle 25) is
> NEVER sent** (it equals the class-default empty string, so UE3 omits it; team names are
> client-side from TeamIndex). The single most important fact for the menu:

| ch | open frame | bd (bits) | TeamIndex (h23) | other props at OPEN |
|----|-----------|-----------|-----------------|---------------------|
| **21** | f1485 | **81** | **2** (spectator/neutral) | NONE — just TeamIndex, 0 trailing bits |
| **56** | f1489 | 389 | **1** | Score=1.0, NumPlayers=31, ReinforcementsRemaining=243, SelectedArtyIndex=10, SavedArtilleryCoords(+more, struct/array tail) |
| **76** | f1494 | 483 | **0** | NumPlayers=30, ReinforcementsRemaining=299, SavedArtilleryCoords(+more) |

> **ch21 (the 81-bit, TeamIndex-only open) is the exact minimal, safe TeamInfo the server
> emits** and is the template to replicate without hanging the client. The two *playing* teams
> (ch56/ch76) additionally carry live gameplay fields (player counts, reinforcement pools, map
> arrays) that are NOT needed for the team-select menu.

---

## 1. OPEN-bunch wire layout (source-exact)

UE3 path (`UnChan.cpp:1379-1442` read / `1769-1799` write, see
`docs/UE3_NetGUID_PackageMap.md §3`). A dynamic actor open bunch payload is:

```
[1] archetype object-ref   : SerializeObject  -> selector bit B=0 (static) + SerializeInt(idx, 0x80000000)
[2] compressed Location     : FVector::SerializeCompressed
[3] (compressed Rotation)   : only if bNetInitialRotation  -> ROTeamInfo is an Info, bNetInitialRotation=FALSE => ABSENT
[4] (NetPlayerIndex)         : only if PlayerController     -> ABSENT for TeamInfo
[5] initial property block   : repeat { SerializeInt(handle, 78) ; [Element byte if static array] ; typed value }
```

### 1.1 The archetype ref (bits 0..31) = constant `0a c1 02 00` for every TeamInfo [H]

`SerializeObject`: `B=0` (static branch) then `SerializeInt(idx, 0x80000000)` = 31 fixed bits.
The 32-bit LE word `0a c1 02 00` = `0x0002C10A`; LSB (bit0)=0 is the selector, the upper 31 bits
= `0x0002C10A >> 1` = **90245 = the ROTeamInfo class static PackageMap index** (matches the known
`TeamInfo=90245`). Resolves only if our PackageMap export matches the client's (see
`docs/UE3_NetGUID_PackageMap.md`).

### 1.2 Compressed Location for (0,0,0) = 11 bits (bits 32..42) [H]

`FVector::SerializeCompressed` (`Core/UnMath.cpp:51`):
```
SerializeInt(Bits, 20)       ; Bits = ceilLog2(1+max|component|) clamped 1..20, minus 1.  For (0,0,0): Bits=0  -> 5 bits, all 0
Max  = 1<<(Bits+2) = 4 ; Bias = 1<<(Bits+1) = 2
SerializeInt(X+Bias=2, 4)    ; 2 bits "01"   -> X=0
SerializeInt(Y+Bias=2, 4)    ; 2 bits "01"   -> Y=0
SerializeInt(Z+Bias=2, 4)    ; 2 bits "01"   -> Z=0
```
=> **header is exactly 43 bits** when Location=(0,0,0) and the actor is an Info (no rotation,
no NetPlayerIndex). Verified: ch21 decodes header=43 bits, then one property, then 0 trailing.

### 1.3 Property block: handle + value [H]

Per property (`UnChan.cpp:1477,1513-1557`):
```
handle  = SerializeInt(FieldNetIndex, ClassCache->GetMaxIndex()=78)   ; ranged minimal-bit int
if (prop.ArrayDim != 1):  Element = <one raw BYTE, 8 bits>            ; Bunch << Element  (NOT SerializeInt!)
value   = NetSerializeItem(...)
```
**Important correction to `docs/re/netfields_all_classes.md`:** the static-array element index is a
**raw 8-bit byte** (`Bunch << Element`, `UnChan.cpp:1516`), *not* `SerializeInt(elem, ArrayDim)`.

Value encodings used here: `int`=32b LE, `float`=32b IEEE, `byte`=8b, `bool`=1b,
`FString`=int32 length (chars incl. NUL; negative=UTF-16) + bytes, `obj ref`=SerializeObject
(§1.1; dynamic actor values become `B=1 + SerializeInt(channelIndex, 2048)`).
`struct Vector`/`Vector2D` = compressed (variable width) — these stop our simple parser.

### 1.4 maxHandle = 78, ROTeamInfo handle map (relevant subset)

| handle | name | type | replication condition |
|--------|------|------|-----------------------|
| 23 | TeamIndex | int (32b) | **initial** |
| 24 | Score | float (32b) | dirty |
| 25 | TeamName | FString | initial — **but == default empty, so NEVER on the wire** |
| 42 | SavedArtilleryCoords | struct Vector | dirty |
| 62 | ReinforcementsRemaining | int | dirty |
| 65 | SelectedArtyIndex | byte | dirty |
| 77 | NumPlayers | byte | dirty |
| 55 | TeamLocationArray[32] | struct Vector[] | dirty (dominant in deltas) |
| 54 | EnemyLocationArray[32] | struct Vector[] | dirty |
| 56 | TeamPRIArray[32] | obj<ROPlayerReplicationInfo>[] | dirty |
| 47 | SpawnTunnels[10] | obj<ROPlaceableSpawn>[] | dirty |

Full table: `docs/re/netfields_all_classes.md` (ROTeamInfo section, handles 0-77).

> **Handle order on the wire is NOT ascending.** ch56 sends 24,23,77,62,65,42… because UE3
> sends in **RepIndex (ClassReps) order**, while each property is *labelled* with its
> **FieldNetIndex** handle. The client maps handle->property regardless of order, so for our
> emulator the send order is free — only the handle label must be correct.

---

## 2. CH21 — TeamIndex 2 (the minimal, safe template) [H]

```
f1485  ch21  bOpen bReliable  ChType2  sq1   bd=81
payload (11 bytes): 0a c1 02 00 40 bd 04 00 00 00 00
```
Bit-exact breakdown (81 bits, 0 padding):
```
bits  0..31  0a c1 02 00   archetype objref: B=0 + SerializeInt(90245,0x80000000)  -> ROTeamInfo class
bits 32..36  (5b) 00000    FVector::SerializeCompressed Bits=0
bits 37..38  (2b) 01       X+2=2 -> X=0
bits 39..40  (2b) 01       Y+2=2 -> Y=0
bits 41..42  (2b) 01       Z+2=2 -> Z=0       (Location = 0,0,0; header = 43 bits)
bits 43..48  (6b)          SerializeInt(handle=23, 78)  -> TeamIndex
bits 49..80  (32b)         int32 = 2
```
**Only one property (TeamIndex=2). No Score, no NumPlayers, no arrays. 0 trailing bits.**
ch21 has **exactly one bunch in the whole capture** (the open) — no deltas ever — confirming
TeamIndex 2 is an inert spectator/neutral pseudo-team. This is the cleanest possible TeamInfo
open and the recommended bootstrap template.

---

## 3. CH56 — TeamIndex 1 (a playing team) [H for listed props]

```
f1489  ch56  bOpen bReliable ChType2  sq1  bd=389
hex: 0ac1020040c50000007fae00000080e6873f0f00001054507959d3e5cd3f7ca10c38c319f08634e0116fc0394e80777400
```
Decoded initial block (after the 43-bit header):
```
@bit43   h=24  Score                    float  1.0
@bit81   h=23  TeamIndex                int    1
@bit119  h=77  NumPlayers               byte   31
@bit134  h=62  ReinforcementsRemaining  int    243
@bit172  h=65  SelectedArtyIndex        byte   10
@bit187  h=42  SavedArtilleryCoords     struct Vector  (compressed; parser stops here)
... 196 trailing bits = the compressed SavedArtilleryCoords vector + further dirty fields
    (StrikeDirection / map arrays); not menu-relevant, not bit-pinned past h=42. [M]
```

---

## 4. CH76 — TeamIndex 0 (a playing team) [H for listed props]

```
f1494  ch76  bOpen bReliable ChType2  sq1  bd=483
hex: 0ac1020040bd000000009a1efe4a0000803a837b41702f08ee05efc015de822ce00183c01142802f1402771801feb0011c620df8450af08c12e01a2d00
```
Decoded initial block (after the 43-bit header):
```
@bit43   h=23  TeamIndex                int    0
@bit81   h=77  NumPlayers               byte   30
@bit96   h=62  ReinforcementsRemaining  int    299
@bit134  h=42  SavedArtilleryCoords     struct Vector  (compressed; parser stops)
... 343 trailing bits = compressed vector + more dirty gameplay fields. [M]
```

So the two *playing* teams are TeamIndex **0** (ch76, 30 players, 299 reinforcements) and
TeamIndex **1** (ch56, 31 players, 243 reinforcements, Score 1.0); TeamIndex **2** (ch21) is the
empty spectator team. (Server is a near-full 64-player public server, hence ~30/team.)

---

## 5. Delta (post-open) traffic on ch56 / ch76 — NOT needed for the menu [M]

After the open, ch56 & ch76 receive a continuous stream of **unreliable ChType0** property
deltas (ch21 gets none). Per-tick dominant fields:

- **h=55 TeamLocationArray[32]** (struct Vector[]) — the live friendly-position map blips; by
  far the most frequent (most deltas lead with it).
- **h=62 ReinforcementsRemaining** — ticks DOWN once per ~second: observed 243→242→241→240→239
  →238→237→236 on ch56 (the supremacy/territory reinforcement counter).
- **h=54 EnemyLocationArray[32]**, **h=42 SavedArtilleryCoords**, **h=56 TeamPRIArray[32]**
  (obj refs = dynamic channel indices), **h=47 SpawnTunnels[10]**, **h=48
  TeamHelicopterPilotNames[10]** (FString[]), **h=77 NumPlayers**.

These array/struct-heavy bunches do not fully bit-decode with the simple parser (compressed
vectors + per-element byte indices misalign a naive reader); they are gameplay/HUD state, not
team-select state. Object-property values resolve as **dynamic refs** (`B=1 +
SerializeInt(chIndex,2048)`), corroborating the object-ref codec polarity in
`docs/UE3_NetGUID_PackageMap.md`.

---

## 6. Why replicating team state HUNG the client, and how to do it safely

**The hang is a bit-desync, not a logic bug.** If the client reads a property handle or value
with the wrong bit width, every subsequent handle is garbage; the `while(FieldCache)` receive
loop (`UnChan.cpp:1485-1578`) keeps consuming "handles" until the bunch's `BunchDataBits` run
out — and if a bogus handle resolves to a function or a wrong-typed property it can spin /
mis-spawn / `NMT_ActorChannelFailure`. Likely culprits in the earlier attempt:

1. **Wrong handle bound.** The handle is `SerializeInt(h, 78)` — must use ROTeamInfo's
   ClassCache field count **78**, not 77, not a power of two. A wrong max changes the bit width
   of *every* handle.
2. **Sending TeamName / a struct / an array.** TeamName must be **omitted** (the real server
   never sends it). Any `struct Vector` (SavedArtilleryCoords, *LocationArray) requires the exact
   `FVector::SerializeCompressed` width; a raw 3×float (96b) instead of the compressed form
   desyncs immediately.
3. **Static-array element index width.** It is a **raw byte (8 bits)**, not
   `SerializeInt(elem,N)` — getting this wrong corrupts the rest of the bunch.
4. **BunchDataBits / padding.** Emit the exact bit count; pad the final partial byte with zero
   bits only (the terminator is the packet's last set bit). ch21 has **0 trailing bits**.

**Safe recipe (verified bit-exact = ch21):** open each TeamInfo as
`bControl=1,bOpen=1,bReliable=1,ChType=2`, ascending ChIndex, payload =
```
0a c1 02 00                              ; archetype ref (ROTeamInfo class, idx 90245) — needs matching PackageMap
+ 11-bit compressed (0,0,0) Location     ; 00000 01 01 01
+ SerializeInt(23, 78)                    ; TeamIndex handle  (6 bits for h=23)
+ int32 TeamIndex value                   ; 0, 1, 2 per team
```
i.e. literally the **81-bit `0ac1020040bd0400000000` form with the TeamIndex int patched**
(bits 49..80). Open **three** teams: TeamIndex 0, 1, and 2 (spectator). Do NOT add Score /
NumPlayers / arrays for the menu — they are optional gameplay state and every extra field is a
new desync risk. The team buttons populate from the resolved TeamInfo actors (the menu rebuilds
`GRI.Teams[]` from the individually-replicated ROTeamInfo channels; there is no replicated
`GRI.Teams` array — see `netfields_all_classes.md`).

If Score is wanted later: handle 24, then a 32-bit float, placed before/after TeamIndex (order
free). Keep everything 32-bit/8-bit/1-bit; avoid structs and arrays until their compressed
widths are reproduced exactly.

---

## Appendix — reproduce

```bash
TSHARK="C:\Program Files\Wireshark\tshark.exe"
PCAP="D:\RE-Tools\rs2_realserver_capture.pcapng"
"$TSHARK" -r "$PCAP" -Y "udp.srcport==7777 && udp.dstport==57867" -T fields \
  -e frame.number -e frame.time_relative -e data.data > s2c_cache.tsv
# then decode_full(payload) at bd_max=12000; for a TeamInfo open bunch:
#   robj()  -> ('static',90245)         # 1 bit + SerializeInt(.,0x80000000) = 0ac10200
#   FVector::SerializeCompressed        # 11 bits for (0,0,0)
#   loop: SerializeInt(handle,78) + typed value
# Opens: ch21 f1485 (81b, TeamIndex=2) ; ch56 f1489 (389b, TeamIndex=1) ; ch76 f1494 (483b, TeamIndex=0)
```
Scratch decoder used: self-contained `solo.py` (codec inline; FRAME_MAX cap), built from
`tools/mock_client.py` framing. Class handle/type table from `netfields_from_u.ps1 -Class
ROTeamInfo` + a UELib UProperty-subclass pass.
