# RS2 / UE3 Control-Channel Wire Spec (EngineVersion 7258)

Authoritative bit-level spec for the Rising Storm 2: Vietnam dedicated-server network
protocol, reversed from `VNGame.exe` (UE3, EngineVersion 7258, EGS/EOS "Leech" build)
and cross-validated against a real client↔server handshake capture
(`D:\RE-Tools\rs2_handshake_capture.pcapng`).

Confidence: **HIGH** for framing + Hello body (decompiled + whole-frame validated);
**MEDIUM** for non-Hello message bodies (handler addresses confirmed, field lists read
from the asm but not all re-verified against capture).

All multi-byte values are little-endian. All bit IO is **LSB-first within each byte**.

---

## 1. Bit primitives

`ReadInt(Max)` / `WriteInt(v,Max)` — UE3 variable-length bounded int, LSB-first:

```
ReadInt(Max):
    Value = 0
    for (Mask = 1; (Value + Mask) < Max && !Error; Mask <<= 1):
        if ReadBit(): Value |= Mask
    return Value
```

The number of bits consumed depends on `Max` (and the value). Our `BitReader::SerializeInt`
already implements this exactly. Fixed-width primitives (`ReadByte`=8b, `ReadInt32`=32b,
`ReadUInt64`=64b, `ReadFloat`=32b) read raw little-endian bits at the current bit position
(NOT byte-aligned). `FString` = `int32 Length` then chars; Length>0 ⇒ ANSI (Length bytes
incl. NUL); Length<0 ⇒ UCS-2/UTF-16LE (−Length code units incl. NUL).

Key binary addrs: ReadInt `0x140079850`/`0x1400752a0`; ReadBit `0x140075270`;
FString `0x140095fc0`.

## 2. Packet layout

```
Packet:
    PacketId   = ReadInt(16384)          # MAX_PACKETID = 16384 (14 bits)
    loop until terminator:
        IsAck = ReadBit()
        if IsAck: AckPacketId = ReadInt(16384)        # ack of a received PacketId; nothing else
        else:     <Bunch>                             # see §3
    # terminator: a single '1' bit after the last entry, then zero-pad to byte boundary.
    # (An empty packet = PacketId + terminator only => the 2-byte "XX40" keepalive/ack packets.)
```

**Terminator (corrected from capture/disasm):** UE3 sends exactly ONE packet per
datagram and the readable bit count is recovered as the **high set bit of the
packet's LAST byte** (`PacketBits = NumBytes*8 - 8 + HighBit(lastByte)`), not the
highest set bit across the whole buffer. The sender's flush guarantees a non-zero
final byte. Captured datagrams sometimes carry a constant trailing byte AFTER the
packet's terminator byte (e.g. the 10-byte retransmitted Hello datagram carries a
9-byte packet); the decoder treats the longest leading prefix that parses exactly
to its last-byte terminator (overflow-free) as the packet and ignores any trailer.

`PacketId` is monotonic inside the connection and only its low 14 bits are written.
On receive, UE3 reconstructs the full value relative to the current PacketId/ACK
reference with `MakeRelative` half-range semantics. The emulator applies the same
rule to outbound ACKs, rejects pre-session/future/exact-half ambiguous values without
moving its reference, and tracks the actual peer-ACK high-water separately. Bunch-bearing
builders stop before the exact 8192-packet ambiguous distance. Because UE3 does not ACK
bunch-less ACK-only/keepalive packets, the emulator locally retires those identities only
while every live retry ledger has a newer attempt inside the half-range. Retry ledgers
match the reconstructed full serial, not the repeating raw wire value.
Reader/demux: `UNetConnection::ReceivedPacket` @ `0x1404a4e60`; PreSend (writer) @ `0x14049e4a0`.

## 3. Bunch header (7258 EOS) — DEFINITIVE

This is the **complete and exact** bunch header for this build. There are **NO**
`bPartial` / `bPartialInitial` / `bPartialFinal` / `bHasMustBeMappedGUIDs` /
`bHasServerPackageMapAcks` / `bIsReplicationPaused` bits. The header is read inline in
`UNetConnection::ReceivedPacket` and written inline in the bunch flusher; the two are
exact mirrors. (Confidence: **HIGH** — both read and write paths disassembled, and both
small and large captured bunches decode cleanly under this layout and ONLY this layout.)

