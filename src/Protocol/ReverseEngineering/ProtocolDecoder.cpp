// src/Protocol/ReverseEngineering/ProtocolDecoder.cpp
//
// Enhanced automatic protocol reverse-engineering system for RS2V.
// Captures, analyzes, and decodes UE3/RS2V wire protocol in real time.

#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include "Protocol/UE3Protocol.h"
#include "Utils/Logger.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <numeric>

// ---- Global singleton ----
static std::unique_ptr<ProtocolDecoder> g_protocolDecoder;

ProtocolDecoder& GetProtocolDecoder() {
    if (!g_protocolDecoder) {
        g_protocolDecoder = std::make_unique<ProtocolDecoder>();
    }
    return *g_protocolDecoder;
}

// ---- Construction / Destruction ----

ProtocolDecoder::ProtocolDecoder() = default;

ProtocolDecoder::~ProtocolDecoder() {
    Shutdown();
}

void ProtocolDecoder::Initialize(const ProtocolDecoderConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;

    m_config = config;
    m_stats = {};
    m_stats.startTime = std::chrono::steady_clock::now();
    m_lastExportTime = m_stats.startTime;

    if (m_config.enabled) {
        std::error_code ec;
        std::filesystem::create_directories(m_config.outputDirectory, ec);
        if (ec) {
            Logger::Warn("ProtocolDecoder: Could not create output dir '%s': %s",
                         m_config.outputDirectory.c_str(), ec.message().c_str());
        }
    }

    m_initialized = true;
    Logger::Info("ProtocolDecoder initialized (enabled=%s, output=%s)",
                 m_config.enabled ? "true" : "false",
                 m_config.outputDirectory.c_str());
}

void ProtocolDecoder::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;

    // Final export before shutdown
    if (m_config.exportJsonDefinitions && !m_decodedStructures.empty()) {
        // Unlock temporarily for export
        m_mutex.unlock();
        ExportProtocolDefinitions();
        m_mutex.lock();
    }

    Logger::Info("ProtocolDecoder shutdown — analyzed %llu packets, decoded %llu structures",
                 (unsigned long long)m_stats.totalPacketsAnalyzed,
                 (unsigned long long)m_stats.structuresDecoded);

    m_clientSessions.clear();
    m_decodedStructures.clear();
    m_bunchHistory.clear();
    m_initialized = false;
}

// ---- Client Session Management ----

void ProtocolDecoder::OnClientConnected(uint32_t clientId, const std::string& ip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled) return;

    auto& session = m_clientSessions[clientId];
    session.clientId = clientId;
    session.clientIP = ip;
    session.connectTime = std::chrono::steady_clock::now();
    session.handshakeComplete = false;
    session.handshakeStage = 0;
    session.totalPacketsReceived = 0;
    session.totalPacketsSent = 0;
    session.totalBytesReceived = 0;
    session.totalBytesSent = 0;
    session.tagCounts.clear();
    session.handshakePackets.clear();
    session.recentPackets.clear();

    m_stats.activeClients = m_clientSessions.size();

    Logger::Info("ProtocolDecoder: Client %u connected from %s — beginning protocol capture",
                 clientId, ip.c_str());
}

void ProtocolDecoder::OnClientDisconnected(uint32_t clientId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled) return;

    auto it = m_clientSessions.find(clientId);
    if (it != m_clientSessions.end()) {
        auto& session = it->second;
        Logger::Info("ProtocolDecoder: Client %u disconnected — captured %llu recv / %llu sent packets",
                     clientId,
                     (unsigned long long)session.totalPacketsReceived,
                     (unsigned long long)session.totalPacketsSent);

        // Export client capture data before removing
        if (m_config.exportJsonDefinitions) {
            m_mutex.unlock();
            ExportClientCapture(clientId);
            m_mutex.lock();
        }

        m_clientSessions.erase(it);
    }
    m_stats.activeClients = m_clientSessions.size();
}

// ---- Packet Capture Hooks ----

void ProtocolDecoder::OnPacketReceived(uint32_t clientId, const std::vector<uint8_t>& rawData,
                                       const std::string& tag) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled || rawData.empty()) return;

    m_stats.totalPacketsAnalyzed++;
    m_stats.totalBytesAnalyzed += rawData.size();

    auto it = m_clientSessions.find(clientId);
    if (it != m_clientSessions.end()) {
        auto& session = it->second;
        session.totalPacketsReceived++;
        session.totalBytesReceived += rawData.size();
        session.tagCounts[tag]++;

        // Track recent packets
        if (m_config.logRawPackets && session.recentPackets.size() < m_config.maxRawPacketsPerClient) {
            session.recentPackets.push_back({
                std::chrono::steady_clock::now(),
                tag,
                rawData,
                true
            });
        }

        // Handshake detection for early packets
        if (!session.handshakeComplete && session.totalPacketsReceived <= 10) {
            AnalyzeHandshake(clientId, rawData);
        }
    }

    // Core payload analysis
    AnalyzePacketPayload(tag, rawData, true);

    // Auto-export check
    MaybeAutoExport();
}

