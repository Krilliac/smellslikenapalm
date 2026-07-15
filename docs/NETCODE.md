# NETCODE — the UE3 7258 wire protocol as smellslikenapalm implements it

The single source of truth for **how the server actually talks to a retail RS2:Vietnam
client** (UE3 EngineVersion 7258, EGS/EOS "Leech" build). This is the *implementation*
view: it describes the code paths in `src/Network` that frame, send, retransmit and decode
UE3 packets, and ties each one to the bit-level reverse-engineering docs that prove the
wire format.

If you are about to touch netcode, read this first, then drill into the cited code and RE
docs. **Do not invent wire layout** — every field width here is pinned in an RE doc or a
capture.

Companion ground-truth docs (cite, don't duplicate):
- `docs/RS2V_ControlChannel_WireSpec_7258.md` — bit-level packet/bunch framing spec (disasm + capture)
- `docs/re/MASTER_replication_reference.md` — THE consolidated replication ground truth (constants, handle tables, codecs)
- `docs/re/open_bunch_structure.md` — SerializeNewActor open-bunch bit layout (canonical)
- `docs/re/ue3_property_value_codec.md` — per-UProperty value codec
- `docs/UE3_ClassNetCache_HandleOrder.md` — how ClassNetCache handle order is derived
- `docs/RS2V_PostJoin_Replication_7258.md` — the post-Join world bootstrap (PackageMap + actor burst)
- `docs/re/pc_ch2_postjoin_timeline.md` — the ch2 reliable-RPC sequence

Code anchors: `src/Network/PacketCodec.{h,cpp}`, `src/Network/ConnectionManager.{h,cpp}`,
`src/Network/HandshakeState.{h,cpp}`, `src/Network/PacketAssembler.h`,
`src/Network/BitReader.{h,cpp}` / `BitWriter.{h,cpp}`, `src/Network/ActorReplication.h`.

---

## 0. The layers

```
UDP datagram
  │
  ▼  ConnectionManager::ParseIncomingControl   (src/Network/ConnectionManager.cpp:1021)
PacketCodec::Decode  ───────────────────────────  packet framing (PacketId / acks / bunches)
  │                                                (src/Network/PacketCodec.cpp:105)
  ├─ ch0 bunches → ControlReassembler → HandshakeState   (NMT / handshake messages)
  └─ ch≥2 bunches → DecodeInboundActorBunch              (actor-channel RPCs: SelectTeam…)
```

Outbound is the mirror: a caller builds bunches → `PacketAssembler::BuildRawBunches…` stamps
PacketId/ChSequence/acks → `PacketCodec::Encode` → `UDPSocket::SendRaw`.

Three things never mix:
1. **Framing** (`PacketCodec`) — PacketId, acks, bunch headers, BunchDataBits. Knows nothing about NMT or actors.
2. **Control messages** (`ControlChannel` / `HandshakeState`) — the NMT byte-stream carried on ch0.
3. **Replication** (`ActorReplication`, the bootstrap replays, the ch2 RPC path) — actor channels (ch≥2).

All bit IO is **LSB-first within each byte**; bounded ints use `SerializeInt`/`ReadInt`
(UE3 `FBitReader::SerializeInt`). Multi-byte fixed values are little-endian.

---

## 1. Packet framing — `PacketCodec`

A UE3 datagram is exactly one packet:

```
Packet:
    PacketId = SerializeInt(16384)        # MAX_PACKETID = 16384 (14-bit bound)
    loop until terminator:
        IsAck = ReadBit()
        if IsAck: AckPacketId = SerializeInt(16384)     # acks a received PacketId; nothing else
        else:     <Bunch>                               # §1.2
    terminator: a single '1' bit after the last entry, then zero-pad to a byte.
```

Implemented in `PacketCodec::Decode` (`src/Network/PacketCodec.cpp:105`) and
`PacketCodec::Encode` (`:217`). Spec: `docs/RS2V_ControlChannel_WireSpec_7258.md` §2–3.

### 1.1 Terminator / readable-bit recovery

There is no length field. The receiver recovers the readable bit count from the **high set
bit of the packet's LAST byte**:

```
PacketBits = NumBytes*8 - 8 + HighBit(lastByte)
```

(`PacketCodec.cpp:127-135`; `HighBit` at `:46`.) The sender's flush guarantees the final
byte is non-zero. Captured datagrams sometimes carry a constant trailing byte *after* the
packet's own terminator byte (e.g. a 10-byte datagram whose UE3 packet is only 9 bytes —
the opening reliable Hello bunch ends at bit 64). `Decode` tolerates this: it treats the
terminator as the high bit of the last byte and the benign terminal `SerializeInt` overflow
(see below) as end-of-stream rather than an error. `Encode` always emits the canonical
`data + '1' terminator + zero pad` and never reproduces a trailer.

**Benign terminal overflow:** the last entry's `SerializeInt` naturally tries to read one
bit past the terminator (it keeps consuming bits until `value+mask >= max`). UE3 tolerates
this via `AtEnd()`, and so does `Decode` — it only rejects a datagram whose *PacketId*
cannot be read (`Decode` sets `Packet::ok=false` only on an empty datagram, an all-zero last
byte, or a PacketId overflow; `:121-146`). Everything else is best-effort/UE3-lenient.

