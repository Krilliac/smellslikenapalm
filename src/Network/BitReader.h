// src/Network/BitReader.h
//
// UE3 FBitReader-compatible bit deserializer. The exact mirror of BitWriter:
// LSB-first bit order within bytes, raw little-endian multi-byte primitives,
// UE3 bounded SerializeInt, and UE3 length-prefixed FString.
//
// BOUNDS SAFETY: every read is bounds-checked against the bit length of the
// backing buffer. A read that would run past the end sets a sticky overflow flag
// (IsOverflowed()) and returns a zero/default value instead of reading OOB. This
// is intentional - real client datagrams and fuzzed input must never cause an
// out-of-bounds read.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class BitReader {
public:
    BitReader(const uint8_t* data, size_t numBytes);
    explicit BitReader(const std::vector<uint8_t>& data);

    // Construct with an EXPLICIT valid bit count (validBits <= numBytes*8). Reads
    // past validBits set the overflow flag even though the backing buffer has more
    // (zero-pad) bytes. Used to parse a bit-packed buffer whose meaningful length
    // is not a whole number of bytes (e.g. a reassembled control-channel stream),
    // so an incomplete trailing message is detected as overflow rather than read
    // into pad bits.
    BitReader(const uint8_t* data, size_t numBytes, size_t validBits);

    // ---- primitive bit IO ----
    bool ReadBit();                                   // returns false on overflow
    uint64_t ReadBits(int count);                     // low `count` bits, LSB-first
    uint32_t SerializeInt(uint32_t maxValue);         // UE3 bounded-int decode
    uint32_t ReadInt(uint32_t maxValue) { return SerializeInt(maxValue); }

    // ---- fixed-width primitives ----
    uint8_t  ReadByte();
    int32_t  ReadInt32();
    uint32_t ReadUInt32();
    uint64_t ReadUInt64();
    float    ReadFloat();

    // ---- FString (UE3 length-prefixed) ----
    // Decodes both ANSI (positive length) and UCS-2 (negative length) forms,
    // dropping the trailing NUL terminator. Result is returned as UTF-8-ish
    // std::string (UCS-2 code units are narrowed to bytes; non-ASCII high bytes
    // are preserved as-is for the low byte). On overflow or a length that would
    // exceed the buffer, returns "" and sets the overflow flag.
    std::string ReadString();

    // ---- state ----
    bool IsOverflowed() const { return m_overflow; }
    size_t BitPos() const { return m_bitPos; }
    size_t NumBits() const { return m_numBits; }
    size_t BitsLeft() const { return (m_bitPos < m_numBits) ? (m_numBits - m_bitPos) : 0; }

    // ---- overflow diagnostics (non-fatal observability) ----
    // When a read would run past the end of the buffer the reader sets the sticky
    // overflow flag and returns a default value; it never reads OOB and never
    // throws. These accessors expose WHICH read overflowed and WHERE, so callers
    // can diagnose a misaligned/misparsed bunch after the fact. Values are only
    // meaningful once IsOverflowed() is true; before that OverflowOp() is nullptr.
    const char* OverflowOp() const { return m_overflowOp; }       // label of failing read
    size_t OverflowBitPos() const { return m_overflowBitPos; }    // bit pos at failure
    size_t OverflowWantBits() const { return m_overflowWantBits; }// bits requested

    // Optional sink, invoked exactly once at the moment overflow is first set.
    // If no handler is installed, a Warn is emitted via the project Logger. The
    // handler must not throw (overflow handling is non-fatal). Args: failing-read
    // label, bit position at failure, bits requested, total readable bits.
    using OverflowHandler =
        std::function<void(const char* op, size_t bitPos, size_t wantBits, size_t numBits)>;
    void SetOverflowHandler(OverflowHandler handler) { m_overflowHandler = std::move(handler); }

    // End-of-decode invariant. After a fully-consumed bunch/message BitsLeft()
    // should be 0; leftover bits usually mean a misaligned or misparsed bunch.
    // Logs a non-fatal Warn (with `context`) when bits remain. Returns BitsLeft().
    // Pure observation - does not advance the cursor or change behavior.
    size_t ReportTrailingBits(const char* context) const;

    // Debug hexdump of the remaining (unconsumed) bits, repacked LSB-first into
    // bytes starting at the current bit position to mirror the on-wire bit order.
    // Caps at `maxBytes` bytes (appends "..." if truncated). Non-mutating; intended
    // for trace logging of a misaligned decode. Returns "" when no bits remain.
    std::string DumpRemainingBits(size_t maxBytes = 64) const;

private:
    // Returns false (and sets overflow) if `count` bits are not available. `op` is
    // a short static label identifying the calling read, recorded for diagnostics.
    bool EnsureBits(size_t count, const char* op = "read");

    // Records overflow context and notifies the handler/Logger (called once).
    void ReportOverflow();

    const uint8_t* m_data;
    size_t m_numBits;   // total readable bits (= numBytes * 8)
    size_t m_bitPos = 0;
    bool   m_overflow = false;

    // Overflow diagnostics, captured at the first failing read.
    const char* m_overflowOp = nullptr;
    size_t m_overflowBitPos = 0;
    size_t m_overflowWantBits = 0;
    OverflowHandler m_overflowHandler;
};
