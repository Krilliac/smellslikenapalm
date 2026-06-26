# Root-Cause: TeamInfo replication HUNG the client (team-select black squares)

Ground-truth diff of **what our server did** vs **what the real RS2 server does** on a
`ROTeamInfo` channel, decoded byte/bit-for-bit from
`D:\RE-Tools\rs2_realserver_capture.pcapng` (S2C `udp.srcport==7777 && udp.dstport==57867`,
decoded with `tools/mock_client.py` `bd_max=12000`).

Conclusion up front: our `TeamIndex` **encoding was correct** (handle 23, maxHandle 78,
`int` 32-bit), but we put it in the **wrong kind of bunch in the wrong place**. The real
server carries `TeamIndex` (and `TeamName`) **inside the channel-OPEN bunch** as part of the
`bNetInitial` property block — it is **never** sent as a standalone post-open bunch. Sending
it as a separate reliable `ChSequence=2` bunch creates a reliable-sequence gap that the
client buffers forever (UnChan.cpp:295-331) → the TeamInfo channel stalls → the menu waits
on `GRI.Teams` population → **hang**.

---

## 1. What the REAL server sends on a TeamInfo channel

Three `ROTeamInfo` actors are opened (classRef static index **90245** confirmed on all
three — that is the `ROTeamInfo` class NetGUID). Each is opened with **exactly ONE reliable
bunch**, then every later update is **unreliable**:

| Frame | Chan | bControl | bOpen | bReliable | ChSeq | ChType | bits | role |
|------|------|----------|-------|-----------|-------|--------|------|------|
| f1485 | ch21 | 1 | 1 | 1 | **1** | **2 (CHTYPE_Actor)** | 81  | OPEN (minimal; TeamIndex==default -1) |
| f1489 | ch56 | 1 | 1 | 1 | **1** | 2 | 389 | OPEN (full initial property block) |
| f1494 | ch76 | 1 | 1 | 1 | **1** | 2 | 483 | OPEN (full initial property block) |
| f1503+ | ch56 | 0 | 0 | **0** | 0 | 0 | … | UNRELIABLE update |
| f1505+ | ch76 | 0 | 0 | **0** | 0 | 0 | … | UNRELIABLE update |

Decoded open-bunch header (all three share the prefix):

```
ch21 open (81b):  classRef(static)=90245   actorRef(static)=155296   <open hdr + property block>
ch56 open (389b): classRef(static)=90245   actorRef(static)=25248    <open hdr + property block>
ch76 open (483b): classRef(static)=90245   actorRef(static)=24224    <open hdr + property block>
```

- `classRef` = `SerializeObject` selector bit 0 (STATIC) + `SerializeInt(idx, 1<<31)` → **90245**
  (= `ROTeamInfo`). Confirms `UE3_PropertyReplication.md` §6 polarity (0 = static/package-map).
- `actorRef` = same codec, the actor's NetGUID.
- After the two refs there is a small native open-header block (class+Location+NetPlayerIndex —
  the exact bit-split is still the documented open item, `UE3_PropertyReplication.md` §8) and
  then the **initial property block** (handle→value pairs, no terminator).
- `ch56`/`ch76` payloads contain the `TeamName` FString and the ROTeamInfo array props
  (`TeamPRIArray`, etc.) — i.e. the **full `bNetInitial` set is in the OPEN bunch**.
- `ch21` is the spectator/neutral `ROTeamInfo` whose `TeamIndex` is still the default `-1`,
  so its open carries essentially no properties (17 zero bits after the refs) — UE3 only
  replicates properties that differ from the class-default object.

**Decisive observation:** across the entire post-join window there is **no** reliable
`ChSequence=2` (or any standalone) property bunch on a TeamInfo channel. The first and only
reliable bunch per channel is the OPEN; everything after is `bReliable=0, ChSeq=0`.

---

## 2. The replication rules that force this (from the decompiled script)

`D:\RE-Tools\rs2-source\Engine\TeamInfo.uc`:

```unrealscript
var databinding const localized string TeamName;
var databinding int Size;
var databinding float Score;
var repnotify databinding int TeamIndex;     // <-- int, repnotify
...
replication
{
    if(bNetDirty   && Role==ROLE_Authority)  Score;
    if(bNetInitial && Role==ROLE_Authority)  TeamIndex, TeamName;   // <-- INITIAL ONLY
}
...
defaultproperties { TeamName="Team"  TeamIndex=-1 ... }   // <-- default TeamIndex = -1
```

