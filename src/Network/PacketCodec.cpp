// src/Network/PacketCodec.cpp
//
// Implementation of the UE3 packet/bunch framing codec. See PacketCodec.h and
// docs/RS2V_ControlChannel_WireSpec_7258.md.
//
// Terminator handling (spec §2, confirmed against VNGame.exe
// UNetConnection::ReceivedPacket @ 0x1404a4e60 and the handshake capture): a UE3
// packet ends with a single '1' bit immediately after the last ack/bunch entry,
// then zero-pads to a byte boundary. The receiver recovers the readable bit
// count by taking the highest set bit of the packet's LAST byte:
//
//     PacketBits = NumBytes*8 - 8 + HighBit(lastByte)
//
// UE3 sends exactly one packet per datagram and the sender's flush guarantees
// the final byte is non-zero, so that formula is exact for a clean datagram.
//
// The real handshake capture, however, sometimes carries trailing byte(s) after
// the packet's own terminator byte (e.g. the retransmitted Hello datagram is 10
// bytes but the UE3 packet is only 9 - the opening reliable bunch ends at bit
// 64, terminator in byte 8; byte 9 is extra). To stay robust against that (and
// against any datagram-level trailers), Decode finds the terminator as the high
// bit of the last byte and, if the resulting parse overflows or fails to land
// exactly on the terminator, drops the final byte and retries. The longest
// trailing-prefix that yields an exact, overflow-free parse is the packet. The
// stripped trailing bytes are not part of this UE3 packet and are not
// reproduced by Encode (which emits the canonical data + terminator + pad).

#include "Network/PacketCodec.h"

#include "Network/BitReader.h"
#include "Network/BitWriter.h"

namespace PacketCodec {

namespace {

// Highest set bit index within `b` (0..7), or -1 if b == 0.
int HighBit(uint8_t b) {
    for (int i = 7; i >= 0; --i) {
        if ((b >> i) & 1u) {
            return i;
        }
    }
    return -1;
}

// Copy `count` bits from `in` (at its current position) into `out`, LSB-first.
void CopyBits(BitReader& in, BitWriter& out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out.WriteBit(in.ReadBit());
    }
}

// Parse a single packet out of the first `numBytes` bytes of `data`, where the
// terminator is taken as the high bit of byte (numBytes-1). Returns true and
// fills `out` only if the parse lands EXACTLY on the terminator with no
// overflow (i.e. these bytes are a complete, well-formed UE3 packet).
bool TryDecodeExact(const uint8_t* data, size_t numBytes, Packet& out) {
    if (numBytes == 0) {
        return false;
    }
    const int hb = HighBit(data[numBytes - 1]);
    if (hb < 0) {
        return false; // last byte all-zero: not a clean packet boundary
    }
    const size_t terminatorBit = numBytes * 8 - 8 + static_cast<size_t>(hb);

    BitReader r(data, numBytes);
    Packet pkt;
    pkt.packetId = r.ReadInt(static_cast<uint32_t>(kMaxPacketId));
    if (r.IsOverflowed()) {
        return false;
    }

    while (r.BitPos() < terminatorBit) {
        const bool isAck = r.ReadBit();
        if (r.IsOverflowed()) {
            return false;
        }
        if (isAck) {
            const uint32_t ack = r.ReadInt(static_cast<uint32_t>(kMaxPacketId));
            if (r.IsOverflowed()) {
                return false;
            }
            pkt.acks.push_back(ack);
            continue;
        }

        // ----- bunch -----
        Bunch b;
        b.bControl = r.ReadBit();
        if (b.bControl) {
            b.bOpen = r.ReadBit();
            b.bClose = r.ReadBit();
        }
        b.bReliable = r.ReadBit();
        b.chIndex = r.ReadInt(static_cast<uint32_t>(kMaxChannels));
        if (b.bReliable) {
            b.chSequence = r.ReadInt(kMaxChSequence);
        }
        if (b.bReliable || b.bOpen) {
            b.chType = r.ReadInt(kChTypeMax);
        }
        const uint32_t bunchDataBits = r.ReadInt(kBunchDataBitsMax);
        if (r.IsOverflowed()) {
            return false;
        }

        BitWriter payloadWriter;
        CopyBits(r, payloadWriter, bunchDataBits);
        if (r.IsOverflowed()) {
            return false;
        }
        b.payload = payloadWriter.GetBytes();
        b.payloadBits = bunchDataBits;
        pkt.bunches.push_back(std::move(b));
    }

    // Must land exactly on the terminator (no partial entry straddling it).
    if (r.BitPos() != terminatorBit || r.IsOverflowed()) {
        return false;
    }

    pkt.ok = true;
    out = std::move(pkt);
    return true;
}

} // namespace

Packet Decode(const uint8_t* data, size_t numBytes) {
    // Try the full datagram first; if it does not parse to an exact terminator
    // (datagram carries trailing bytes after the packet's own terminator byte),
    // drop trailing bytes and retry. The spec only ever sees a 1-byte trailer, so
    // cap the retry at a small constant instead of walking every length (the old
    // n..1 loop was O(n^2) per datagram and pathological for large non-UE3 frames).
    constexpr size_t kMaxTrailerRetry = 3;
    const size_t lo = (numBytes > kMaxTrailerRetry) ? (numBytes - kMaxTrailerRetry) : 0;
    for (size_t n = numBytes; n > lo; --n) {
        Packet pkt;
        if (TryDecodeExact(data, n, pkt)) {
            return pkt;
        }
    }
    return Packet{}; // ok == false: empty / all-zero / unparseable
}

std::vector<uint8_t> Encode(const Packet& pkt) {
    BitWriter w;

    w.WriteInt(pkt.packetId, static_cast<uint32_t>(kMaxPacketId));

    for (const uint32_t ack : pkt.acks) {
        w.WriteBit(true); // IsAck = 1
        w.WriteInt(ack, static_cast<uint32_t>(kMaxPacketId));
    }

    for (const Bunch& b : pkt.bunches) {
        w.WriteBit(false); // IsAck = 0
        w.WriteBit(b.bControl);
        if (b.bControl) {
            w.WriteBit(b.bOpen);
            w.WriteBit(b.bClose);
        }
        w.WriteBit(b.bReliable);
        w.WriteInt(b.chIndex, static_cast<uint32_t>(kMaxChannels));
        if (b.bReliable) {
            w.WriteInt(b.chSequence, kMaxChSequence);
        }
        if (b.bReliable || b.bOpen) {
            w.WriteInt(b.chType, kChTypeMax);
        }
        w.WriteInt(b.payloadBits, kBunchDataBitsMax);

        // Write the payloadBits payload bits LSB-first from the packed payload.
        BitReader payloadReader(b.payload);
        CopyBits(payloadReader, w, b.payloadBits);
    }

    w.WriteBit(true); // terminator '1' bit; GetBytes() zero-pads to a byte.
    return w.GetBytes();
}

} // namespace PacketCodec
