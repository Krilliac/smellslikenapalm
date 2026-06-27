// tests/CompressionHandlerTests.cpp
//
// Native unit tests for src/Protocol/CompressionHandler.{h,cpp} — the static
// Compress/Decompress facade. Exercises the real static API (the legacy
// CompressionTests.cpp assumed a non-existent instance API and was excluded).
//
// zlib is linked into rs2v_core (RS2V_HAS_ZLIB), so the ZLIB path is exercised
// directly; the NONE passthrough path is always available.

#include "TestFramework.h"

#include "Protocol/CompressionHandler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
// A sizable, highly-compressible buffer.
std::vector<uint8_t> Repetitive(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>('A' + (i % 4));
    return v;
}
}  // namespace

TEST(CompressionHandler, NonePassthroughRoundTrip) {
    std::vector<uint8_t> in = Bytes("hello passthrough");
    std::vector<uint8_t> comp, out;
    ASSERT_TRUE(CompressionHandler::Compress(in, comp, CompressionAlgorithm::NONE));
    EXPECT_EQ(comp, in);  // NONE is an identity transform
    ASSERT_TRUE(CompressionHandler::Decompress(comp, out, CompressionAlgorithm::NONE));
    EXPECT_EQ(out, in);
}

TEST(CompressionHandler, ZlibRoundTrip) {
    std::vector<uint8_t> in = Repetitive(8192);
    std::vector<uint8_t> comp, out;

    ASSERT_TRUE(CompressionHandler::Compress(in, comp, CompressionAlgorithm::ZLIB));
    ASSERT_TRUE(CompressionHandler::Decompress(comp, out, CompressionAlgorithm::ZLIB));
    EXPECT_EQ(out, in) << "zlib round-trip must reproduce the original bytes";
}

TEST(CompressionHandler, ZlibActuallyCompressesRepetitiveData) {
    std::vector<uint8_t> in = Repetitive(16384);
    std::vector<uint8_t> comp;
    ASSERT_TRUE(CompressionHandler::Compress(in, comp, CompressionAlgorithm::ZLIB));
    // Highly-repetitive input must shrink substantially.
    EXPECT_LT(comp.size(), in.size());
}

TEST(CompressionHandler, ZlibEmptyInputRoundTrips) {
    std::vector<uint8_t> in;
    std::vector<uint8_t> comp, out;
    ASSERT_TRUE(CompressionHandler::Compress(in, comp, CompressionAlgorithm::ZLIB));
    ASSERT_TRUE(CompressionHandler::Decompress(comp, out, CompressionAlgorithm::ZLIB));
    EXPECT_TRUE(out.empty());
}

RS2V_TEST_MAIN()
