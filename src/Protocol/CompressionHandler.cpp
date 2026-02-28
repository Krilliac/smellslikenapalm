// src/Protocol/CompressionHandler.cpp
#include "Protocol/CompressionHandler.h"
#include "Utils/Logger.h"
#include <cstring>

// Include compression library headers conditionally
#ifdef RS2V_HAS_ZLIB
#include <zlib.h>
#endif

bool CompressionHandler::Compress(const std::vector<uint8_t>& input,
                                  std::vector<uint8_t>& output,
                                  CompressionAlgorithm algo,
                                  int level)
{
    Logger::Trace("[CompressionHandler::Compress] entry — input.size()=%zu, algo=%d, level=%d",
                  input.size(), static_cast<int>(algo), level);
    switch (algo) {
        case CompressionAlgorithm::NONE:
            Logger::Debug("[CompressionHandler::Compress] algorithm=NONE — copying input directly to output (passthrough)");
            output = input;
            Logger::Trace("[CompressionHandler::Compress] exit — returning true (passthrough, %zu bytes)", output.size());
            return true;

        case CompressionAlgorithm::ZLIB: {
            Logger::Debug("[CompressionHandler::Compress] algorithm=ZLIB — attempting zlib compression with level=%d", level);
#ifdef RS2V_HAS_ZLIB
            uLongf destLen = compressBound(input.size());
            Logger::Debug("[CompressionHandler::Compress] zlib compressBound=%lu for input size=%zu",
                          (unsigned long)destLen, input.size());
            output.resize(destLen);
            int effectiveLevel = level < 0 ? Z_DEFAULT_COMPRESSION : level;
            Logger::Debug("[CompressionHandler::Compress] effective compression level=%d (Z_DEFAULT_COMPRESSION=%d)",
                          effectiveLevel, Z_DEFAULT_COMPRESSION);
            int ret = ::compress2(output.data(), &destLen,
                                  input.data(), input.size(),
                                  effectiveLevel);
            if (ret != Z_OK) {
                Logger::Error("CompressionHandler: zlib compress2 failed (code %d)", ret);
                Logger::Debug("[CompressionHandler::Compress] zlib error details: ret=%d, input.size()=%zu, destLen=%lu",
                              ret, input.size(), (unsigned long)destLen);
                Logger::Trace("[CompressionHandler::Compress] exit — returning false (zlib compress2 failed)");
                return false;
            }
            output.resize(destLen);
            float ratio = input.size() > 0 ? (float)destLen / (float)input.size() * 100.0f : 0.0f;
            Logger::Debug("[CompressionHandler::Compress] zlib compression successful: %zu -> %lu bytes (%.1f%% of original)",
                          input.size(), (unsigned long)destLen, ratio);
            Logger::Info("[CompressionHandler::Compress] compressed %zu bytes to %lu bytes via zlib (%.1f%% ratio)",
                         input.size(), (unsigned long)destLen, ratio);
            Logger::Trace("[CompressionHandler::Compress] exit — returning true (zlib compression complete)");
            return true;
#else
            Logger::Error("CompressionHandler: zlib not available");
            Logger::Debug("[CompressionHandler::Compress] RS2V_HAS_ZLIB not defined — zlib support not compiled in");
            output = input;
            Logger::Trace("[CompressionHandler::Compress] exit — returning false (zlib not available)");
            return false;
#endif
        }

        case CompressionAlgorithm::LZ4: {
            Logger::Debug("[CompressionHandler::Compress] algorithm=LZ4 — LZ4 support not available in this build");
            // LZ4 not available in this build
            Logger::Error("CompressionHandler: LZ4 not available");
            output = input;
            Logger::Trace("[CompressionHandler::Compress] exit — returning false (LZ4 not available)");
            return false;
        }
    }
    Logger::Error("[CompressionHandler::Compress] unhandled compression algorithm: %d", static_cast<int>(algo));
    Logger::Trace("[CompressionHandler::Compress] exit — returning false (unhandled algorithm)");
    return false;
}

