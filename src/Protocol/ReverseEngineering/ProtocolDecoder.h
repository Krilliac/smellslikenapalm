// src/Protocol/ReverseEngineering/ProtocolDecoder.h
//
// Enhanced automatic protocol reverse-engineering system for RS2V.
// When a game client connects, this system begins capturing and analyzing
// all packets to decode the UE3/RS2V wire protocol in real time.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <chrono>
#include <functional>
#include <optional>

// Forward declarations
class Packet;
struct PacketMetadata;

// Detected field type from heuristic analysis
enum class DetectedFieldType : uint8_t {
    Unknown = 0,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Int8,
    Int16,
    Int32,
    Float32,
    Float64,
    String,          // length-prefixed string (4-byte len + data)
    Vector3,         // 3x float32
    Boolean,         // single byte 0/1
    Bitfield,        // packed flags
    BlobData,        // variable-length raw binary
    Timestamp,       // 64-bit time value
    EntityId,        // 32-bit entity reference
    Enum8,           // small enum value
    Enum16,          // 16-bit enum
    CompressedData,  // zlib/lz4 compressed payload
    Padding          // alignment/padding bytes
};

// A single detected field within a packet structure
struct DetectedField {
    size_t               offset = 0;         // byte offset within payload
    size_t               size = 0;           // field size in bytes (0 = variable)
    DetectedFieldType    type = DetectedFieldType::Unknown;
    std::string          suggestedName;      // heuristic-suggested field name
    float                confidence = 0.0f;  // 0.0 - 1.0 confidence in type detection

    // Statistical data collected from multiple samples
    uint64_t             sampleCount = 0;
    double               minValue = 0.0;
    double               maxValue = 0.0;
    double               avgValue = 0.0;
    double               variance = 0.0;

    // For string fields
    size_t               minLength = 0;
    size_t               maxLength = 0;

    // For enum fields
    std::map<uint32_t, uint64_t> observedValues; // value -> count
};

// A decoded packet structure definition
struct DecodedPacketStructure {
    std::string              packetTag;       // e.g. "PLAYER_MOVE"
    uint16_t                 packetTypeId = 0;
    std::vector<DetectedField> fields;
    uint64_t                 totalSamples = 0;
    float                    structureConfidence = 0.0f;

    // Size statistics
    size_t                   minPayloadSize = SIZE_MAX;
    size_t                   maxPayloadSize = 0;
    double                   avgPayloadSize = 0.0;

    // Timing
    std::chrono::steady_clock::time_point firstSeen;
    std::chrono::steady_clock::time_point lastSeen;

    // Direction tracking
    uint64_t                 clientToServerCount = 0;
    uint64_t                 serverToClientCount = 0;
};

// Per-client connection session for protocol analysis
struct ClientAnalysisSession {
    uint32_t clientId = 0;
    std::string clientIP;
    std::chrono::steady_clock::time_point connectTime;

    // Handshake tracking
    bool     handshakeComplete = false;
    uint32_t handshakeStage = 0;
    std::vector<std::vector<uint8_t>> handshakePackets;

    // Packet sequence tracking
    uint64_t totalPacketsReceived = 0;
    uint64_t totalPacketsSent = 0;
    uint64_t totalBytesReceived = 0;
    uint64_t totalBytesSent = 0;

    // Per-tag packet counters
    std::unordered_map<std::string, uint64_t> tagCounts;

    // Raw packet log (capped to prevent memory bloat)
    struct RawPacketRecord {
        std::chrono::steady_clock::time_point timestamp;
        std::string tag;
        std::vector<uint8_t> data;
        bool inbound; // true = client->server, false = server->client
    };
    std::vector<RawPacketRecord> recentPackets;
    static constexpr size_t MAX_RECENT_PACKETS = 1000;
};

// UE3 bunch-level analysis data
struct BunchAnalysis {
    uint8_t  channelIndex = 0;
    uint16_t sequence = 0;
    bool     reliable = false;
    bool     openChannel = false;
    bool     closeChannel = false;
    size_t   payloadLength = 0;
    std::vector<uint8_t> rawPayload;
};

// Configuration for the protocol decoder
struct ProtocolDecoderConfig {
    bool     enabled = true;
    bool     logRawPackets = true;
    bool     exportJsonDefinitions = true;
    bool     detectUE3Bunches = true;
    bool     trackFieldStatistics = true;
    size_t   minSamplesForConfidence = 10;
    float    fieldConfidenceThreshold = 0.7f;
    std::string outputDirectory = "protocol_analysis";
    size_t   maxRawPacketsPerClient = 1000;
    int      exportIntervalSeconds = 300; // auto-export every 5 min
};

