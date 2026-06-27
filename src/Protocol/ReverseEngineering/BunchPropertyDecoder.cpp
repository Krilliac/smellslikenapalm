// src/Protocol/ReverseEngineering/BunchPropertyDecoder.cpp

#include "Protocol/ReverseEngineering/BunchPropertyDecoder.h"
#include "Network/BitReader.h"

#include <algorithm>
#include <cmath>
#include <sstream>

double BunchDecodeResult::FitScore() const {
    if (bitsTotal == 0) return 0.0;
    double coverage = static_cast<double>(bitsConsumed) / static_cast<double>(bitsTotal);
    double base = coverage;
    // Reward decoding multiple real properties; a single accidental match is weak.
    base += 0.10 * static_cast<double>(properties.size());
    switch (status) {
        case BunchDecodeStatus::CleanEnd:          base += 0.25; break;
        case BunchDecodeStatus::StoppedUnresolved: base -= 0.05; break;
        case BunchDecodeStatus::Overflow:          base -= 0.50; break;
        case BunchDecodeStatus::NoFields:          return 0.0;
    }
    if (base < 0.0) base = 0.0;
    return base;
}

namespace {

// --- value decoders. Each returns false (without trusting the value) when the
//     type cannot be advanced safely, so the caller stops cleanly. ---

// FVector::SerializeCompressed (codec §4.1).
bool DecodeCompressedVector(BitReader& r, uint32_t /*maxCh*/, std::string& out) {
    uint32_t bits = r.SerializeInt(20);           // per-component bit budget 0..19
    if (r.IsOverflowed()) return false;
    uint32_t maxv = 1u << (bits + 2);
    int32_t bias = static_cast<int32_t>(1u << (bits + 1));
    int32_t x = static_cast<int32_t>(r.SerializeInt(maxv)) - bias;
    int32_t y = static_cast<int32_t>(r.SerializeInt(maxv)) - bias;
    int32_t z = static_cast<int32_t>(r.SerializeInt(maxv)) - bias;
    if (r.IsOverflowed()) return false;
    std::ostringstream os;
    os << "(" << x << "," << y << "," << z << ")";
    out = os.str();
    return true;
}

// FRotator::SerializeCompressed (codec §4.2): per-component presence bit + hi byte.
bool DecodeCompressedRotator(BitReader& r, std::string& out) {
    int hi[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        bool present = r.ReadBit();
        if (present) hi[i] = static_cast<int>(r.ReadByte()) << 8;
    }
    if (r.IsOverflowed()) return false;
    std::ostringstream os;
    os << "P=" << hi[0] << " Y=" << hi[1] << " R=" << hi[2];
    out = os.str();
    return true;
}

// UPackageMapLevel::SerializeObject (codec §3).
bool DecodeObjectRef(BitReader& r, uint32_t maxChannels, std::string& out) {
    bool selector = r.ReadBit();
    if (r.IsOverflowed()) return false;
    if (selector) {
        uint32_t idx = r.SerializeInt(maxChannels);
        if (r.IsOverflowed()) return false;
        out = (idx == 0) ? "None" : ("actor@ch" + std::to_string(idx));
    } else {
        uint32_t idx = r.SerializeInt(0x80000000u);  // MAX_OBJECT_INDEX
        if (r.IsOverflowed()) return false;
        out = "static#" + std::to_string(idx);
    }
    return true;
}

bool DecodeValue(BitReader& r, const NetField& f, uint32_t maxChannels,
                 std::string& summary) {
    switch (f.valueType) {
        case NetValueType::Bool: {
            bool b = r.ReadBit();
            summary = b ? "true" : "false";
            return !r.IsOverflowed();
        }
        case NetValueType::Byte: {
            uint8_t v = r.ReadByte();
            summary = std::to_string(static_cast<unsigned>(v));
            return !r.IsOverflowed();
        }
        case NetValueType::Int: {
            int32_t v = r.ReadInt32();
            summary = std::to_string(v);
            return !r.IsOverflowed();
        }
        case NetValueType::Float: {
            float v = r.ReadFloat();
            std::ostringstream os; os << v; summary = os.str();
            return !r.IsOverflowed();
        }
        case NetValueType::String: {
            std::string s = r.ReadString();
            if (r.IsOverflowed()) return false;
            if (s.size() > 48) s = s.substr(0, 48) + "...";
            summary = "\"" + s + "\"";
            return true;
        }
        case NetValueType::StructVector:
            return DecodeCompressedVector(r, maxChannels, summary);
        case NetValueType::StructRotator:
            return DecodeCompressedRotator(r, summary);
        case NetValueType::StructQuat: {
            float x = r.ReadFloat(), y = r.ReadFloat(), z = r.ReadFloat();
            if (r.IsOverflowed()) return false;
            std::ostringstream os; os << "(" << x << "," << y << "," << z << ",w?)";
            summary = os.str();
            return true;
        }
        case NetValueType::StructPlane: {
            // 4x SWORD (16-bit signed), codec §2.7.
            int16_t c[4];
            for (int i = 0; i < 4; ++i)
                c[i] = static_cast<int16_t>(r.ReadBits(16));
            if (r.IsOverflowed()) return false;
            std::ostringstream os;
            os << "(" << c[0] << "," << c[1] << "," << c[2] << "," << c[3] << ")";
            summary = os.str();
            return true;
        }
        case NetValueType::StructUniqueNetId: {
            uint64_t v = r.ReadUInt64();
            if (r.IsOverflowed()) return false;
            summary = "uid:" + std::to_string(v);
            return true;
        }
        case NetValueType::Object:
        case NetValueType::Class:
            return DecodeObjectRef(r, maxChannels, summary);
        case NetValueType::DynArray:
            // NetSerializeItem no-op (codec §2.8): consumes 0 value bits.
            summary = "(dynarray:no-op)";
            return true;

        // Undecodable widths — stop cleanly rather than desync the cursor.
        case NetValueType::EnumByte:    summary = "(enum:width-unknown)";   return false;
        case NetValueType::StructOther: summary = "(struct:layout-unknown)"; return false;
        case NetValueType::Unknown:
        default:                        summary = "(type-unknown)";          return false;
    }
}

} // namespace