```
Bunch (read order, LSB-first):
    bControl  = ReadBit()                # == (bOpen || bClose); see writer note below
    if bControl:
        bOpen  = ReadBit()
        bClose = ReadBit()
    # (if !bControl, bOpen and bClose are forced 0)
    bReliable = ReadBit()
    ChIndex   = ReadInt(1023)            # MAX_CHANNELS wire bound = 1023 (0x3ff). Control channel = 0.
    if bReliable:
        ChSequence = ReadInt(1024)       # low 10 bits; MakeRelative(raw, InReliable[ChIndex], 1024)
    if (bReliable || bOpen):
        ChType = ReadInt(8)              # CHTYPE_MAX = 8. Control channel ChType = 1.
    BunchDataBits = ReadInt(MaxPacket*8) # receive path: ReadInt([conn+0x10c] << 3)
    <payload: BunchDataBits bits>        # then copied into the FInBunch buffer
```

### Disassembly proof (authoritative)

**READ** — `UNetConnection::ReceivedPacket` @ `0x1404a4e60` (1496 bytes, .text
`0x1404a4e60`–`0x1404a5438`). The per-bunch header read is inline at `0x1404a50ca`–
`0x1404a5194`. `ReadBit` = `0x140075270`; `ReadInt(Max)` = `0x1400752a0`
(thunks vtable `SerializeInt` at `[reader]+0x18`).

| asm addr | op | field |
|----------|----|-------|
| `0x1404a50e3` `call 0x140075270`; `test al,al` @ `0x1404a50ec` | ReadBit | **bControl** |
| `0x1404a50f3` ReadBit → `[rbp-0x24]` | ReadBit (if bControl) | **bOpen** |
| `0x1404a50fe` ReadBit → `[rbp-0x23]` | ReadBit (if bControl) | **bClose** |
| `0x1404a5108` `mov word [rbp-0x24], 0` | — | bOpen/bClose = 0 when !bControl |
| `0x1404a5111` ReadBit → `[rbp-0x22]` | ReadBit | **bReliable** |
| `0x1404a5121` `mov edx,0x3ff; call 0x1400752a0` → `[rbp-0x30]` | ReadInt(1023) | **ChIndex** |
| `0x1404a5140` `mov edx,0x400; call …` then `-ebx -0x200 &0x3ff +…` | ReadInt(1024) (if bReliable) | **ChSequence** (delta) |
| `0x1404a517b` `mov edx,8; call …` → `[rbp-0x2c]` | ReadInt(8) (if bReliable\|\|bOpen) | **ChType** |
| `0x1404a5183` `mov edx,[rdi+0x10c]; shl edx,3; call 0x1400752a0` → ebx | ReadInt([conn+0x10c]<<3) | **BunchDataBits** |
| `0x1404a51b4` `call 0x14007b640` (r8=BunchDataBits, rcx=FInBunch) | — | grow buffer + copy payload bits |

The next field read immediately after BunchDataBits is the **payload copy**
(`0x14007b640` is a buffer-grow + bit-copy routine: sets `[FInBunch+0x94]=numBits`,
computes `(bits+7)>>3`, copies). There is no flag read between ChType and BunchDataBits,
and none between BunchDataBits and the payload. The per-bunch `FInBunch` ctor
(`0x14046d870`, called once at `0x1404a50d2`) reads nothing from the wire.

**WRITE** — bunch-header serializer @ `0x1404a79d0` (the "WriteBitsToSendBuffer" /
flush path). `WriteBit` = `0x14007c390`; `WriteInt(v,Max)` = `0x14007c3e0`. It writes,
in this exact order, mirroring the read — FOutBunch field offsets in brackets:

| asm addr | op | field |
|----------|----|-------|
| `0x1404a7a44` WriteBit dl where `dl = ([+0xc8] \|\| [+0xc9])` | WriteBit | **bControl** = bOpen\|bClose (derived, not stored) |
| `0x1404a7a66` WriteBit `[rdi+0xc8]` | WriteBit (if bControl) | **bOpen** |
| `0x1404a7a77` WriteBit `[rdi+0xc9]` | WriteBit (if bControl) | **bClose** |
| `0x1404a7a88` WriteBit `[rdi+0xca]` | WriteBit | **bReliable** |
| `0x1404a7a9e` `r8=0x3ff` WriteInt `[rdi+0xb8]` | WriteInt(1023) | **ChIndex** |
| `0x1404a7abd` `r8=0x400` WriteInt `[rdi+0xc0]` (if `[+0xca]`) | WriteInt(1024) (if bReliable) | **ChSequence** |
| `0x1404a7ae5` `r8=8` WriteInt `[rdi+0xbc]` (if `[+0xca]\|\|[+0xc8]`) | WriteInt(8) (if bReliable\|\|bOpen) | **ChType** |
| `0x1404a7b00` `r8=[rsi+0x10c]<<3` WriteInt `[rdi+0x94]` | WriteInt(MaxPacket*8) | **BunchDataBits** |
| (then) payload bits | — | bunch payload |