### 1.2 Bunch header — field order and conditional presence

This is the **complete, exact** header for 7258. There are **no** `bPartial*`,
`bHasMustBeMappedGUIDs`, `bHasServerPackageMapAcks`, or `bIsReplicationPaused` bits — they do
not exist on this build (`docs/RS2V_ControlChannel_WireSpec_7258.md` §3, both read and write
paths disassembled). Read order (`PacketCodec.cpp:160-185`):

```
bControl   = ReadBit()
if bControl:
    bOpen  = ReadBit()
    bClose = ReadBit()
# if !bControl, bOpen and bClose are forced 0
bReliable  = ReadBit()
ChIndex    = SerializeInt(1023)        # MAX_CHANNELS wire bound; control channel = 0
if bReliable:
    ChSequence = SerializeInt(1024)    # 0x400
if (bReliable || bOpen):
    ChType     = SerializeInt(8)       # CHTYPE_MAX; control=1, actor=2
BunchDataBits  = SerializeInt(MaxPacket*8)
<payload: BunchDataBits bits>
```

Conditional presence is the part that bites: **ChSequence is present only when `bReliable`**,
and **ChType is present only when `bReliable || bOpen`**. Get either condition wrong and the
whole rest of the datagram shifts by a few bits and mis-decodes. `Encode` mirrors this
exactly (`:228-262`).

The decoded `Bunch` is `{bControl,bOpen,bClose,bReliable,chIndex,chSequence,chType,payload,payloadBits}`
(`PacketCodec.h:64`). `payload` holds the bunch data bits packed LSB-first; `payloadBits` is
the exact count.

### 1.3 BunchDataBits / MaxPacket — the phase- and direction-dependent bound

`BunchDataBits = SerializeInt(MaxPacket*8)`. The **width** of that `SerializeInt`, and thus
where the payload starts, depends on `MaxPacket`. This is the single most error-prone
constant in the codec. Values (`PacketCodec.h:38-60`):

| Constant | Value | Bound | Used for |
|---|---|---|---|
| `kHandshakeMaxPacketBytes` | 8 | 64 | historical StatelessConnect default (see note) |
| `kNmtMaxPacketBytes` | 2048 | 16384 | **DECODE all inbound (C2S)** |
| `kServerSendMaxPacketBytes` | 1500 | 12000 | **ENCODE all outbound (S2C, we are the server)** |

Key facts, hard-won (see the long comments at `PacketCodec.h:29-60` and
`ConnectionManager.cpp:1029-1040`):

- **MaxPacket is asymmetric.** The retail client encodes its C2S bunches at MaxPacket 2048
  (bound 16384 — proven byte-exact against the Login version fields 7038/7258, the SteamID,
  rate 80000, and the login URL). A dedicated server encodes S2C at ~MTU (1500 → bound
  12000), which is what the client expects from a server. So: **decode inbound at 16384,
  encode outbound at 12000.**
- **There is no small-bound "handshake phase" on decode.** The client frames BunchDataBits
  at 2048 from the very first packet, including the StatelessConnect bunches. Decoding the
  handshake bunches at the old bound 64 misaligned them (the NMT byte landed in the 2nd
  byte) and mis-keyed HandshakeStart/Response. `ParseIncomingControl` therefore **always**
  decodes at `kNmtMaxPacketBytes` (`ConnectionManager.cpp:1038`). `kHandshakeMaxPacketBytes`
  is retained only as the codec's API default.
- One bit too narrow (1024 → bound 8192, width 13) right-shifts the entire NMT payload by
  one bit per bunch and mis-reads every byte (Hello `0x00`→`0x20`, Login `0x10`→`0x08`).

`PacketCodec::SetDebugTracing(true)` (`:102`) turns on a non-fatal per-bunch trace +
invariant warnings (payloadBits ≥ bound, leftover unconsumed bits, malformed datagram). It
never alters the wire bytes or parse result — observability only.

---

## 2. The StatelessConnect handshake

Driven by `HandshakeState` (`src/Network/HandshakeState.{h,cpp}`), one instance per
connection, fed complete ch0 control messages by the reassembler. It **uses** the
`ControlChannel` message codec; it never re-implements framing.

