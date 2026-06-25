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

`PacketId` is reconstructed from the 14-bit wire value as a rolling sequence
(`(wire - last - 0x2000) & 0x3fff` delta math against the last received id).
Reader/demux: `UNetConnection::ReceivedPacket` @ `0x1404a4e60`; PreSend (writer) @ `0x14049e4a0`.

## 3. Bunch header

```
Bunch:
    bControl  = ReadBit()
    if bControl:
        bOpen  = ReadBit()
        bClose = ReadBit()
    bReliable = ReadBit()
    ChIndex   = ReadInt(1023)            # MAX_CHANNELS wire bound = 1023 (0x3ff). Control channel = 0.
    if bReliable:
        ChSequence = ReadInt(1024)       # 0x400, delta-decoded ((d - 0x200) & 0x3ff)
    if (bReliable || bOpen):
        ChType = ReadInt(8)              # CHTYPE_MAX = 8. Control channel ChType = 1.
    BunchDataBits = ReadInt(MaxPacket*8) # receive path: ReadInt([conn+0x10c] << 3)
    <payload: BunchDataBits bits>
```

**MaxPacket = 8 during the control handshake** ⇒ `BunchDataBits = ReadInt(64)` ⇒ a control
bunch carries at most 63 data bits (~7 bytes). Large control messages are therefore split
across **many small reliable bunches** and reassembled in `ChSequence` order before the
message is parsed. (MaxPacket almost certainly grows after netspeed negotiation; the large
replication packets later in the capture use larger bunches. Revisit `[conn+0x10c]` for the
replication phase — its static init site was not located, value pinned empirically.)

No `bHasMustBeMappedGUIDs` / `bHasPackageMapExports` / `bPartial` bits exist in this build's
bunch header (the package-map GUID exports live inside the bunch payload, handled by
`UControlChannel`, not in the header).

Writer (mirror): `UChannel::WriteBitsToSendBuffer` @ `0x1404a79d0`.

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
C→S  Hello (opening reliable bunch, ChSeq 1; retransmitted until acked)
C→S  ...continuation reliable bunches (ChSeq 2,3,…) carrying the rest of the Hello body
S→C  ack(client PacketId) + Challenge
C→S  ack + Netspeed
C→S  Login  (large; EOS/Leech auth ticket blob, many bunches)
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
