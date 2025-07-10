// src/Utils/PacketAnalysis.cpp

#include "Utils/PacketAnalysis.h"
#include "Protocol/PacketTypes.h"
#include "Protocol/ProtocolUtils.h"
#include "Protocol/MessageDecoder.h"
#include "Protocol/CompressionHandler.h"
#include "Network/NetworkUtils.h"
#include "Utils/PerformanceProfiler.h"
#include "Utils/FileUtils.h"
#include "Utils/HandlerLibraryManager.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace PacketAnalysisUtils;
using GeneratedHandlers::HandlerLibraryManager;

static std::unique_ptr<PacketAnalyzer> g_globalAnalyzer;

struct PacketAnalyzer::Impl {
    AnalysisFlags flags = AnalysisFlags::ALL;
    std::string outputDir = "packet_analysis";
    size_t maxFileSize = 10 * 1024 * 1024;
    AnalysisCallback callback;

    bool patternDetectionEnabled = true;
    std::unordered_map<std::string, TrafficPattern> patterns;

    AnalysisStatistics stats{};

    std::ofstream currentLogFile;
    size_t currentFileSize = 0;
    int fileCounter = 0;

    std::unique_ptr<PerformanceProfiler> profiler;

    void EnsureOutputDirectory() {
        std::error_code ec;
        if (!std::filesystem::create_directories(outputDir, ec) && ec) {
            Logger::Warn("PacketAnalyzer: could not create directory '%s': %s",
                         outputDir.c_str(), ec.message().c_str());
        }
    }

    void RotateLogFile() {
        if (currentLogFile.is_open()) currentLogFile.close();
        std::string filename = outputDir + "/packet_analysis_" + std::to_string(fileCounter++) + ".log";
        currentLogFile.open(filename, std::ios::app);
        if (!currentLogFile.is_open()) {
            Logger::Error("PacketAnalyzer: failed to open log file '%s'", filename.c_str());
        }
        currentFileSize = 0;
    }

    void WriteToLog(const std::string& content) {
        if (!currentLogFile.is_open() || currentFileSize >= maxFileSize) {
            RotateLogFile();
        }
        if (currentLogFile.is_open()) {
            currentLogFile << content << "\n";
            currentFileSize += content.size() + 1;
            currentLogFile.flush();
        }
    }
};

PacketAnalyzer::PacketAnalyzer()
    : m_impl(std::make_unique<Impl>()){
    m_impl->profiler = std::make_unique<PerformanceProfiler>();
}

PacketAnalyzer::~PacketAnalyzer() = default;

void PacketAnalyzer::Initialize(const std::string& outputDir) {
    if (!g_globalAnalyzer) {
        m_impl->outputDir = outputDir.empty() ? m_impl->outputDir : outputDir;
        m_impl->EnsureOutputDirectory();
        m_impl->RotateLogFile();
        Logger::Info("PacketAnalyzer initialized with output directory: %s", m_impl->outputDir.c_str());
    }
}

void PacketAnalyzer::Shutdown() {
    if (m_impl->currentLogFile.is_open()) m_impl->currentLogFile.close();
    Logger::Info("PacketAnalyzer shutdown");
}

