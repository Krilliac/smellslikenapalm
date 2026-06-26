// src/Network/BitReader.cpp
//
// Implementation of the UE3 FBitReader-compatible bit deserializer. See
// BitReader.h. Mirrors BitWriter exactly so writer->reader round-trips.

#include "Network/BitReader.h"

#include "Utils/Logger.h"

#include <cstdio>
#include <cstring>

BitReader::BitReader(const uint8_t* data, size_t numBytes)
    : m_data(data), m_numBits(numBytes * 8) {}

BitReader::BitReader(const std::vector<uint8_t>& data)
    : m_data(data.empty() ? nullptr : data.data()), m_numBits(data.size() * 8) {}

BitReader::BitReader(const uint8_t* data, size_t numBytes, size_t validBits)
    : m_data(data), m_numBits(validBits <= numBytes * 8 ? validBits : numBytes * 8) {}

bool BitReader::EnsureBits(size_t count, const char* op) {
    if (m_overflow) {
        return false;
    }
    if (m_bitPos + count > m_numBits) {
        // Non-fatal: flag overflow, capture context, notify, and bail. The caller
        // returns a zero/default value; we never read out of bounds and never throw.
        m_overflow = true;
        m_overflowOp = (op != nullptr) ? op : "read";
        m_overflowBitPos = m_bitPos;
        m_overflowWantBits = count;
        ReportOverflow();
        return false;
    }
    return true;
}

void BitReader::ReportOverflow() {
    const size_t left = (m_overflowBitPos < m_numBits) ? (m_numBits - m_overflowBitPos) : 0;
    if (m_overflowHandler) {
        // Handler is responsible for its own logging; contractually must not throw.
        m_overflowHandler(m_overflowOp, m_overflowBitPos, m_overflowWantBits, m_numBits);
        return;
    }
    Logger::Warn(
        "[BitReader] overflow on '%s': want %zu bit(s) at pos %zu, but only %zu readable (%zu left)",
        m_overflowOp, m_overflowWantBits, m_overflowBitPos, m_numBits, left);
}

bool BitReader::ReadBit() {
    if (!EnsureBits(1, "ReadBit")) {
        return false;
    }
    const size_t byteIndex = m_bitPos >> 3;
    const int bitInByte = static_cast<int>(m_bitPos & 7);
    ++m_bitPos;
    return (m_data[byteIndex] >> bitInByte) & 1u;
}

uint64_t BitReader::ReadBits(int count) {
    if (count < 0) count = 0;
    if (count > 64) count = 64;
    if (!EnsureBits(static_cast<size_t>(count), "ReadBits")) {
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < count; ++i) {
        const size_t byteIndex = m_bitPos >> 3;
        const int bitInByte = static_cast<int>(m_bitPos & 7);
        const uint64_t bit = (m_data[byteIndex] >> bitInByte) & 1u;
        value |= (bit << i);
        ++m_bitPos;
    }
    return value;
}

uint32_t BitReader::SerializeInt(uint32_t maxValue) {
    // Mirror of BitWriter::SerializeInt.
    if (maxValue == 0) {
        return 0;
    }
    uint32_t value = 0;
    for (uint32_t mask = 1; (value + mask) < maxValue && mask != 0; mask <<= 1) {
        if (ReadBit()) {
            value |= mask;
        }
        if (m_overflow) {
            return value;
        }
    }
    return value;
}

uint8_t BitReader::ReadByte() {
    return static_cast<uint8_t>(ReadBits(8));
}

uint32_t BitReader::ReadUInt32() {
    const uint32_t b0 = ReadByte();
    const uint32_t b1 = ReadByte();
    const uint32_t b2 = ReadByte();
    const uint32_t b3 = ReadByte();
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

int32_t BitReader::ReadInt32() {
    const uint32_t u = ReadUInt32();
    int32_t v;
    std::memcpy(&v, &u, 4);
    return v;
}

uint64_t BitReader::ReadUInt64() {
    const uint64_t lo = ReadUInt32();
    const uint64_t hi = ReadUInt32();
    return lo | (hi << 32);
}

float BitReader::ReadFloat() {
    const uint32_t u = ReadUInt32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string BitReader::ReadString() {
    const int32_t len = ReadInt32();
    if (m_overflow) {
        return std::string();
    }
    if (len == 0) {
        return std::string(); // UE3 empty FString
    }

    if (len > 0) {
        // ANSI, `len` bytes INCLUDING the NUL terminator.
        const size_t count = static_cast<size_t>(len);
        // Bounds-check before reading the character payload.
        if (!EnsureBits(count * 8, "ReadString(ANSI)")) {
            return std::string();
        }
        std::string out;
        out.reserve(count > 0 ? count - 1 : 0);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t c = ReadByte();
            // Drop the trailing NUL terminator (last char).
            if (i + 1 < count) {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    // Negative => UCS-2/UTF-16LE, (-len) code units INCLUDING the NUL.
    const int64_t neg = -static_cast<int64_t>(len);
    const size_t units = static_cast<size_t>(neg);
    if (!EnsureBits(units * 16, "ReadString(UCS2)")) {
        return std::string();
    }
    std::string out;
    out.reserve(units > 0 ? units - 1 : 0);
    for (size_t i = 0; i < units; ++i) {
        const uint16_t cu = static_cast<uint16_t>(ReadByte() | (ReadByte() << 8));
        if (i + 1 < units) {
            // Narrow: preserve the low byte (lossy for >0xFF, acceptable here -
            // handshake strings are ASCII session ids / map names).
            out.push_back(static_cast<char>(cu & 0xFF));
        }
    }
    return out;
}

size_t BitReader::ReportTrailingBits(const char* context) const {
    const size_t left = BitsLeft();
    const char* ctx = (context != nullptr) ? context : "decode";
    if (m_overflow) {
        // Already overflowed; the bigger problem was logged at the failing read.
        // Surface it here too so end-of-decode callers get a single, clear signal.
        Logger::Warn("[BitReader] %s: ended in OVERFLOW state (last read '%s' at bit %zu)",
                     ctx, (m_overflowOp != nullptr) ? m_overflowOp : "?", m_overflowBitPos);
    } else if (left != 0) {
        Logger::Warn("[BitReader] %s: %zu trailing bit(s) after decode (pos %zu / %zu)"
                     " - likely misaligned/misparsed bunch",
                     ctx, left, m_bitPos, m_numBits);
    }
    return left;
}

std::string BitReader::DumpRemainingBits(size_t maxBytes) const {
    std::string out;
    const size_t left = BitsLeft();
    if (left == 0 || m_data == nullptr) {
        return out;
    }
    const size_t totalBytes = (left + 7) / 8;        // bytes needed to hold the tail
    const size_t n = (totalBytes < maxBytes) ? totalBytes : maxBytes;
    char buf[8];
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = 0;
        for (int bit = 0; bit < 8; ++bit) {
            const size_t pos = m_bitPos + i * 8 + static_cast<size_t>(bit);
            if (pos >= m_numBits) {
                break;
            }
            const size_t byteIndex = pos >> 3;
            const int bitInByte = static_cast<int>(pos & 7);
            const uint8_t v = (m_data[byteIndex] >> bitInByte) & 1u;
            b |= static_cast<uint8_t>(v << bit);     // LSB-first, mirrors wire order
        }
        std::snprintf(buf, sizeof(buf), "%02X ", b);
        out += buf;
    }
    if (n < totalBytes) {
        out += "...";
    }
    return out;
}
