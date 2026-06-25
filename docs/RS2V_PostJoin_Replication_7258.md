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
       Client mirrors this upstream (C2S f193..f200, also partial bunches).
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
| f185  | 23 | 0 | 1 | (partial) | 24 | n/a | 0x07 | PackageMap export tail (ENV_VN_Posters, ROGameContent, WP_VN_USA_M2_HMG, WW_WEP_M2_Browning ...). **Does NOT decode at the simple 2048 model — it is a *partial bunch* (UE3 bPartial/bPartialInitial/bPartialFinal flags), see §4.** |
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

All large frames decode with `trail=0` at MaxPacket=2048 EXCEPT f185 (partial bunch).
Every f167-f184 bunch yields clean package-name ASCII, confirming the model.

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
(C2S f193-f200, partial bunches).

The single most important S2C NMT to implement is **0x07 (PackageMap export)** —
the client will sit on the loading screen until it receives a package list it can
reconcile, then it expects the actor channels.

---

## 4. The f185 anomaly — partial bunches

f167-f184 decode cleanly as single reliable control bunches with
`BunchDataBits = ReadInt(16384)`. **f185 does not** — the simple model reads
`BunchDataBits = 13928` which overflows the packet (only ~5735 bits remain).

Root cause: the PackageMap export is one *logical* reliable bunch far larger than a
single 2048-byte packet, so UE3 splits it into **partial bunches**. EngineVersion
7258 bunch headers therefore carry extra partial flags that the simplified header
parse omits:

```
... bReliable(1); ChIndex; if bReliable ChSequence;
    bPartial(1); if bPartial { bPartialInitial(1); bPartialFinal(1) }
    if (bReliable||bOpen) ChType;
    BunchDataBits = ReadInt(MaxPacket*8);
```

Proof: bit-shifting f185 right by 5 bits (≈ the width of the missing partial-flag
field) reveals clean package names: `ENV_VN_Posters, ENV_VN_Trenches,
M_VN_Atmospherics, ROGameContent, WP_VN_USA_M2_HMG, WW_WEP_M2_Browning,
WP_VN_VC_DshK_HMG, WW_WEP_M1_Garand`. So f185 is simply the continuation/final
fragment of the PackageMap export.

**Action item:** add bPartial/bPartialInitial/bPartialFinal parsing to our
BitReader bunch header (and to `mock_client.py`) so f185 (and the C2S f193-f200)
decode and so our emulator can both *send* and *receive* split package-map bunches.
The exact bit position of the partial flags relative to ChType is the one
remaining header detail to pin (the ~5-bit shift suggests partial flags sit
between ChSequence and ChType, consistent with later UE3).

---

## 5. Implementation roadmap (prioritized)

Goal: get a real client from the loading screen into the map after Join.

### STRICTLY REQUIRED (in this order)
1. **NMT_Welcome (ch0)** — already implemented/validated (f162). Sends Map URL +
   GameClass. Without it the client never starts loading.
2. **PackageMap / NetGUID export (ch0, NMT byte 0x07)** — THE missing piece. The
   server must push the full ordered package list the map depends on, each entry =
   `NetGUID + flags + PackageName(FString) + Extension("upk"/"roe"/"@"/"None") +
   16-byte package GUID`, terminated by a `None`. This spans many reliable bunches
   (split as PARTIAL bunches — see §4). The client verifies it has every package
   (by name+GUID) before it will spawn. Use the exact package list captured in
   f167-f185 for our stock map as the reference set.
   - Sub-requirement: implement **partial bunch** send/receive
     (bPartial/bPartialInitial/bPartialFinal) so the >2KB package map can be split.
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
`NMT_Welcome` -> full `PackageMap export` (with partial bunches) -> on client
NMT 0x09 -> open `ch2 GRI` + `ch3 TeamInfo` + `ch6 PlayerController` (+ PRI on
ch7/ch8) with valid initial actor bunches. That is the critical path; everything
in §5.6-5.8 can follow.

### Biggest unknowns to resolve next
- Exact byte layout of a single PackageMap export entry (NetGUID compression +
  FString package name + GUID) — decode one entry bit-exact from f167.
- Exact partial-bunch flag bit positions (§4).
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
# decode at MaxPacket=2048 (BunchDataBits = ReadInt(16384)):
python decode2048.py s2c_frames.txt <frame...>
```
Key fix vs `tools/mock_client.py`: the mock uses `bd = r.rint(64)` (handshake-phase
width). For the established phase it MUST be `bd = r.rint(16384)`. With that change
every S2C frame from f157-f184 decodes with `trail=0`; only the partial-bunch frames
(f185, C2S f193-f200) still need the partial-flag header addition.
```