PacketAnalysisResult PacketAnalyzer::AnalyzePacket(
    const std::vector<uint8_t>& data,
    const std::string& context,
    uint32_t clientId,
    AnalysisFlags flags) {

    PacketAnalysisResult result{};
    result.context = context;
    result.clientId = clientId;
    result.payloadSize = data.size();
    result.isValid = true;

    if (data.empty()) {
        result.isValid = false;
        result.errors.push_back("Empty packet data");
        Logger::Warn("PacketAnalyzer: %s - empty packet received", context.c_str());
        return result;
    }

    auto& stats = m_impl->stats;
    stats.totalPacketsAnalyzed++;
    stats.totalBytesAnalyzed += data.size();
    stats.clientPacketCounts[clientId]++;

    auto start = std::chrono::high_resolution_clock::now();

    if (uint32_t(flags) & uint32_t(AnalysisFlags::HEX_DUMP)) {
        result.hexDump = GenerateHexDump(data, true);
    }
    if (uint32_t(flags) & uint32_t(AnalysisFlags::PROTOCOL_VALIDATION)) {
        if (!ValidatePacketIntegrity(data)) {
            result.isValid = false;
            result.errors.push_back("Invalid packet structure");
            stats.malformedPackets++;
            Logger::Error("PacketAnalyzer: %s - packet integrity failed", context.c_str());
        }
    }
    if (uint32_t(flags) & uint32_t(AnalysisFlags::SECURITY_ANALYSIS)) {
        auto anomalies = DetectAnomalies(data, context);
        if (!anomalies.empty()) {
            result.warnings.insert(result.warnings.end(), anomalies.begin(), anomalies.end());
            stats.suspiciousPackets++;
            Logger::Warn("PacketAnalyzer: %s - security anomalies detected", context.c_str());
        }
    }
    if (m_impl->patternDetectionEnabled &&
        uint32_t(flags) & uint32_t(AnalysisFlags::PATTERN_DETECTION)) {
        std::string key = context + "_" + std::to_string(data.size());
        auto now = std::chrono::steady_clock::now();
        auto& pat = m_impl->patterns[key];
        if (++pat.frequency == 1) {
            pat.patternType = context;
            pat.firstSeen = now;
        }
        pat.lastSeen = now;
        pat.clientIds.push_back(clientId);
        if (pat.frequency > 100 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - pat.firstSeen).count() < 10) {
            pat.suspicious = true;
            result.warnings.push_back("Suspicious traffic pattern detected");
            Logger::Warn("PacketAnalyzer: %s - suspicious pattern", context.c_str());
        }
    }
    if (uint32_t(flags) & uint32_t(AnalysisFlags::PERFORMANCE_METRICS)) {
        m_impl->profiler->Begin("PacketAnalysis");
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        m_impl->profiler->End("PacketAnalysis");
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.processingTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    stats.totalAnalysisTime += result.processingTime;

    std::ostringstream entry;
    entry << "[" << std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count() << "] "
          << "Context:" << context << ", Client:" << clientId
          << ", Size:" << data.size() << "B, Valid:" << (result.isValid?"true":"false")
          << ", Time:" << result.processingTime.count() << "μs";
    if (!result.errors.empty()) {
        entry << ", Errors:[";
        for (size_t i=0;i<result.errors.size();++i){
            if (i) entry<<", ";
            entry<<result.errors[i];
        }
        entry<<"]";
    }
    if (!result.warnings.empty()) {
        entry << ", Warnings:[";
        for (size_t i=0;i<result.warnings.size();++i){
            if (i) entry<<", ";
            entry<<result.warnings[i];
        }
        entry<<"]";
    }
    m_impl->WriteToLog(entry.str());

    if (m_impl->callback) {
        try { m_impl->callback(result); }
        catch (const std::exception& ex) {
            Logger::Error("PacketAnalyzer callback error: %s", ex.what());
        }
    }

    // dynamic handler dispatch
    PacketType type = ProtocolUtils::TagToType(result.packetTag);
    std::string handlerName;
    switch(type) {
        case PacketType::PT_HEARTBEAT:           handlerName="Handle_HEARTBEAT"; break;
        case PacketType::PT_CHAT_MESSAGE:        handlerName="Handle_CHAT_MESSAGE"; break;
        case PacketType::PT_PLAYER_SPAWN:        handlerName="Handle_PLAYER_SPAWN"; break;
        case PacketType::PT_PLAYER_MOVE:         handlerName="Handle_PLAYER_MOVE"; break;
        case PacketType::PT_PLAYER_ACTION:       handlerName="Handle_PLAYER_ACTION"; break;
        case PacketType::PT_HEALTH_UPDATE:       handlerName="Handle_HEALTH_UPDATE"; break;
        case PacketType::PT_TEAM_UPDATE:         handlerName="Handle_TEAM_UPDATE"; break;
        case PacketType::PT_SPAWN_ENTITY:        handlerName="Handle_SPAWN_ENTITY"; break;
        case PacketType::PT_DESPAWN_ENTITY:      handlerName="Handle_DESPAWN_ENTITY"; break;
        case PacketType::PT_ACTOR_REPLICATION:   handlerName="Handle_ACTOR_REPLICATION"; break;
        case PacketType::PT_OBJECTIVE_UPDATE:    handlerName="Handle_OBJECTIVE_UPDATE"; break;
        case PacketType::PT_SCORE_UPDATE:        handlerName="Handle_SCORE_UPDATE"; break;
        case PacketType::PT_SESSION_STATE:       handlerName="Handle_SESSION_STATE"; break;
        case PacketType::PT_CHAT_HISTORY:        handlerName="Handle_CHAT_HISTORY"; break;
        case PacketType::PT_ADMIN_COMMAND:       handlerName="Handle_ADMIN_COMMAND"; break;
        case PacketType::PT_SERVER_NOTIFICATION: handlerName="Handle_SERVER_NOTIFICATION"; break;
        case PacketType::PT_MAP_CHANGE:          handlerName="Handle_MAP_CHANGE"; break;
        case PacketType::PT_CONFIG_SYNC:         handlerName="Handle_CONFIG_SYNC"; break;
        case PacketType::PT_COMPRESSION:         handlerName="Handle_COMPRESSION"; break;
        case PacketType::PT_RPC_CALL:            handlerName="Handle_RPC_CALL"; break;
        case PacketType::PT_RPC_RESPONSE:        handlerName="Handle_RPC_RESPONSE"; break;
        default: break;
    }
    if (!handlerName.empty()) {
        auto fn = HandlerLibraryManager::Instance().GetHandler(handlerName);
        if (fn) {
            try { fn(result); }
            catch (const std::exception& ex) {
                Logger::Error("Handler %s threw exception: %s", handlerName.c_str(), ex.what());
            }
        }
    }

    return result;
}

