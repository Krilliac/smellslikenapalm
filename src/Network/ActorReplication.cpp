// src/Network/ActorReplication.cpp
// See ActorReplication.h.

#include "Network/ActorReplication.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "Utils/Logger.h"

// ---------------------------------------------------------------------------
// Non-fatal encoder self-checks (observability layer).
//
// When RS2V_ACTORREPL_SELFCHECK is enabled (default), every encoding primitive
// validates its own bit-stream invariants and reads back what it just wrote with
// a BitReader, logging (NOT throwing) on any mismatch. This catches encoder bugs
// such as the None-object-ref mis-encode that corrupted ChangedTeams BEFORE the
// bytes reach the real client (where the symptom is an opaque channel hang).
//
// Everything here is side-effect-free w.r.t. the wire: it only inspects bits the
// caller already committed. Disable by defining RS2V_ACTORREPL_SELFCHECK=0.
// ---------------------------------------------------------------------------
#ifndef RS2V_ACTORREPL_SELFCHECK
#define RS2V_ACTORREPL_SELFCHECK 1
#endif

namespace ActorRepl {

namespace {
#if RS2V_ACTORREPL_SELFCHECK
// Advance a fresh reader (positioned at bit 0) to absolute bit offset `startBit`
// by reading-and-discarding. Returns false if the reader overflowed getting there
// (which itself indicates a length bug worth logging).
bool AdvanceReader(BitReader& r, size_t startBit) {
    while (startBit >= 64) { r.ReadBits(64); startBit -= 64; }
    if (startBit) r.ReadBits(static_cast<int>(startBit));
    return !r.IsOverflowed();
}
#endif
} // namespace

void WriteNetGUID(BitWriter& w, const NetGUIDRef& ref) {
    const uint32_t max = ref.isDynamic ? kDynamicChannelMax : kStaticObjectMax;
#if RS2V_ACTORREPL_SELFCHECK
    const size_t startBit = w.NumBits();
    // Invariant: index must be representable in [0, max). A dynamic ref past
    // MAX_CHANNELS, or a static index with the top bit set, would be silently
    // truncated by SerializeInt and resolve to the wrong object on the client.
    if (ref.index >= max) {
        Logger::Warn("ActorRepl::WriteNetGUID: index %u out of range [0,%u) (%s) - "
                     "will mis-encode object ref",
                     ref.index, max, ref.isDynamic ? "dynamic/channel" : "static/object");
    }
    // The None-objref class of bug: per UE3 UPackageMapLevel::SerializeObject
    // (UnNetDrv.cpp), a NULL/None reference is the DYNAMIC index-0 form (selector bit 1 +
    // SerializeInt(0, MAX_CHANNELS), Index<=0 => None). So the actual mis-encode is a
    // STATIC (isDynamic=false) index-0 ref - a static object index 0 is the package's
    // first export, never None. Flag THAT.
    if (!ref.isDynamic && ref.index == 0) {
        Logger::Warn("ActorRepl::WriteNetGUID: static ref to object index 0 - likely a "
                     "mis-encoded None/NULL (None is the dynamic index-0 form)");
    }
#endif
    w.WriteBit(ref.isDynamic);  // flag: 1=dynamic (channel index), 0=static (object index)
    w.SerializeInt(ref.index, max);
#if RS2V_ACTORREPL_SELFCHECK
    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    if (AdvanceReader(r, startBit)) {
        NetGUIDRef back = ReadNetGUID(r);
        if (r.IsOverflowed() || back.isDynamic != ref.isDynamic || back.index != ref.index) {
            Logger::Warn("ActorRepl::WriteNetGUID: round-trip mismatch wrote(%s,%u) "
                         "read(%s,%u)%s",
                         ref.isDynamic ? "dyn" : "stat", ref.index,
                         back.isDynamic ? "dyn" : "stat", back.index,
                         r.IsOverflowed() ? " [overflow]" : "");
        }
    }
#endif
}

NetGUIDRef ReadNetGUID(BitReader& r) {
    NetGUIDRef ref;
    ref.isDynamic = r.ReadBit();
    ref.index = r.SerializeInt(ref.isDynamic ? kDynamicChannelMax : kStaticObjectMax);
    return ref;
}

namespace {
// appCeilLogTwo: smallest n with 2^n >= v. CeilLogTwo(1)=0, (2)=1, (3)=2, (4)=2.
uint32_t CeilLogTwo(uint32_t v) {
    if (v <= 1) return 0;
    uint32_t n = 0, x = v - 1;
    while (x > 0) { x >>= 1; ++n; }
    return n;
}
uint32_t AbsI(int32_t v) { return v < 0 ? static_cast<uint32_t>(-(int64_t)v) : static_cast<uint32_t>(v); }
// UE3 appRound (UnVcWin32.h) rounds half toward +inf == floor(f+0.5), NOT half-away-from-
// zero. They differ only at exact negative half-integers (appRound(-0.5)=0, ours was -1) -
// but a 1-unit difference shifts the compressed-vector bit width. Match UE3 exactly.
int32_t RoundToInt(float f) { return static_cast<int32_t>(std::floor(f + 0.5f)); }
} // namespace

void WriteCompressedVector(BitWriter& w, float x, float y, float z) {
    const int32_t ix = RoundToInt(x), iy = RoundToInt(y), iz = RoundToInt(z);
    uint32_t maxc = AbsI(ix);
    if (AbsI(iy) > maxc) maxc = AbsI(iy);
    if (AbsI(iz) > maxc) maxc = AbsI(iz);
    uint32_t bits = CeilLogTwo(1u + maxc);
    if (bits < 1) bits = 1;
    if (bits > 20) bits = 20;
    bits -= 1;
#if RS2V_ACTORREPL_SELFCHECK
    const size_t startBit = w.NumBits();
    // Invariant: the value handed to SerializeInt(bits,20) must be in [0,20) so it
    // (and therefore the derived Bias/Max field widths) round-trips. The clamp
    // above guarantees this; a violation means the clamp/decrement logic changed.
    if (bits >= 20) {
        Logger::Warn("ActorRepl::WriteCompressedVector: bits %u out of [0,20) "
                     "(maxc=%u) - corrupt vector field width", bits, maxc);
    }
#endif
    w.SerializeInt(bits, 20);
    const uint32_t bias = 1u << (bits + 1);
    const uint32_t mx   = 1u << (bits + 2);
    w.SerializeInt(static_cast<uint32_t>(ix + static_cast<int32_t>(bias)), mx);
    w.SerializeInt(static_cast<uint32_t>(iy + static_cast<int32_t>(bias)), mx);
    w.SerializeInt(static_cast<uint32_t>(iz + static_cast<int32_t>(bias)), mx);
#if RS2V_ACTORREPL_SELFCHECK
    // Components are quantized to integers on the wire; compare against the rounded
    // ints we actually encoded, not the input floats.
    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    if (AdvanceReader(r, startBit)) {
        float rx = 0.f, ry = 0.f, rz = 0.f;
        ReadCompressedVector(r, rx, ry, rz);
        if (r.IsOverflowed() ||
            RoundToInt(rx) != ix || RoundToInt(ry) != iy || RoundToInt(rz) != iz) {
            Logger::Warn("ActorRepl::WriteCompressedVector: round-trip mismatch "
                         "wrote(%d,%d,%d) read(%d,%d,%d)%s",
                         ix, iy, iz, RoundToInt(rx), RoundToInt(ry), RoundToInt(rz),
                         r.IsOverflowed() ? " [overflow]" : "");
        }
    }
#endif
}

void ReadCompressedVector(BitReader& r, float& x, float& y, float& z) {
    const uint32_t bits = r.SerializeInt(20);
    const int32_t bias = static_cast<int32_t>(1u << (bits + 1));
    const uint32_t mx  = 1u << (bits + 2);
    x = static_cast<float>(static_cast<int32_t>(r.SerializeInt(mx)) - bias);
    y = static_cast<float>(static_cast<int32_t>(r.SerializeInt(mx)) - bias);
    z = static_cast<float>(static_cast<int32_t>(r.SerializeInt(mx)) - bias);
}

void WriteCompressedRotator(BitWriter& w, uint16_t pitch, uint16_t yaw, uint16_t roll) {
    const uint8_t bp = static_cast<uint8_t>(pitch >> 8);
    const uint8_t by = static_cast<uint8_t>(yaw >> 8);
    const uint8_t br = static_cast<uint8_t>(roll >> 8);
    w.WriteBit(bp != 0); if (bp) w.WriteByte(bp);
    w.WriteBit(by != 0); if (by) w.WriteByte(by);
    w.WriteBit(br != 0); if (br) w.WriteByte(br);
}

void ReadCompressedRotator(BitReader& r, uint16_t& pitch, uint16_t& yaw, uint16_t& roll) {
    pitch = r.ReadBit() ? static_cast<uint16_t>(r.ReadByte() << 8) : 0;
    yaw   = r.ReadBit() ? static_cast<uint16_t>(r.ReadByte() << 8) : 0;
    roll  = r.ReadBit() ? static_cast<uint16_t>(r.ReadByte() << 8) : 0;
}

void WriteActorOpenHeader(BitWriter& w, const ActorOpenHeader& hdr) {
    WriteNetGUID(w, hdr.classRef);                         // static class/archetype ref
    WriteCompressedVector(w, hdr.locX, hdr.locY, hdr.locZ); // Location (always present)
    if (hdr.hasRotation) {
        WriteCompressedRotator(w, hdr.pitch, hdr.yaw, hdr.roll);
    }
    if (hdr.isPlayerController) {
        w.WriteByte(static_cast<uint8_t>(hdr.netPlayerIndex)); // NetPlayerIndex = raw BYTE
                                                               // (UnChan.cpp:1417 `Bunch << NetPlayerIndex`),
                                                               // NOT SerializeInt(_,MAX_CHANNELS).
    }
}

bool RewriteCapturedActorOpenClassAndLocation(
    const uint8_t* payload, size_t payloadBytes, uint32_t payloadBits,
    uint32_t expectedClassRef, uint32_t replacementClassRef,
    float locX, float locY, float locZ,
    std::vector<uint8_t>& rewrittenPayload, uint32_t& rewrittenPayloadBits) {
    auto fail = [&]() {
        rewrittenPayload.clear();
        rewrittenPayloadBits = 0;
        return false;
    };

    if (payload == nullptr || payloadBits == 0 ||
        expectedClassRef == 0 || expectedClassRef >= kStaticObjectMax ||
        replacementClassRef == 0 || replacementClassRef >= kStaticObjectMax ||
        payloadBits > BitWriter::kMaxSaneBunchBits) {
        return fail();
    }

    const size_t requiredBytes = (static_cast<size_t>(payloadBits) + 7u) / 8u;
    if (requiredBytes > payloadBytes) {
        return fail();
    }

    // WriteCompressedVector's largest field has Bias=2^20, so its exact signed
    // integer domain is [-2^20, 2^20-1]. Check in double precision before the
    // float-to-int conversion performed by the codec.
    auto validComponent = [](float value) {
        if (!std::isfinite(value)) return false;
        const double rounded = std::floor(static_cast<double>(value) + 0.5);
        return rounded >= -1048576.0 && rounded <= 1048575.0;
    };
    if (!validComponent(locX) || !validComponent(locY) || !validComponent(locZ)) {
        return fail();
    }

    // Pass only the bytes actually covered by payloadBits. Besides documenting
    // the bound, this avoids overflow in BitReader's numBytes*8 calculation if a
    // hostile caller supplies an unrelated, enormous payloadBytes value.
    BitReader source(payload, requiredBytes, payloadBits);
    source.SetOverflowHandler([](const char*, size_t, size_t, size_t) {});

    const NetGUIDRef classRef = ReadNetGUID(source);
    if (source.IsOverflowed() || classRef.isDynamic || classRef.index == 0 ||
        classRef.index != expectedClassRef) {
        return fail();
    }

    float oldX = 0.f, oldY = 0.f, oldZ = 0.f;
    ReadCompressedVector(source, oldX, oldY, oldZ);
    if (source.IsOverflowed()) {
        return fail();
    }
    (void)oldX;
    (void)oldY;
    (void)oldZ;

    const size_t tailBits = source.BitsLeft();
    BitWriter rewritten;
    WriteNetGUID(
        rewritten,
        NetGUIDRef{/*isDynamic=*/false, replacementClassRef});
    WriteCompressedVector(rewritten, locX, locY, locZ);

    // Reject growth before copying the opaque tail. Besides keeping the result
    // inside the project's sane bunch bound, this avoids asking BitWriter to
    // cross the limit (and emitting an avoidable diagnostic for hostile input).
    if (rewritten.HadInvariantViolation() || rewritten.ExceededBunchLimit() ||
        rewritten.NumBits() > BitWriter::kMaxSaneBunchBits ||
        tailBits > BitWriter::kMaxSaneBunchBits - rewritten.NumBits()) {
        return fail();
    }
    for (size_t i = 0; i < tailBits; ++i) {
        rewritten.WriteBit(source.ReadBit());
    }
    if (source.IsOverflowed() || rewritten.HadInvariantViolation() ||
        rewritten.ExceededBunchLimit() ||
        rewritten.NumBits() > std::numeric_limits<uint32_t>::max()) {
        return fail();
    }

    // Build the replacement fully before assigning so input may safely point
    // into rewrittenPayload's current storage.
    rewrittenPayload = rewritten.GetBytes();
    rewrittenPayloadBits = static_cast<uint32_t>(rewritten.NumBits());
    return true;
}

bool RewriteCapturedActorOpenLocation(
    const uint8_t* payload, size_t payloadBytes, uint32_t payloadBits,
    float locX, float locY, float locZ,
    std::vector<uint8_t>& rewrittenPayload, uint32_t& rewrittenPayloadBits) {
    auto fail = [&]() {
        rewrittenPayload.clear();
        rewrittenPayloadBits = 0;
        return false;
    };

    if (payload == nullptr || payloadBits == 0 ||
        payloadBits > BitWriter::kMaxSaneBunchBits) {
        return fail();
    }
    const size_t requiredBytes = (static_cast<size_t>(payloadBits) + 7u) / 8u;
    if (requiredBytes > payloadBytes) {
        return fail();
    }

    BitReader source(payload, requiredBytes, payloadBits);
    source.SetOverflowHandler([](const char*, size_t, size_t, size_t) {});
    const NetGUIDRef classRef = ReadNetGUID(source);
    if (source.IsOverflowed() || classRef.isDynamic || classRef.index == 0) {
        return fail();
    }

    return RewriteCapturedActorOpenClassAndLocation(
        payload, payloadBytes, payloadBits,
        classRef.index, classRef.index,
        locX, locY, locZ, rewrittenPayload, rewrittenPayloadBits);
}

bool RewriteCapturedDynamicChannelRefs(
    const uint8_t* payload, size_t payloadBytes, uint32_t payloadBits,
    const std::vector<CapturedDynamicChannelRewrite>& rewrites,
    std::vector<uint8_t>& rewrittenPayload) {
    auto fail = [&]() {
        rewrittenPayload.clear();
        return false;
    };

    if (payload == nullptr || payloadBits == 0 ||
        payloadBits > BitWriter::kMaxSaneBunchBits) {
        return fail();
    }
    const size_t requiredBytes = (static_cast<size_t>(payloadBits) + 7u) / 8u;
    if (requiredBytes > payloadBytes) {
        return fail();
    }

    constexpr size_t kDynamicRefBits = 11u;
    for (size_t i = 0; i < rewrites.size(); ++i) {
        const CapturedDynamicChannelRewrite& rewrite = rewrites[i];
        if (rewrite.bitOffset > payloadBits ||
            payloadBits - rewrite.bitOffset < kDynamicRefBits ||
            rewrite.expectedChannel == 0 ||
            rewrite.expectedChannel >= kDynamicChannelMax ||
            rewrite.replacementChannel == 0 ||
            rewrite.replacementChannel >= kDynamicChannelMax) {
            return fail();
        }

        const size_t rewriteEnd = rewrite.bitOffset + kDynamicRefBits;
        for (size_t j = 0; j < i; ++j) {
            const size_t priorStart = rewrites[j].bitOffset;
            const size_t priorEnd = priorStart + kDynamicRefBits;
            if (rewrite.bitOffset < priorEnd && priorStart < rewriteEnd) {
                return fail();
            }
        }
    }

    auto readBit = [payload](size_t bit) {
        return static_cast<uint8_t>(
            (payload[bit >> 3u] >> (bit & 7u)) & 1u);
    };
    for (const CapturedDynamicChannelRewrite& rewrite : rewrites) {
        if (readBit(rewrite.bitOffset) == 0) {
            return fail();
        }
        uint32_t channel = 0;
        for (uint32_t bit = 0; bit < 10u; ++bit) {
            channel |= static_cast<uint32_t>(
                           readBit(rewrite.bitOffset + 1u + bit))
                       << bit;
        }
        if (channel != rewrite.expectedChannel) {
            return fail();
        }
    }

    // Copy every source byte, including the unused high padding bits of the
    // final byte. Only the ten channel-value bits below are permitted to move.
    std::vector<uint8_t> candidate(payload, payload + requiredBytes);
    auto writeBit = [&candidate](size_t bit, bool value) {
        const uint8_t mask = static_cast<uint8_t>(1u << (bit & 7u));
        uint8_t& byte = candidate[bit >> 3u];
        if (value) byte = static_cast<uint8_t>(byte | mask);
        else byte = static_cast<uint8_t>(byte & static_cast<uint8_t>(~mask));
    };
    for (const CapturedDynamicChannelRewrite& rewrite : rewrites) {
        for (uint32_t bit = 0; bit < 10u; ++bit) {
            writeBit(rewrite.bitOffset + 1u + bit,
                     ((rewrite.replacementChannel >> bit) & 1u) != 0);
        }
    }

    rewrittenPayload = std::move(candidate);
    return true;
}

// ---- property serialization (handle + typed value) --------------------------
namespace {
#if RS2V_ACTORREPL_SELFCHECK
// Invariant: a replicated property handle must be in [0, maxHandle). Outside it,
// SerializeInt truncates and the client decodes the wrong property (or desyncs the
// whole bunch). maxHandle==0 means the caller passed a bad ClassNetCache count.
void CheckHandle(const char* what, uint32_t handle, uint32_t maxHandle) {
    if (maxHandle == 0 || handle >= maxHandle) {
        Logger::Warn("ActorRepl::WriteProp%s: handle %u out of range [0,%u) - "
                     "mis-encoded property id", what, handle, maxHandle);
    }
}
// Read back [handle][value] starting at bit `startBit` and compare. `cmpValue`
// reads the typed value off `rr` and returns whether it matches what was written.
template <typename CmpFn>
void CheckProp(const BitWriter& w, size_t startBit, const char* what,
               uint32_t handle, uint32_t maxHandle, CmpFn cmpValue) {
    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    if (!AdvanceReader(r, startBit)) return;
    const uint32_t h = r.SerializeInt(maxHandle);
    const bool valOk = cmpValue(r);
    if (r.IsOverflowed() || h != handle || !valOk) {
        Logger::Warn("ActorRepl::WriteProp%s: round-trip mismatch handle wrote=%u "
                     "read=%u%s%s", what, handle, h,
                     valOk ? "" : " [value]", r.IsOverflowed() ? " [overflow]" : "");
    }
}
#endif // RS2V_ACTORREPL_SELFCHECK
} // namespace

void WritePropBool(BitWriter& w, uint32_t handle, uint32_t maxHandle, bool v) {
#if RS2V_ACTORREPL_SELFCHECK
    CheckHandle("Bool", handle, maxHandle);
    const size_t startBit = w.NumBits();
#endif
    w.SerializeInt(handle, maxHandle);
    w.WriteBit(v);
#if RS2V_ACTORREPL_SELFCHECK
    CheckProp(w, startBit, "Bool", handle, maxHandle,
              [&](BitReader& rr){ return rr.ReadBit() == v; });
#endif
}
void WritePropByte(BitWriter& w, uint32_t handle, uint32_t maxHandle, uint8_t v,
                   uint32_t numBits) {
#if RS2V_ACTORREPL_SELFCHECK
    CheckHandle("Byte", handle, maxHandle);
    const size_t startBit = w.NumBits();
#endif
    // UE3 UByteProperty::NetSerializeItem (UnProp.cpp): an ENUM byte serializes in
    // appCeilLogTwo(NumEnums-1) bits, a plain byte in 8. Caller passes numBits (default 8;
    // e.g. ENetRole -> 3). Emitting a fixed 8 bits for an enum desyncs the rest of the bunch.
    w.SerializeInt(handle, maxHandle);
    w.WriteBits(v, static_cast<int>(numBits));
#if RS2V_ACTORREPL_SELFCHECK
    CheckProp(w, startBit, "Byte", handle, maxHandle,
              [&](BitReader& rr){ return rr.ReadBits(static_cast<int>(numBits)) == v; });
#endif
}
void WritePropInt(BitWriter& w, uint32_t handle, uint32_t maxHandle, int32_t v) {
#if RS2V_ACTORREPL_SELFCHECK
    CheckHandle("Int", handle, maxHandle);
    const size_t startBit = w.NumBits();
#endif
    w.SerializeInt(handle, maxHandle);
    w.WriteInt32(v);
#if RS2V_ACTORREPL_SELFCHECK
    CheckProp(w, startBit, "Int", handle, maxHandle,
              [&](BitReader& rr){ return rr.ReadInt32() == v; });
#endif
}
void WritePropFloat(BitWriter& w, uint32_t handle, uint32_t maxHandle, float v) {
#if RS2V_ACTORREPL_SELFCHECK
    CheckHandle("Float", handle, maxHandle);
    const size_t startBit = w.NumBits();
#endif
    w.SerializeInt(handle, maxHandle);
    w.WriteFloat(v);
#if RS2V_ACTORREPL_SELFCHECK
    CheckProp(w, startBit, "Float", handle, maxHandle,
              [&](BitReader& rr){ const float f = rr.ReadFloat();
                                  return f == v || (f != f && v != v); });
#endif
}
void WritePropString(BitWriter& w, uint32_t handle, uint32_t maxHandle, const std::string& v) {
#if RS2V_ACTORREPL_SELFCHECK
    CheckHandle("String", handle, maxHandle);
    const size_t startBit = w.NumBits();
#endif
    w.SerializeInt(handle, maxHandle);
    w.WriteString(v);
#if RS2V_ACTORREPL_SELFCHECK
    CheckProp(w, startBit, "String", handle, maxHandle,
              [&](BitReader& rr){ return rr.ReadString() == v; });
#endif
}
void WritePropObject(BitWriter& w, uint32_t handle, uint32_t maxHandle, const NetGUIDRef& v) {
#if RS2V_ACTORREPL_SELFCHECK
    CheckHandle("Object", handle, maxHandle);
    const size_t startBit = w.NumBits();
#endif
    w.SerializeInt(handle, maxHandle);
    WriteNetGUID(w, v);
#if RS2V_ACTORREPL_SELFCHECK
    CheckProp(w, startBit, "Object", handle, maxHandle,
              [&](BitReader& rr){ NetGUIDRef b = ReadNetGUID(rr);
                                  return b.isDynamic == v.isDynamic && b.index == v.index; });
#endif
}

PacketCodec::Bunch MakeOpeningActorBunch(
    uint32_t chIndex, uint32_t chSeq,
    const ActorOpenHeader& hdr,
    const std::function<void(BitWriter&)>& writeProps) {
    BitWriter w;
    WriteActorOpenHeader(w, hdr);
    if (writeProps) {
        writeProps(w);
    }
    PacketCodec::Bunch b;
    // UE3 only serializes bOpen/bClose when bControl is set. Leaving bControl
    // false while setting bOpen merely looks correct in memory: PacketCodec drops
    // the open bit on the wire and the client rejects the reliable payload as data
    // for a nonexistent actor channel. Real actor opens use flags 0xD
    // (bControl|bOpen|bReliable).
    b.bControl = true;
    b.bOpen = true;       // opening an actor channel
    b.bClose = false;
    b.bReliable = true;
    b.chIndex = chIndex;
    b.chType = 2;         // CHTYPE_Actor
    b.chSequence = chSeq;
    b.payload = w.GetBytes();
    b.payloadBits = static_cast<uint32_t>(w.NumBits());
    return b;
}

} // namespace ActorRepl
