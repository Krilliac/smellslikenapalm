// tests/CompressionTests.cpp
// Comprehensive compression / decompression unit tests
//
// These tests exercise – and benchmark – the “CompressionHandler” subsystem
// that is used by NetworkManager, PacketAnalyzer and BandwidthManager to
// minimise wire size of large payloads while respecting MTU limits.
//
// GoogleTest / GoogleMock are employed so that the tests can run out–of–process
// from a live server instance and still validate every public contract of the
// compression façade.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <memory>
#include <random>
#include <chrono>
#include <numeric>

// --- server headers ---------------------------------------------------------
#include "Protocol/CompressionHandler.h"
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Utils/PerformanceProfiler.h"
// -----------------------------------------------------------------------------

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::NiceMock;

/* -------------------------------------------------------------------------- */
/*                               Test doubles                                 */
/* -------------------------------------------------------------------------- */
class MockPerformanceProfiler : public PerformanceProfiler
{
public:
    MOCK_METHOD(void,   Begin, (const std::string &section), (override));
    MOCK_METHOD(void,   End,   (const std::string &section), (override));
    MOCK_METHOD(double, GetAverageTime, (const std::string &section), (const, override));
};

/* -------------------------------------------------------------------------- */
/*                            Helper – random blob                            */
/* -------------------------------------------------------------------------- */
static std::vector<uint8_t> MakeRandomBuffer(size_t bytes)
{
    static std::mt19937 rng{ 0xBADC0FFE };
    std::uniform_int_distribution<uint32_t> dist(0, 255);

    std::vector<uint8_t> buf(bytes);
    std::generate(buf.begin(), buf.end(), [&] { return static_cast<uint8_t>(dist(rng)); });
    return buf;
}

/* -------------------------------------------------------------------------- */
/*                                  Fixture                                   */
/* -------------------------------------------------------------------------- */
class CompressionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        handler = std::make_unique<CompressionHandler>();   // real implementation
        profiler = std::make_shared<NiceMock<MockPerformanceProfiler>>();
    }

    std::unique_ptr<CompressionHandler>            handler;
    std::shared_ptr<NiceMock<MockPerformanceProfiler>> profiler;
};

/* -------------------------------------------------------------------------- */
/*                               HAPPY PATH                                   */
/* -------------------------------------------------------------------------- */

