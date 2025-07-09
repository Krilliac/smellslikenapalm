// src/Protocol/CompressionHandler.h
#pragma once

#include <vector>
#include <cstdint>
#include <string>

// Compression algorithms supported
enum class CompressionAlgorithm {
    NONE,
    ZLIB,
    LZ4
};

class CompressionHandler {
public:
    // Compress raw data with specified algorithm.
    // Returns true on success, false on failure.
    static bool Compress(const std::vector<uint8_t>& input,
                         std::vector<uint8_t>& output,
                         CompressionAlgorithm algo = CompressionAlgorithm::ZLIB,
                         int level = -1  // algorithm-specific compression level
    );

    // Decompress data compressed by Compress().
    // Returns true on success, false on failure.
    static bool Decompress(const std::vector<uint8_t>& input,
                           std::vector<uint8_t>& output,
                           CompressionAlgorithm algo = CompressionAlgorithm::ZLIB
    );
};