void ProtocolDecoder::OnPacketSent(uint32_t clientId, const std::vector<uint8_t>& rawData,
                                    const std::string& tag) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled || rawData.empty()) return;

    m_stats.totalPacketsAnalyzed++;
    m_stats.totalBytesAnalyzed += rawData.size();

    auto it = m_clientSessions.find(clientId);
    if (it != m_clientSessions.end()) {
        auto& session = it->second;
        session.totalPacketsSent++;
        session.totalBytesSent += rawData.size();

        if (m_config.logRawPackets && session.recentPackets.size() < m_config.maxRawPacketsPerClient) {
            session.recentPackets.push_back({
                std::chrono::steady_clock::now(),
                tag,
                rawData,
                false
            });
        }
    }

    AnalyzePacketPayload(tag, rawData, false);
}

void ProtocolDecoder::OnRawUDPReceived(uint32_t clientId, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled || !m_config.detectUE3Bunches || len < 8) return;

    AnalyzeUE3Bunch(data, len);
}

// ---- Core Analysis Methods ----

void ProtocolDecoder::AnalyzePacketPayload(const std::string& tag, const std::vector<uint8_t>& data, bool inbound) {
    auto& structure = m_decodedStructures[tag];
    bool isNew = (structure.totalSamples == 0);

    if (isNew) {
        structure.packetTag = tag;
        structure.firstSeen = std::chrono::steady_clock::now();
        m_stats.uniquePacketTypes++;
        m_stats.structuresDecoded++;

        Logger::Info("ProtocolDecoder: NEW packet type discovered: '%s' (size=%zu)",
                     tag.c_str(), data.size());

        if (m_discoveryCallback) {
            m_discoveryCallback(tag, structure);
        }
    }

    structure.lastSeen = std::chrono::steady_clock::now();
    structure.totalSamples++;

    if (inbound) structure.clientToServerCount++;
    else         structure.serverToClientCount++;

    // Update size statistics
    structure.minPayloadSize = std::min(structure.minPayloadSize, data.size());
    structure.maxPayloadSize = std::max(structure.maxPayloadSize, data.size());
    structure.avgPayloadSize = structure.avgPayloadSize
        + (static_cast<double>(data.size()) - structure.avgPayloadSize) / structure.totalSamples;

    // Detect field types from payload
    DetectFieldTypes(structure, data);

    // After enough samples, refine the detection
    if (structure.totalSamples == m_config.minSamplesForConfidence) {
        RefineFieldDetection(structure);
        Logger::Info("ProtocolDecoder: Structure for '%s' refined after %llu samples (%zu fields detected)",
                     tag.c_str(), (unsigned long long)structure.totalSamples, structure.fields.size());
    }
}

void ProtocolDecoder::AnalyzeUE3Bunch(const uint8_t* data, size_t len) {
    UE3Protocol proto;
    UE3BunchHeader header{};
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;

    if (proto.ParseBunch(data, len, header, payload, payloadLen)) {
        BunchAnalysis ba;
        ba.channelIndex = header.ChannelIndex;
        ba.sequence = header.ChSequence;
        ba.reliable = header.Reliable;
        ba.openChannel = header.Open;
        ba.closeChannel = header.Close;
        ba.payloadLength = payloadLen;
        if (payload && payloadLen > 0) {
            ba.rawPayload.assign(payload, payload + payloadLen);
        }

        m_bunchHistory.push_back(std::move(ba));
        m_stats.bunchesDecoded++;

        // Cap history
        if (m_bunchHistory.size() > 10000) {
            m_bunchHistory.erase(m_bunchHistory.begin(), m_bunchHistory.begin() + 5000);
        }

        if (header.Open) {
            Logger::Debug("ProtocolDecoder: UE3 bunch OPEN on channel %u, seq=%u, payload=%zu",
                          header.ChannelIndex, header.ChSequence, payloadLen);
        }
    }
}

