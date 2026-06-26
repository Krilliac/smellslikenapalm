# SAFE State-Replication Plan — populate the team buttons WITHOUT hanging the client

Goal: make the team-select menu's team buttons render (not black squares) and make a
team pick stick, on the real retail client, **without re-triggering the hang**. Grounded
in `docs/re/hang_rootcause_teaminfo.md`, `open_bunch_structure.md`,
`teaminfo_channels_properties.md`, `pri_channels_properties.md`, and
`MASTER_replication_reference.md`.

---

## 1. Why it hung (the one fact this whole plan is built on)

`TeamIndex` (and `TeamName`) are **`bNetInitial` properties**: UE3 only ever sends them
inside the channel's reliable **OPEN bunch (ChSequence=1, bOpen=1)**, as part of the
initial property block. The hung attempt instead sent `TeamIndex` as a SEPARATE reliable
bunch with `ChSequence=2, bOpen=0`. The value encoding (handle 23, maxHandle 78, int32)
was correct — the **bunch placement was fatal**:

- `UChannel::ReceivedRawBunch` (UnChan.cpp:295–331) processes reliable bunches strictly
  in order. A `seq2` arriving with no preceding `seq1` on that channel is buffered in
  `InRec` **forever** → channel stalls → repnotify never fires → `GRI.Teams[]` never
  populates → black squares + apparent hang.
- If the sequence ever regressed below `InReliable`, the `check()` at UnChan.cpp:303
  is a hard assert = freeze. "Stall before menu, silently ignored after menu" is the
  signature of a reliable-sequence mismatch, not an encoding bug.

**Conclusion: state must ride the OPEN bunch's bNetInitial block. Do NOT send state as
separate later reliable bunches.** Any later UPDATE must be UNRELIABLE (bReliable=0,
ChSeq=0) on the already-open channel.

---

## 2. Answer to the design question

**YES** — properties must go INSIDE the open bunch's initial property block, which means
the fix is to **rebuild `data/actor_bootstrap.bin`** so each actor's open bunch carries
the needed bNetInitial values, and to extend the value block exactly (bit-exact bunch
length). Do NOT add new reliable post-open bunches. The current bootstrap replays real
captured open bunches verbatim; we either (a) regenerate from the capture (already
correct for the real-server data) or (b) author our own open bunches via
`ActorRepl::WriteActorOpenHeader` + `WriteProp*` with the exact handle/value set below.

---

## 3. Minimal property set (what each open bunch MUST carry)

| Actor (channel) | handle | property | type | value | why |
|---|---|---|---|---|---|
| GRI ch54 | 24 | ServerName | FString | server name | menu header |
| GRI ch54 | 31 | bMatchHasBegun | bool | 1 | gates menu/pre-game |
| GRI ch54 | 182 | MaxPlayers | byte | e.g. 64 | menu slot count |
| TeamInfo ch76 | 23 | TeamIndex | int32 | **0** | team-0 button identity |
| TeamInfo ch56 | 23 | TeamIndex | int32 | **1** | team-1 button identity |
| TeamInfo ch21 | 23 | TeamIndex | int32 | **2** | spectator/neutral (optional) |
| local PRI ch26 | 37 | PlayerName | FString | player name | shows the player in menu |
| local PRI ch26 | 35 | Team | obj→TeamInfo | dynamic ref to ch56/ch76 (AFTER pick) | binds player to chosen team |

Do **NOT** send: `TeamName`(25 — real server never does), GRI Teams[] (no such net
prop), TeamInfo structs/arrays (Score/NumPlayers/ReinforcementsRemaining/
SavedArtilleryCoords/TeamLocationArray) for the menu. They are not needed to render
buttons and every extra field is another chance to mis-size a bit and desync.

---

## 4. Exact encodings / byte layouts (copy-paste ready)

All ranged ints are LSB-first; static-array elem index is a raw 8-bit byte (not used below).

