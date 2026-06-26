// src/Network/BitWriter.cpp
//
// Implementation of the UE3 FBitWriter-compatible bit serializer. See
// BitWriter.h for the wire conventions and spec citations.

#include "Network/BitWriter.h"

#include "Utils/Logger.h"

#include <cstring>

void BitWriter::CheckBunchLimit() {
    // Non-fatal: log exactly once when the running bit count first exceeds the
    // sane bunch bound. We do NOT truncate or alter output here.
    if (!m_exceededBunchLimit && m_numBits > kMaxSaneBunchBits) {
        m_exceededBunchLimit = true;
        m_invariantViolated = true;
        Logger::Warn("BitWriter: bunch size invariant exceeded: %zu bits written "
                     "(> %zu). Continuing without truncation.",
                     m_numBits, static_cast<size_t>(kMaxSaneBunchBits));
    }
}

void BitWriter::WriteBit(bool bit) {
    const size_t byteIndex = m_numBits >> 3;
    const int bitInByte = static_cast<int>(m_numBits & 7);
    if (byteIndex >= m_buffer.size()) {
        m_buffer.push_back(0);
    }
    if (bit) {
        // LSB-first within the byte: bit N occupies mask (1 << N).
        m_buffer[byteIndex] |= static_cast<uint8_t>(1u << bitInByte);
    }
    ++m_numBits;
    CheckBunchLimit();
}

void BitWriter::WriteBits(uint64_t value, int count) {
    // Invariant: count must be in [0,64]. An out-of-range count indicates a
    // caller bug; LOG it and clamp rather than reading/writing garbage. The
    // clamp preserves the historical (silent) behavior so wire output for valid
    // callers is unchanged.
    if (count < 0 || count > 64) {
        m_invariantViolated = true;
        Logger::Warn("BitWriter::WriteBits: count %d out of range [0,64]; clamping.",
                     count);
        if (count < 0) count = 0;
        if (count > 64) count = 64;
    }
    if (m_trace) {
        Logger::Trace("BitWriter: WriteBits value=0x%llx count=%d @bit %zu",
                      static_cast<unsigned long long>(value), count, m_numBits);
    }
    for (int i = 0; i < count; ++i) {
        WriteBit((value >> i) & 1u);
    }
}

void BitWriter::SerializeInt(uint32_t value, uint32_t maxValue) {
    // UE3 FArchive::SerializeInt bounded-int encoding. `maxValue` is the
    // exclusive upper bound (number of distinct values). For each bit position,
    // we emit a bit only while the running mask is strictly below maxValue, and
    // we set that bit if `value` has it AND including it keeps us < maxValue.
    //
    // Reference algorithm (UE3):
    //   for (Mask = 1; (NewValue + Mask) < Max && Mask; Mask <<= 1)
    //       { write bit (Value & Mask); if set NewValue |= Mask; }
    if (maxValue == 0) {
        return; // degenerate; nothing to encode
    }
    // Invariant: value must be in [0, maxValue). A value at/above the exclusive
    // bound cannot be represented by UE3's bounded encoding and would silently
    // truncate on the wire, desyncing the client. LOG it and clamp to the
    // largest representable value (maxValue-1) so we stay in sync rather than
    // crash. This does not change behavior for valid (value < maxValue) callers.
    if (value >= maxValue) {
        m_invariantViolated = true;
        Logger::Warn("BitWriter::SerializeInt: value %u >= maxValue %u; "
                     "clamping to %u.", value, maxValue, maxValue - 1);
        value = maxValue - 1;
    }
    if (m_trace) {
        Logger::Trace("BitWriter: SerializeInt value=%u max=%u @bit %zu",
                      value, maxValue, m_numBits);
    }
    uint32_t newValue = 0;
    for (uint32_t mask = 1; (newValue + mask) < maxValue && mask != 0; mask <<= 1) {
        const bool bit = (value & mask) != 0;
        WriteBit(bit);
        if (bit) {
            newValue |= mask;
        }
    }
}

void BitWriter::WriteByte(uint8_t v) {
    WriteBits(v, 8);
}

void BitWriter::WriteUInt32(uint32_t v) {
    // Little-endian raw bytes; each byte LSB-first => byte-order preserved.
    WriteByte(static_cast<uint8_t>(v & 0xFF));
    WriteByte(static_cast<uint8_t>((v >> 8) & 0xFF));
    WriteByte(static_cast<uint8_t>((v >> 16) & 0xFF));
    WriteByte(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void BitWriter::WriteInt32(int32_t v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    WriteUInt32(u);
}

void BitWriter::WriteUInt64(uint64_t v) {
    WriteUInt32(static_cast<uint32_t>(v & 0xFFFFFFFFull));
    WriteUInt32(static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

void BitWriter::WriteFloat(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    WriteUInt32(u);
}

void BitWriter::WriteRawBytes(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        WriteByte(data[i]);
    }
}

void BitWriter::WriteString(const std::string& s) {
    if (s.empty()) {
        // UE3: empty FString => length 0, no characters.
        WriteInt32(0);
        return;
    }
    // Positive length => ANSI, INCLUDING the NUL terminator.
    const int32_t len = static_cast<int32_t>(s.size()) + 1; // +1 for NUL
    WriteInt32(len);
    for (char c : s) {
        WriteByte(static_cast<uint8_t>(c));
    }
    WriteByte(0); // NUL terminator
}

void BitWriter::WriteStringUCS2(const std::wstring& s) {
    if (s.empty()) {
        WriteInt32(0);
        return;
    }
    // Negative length => UCS-2/UTF-16LE, (-length) units INCLUDING the NUL.
    const int32_t units = static_cast<int32_t>(s.size()) + 1; // +1 for NUL
    WriteInt32(-units);
    for (wchar_t wc : s) {
        const uint16_t cu = static_cast<uint16_t>(wc);
        WriteByte(static_cast<uint8_t>(cu & 0xFF));
        WriteByte(static_cast<uint8_t>((cu >> 8) & 0xFF));
    }
    WriteByte(0); // NUL terminator, low byte
    WriteByte(0); // NUL terminator, high byte
}

std::vector<uint8_t> BitWriter::GetBytes() const {
    // The buffer already holds packed bytes with the final byte zero-padded
    // (unused high bits remain 0 because WriteBit only sets bits it needs).
    return m_buffer;
}