void ProtocolDecoder::DetectFieldTypes(DecodedPacketStructure& structure, const std::vector<uint8_t>& data) {
    if (data.empty()) return;

    // On first sample, do initial field layout detection
    if (structure.totalSamples <= 1) {
        structure.fields.clear();
        size_t offset = 0;
        size_t fieldIndex = 0;
        while (offset < data.size()) {
            size_t remaining = data.size() - offset;
            DetectedField field;
            field.offset = offset;
            field.sampleCount = 1;

            DetectedFieldType fieldType = ClassifyFieldAt(data, offset, remaining);
            field.type = fieldType;

            switch (fieldType) {
                case DetectedFieldType::String: {
                    if (remaining >= 4) {
                        uint32_t strLen = 0;
                        std::memcpy(&strLen, &data[offset], 4);
                        if (strLen < 10000 && offset + 4 + strLen <= data.size()) {
                            field.size = 4 + strLen;
                            field.minLength = strLen;
                            field.maxLength = strLen;
                        } else {
                            field.type = DetectedFieldType::UInt32;
                            field.size = 4;
                        }
                    } else {
                        field.type = DetectedFieldType::Unknown;
                        field.size = remaining;
                    }
                    break;
                }
                case DetectedFieldType::Vector3:
                    field.size = 12;
                    break;
                case DetectedFieldType::Float32:
                    field.size = 4;
                    break;
                case DetectedFieldType::UInt32:
                case DetectedFieldType::EntityId:
                    field.size = 4;
                    break;
                case DetectedFieldType::UInt16:
                case DetectedFieldType::Enum16:
                    field.size = 2;
                    break;
                case DetectedFieldType::UInt8:
                case DetectedFieldType::Boolean:
                case DetectedFieldType::Enum8:
                    field.size = 1;
                    break;
                case DetectedFieldType::UInt64:
                case DetectedFieldType::Timestamp:
                    field.size = 8;
                    break;
                default:
                    // Consume remaining bytes as blob
                    field.size = remaining;
                    field.type = DetectedFieldType::BlobData;
                    break;
            }

            field.suggestedName = GenerateFieldName(field.type, fieldIndex);
            field.confidence = 0.3f; // low initial confidence

            if (m_config.trackFieldStatistics && field.type != DetectedFieldType::BlobData) {
                UpdateFieldStatistics(field, data, offset);
            }

            structure.fields.push_back(field);
            offset += field.size;
            fieldIndex++;
        }
    } else {
        // Update statistics for existing fields with new sample
        if (m_config.trackFieldStatistics) {
            size_t offset = 0;
            for (auto& field : structure.fields) {
                if (offset + field.size <= data.size()) {
                    field.sampleCount++;
                    UpdateFieldStatistics(field, data, offset);
                    offset += field.size;
                } else {
                    break;
                }
            }
        }
    }
}

void ProtocolDecoder::RefineFieldDetection(DecodedPacketStructure& structure) {
    for (auto& field : structure.fields) {
        // Refine confidence based on sample count and value distribution
        float sampleFactor = std::min(1.0f, static_cast<float>(field.sampleCount) /
                                             static_cast<float>(m_config.minSamplesForConfidence));

        switch (field.type) {
            case DetectedFieldType::Boolean:
                // Confirm: all values should be 0 or 1
                if (field.observedValues.size() <= 2 &&
                    field.observedValues.count(0) + field.observedValues.count(1) ==
                    field.observedValues.size()) {
                    field.confidence = 0.95f * sampleFactor;
                } else {
                    field.type = DetectedFieldType::UInt8;
                    field.confidence = 0.5f * sampleFactor;
                }
                break;

            case DetectedFieldType::Float32:
                // Confirm: values should be in reasonable float range
                if (field.minValue > -1e6 && field.maxValue < 1e6 && field.variance > 0) {
                    field.confidence = 0.85f * sampleFactor;
                } else {
                    field.confidence = 0.5f * sampleFactor;
                }
                break;

            case DetectedFieldType::Vector3:
                field.confidence = 0.8f * sampleFactor;
                break;

            case DetectedFieldType::String:
                if (field.minLength > 0 && field.maxLength < 10000) {
                    field.confidence = 0.9f * sampleFactor;
                }
                break;

            case DetectedFieldType::EntityId:
                // Entity IDs tend to have limited range and multiple occurrences
                if (field.observedValues.size() < field.sampleCount * 0.8) {
                    field.confidence = 0.7f * sampleFactor;
                }
                break;

            case DetectedFieldType::Timestamp:
                field.confidence = 0.85f * sampleFactor;
                break;

            case DetectedFieldType::Enum8:
            case DetectedFieldType::Enum16:
                if (field.observedValues.size() <= 20) {
                    field.confidence = 0.75f * sampleFactor;
                } else {
                    field.type = (field.size == 1) ? DetectedFieldType::UInt8 : DetectedFieldType::UInt16;
                    field.confidence = 0.5f * sampleFactor;
                }
                break;

            default:
                field.confidence = 0.4f * sampleFactor;
                break;
        }
    }

    // Calculate overall structure confidence
    if (!structure.fields.empty()) {
        float totalConf = 0;
        for (const auto& f : structure.fields) {
            totalConf += f.confidence;
        }
        structure.structureConfidence = totalConf / structure.fields.size();
    }
}

