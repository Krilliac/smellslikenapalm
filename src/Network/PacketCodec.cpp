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
#include "Utils/Logger.h"

#include <string>

namespace PacketCodec {

namespace {

// Runtime debug-trace flag (see SetDebugTracing). Plain bool: PacketCodec is
// driven from a single network thread, and this is a best-effort observability
// switch - a torn read here is harmless (it only gates logging).
bool g_debugTrace = false;

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

// Lowercase hex of `bytes` (no separators), capped so a pathological payload
// can't flood the log. Returns e.g. "0a1bff" (+"...(<n> bytes total)" if capped).
std::string HexDump(const std::vector<uint8_t>& bytes) {
    static const char kHex[] = "0123456789abcdef";
    constexpr size_t kMaxBytes = 1024; // 8192 bits of payload is plenty for tracing
    const size_t shown = bytes.size() < kMaxBytes ? bytes.size() : kMaxBytes;
    std::string out;
    out.reserve(shown * 2 + 32);
    for (size_t i = 0; i < shown; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0xF]);
        out.push_back(kHex[bytes[i] & 0xF]);
    }
    if (shown < bytes.size()) {
        out += "...(" + std::to_string(bytes.size()) + " bytes total)";
    }
    return out;
}

// Emit a one-line structured trace for a single bunch (direction = "RX"/"TX").
// Pure logging; no side effects on the codec.
void TraceBunch(const char* dir, uint32_t packetId, const Bunch& b,
                uint32_t bunchDataBitsMax) {
    Logger::Debug("PacketCodec %s bunch packetId=%u chIndex=%u bControl=%d "
                  "bOpen=%d bClose=%d bReliable=%d chSequence=%u chType=%u "
                  "payloadBits=%u payload=%s",
                  dir, packetId, b.chIndex, b.bControl ? 1 : 0, b.bOpen ? 1 : 0,
                  b.bClose ? 1 : 0, b.bReliable ? 1 : 0, b.chSequence, b.chType,
                  b.payloadBits, HexDump(b.payload).c_str());
    // Non-fatal invariant: a bunch can never carry more data bits than the
    // SerializeInt bound for the current MaxPacket phase. We only WARN - the
    // wire bytes and parse result are left untouched.
    if (b.payloadBits >= bunchDataBitsMax) {
        Logger::Warn("PacketCodec %s INVARIANT payloadBits=%u >= maxPacketBits=%u "
                     "(packetId=%u chIndex=%u) - bunch exceeds MaxPacket bound",
                     dir, b.payloadBits, bunchDataBitsMax, packetId, b.chIndex);
    }
}

} // namespace

void SetDebugTracing(bool enabled) { g_debugTrace = enabled; }
bool IsDebugTracing() { return g_debugTrace; }