// overload for Packet object
PacketAnalysisResult PacketAnalyzer::AnalyzePacket(
    const Packet& packet,
    const PacketMetadata& meta,
    const std::string& context,
    AnalysisFlags flags) {
    auto res = AnalyzePacket(packet.RawData(), context, meta.clientId, flags);
    if (uint32_t(flags)&uint32_t(AnalysisFlags::STRUCTURED_DECODE)) {
        res.packetTag = packet.GetTag();
        res.structuredData = DecodePacketStructure(packet);
        m_impl->stats.packetTypeCounts[res.packetTag]++;
    }
    return res;
}

void DumpPacketForAnalysis(const std::vector<uint8_t>& data, const std::string& context) {
    if (!g_globalAnalyzer) {
        g_globalAnalyzer = std::make_unique<PacketAnalyzer>();
        g_globalAnalyzer->Initialize("");
    }
    g_globalAnalyzer->AnalyzePacket(data, context, 0, AnalysisFlags::ALL);
}

void DumpPacketForAnalysis(const Packet& packet, const PacketMetadata& meta, const std::string& context) {
    if (!g_globalAnalyzer) {
        g_globalAnalyzer = std::make_unique<PacketAnalyzer>();
        g_globalAnalyzer->Initialize("");
    }
    g_globalAnalyzer->AnalyzePacket(packet, meta, context, AnalysisFlags::ALL);
}