Load-bearing facts:

1. **`TeamIndex` is `int` (32-bit).** Our `int32` value type was **correct**.
2. **`TeamIndex` replicates under `bNetInitial`** → it is only ever emitted in the **initial
   (channel-open) bunch**. The real server has no code path that sends it later as its own
   bunch. Score is the only `bNetDirty` (periodic) field here.
3. **Default `TeamIndex = -1`** → it is replicated whenever the team's index (0/1) differs
   from -1, i.e. in the open bunch for the two real teams (ch56/ch76), and omitted for the
   default-valued spectator team (ch21).
4. **`TeamIndex` is `repnotify`.** On receipt the client runs:
   `ReplicatedEvent('TeamIndex') → if(WorldInfo.GRI != none) WorldInfo.GRI.SetTeam(TeamIndex, self)`.
   `GameReplicationInfo.SetTeam(Index, TI)` does `if(Index>=0) … Teams[Index]=TI` (dynamic
   array auto-grows; no OOB). This is what actually populates `GRI.Teams[]` so the menu can
   render the team buttons. **The black squares = `GRI.Teams[]` never populated** because the
   `repnotify` never ran with a valid value.

---

## 3. Our (hung) attempt vs the real server — the precise DIFF

Our attempt: a **separate reliable bunch** on ch21/56,
`payload = SerializeInt(23,78) + int32 TeamIndex`, **38 bits**, `ChSequence=2`, `bOpen=0`
(open was seq1).

| Aspect | Real server | Our attempt | Match? |
|---|---|---|---|
| Handle | 23 (maxHandle 78) | 23 (maxHandle 78) | ✅ correct |
| Handle bit-width | 6 bits (`SerializeInt(23,78)` stops at mask 32) | 6 bits | ✅ |
| Value type | `int` 32-bit LE | `int32` | ✅ |
| **Carried in** | the **channel-OPEN bunch** (reliable, seq1, **bOpen=1**), in the initial property block after the SerializeNewActor header | a **separate** bunch, **bOpen=0**, seq2 | ❌ |
| **Reliability of a post-open property bunch** | post-open updates are **UNRELIABLE** (`bReliable=0, ChSeq=0`) | **reliable** seq2 | ❌ |
| `TeamName` (handle 25, FString) | also sent in the same OPEN bunch | not sent | ❌ |
| Sibling initial props (ROTeamInfo arrays, etc.) | sent in the OPEN bunch | not sent | ❌ |

So the value bytes were right; the **bunch type, reliability, sequence, and placement** were
all wrong.

---

## 4. Root-cause hypothesis for the HANG (and the "no effect after menu")

UE3 processes reliable bunches **strictly in order, per channel**. Receiver path
(`D:\RE-Tools\UE3-src\Development\Src\Engine\Src`):

`UNetConnection::ReceivedPacket` (UnConn.cpp:1137):
```cpp
// Ignore if reliable packet has already been processed.
if( Bunch.bReliable && Bunch.ChSequence<=InReliable[Bunch.ChIndex] )
{ debugf("Received outdated bunch"); continue; }     // <-- silently dropped
```

`UChannel::ReceivedRawBunch` (UnChan.cpp:295-331):
```cpp
if( Bunch.bReliable && Bunch.ChSequence != Connection->InReliable[ChIndex]+1 )
{
    checkSlow(!Bunch.bOpen);
    check(Bunch.ChSequence > Connection->InReliable[ChIndex]);   // <-- assert if it regressed
    // queue the bunch, WAITING for the missing predecessor — do NOT process it
    ... insert into InRec (sorted) ...
}
else { ReceivedSequencedBunch(Bunch); /* then drain InRec in order */ }
```

A reliable bunch is only processed when `ChSequence == InReliable[ch]+1`. Otherwise it is
**buffered in `InRec` indefinitely** until the gap fills.

**Primary hypothesis — reliable-sequence gap (stall):**
`TeamIndex` is a `bNetInitial` property, so the real flow is *open(seq1) carries TeamIndex*.
By splitting it into a standalone `seq2` bunch, success requires the client to have **already
received and processed the seq1 OPEN on that exact channel index**, so `InReliable[ch]==1`.
If that precondition is not met — the channel was never opened on that index, the open wasn't
acked/processed yet, or our two bunches' sequence numbers weren't contiguous — then `seq2`
arrives with `2 != InReliable+1`, gets queued in `InRec`, and waits for a `seq1` that never
comes. The TeamInfo channel **never advances**; `repnotify` never fires; `GRI.Teams[]` stays
empty; the team-select UI blocks waiting for team data → the client **appears hung**.
(If the standalone bunch's sequence had actually *regressed* below `InReliable`, the
`check(ChSequence > InReliable)` at UnChan.cpp:303 would **assert** → a hard freeze.)

