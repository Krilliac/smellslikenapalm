// src/Protocol/CompressionHandler.cpp
#include "Protocol/CompressionHandler.h"
#include "Utils/Logger.h"
#include <cstring>

// Include compression library headers
#include <zlib.h>
#include <lz4.h>

bool CompressionHandler::Compress(const std::vector<uint8_t>& input,
                                  std::vector<uint8_t>& output,
                                  CompressionAlgorithm algo,
                                  int level)
{
    switch (algo) {
        case CompressionAlgorithm::NONE:
            output = input;
            return true;

        case CompressionAlgorithm::ZLIB: {
            uLongf destLen = compressBound(input.size());
            output.resize(destLen);
            int ret = ::compress2(output.data(), &destLen,
                                  input.data(), input.size(),
                                  level < 0 ? Z_DEFAULT_COMPRESSION : level);
            if (ret != Z_OK) {
                Logger::Error("CompressionHandler: zlib compress2 failed (code %d)", ret);
                return false;
            }
            output.resize(destLen);
            return true;
        }

        case CompressionAlgorithm::LZ4: {
            int maxDst = LZ4_compressBound(input.size());
            output.resize(maxDst);
            int compressedSize = LZ4_compress_default(
                reinterpret_cast<const char*>(input.data()),
                reinterpret_cast<char*>(output.data()),
                input.size(), maxDst);
            if (compressedSize <= 0) {
                Logger::Error("CompressionHandler: LZ4_compress_default failed");
                return false;
            }
            output.resize(compressedSize);
            return true;
        }
    }
    return false;
}

bool CompressionHandler::Decompress(const std::vector<uint8_t>& input,
                                    std::vector<uint8_t>& output,
                                    CompressionAlgorithm algo)
{
    switch (algo) {
        case CompressionAlgorithm::NONE:
            output = input;
            return true;

        case CompressionAlgorithm::ZLIB: {
            // Need to know or guess decompressed size; here we try doubling until success
            uLongf destLen = input.size() * 2;
            output.resize(destLen);
            int ret = Z_BUF_ERROR;
            while (ret == Z_BUF_ERROR) {
                ret = ::uncompress(output.data(), &destLen,
                                   input.data(), input.size());
                if (ret == Z_BUF_ERROR) {
                    destLen *= 2;
                    output.resize(destLen);
                }
            }
            if (ret != Z_OK) {
                Logger::Error("CompressionHandler: zlib uncompress failed (code %d)", ret);
                return false;
            }
            output.resize(destLen);
            return true;
        }

        case CompressionAlgorithm::LZ4: {
            // LZ4 requires original size; assume first 4 bytes prefix it
            if (input.size() < 4) {
                Logger::Error("CompressionHandler: LZ4 input too small for size prefix");
                return false;
            }
            uint32_t origSize;
            std::memcpy(&origSize, input.data(), 4);
            output.resize(origSize);
            int decoded = LZ4_decompress_safe(
                reinterpret_cast<const char*>(input.data()) + 4,
                reinterpret_cast<char*>(output.data()),
                input.size() - 4, origSize);
            if (decoded < 0) {
                Logger::Error("CompressionHandler: LZ4_decompress_safe failed (code %d)", decoded);
                return false;
            }
            return true;
        }
    }
    return false;
}