void ProtocolDecoder::UpdateFieldStatistics(DetectedField& field, const std::vector<uint8_t>& data, size_t offset) {
    if (offset + field.size > data.size()) return;

    double value = 0;
    switch (field.type) {
        case DetectedFieldType::UInt8:
        case DetectedFieldType::Boolean:
        case DetectedFieldType::Enum8:
            value = data[offset];
            field.observedValues[data[offset]]++;
            break;

        case DetectedFieldType::UInt16:
        case DetectedFieldType::Enum16: {
            uint16_t v = 0;
            std::memcpy(&v, &data[offset], 2);
            value = v;
            field.observedValues[v]++;
            break;
        }

        case DetectedFieldType::UInt32:
        case DetectedFieldType::EntityId: {
            uint32_t v = 0;
            std::memcpy(&v, &data[offset], 4);
            value = v;
            if (field.observedValues.size() < 1000) { // cap map size
                field.observedValues[v]++;
            }
            break;
        }

        case DetectedFieldType::Float32: {
            float v = 0;
            std::memcpy(&v, &data[offset], 4);
            value = v;
            break;
        }

        case DetectedFieldType::UInt64:
        case DetectedFieldType::Timestamp: {
            uint64_t v = 0;
            std::memcpy(&v, &data[offset], 8);
            value = static_cast<double>(v);
            break;
        }

        case DetectedFieldType::String: {
            if (offset + 4 <= data.size()) {
                uint32_t strLen = 0;
                std::memcpy(&strLen, &data[offset], 4);
                field.minLength = std::min(field.minLength, static_cast<size_t>(strLen));
                field.maxLength = std::max(field.maxLength, static_cast<size_t>(strLen));
            }
            return; // no numeric stats for strings
        }

        default:
            return;
    }

    // Running statistics (Welford's algorithm)
    double n = static_cast<double>(field.sampleCount);
    if (field.sampleCount == 1) {
        field.minValue = value;
        field.maxValue = value;
        field.avgValue = value;
        field.variance = 0;
    } else {
        field.minValue = std::min(field.minValue, value);
        field.maxValue = std::max(field.maxValue, value);
        double oldAvg = field.avgValue;
        field.avgValue += (value - oldAvg) / n;
        field.variance += (value - oldAvg) * (value - field.avgValue);
    }
}

// ---- Heuristic Field Detection ----

DetectedFieldType ProtocolDecoder::ClassifyFieldAt(const std::vector<uint8_t>& data, size_t offset, size_t remaining) {
    // Try most specific patterns first

    // Vector3 (12 bytes: 3x float32)
    if (remaining >= 12 && LooksLikeVector3(data, offset)) {
        return DetectedFieldType::Vector3;
    }

    // Timestamp (8 bytes: large uint64 that looks like epoch millis)
    if (remaining >= 8 && LooksLikeTimestamp(data, offset)) {
        return DetectedFieldType::Timestamp;
    }

    // String (4-byte length prefix + printable ASCII)
    if (remaining >= 4 && LooksLikeString(data, offset)) {
        return DetectedFieldType::String;
    }

    // Float32 (4 bytes in reasonable float range)
    if (remaining >= 4 && LooksLikeFloat(data, offset)) {
        return DetectedFieldType::Float32;
    }

    // EntityId / UInt32 (4 bytes, reasonable ID range)
    if (remaining >= 4 && LooksLikeEntityId(data, offset)) {
        return DetectedFieldType::EntityId;
    }

    // UInt32 fallback for 4+ bytes
    if (remaining >= 4) {
        return DetectedFieldType::UInt32;
    }

    // UInt16 for 2-3 bytes remaining
    if (remaining >= 2) {
        return DetectedFieldType::UInt16;
    }

    // Boolean (single byte 0 or 1)
    if (remaining >= 1 && LooksLikeBoolean(data, offset)) {
        return DetectedFieldType::Boolean;
    }

    // UInt8 fallback
    if (remaining >= 1) {
        return DetectedFieldType::UInt8;
    }

    return DetectedFieldType::Unknown;
}

bool ProtocolDecoder::LooksLikeString(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) return false;

    uint32_t strLen = 0;
    std::memcpy(&strLen, &data[offset], 4);

    // Reasonable string length check
    if (strLen == 0 || strLen > 2048) return false;
    if (offset + 4 + strLen > data.size()) return false;

    // Check if content looks like printable ASCII/UTF-8
    size_t printable = 0;
    for (size_t i = 0; i < strLen && i < 64; ++i) {
        uint8_t c = data[offset + 4 + i];
        if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t' || c >= 0x80) {
            printable++;
        }
    }

    size_t checkLen = std::min(strLen, static_cast<uint32_t>(64));
    return checkLen > 0 && (printable * 100 / checkLen) >= 80;
}

