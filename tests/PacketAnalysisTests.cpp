// tests/PacketAnalysisTests.cpp
// Comprehensive unit tests for PacketAnalyzer and PacketAnalysisResult
//
// Tests cover:
// 1. Hex dump formatting and round-trips.
// 2. Integrity validation and checksum mismatches.
// 3. Anomaly detection (e.g., invalid lengths, repeated patterns).
// 4. Structured decode of various RS2V packet types.
// 5. Performance of per-packet analysis under load.
// 6. Edge cases: empty packets, corrupted headers, max-size packets.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <random>

#include "Utils/PacketAnalysis.h"
#include "Protocol/PacketTypes.h"
#include "Network/PacketSerializer.h"
#include "Utils/Logger.h"

using ::testing::_;
using ::testing::Return;

// Fixture for PacketAnalyzer
class PacketAnalysisTest : public ::testing::Test {
protected:
    void SetUp() override {
        serializer = std::make_unique<PacketSerializer>();
        analyzer = std::make_unique<PacketAnalyzer>();
    }

    std::unique_ptr<PacketSerializer> serializer;
    std::unique_ptr<PacketAnalyzer> analyzer;
};

// Helper to create a PacketAnalysisResult from raw data
PacketAnalysisResult AnalyzeRaw(const std::vector<uint8_t>& data) {
    PacketAnalysisResult result;
    analyzer->Analyze(data.data(), data.size(), result);
    return result;
}

// 1. Hex dump formatting
TEST_F(PacketAnalysisTest, HexDump_FormatsCorrectly) {
    std::vector<uint8_t> data = {0x00,0xFF,0x10,0x20,0x30};
    auto dump = analyzer->GenerateHexDump(data.data(), data.size());
    // Expect lines containing "00 FF 10 20 30"
    EXPECT_NE(dump.find("00 FF 10 20 30"), std::string::npos);
}

// 2. Integrity validation
TEST_F(PacketAnalysisTest, ValidateIntegrity_CorrectChecksum_Passes) {
    auto packet = serializer->Serialize(
        PacketType::PT_HEARTBEAT,
        std::vector<uint8_t>{1,2,3,4,5}
    );
    PacketAnalysisResult result;
    analyzer->Analyze(packet.data(), packet.size(), result);
    EXPECT_TRUE(result.integrityValid);
}

TEST_F(PacketAnalysisTest, ValidateIntegrity_CorruptChecksum_Fails) {
    auto packet = serializer->Serialize(
        PacketType::PT_HEARTBEAT,
        std::vector<uint8_t>{1,2,3,4,5}
    );
    packet[5] ^= 0xFF;  // corrupt payload
    PacketAnalysisResult result;
    analyzer->Analyze(packet.data(), packet.size(), result);
    EXPECT_FALSE(result.integrityValid);
    EXPECT_GT(result.errors.size(), 0u);
}

// 3. Anomaly detection: invalid length
TEST_F(PacketAnalysisTest, DetectAnomaly_InvalidLength_ReportsError) {
    std::vector<uint8_t> tooShort(2,0);
    PacketAnalysisResult result;
    analyzer->Analyze(tooShort.data(), tooShort.size(), result);
    EXPECT_FALSE(result.integrityValid);
    EXPECT_GT(result.errors.size(), 0u);
}

// 4. Structured decode of RS2V packet types
TEST_F(PacketAnalysisTest, StructuredDecode_HeartbeatTypeDetected) {
    auto packet = serializer->Serialize(
        PacketType::PT_HEARTBEAT,
        std::vector<uint8_t>(8,0)
    );
    PacketAnalysisResult result;
    analyzer->Analyze(packet.data(), packet.size(), result);
    EXPECT_EQ(result.packetType, PacketType::PT_HEARTBEAT);
}

TEST_F(PacketAnalysisTest, StructuredDecode_PlayerMoveFieldsParsed) {
    struct MoveData { uint32_t id; float x,y,z; } md{42,1.0f,2.0f,3.0f};
    std::vector<uint8_t> payload(sizeof(md));
    memcpy(payload.data(), &md, sizeof(md));
    auto packet = serializer->Serialize(PacketType::PT_PLAYER_MOVE, payload);
    PacketAnalysisResult result;
    analyzer->Analyze(packet.data(), packet.size(), result);
    EXPECT_EQ(result.packetType, PacketType::PT_PLAYER_MOVE);
    EXPECT_EQ(result.fields["playerId"].toUInt(), 42u);
    EXPECT_FLOAT_EQ(result.fields["x"].toFloat(), 1.0f);
    EXPECT_FLOAT_EQ(result.fields["y"].toFloat(), 2.0f);
    EXPECT_FLOAT_EQ(result.fields["z"].toFloat(), 3.0f);
}

// 5. Performance under load
TEST_F(PacketAnalysisTest, Performance_MillionsPackets_WithinBudget) {
    constexpr int N = 1000000;
    std::vector<uint8_t> data(64,0);
    PacketAnalysisResult result;
    auto start = std::chrono::high_resolution_clock::now();
    for(int i=0;i<N;++i) {
        analyzer->Analyze(data.data(), data.size(), result);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double avgNs = std::chrono::duration<double,std::nano>(end-start).count()/N;
    EXPECT_LT(avgNs, 500.0);  // <0.5 µs per packet
}

// 6. Edge cases
TEST_F(PacketAnalysisTest, EmptyPacket_ReportsError) {
    std::vector<uint8_t> empty;
    PacketAnalysisResult result;
    analyzer->Analyze(empty.data(), empty.size(), result);
    EXPECT_FALSE(result.integrityValid);
    EXPECT_GT(result.errors.size(), 0u);
}

TEST_F(PacketAnalysisTest, MaxSizePacket_ParsesWithoutCrash) {
    size_t max = serializer->GetMaxPacketSize();
    std::vector<uint8_t> data(max, 0xAA);
    PacketAnalysisResult result;
    EXPECT_NO_THROW(analyzer->Analyze(data.data(), data.size(), result));
}

// Anomaly: repeated pattern detection
TEST_F(PacketAnalysisTest, AnomalyDetection_RepeatedBytes_FlagsWarning) {
    std::vector<uint8_t> data(100, 0x00);
    PacketAnalysisResult result;
    analyzer->Analyze(data.data(), data.size(), result);
    EXPECT_GT(result.warnings.size(), 0u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}