// Utility functions
namespace PacketAnalysisUtils {
    std::string GenerateHexDump(const std::vector<uint8_t>& data, bool includeAscii) {
        std::ostringstream oss;
        const size_t bytesPerLine = 16;
        
        for (size_t i = 0; i < data.size(); i += bytesPerLine) {
            // Offset
            oss << std::hex << std::setw(8) << std::setfill('0') << i << ": ";
            
            // Hex bytes
            for (size_t j = 0; j < bytesPerLine; ++j) {
                if (i + j < data.size()) {
                    oss << std::hex << std::setw(2) << std::setfill('0') 
                        << static_cast<int>(data[i + j]) << " ";
                } else {
                    oss << "   ";
                }
            }
            
            // ASCII representation
            if (includeAscii) {
                oss << " |";
                for (size_t j = 0; j < bytesPerLine && i + j < data.size(); ++j) {
                    char c = static_cast<char>(data[i + j]);
                    oss << (std::isprint(c) ? c : '.');
                }
                oss << "|";
            }
            
            oss << "\n";
        }
        
        return oss.str();
    }
    
    std::string DecodePacketStructure(const Packet& packet) {
        std::ostringstream oss;
        // Resolve tag to enum
        auto tagStr = packet.GetTag();
        PacketType type = ProtocolUtils::TagToType(tagStr);

        oss << "Tag: " << tagStr << "\n";
        oss << "Payload Size: " << packet.GetPayloadSize() << " bytes\n";

        // Decode each RS2V packet type explicitly
        switch (type) {
            case PacketType::PT_HEARTBEAT:
                oss << "Type: Heartbeat\n";
                oss << "Timestamp: " << packet.ReadUInt64() << "\n";
                break;

            case PacketType::PT_CHAT_MESSAGE:
                oss << "Type: Chat Message\n";
                oss << "SenderID: " << packet.ReadUInt32() << "\n";
                oss << "Message: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_PLAYER_SPAWN:
                oss << "Type: Player Spawn\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "SpawnLocation: (" 
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ")\n";
                break;

            case PacketType::PT_PLAYER_MOVE:
                oss << "Type: Player Movement\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "Position: (" 
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ")\n";
                oss << "Rotation: (" 
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ")\n";
                break;

            case PacketType::PT_PLAYER_ACTION:
                oss << "Type: Player Action\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "ActionCode: " << packet.ReadUInt16() << "\n";
                oss << "ActionData: " << packet.ReadBytesRemaining() << " bytes\n";
                break;

            case PacketType::PT_HEALTH_UPDATE:
                oss << "Type: Health Update\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "NewHealth: " << packet.ReadFloat() << "\n";
                break;

            case PacketType::PT_TEAM_UPDATE:
                oss << "Type: Team Update\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "OldTeam: " << packet.ReadUInt8() << "\n";
                oss << "NewTeam: " << packet.ReadUInt8() << "\n";
                break;

            case PacketType::PT_SPAWN_ENTITY:
            case PacketType::PT_DESPAWN_ENTITY:
                oss << "Type: " 
                    << (type == PacketType::PT_SPAWN_ENTITY ? "Spawn Entity" : "Despawn Entity") << "\n";
                oss << "EntityID: " << packet.ReadUInt32() << "\n";
                break;

            case PacketType::PT_ACTOR_REPLICATION:
                oss << "Type: Actor Replication\n";
                oss << "ActorCount: " << packet.ReadUInt16() << "\n";
                // Could loop actor entries here if desired
                break;

            case PacketType::PT_OBJECTIVE_UPDATE:
                oss << "Type: Objective Update\n";
                oss << "ObjectiveID: " << packet.ReadUInt16() << "\n";
                oss << "State: " << packet.ReadUInt8() << "\n";
                break;

            case PacketType::PT_SCORE_UPDATE:
                oss << "Type: Score Update\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "NewScore: " << packet.ReadUInt32() << "\n";
                break;

            case PacketType::PT_SESSION_STATE:
                oss << "Type: Session State\n";
                oss << "AliveCount: " << packet.ReadUInt16() << "\n";
                oss << "TotalPlayers: " << packet.ReadUInt16() << "\n";
                break;

            case PacketType::PT_CHAT_HISTORY:
                oss << "Type: Chat History\n";
                {
                    uint16_t entries = packet.ReadUInt16();
                    oss << "Entries: " << entries << "\n";
                    for (int i = 0; i < entries; ++i) {
                        oss << "  [" << packet.ReadUInt32() << "] "
                            << packet.ReadString() << "\n";
                    }
                }
                break;

            case PacketType::PT_ADMIN_COMMAND:
                oss << "Type: Admin Command\n";
                oss << "Command: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_SERVER_NOTIFICATION:
                oss << "Type: Server Notification\n";
                oss << "Code: " << packet.ReadUInt16() << "\n";
                oss << "Message: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_MAP_CHANGE:
                oss << "Type: Map Change\n";
                oss << "NewMap: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_CONFIG_SYNC:
                oss << "Type: Config Sync\n";
                {
                    uint32_t syncVersion = 0;
                    if (packet.BytesRemaining() >= sizeof(syncVersion)) {
                        syncVersion = packet.ReadUInt32();
                        oss << "SyncVersion: " << syncVersion << "\n";
                    } else {
                        oss << "Warning: Insufficient data for SyncVersion\n";
                    }

                    uint16_t entryCount = 0;
                    if (packet.BytesRemaining() >= sizeof(entryCount)) {
                        entryCount = packet.ReadUInt16();
                        oss << "EntryCount: " << entryCount << "\n";
                    } else {
                        oss << "Warning: Insufficient data for EntryCount\n";
                    }

                    for (uint16_t i = 0; i < entryCount; ++i) {
                        std::string key;
                        try {
                            key = packet.ReadString();
                        } catch (...) {
                            oss << "Error: Failed to read key for entry " << i << "\n";
                            break;
                        }

                        std::string value;
                        try {
                            value = packet.ReadString();
                        } catch (...) {
                            oss << "Error: Failed to read value for key '" << key << "'\n";
                            break;
                        }

                        oss << "  [" << i << "] " << key << " = " << value << "\n";
                    }

                    size_t remaining = packet.BytesRemaining();
                    if (remaining > 0) {
                        oss << "TrailingBytes: " << remaining << "\n";
                        oss << "TrailingHex:\n"
                            << GenerateHexDump(packet.ReadBytesRemainingVector(), false);
                    }
                }
                break;

            case PacketType::PT_COMPRESSION:
                oss << "Type: Compressed Payload\n";
                oss << "OriginalSize: " << packet.ReadUInt32() << "\n";
                break;

            case PacketType::PT_RPC_CALL:
            case PacketType::PT_RPC_RESPONSE:
                oss << "Type: " 
                    << (type == PacketType::PT_RPC_CALL ? "RPC Call" : "RPC Response") << "\n";
                oss << "RPCName: " << packet.ReadString() << "\n";
                oss << "ArgsSize: " << packet.ReadBytesRemaining() << "\n";
                break;

            case PacketType::PT_CUSTOM_START:
            case PacketType::PT_MAX:
            default:
                oss << "Type: Custom/Unknown\n";
                oss << "Raw Data Hex:\n" << GenerateHexDump(packet.RawData(), false);
                break;
        }

        return oss.str();
    }
    
    bool ValidatePacketIntegrity(const std::vector<uint8_t>& data) {
        if (data.size() < 4) return false;
        if (data.size() > 1024 * 1024) return false;
        return true;
    }
    
    std::vector<std::string> DetectAnomalies(const std::vector<uint8_t>& data, const std::string& context) {
        std::vector<std::string> anomalies;
        if (data.size() > 10000) {
            anomalies.push_back("Unusually large packet size");
        }
        if (data.size() > 10) {
            std::unordered_map<uint8_t, int> byteFreq;
            for (uint8_t byte : data) {
                byteFreq[byte]++;
            }
            for (const auto& [byte, freq] : byteFreq) {
                if (freq > static_cast<int>(data.size() * 0.8)) {
                    anomalies.push_back("Suspicious byte pattern detected");
                    break;
                }
            }
        }
        return anomalies;
    }
    
    float CalculateCompressionRatio(const std::vector<uint8_t>& original, const std::vector<uint8_t>& compressed) {
        if (original.empty()) return 0.0f;
        return static_cast<float>(compressed.size()) / static_cast<float>(original.size());
    }
}