bool ProtocolDecoder::LooksLikeFloat(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) return false;

    float v = 0;
    std::memcpy(&v, &data[offset], 4);

    // Check for reasonable game float values (positions, rotations, velocities)
    if (std::isnan(v) || std::isinf(v)) return false;
    if (v == 0.0f) return false; // Could be uint32 zero

    // Game world coordinates and rotations typically in range
    return (v > -100000.0f && v < 100000.0f);
}

bool ProtocolDecoder::LooksLikeVector3(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 12 > data.size()) return false;

    float x, y, z;
    std::memcpy(&x, &data[offset], 4);
    std::memcpy(&y, &data[offset + 4], 4);
    std::memcpy(&z, &data[offset + 8], 4);

    // All three must be valid floats in game world range
    if (std::isnan(x) || std::isnan(y) || std::isnan(z)) return false;
    if (std::isinf(x) || std::isinf(y) || std::isinf(z)) return false;

    bool xValid = (x > -100000.0f && x < 100000.0f);
    bool yValid = (y > -100000.0f && y < 100000.0f);
    bool zValid = (z > -100000.0f && z < 100000.0f);

    // At least 2 of 3 should be non-zero for it to be a vector
    int nonZero = (x != 0.0f ? 1 : 0) + (y != 0.0f ? 1 : 0) + (z != 0.0f ? 1 : 0);

    return xValid && yValid && zValid && nonZero >= 2;
}

bool ProtocolDecoder::LooksLikeTimestamp(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 8 > data.size()) return false;

    uint64_t v = 0;
    std::memcpy(&v, &data[offset], 8);

    // Check if it looks like epoch milliseconds (roughly 2020-2030 range)
    // 2020-01-01 = 1577836800000 ms, 2030-01-01 = 1893456000000 ms
    return (v > 1577836800000ULL && v < 2000000000000ULL);
}

bool ProtocolDecoder::LooksLikeEntityId(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) return false;

    uint32_t v = 0;
    std::memcpy(&v, &data[offset], 4);

    // Entity IDs are typically small-ish positive integers
    return (v > 0 && v < 100000);
}

bool ProtocolDecoder::LooksLikeBoolean(const std::vector<uint8_t>& data, size_t offset) {
    return (data[offset] == 0 || data[offset] == 1);
}

// ---- Handshake Analysis ----

void ProtocolDecoder::AnalyzeHandshake(uint32_t clientId, const std::vector<uint8_t>& data) {
    auto it = m_clientSessions.find(clientId);
    if (it == m_clientSessions.end()) return;

    auto& session = it->second;
    session.handshakePackets.push_back(data);
    session.handshakeStage++;

    Logger::Info("ProtocolDecoder: Client %u handshake packet #%u (size=%zu bytes)",
                 clientId, session.handshakeStage, data.size());

    // Log hex dump of handshake packets for manual analysis
    std::ostringstream hex;
    hex << "  Hex: ";
    for (size_t i = 0; i < std::min(data.size(), (size_t)64); ++i) {
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
    }
    if (data.size() > 64) hex << "... (" << data.size() << " total)";
    Logger::Debug("%s", hex.str().c_str());

    // Heuristic: UE3 handshakes typically complete within 5-8 packets
    if (session.handshakeStage >= 5) {
        session.handshakeComplete = true;
        Logger::Info("ProtocolDecoder: Client %u handshake appears complete after %u packets",
                     clientId, session.handshakeStage);
    }
}

// ---- Query Methods ----

std::vector<DecodedPacketStructure> ProtocolDecoder::GetAllDecodedStructures() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DecodedPacketStructure> result;
    result.reserve(m_decodedStructures.size());
    for (const auto& [tag, structure] : m_decodedStructures) {
        result.push_back(structure);
    }
    return result;
}