### 4.1 TeamInfo OPEN bunch — the verified minimal-safe template (ch21 form)
Flags: bControl=1 bOpen=1 bClose=0 bReliable=1 ChType=2 ChSeq=1.
```
[ classRef ]  LE32 = 90245<<1 = 0x0002C10A  -> bytes 0a c1 02 00         (32 bits)
[ Location ]  zero vector: SerializeInt(0,20)=5b '00000' + 3×SerializeInt(2,4)=2b '01'   (11 bits)
[ handle   ]  SerializeInt(23, 78)                                        (6 bits)
[ TeamIndex]  int32 LE, value 0/1/2                                       (32 bits)
[ end exactly here — bunchDataBits = 32+11+6+32 = 81 ]
```
Real ch21 payload (TeamIndex=2): `0a c1 02 00 40 bd 04 00 00 00 00`, bd=81. For
TeamIndex=0 the int32 value bytes are `00 00 00 00`; for 1, `01 00 00 00`. Emit ALL THREE
TeamInfo channels in this exact 81-bit form for the menu.

### 4.2 GRI ch54 — extend its open block with the 3 menu props
Open header = classRef `ce 29 02 00` (32b) + zero Location (11b). Then append, each as
`SerializeInt(handle,184)` (8 bits, since ceil(log2(184))=8) + value:
```
ServerName     : h24  + FString(int32 len incl NUL + ANSI bytes)
bMatchHasBegun : h31  + 1 bit (=1)
MaxPlayers     : h182 + byte (8 bits)
```
Bunch must end exactly after MaxPlayers' last bit. (GRI's real open also carries
TimeLimit/RemainingTime/GameClass/MOTD; you may keep replaying the captured bunch, which
already has them. If authoring fresh, the 3 above are the minimum.)

### 4.3 local PRI ch26 — PlayerName in open; Team as UNRELIABLE delta after pick
Open block (handle via `SerializeInt(h,98)`, 7 bits):
```
PlayerName : h37 + FString(int32 len incl NUL + ANSI)
```
After the player picks a team, send `Team` as an **UNRELIABLE** bunch (bReliable=0,
ChSeq=0, bOpen=0) on ch26:
```
h35 (SerializeInt(35,98), 7b) + selector bit '1' + SerializeInt(teamChannel,1024)
   teamChannel = 76 (team 0) or 56 (team 1) — must be an ALREADY-OPEN TeamInfo channel
```
If `Team` is sent before its TeamInfo channel is open, or as static/None, the ref is
unresolved → black square. Order matters (§5).

---

## 5. Order of operations (single tick / bootstrap order)

1. Open **ch2** (ROPlayerController, NetPlayerIndex=0) FIRST, standalone packet (already
   done — client adoption gate).
2. Send NMT 0x24 (`24 01 00 00 00`) — already done.
3. Open the **3 TeamInfo channels** (ch76=team0, ch56=team1, ch21=team2) each with the
   §4.1 81-bit open bunch, BEFORE any PRI.Team ref can point at them.