There are two sub-phases. Until the StatelessConnect handshake completes, inbound control
messages route by **subtype** (the payload's first byte), not the NMT switch
(`HandshakeState.cpp:79-81`, `HandleHandshakeMessage` at `:146`):

```
StatelessConnect (pre-NMT):
  C→S  0x1d HandshakeStart      → S→C 0x1e HandshakeChallenge (32-bit server nonce)
  C→S  0x1f HandshakeResponse   → S→C 0x20 HandshakeComplete   ; m_controlHandshakeComplete = true
                                                                ; phase → AwaitingHello
```

Both the challenge nonce and the client's response are **accepted blindly** — no CRC32 /
cookie validation (`HandshakeState.cpp:157-171`; this is a server policy choice, not a wire
requirement). The handshake NMT byte is the *first* byte of the payload; there is no `0x00`
family prefix (capture: HandshakeStart = `[1d 01]`, Response = `[1f …]`).

Then the NMT phase (`HandshakeState.h:13-23`, happy path):

```
AwaitingHello --Hello(0x00)--> ChallengeSent --Login--> WelcomeSent --Join(0x06)--> Joined
                                     ^
                       Netspeed may arrive any time after Hello (does not change primary state)
```

- **Hello** (`OnHello`, `:185`): RS2's on-wire NMT_Hello is minimal (NMT + a single BYTE) —
  the version/SteamId/session fields are *not* in this message; they arrive in Login. We do
  not gate on version. Server replies with **Challenge** (a single 32-bit cookie) → `ChallengeSent`.
- **Login** (`OnLogin` / Steam login `0x10`/`0x12`): both routes funnel through
  `CompleteLogin` (`HandshakeState.h:142-145`). The EOS build uses **Steam login**, not a
  separate Hello→Challenge. Steam auth (`NMT_SteamAuth`/`NMT_SteamLogin`) is **stubbed**:
  accepted with no ticket validation (`HandleSteamAuthStub`). `CompleteLogin` sends
  **Welcome**, parses the FURL options, fires `ClientLoggedIn`, → `WelcomeSent`.
- **Join** (`OnJoin`): fires `ClientJoined`, → `Joined`. Handshake complete.

Two Game-facing events cross the Network→Game boundary *without* a compile dependency
(`HandshakeState.h:48-65`): `ClientLoggedInEvent` (carries stubbed steamId/session + parsed
`URLOptions`) and `ClientJoinedEvent`. `ConnectionManager` owns the observer dispatch
(`FireClientLoggedIn`/`FireClientJoined`). **These two events are the bootstrap triggers** —
see §4.

---

## 3. Reliable retransmission

UE3 reliability rule: a reliable bunch must be re-sent until the client acks the *packet*
that carried it. Without this, one dropped reliable bunch in the bootstrap burst stalls that
channel forever and the client soft-locks (can't even disconnect). The send and retry state
lives in `ConnectionManager`:

**Per-channel send state** (`ControlState`): `outbound` (`PacketAssembler` — assigns
PacketIds and drains queued acks), the inbound `reassembler`, ch2 RPC bookkeeping
(`ch2Reliable`, `actorChType`, `teamSelected`), and `pendingReliable` — the list of
un-acked reliable bunch-sets. `ch2Reliable` is an `OutboundReliableSequencer` instance
owned by that connection's PlayerController channel; it allocates the explicit
ChSequence values used by ch2 actor bunches:

```cpp
struct SentReliable {
    std::vector<uint32_t> packetIds;          // every PacketId this set has gone out in
    uint64_t lastSendMs;
    int      resendCount;
    std::vector<PacketCodec::Bunch> bunches;  // the reliable bunches, verbatim
};
```

**Reserve and record.** `OutboundReliableSequencer::ReserveBatch` reserves a contiguous
batch atomically in the modulo-1024 ch2 sequence space. Sequence **0 is valid after wrap**;
there is no zero sentinel once the allocator has been initialized. At most **511** values
may span the forward issuance window from the oldest unresolved value through the cursor,
keeping modular ordering strictly inside half a cycle. An out-of-order packet ACK stops
retransmission but leaves a window tombstone until every older gap is ACKed; raw in-flight
count therefore cannot reopen capacity prematurely. A failed reservation changes neither
the cursor nor the in-flight/window state, so sequence pressure is transient backpressure
rather than a manufactured gap. Only one unpublished reservation may exist, and its opaque
monotonic token prevents an ancient same-shaped batch from cancelling a later modulo-lap
reservation.

`SendReservedCh2Bunches` verifies that the reliable ch2 bunches consume the reservation
in order, then passes them to `SendReliableBunches`. The latter builds one packet, attempts
the initial datagram send, and records its reliable bunches in `pendingReliable`. Once that
retry ledger owns the batch, `CommitBatch` removes rollback eligibility. This commit is
based on successful queueing in `pendingReliable`, not on the first UDP send succeeding;
`RetransmitTick` can recover a failed first send. If a batch is rejected before queueing,
the latest unpublished reservation is cancelled and the cursor is rewound without a gap.

**Ack-clear — `OnClientAck`:** when a client ack names any PacketId associated with a
`SentReliable`, release each tracked reliable ch2 ChSequence from `ch2Reliable`, then drop
the pending set. Non-ch2 reliable state continues to use its owning subsystem's lifecycle.

**RTO resend — `RetransmitTick`:** called every pump cycle from `PumpNetwork`. For each
pending set older than `kRtoMs = 250` and under
`kMaxResends = 12`, it rebuilds a packet from the **same bunches verbatim** — same
per-channel ChSequence — in a **NEW PacketId**, sends it, and appends the new PacketId to the
set. The client fills the gap or ignores the duplicate.

**Critical invariant — never manufacture a sequence gap.** Resends keep the *original*
ChSequence; a NEW PacketId is fine, a new ChSequence is not. The old "proof-of-life re-send"
of ClientShowTeamSelect sent a fresh bunch at `seq+1`, which (if the original seq was
dropped) created a ch2 reliable-sequence hole → permanent ch2 stall → soft-lock. That code
was removed; retransmission now redelivers the original (`ConnectionManager.cpp:889-892`).

**Ack policy (receive side).** We ack an inbound packet **only if it carried bunch data**
(`ParseIncomingControl:1060-1062`). Acking a pure-ack packet makes the peer ack our ack, and
us ack that, forever — an observed infinite ack ping-pong against the live client. The ack
rides on the next outbound packet (drained by the PacketAssembler), or a standalone
ack-only packet if nothing else is going out (`:1096-1098`).

---

## 4. World bootstrap

After the handshake the retail client sits on the loading screen until the server replicates
the world. The order is **PackageMap export first, then the actor channel burst** — and the
*timing* matters, because each step gates a client reply.

### 4.1 PackageMap export — on ClientLoggedIn (right after Welcome)

`SendReplicationBootstrap` (`ConnectionManager.cpp:1004`) is called from `FireClientLoggedIn`
(`:438`), i.e. **immediately after NMT_Welcome, before the client's Join**. This matches the
capture: Welcome (f162) → PackageMap (f167–185) → client "packages verified" reply (f227).
Sending it on Join instead would deadlock a real client — it won't send Join until it has
reconciled the PackageMap (`:433-437`).

The records are loaded from `data/replication_bootstrap.bin`, a stream of
`[uint32 LE len][payload]` records (`GetReplicationBootstrapRecords`, `:567`), each a complete
control-channel message (e.g. an NMT 0x07 PackageMap chunk) sent as one reliable control
bunch via `SendRawToClient`. No-op (logged) if the file is absent — the handshake is
unaffected.

Every reliable control packet is also entered separately in the connection retransmission
ledger. This is not optional even on loopback: the 29-datagram PackageMap burst repeatedly
lost only its final ch0 sequence 33 while every earlier packet was acknowledged. The client
then omitted the six packages carried by that packet and waited forever before `NMT_Join`.
The original and official payloads were byte-identical; retrying the same bunch with a new
PacketId produced the final six `NMT_Have` records and an immediate Join. Control traffic
uses an 8-second retry delay with 64 seconds of cold-start headroom, while actor traffic
retains its shorter retry schedule.

The canonical artifact remains the unconditional default. For bounded installed-package
GUID testing only, start the process with
`RS2V_REPLICATION_BOOTSTRAP_VARIANT=installed` to select the separate
`data/replication_bootstrap_installed.bin` candidate. The selector accepts only that exact
lowercase value; every other non-empty value fails closed with post-Welcome replication
disabled. Startup logs include the exact selected variant and artifact path, and a missing
candidate never falls back to canonical implicitly.

Both complete 34,199-byte PackageMap/control artifacts are identity-pinned before parsing:
canonical SHA-256 `A8EA6DCA...D5E53D6`, installed SHA-256
`519D7594...DA596F`. A swapped, corrupted, or merely structurally valid wrong artifact is
rejected before it can be paired with the selected numeric object layout.

This is an integrity pin for emulator-owned artifacts, **not client attestation**. The
minimal retail `NMT_Hello` carries neither a binary hash nor package hashes, and the runtime
does not read or authenticate the connected client's files. The installed cohort was
rechecked against `VNGame.exe` SHA-256 `E578DDE4...AAF7E11` and `ROGame.u` SHA-256
`AED4E60D...C44961`; the JSONL audit records package headers and paths but not those full-file
hashes. A future installed-build gate must therefore compare an independently supplied
local/build fingerprint and fail closed on mismatch; it still cannot prove a remote client
without a new authenticated attestation protocol.

This installed candidate is deliberately **not** treated as a new canonical capture. Its
provenance is recorded in `data/replication_bootstrap_installed.audit.jsonl`: all 473
package identities match the installed retail build after the nine GUID replacements, but
the final generation rows grew by four network objects in `ROGame.u` and one in
`OnlineSubsystemSteamworks.u`. Passing UE3's package-identity gate therefore proves only
that the client may proceed past `NMT_Uses`; downstream `ObjectBase`/static references must
still be validated from the retail packet trace before this artifact can be promoted.

The 2026-07-14 retail dogfood runs prove this candidate clears PackageMap reconciliation,
loads `VNTE-Resort`, reaches `NMT_Join`, creates all six menu-critical actors, and adopts
ch2 as the local `ROPlayerController`. Source-grounded installed-package indices provide
the actor-side candidate under the same `installed` selector. Actor opens reference their
`Default__` archetypes, while HUD/GameInfo values are UClass refs:
`Default__ROPlayerController=57522`, `Default__ROTeamInfo=90248`,
`Default__ROPlayerReplicationInfo=86704`, `Default__ROGameReplicationInfo=70889`,
embedded Territory `GameClass=69603`, and
`ClientSetHUD`'s `ROHUD` UClass `76594`. The installed Resort PackageMap base is five
objects later than the capture (`288295 -> 288300`), so six `MapBoundaries` plus the Axis
and Allies spawn-protection references also move by exactly +5. Three of those eight stale
map refs resolved to a wrong object of the expected class and produced no client warning;
all eight are therefore patched as one decoded cohort. The server derives the candidate in
memory from canonical `data/actor_bootstrap.bin` using 19 pinned byte changes representing
16 semantic static-reference corrections. Derivation requires the 14,537-byte source
capture SHA-256 `AF0CA556...0522B3E0` and verifies candidate SHA-256
`6DF1E41B...851418`; a wrong source, unsupported selector, or unavailable SHA-256 support
disables actor replication instead of falling back. Installed full-world replay remains
disabled because only the menu-critical Resort records have been completely grounded.

Authored `spawns.txt` h59 references follow that same canonical map layout. The
connection's frozen artifact selection now carries a map-object offset (`0` for canonical,
`+5` for installed), and `SendRetailSpawnLocations` rebases each nonzero
`ROVolumePlayerStartGroup` reference with a widened, fail-closed bounds check. This keeps one
grounded map fixture valid for both artifacts; it also prevents Compound's installed US SK
slot from resolving five exports early as `ROVolumeMapBoundary_9`.

The playable `ROTeamInfo` opens must also seed h62 `ReinforcementsRemaining` with a
positive value before h59/h210 can open spawn selection. Retail
`ROUISceneSpawnSelect.UpdateReinforcements()` treats the cooked/default zero as an
exhausted team and closes the scene during initialization. Official opens carry h62
(`243` and `299` in the grounded capture); the live builder publishes the authoritative
`TicketSystem` count, clamped to `int32`. An initial-zero emulator pool means unlimited,
so it uses a positive display sentinel instead of retail's destructive zero. On Skirmish
maps such as Compound, the repaired scene intentionally auto-selects normal slot 0 and
closes on first render; a persistent selection map is expected on Territories maps such
as Resort.

Maps without an exact bounded retail profile no longer borrow Resort's Welcome/actor
stream. Both PackageMap and actor bootstrap fail closed for such a map, preventing the
client from loading Resort while the server is authoritative for a different world.

### 4.2 Actor channel burst — on ClientJoined

`SendActorBootstrap` (`ConnectionManager.cpp:656`) is called from `FireClientJoined`
(`:461`). It replays the official server's post-Join open burst from `data/actor_bootstrap.bin`,
a stream of full bunch descriptors
`[u16 chIndex][u8 chType][u8 flags][u16 chSeq][u32 bunchDataBits][payload]`
(`GetActorBootstrapRecords`, `:613`; flags: b0 bOpen, b1 bClose, b2 bReliable, b3 bControl).
Three deliberate framing decisions, each fixing a real soft-lock:

1. **NMT 0x24 first** (`:678-679`). The real server sends one NMT 0x24 (`24 01 00 00 00`,
   int32 LE = 1) on ch0 *immediately after Join and before any actor channel*. Our flow
   lacked it; we now send it first.
2. **ch2 (the PlayerController) opened first, standalone** (`:706-716`). The client adopts
   ch2 (NetPlayerIndex==0) as its LOCAL PlayerController via `HandleClientPlayer`, and the
   team menu only opens once that adoption succeeds (ShowTeamSelect's
   `LocalPlayer(Player)!=none` gate). Burying ch2 in the middle of 138 other opens made
   adoption intermittent; a clean standalone packet up front makes it reliable.
3. **Batched opens** (`:683-744`). The rest of the opens are packed into
   ~`kBatchBitBudget = 11000`-bit (~1400-byte) packets (≈10–14 opens each) instead of one
   datagram per bunch. 139 back-to-back single-bunch datagrams overflow the client's UDP
   receive buffer (even on loopback) and intermittently drop the ch2 open. Batching matches
   how the real server frames its burst (multiple bunches per packet). A ch0 record in the
   stream flushes the pending batch first (ordering) and rides the normal control path.

Every actor bunch goes out through `SendReliableBunches`, so the whole burst is covered by
the retransmission machinery in §3.

> The actor payloads in `actor_bootstrap.bin` are a **best-effort verbatim replay** — they
> contain session-specific NetGUIDs and the recorded player's state. Correct per-session
> actor replication (building these from live game state via `ActorReplication.h`) is a later
> step. The *framing* (this doc) is correct and session-independent.

### 4.3 Open-bunch (SerializeNewActor) payload layout

The opening bunch of an actor channel carries, in order (canonical:
`docs/re/open_bunch_structure.md`, `MASTER_replication_reference.md` §2):

```
[ classRef ]       32 bits = 1 selector bit(=0) + SerializeInt(idx, 0x80000000)   (static class index)
[ Location ]       FVector::SerializeCompressed — ALWAYS present (zero vec = 11 bits)
[ Rotation ]       FRotator::SerializeCompressed — ONLY if class bNetInitialRotation
                   (false for PC/GRI/TeamInfo/PRI)
[ NetPlayerIndex ] 8-bit BYTE — ONLY for PlayerController channels (ch2 = 0)
[ property block ] repeat { SerializeInt(handle,maxHandle); typedValue } until BunchDataBits exhausted
```

There is **NO separate per-actor NetGUID** — actor identity is the channel index. (This
retracted an earlier 64-bit `[classNetGUID][actorNetGUID]` claim; removing those 32 bits —
they were the compressed Location — made GRI/TeamInfo/PRI decode bit-exact.) The minimal
TeamInfo opens are just `classRef + Location + property block` (81-bit open for the inert
spectator TeamInfo on ch21).

---

## 5. ClassNetCache handle model + the value codecs

### 5.1 How handle / maxHandle are derived

Every replicated property and `reliable client`/`server` function has a stable **wire
handle** = `FieldsBase(class) + rank`, where rank is the field's position when the class's
own net fields are **sorted by the engine's real `NetIndex`** (UObject.NetIndex), base-class
chain first. `maxHandle(class)` = total net-field count over the whole inheritance chain. A
property/function handle is written as `SerializeInt(handle, maxHandle)` — roughly
`ceil(log2(maxHandle))` bits.

This is **derived from the compiled `.u` packages, not the decompiled `.uc` declaration
order** (which is reordered and gives wrong handles). `tools/netfields_from_u.ps1` loads the
`BrewedPCServer` `.u` files via UELib, walks each class's inheritance chain, collects
`CPF_Net` properties + `FUNC_Net` first-declared functions (`Super==null`), **sorts by
`NetIndex`**, and emits `tools/netfields_u_<Class>.txt` (handle tables). See
`docs/UE3_ClassNetCache_HandleOrder.md` and `MASTER_replication_reference.md` §5.

Pinned `maxHandle` values (capture-verified, `MASTER` §0):

| Class | maxHandle | Channel(s) |
|---|---|---|
| ROPlayerController | **531** | ch2 |
| ROGameReplicationInfo | 184 | ch54 |
| ROTeamInfo | 78 | ch21/56/76 |
| ROPlayerReplicationInfo | 98 | PRI channels |
| ROPawn / ROWeapon | 170 / 99 | spawn clusters |

Triple-confirmed for ClientShowTeamSelect: NetIndex sort → handle 206; decoding the
real-server capture's 20571 S2C ch2 bunches with this map yields ZERO Server-function
mismatches; and `SerializeInt(206,531) = bytes CE 00` exactly matches the official server's
own ClientShowTeamSelect at f1637 (`ConnectionManager.cpp:758-766`).

### 5.2 Object-reference / NetGUID codec (`UPackageMap::SerializeObject`)

```
[ selector bit ]
  selector == 0  → STATIC object:  SerializeInt(index, 0x80000000)   (~31 bits)
  selector == 1  → DYNAMIC actor:  SerializeInt(channelIndex, 1024)  (~10 bits)
None = selector 1 + SerializeInt(0, 1024)   (all-zero ~10 bits)
```

`MASTER` §3; used e.g. for PRI.Team (handle 35) → the TeamInfo's open channel
(`ConnectionManager.cpp:960-961`). **The dynamic/None bound is 1024 on this build**, not the
generic-UE3 2048 — every bit-exact decode in the capture reproduces only with 1024
(`ActorReplication.h kDynamicChannelMax`). If a future re-cook desyncs object refs by one
bit, this is the first suspect.

### 5.3 Compressed vector / rotator

`FVector::SerializeCompressed` (UnMath.cpp:51) — always present in an open header:

```
Bits = Clamp(CeilLogTwo(1+max(|ix|,|iy|,|iz|)),1,20)-1     ; ix=round(x)…
SerializeInt(Bits, 20)                                      ; magnitude class
for each axis: SerializeInt(comp + (1<<(Bits+1)), 1<<(Bits+2))   ; Bits+2 bits each
```

Zero vector = 11 bits (`SerializeInt(0,20)` + 3×`SerializeInt(2,4)`). Components are
integer-rounded (lossy). `FRotator::SerializeCompressed` (UnMath.cpp:84): per
Pitch/Yaw/Roll, 1 presence bit + (if `(angle>>8)!=0`) the 8-bit high byte; absent for all
menu actors. Full detail: `MASTER` §2, `docs/re/ue3_property_value_codec.md`.

### 5.4 Property value codec (summary)

Per replicated property: `handle = SerializeInt(FieldNetIndex, maxHandle)`; if `ArrayDim!=1`
an 8-bit **raw** element index; then the typed value. No count, no terminator — the loop ends
when the bunch runs out of bits. **The bunch MUST end exactly on the last value's last bit**;
stray pad bits get misread as a bogus handle (this is what hung the client). Type encodings
(bool=1 bit, byte=8, enum=`ceil(log2(NumEnums-1))`, int/float=32 LE, FString=`int32
SaveNum`+chars, UniqueNetId=64-bit LE SteamID64, etc.): `MASTER` §4 /
`docs/re/ue3_property_value_codec.md`.

---

## 6. Actor-channel RPC path — and the per-parameter "Send" bit

### 6.1 Server → client RPC (`SendCh2Rpc`)

`SendCh2Rpc` sends a reliable server→client function call on the PlayerController channel
(ch2). The bunch is `bReliable=1, bOpen=0, bClose=0, chIndex=2,
chType=actorChType`. Its ChSequence comes from a one-value atomic reservation in
`ch2Reliable`, which adopts the externally assigned ch2-open sequence during actor
bootstrap. Allocation advances modulo 1024, including sequence 0, and returns transient
backpressure when the 511-value issuance window cannot accept the reservation. The payload
the caller packs is:

```
SerializeInt(handle, maxHandle)   +   [serialized params…]
```

It goes out through the reserved-ch2 wrapper and `SendReliableBunches`, so the reservation
is committed only after it enters `pendingReliable`, retransmissions reuse that same
ChSequence, and the value is released only by an ACK for one of the packet attempts.
ClientShowTeamSelect is the simplest case: a `reliable client` function with **no params**,
so the payload is just `SerializeInt(206, 531)` = `CE 00` (9 bits) and nothing else.

**Possession recovery is generation-bound.** A canonical standalone reliable 9-bit
`AskForPawn` (handle 42) is only eligible while the owning pawn is alive and spawned and
the open pawn graph, recovery binding, and authoritative owning-pawn life all name the
same nonzero generation. Death, failed graph publication, team/deployment reset, and map
travel invalidate that binding, so no response is emitted while lifecycle state is stale or
unbound. The h42 request is parameterless and h44 names the stable ch209, however, so the wire
does not identify which life originated a message that arrives only after a later generation is
already bound. A matching `ServerAcknowledgePossession` is therefore applied only while the
current generation is live; this is a lifecycle gate, not cryptographic correlation to a life.
The recovery count and rate-limit timestamp are committed only after the three-reliable-bunch
`GivePawn` response is successfully queued in `pendingReliable`. Allocator backpressure or
a rejected queue therefore consumes neither the per-generation response budget nor its
rate-limit interval.

### 6.2 Client → server RPC (`DecodeInboundActorBunch`)

`ParseIncomingControl` routes ch≥2 bunches to `DecodeInboundActorBunch`
(`ConnectionManager.cpp:887`, dispatch at `:1088-1089`). It reads the field handle
(`SerializeInt(kRoPcMaxHandle=531)`) off a reliable, non-open/close bunch and dispatches by
handle (`RoPcHandleName`, `:871`: 170 SelectTeam, 172 ChangedTeams, 175 SelectRoleByClass,
206 ClientShowTeamSelect, 210 ChangedRole, …).

### 6.3 The UE3 per-parameter "Send" presence bit — DO NOT REINTRODUCE THE BUG

**This is the single most important wire detail in the RPC path.** UE3 serializes
function-call parameters with a per-parameter presence flag (UnScript.cpp
`InternalProcessRemoteFunction:2980-3010`; receive side UnChan.cpp:1628-1640):

- For each **non-bool** parameter: write a 1-bit **"Send" flag** first; the value follows
  **only if Send==1**. `Send==0` means the value equals its default and is **omitted entirely**.
- **Bool** parameters get **NO** presence bit — just their single value bit.

#### Receive bug it caused (team-0 selection)

`SelectTeam(byte TeamID)` (handle 170). When the player picks team 0 (`TeamID==0 ==` the
default), the client sends `Send=0` and **no byte**. The old decoder read the byte raw — which
overflowed past the bunch and silently dropped the team-0 pick. (Team 1 only ever "worked" by
a `&1` masking accident.) The fix (`ConnectionManager.cpp:909-926`):

```cpp
const bool hasTeamId = r.ReadBit();   // the per-param Send flag
uint8_t teamId = 0;                   // Send==0 → default 0 (a VALID selection)
if (hasTeamId) {
    teamId = r.ReadByte();
    if (r.IsOverflowed()) { /* truncated → ignore */ return; }
}
```

#### Send bug it caused (ChangedTeams → role select)

`ChangedTeams(byte TeamIndex, bool bShowRoleSelection, optional Class<GameInfo> GameTypeClass,
optional bool bTeamBalancing, optional bool bShowLobby)` (handle 172) is what the server
sends after SelectTeam to close team-select and open role-select. The old encoder wrote the
byte with **no Send bit** and encoded `GameTypeClass=None` as a `1-bit selector + 10-bit
index` (11 wrong bits) — misaligning the whole bunch so the client mis-decoded ChangedTeams
and never advanced. Correct encoding (`ConnectionManager.cpp:982-1000`):

```cpp
fw.SerializeInt(172, kRoPcMaxHandle);     // handle
const bool sendTeamIdx = (teamId != 0);   // byte TeamIndex (non-bool)
fw.WriteBit(sendTeamIdx);                 //   Send presence bit
if (sendTeamIdx) fw.WriteByte(teamId);    //   value only if Send==1
fw.WriteBit(true);                        // bool bShowRoleSelection (value bit, NO Send bit)
fw.WriteBit(false);                       // Class<GameInfo> GameTypeClass = None == default → Send=0, NO value
fw.WriteBit(false);                       // bool bTeamBalancing (value bit)
fw.WriteBit(false);                       // bool bShowLobby (value bit)
```

**Rule to never reintroduce:** non-bool param ⇒ emit/consume a Send bit, value present only
if Send==1; bool param ⇒ value bit only, no Send bit. This applies symmetrically to both the
encode (`SendCh2Rpc` callers) and decode (`DecodeInboundActorBunch`) sides.

### 6.4 Compound h434 readiness bunches are ordered RPC sequences

`ServerSetReadyToSpawn` (h434) is not always alone. A 2026-07-14 installed
Compound trace carried the exact 41-bit payload `b2d192bd8900`:

```text
h434 Ready(default) + h180 ServerSetThirdPersonSpectate()
  + h434 ForceOnly(explicit) + h275 ServerStopVoiceChat(false)
```

The local capture corpus also grounds a 62-bit variant ending in
`h44 ServerAcknowledgePossession(dynamic ch209) + h284 ServerEnableFocus(false)`
and a 65-bit `h434 NotReady + h89 ServerSetSpectatorLocation(Vector)` variant.
`DeploymentReplication` validates only those exact schemas (plus standalone
h434) before the coordinator mutates. Readiness transitions are applied in wire
order, and deployment is deferred until the full sequence is processed. Thus
the transient leading Ready in the 41/62-bit forms cannot briefly authorize a
spawn before the final ForceOnly revokes it. Unknown companions, true companion
bools, malformed object refs, truncation, and extra bits remain fail-closed.

### 6.5 The SelectTeam → role-select advance (server-side team persist)

When SelectTeam arrives, the server (`:927-1000`):
1. Clamps `teamId` to 0/1 and sets `teamSelected`.
2. **Persists the team server-side** via `TeamManager::AddPlayerToTeam` (RS2 0/1 →
   TeamManager 1/2). Without this the server kept the join-time auto-picked team and a player
   who clicked NVA got the US loadout/spawn (the SelectTeam team-persist bug).
3. Sends **one UNRELIABLE delta on the local PRI channel (ch26)** clearing
   `bWaitingPlayer(31)/bOnlySpectator(32)/bIsSpectator(33)` and binding `Team(35)` →
   the TeamInfo channel (team0→ch76, team1→ch56). Unreliable (bReliable=0, ChSeq=0) so it
   can't re-trigger the reliable-buffer hang. Properties in ascending-handle order.
4. Sends **ChangedTeams** (§6.3) on ch2 to open role select.

Full RPC timeline (chSeq order, capture-verified): `docs/re/pc_ch2_postjoin_timeline.md` and
`MASTER` §6. Minimal causal chain to pop the menu shell:
**ClientShowTeamSelect(206) → ClientGotoState(41)**. ChangedTeams/ClientSetHUD/ClientRestart
are never sent in the menu phase.

---

## 7. Quick reference — where each thing lives

| Concern | Code | RE doc |
|---|---|---|
| Packet/bunch framing | `PacketCodec::Decode/Encode` (PacketCodec.cpp:105/217) | `RS2V_ControlChannel_WireSpec_7258.md` §2–3 |
| MaxPacket / BunchDataBits bound | `PacketCodec.h:38-60`; decode bound `ConnectionManager.cpp:1038` | `MASTER` §0 |
| StatelessConnect + NMT handshake | `HandshakeState.cpp` (handshake `:146`, Hello `:185`, login `CompleteLogin`) | `RS2V_ControlChannel_WireSpec_7258.md` §1; `HandshakeState.h:13-23` |
| Reliable retransmission | `SendReliableBunches`/`OnClientAck`/`RetransmitTick` (ConnectionManager.cpp:792/814/825), called from PumpNetwork:138 | (this doc §3) |
| Ack policy | `ParseIncomingControl:1055-1068`, `:1096-1098` | — |
| PackageMap export | `SendReplicationBootstrap` (:1004) ← FireClientLoggedIn:438 | `RS2V_PostJoin_Replication_7258.md` |
| Actor burst (ch2-first, batched, NMT 0x24) | `SendActorBootstrap` (:656) ← FireClientJoined:461 | `RS2V_PostJoin_Replication_7258.md`, `re/postjoin_packet_timeline.md` |
| Open-bunch layout | `ActorReplication.h` (`WriteActorOpenHeader`/`WriteProp*`) | `re/open_bunch_structure.md`, `MASTER` §2 |
| Handle / maxHandle derivation | `tools/netfields_from_u.ps1` → `tools/netfields_u_<Class>.txt` | `UE3_ClassNetCache_HandleOrder.md`, `MASTER` §5 |
| Object-ref / compressed-vector codecs | `BitReader`/`BitWriter`, `ActorReplication.h` | `MASTER` §2–4, `re/ue3_property_value_codec.md` |
| S2C RPC (ch2) | `SendCh2Rpc` (:851) | `MASTER` §6, `re/pc_ch2_postjoin_timeline.md` |
| C2S RPC + Send bit | `DecodeInboundActorBunch` (:887); ChangedTeams encode (:982) | `MASTER` §6 |

---

## 8. Footguns checklist (read before changing wire code)

1. **Bunch header conditionals**: ChSequence only if `bReliable`; ChType only if
   `bReliable||bOpen`. Wrong condition = whole-datagram bit shift.
2. **Decode at 16384, encode at 12000.** MaxPacket is asymmetric; never decode inbound at the
   server-send bound.
3. **A reliable resend keeps the original ChSequence** (new PacketId only). A new ChSequence
   manufactures a gap → channel stall → soft-lock.
4. **Only ack packets that carried bunches.** Acking acks = infinite ping-pong.
5. **Non-bool RPC params have a Send bit; bools don't.** Send==0 ⇒ value omitted. (§6.3.)
6. **Property/open bunches must end exactly on the last bit** — no trailing pad, or the next
   "handle" is garbage.
7. **Dynamic channel / object-ref bound is 1024 on this build**, not 2048.
8. **ch2 opens standalone and first**; the rest batch under ~11000 bits/packet.
9. **PackageMap goes on ClientLoggedIn (pre-Join); the actor burst on ClientJoined.** Swapping
   the order deadlocks a real client.