std::optional<DecodedPacketStructure> ProtocolDecoder::GetStructure(const std::string& tag) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_decodedStructures.find(tag);
    if (it != m_decodedStructures.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<ClientAnalysisSession> ProtocolDecoder::GetClientSession(uint32_t clientId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clientSessions.find(clientId);
    if (it != m_clientSessions.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<uint32_t> ProtocolDecoder::GetActiveClients() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint32_t> clients;
    for (const auto& [id, session] : m_clientSessions) {
        clients.push_back(id);
    }
    return clients;
}

// ---- Export and Reporting ----

bool ProtocolDecoder::ExportProtocolDefinitions(const std::string& outputPath) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string path = outputPath.empty()
        ? (m_config.outputDirectory + "/protocol_definitions.json")
        : outputPath;

    std::ofstream file(path);
    if (!file.is_open()) {
        Logger::Error("ProtocolDecoder: Failed to open '%s' for export", path.c_str());
        return false;
    }

    file << "{\n";
    file << "  \"exportTime\": \"" << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "\",\n";
    file << "  \"totalPacketsAnalyzed\": " << m_stats.totalPacketsAnalyzed << ",\n";
    file << "  \"totalBytesAnalyzed\": " << m_stats.totalBytesAnalyzed << ",\n";
    file << "  \"uniquePacketTypes\": " << m_stats.uniquePacketTypes << ",\n";
    file << "  \"structures\": {\n";

    bool first = true;
    for (const auto& [tag, structure] : m_decodedStructures) {
        if (!first) file << ",\n";
        first = false;
        file << "    " << StructureToJson(structure);
    }

    file << "\n  }\n";
    file << "}\n";

    file.close();
    Logger::Info("ProtocolDecoder: Exported %zu structures to '%s'",
                 m_decodedStructures.size(), path.c_str());
    return true;
}

bool ProtocolDecoder::ExportClientCapture(uint32_t clientId, const std::string& outputPath) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_clientSessions.find(clientId);
    if (it == m_clientSessions.end()) return false;
    const auto& session = it->second;

    std::string path = outputPath.empty()
        ? (m_config.outputDirectory + "/client_" + std::to_string(clientId) + "_capture.json")
        : outputPath;

    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "{\n";
    file << "  \"clientId\": " << session.clientId << ",\n";
    file << "  \"clientIP\": \"" << session.clientIP << "\",\n";
    file << "  \"totalPacketsReceived\": " << session.totalPacketsReceived << ",\n";
    file << "  \"totalPacketsSent\": " << session.totalPacketsSent << ",\n";
    file << "  \"totalBytesReceived\": " << session.totalBytesReceived << ",\n";
    file << "  \"totalBytesSent\": " << session.totalBytesSent << ",\n";
    file << "  \"handshakeComplete\": " << (session.handshakeComplete ? "true" : "false") << ",\n";
    file << "  \"handshakeStages\": " << session.handshakeStage << ",\n";

    // Tag counts
    file << "  \"packetTagCounts\": {\n";
    bool firstTag = true;
    for (const auto& [tag, count] : session.tagCounts) {
        if (!firstTag) file << ",\n";
        firstTag = false;
        file << "    \"" << tag << "\": " << count;
    }
    file << "\n  },\n";

    // Handshake packets (hex)
    file << "  \"handshakePackets\": [\n";
    for (size_t i = 0; i < session.handshakePackets.size(); ++i) {
        const auto& pkt = session.handshakePackets[i];
        file << "    \"";
        for (auto b : pkt) {
            file << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        }
        file << "\"";
        if (i + 1 < session.handshakePackets.size()) file << ",";
        file << "\n";
    }
    file << "  ],\n";

    // Recent packets summary
    file << "  \"recentPackets\": [\n";
    for (size_t i = 0; i < session.recentPackets.size(); ++i) {
        const auto& rp = session.recentPackets[i];
        file << "    {\"tag\": \"" << rp.tag << "\", \"size\": " << rp.data.size()
             << ", \"direction\": \"" << (rp.inbound ? "recv" : "send") << "\", \"hex\": \"";
        // First 32 bytes only
        for (size_t j = 0; j < std::min(rp.data.size(), (size_t)32); ++j) {
            file << std::hex << std::setw(2) << std::setfill('0') << (int)rp.data[j];
        }
        if (rp.data.size() > 32) file << "...";
        file << "\"}";
        if (i + 1 < session.recentPackets.size()) file << ",";
        file << "\n";
    }
    file << "  ]\n";

    file << "}\n";
    file.close();

    Logger::Info("ProtocolDecoder: Exported capture for client %u to '%s'", clientId, path.c_str());
    return true;
}