4. Open **GRI ch54** with §4.2 props (ServerName/bMatchHasBegun/MaxPlayers).
5. Open **PRI channels** (incl. local ch26) with §4.3 PlayerName in the open block.
   Do NOT put Team in the open block (it's a non-initial delta).
6. Send **ClientShowTeamSelect (h206)** then **ClientGotoState (h41)** on ch2 (reliable,
   normal chSeq progression) to open the menu shell.
7. On team pick: send PRI.Team (§4.3) as an UNRELIABLE delta. Optionally ChangedTeams(172)
   / ChangedRole(210) on ch2 as the post-pick confirmation.

Invariant: **one open = one reliable seq1 bunch per channel index; never reuse a channel
index, never reset per-channel sequence, never send a state property as seq2+.**

---

## 6. Test plan — one property first, with hang detection

### 6.1 Smoke test (prove the path, no menu)
Build/replay ONLY the 3 TeamInfo opens in the §4.1 form (TeamIndex 0/1/2) + ch2 + GRI +
local PRI. This is exactly what the real server's menu phase carries. Expect: menu opens,
buttons show team identities, no hang.

### 6.2 Single-property bisection
Start from the known-good captured bootstrap. Add **ONE** field at a time, rebuild
`data/actor_bootstrap.bin`, reconnect:
1. First: GRI ServerName (h24) only → confirm server name renders.
2. Then: TeamInfo TeamIndex (already in template) → confirm buttons.
3. Then: GRI bMatchHasBegun (h31), MaxPlayers (h182).
4. Then: PRI PlayerName (h37).
5. Last: PRI.Team delta (unreliable) after a pick.
Never add two unknowns at once — a desync after step N localizes the bug to field N.

### 6.3 Hang detection via wire tracing / invariants (Harden fleet)
The Harden fleet added invariants/tracing to BitWriter, BitReader, PacketCodec,
ActorReplication, WireTrace (`src/Network/WireTrace.h`). Use them as the hang oracle —
do NOT eyeball the client:
- **Bit-exactness gate (most important):** after writing each open bunch, assert
  `BitWriter.bitcount == expected bunchDataBits` and re-decode it with BitReader and
  assert it consumes to **exactly 0 bits remaining** (the same property the capture
  decodes satisfy: 81/81, 971/971…). A nonzero remainder = the exact failure that hangs
  the client. Reject the bunch before it goes on the wire.
- **WireTrace each S2C bunch** (chIndex, bOpen, bReliable, chSeq, bunchDataBits) and
  assert reliable chSeq is strictly monotonic with NO gaps per channel, and that NO
  channel ever emits a reliable bunch with chSeq>1 carrying a bNetInitial property.
- **Hang signature to watch on C2S:** the real client acks every server packet near-
  instantly. If after sending the team bunches the client STOPS acking (no C2S pure-ack)
  and stops its ch2 menu RPC traffic → reliable-sequence stall (the hang). If it sends
  empty `bClose` bunches on the new channels → ActorChannelFailure (bad class ref /
  unresolved object ref), not a sequence bug. Decode C2S with `bd_max=16384`.
- Compare your generated bunch byte-for-byte against the capture's equivalent bunch
  (gen_actor_bootstrap already pulls them) — any diff in bunchDataBits is the bug.

---

## 7. Executive summary

The black squares and the prior hang have the **same root**: the client only accepts
team state that rides each actor's **reliable OPEN bunch (seq1) bNetInitial block**, ends
on an exact bit boundary, and references already-open channels. The hang was caused by
sending `TeamIndex` as a standalone `seq2` reliable bunch, which UE3 buffers forever.
The fix is to carry the minimal state INSIDE the open bunches: 3 TeamInfo opens each
carrying just `TeamIndex` (0/1/2) in the verified 81-bit form, GRI carrying
ServerName/bMatchHasBegun/MaxPlayers, local PRI carrying PlayerName, and `PRI.Team` sent
later as an UNRELIABLE delta (selector=1 + channel index of an open TeamInfo) only after
the pick. Validate every bunch with a re-decode-to-exactly-0-bits assertion (WireTrace +
BitReader) before it hits the wire; bisect one field at a time.

**SINGLE MOST IMPORTANT NEXT ACTION:** Rebuild `data/actor_bootstrap.bin` so the three
ROTeamInfo channels (ch76/ch56/ch21) each open with the verified 81-bit bunch carrying
ONLY `TeamIndex` (= 0 / 1 / 2; handle `SerializeInt(23,78)` + int32), assert each
re-decodes to exactly 0 bits remaining via the new BitReader/WireTrace invariants, then
reconnect the retail client and confirm the team buttons populate with no hang. That one
change is the smallest step that should turn the black squares into real team buttons.
