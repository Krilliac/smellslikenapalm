#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <functional>
#include "Network/NetworkPacket.h"
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"

// Analysis configuration flags
enum class AnalysisFlags : uint32_t {
    NONE = 0,
    HEX_DUMP = 1 << 0,
    STRUCTURED_DECODE = 1 << 1,
    TIMING_ANALYSIS = 1 << 2,
    COMPRESSION_ANALYSIS = 1 << 3,
    SECURITY_ANALYSIS = 1 << 4,
    PATTERN_DETECTION = 1 << 5,
    BANDWIDTH_TRACKING = 1 << 6,
    PROTOCOL_VALIDATION = 1 << 7,
    PERFORMANCE_METRICS = 1 << 8,
    ALL = 0xFFFFFFFF
};

// Analysis result structure
struct PacketAnalysisResult {
    std::string packetTag;
    PacketType packetType;
    size_t payloadSize;
    std::chrono::microseconds processingTime;
    std::string hexDump;
    std::string structuredData;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool isValid;
    float compressionRatio;
    uint32_t clientId;
    std::string context;
};

// Traffic pattern analysis
struct TrafficPattern {
    std::string patternType;
    uint32_t frequency;
    std::chrono::steady_clock::time_point firstSeen;
    std::chrono::steady_clock::time_point lastSeen;
    std::vector<uint32_t> clientIds;
    bool suspicious;
};

class PacketAnalyzer {
public:
    using AnalysisCallback = std::function<void(const PacketAnalysisResult&)>;

    explicit PacketAnalyzer();
    ~PacketAnalyzer();

    // Initialize with configuration
    void Initialize(const std::string& outputDir = "packet_analysis");
    void Shutdown();

    // Core analysis function
    PacketAnalysisResult AnalyzePacket(
        const std::vector<uint8_t>& data,
        const std::string& context,
        uint32_t clientId = 0,
        AnalysisFlags flags = AnalysisFlags::ALL);

    // Overloaded for Packet objects
    PacketAnalysisResult AnalyzePacket(
        const Packet& packet,
        const PacketMetadata& metadata,
        const std::string& context,
        AnalysisFlags flags = AnalysisFlags::ALL);

    // Configuration
    void SetAnalysisFlags(AnalysisFlags flags);
    void SetOutputDirectory(const std::string& dir);
    void SetMaxFileSize(size_t maxBytes);
    void SetCallback(AnalysisCallback callback);

    // Pattern detection
    void EnablePatternDetection(bool enabled);
    std::vector<TrafficPattern> GetDetectedPatterns() const;
    void ClearPatterns();

    // Statistics
    struct AnalysisStatistics {
        uint64_t totalPacketsAnalyzed;
        uint64_t totalBytesAnalyzed;
        uint64_t malformedPackets;
        uint64_t suspiciousPackets;
        std::chrono::microseconds totalAnalysisTime;
        std::unordered_map<std::string, uint64_t> packetTypeCounts;
        std::unordered_map<uint32_t, uint64_t> clientPacketCounts;
    };
    
    AnalysisStatistics GetStatistics() const;
    void ResetStatistics();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Global analysis functions for backward compatibility
void DumpPacketForAnalysis(const std::vector<uint8_t>& data, const std::string& context);
void DumpPacketForAnalysis(const Packet& packet, const PacketMetadata& metadata, const std::string& context);

// Analysis utilities
namespace PacketAnalysisUtils {
    std::string GenerateHexDump(const std::vector<uint8_t>& data, bool includeAscii = true);
    std::string DecodePacketStructure(const Packet& packet);
    bool ValidatePacketIntegrity(const std::vector<uint8_t>& data);
    std::vector<std::string> DetectAnomalies(const std::vector<uint8_t>& data, const std::string& context);
    float CalculateCompressionRatio(const std::vector<uint8_t>& original, const std::vector<uint8_t>& compressed);
}