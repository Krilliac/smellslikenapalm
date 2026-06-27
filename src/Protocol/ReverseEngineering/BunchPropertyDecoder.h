// src/Protocol/ReverseEngineering/BunchPropertyDecoder.h
//
// Decodes the bit-packed property-record stream inside a UE3 actor-channel bunch
// into (handle -> name -> value) records, using a NetFieldTable for the channel's
// class and the authoritative codec in docs/re/ue3_property_value_codec.md.
//
// This replaces the byte-aligned guesswork in ProtocolDecoder's payload analysis
// for the case where the class is known: properties are bit-packed (no byte
// alignment), so the only correct way to read them is the ranged-int / compressed
// codecs implemented here on top of BitReader.
//
// It is best-effort by design. The property stream has no length or terminator
// (UnChan.cpp:1485) — it ends when a handle resolves to no field or the bits run
// out. Some value types (enum byte width, arbitrary structs, RPC params) are not
// determinable from the handle tables alone; the decoder records what it can and
// reports WHERE and WHY it stopped rather than guessing past an unknown width.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include "Protocol/ReverseEngineering/NetFieldTable.h"

enum class BunchDecodeStatus : uint8_t {
    CleanEnd,           // handle out of range / bits exhausted normally
    StoppedUnresolved,  // hit a value type we cannot decode (enum/struct/RPC)
    Overflow,           // a read ran past the buffer mid-value (likely misaligned)
    NoFields            // table empty / no class
};

struct DecodedProperty {
    uint32_t    handle = 0;
    std::string name;
    std::string type;          // rawType from the table
    std::string valueSummary;  // human-readable decoded value
    size_t      bitStart = 0;  // bit offset of the handle within the payload
    size_t      bitLen = 0;    // total bits consumed by this record
    bool        valueDecoded = false;
};

struct BunchDecodeResult {
    std::string                  className;
    std::vector<DecodedProperty> properties;
    BunchDecodeStatus            status = BunchDecodeStatus::NoFields;
    size_t                       bitsConsumed = 0;
    size_t                       bitsTotal = 0;

    // Fit score for best-class matching: rewards clean ends and bit coverage,
    // penalises early unresolved stops. Range roughly [0,1+].
    double FitScore() const;
};

class BunchPropertyDecoder {
public:
    // maxChannels = the MAX_CHANNELS / None bound used by the object-ref codec.
    // RS2-7258 capture confirms 1024 for this build (MASTER_replication_reference
    // §0; ue3_property_value_codec §3 WARNING). Configurable to re-verify.
    explicit BunchPropertyDecoder(uint32_t maxChannels = 1024)
        : m_maxChannels(maxChannels) {}

    // Decode the property stream in [payload, payload+numBytes). validBits caps
    // the meaningful bit length (defaults to numBytes*8). startBit skips a leading
    // header (e.g. an open bunch's SerializeNewActor block, see ParseOpenHeader).
    BunchDecodeResult Decode(const NetFieldTable& table,
                             const uint8_t* payload, size_t numBytes,
                             size_t validBits = 0, size_t startBit = 0) const;

    // Result of parsing an actor-channel OPEN bunch's SerializeNewActor header
    // (docs/re/open_bunch_structure.md): a static class ref, compressed Location,
    // optional Rotation, optional PlayerController NetPlayerIndex byte. The
    // property block begins at headerBits.
    struct OpenHeaderResult {
        bool        ok = false;
        uint32_t    classIndex = 0;   // package-map static export index
        std::string className;        // resolved name (empty if unknown)
        size_t      headerBits = 0;   // bit offset where the property block starts
    };

    // Parse the open header. classResolver maps a static class export index to a
    // class name (empty if unknown); it decides PlayerController (NetPlayerIndex
    // byte) and lets the caller bind the channel exactly. hasInitialRotation tells
    // whether the class emits a compressed Rotation (false for PC/Info/PRI/GRI/
    // TeamInfo — see the doc); when null, rotation is assumed absent.
    OpenHeaderResult ParseOpenHeader(
        const uint8_t* payload, size_t numBytes, size_t validBits,
        const std::function<std::string(uint32_t)>& classResolver,
        const std::function<bool(const std::string&)>& hasInitialRotation = {}) const;

    void SetMaxChannels(uint32_t v) { m_maxChannels = v; }
    uint32_t MaxChannels() const { return m_maxChannels; }

private:
    uint32_t m_maxChannels;
};