The writer writes **exactly** these eight fields then the payload — no partial/extra
flags. Read and write are bit-for-bit symmetric. FOutBunch layout: `+0x94`=DataBits,
`+0xb8`=ChIndex, `+0xbc`=ChType, `+0xc0`=ChSequence, `+0xc8`=bOpen, `+0xc9`=bClose,
`+0xca`=bReliable.

### `bControl` is derived, not a stored flag

On the wire, the first bit is `bControl = (bOpen || bClose)`. The engine stores bOpen
and bClose separately and emits `bControl` as their OR. On read, a set `bControl`
gates reading bOpen/bClose; a clear `bControl` forces both to 0. This matches our
`Bunch.bControl` field and the existing codec — no change needed there.

### BunchDataBits bound — the resolved point of confusion

`BunchDataBits = ReadInt([conn+0x10c] << 3)` where `[conn+0x10c]` = **MaxPacket bytes**.
`[conn+0x10c]` is negotiated (set from several register-sourced stores near the
connection-init code, e.g. `0x14049751f`, `0x1404a167b`; no constant immediate — it is
**not** a fixed `8`). Early small-frame analysis used 16384 as a compatible bound, but
those frames did not pin the exact value. Saturated traffic later resolved the production
directions: retail C2S uses MaxPacket 1280 (bound 10240) and dedicated-server S2C uses
the 1500-byte MTU choice (bound 12000, within the capture-compatible interval). Both
apply from the first packet; there is no small-bound handshake phase.

### Capture bit-evidence (cross-check; the "partial flags" hypothesis is REFUTED)

Decoder run against `rs2_handshake_capture.pcapng` S2C frames. "consumes_all" = parse
ends exactly on the last-byte terminator. "npart=N" = N speculative partial-flag bits
inserted between ChType and BunchDataBits.

- **f165 (Challenge, NMT 0x03)** — `rint(64)`: garbage (err, 4 phantom bunches).
  `rint(12000)` npart=0: **one** bunch `ch0 sq4 ct1 bd=144 NMT=0x03`,
  payload `03 5a1c0000 09000000 30 32 35 34 37 43 35 38 00` = `int32, FString` (Challenge
  body), clean. (Confidence **HIGH**.)
- **f162 (Welcome, control handshake)** — `rint(12000)` npart=0: one bunch
  `ch0 sq3 ct1 bd=80`, clean; `rint(64)` produces phantom bunches. Proves the large
  bound is in force even **during** the handshake. (Confidence **HIGH**.)
- **f167 (PackageMap export, ~10 000 bits)** — `rint(12000)` **npart=0**: **one** bunch,
  `ch0 sq5 ct1 bd=10032 NMT=0x07`, err=False, consumes_all=True, and the payload is a
  clean UE3 package-name table: ASCII `Core`, `None`, `Engine`, `ROGame`,
  `GameFramework`, … With **npart=5** the same frame parses to err=True / 6 phantom
  bunches and pure noise (`pzs+`, `(r;Ks+`). The clean decode is the **no-partial-flags**
  one; inserting 5 partial bits **destroys** it. (Confidence **HIGH**.)
- **f185** — `rint(12000)` npart=0: single bunch with `bd=5736`, consumes_all=True,
  and no trailing bits. `npart=5`: garbage. (Confidence **HIGH**.)
- **f160** — pure ack burst: PacketId then 133× `IsAck=1, AckPacketId=ReadInt(16384)`
  (acks 0..132), no bunches. Confirms the ack path and the layout under load.
  (Confidence **HIGH**.)