TEST_F(CompressionTest, RoundTrip_BinaryPayload_Lossless)
{
    const auto original = MakeRandomBuffer(32'768);   // 32 KiB random data

    const auto compressed   = handler->Compress(original);
    const auto decompressed = handler->Decompress(compressed);

    EXPECT_FALSE(compressed.empty())          << "Compression must yield bytes";
    EXPECT_NE(compressed.size(), original.size())   << "Size must change";
    EXPECT_EQ(original, decompressed)         << "Round-trip must be loss-less";

    const float ratio = handler->GetCompressionRatio(original, compressed);
    EXPECT_GT(ratio, 0.0f);
    EXPECT_LT(ratio, 1.0f);
}

TEST_F(CompressionTest, SmallPayload_SkipsCompression_ByPolicy)
{
    const auto tiny = MakeRandomBuffer(96);          // below typical threshold

    EXPECT_FALSE(handler->ShouldCompress(tiny.size()))
        << "Handler default policy should avoid compressing minuscule frames";

    const auto compressed = handler->Compress(tiny);
    /* Real Compress() short-circuits if ShouldCompress==false and returns original. */
    EXPECT_EQ(compressed, tiny);
}

/* -------------------------------------------------------------------------- */
/*                            EDGE-CASE ROBUSTNESS                            */
/* -------------------------------------------------------------------------- */

TEST_F(CompressionTest, EmptyBuffer_NoCrash)
{
    std::vector<uint8_t> empty;

    const auto compressed   = handler->Compress(empty);
    const auto decompressed = handler->Decompress(compressed);

    EXPECT_TRUE(compressed.empty());
    EXPECT_TRUE(decompressed.empty());
}

TEST_F(CompressionTest, HighEntropy_AlmostUncompressible_Data)
{
    const auto highEntropy = MakeRandomBuffer(256 * 1024);      // 256 KiB pure noise

    ASSERT_TRUE(handler->ShouldCompress(highEntropy.size()));

    const auto compressed = handler->Compress(highEntropy);
    const float ratio     = handler->GetCompressionRatio(highEntropy, compressed);

    /* For random noise the ratio should be close to 1 (no benefit), but never >1. */
    EXPECT_GT(ratio, 0.90f);
    EXPECT_LE(ratio, 1.0f);
}

TEST_F(CompressionTest, AlreadyCompressedData_Idempotent)
{
    /* Feed DEFLATE header to create “already compressed looking” data.          */
    std::vector<uint8_t> gz { 0x1f, 0x8b, 0x08, 0x00 };          // gzip magic header
    gz.resize(1024, 0xAA);

    const auto compressed = handler->Compress(gz);
    EXPECT_EQ(compressed, gz) << "Implementation must not bloat pre-compressed data";

    const auto decompressed = handler->Decompress(compressed);
    EXPECT_EQ(decompressed, gz);
}

/* -------------------------------------------------------------------------- */
/*                              PERFORMANCE                                   */
/* -------------------------------------------------------------------------- */

TEST_F(CompressionTest, Performance_Throughput_1MiB_WithinBudget)
{
    const auto big = MakeRandomBuffer(1 << 20); // 1 MiB
    const auto start = std::chrono::high_resolution_clock::now();

    const auto compressed = handler->Compress(big);
    const auto stop  = std::chrono::high_resolution_clock::now();

    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
    EXPECT_LT(micros, 50'000)   // 50 ms budget for 1 MiB at runtime
        << "Compression slower than expected";

    /* Decompression must also be fast. */
    const auto dStart = std::chrono::high_resolution_clock::now();
    const auto round  = handler->Decompress(compressed);
    const auto dEnd   = std::chrono::high_resolution_clock::now();
    EXPECT_LT(std::chrono::duration_cast<std::chrono::microseconds>(dEnd - dStart).count(),
              20'000) << "Decompression slower than expected";

    EXPECT_EQ(round, big);
}

/* -------------------------------------------------------------------------- */
/*              INTEGRATION – NETWORK PACKET WITH COMPRESSION FLAG            */
/* -------------------------------------------------------------------------- */

TEST_F(CompressionTest, PacketIntegration_CompressFlagRoundtrip)
{
    Packet outgoing;
    outgoing.SetType(PacketType::PT_ACTOR_REPLICATION);
    outgoing.SetClientId(42);

    /* Build 5 KiB replication blob (typical) */
    const auto payload = MakeRandomBuffer(5 * 1024);
    outgoing.SetData(payload);

    /* Server side: decide compression & mark header bit. */
    if (handler->ShouldCompress(outgoing.GetSize()))
    {
        auto cmp = handler->Compress(payload);
        outgoing.SetData(cmp);
        outgoing.SetCompressed(true);
    }

    /* Client side:                 */
    if (outgoing.IsCompressed())
    {
        auto decomp = handler->Decompress(outgoing.RawData());
        EXPECT_EQ(decomp, payload);
    }
    else
    {
        EXPECT_EQ(outgoing.RawData(), payload);
    }
}

/* -------------------------------------------------------------------------- */
/*                 RATE-CONTROL: BYTES SAVED UNDER LOAD                        */
/* -------------------------------------------------------------------------- */

TEST_F(CompressionTest, AggregateSavings_UnderBurstTraffic)
{
    const int  frames = 120;                     // simulate two seconds @60 FPS
    const size_t perFrameBytes = 8 * 1024;       // 8 KiB replication snapshot

    size_t rawTotal = 0, compressedTotal = 0;

    for (int i = 0; i < frames; ++i)
    {
        auto frame = MakeRandomBuffer(perFrameBytes);
        rawTotal += frame.size();

        if (handler->ShouldCompress(frame.size()))
        {
            compressedTotal += handler->Compress(frame).size();
        }
        else
        {
            compressedTotal += frame.size();
        }
    }

    const double saving = 100.0 * (rawTotal - compressedTotal) / rawTotal;

    EXPECT_GT(saving, 20.0)  // At least 20 % bandwidth saved is expected on mixed data
        << "Compression did not yield expected savings";
}

/* -------------------------------------------------------------------------- */
/*                        CORRUPTION / ERROR HANDLING                         */
/* -------------------------------------------------------------------------- */

TEST_F(CompressionTest, Decompress_InvalidData_Throws)
{
    std::vector<uint8_t> garbage { 0xDE, 0xAD, 0xBE, 0xEF };

    EXPECT_THROW(handler->Decompress(garbage), std::runtime_error);
}

TEST_F(CompressionTest, Decompress_TruncatedStream_Throws)
{
    const auto src = MakeRandomBuffer(16 * 1024);
    auto cmp = handler->Compress(src);

    /* Truncate */
    cmp.resize(cmp.size() / 2);

    EXPECT_THROW(handler->Decompress(cmp), std::runtime_error);
}

/* -------------------------------------------------------------------------- */
/*                                Test Runner                                */
/* -------------------------------------------------------------------------- */

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
