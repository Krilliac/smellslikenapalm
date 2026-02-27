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
    switch (algo) {
        case CompressionAlgorithm::NONE:
            output = input;
            return true;

        case CompressionAlgorithm::ZLIB: {
#ifdef RS2V_HAS_ZLIB
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
#else
            Logger::Error("CompressionHandler: zlib not available");
            output = input;
            return false;
#endif
        }

        case CompressionAlgorithm::LZ4: {
            // LZ4 not available in this build
            Logger::Error("CompressionHandler: LZ4 not available");
            output = input;
            return false;
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
#ifdef RS2V_HAS_ZLIB
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
#else
            Logger::Error("CompressionHandler: zlib not available");
            output = input;
            return false;
#endif
        }

        case CompressionAlgorithm::LZ4: {
            Logger::Error("CompressionHandler: LZ4 not available");
            output = input;
            return false;
        }
    }
    return false;
}