std::string ProtocolDecoder::GenerateProtocolReport() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ostringstream report;
    report << "=== RS2V Protocol Decoder Report ===\n\n";

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_stats.startTime);
    report << "Runtime: " << elapsed.count() << " seconds\n";
    report << "Total Packets Analyzed: " << m_stats.totalPacketsAnalyzed << "\n";
    report << "Total Bytes Analyzed: " << m_stats.totalBytesAnalyzed << "\n";
    report << "Unique Packet Types: " << m_stats.uniquePacketTypes << "\n";
    report << "UE3 Bunches Decoded: " << m_stats.bunchesDecoded << "\n";
    report << "Active Clients: " << m_stats.activeClients << "\n\n";

    report << "--- Decoded Packet Structures ---\n\n";
    for (const auto& [tag, s] : m_decodedStructures) {
        report << "Packet: " << tag << "\n";
        report << "  Samples: " << s.totalSamples
               << " (C->S: " << s.clientToServerCount << ", S->C: " << s.serverToClientCount << ")\n";
        report << "  Payload Size: min=" << s.minPayloadSize
               << " max=" << s.maxPayloadSize
               << " avg=" << std::fixed << std::setprecision(1) << s.avgPayloadSize << "\n";
        report << "  Structure Confidence: " << std::setprecision(1)
               << (s.structureConfidence * 100.0f) << "%\n";
        report << "  Fields (" << s.fields.size() << "):\n";
        for (size_t i = 0; i < s.fields.size(); ++i) {
            const auto& f = s.fields[i];
            report << "    [" << i << "] offset=" << f.offset
                   << " size=" << f.size
                   << " type=" << FieldTypeToString(f.type)
                   << " name=\"" << f.suggestedName << "\""
                   << " conf=" << std::setprecision(0) << (f.confidence * 100.0f) << "%";
            if (f.type != DetectedFieldType::String && f.sampleCount > 0) {
                report << " min=" << std::setprecision(1) << f.minValue
                       << " max=" << f.maxValue
                       << " avg=" << f.avgValue;
            }
            if (f.type == DetectedFieldType::String) {
                report << " strlen=" << f.minLength << "-" << f.maxLength;
            }
            if (!f.observedValues.empty() && f.observedValues.size() <= 10) {
                report << " values={";
                bool first = true;
                for (const auto& [v, c] : f.observedValues) {
                    if (!first) report << ",";
                    first = false;
                    report << v << ":" << c;
                }
                report << "}";
            }
            report << "\n";
        }
        report << "\n";
    }

    // Client sessions
    if (!m_clientSessions.empty()) {
        report << "--- Active Client Sessions ---\n\n";
        for (const auto& [id, cs] : m_clientSessions) {
            report << "Client " << id << " (" << cs.clientIP << ")\n";
            report << "  Packets: recv=" << cs.totalPacketsReceived
                   << " sent=" << cs.totalPacketsSent << "\n";
            report << "  Bytes: recv=" << cs.totalBytesReceived
                   << " sent=" << cs.totalBytesSent << "\n";
            report << "  Handshake: " << (cs.handshakeComplete ? "complete" : "in progress")
                   << " (stage " << cs.handshakeStage << ")\n";
            if (!cs.tagCounts.empty()) {
                report << "  Packet types seen:\n";
                for (const auto& [tag, count] : cs.tagCounts) {
                    report << "    " << tag << ": " << count << "\n";
                }
            }
        }
    }

    return report.str();
}

// ---- Callbacks ----

void ProtocolDecoder::SetDiscoveryCallback(ProtocolDiscoveryCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_discoveryCallback = std::move(cb);
}

ProtocolDecoder::DecoderStatistics ProtocolDecoder::GetStatistics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

// ---- Helper Methods ----

std::string ProtocolDecoder::FieldTypeToString(DetectedFieldType type) const {
    switch (type) {
        case DetectedFieldType::Unknown:        return "unknown";
        case DetectedFieldType::UInt8:           return "uint8";
        case DetectedFieldType::UInt16:          return "uint16";
        case DetectedFieldType::UInt32:          return "uint32";
        case DetectedFieldType::UInt64:          return "uint64";
        case DetectedFieldType::Int8:            return "int8";
        case DetectedFieldType::Int16:           return "int16";
        case DetectedFieldType::Int32:           return "int32";
        case DetectedFieldType::Float32:         return "float32";
        case DetectedFieldType::Float64:         return "float64";
        case DetectedFieldType::String:          return "string";
        case DetectedFieldType::Vector3:         return "vector3";
        case DetectedFieldType::Boolean:         return "bool";
        case DetectedFieldType::Bitfield:        return "bitfield";
        case DetectedFieldType::BlobData:        return "blob";
        case DetectedFieldType::Timestamp:       return "timestamp";
        case DetectedFieldType::EntityId:        return "entity_id";
        case DetectedFieldType::Enum8:           return "enum8";
        case DetectedFieldType::Enum16:          return "enum16";
        case DetectedFieldType::CompressedData:  return "compressed";
        case DetectedFieldType::Padding:         return "padding";
    }
    return "unknown";
}