**Reconciliation.** The prior "PackageMap frames are UE3 PARTIAL bunches / +5-bit shift"
conclusion was an artifact of the `mock_client.py` `rint(64)` bug. With the wrong Max,
`ReadInt` consumes ~6 bits for BunchDataBits instead of ~14, so every large bunch's
length and payload were misaligned; the analyst recovered a readable payload by adding
~5 fudge bits, which coincidentally re-aligned ONE frame. The actual fix is the correct
direction-specific `BunchDataBits` bound. A 16384 analysis bound happened to decode these
particular S2C values because it selected the same SerializeInt widths; it was not proof
of MaxPacket 2048. With the resolved bound, small handshake bunches, NMT bunches, and the
large PackageMap bunches decode cleanly with **zero** partial flags. The package-map
GUID/name exports are payload content handled by `UControlChannel`, not header bits.

### Flag values by bunch class

| Bunch class | bControl | bOpen | bClose | bReliable | partial flags |
|-------------|:--------:|:-----:|:------:|:---------:|:-------------:|
| (a) handshake opening (Hello/Challenge/Welcome) | 1 (open) | 1 | 0 | 1 | none exist |
| (b) subsequent small NMT bunches | 0 | 0 | 0 | 1 | none exist |
| (c) large PackageMap export (f167…f185) | 0 | 0 | 0 | 1 | none exist |
| (d) actor-channel open | 1 | 1 | 0 | 1 | none exist |

There is no `bPartial` concept on the wire in 7258 EOS. Every observed retail control
message fits in one correctly bounded reliable bunch. If future evidence exposes a
logical stream spanning bunches, its boundary must come from channel/message semantics;
there are no `bPartialInitial/bPartial/bPartialFinal` bits to chain it. Do not infer such a
stream from the header alone.

Writer (mirror): bunch-header serializer @ `0x1404a79d0`.

## 4. Control messages (NMT)

After reassembly, the control-channel bunch stream is parsed by
`UControlChannel::ReceivedBunch` (loop @ `0x140488de0`, switch @ `0x14048935e`, jump table
@ `0x1404895fc`, 37 cases 0x00–0x24). The **message type is an 8-bit byte**, read first,
then the per-NMT body:

| NMT | Name (our enum) | Dir | Body (on-wire order) | Handler |
|----:|-----------------|-----|----------------------|---------|
| 0x00 | Hello | C→S | `int32 MinVer, int32 Ver, QWORD SteamId, FString, FString` (endian-aware) | 0x140475e20 |
| 0x01 | Welcome | S→C | `FString, FString, QWORD` | 0x140475890 |
| 0x02 | Upgrade | S→C | `int32, int32` | 0x140475980 |
| 0x03 | Challenge | S→C | `int32, FString` | 0x140475a30 |
| 0x04 | Netspeed | C→S | `int32` | 0x140476520 |
| 0x05 | Login | C→S | `FString, FString, QWORD` | 0x140475ae0 |
| 0x06 | Failure | S→C | `FString` | 0x1404761b0 |
| 0x07 | (Join-family) | C→S | `int32 ×4, FString, FString` | 0x140475b70 |
| 0x08 | (Join GUID rebind) | C→S | `int32 ×4 (+conditional int32)` | 0x140475d00 |
| 0x0a | | | `QWORD, FString` | 0x140475dc0 |
| 0x10 | (Steam login) | C→S | `int32, int32, QWORD` | 0x140475f80 |
| 0x16 | | | `FString, FString` | 0x1404761f0 |
| 0x0f/0x13/0x15/0x1d/0x1e/0x1f/0x24 | | | `int32` | 0x140476520 |
| 0x14/0x21/0x22 | | | `FString` | 0x1404761b0 |

Notes:
- The NMT **values** match our `NetMessages.h` (Hello=0 … Failure=6); some **bodies** differ
  from prior codec assumptions — notably Welcome (`FStr,FStr,QWORD`, not 3×FStr),
  Challenge (`int32,FStr`, not just FStr), and Login (`FStr,FStr,QWORD`).
- The Hello body **matches** the existing `ControlChannel::HelloMessage` layout.

## 5. Handshake sequence (from capture)

```
C→S  Hello (one opening reliable bunch, ChSeq 1; retransmitted until acked)
S→C  ack(client PacketId) + Challenge
C→S  ack + Netspeed
C→S  Login  (large; EOS/Leech auth ticket blob in one MaxPacket-bounded bunch)
S→C  Welcome + NetGUID/PackageMap
…    world/actor replication (join complete)
```

**Critical:** the server MUST ack received packets. A real client retransmits its Hello
opening bunch indefinitely until acked (observed ~30× in the capture before the server
responded). The emulator's outbound path must send acks and assign PacketId/ChSequence.

## 6. Emulator integration