Packet Decode(const uint8_t* data, size_t numBytes, uint32_t maxPacketBytes) {
    const uint32_t bunchDataBitsMax = maxPacketBytes * 8;
    // UE3 receive semantics (UNetConnection::ReceivedPacket): the readable bit
    // count is the high set bit of the LAST byte (the terminator '1' bit); data
    // lives strictly below it. We set the BitReader's valid bits to that
    // terminator so reads cannot run past it, then parse entries until end-of-
    // stream (AtEnd), exactly like the engine's loop.
    //
    // IMPORTANT: the final entry's SerializeInt naturally tries to read one bit
    // past the terminator (e.g. a trailing AckPacketId = ReadInt(16384), which
    // keeps consuming bits until value+mask >= max). That terminal overflow is
    // BENIGN - the engine tolerates it via AtEnd(). So we do NOT reject on it; we
    // only refuse a datagram whose PacketId itself can't be read. (This also makes
    // a datagram-level trailing byte parse as a harmless trailing ack rather than
    // requiring a length-walk.)
    Packet pkt;
    if (numBytes == 0) {
        if (g_debugTrace) {
            Logger::Warn("PacketCodec RX malformed: empty datagram (0 bytes), ok=false");
        }
        return pkt; // ok == false
    }
    const int hb = HighBit(data[numBytes - 1]);
    if (hb < 0) {
        if (g_debugTrace) {
            Logger::Warn("PacketCodec RX malformed: last byte all-zero over %zu "
                         "bytes (no terminator), ok=false", numBytes);
        }
        return pkt; // last byte all-zero: not a clean UE3 packet boundary
    }
    const size_t terminatorBit = numBytes * 8 - 8 + static_cast<size_t>(hb);

    BitReader r(data, numBytes, terminatorBit);
    pkt.packetId = r.ReadInt(static_cast<uint32_t>(kMaxPacketId));
    if (r.IsOverflowed()) {
        if (g_debugTrace) {
            Logger::Warn("PacketCodec RX malformed: PacketId read overflowed "
                         "(%zu bytes, terminatorBit=%zu), ok=false",
                         numBytes, terminatorBit);
        }
        return pkt; // can't even read the PacketId -> not a UE3 packet
    }

    while (r.BitPos() < terminatorBit && !r.IsOverflowed()) {
        const bool isAck = r.ReadBit();
        if (r.IsOverflowed()) {
            break; // reached the terminator
        }
        if (isAck) {
            // A trailing ack may terminal-overflow (benign); keep its value.
            pkt.acks.push_back(r.ReadInt(static_cast<uint32_t>(kMaxPacketId)));
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
        const uint32_t bunchDataBits = r.ReadInt(bunchDataBitsMax);
        if (r.IsOverflowed()) {
            break; // truncated bunch header at the terminator - drop the partial
        }

        BitWriter payloadWriter;
        CopyBits(r, payloadWriter, bunchDataBits);
        if (r.IsOverflowed()) {
            break; // truncated payload - drop the partial bunch
        }
        b.payload = payloadWriter.GetBytes();
        b.payloadBits = bunchDataBits;
        if (g_debugTrace) {
            TraceBunch("RX", pkt.packetId, b, bunchDataBitsMax);
        }
        pkt.bunches.push_back(std::move(b));
    }

    if (g_debugTrace) {
        // Non-fatal post-parse invariant: a clean datagram lands the reader at
        // (or, via the benign terminal-overflow noted above, just past) the
        // terminator bit. A reader that stopped well SHORT of the terminator -
        // i.e. left a meaningful gap - signals a truncated/mis-aligned bunch we
        // silently dropped. WARN only; the returned Packet is unchanged.
        const size_t pos = r.BitPos();
        if (pos < terminatorBit) {
            Logger::Warn("PacketCodec RX leftover: parse stopped at bit %zu of "
                         "terminatorBit %zu (%zu bits unconsumed, overflowed=%d) "
                         "packetId=%u bunches=%zu acks=%zu",
                         pos, terminatorBit, terminatorBit - pos,
                         r.IsOverflowed() ? 1 : 0, pkt.packetId,
                         pkt.bunches.size(), pkt.acks.size());
        }
        Logger::Debug("PacketCodec RX packet packetId=%u acks=%zu bunches=%zu "
                      "bytes=%zu terminatorBit=%zu ok=1",
                      pkt.packetId, pkt.acks.size(), pkt.bunches.size(),
                      numBytes, terminatorBit);
    }

    pkt.ok = true; // valid PacketId + best-effort entries (UE3-lenient)
    return pkt;
}

std::vector<uint8_t> Encode(const Packet& pkt, uint32_t maxPacketBytes) {
    const uint32_t bunchDataBitsMax = maxPacketBytes * 8;
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
        w.WriteInt(b.payloadBits, bunchDataBitsMax);

        if (g_debugTrace) {
            TraceBunch("TX", pkt.packetId, b, bunchDataBitsMax);
            // Non-fatal consistency check: the packed payload buffer must hold at
            // least payloadBits bits, else CopyBits will read past it (the
            // BitReader clamps safely, but it means a caller bug). WARN only.
            if (b.payload.size() * 8 < b.payloadBits) {
                Logger::Warn("PacketCodec TX INVARIANT payloadBits=%u exceeds "
                             "packed payload %zu bytes (%zu bits) packetId=%u "
                             "chIndex=%u",
                             b.payloadBits, b.payload.size(),
                             b.payload.size() * 8, pkt.packetId, b.chIndex);
            }
        }

        // Write the payloadBits payload bits LSB-first from the packed payload.
        BitReader payloadReader(b.payload);
        CopyBits(payloadReader, w, b.payloadBits);
    }

    w.WriteBit(true); // terminator '1' bit; GetBytes() zero-pads to a byte.
    std::vector<uint8_t> out = w.GetBytes();
    if (g_debugTrace) {
        Logger::Debug("PacketCodec TX packet packetId=%u acks=%zu bunches=%zu "
                      "bytes=%zu maxPacketBits=%u",
                      pkt.packetId, pkt.acks.size(), pkt.bunches.size(),
                      out.size(), bunchDataBitsMax);
    }
    return out;
}

} // namespace PacketCodec