// Advance a BitReader by `bits` (it has no seek). Reads are bounds-checked, so a
// startBit past the buffer simply leaves the reader overflowed.
static void SkipBits(BitReader& r, size_t bits) {
    while (bits > 0) {
        int n = static_cast<int>(std::min<size_t>(32, bits));
        r.ReadBits(n);
        bits -= n;
    }
}

BunchPropertyDecoder::OpenHeaderResult BunchPropertyDecoder::ParseOpenHeader(
    const uint8_t* payload, size_t numBytes, size_t validBits,
    const std::function<std::string(uint32_t)>& classResolver,
    const std::function<bool(const std::string&)>& hasInitialRotation) const {

    OpenHeaderResult out;
    if (!payload || numBytes == 0) return out;
    size_t bitsTotal = (validBits != 0) ? validBits : numBytes * 8;
    BitReader r(payload, numBytes, bitsTotal);

    // [class/archetype ref] = 1 selector bit + SerializeInt(index, 1<<31).
    // For a class ref the selector is 0 (static object).
    bool selector = r.ReadBit();
    if (selector) {
        // Dynamic ref form — not a class ref; can't identify the actor class.
        return out;
    }
    out.classIndex = r.SerializeInt(0x80000000u);
    if (r.IsOverflowed()) return out;

    if (classResolver) out.className = classResolver(out.classIndex);

    // [compressed Location] — ALWAYS present.
    std::string tmp;
    if (!DecodeCompressedVector(r, m_maxChannels, tmp)) return out;

    // [compressed Rotation] — only when the class's bNetInitialRotation is true
    // (false for PC/Info/PRI/GRI/TeamInfo, so absent for all menu actors).
    if (hasInitialRotation && hasInitialRotation(out.className)) {
        if (!DecodeCompressedRotator(r, tmp)) return out;
    }

    // [NetPlayerIndex] — a BYTE, only for PlayerController actors.
    if (out.className.find("PlayerController") != std::string::npos) {
        r.ReadByte();
        if (r.IsOverflowed()) return out;
    }

    out.headerBits = r.BitPos();
    out.ok = !r.IsOverflowed();
    return out;
}

BunchDecodeResult BunchPropertyDecoder::Decode(const NetFieldTable& table,
                                               const uint8_t* payload, size_t numBytes,
                                               size_t validBits, size_t startBit) const {
    BunchDecodeResult result;
    result.className = table.ClassName();
    result.bitsTotal = (validBits != 0) ? validBits : numBytes * 8;

    if (table.Size() == 0 || numBytes == 0) {
        result.status = BunchDecodeStatus::NoFields;
        return result;
    }

    BitReader r(payload, numBytes, result.bitsTotal);
    if (startBit > 0) SkipBits(r, startBit);
    if (r.IsOverflowed()) { result.status = BunchDecodeStatus::Overflow; return result; }
    result.status = BunchDecodeStatus::CleanEnd;

    // Bound the loop independently of bit math (defensive against a pathological
    // table); a real bunch never carries this many records.
    const size_t kMaxRecords = 4096;
    for (size_t i = 0; i < kMaxRecords; ++i) {
        if (r.BitsLeft() == 0) { result.status = BunchDecodeStatus::CleanEnd; break; }

        size_t hStart = r.BitPos();
        uint32_t handle = r.SerializeInt(table.MaxIndex());
        if (r.IsOverflowed()) { result.status = BunchDecodeStatus::CleanEnd; break; }

        const NetField* f = table.GetField(handle);
        if (!f) {
            // Out-of-range handle ends the property section (UnChan.cpp:1485).
            result.status = BunchDecodeStatus::CleanEnd;
            break;
        }

        DecodedProperty p;
        p.handle = handle;
        p.name = f->name;
        p.type = f->rawType;
        p.bitStart = hStart;

        if (f->kind == NetPropKind::Func) {
            // RPC marker — params need the function signature we don't have.
            p.valueSummary = "(rpc-call)";
            p.valueDecoded = false;
            p.bitLen = r.BitPos() - hStart;
            result.properties.push_back(std::move(p));
            result.status = BunchDecodeStatus::StoppedUnresolved;
            break;
        }

        std::string summary;
        bool ok = DecodeValue(r, *f, m_maxChannels, summary);
        p.valueSummary = summary;
        p.valueDecoded = ok;
        p.bitLen = r.BitPos() - hStart;
        result.properties.push_back(std::move(p));

        if (!ok) {
            result.status = r.IsOverflowed() ? BunchDecodeStatus::Overflow
                                             : BunchDecodeStatus::StoppedUnresolved;
            break;
        }
        if (r.IsOverflowed()) { result.status = BunchDecodeStatus::Overflow; break; }
    }

    result.bitsConsumed = r.BitPos();
    return result;
}