std::string ProtocolDecoder::GenerateFieldName(DetectedFieldType type, size_t fieldIndex) const {
    std::string prefix;
    switch (type) {
        case DetectedFieldType::UInt8:
        case DetectedFieldType::UInt16:
        case DetectedFieldType::UInt32:
        case DetectedFieldType::UInt64:      prefix = "value"; break;
        case DetectedFieldType::Int8:
        case DetectedFieldType::Int16:
        case DetectedFieldType::Int32:       prefix = "signed_value"; break;
        case DetectedFieldType::Float32:
        case DetectedFieldType::Float64:     prefix = "float_value"; break;
        case DetectedFieldType::String:      prefix = "text"; break;
        case DetectedFieldType::Vector3:     prefix = "position"; break;
        case DetectedFieldType::Boolean:     prefix = "flag"; break;
        case DetectedFieldType::Bitfield:    prefix = "flags"; break;
        case DetectedFieldType::BlobData:    prefix = "data"; break;
        case DetectedFieldType::Timestamp:   prefix = "timestamp"; break;
        case DetectedFieldType::EntityId:    prefix = "entity_id"; break;
        case DetectedFieldType::Enum8:
        case DetectedFieldType::Enum16:      prefix = "type"; break;
        case DetectedFieldType::CompressedData: prefix = "compressed"; break;
        case DetectedFieldType::Padding:     prefix = "pad"; break;
        default:                             prefix = "field"; break;
    }
    return prefix + "_" + std::to_string(fieldIndex);
}

std::string ProtocolDecoder::StructureToJson(const DecodedPacketStructure& structure) const {
    std::ostringstream json;
    json << "\"" << structure.packetTag << "\": {\n";
    json << "      \"totalSamples\": " << structure.totalSamples << ",\n";
    json << "      \"confidence\": " << std::fixed << std::setprecision(3) << structure.structureConfidence << ",\n";
    json << "      \"minPayloadSize\": " << structure.minPayloadSize << ",\n";
    json << "      \"maxPayloadSize\": " << structure.maxPayloadSize << ",\n";
    json << "      \"avgPayloadSize\": " << std::setprecision(1) << structure.avgPayloadSize << ",\n";
    json << "      \"clientToServer\": " << structure.clientToServerCount << ",\n";
    json << "      \"serverToClient\": " << structure.serverToClientCount << ",\n";
    json << "      \"fields\": [\n";

    for (size_t i = 0; i < structure.fields.size(); ++i) {
        const auto& f = structure.fields[i];
        json << "        {\n";
        json << "          \"name\": \"" << f.suggestedName << "\",\n";
        json << "          \"offset\": " << f.offset << ",\n";
        json << "          \"size\": " << f.size << ",\n";
        json << "          \"type\": \"" << FieldTypeToString(f.type) << "\",\n";
        json << "          \"confidence\": " << std::setprecision(3) << f.confidence << ",\n";
        json << "          \"samples\": " << f.sampleCount;

        if (f.type != DetectedFieldType::String && f.sampleCount > 0) {
            json << ",\n";
            json << "          \"min\": " << std::setprecision(2) << f.minValue << ",\n";
            json << "          \"max\": " << f.maxValue << ",\n";
            json << "          \"avg\": " << f.avgValue;
        }

        if (f.type == DetectedFieldType::String) {
            json << ",\n";
            json << "          \"minLength\": " << f.minLength << ",\n";
            json << "          \"maxLength\": " << f.maxLength;
        }

        if (!f.observedValues.empty() && f.observedValues.size() <= 20) {
            json << ",\n          \"observedValues\": {";
            bool first = true;
            for (const auto& [v, c] : f.observedValues) {
                if (!first) json << ", ";
                first = false;
                json << "\"" << v << "\": " << c;
            }
            json << "}";
        }

        json << "\n        }";
        if (i + 1 < structure.fields.size()) json << ",";
        json << "\n";
    }

    json << "      ]\n";
    json << "    }";
    return json.str();
}

void ProtocolDecoder::MaybeAutoExport() {
    if (!m_config.exportJsonDefinitions) return;
    if (m_config.exportIntervalSeconds <= 0) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastExportTime);
    if (elapsed.count() >= m_config.exportIntervalSeconds) {
        m_lastExportTime = now;
        // Export outside lock — but we're already locked, so just write directly
        std::string path = m_config.outputDirectory + "/protocol_definitions.json";
        std::ofstream file(path);
        if (file.is_open()) {
            file << "{\n  \"autoExport\": true,\n  \"structures\": {\n";
            bool first = true;
            for (const auto& [tag, s] : m_decodedStructures) {
                if (!first) file << ",\n";
                first = false;
                file << "    " << StructureToJson(s);
            }
            file << "\n  }\n}\n";
            file.close();
        }
    }
}