// Callback for real-time protocol discoveries
using ProtocolDiscoveryCallback = std::function<void(
    const std::string& packetTag,
    const DecodedPacketStructure& structure)>;

class ProtocolDecoder {
public:
    ProtocolDecoder();
    ~ProtocolDecoder();

    // Lifecycle
    void Initialize(const ProtocolDecoderConfig& config = {});
    void Shutdown();

    // Client session management - called from ConnectionManager
    void OnClientConnected(uint32_t clientId, const std::string& ip);
    void OnClientDisconnected(uint32_t clientId);

    // Packet capture hooks - called from network layer
    void OnPacketReceived(uint32_t clientId, const std::vector<uint8_t>& rawData,
                          const std::string& tag);
    void OnPacketSent(uint32_t clientId, const std::vector<uint8_t>& rawData,
                      const std::string& tag);

    // Raw UDP data analysis (pre-packet parsing, for UE3 bunch analysis)
    void OnRawUDPReceived(uint32_t clientId, const uint8_t* data, size_t len);

    // Query decoded protocol structures
    std::vector<DecodedPacketStructure> GetAllDecodedStructures() const;
    std::optional<DecodedPacketStructure> GetStructure(const std::string& tag) const;

    // Client analysis data
    std::optional<ClientAnalysisSession> GetClientSession(uint32_t clientId) const;
    std::vector<uint32_t> GetActiveClients() const;

    // Export and reporting
    bool ExportProtocolDefinitions(const std::string& outputPath = "") const;
    bool ExportClientCapture(uint32_t clientId, const std::string& outputPath = "") const;
    std::string GenerateProtocolReport() const;

    // Callbacks
    void SetDiscoveryCallback(ProtocolDiscoveryCallback cb);

    // Statistics
    struct DecoderStatistics {
        uint64_t totalPacketsAnalyzed = 0;
        uint64_t totalBytesAnalyzed = 0;
        uint64_t uniquePacketTypes = 0;
        uint64_t structuresDecoded = 0;
        uint64_t bunchesDecoded = 0;
        uint64_t activeClients = 0;
        std::chrono::steady_clock::time_point startTime;
    };
    DecoderStatistics GetStatistics() const;

private:
    ProtocolDecoderConfig m_config;
    mutable std::mutex m_mutex;
    bool m_initialized = false;

    // Per-client analysis sessions
    std::unordered_map<uint32_t, ClientAnalysisSession> m_clientSessions;

    // Decoded protocol structures (tag -> structure)
    std::map<std::string, DecodedPacketStructure> m_decodedStructures;

    // UE3 bunch analysis results
    std::vector<BunchAnalysis> m_bunchHistory;

    // Statistics
    DecoderStatistics m_stats;

    // Callback
    ProtocolDiscoveryCallback m_discoveryCallback;

    // Auto-export timer
    std::chrono::steady_clock::time_point m_lastExportTime;

    // Core analysis methods
    void AnalyzePacketPayload(const std::string& tag, const std::vector<uint8_t>& data, bool inbound);
    void AnalyzeUE3Bunch(const uint8_t* data, size_t len);
    void DetectFieldTypes(DecodedPacketStructure& structure, const std::vector<uint8_t>& data);
    void RefineFieldDetection(DecodedPacketStructure& structure);
    void UpdateFieldStatistics(DetectedField& field, const std::vector<uint8_t>& data, size_t offset);

    // Heuristic field detection
    DetectedFieldType ClassifyFieldAt(const std::vector<uint8_t>& data, size_t offset, size_t remaining);
    bool LooksLikeString(const std::vector<uint8_t>& data, size_t offset);
    bool LooksLikeFloat(const std::vector<uint8_t>& data, size_t offset);
    bool LooksLikeVector3(const std::vector<uint8_t>& data, size_t offset);
    bool LooksLikeTimestamp(const std::vector<uint8_t>& data, size_t offset);
    bool LooksLikeEntityId(const std::vector<uint8_t>& data, size_t offset);
    bool LooksLikeBoolean(const std::vector<uint8_t>& data, size_t offset);

    // Handshake analysis
    void AnalyzeHandshake(uint32_t clientId, const std::vector<uint8_t>& data);

    // Export helpers
    std::string FieldTypeToString(DetectedFieldType type) const;
    std::string StructureToJson(const DecodedPacketStructure& structure) const;
    std::string GenerateFieldName(DetectedFieldType type, size_t fieldIndex) const;

    // Auto-export check
    void MaybeAutoExport();
};

// Global singleton access
ProtocolDecoder& GetProtocolDecoder();