bool CompressionHandler::Decompress(const std::vector<uint8_t>& input,
                                    std::vector<uint8_t>& output,
                                    CompressionAlgorithm algo)
{
    Logger::Trace("[CompressionHandler::Decompress] entry — input.size()=%zu, algo=%d",
                  input.size(), static_cast<int>(algo));
    switch (algo) {
        case CompressionAlgorithm::NONE:
            Logger::Debug("[CompressionHandler::Decompress] algorithm=NONE — copying input directly to output (passthrough)");
            output = input;
            Logger::Trace("[CompressionHandler::Decompress] exit — returning true (passthrough, %zu bytes)", output.size());
            return true;

        case CompressionAlgorithm::ZLIB: {
            Logger::Debug("[CompressionHandler::Decompress] algorithm=ZLIB — attempting zlib decompression");
#ifdef RS2V_HAS_ZLIB
            uLongf destLen = input.size() * 2;
            Logger::Debug("[CompressionHandler::Decompress] initial destLen guess=%lu (2x input)", (unsigned long)destLen);
            output.resize(destLen);
            int ret = Z_BUF_ERROR;
            int attempts = 0;
            while (ret == Z_BUF_ERROR) {
                attempts++;
                Logger::Debug("[CompressionHandler::Decompress] zlib uncompress attempt %d with destLen=%lu",
                              attempts, (unsigned long)destLen);
                ret = ::uncompress(output.data(), &destLen,
                                   input.data(), input.size());
                if (ret == Z_BUF_ERROR) {
                    destLen *= 2;
                    Logger::Debug("[CompressionHandler::Decompress] Z_BUF_ERROR — doubling buffer to destLen=%lu",
                                  (unsigned long)destLen);
                    output.resize(destLen);
                }
            }
            if (ret != Z_OK) {
                Logger::Error("CompressionHandler: zlib uncompress failed (code %d)", ret);
                Logger::Debug("[CompressionHandler::Decompress] zlib error details: ret=%d, input.size()=%zu, attempts=%d, final destLen=%lu",
                              ret, input.size(), attempts, (unsigned long)destLen);
                Logger::Trace("[CompressionHandler::Decompress] exit — returning false (zlib uncompress failed)");
                return false;
            }
            output.resize(destLen);
            float ratio = input.size() > 0 ? (float)destLen / (float)input.size() : 0.0f;
            Logger::Debug("[CompressionHandler::Decompress] zlib decompression successful in %d attempts: %zu -> %lu bytes (%.1fx expansion)",
                          attempts, input.size(), (unsigned long)destLen, ratio);
            Logger::Info("[CompressionHandler::Decompress] decompressed %zu bytes to %lu bytes via zlib (%.1fx expansion)",
                         input.size(), (unsigned long)destLen, ratio);
            Logger::Trace("[CompressionHandler::Decompress] exit — returning true (zlib decompression complete)");
            return true;
#else
            Logger::Error("CompressionHandler: zlib not available");
            Logger::Debug("[CompressionHandler::Decompress] RS2V_HAS_ZLIB not defined — zlib support not compiled in");
            output = input;
            Logger::Trace("[CompressionHandler::Decompress] exit — returning false (zlib not available)");
            return false;
#endif
        }

        case CompressionAlgorithm::LZ4: {
            Logger::Debug("[CompressionHandler::Decompress] algorithm=LZ4 — LZ4 support not available in this build");
            Logger::Error("CompressionHandler: LZ4 not available");
            output = input;
            Logger::Trace("[CompressionHandler::Decompress] exit — returning false (LZ4 not available)");
            return false;
        }
    }
    Logger::Error("[CompressionHandler::Decompress] unhandled decompression algorithm: %d", static_cast<int>(algo));
    Logger::Trace("[CompressionHandler::Decompress] exit — returning false (unhandled algorithm)");
    return false;
}