**Why "after opening the menu it had no effect":** by then the channel's `InReliable` has
already advanced past 2 (the real open + its real initial block were processed), so our
re-sent `ChSequence<=InReliable` bunch hits the **"Received outdated bunch → continue"** path
(UnConn.cpp:1137) and is silently discarded. Hence: *before = stall, after = ignored* — the
exact signature of a reliable-sequence mismatch, not a payload-encoding bug.

**Ruled out:**
- *Encoding*: handle 23 / maxHandle 78 / `int` 32-bit are all correct (§3).
- *repnotify crash*: `SetTeam` guards `Index>=0` and `Teams[Index]=` auto-grows the dynamic
  array — no OOB for TeamIndex 0/1 (§2.4).
- *Wrong value type (int vs byte)*: `TeamIndex` is `int`, so 32-bit was right.

---

## 5. The fix (what to emit instead)

1. **Put `TeamIndex` in the channel-OPEN bunch.** Emit one reliable `bOpen=1, ChType=2
   (CHTYPE_Actor), ChSequence=1` bunch per TeamInfo actor =
   `SerializeNewActor header (classRef=90245 + actorRef + open hdr)` **+ initial property
   block**. The property block, in ascending handle order, must include at minimum:
   - handle **23** `TeamIndex` → `int32` (the team's index, 0 or 1),
   - handle **25** `TeamName` → FString (`INT len incl NUL` + ANSI chars),
   - plus the ROTeamInfo `bNetInitial` array props the real ch56/ch76 opens carry.
   No terminator; set `BunchDataBits` to the exact bit count (`UE3_PropertyReplication.md` §3).
2. **Never send a `bNetInitial` property as a later standalone bunch.** For mid-game team
   changes, replicate as an **UNRELIABLE** bunch (`bReliable=0, ChSeq=0`) on the
   already-open channel (matches the real server's post-open updates; note UnConn.cpp:1144
   discards an unreliable bunch that arrives *before* the channel is open).
3. **Keep channel indices/sequences consistent.** The OPEN must be the first reliable bunch
   (seq1) on that channel index; the client's `InReliable[ch]` must reach 1 from that open
   before any seq2 is meaningful. Do not reuse/duplicate a channel index across actors or
   reset the per-channel sequence.
4. Skip `TeamIndex` for a default (-1) team — UE3 omits default-valued properties (this is why
   real ch21 is nearly empty).

Once `TeamIndex` (and `TeamName`) ride the open bunch, the client's `repnotify` runs
`GRI.SetTeam(TeamIndex, self)`, `GRI.Teams[]` populates, and the team-select buttons render
instead of black squares — with no reliable-sequence gap to stall the connection.

---

## Appendix: sources

- Capture: `D:\RE-Tools\rs2_realserver_capture.pcapng`; decode `tools/mock_client.py`
  (`decode_packet(..., bd_max=12000)`), scratchpad `extract_team.py` / `parse_open.py`.
- `D:\RE-Tools\rs2-source\Engine\TeamInfo.uc` (TeamIndex `int repnotify`, `bNetInitial`
  condition, default -1, ReplicatedEvent→GRI.SetTeam).
- `D:\RE-Tools\rs2-source\Engine\GameReplicationInfo.uc:264` (`SetTeam` → `Teams[Index]=TI`).
- `D:\RE-Tools\UE3-src\Development\Src\Engine\Src\UnConn.cpp:1056-1148` (bunch header parse;
  outdated-bunch drop at :1137; unreliable-before-open drop at :1144).
- `D:\RE-Tools\UE3-src\Development\Src\Engine\Src\UnChan.cpp:292-359` (`ReceivedRawBunch`
  in-order reliable queueing — the stall mechanism), :366-421 (`SendBunch` sequence assign).
- `D:\RE-Tools\UE3-src\Development\Src\Engine\Inc\UnNet.h:59-63` (CHTYPE_Control=1,
  CHTYPE_Actor=2).
- Handle map: `tools/netfields_u_ROTeamInfo.txt` (handle 23=TeamIndex, 24=Score, 25=TeamName,
  maxHandle 78); `UE3_PropertyReplication.md`, `UE3_ClassNetCache_HandleOrder.md`.
