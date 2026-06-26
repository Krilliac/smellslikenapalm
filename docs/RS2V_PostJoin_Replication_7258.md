# RS2V Post-Join World / Actor Replication Spec (EngineVersion 7258)

Reverse-engineered from the real client<->server loopback capture
`D:\RE-Tools\rs2_handshake_capture.pcapng`, decoded byte-exact with the validated
UE3 FBitReader framing at **MaxPacket = 2048** (`BunchDataBits = ReadInt(16384)`).

Decoder: `tools/mock_client.py` framing, re-run with the 2048 bunch-width fix
(the mock's `bd = r.rint(64)` is wrong for this phase — it must be `r.rint(16384)`).
Scratch decoder used here: `decode2048.py` (LSB-first, terminator = high set bit of
last byte, per-entry bIsAck/bunch parse exactly as documented in MEMORY).

This document covers the S2C stream **from NMT_Welcome (f162) onward** — the part
that takes a real client off the loading screen and into the map.

---

## 0. TL;DR — the post-Join sequence the server drives

```
  (control handshake already done: StatelessConnect -> Steam login)
1. f157  S2C ch0  NMT_Challenge/Netspeed-class   (40b)
2. f159/160 S2C ch0  large ch0 reliable + bulk ACK of client's pkts
3. f162  S2C ch0  NMT_Welcome  (Map + GameClass + options)   <-- already known
4. f165  S2C ch0  NMT  (post-welcome control, carries "02547C58"-style token)
5. f167..f185  S2C ch0  *** PackageMap / NetGUID export burst ***  (NMT byte 0x07)
       ~19 huge reliable control bunches, ~10000 bits each, listing EVERY
       package the map depends on (Core, Engine, ROGame, WW_*, ENV_VN_*, CHR_*,
       WP_VN_*, FX_VN_*, ROGameContent, the .roe entry, etc) + each package GUID.
       Client mirrors this upstream (C2S f193..f200, same independent-bunch format).
6. client finishes verifying packages -> C2S f227 ch0 NMT 0x09 (ready / join-ack)
7. f231  S2C  *** ACTOR CHANNEL OPEN BURST ***  (the actual world bootstrap)
       Opens ch2..ch8 (ChType 2 = actor channel), each bOpen=1 bReliable=1:
         ch2 (922b)  GameReplicationInfo / ROGameReplicationInfo
         ch3 (940b)  TeamInfo  (carries team names "BRAVO","FOXTROT")
         ch4 (81b)   TeamInfo / PRI  (initial-only)
         ch5 (81b)   TeamInfo / PRI  (initial-only)
         ch6 (1993b) the client's PlayerController  (largest initial state)
         ch7 (69b)   small actor (PlayerReplicationInfo or GameInfo proxy)
         ch8 (286b)  actor (PRI / voice owner)
       + ch0 NMT 0x24 (control) + an unreliable ch2 update (first=0x17) in same pkt.
8. f232.. steady-state property deltas on those channels + periodic ch0 control
       (NMT 0x23 keepalive/heartbeat) and new channel opens as more actors relevant.
```

The client transitions off the loading screen once it has (a) NMT_Welcome,
(b) the full PackageMap (it will not spawn until package GUIDs are reconciled),
and (c) at least the GameReplicationInfo actor channel + its own PlayerController
channel opened and replicated.

---

## 1. Ordered S2C bunch table (from f162 / NMT_Welcome onward)

`ct` = ChType (1 = control channel, 2 = actor channel, 0 = subsequent/continued
actor bunch on an already-open channel). Flags `O/Cl/R` = bOpen/bClose/bReliable.
`sq` = reliable ChSequence (connection-global). All decodes are `trail=0` (clean)
unless noted.

| Frame | PacketId | ChIndex | ct | O/Cl/R | sq | Bits  | First byte | Decode |
|-------|----------|---------|----|--------|----|-------|-----------|--------|
| f157  | 0  | 0 | 1 | 0/0/1 | 1  | 40   | 0x1e | ch0 control (Challenge/Netspeed-class, see §3) |
| f159  | 1  | 0 | 1 | 0/0/1 | 2  | 8    | 0x20 | ch0 control + bulk ACK of client pkts 0..131 |
| f160  | 2  | - | - | -     | -  | -    | -    | pure ACK packet (client pkts 0..132) |
| **f162** | 3 | 0 | 1 | 0/0/1 | 3  | 80   | (NMT_Welcome) | **NMT_Welcome**: Map URL + GameClass + opt (known) |
| f165  | 4  | 0 | 1 | 0/0/1 | 4  | 144  | 0x03 | ch0 control; carries an 8-hex token ("02547C58") |
| f167  | 5  | 0 | 1 | 0/0/1 | 5  | 10032| 0x07 | **PackageMap export #1**: Core, Engine, ROGame, GameFramework, WW_Global, OnlineSubsystemSteamworks, Grip, ENV_VN_*, VN_UI_*, ROGameMenus, UI_Mats ... |
| f168  | 6  | 0 | 1 | 0/0/1 | 6  | 9584 | 0x07 | PackageMap export #2 (VN_UI_*, WW_MUS, WW_WEP_Shared, FX_VN_*, WP_VN_3rd_Master, CHR_VN_US_*) |
| f169  | 7  | 0 | 1 | 0/0/1 | 7  | 9584 | 0x07 | PackageMap export #3 (FX_RS_*, CHR_VN_*, WP_VN_*, CHR_Playeranim_Master, WW_FOL_US, WW_WEP_Bullets ...) |
| f170..f182 | 8..20 | 0 | 1 | 0/0/1 | 8..20 | ~9600-10100 | 0x07 | PackageMap export #4..#16 (continues) |
| f183  | 21 | 0 | 1 | 0/0/1 | 21 | 9616 | 0x07 | PackageMap export (FX_PostProcess, EngineSounds, GFxUI, AkAudio, ROEntry/.roe, ENV_VN_* ...) |
| f183  | 21 | 0 | 1 | 0/0/1 | 22 | 504  | 0x07 | PackageMap tail ("M_VN_Sky") — second bunch in same packet |
| f184  | 22 | 0 | 1 | 0/0/1 | 23 | 9896 | 0x07 | PackageMap export (ENV_VN_Compound_*, ENV_VN_PalmTrees, ENV_VN_Rice, T_VN_*, FX_VN_Animals ...) |
| f185  | 23 | 0 | 1 | 0/0/1 | 24 | 5736 | 0x07 | PackageMap export tail: last 8 packages (ENV_VN_Posters, ENV_VN_Trenches, M_VN_Atmospherics, ROGameContent, WP_VN_USA_M2_HMG, WW_WEP_M2_Browning, WP_VN_VC_DshK_HMG, WW_WEP_M1_Garand) **+ 153-byte control trailer** (Steam Workshop config, ROEntry, ROGame.ROGameInfo, level GUID). Ordinary reliable bunch; the "anomaly" was a decoder bound bug (used 16384 not MaxPacket*8≈12000) — see §4.3. |
| f186  | 24 | - | - | -     | -  | -    | -    | pure ACK |
| ...   | .. | 25..50 | | | | | | mostly pure-ACK packets while client uploads its PackageMap (C2S f193-f200) and verifies |
| **f231** | 51 | 2 | 2 | 1/0/1 | 1 | 922 | 0x60 | **OPEN actor ch2** = GameReplicationInfo/ROGameReplicationInfo |
| f231  | 51 | 0 | 1 | 0/0/1 | 25 | 40  | 0x24 | ch0 control (NMT 0x24) interleaved |
| f231  | 51 | 3 | 2 | 1/0/1 | 1  | 940 | 0x0a | **OPEN actor ch3** = TeamInfo (strings "BRAVO","FOXTROT") |
| f231  | 51 | 4 | 2 | 1/0/1 | 1  | 81  | 0x0a | **OPEN actor ch4** = TeamInfo/PRI (initial-only, twin of ch5) |
| f231  | 51 | 5 | 2 | 1/0/1 | 1  | 81  | 0x0a | **OPEN actor ch5** = TeamInfo/PRI (initial-only, twin of ch4) |
| f231  | 51 | 6 | 2 | 1/0/1 | 1  | 1993| 0xce | **OPEN actor ch6** = local PlayerController (largest initial state) |
| f231  | 51 | 7 | 2 | 1/0/1 | 1  | 69  | 0x50 | **OPEN actor ch7** = small actor (PRI or GameInfo proxy) |
| f231  | 51 | 8 | 2 | 1/0/1 | 1  | 286 | 0x5a | **OPEN actor ch8** = actor (PRI / voice owner) |
| f231  | 51 | 2 | 0 | 0/0/0 | 0  | 20  | 0x17 | unreliable property delta on ch2 (already open) |
| f232  | 52 | 2 | 0 | 0/0/0 | 0  | 20  | 0x17 | ch2 unreliable update |
| f234  | 53 | 2 | 2 | 0/0/1 | 2  | 22  | 0x29 | ch2 reliable property update |
| f235  | 54 | 2 | 0 | 0/0/0 | 0  | 20  | 0x17 | ch2 unreliable update |
| f249  | 55 | 3 | 0 | 0/0/0 | 0  | 25  | 0x38 | ch3 unreliable update |
| f252  | 56 | 0 | 1 | 0/0/1 | 26 | 232 | 0x23 | ch0 control (NMT 0x23 — periodic heartbeat/keepalive) |
| f266  | 57 | 0 | 1 | 0/0/1 | 27 | 232 | 0x23 | ch0 control heartbeat |
| f279  | 58 | 0 | 1 | 0/0/1 | 28 | 424 | 0x23 | ch0 control + ch8 unreliable update (0xe3) |
| f297+ | 60+ | 0/2/3/8 | mixed | | | | | steady-state: ch0 NMT 0x23 heartbeats + periodic actor property deltas |

All 20 export frames (f167-f185) decode with `trail=0` once `BunchDataBits` uses the
correct `SerializeInt(MaxPacket*8≈12000)` bound (the old decoder hardcoded 16384,
which over-read f185 by one bit — see §4.3). There are NO partial bunches. Every
bunch yields clean package-name ASCII, confirming the model.

---

## 2. Actor channels — which open, and what UE3 class each is

ChType 2 (`CHTYPE_Actor` in UE3 `EChannelType`) opens carry a `SerializeNewActor`
header: a compressed NetGUID for the actor, and (because bNetInitial) the
archetype/class NetGUID, then the initial property block. The leading bytes of each
f231 open bunch:

```
ch2  60 c1 01 00 6d 3d 93 b8 e4 1a   922b
ch3  0a c1 02 00 40 bd 00 00 00 00   940b   -> "BRAVO","FOXTROT"
ch4  0a c1 02 00 40 bd 02 00 00 00    81b
ch5  0a c1 02 00 40 bd 04 00 00 00    81b
ch6  ce 29 02 00 40 0d 91 60 0e 00  1993b
ch7  50 cf 08 00 18 00 40 80 18       69b
ch8  5a a5 02 00 40 2d 0d 00 00 00   286b
```

Notes:
- ch4/ch5 are byte-for-byte identical except one index byte (02 vs 04) and are
  tiny (81b) — these are the two **TeamInfo** actors (the two playable teams /
  squads). ch3 (940b) carries the team display strings "BRAVO"/"FOXTROT" so it is
  the first/primary TeamInfo (or the squad-name carrying ROTeamInfo).
- ch2 (922b) is the first and largest "info" actor opened — the
  **GameReplicationInfo / ROGameReplicationInfo** (cross-ref
  `D:\RE-Tools\rs2-source\ROGame\ROGameReplicationInfo.uc` extends
  `Engine\GameReplicationInfo.uc`; its `bNetInitial` block replicates
  ServerName, GameClass, GoalScore, RemainingTime, ElapsedTime — exactly the
  kind of one-shot initial payload seen here).
- ch6 (1993b) is by far the largest initial state and is the client's
  **PlayerController** (`ROGame\ROPlayerController.uc` extends
  `GamePlayerController`). UE3 always opens the owning client's PlayerController
  channel right after the GRI; its initial bunch is large (it owns lots of
  config/replicated state).
- ch7 / ch8 (69b / 286b) are **PlayerReplicationInfo** actors
  (`Engine\PlayerReplicationInfo.uc` / `ROGame\ROPlayerReplicationInfo.uc`) — one
  is the local player's PRI (owned by the PC on ch6), the other a bot/other PRI.

### Bootstrap actor identification (confidence)
| Channel | UE3 class (rs2-source) | Confidence | Evidence |
|---------|------------------------|-----------|----------|
| ch2 | ROGameReplicationInfo (extends GameReplicationInfo) | HIGH | opened first, large initial-only block matching GRI repl layout |
| ch3 | TeamInfo / ROTeamInfo | HIGH | "BRAVO","FOXTROT" team strings |
| ch4,ch5 | TeamInfo (2nd) / PRI | MED | identical 81b twins, index 02/04 |
| ch6 | ROPlayerController (local) | HIGH | largest initial bunch, opened right after GRI |
| ch7,ch8 | (RO)PlayerReplicationInfo | MED | small, opened with PC, PRI-sized |

The level / GameInfo themselves are NOT given actor channels: the level package and
the GameInfo class come across via **NMT_Welcome (map + game class)** and the
**PackageMap export**, not as replicated actors. GameInfo is server-only in UE3;
the client never gets a GameInfo channel. GameReplicationInfo (ch2) is the
client-visible proxy for game state.

---

## 3. NMT message bytes seen S2C (and their meaning)

The control channel (ch0, ChType 1) multiplexes UE3 `NMT_*` messages
(C++ enum in `EngineBaseTypes`/`DataChannel`, NOT in the decompiled .uc). The
canonical UE3 control-message enum order is:

```
0  NMT_Hello            8  NMT_Login          16 NMT_Beacon...
1  NMT_Welcome          9  NMT_Failure / Join  (build-specific)
2  NMT_Upgrade         10  NMT_Join
3  NMT_Challenge       11  NMT_JoinSplit
4  NMT_Netspeed        12  ...
5  NMT_Login           ...
6  NMT_Failure
7  NMT_Uses / PackageMap-class (DLMgr) <- carries the package/GUID export here
```

Observed first-byte values in S2C control (ch0) bunches and the best mapping
(the *exact* byte alignment of small control bunches differs from the documented
enum value by the channel-message framing — the validated handshake already pins
f162 = NMT_Welcome; values below are the raw decoded first byte + role):

| Raw first byte (S2C ch0) | Frames | Role (from position in flow) |
|--------------------------|--------|------------------------------|
| 0x1e | f157 | post-login control (Challenge/Netspeed-class) |
| 0x20 | f159 | control + ACK batching |
| (NMT_Welcome) | f162 | **NMT_Welcome** — Map + GameClass + URL options (pinned) |
| 0x03 | f165 | post-Welcome control token (8-hex value) |
| **0x07** | f167-f185 | **PackageMap / NetGUID export** (the package list) — the dominant post-Welcome S2C traffic |
| 0x24 | f231 | control sent alongside actor-open burst |
| 0x23 | f252,f266,f279,f297,f310,... | periodic control heartbeat/keepalive (steady state) |

C2S side for context: client replies on ch0 with NMT 0x09 (f227) once it has
verified all packages — this is the "ready / start sending actors" signal that
triggers the f231 actor-open burst. The client also uploads its OWN PackageMap
(C2S f193-f200, same independent NMT_Uses bunch format).

The single most important S2C NMT to implement is **0x07 (PackageMap export)** —
the client will sit on the loading screen until it receives a package list it can
reconcile, then it expects the actor channels.

---

## 4. PackageMap export wire format (DEFINITIVE — bit-exact)

This section supersedes the earlier "names flow continuously / one fragmented
logical bunch" guess. The PackageMap export is **NOT** a single fragmented logical
bunch. It is a stream of **independent `NMT_Uses` (0x07) control messages**, one
per package, that the server packs many-to-a-bunch into reliable ch0 bunches. The
client concatenates the bunch payloads and consumes the 0x07 records in order.

Decoders used: `tools/mock_client.py` framing (no-partial header) for all 20 bunches,
with the corrected `BunchDataBits` bound (MaxPacket*8≈12000, not 16384). All claims
below are reproduced by the scratch scripts (`pmdecode.py`, `reconstruct.py`,
`finalize.py`) and corroborated by the write-side disassembly (§4.6).

### 4.1 The export at a glance
- **20 bunches** carrying the export, `chSeq 5 .. 24` (S2C frames **f167-f185**),
  all on **ch0, chType 1 (control), bReliable=1**.
- **321 packages total**, **zero duplicate names**, parsed cleanly end-to-end.
- chSeq 5-23 (f167-f184) are ordinary reliable control bunches; each
  `BunchDataBits == remaining packet bits` (consumes the whole datagram, one bunch
  per packet). f183 carries two bunches (chSeq 21 full + chSeq 22 = the tiny 63-byte
  `M_VN_Sky` straggler).
- **chSeq 24 (f185) is an ordinary reliable bunch** (NOT partial — see §4.3). It
  carries the last 8 package records **plus a 153-byte control trailer** (Steam
  Workshop config + the `ROEntry` / `ROGame.ROGameInfo` GameInfo class + the level
  GUID `a132a04f-7060-11f1-a540-dc9840089caf`).

### 4.2 Per-package record (`NMT_Uses`, byte 0x07) — exact layout

Every package is one self-contained byte record. The first byte of every bunch is
the 0x07 of its first record (there is no separate per-bunch NMT header byte — the
0x07 IS the message-type tag, repeated once per package):

```
byte   field
0x00   0x07                     NMT_Uses message type
0x01   FGuid (16 bytes)         package NetGUID (raw little-endian 4x u32)
0x11   FString PackageName      u32 len (incl NUL) + ASCII + NUL   e.g. "Core\0"
       FString Extension        "u\0" for script packages, "upk\0" for content,
                                 (entry/level uses ".roe"-class — see trailer)
       u32 PackageFlags         e.g. 0x20204000 / 0x20004001 (LE bytes 00402020 /
                                 01400020); low byte 00/01 = bForcedExport-ish bit
       u32 Generation/NetObjs   = 2 in every observed record
       FString "None\0"         end-of-record sentinel (the "downloadable URL" /
                                 next-name field, empty -> "None")
       8 bytes 0x00             trailing pad (two zeroed u32 — likely GenerationGUID
                                 count / file size fields, always 0 here)
```

First three decoded records (verbatim from f167):

| name | ext | flags(LE) | gen | NetGUID (hex) |
|------|-----|-----------|-----|---------------|
| Core | u | 00402020 | 2 | a8e1984e77d0174b9975823319871988 |
| Engine | u | 00402020 | 2 | b32ce17a78767444bd5a3e74a78de212 |
| ROGame | u | 01402020 | 2 | 4f72ee333551f84397fd9517c15a8d5e |

The list begins Core, Engine, ROGame, GameFramework, WW_Global, EditorResources,
EngineResources, OnlineSubsystemSteamworks, Grip, ENV_Surface_Types, ... and ends
(in f185) ENV_VN_Posters, ENV_VN_Trenches, M_VN_Atmospherics, ROGameContent,
WP_VN_USA_M2_HMG, WW_WEP_M2_Browning, WP_VN_VC_DshK_HMG, WW_WEP_M1_Garand.

### 4.3 f185 decoded bit-exact (the "anomaly" — fully explained, NOT partial)

**There are NO partial bunches in EngineVersion 7258.** This is confirmed from the
disassembly (§4.6): `UNetConnection::SendRawBunch` @ `0x1404a79d0` contains no
`bPartial`/`bPartialInitial`/`bPartialFinal` bits — it is the classic pre-partial
UE3 bunch format. f185 is therefore an **ordinary, complete, separate reliable ch0
control bunch**, identical in structure to f167-f184.

The apparent anomaly was a **decoder bug, not a wire feature**. The earlier decoder
read `BunchDataBits = ReadInt(16384)`. The real serialize bound is
`SerializeInt(BunchDataBits, MaxPacket*8)` and **MaxPacket is NOT 2048** — it is
~1500 (Ethernet MTU), so the bound is ~12000, not 16384. UE3 `SerializeInt(max)`
reads bits while `(value+mask) < max`; with the too-large bound 16384 the reader
consumed one extra bit, mis-reading f185's `BunchDataBits` as 13928 (overflow).
With the correct bound it reads **5736**.

Bit-exact decode of f185 (724 bytes, terminator bit 5789), no-partial layout:

```
bit  field                              value
0    PacketId       = SerializeInt(16384)   23
14   bIsAck                                  0
15   bControl                                0   (=> no bOpen/bClose bits)
16   bReliable                               1
17   ChIndex        = SerializeInt(1023)     0
27   ChSequence     = SerializeInt(1024)     24
37   ChType         = SerializeInt(8)        1   (3 bits — present, reliable)
40   BunchDataBits  = SerializeInt(~12000)   5736  (13 bits)
53   payload (5736 bits = 717 bytes)              starts 0x07 "ENV_VN_Posters"...
5789 terminator
```

Payload starts at **bit 53**, ends **exactly at the terminator (bit 5789)** —
`consumesAll=True`, no overflow, no leftover. `ENV_VN_Posters` sits at payload
byte 21 exactly per §4.2.

**Empirical confirmation of the bound:** sweeping the `SerializeInt` max, ALL 20
export bunches (f167-f185) decode with `consumesAll=True` iff the bound is in
**(10088, 13928]** (the largest bunch, f177, is 10088 bits, so max>10088; f185
requires max≤13928). MaxPacket=1500 → bound 12000 sits squarely in this window and
is the most likely value. **Fix for `tools/mock_client.py` and our C++ BitReader:
use the negotiated `MaxPacket*8` (~12000), not the hardcoded 16384, for
BunchDataBits.** That single change makes f185 decode cleanly with no partial-flag
machinery.

### 4.4 Header bit layout (EngineVersion 7258, disassembly-confirmed)

From `UNetConnection::SendRawBunch` @ `0x1404a79d0` (write side), exact order:

```
PacketId = SerializeInt(16384)
per entry:
  bIsAck                                  ; if 1: SerializeInt(16384) ack, continue
  bControl  (= bOpen || bClose)
  if bControl { bOpen; bClose }
  bReliable
  ChIndex = SerializeInt(1023)
  if bReliable { ChSequence = SerializeInt(1024) }
  if (bReliable || bOpen) { ChType = SerializeInt(8) }    ; 3 bits
  BunchDataBits = SerializeInt(MaxPacket*8)               ; MaxPacket~1500 => ~12000
  payload[BunchDataBits]
```

No partial flags exist. Large payloads are split at the **message** level (many
separate complete bunches), never at the bunch layer.

### 4.5 Reconstructed stream & how our emulator SENDS it

- The full client-consumed byte stream (all 20 bunch payloads concatenated in
  ascending chSeq order) = **22852 bytes**, written to
  `data/packagemap_export_7258.bin`. Per-bunch boundaries + the send plan are in
  `data/packagemap_chunks.json`.
- **Send plan (20 separate SendRawToClient bunches):** emit all 20 chunks (chSeq
  5..24) as ordinary reliable ch0 control bunches — `bControl=0, bReliable=1,
  ChIndex=0, ChType=1, bPartial=N/A`. One bunch per UDP datagram (the retail server
  coalesces a couple of tiny ones, e.g. f183 carried chSeq 21+22, but one-per-packet
  is fine). Each payload is ≤ ~1260 bytes and fits inside MaxPacket. Increment the
  connection-global `ChSequence` per bunch. The 20th bunch (chSeq 24) is just
  another such bunch carrying the last 8 package records + the 153-byte control
  trailer — **no special partial handling needed**. The simplest correct emulator
  loop: for each captured chunk payload in `packagemap_chunks.json` (sorted by
  chSeq), call the existing reliable-control-bunch send with that payload.
- ACK discipline is unchanged: each is a reliable bunch with a connection-global
  ChSequence; the client ACKs them and the server must hold the resend window (the
  capture shows the client bulk-ACKing via f186+).

**C2S note:** the client mirrors its own PackageMap upstream (C2S f193-f200) using
the same record format. Since there are no partial bunches, those are likewise
independent NMT_Uses bunches; our receive path just needs the correct
`BunchDataBits` bound (MaxPacket*8, ~12000 — NOT 16384) to decode them.

### 4.6 Disassembly anchors (VNGame.exe, Win64, write side)

From `D:\rs2dedicatedserver\Binaries\Win64\VNGame.exe` (no symbols; anchored via
string xrefs with radare2). VAs:

| Address | Function | Evidence |
|---------|----------|----------|
| `0x1404a79d0` | `UNetConnection::SendRawBunch` (header bit writer) | emits the §4.4 header, **no partial bits**; `PreSend overflowed` guard @0x1404a7b79 (asserts fit, never splits) |
| `0x14048c5e0` | `UChannel::SendBunch` | single SendRawBunch call @0x14048c8d7; **no fragment/RaiseBunch loop** |
| `0x1404a7680` | PackageMap send loop | iterates `[conn+0xf0]` PackageMap, count `[+0x68]`, base `[+0x60]`, **stride 0x54** (FPackageInfo); one call per package |
| `0x1404a6a60` | per-package serializer | `mov byte [rbp+0x47], 7` @`0x1404a6abf` → writes NMT 0x07; then FGuid(16)+FString name+flags+UGCItemId; flush via `[netdriver+0x288]` @`0x1404a6d2b` |
| `0x14046d7a0` | `FOutBunch` ctor | vtable `0x1410140b0`; sets `[bunch+0xca]=1` (bReliable=1) — fresh reliable bunch per package |
| `0x14007c390` | `FBitWriter::WriteBit` | single-bit writer (IsAck/bControl/bOpen/bClose/bReliable) |
| `0x14007c3e0` | `FBitWriter::SerializeInt(value,max)` | UE3 ranged-int (bit width via `bsr`); ChIndex/ChSeq/ChType/BunchDataBits |

FOutBunch field offsets: `+0xb8` ChIndex, `+0xbc` ChType, `+0xc0` ChSequence,
`+0xc8` bOpen, `+0xc9` bClose, `+0xca` bReliable, `+0x94` BunchDataBits. The
`BunchDataBits` max = `[conn+0x10c] << 3` = MaxPacketBytes*8 (≈12000 — see §4.3).

Exact write-order addresses in `SendRawBunch`: WriteBit(0)@`0x1404a7a27`;
WriteBit(bOpen||bClose)@`0x1404a7a44`; WriteBit(bOpen)@`0x1404a7a61`,
WriteBit(bClose)@`0x1404a7a77`; WriteBit(bReliable)@`0x1404a7a88`;
SerializeInt(ChIndex,1023)@`0x1404a7a9e`; SerializeInt(ChSequence,1024)@`0x1404a7abd`;
SerializeInt(ChType,8)@`0x1404a7ae5`; SerializeInt(BunchDataBits,MaxPacket*8)@`0x1404a7b00`.

This proves: **each package is one fresh reliable NMT_Uses (0x07) bunch sent
individually** (option (a) from the original question) — many separate messages the
client concatenates, NOT one fragmented logical bunch. The 0x07 byte is written once
per package record. NMT 0x07 = NMT_Uses (index 7 in the UE3 control-message name
table at `0x141014508`; payload shape = per-package {GUID,name,flags,generation}).

---

## 5. Implementation roadmap (prioritized)

Goal: get a real client from the loading screen into the map after Join.

### STRICTLY REQUIRED (in this order)
1. **NMT_Welcome (ch0)** — already implemented/validated (f162). Sends Map URL +
   GameClass. Without it the client never starts loading.
2. **PackageMap / NetGUID export (ch0, NMT byte 0x07 = NMT_Uses)** — THE missing
   piece. The server must push the package list the map depends on. Each package is
   one self-contained NMT_Uses record (§4.2): `0x07 + FGuid(16) + PackageName(FString)
   + Extension(FString "u"/"upk") + flags(u32) + generation(u32) + "None"(FString) +
   8 zero bytes`. These are sent as **20 independent reliable ch0 control bunches**
   (NOT partial bunches — §4.3/§4.6), packing many records per bunch. The client
   verifies it has every package (by name+GUID) before it will spawn. The exact
   321-package list is captured to `data/packagemap_export_7258.bin` +
   `data/packagemap_chunks.json` — replay those payloads verbatim.
   - Sub-requirement: just send each chunk payload as a reliable control bunch with
     the correct `BunchDataBits = SerializeInt(MaxPacket*8≈12000)`. NO partial-bunch
     machinery is needed (the engine has none — §4.6).
3. **Wait for client ready (C2S ch0 NMT 0x09)** before opening actor channels.
4. **Open the bootstrap actor channels (ChType 2, bOpen=1, bReliable=1)** in this
   order, mirroring f231:
   a. **ch2 = ROGameReplicationInfo** (REQUIRED — client needs game state to leave
      loading; the GRI is the world's root replicated object).
   b. **ch3 (+ch4,ch5) = TeamInfo** (REQUIRED — GRI.Teams[] must resolve or the HUD
      / team select cannot init).
   c. **ch6 = local ROPlayerController** (REQUIRED — until the client's own PC
      channel opens and replicates, the client has no pawn/possession path and
      stays on the loading/spawn screen).
   d. **ch7/ch8 = PlayerReplicationInfo** (REQUIRED for the local player's PRI; bot
      PRIs are optional).
   Each open bunch must carry a valid `SerializeNewActor` (NetGUID + class NetGUID
   that resolves against the PackageMap we just sent) + the actor's bNetInitial
   property block.
5. **ACK discipline** — the server ACKs the client's packets aggressively
   (f159/f160/f186 etc. are pure/bulk ACK packets). Reliable channel sequencing is
   connection-global; our emulator must already do this (handshake works), but the
   replication phase generates far more reliable bunches, so the resend/ack window
   must hold.

### OPTIONAL / steady-state (after the client is in the map)
6. ch0 NMT 0x23 periodic heartbeat (f252+). Keeps connection alive; not required to
   *enter* the map but required to *stay* in cleanly.
7. Property delta updates (unreliable ChType 0 bunches, first byte 0x17/0x29/0x38)
   on the open actor channels — needed for live gameplay, not for the initial
   transition.
8. Opening additional actor channels for other players/pawns/world actors as they
   become net-relevant.

### Minimal viable "client leaves loading screen" set
`NMT_Welcome` -> full `PackageMap export` (20 independent reliable bunches) -> on
client NMT 0x09 -> open `ch2 GRI` + `ch3 TeamInfo` + `ch6 PlayerController` (+ PRI on
ch7/ch8) with valid initial actor bunches. That is the critical path; everything
in §5.6-5.8 can follow.

### Biggest unknowns to resolve next
- ~~Exact byte layout of a single PackageMap export entry~~ — DONE (§4.2).
- ~~Exact partial-bunch flag bit positions~~ — RESOLVED: there are none (§4.3/§4.6).
- The `SerializeNewActor` NetGUID/class-NetGUID encoding so our actor opens
  resolve against the package map (decode ch2/ch6 opens from f231 bit-exact).
- Property block layout per class (handle index + conditional props) for the GRI,
  TeamInfo, PlayerController initial bunches.

---

## Appendix: how to reproduce

```
# all S2C frames, hex:
"C:\Program Files\Wireshark\tshark.exe" -r D:\RE-Tools\rs2_handshake_capture.pcapng \
  -Y "udp.srcport==7777" -T fields -e frame.number -e data.data
```
Two decoder fixes vs the original `tools/mock_client.py`:
1. Handshake phase uses `bd = r.rint(64)`; the established (NMT/replication) phase
   uses `bd = r.rint(MaxPacket*8)`.
2. The established-phase `BunchDataBits` bound is **MaxPacket*8 ≈ 12000**
   (MaxPacket ≈ 1500), **NOT 16384**. With 16384 every export frame decodes EXCEPT
   f185, which over-reads its `BunchDataBits` by one bit (the false "partial bunch").
   With the correct ~12000 bound, all 20 export bunches (f167-f185) decode with
   `trail=0` and there are NO partial bunches (confirmed by disassembly, §4.6).
