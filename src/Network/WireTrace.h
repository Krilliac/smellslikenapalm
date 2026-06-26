// src/Network/WireTrace.h
//
// Header-only, reusable wire tracer for UE3 (RS2 EngineVersion 7258) bunches.
//
// PURPOSE: a single drop-in function the ConnectionManager (or any net code) can
// call on every INBOUND and OUTBOUND PacketCodec::Bunch to see exactly what
// crosses the wire. It decodes the bunch framing fields already parsed by
// PacketCodec plus the bunch payload's FIRST property handle - the UE3
// SerializeInt(handle, maxHandle) that prefixes a replicated property/RPC inside
// an actor/object bunch - and emits one structured log line with the channel
// index, flags, sequence, bit count, the decoded first handle, and a hex dump of
// the payload bytes.
//
// This is OBSERVABILITY ONLY. It NEVER mutates the bunch, never touches the wire
// bytes, never throws, and never alters control flow. A malformed/short payload
// is reported (firsthandle=overflow) rather than crashing - it constructs its own
// throwaway BitReader over a COPY of the payload bits, so the caller's decode
// state is untouched.
//
// maxHandleHint is the ClassNetCache maxHandle for the channel's actor class, used
// as the SerializeInt bound for the first handle (e.g. ROPlayerController=531,
// ROGameReplicationInfo=184, ROTeamInfo=78). Pass 0 when unknown/not an actor
// bunch (e.g. a control-channel NMT bunch) to skip handle decoding.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Network/PacketCodec.h"
#include "Network/BitReader.h"
#include "Utils/Logger.h"

namespace WireTrace {

// Lower-case hex dump of the first `maxBytes` bytes backing `payloadBits` bits
// (LSB-first packed, the same layout BitReader/BitWriter use). Appends ".." if
// truncated. Non-mutating; safe on an empty payload.
inline std::string HexDumpPayload(const std::vector<uint8_t>& payload,
                                  uint32_t payloadBits,
                                  size_t maxBytes = 64) {
    static const char* kHex = "0123456789abcdef";
    const size_t fullBytes = static_cast<size_t>((payloadBits + 7u) / 8u);
    // Never read past the actual backing vector even if payloadBits claims more.
    const size_t avail = (fullBytes < payload.size()) ? fullBytes : payload.size();
    const size_t n = (avail < maxBytes) ? avail : maxBytes;
    std::string out;
    out.reserve(n * 2 + 2);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = payload[i];
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    if (n < avail) {
        out.append("..");
    }
    return out;
}

// Decode the FIRST UE3 SerializeInt(handle, maxHandleHint) from a bunch payload
// WITHOUT disturbing anything. Sets `outOverflowed` true (and returns 0) when the
// payload is too short, maxHandleHint is < 2, or there are no payload bits.
inline uint32_t DecodeFirstHandle(const PacketCodec::Bunch& bunch,
                                  uint32_t maxHandleHint,
                                  bool& outOverflowed) {
    outOverflowed = false;
    if (maxHandleHint < 2 || bunch.payloadBits == 0 || bunch.payload.empty()) {
        outOverflowed = true;
        return 0;
    }
    // Read over a private BitReader bounded to the bunch's exact valid bit count
    // so trailing pad bits are treated as overflow rather than decoded.
    BitReader r(bunch.payload.data(), bunch.payload.size(),
                static_cast<size_t>(bunch.payloadBits));
    const uint32_t handle = r.SerializeInt(maxHandleHint);
    if (r.IsOverflowed()) {
        outOverflowed = true;
        return 0;
    }
    return handle;
}

// THE tracer. Logs ONE structured line describing the bunch, e.g.:
//   [WireTrace] OUT ch=54 ctrl=0 open=1 close=0 rel=1 seq=3 type=0 bits=312
//               firsthandle=12 (max=184) hex=8b0c00...
// `dir` is a free-form label ("IN"/"OUT"/"") so the ConnectionManager can mark
// direction. Pure observation; returns nothing and changes no state. Emitted at
// Debug level so it can be silenced via the log level alone.
inline void DumpBunch(const PacketCodec::Bunch& bunch,
                      uint32_t maxHandleHint,
                      const char* dir = "") {
    bool overflowed = false;
    const uint32_t firstHandle = DecodeFirstHandle(bunch, maxHandleHint, overflowed);
    const std::string hex = HexDumpPayload(bunch.payload, bunch.payloadBits);

    if (overflowed) {
        Logger::Debug(
            "[WireTrace] %s ch=%u ctrl=%d open=%d close=%d rel=%d seq=%u type=%u "
            "bits=%u firsthandle=overflow (max=%u) hex=%s",
            (dir && dir[0]) ? dir : "--",
            bunch.chIndex,
            bunch.bControl ? 1 : 0,
            bunch.bOpen ? 1 : 0,
            bunch.bClose ? 1 : 0,
            bunch.bReliable ? 1 : 0,
            bunch.chSequence,
            bunch.chType,
            bunch.payloadBits,
            maxHandleHint,
            hex.c_str());
    } else {
        Logger::Debug(
            "[WireTrace] %s ch=%u ctrl=%d open=%d close=%d rel=%d seq=%u type=%u "
            "bits=%u firsthandle=%u (max=%u) hex=%s",
            (dir && dir[0]) ? dir : "--",
            bunch.chIndex,
            bunch.bControl ? 1 : 0,
            bunch.bOpen ? 1 : 0,
            bunch.bClose ? 1 : 0,
            bunch.bReliable ? 1 : 0,
            bunch.chSequence,
            bunch.chType,
            bunch.payloadBits,
            firstHandle,
            maxHandleHint,
            hex.c_str());
    }
}

} // namespace WireTrace
