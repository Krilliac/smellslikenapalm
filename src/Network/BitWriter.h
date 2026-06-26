// src/Network/BitWriter.h
//
// UE3 FBitWriter-compatible bit serializer.
//
// Wire conventions (spec §2, RS2V_ControlChannel_WireSpec_7258.md) [UE3]:
//   * Bits are packed LSB-first WITHIN each byte: the first bit written occupies
//     bit 0 (value 1) of byte 0, the next bit occupies bit 1 (value 2), etc.
//   * Multi-byte primitives (BYTE/INT/DWORD/FLOAT/QWORD) are serialized as their
//     raw little-endian bytes, each byte emitted LSB-first as 8 bits. Because the
//     bytes themselves are LSB-first, a byte-aligned write reproduces the source
//     bytes verbatim.
//   * SerializeInt(value, max) is UE3's bounded-int encoding: it writes only as
//     many bits as are needed to represent values in [0, max), LSB-first, and
//     stops early once the remaining magnitude can no longer reach `max`.
//   * FString: INT length prefix. length > 0  => ANSI, 1 byte/char, INCLUDING the
//     NUL terminator (so length == strlen+1). length < 0 => UCS-2/UTF-16LE,
//     (-length) code units INCLUDING the NUL terminator. length == 0 => empty.
//
// This mirrors Unreal's FBitWriter closely enough to round-trip with BitReader
// and to produce bytes a stock UE3 client/server will accept once the bunch
// framing (handled elsewhere) is correct.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BitWriter {
public:
    BitWriter() = default;

    // ---- primitive bit IO ----

    // Append a single bit (LSB-first within the current byte).
    void WriteBit(bool bit);

    // Append the low `count` bits of `value`, LSB-first. count in [0,64].
    void WriteBits(uint64_t value, int count);

    // UE3 bounded-int encoding. Writes the minimum number of bits to represent
    // `value` given an exclusive upper bound `maxValue` (i.e. valid values are
    // [0, maxValue)). Matches FArchive::SerializeInt(Value, Max).
    void SerializeInt(uint32_t value, uint32_t maxValue);

    // Alias matching the spec's "WriteInt"/SerializeInt naming.
    void WriteInt(uint32_t value, uint32_t maxValue) { SerializeInt(value, maxValue); }

    // ---- fixed-width primitives (raw little-endian, byte semantics) ----
    void WriteByte(uint8_t v);
    void WriteUInt8(uint8_t v) { WriteByte(v); }
    void WriteInt32(int32_t v);
    void WriteUInt32(uint32_t v);
    void WriteUInt64(uint64_t v);
    void WriteFloat(float v);
    void WriteRawBytes(const uint8_t* data, size_t len);

    // ---- FString (UE3 length-prefixed) ----
    // Serializes `s` as ANSI when it is pure 7-bit/8-bit content with no chars
    // requiring UCS-2. Empty string => length 0 (no characters).
    void WriteString(const std::string& s);

    // Force UCS-2/UTF-16LE encoding (negative length). Each char of `s` is
    // widened to a 16-bit code unit; a NUL terminator is appended.
    void WriteStringUCS2(const std::wstring& s);

    // ---- output / state ----

    // Pad the final partial byte with zero bits up to a byte boundary and return
    // the packed buffer. Does not mutate internal state.
    std::vector<uint8_t> GetBytes() const;

    // Number of bits written so far.
    size_t NumBits() const { return m_numBits; }

    // Number of whole + partial bytes the current bits occupy.
    size_t NumBytes() const { return (m_numBits + 7) / 8; }

    void Reset() {
        m_buffer.clear();
        m_numBits = 0;
        m_invariantViolated = false;
        m_exceededBunchLimit = false;
    }

    // ---- diagnostics / invariants (non-fatal) ----

    // A sane upper bound on the size of a single UE3 bunch. UE3's reliable bunch
    // payload is well under this; crossing it almost always indicates a logic bug
    // upstream (e.g. an unbounded loop emitting properties). Exceeding it is
    // LOGGED once and exposed via ExceededBunchLimit(); it is NOT enforced.
    static constexpr size_t kMaxSaneBunchBits = 16384;

    // True if any invariant was violated since construction/Reset() (e.g. a
    // SerializeInt value >= maxValue, or an out-of-range WriteBits count). The
    // offending write is still performed (clamped) so the wire format is
    // unchanged; this flag lets callers detect that something was off.
    bool HadInvariantViolation() const { return m_invariantViolated; }

    // True if total bits written has crossed kMaxSaneBunchBits.
    bool ExceededBunchLimit() const { return m_exceededBunchLimit; }

    // Optional per-op verbose tracing (default OFF). When enabled, each logical
    // write op is logged at LogLevel::Trace. This does not affect output bytes.
    void SetTrace(bool enabled) { m_trace = enabled; }
    bool TraceEnabled() const { return m_trace; }

private:
    // Flag + log the first time total bits exceed kMaxSaneBunchBits. Called from
    // the single bit-increment point so it fires exactly once per overrun.
    void CheckBunchLimit();

    std::vector<uint8_t> m_buffer; // packed bytes; last byte may be partial
    size_t m_numBits = 0;          // total bits written
    bool m_trace = false;          // verbose per-op trace (default off)
    bool m_invariantViolated = false; // sticky: an invariant was violated
    bool m_exceededBunchLimit = false; // sticky: crossed kMaxSaneBunchBits
};
