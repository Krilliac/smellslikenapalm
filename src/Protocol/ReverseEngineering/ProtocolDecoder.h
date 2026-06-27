// src/Protocol/ReverseEngineering/ProtocolDecoder.h
//
// Enhanced automatic protocol reverse-engineering system for RS2V.
// When a game client connects, this system begins capturing and analyzing
// all packets to decode the UE3/RS2V wire protocol in real time.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <functional>
#include <optional>

#include "Protocol/ReverseEngineering/NetFieldTable.h"
#include "Protocol/ReverseEngineering/BunchPropertyDecoder.h"

// Forward declarations
class Packet;
struct PacketMetadata;
struct UE3BunchHeader;

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
    double               variance = 0.0;   // population variance (m2 / n)
    double               m2 = 0.0;         // Welford running sum of squared deltas

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

    // Layout inference: rather than locking the field layout from the FIRST
    // (possibly atypical) packet, we buffer the earliest samples and infer the
    // layout once from the MODAL payload size. layoutFinalized gates that.
    bool                     layoutFinalized = false;
    std::vector<std::vector<uint8_t>> sampleBuffer;   // capped; cleared after infer
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

    // Raw packet log — a rolling window of the MOST RECENT packets. A deque so
    // that, once the cap is reached, the oldest record is popped in O(1) and the
    // newest is retained (the previous vector-with-cap kept only the *earliest*
    // N packets and silently dropped everything after).
    struct RawPacketRecord {
        std::chrono::steady_clock::time_point timestamp;
        std::string tag;
        std::vector<uint8_t> data;
        bool inbound; // true = client->server, false = server->client
    };
    std::deque<RawPacketRecord> recentPackets;
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
    // SAFE-BY-DEFAULT: retaining full attacker-controlled payloads and writing
    // JSON to disk on a shipped server is a memory/disk-fill and info-leak risk,
    // so both default OFF. Structural analysis (sizes, fields, bunches) still
    // runs. Operators opt in for an active RE session.
    bool     logRawPackets = false;
    bool     exportJsonDefinitions = false;
    bool     detectUE3Bunches = true;
    bool     trackFieldStatistics = true;
    bool     decodeBunchProperties = true;   // bit-packed property decode vs handle tables
    bool     asyncAnalysis = true;           // run analysis off the network hot path
    bool     persistState = true;            // merge cumulative stats across runs
    size_t   minSamplesForConfidence = 10;
    float    fieldConfidenceThreshold = 0.7f;
    std::string outputDirectory = "protocol_analysis";
    std::string netfieldsDir = "data/re/netfields"; // per-class handle tables
    uint32_t maxChannels = 1024;             // object-ref / None bound (RS2-7258 [H])
    size_t   maxRawPacketsPerClient = 1000;
    size_t   maxSampleBuffer = 64;           // payloads buffered for layout inference
    size_t   maxAnalysisQueue = 50000;       // async queue cap (drop-oldest beyond)
    int      exportIntervalSeconds = 300; // auto-export every 5 min
};

// Per-channel decoded-property aggregation (results of bit-packed bunch decode).
struct ChannelPropertyStats {
    std::string className;                       // best-fit class for this channel
    double      bestFitScore = 0.0;
    uint64_t    bunchesDecoded = 0;
    // property name -> times seen, with a sample of the last decoded value
    struct PropAgg { uint64_t count = 0; std::string lastValue; uint32_t handle = 0; };
    std::map<std::string, PropAgg> properties;
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

    // Decoded bit-packed bunch properties, keyed by UE3 channel index.
    std::map<uint8_t, ChannelPropertyStats> GetChannelPropertyStats() const;

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

    // Bit-packed property decoding (channel -> aggregated decoded properties).
    NetFieldRegistry m_netFields;
    std::unique_ptr<BunchPropertyDecoder> m_propDecoder;
    std::map<uint8_t, ChannelPropertyStats> m_channelStats;

    // Statistics
    DecoderStatistics m_stats;

    // Callback
    ProtocolDiscoveryCallback m_discoveryCallback;

    // Auto-export timer
    std::chrono::steady_clock::time_point m_lastExportTime;

    // ---- async analysis pipeline ----
    // The network hot path only copies bytes and enqueues; a single worker thread
    // drains the queue and runs all analysis, so per-packet work never blocks the
    // socket threads. FIFO + single worker preserves connect/packet/disconnect
    // ordering. The queue is bounded (drop-oldest) to cap memory under flood.
    enum class CaptureKind { Connected, Disconnected, PacketRecv, PacketSent, RawUDP };
    struct CaptureEvent {
        CaptureKind kind;
        uint32_t clientId = 0;
        std::string ip;          // Connected
        std::string tag;         // PacketRecv/Sent
        std::vector<uint8_t> data;
    };
    std::deque<CaptureEvent> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::thread m_worker;
    std::atomic<bool> m_workerRunning{false};
    uint64_t m_droppedEvents = 0;

    void Enqueue(CaptureEvent&& ev);
    void WorkerLoop();
    void ProcessEvent(const CaptureEvent& ev);     // runs under m_mutex
    // Synchronous bodies (called by ProcessEvent or directly in sync mode).
    void HandleConnected(uint32_t clientId, const std::string& ip);
    void HandleDisconnected(uint32_t clientId);
    void HandlePacket(uint32_t clientId, const std::vector<uint8_t>& data,
                      const std::string& tag, bool inbound);
    void HandleRawUDP(uint32_t clientId, const std::vector<uint8_t>& data);

    // Core analysis methods
    void AnalyzePacketPayload(const std::string& tag, const std::vector<uint8_t>& data, bool inbound);
    void AnalyzeUE3Bunch(const uint8_t* data, size_t len);
    void DecodeBunchProperties(const UE3BunchHeader& header,
                               const uint8_t* payload, size_t payloadLen);
    void InferLayoutFromSamples(DecodedPacketStructure& structure);
    void DetectFieldTypes(DecodedPacketStructure& structure, const std::vector<uint8_t>& data);
    void RefineFieldDetection(DecodedPacketStructure& structure);
    void UpdateFieldStatistics(DetectedField& field, const std::vector<uint8_t>& data, size_t offset);

    // Persistence (cumulative cross-run stats).
    void LoadPersistentState();
    void SavePersistentState() const;

    // Export internals that assume m_mutex is already held (no re-lock dance).
    bool ExportProtocolDefinitionsLocked(const std::string& outputPath) const;
    bool ExportClientCaptureLocked(uint32_t clientId, const std::string& outputPath) const;

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
    static std::string JsonEscape(const std::string& s);

    // Auto-export check
    void MaybeAutoExport();
};

// Global singleton access
ProtocolDecoder& GetProtocolDecoder();