- Inbound: replace the provisional `kProvisionalBunchHeaderOffset=0` in
  `ConnectionManager::ParseIncomingControl` with a real packet→ack→bunch decoder that
  reassembles reliable control bunches by ChSequence and dispatches the NMT byte + body to
  the handshake state machine. Payload is bit-aligned (not byte) — share one `BitReader`.
- Outbound: wrap message payloads in bunch + packet framing (PacketId, ChSequence, the §3
  header, BunchDataBits) and emit acks for received packets.
- Validate with the captured frames as byte-exact fixtures (see §5 frame ids).

### 6.1 Current `PacketCodec` requirements (post-Join replication)

Good news: the §3 disassembly confirms the bunch **header layout is exactly what
`PacketCodec::Decode`/`Encode` already implement** — `bControl, [bOpen,bClose], bReliable,
ChIndex(1023), [ChSequence(1024)], [ChType(8)], BunchDataBits` and payload, in that order.
There are **no partial/extra flag bits to add**. The current implementation must preserve
these three requirements:

1. **Direction-specific BunchDataBits bound.** Both `Decode` and `Encode` take
   `maxPacketBytes` and use `bunchDataBitsMax = maxPacketBytes * 8` for
   `r.ReadInt(bunchDataBitsMax)` / `w.WriteInt(b.payloadBits, bunchDataBitsMax)`. This is
   **correct** and matches `ReadInt([conn+0x10c]<<3)`. Live callers pass
   `kClientSendMaxPacketBytes=1280` for C2S decode and
   `kServerSendMaxPacketBytes=1500` for S2C encode from the first packet. The legacy
   8-byte default exists only for low-level historical fixtures.
   - No struct/field changes to `Bunch` or `Packet`; no new header bits to serialize.

2. **No invented partial-bunch flags.** Do NOT add `bPartial*` handling. Every observed
   retail C2S control message fits in one correctly bounded reliable bunch, and the current
   `ControlReassembler` orders/deduplicates those bunches before dispatching one payload per
   callback. A future general `UControlChannel` continuous-stream parser would need explicit
   message delimiting and evidence; the current class does not claim cross-bunch logical
   message concatenation.

3. **Keep the benign terminal-overflow tolerance.** `Decode` already stops on
   `IsOverflowed()` at the terminator and still returns `ok`. The large frames (f185) rely
   on this — do not tighten it.

The resolved implementation therefore needs direction-correct call sites, strict
per-channel reliable ordering, and the existing no-extra-flags header — no new flag
plumbing.

### 6.2 Reliable ChSequence ownership and wrap

`ChSequence` is **per channel**, not connection-global. UE3 stores
`InReliable[MAX_CHANNELS]` and `OutReliable[MAX_CHANNELS]`; the receive path applies
`MakeRelative(raw, InReliable[ChIndex], MAX_CHSEQUENCE)` before `UChannel` accepts only
the exact `InReliable[ChIndex] + 1` successor. Later same-channel bunches are buffered
strictly until the missing reliable arrives. A pending-count heuristic must not skip a
gap: the corrected C2S `MaxPacket=1280` decode removed the phantom channels and apparent
ch0 holes that originally motivated that workaround.

On the wire this is a modulo-1024 value (`1023 -> 0`). Emulator ordering therefore uses
the equivalent half-range relation: distances `1..511` are newer, distance `512` is on
the older side exactly like UE3 `MakeRelative`, and values outside `0..1023` are invalid.
The sender's `RELIABLE_BUFFER=128` limits ordinary forward successors to `1..127`.
`PacketAssembler` applies that same bound to outbound ch0: ACKed successors remain
issuance-window tombstones behind an older gap, and a full window rejects a new logical
control message without consuming its `PacketId`, `ChSequence`, or queued ACKs. The caller
must never issue sequence 128 past the gap. `ConnectionManager` retains required messages
in a bounded FIFO (127 messages, 256 KiB) and flushes them in protocol order when contiguous
ACK progress reopens capacity. It fails the session closed if that queue is exhausted, the
complete encoded packet exceeds S2C MaxPacket, or the prepare/ledger/commit transaction
cannot be completed consistently. An ACK-free full-packet preflight occurs before queueing,
so an impossible payload never waits behind backpressure. Join completion, handshake
visibility, actor bootstrap, and the Game callback also wait for this earlier ch0 work to
drain; the bootstrap then publishes ch2 OPEN and NMT 0x24 as consecutive reliable
bunches under one PacketId/retry ledger before later actors.
