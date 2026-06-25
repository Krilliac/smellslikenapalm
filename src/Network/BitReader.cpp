// src/Network/BitReader.cpp
//
// Implementation of the UE3 FBitReader-compatible bit deserializer. See
// BitReader.h. Mirrors BitWriter exactly so writer->reader round-trips.

#include "Network/BitReader.h"

#include <cstring>

BitReader::BitReader(const uint8_t* data, size_t numBytes)
    : m_data(data), m_numBits(numBytes * 8) {}

BitReader::BitReader(const std::vector<uint8_t>& data)
    : m_data(data.empty() ? nullptr : data.data()), m_numBits(data.size() * 8) {}

bool BitReader::EnsureBits(size_t count) {
    if (m_overflow) {
        return false;
    }
    if (m_bitPos + count > m_numBits) {
        m_overflow = true;
        return false;
    }
    return true;
}

bool BitReader::ReadBit() {
    if (!EnsureBits(1)) {
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
    if (!EnsureBits(static_cast<size_t>(count))) {
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
        if (!EnsureBits(count * 8)) {
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
    if (!EnsureBits(units * 16)) {
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
