// src/Utils/PacketAnalysis.cpp

#include "Utils/PacketAnalysis.h"
#include "Utils/Logger.h"
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
        Logger::Trace("[PacketAnalyzer::Impl::EnsureOutputDirectory] Entry: outputDir='%s'", outputDir.c_str());
        std::error_code ec;
        if (!std::filesystem::create_directories(outputDir, ec) && ec) {
            Logger::Warn("PacketAnalyzer: could not create directory '%s': %s",
                         outputDir.c_str(), ec.message().c_str());
        } else {
            Logger::Debug("[PacketAnalyzer::Impl::EnsureOutputDirectory] Output directory ensured: '%s'", outputDir.c_str());
        }
        Logger::Trace("[PacketAnalyzer::Impl::EnsureOutputDirectory] Exit");
    }

    void RotateLogFile() {
        Logger::Trace("[PacketAnalyzer::Impl::RotateLogFile] Entry: fileCounter=%d, currentFileSize=%zu", fileCounter, currentFileSize);
        if (currentLogFile.is_open()) {
            Logger::Debug("[PacketAnalyzer::Impl::RotateLogFile] Closing current log file before rotation");
            currentLogFile.close();
        }
        std::string filename = outputDir + "/packet_analysis_" + std::to_string(fileCounter++) + ".log";
        Logger::Debug("[PacketAnalyzer::Impl::RotateLogFile] Opening new log file: '%s'", filename.c_str());
        currentLogFile.open(filename, std::ios::app);
        if (!currentLogFile.is_open()) {
            Logger::Error("PacketAnalyzer: failed to open log file '%s'", filename.c_str());
        } else {
            Logger::Info("[PacketAnalyzer::Impl::RotateLogFile] Log file rotated to '%s'", filename.c_str());
        }
        currentFileSize = 0;
        Logger::Trace("[PacketAnalyzer::Impl::RotateLogFile] Exit: fileCounter=%d", fileCounter);
    }

    void WriteToLog(const std::string& content) {
        Logger::Trace("[PacketAnalyzer::Impl::WriteToLog] Entry: content.size()=%zu, currentFileSize=%zu, maxFileSize=%zu",
                      content.size(), currentFileSize, maxFileSize);
        if (!currentLogFile.is_open() || currentFileSize >= maxFileSize) {
            Logger::Debug("[PacketAnalyzer::Impl::WriteToLog] Log file needs rotation (open=%s, currentFileSize=%zu, maxFileSize=%zu)",
                          currentLogFile.is_open() ? "true" : "false", currentFileSize, maxFileSize);
            RotateLogFile();
        }
        if (currentLogFile.is_open()) {
            currentLogFile << content << "\n";
            currentFileSize += content.size() + 1;
            currentLogFile.flush();
            Logger::Trace("[PacketAnalyzer::Impl::WriteToLog] Wrote %zu bytes to log, currentFileSize now=%zu", content.size() + 1, currentFileSize);
        } else {
            Logger::Error("[PacketAnalyzer::Impl::WriteToLog] Cannot write to log, file is not open");
        }
        Logger::Trace("[PacketAnalyzer::Impl::WriteToLog] Exit");
    }
};

PacketAnalyzer::PacketAnalyzer()
    : m_impl(std::make_unique<Impl>()){
    Logger::Trace("[PacketAnalyzer::PacketAnalyzer] Entry: constructor called");
    m_impl->profiler = std::make_unique<PerformanceProfiler>();
    Logger::Debug("[PacketAnalyzer::PacketAnalyzer] PerformanceProfiler instance created");
    Logger::Trace("[PacketAnalyzer::PacketAnalyzer] Exit: constructor complete");
}

PacketAnalyzer::~PacketAnalyzer() {
    Logger::Trace("[PacketAnalyzer::~PacketAnalyzer] Entry: destructor called");
    Logger::Debug("[PacketAnalyzer::~PacketAnalyzer] Destroying PacketAnalyzer, releasing Impl");
    Logger::Trace("[PacketAnalyzer::~PacketAnalyzer] Exit: destructor complete");
}

void PacketAnalyzer::Initialize(const std::string& outputDir) {
    Logger::Trace("[PacketAnalyzer::Initialize] Entry: outputDir='%s'", outputDir.c_str());
    if (!g_globalAnalyzer) {
        Logger::Debug("[PacketAnalyzer::Initialize] g_globalAnalyzer is null, proceeding with initialization");
        m_impl->outputDir = outputDir.empty() ? m_impl->outputDir : outputDir;
        Logger::Debug("[PacketAnalyzer::Initialize] Output directory set to: '%s'", m_impl->outputDir.c_str());
        m_impl->EnsureOutputDirectory();
        m_impl->RotateLogFile();
        Logger::Info("PacketAnalyzer initialized with output directory: %s", m_impl->outputDir.c_str());
    } else {
        Logger::Debug("[PacketAnalyzer::Initialize] g_globalAnalyzer already exists, skipping initialization");
    }
    Logger::Trace("[PacketAnalyzer::Initialize] Exit");
}

void PacketAnalyzer::Shutdown() {
    Logger::Trace("[PacketAnalyzer::Shutdown] Entry");
    if (m_impl->currentLogFile.is_open()) {
        Logger::Debug("[PacketAnalyzer::Shutdown] Closing current log file");
        m_impl->currentLogFile.close();
    } else {
        Logger::Debug("[PacketAnalyzer::Shutdown] No log file open to close");
    }
    Logger::Info("PacketAnalyzer shutdown");
    Logger::Trace("[PacketAnalyzer::Shutdown] Exit");
}

PacketAnalysisResult PacketAnalyzer::AnalyzePacket(
    const std::vector<uint8_t>& data,
    const std::string& context,
    uint32_t clientId,
    AnalysisFlags flags) {

    Logger::Trace("[PacketAnalyzer::AnalyzePacket] Entry: data.size()=%zu, context='%s', clientId=%u, flags=%u",
                  data.size(), context.c_str(), clientId, (unsigned)flags);

    PacketAnalysisResult result{};
    result.context = context;
    result.clientId = clientId;
    result.payloadSize = data.size();
    result.isValid = true;

    if (data.empty()) {
        result.isValid = false;
        result.errors.push_back("Empty packet data");
        Logger::Warn("PacketAnalyzer: %s - empty packet received", context.c_str());
        Logger::Trace("[PacketAnalyzer::AnalyzePacket] Exit: returning result (empty packet)");
        return result;
    }

    auto& stats = m_impl->stats;
    stats.totalPacketsAnalyzed++;
    stats.totalBytesAnalyzed += data.size();
    stats.clientPacketCounts[clientId]++;
    Logger::Debug("[PacketAnalyzer::AnalyzePacket] Stats updated: totalPackets=%zu, totalBytes=%zu, clientPackets[%u]=%zu",
                  stats.totalPacketsAnalyzed, stats.totalBytesAnalyzed, clientId, stats.clientPacketCounts[clientId]);

    auto start = std::chrono::high_resolution_clock::now();

    if (uint32_t(flags) & uint32_t(AnalysisFlags::HEX_DUMP)) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] HEX_DUMP flag set, generating hex dump for %zu bytes", data.size());
        result.hexDump = GenerateHexDump(data, true);
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] Hex dump generated, length=%zu", result.hexDump.size());
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] HEX_DUMP flag not set, skipping hex dump");
    }
    if (uint32_t(flags) & uint32_t(AnalysisFlags::PROTOCOL_VALIDATION)) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] PROTOCOL_VALIDATION flag set, validating packet integrity");
        if (!ValidatePacketIntegrity(data)) {
            result.isValid = false;
            result.errors.push_back("Invalid packet structure");
            stats.malformedPackets++;
            Logger::Error("PacketAnalyzer: %s - packet integrity failed", context.c_str());
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] Integrity check failed, malformedPackets=%zu", stats.malformedPackets);
        } else {
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] Packet integrity validation passed");
        }
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] PROTOCOL_VALIDATION flag not set, skipping validation");
    }
    if (uint32_t(flags) & uint32_t(AnalysisFlags::SECURITY_ANALYSIS)) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] SECURITY_ANALYSIS flag set, detecting anomalies");
        auto anomalies = DetectAnomalies(data, context);
        if (!anomalies.empty()) {
            result.warnings.insert(result.warnings.end(), anomalies.begin(), anomalies.end());
            stats.suspiciousPackets++;
            Logger::Warn("PacketAnalyzer: %s - security anomalies detected", context.c_str());
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] %zu anomalies detected, suspiciousPackets=%zu", anomalies.size(), stats.suspiciousPackets);
        } else {
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] No security anomalies detected");
        }
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] SECURITY_ANALYSIS flag not set, skipping anomaly detection");
    }
    if (m_impl->patternDetectionEnabled &&
        uint32_t(flags) & uint32_t(AnalysisFlags::PATTERN_DETECTION)) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] PATTERN_DETECTION flag set and enabled, analyzing patterns");
        std::string key = context + "_" + std::to_string(data.size());
        auto now = std::chrono::steady_clock::now();
        // GUARD (memory-exhaustion DoS): pattern detection runs on the ALWAYS-ON receive path.
        // Bound the pattern map so a wide spread of distinct (context,size) keys can't grow it
        // without limit. Reset history BEFORE inserting a new key (so we never clear the map
        // while holding a reference into it). Pattern detection is a short-window heuristic, so
        // dropping old history is harmless.
        constexpr size_t kMaxPatterns = 4096;
        if (m_impl->patterns.size() >= kMaxPatterns &&
            m_impl->patterns.find(key) == m_impl->patterns.end()) {
            Logger::Warn("PacketAnalyzer: pattern map hit %zu entries; clearing to bound memory", kMaxPatterns);
            m_impl->patterns.clear();
        }
        auto& pat = m_impl->patterns[key];
        if (++pat.frequency == 1) {
            pat.patternType = context;
            pat.firstSeen = now;
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] New pattern detected: key='%s'", key.c_str());
        }
        pat.lastSeen = now;
        // GUARD (memory-exhaustion DoS): clientIds was appended on EVERY analyzed packet and is
        // never read back - on the always-on path it grew unbounded until OOM. Keep only DISTINCT
        // client ids and cap the count (the field's only meaning is "which clients show this
        // pattern"), so a steady packet flood can no longer exhaust memory here.
        constexpr size_t kMaxClientIdsPerPattern = 256;  // ~max concurrent clients
        if (pat.clientIds.size() < kMaxClientIdsPerPattern &&
            std::find(pat.clientIds.begin(), pat.clientIds.end(), clientId) == pat.clientIds.end()) {
            pat.clientIds.push_back(clientId);
        }
        Logger::Trace("[PacketAnalyzer::AnalyzePacket] Pattern key='%s', frequency=%u", key.c_str(), pat.frequency);
        if (pat.frequency > 100 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - pat.firstSeen).count() < 10) {
            pat.suspicious = true;
            result.warnings.push_back("Suspicious traffic pattern detected");
            Logger::Warn("PacketAnalyzer: %s - suspicious pattern", context.c_str());
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] Pattern '%s' flagged as suspicious: frequency=%u in <10 seconds", key.c_str(), pat.frequency);
        }
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] Pattern detection skipped (enabled=%s, flag set=%s)",
                      m_impl->patternDetectionEnabled ? "true" : "false",
                      (uint32_t(flags) & uint32_t(AnalysisFlags::PATTERN_DETECTION)) ? "true" : "false");
    }
    if (uint32_t(flags) & uint32_t(AnalysisFlags::PERFORMANCE_METRICS)) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] PERFORMANCE_METRICS flag set, profiling packet analysis");
        m_impl->profiler->Begin("PacketAnalysis");
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        m_impl->profiler->End("PacketAnalysis");
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] Performance profiling section completed");
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] PERFORMANCE_METRICS flag not set, skipping profiling");
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.processingTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    stats.totalAnalysisTime += result.processingTime;
    Logger::Debug("[PacketAnalyzer::AnalyzePacket] Analysis took %lld microseconds", (long long)result.processingTime.count());

    std::ostringstream entry;
    entry << "[" << std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count() << "] "
          << "Context:" << context << ", Client:" << clientId
          << ", Size:" << data.size() << "B, Valid:" << (result.isValid?"true":"false")
          << ", Time:" << result.processingTime.count() << "us";
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
    Logger::Debug("[PacketAnalyzer::AnalyzePacket] Writing analysis entry to log file");
    m_impl->WriteToLog(entry.str());

    if (m_impl->callback) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] Invoking analysis callback");
        try { m_impl->callback(result); }
        catch (const std::exception& ex) {
            Logger::Error("PacketAnalyzer callback error: %s", ex.what());
        }
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] No callback registered, skipping callback invocation");
    }

    // dynamic handler dispatch
    Logger::Debug("[PacketAnalyzer::AnalyzePacket] Resolving packet type for dynamic handler dispatch, packetTag='%s'", result.packetTag.c_str());
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
        default:
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] Unknown packet type, no handler to dispatch");
            break;
    }
    if (!handlerName.empty()) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket] Dispatching to handler '%s'", handlerName.c_str());
        auto fn = HandlerLibraryManager::Instance().GetHandler(handlerName);
        if (fn) {
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] Handler '%s' found, invoking", handlerName.c_str());
            try { fn(result); }
            catch (const std::exception& ex) {
                Logger::Error("Handler %s threw exception: %s", handlerName.c_str(), ex.what());
            }
        } else {
            Logger::Debug("[PacketAnalyzer::AnalyzePacket] Handler '%s' not found in library", handlerName.c_str());
        }
    }

    Logger::Info("[PacketAnalyzer::AnalyzePacket] Packet analysis complete: context='%s', clientId=%u, size=%zu, valid=%s, errors=%zu, warnings=%zu",
                 context.c_str(), clientId, data.size(), result.isValid ? "true" : "false", result.errors.size(), result.warnings.size());
    Logger::Trace("[PacketAnalyzer::AnalyzePacket] Exit: returning result");
    return result;
}

// overload for Packet object
PacketAnalysisResult PacketAnalyzer::AnalyzePacket(
    const Packet& packet,
    const PacketMetadata& meta,
    const std::string& context,
    AnalysisFlags flags) {
    Logger::Trace("[PacketAnalyzer::AnalyzePacket(Packet)] Entry: context='%s', meta.clientId=%u, flags=%u",
                  context.c_str(), meta.clientId, (unsigned)flags);
    auto res = AnalyzePacket(packet.RawData(), context, meta.clientId, flags);
    if (uint32_t(flags)&uint32_t(AnalysisFlags::STRUCTURED_DECODE)) {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket(Packet)] STRUCTURED_DECODE flag set, decoding packet structure");
        res.packetTag = packet.GetTag();
        res.structuredData = DecodePacketStructure(packet);
        m_impl->stats.packetTypeCounts[res.packetTag]++;
        Logger::Debug("[PacketAnalyzer::AnalyzePacket(Packet)] Decoded packet tag='%s', structuredData.size()=%zu, typeCount=%zu",
                      res.packetTag.c_str(), res.structuredData.size(), m_impl->stats.packetTypeCounts[res.packetTag]);
    } else {
        Logger::Debug("[PacketAnalyzer::AnalyzePacket(Packet)] STRUCTURED_DECODE flag not set, skipping structure decode");
    }
    Logger::Trace("[PacketAnalyzer::AnalyzePacket(Packet)] Exit: returning result");
    return res;
}

void DumpPacketForAnalysis(const std::vector<uint8_t>& data, const std::string& context) {
    Logger::Trace("[DumpPacketForAnalysis] Entry: data.size()=%zu, context='%s'", data.size(), context.c_str());
    if (!g_globalAnalyzer) {
        Logger::Debug("[DumpPacketForAnalysis] Global analyzer not initialized, creating new instance");
        g_globalAnalyzer = std::make_unique<PacketAnalyzer>();
        g_globalAnalyzer->Initialize("");
        Logger::Info("[DumpPacketForAnalysis] Global PacketAnalyzer created and initialized with default output directory");
    }
    Logger::Debug("[DumpPacketForAnalysis] Delegating to global analyzer for packet of %zu bytes", data.size());
    g_globalAnalyzer->AnalyzePacket(data, context, 0, AnalysisFlags::ALL);
    Logger::Trace("[DumpPacketForAnalysis] Exit");
}

void DumpPacketForAnalysis(const Packet& packet, const PacketMetadata& meta, const std::string& context) {
    Logger::Trace("[DumpPacketForAnalysis(Packet)] Entry: context='%s', meta.clientId=%u", context.c_str(), meta.clientId);
    if (!g_globalAnalyzer) {
        Logger::Debug("[DumpPacketForAnalysis(Packet)] Global analyzer not initialized, creating new instance");
        g_globalAnalyzer = std::make_unique<PacketAnalyzer>();
        g_globalAnalyzer->Initialize("");
        Logger::Info("[DumpPacketForAnalysis(Packet)] Global PacketAnalyzer created and initialized with default output directory");
    }
    Logger::Debug("[DumpPacketForAnalysis(Packet)] Delegating to global analyzer");
    g_globalAnalyzer->AnalyzePacket(packet, meta, context, AnalysisFlags::ALL);
    Logger::Trace("[DumpPacketForAnalysis(Packet)] Exit");
}

// Utility functions
namespace PacketAnalysisUtils {
    std::string GenerateHexDump(const std::vector<uint8_t>& data, bool includeAscii) {
        Logger::Trace("[PacketAnalysisUtils::GenerateHexDump] Entry: data.size()=%zu, includeAscii=%s",
                      data.size(), includeAscii ? "true" : "false");
        std::ostringstream oss;
        const size_t bytesPerLine = 16;

        Logger::Debug("[PacketAnalysisUtils::GenerateHexDump] Generating hex dump with %zu bytes per line", bytesPerLine);
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

        std::string result = oss.str();
        Logger::Debug("[PacketAnalysisUtils::GenerateHexDump] Hex dump generated, output length=%zu chars", result.size());
        Logger::Trace("[PacketAnalysisUtils::GenerateHexDump] Exit: returning hex dump string");
        return result;
    }

    std::string DecodePacketStructure(const Packet& packet) {
        Logger::Trace("[PacketAnalysisUtils::DecodePacketStructure] Entry");
        std::ostringstream oss;
        // Resolve tag to enum
        auto tagStr = packet.GetTag();
        PacketType type = ProtocolUtils::TagToType(tagStr);
        Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding packet with tag='%s', payloadSize=%u",
                      tagStr.c_str(), packet.GetPayloadSize());

        oss << "Tag: " << tagStr << "\n";
        oss << "Payload Size: " << packet.GetPayloadSize() << " bytes\n";

        // Decode each RS2V packet type explicitly
        switch (type) {
            case PacketType::PT_HEARTBEAT:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Heartbeat packet");
                oss << "Type: Heartbeat\n";
                oss << "Timestamp: " << packet.ReadUInt64() << "\n";
                break;

            case PacketType::PT_CHAT_MESSAGE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Chat Message packet");
                oss << "Type: Chat Message\n";
                oss << "SenderID: " << packet.ReadUInt32() << "\n";
                oss << "Message: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_PLAYER_SPAWN:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Player Spawn packet");
                oss << "Type: Player Spawn\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "SpawnLocation: ("
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ", "
                    << packet.ReadFloat() << ")\n";
                break;

            case PacketType::PT_PLAYER_MOVE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Player Movement packet");
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
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Player Action packet");
                oss << "Type: Player Action\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "ActionCode: " << packet.ReadUInt16() << "\n";
                oss << "ActionData: " << packet.ReadBytesRemaining() << " bytes\n";
                break;

            case PacketType::PT_HEALTH_UPDATE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Health Update packet");
                oss << "Type: Health Update\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "NewHealth: " << packet.ReadFloat() << "\n";
                break;

            case PacketType::PT_TEAM_UPDATE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Team Update packet");
                oss << "Type: Team Update\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "OldTeam: " << packet.ReadUInt8() << "\n";
                oss << "NewTeam: " << packet.ReadUInt8() << "\n";
                break;

            case PacketType::PT_SPAWN_ENTITY:
            case PacketType::PT_DESPAWN_ENTITY:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding %s packet",
                              type == PacketType::PT_SPAWN_ENTITY ? "Spawn Entity" : "Despawn Entity");
                oss << "Type: "
                    << (type == PacketType::PT_SPAWN_ENTITY ? "Spawn Entity" : "Despawn Entity") << "\n";
                oss << "EntityID: " << packet.ReadUInt32() << "\n";
                break;

            case PacketType::PT_ACTOR_REPLICATION:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Actor Replication packet");
                oss << "Type: Actor Replication\n";
                oss << "ActorCount: " << packet.ReadUInt16() << "\n";
                // Could loop actor entries here if desired
                break;

            case PacketType::PT_OBJECTIVE_UPDATE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Objective Update packet");
                oss << "Type: Objective Update\n";
                oss << "ObjectiveID: " << packet.ReadUInt16() << "\n";
                oss << "State: " << packet.ReadUInt8() << "\n";
                break;

            case PacketType::PT_SCORE_UPDATE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Score Update packet");
                oss << "Type: Score Update\n";
                oss << "PlayerID: " << packet.ReadUInt32() << "\n";
                oss << "NewScore: " << packet.ReadUInt32() << "\n";
                break;

            case PacketType::PT_SESSION_STATE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Session State packet");
                oss << "Type: Session State\n";
                oss << "AliveCount: " << packet.ReadUInt16() << "\n";
                oss << "TotalPlayers: " << packet.ReadUInt16() << "\n";
                break;

            case PacketType::PT_CHAT_HISTORY:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Chat History packet");
                oss << "Type: Chat History\n";
                {
                    uint16_t entries = packet.ReadUInt16();
                    oss << "Entries: " << entries << "\n";
                    Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Chat History has %u entries", entries);
                    for (int i = 0; i < entries; ++i) {
                        oss << "  [" << packet.ReadUInt32() << "] "
                            << packet.ReadString() << "\n";
                        Logger::Trace("[PacketAnalysisUtils::DecodePacketStructure] Decoded chat history entry %d of %u", i + 1, entries);
                    }
                }
                break;

            case PacketType::PT_ADMIN_COMMAND:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Admin Command packet");
                oss << "Type: Admin Command\n";
                oss << "Command: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_SERVER_NOTIFICATION:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Server Notification packet");
                oss << "Type: Server Notification\n";
                oss << "Code: " << packet.ReadUInt16() << "\n";
                oss << "Message: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_MAP_CHANGE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Map Change packet");
                oss << "Type: Map Change\n";
                oss << "NewMap: " << packet.ReadString() << "\n";
                break;

            case PacketType::PT_CONFIG_SYNC:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Config Sync packet");
                oss << "Type: Config Sync\n";
                {
                    uint32_t syncVersion = 0;
                    if (packet.BytesRemaining() >= sizeof(syncVersion)) {
                        syncVersion = packet.ReadUInt32();
                        oss << "SyncVersion: " << syncVersion << "\n";
                        Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Config SyncVersion=%u", syncVersion);
                    } else {
                        oss << "Warning: Insufficient data for SyncVersion\n";
                        Logger::Warn("[PacketAnalysisUtils::DecodePacketStructure] Insufficient data for SyncVersion, bytesRemaining=%zu", packet.BytesRemaining());
                    }

                    uint16_t entryCount = 0;
                    if (packet.BytesRemaining() >= sizeof(entryCount)) {
                        entryCount = packet.ReadUInt16();
                        oss << "EntryCount: " << entryCount << "\n";
                        Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Config EntryCount=%u", entryCount);
                    } else {
                        oss << "Warning: Insufficient data for EntryCount\n";
                        Logger::Warn("[PacketAnalysisUtils::DecodePacketStructure] Insufficient data for EntryCount, bytesRemaining=%zu", packet.BytesRemaining());
                    }

                    for (uint16_t i = 0; i < entryCount; ++i) {
                        std::string key;
                        try {
                            key = packet.ReadString();
                        } catch (const std::exception& ex) {
                            oss << "Error: Failed to read key for entry " << i << "\n";
                            Logger::Error("[PacketAnalysisUtils::DecodePacketStructure] Failed to read key for config entry %u: %s", i, ex.what());
                            break;
                        } catch (...) {
                            oss << "Error: Failed to read key for entry " << i << "\n";
                            Logger::Error("[PacketAnalysisUtils::DecodePacketStructure] Failed to read key for config entry %u: unknown exception", i);
                            break;
                        }

                        std::string value;
                        try {
                            value = packet.ReadString();
                        } catch (const std::exception& ex) {
                            oss << "Error: Failed to read value for key '" << key << "'\n";
                            Logger::Error("[PacketAnalysisUtils::DecodePacketStructure] Failed to read value for config key '%s': %s", key.c_str(), ex.what());
                            break;
                        } catch (...) {
                            oss << "Error: Failed to read value for key '" << key << "'\n";
                            Logger::Error("[PacketAnalysisUtils::DecodePacketStructure] Failed to read value for config key '%s': unknown exception", key.c_str());
                            break;
                        }

                        oss << "  [" << i << "] " << key << " = " << value << "\n";
                        Logger::Trace("[PacketAnalysisUtils::DecodePacketStructure] Config entry %u: key='%s', value='%s'", i, key.c_str(), value.c_str());
                    }

                    size_t remaining = packet.BytesRemaining();
                    if (remaining > 0) {
                        Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Config sync has %zu trailing bytes", remaining);
                        oss << "TrailingBytes: " << remaining << "\n";
                        oss << "TrailingHex:\n"
                            << GenerateHexDump(packet.ReadBytesRemainingVector(), false);
                    }
                }
                break;

            case PacketType::PT_COMPRESSION:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding Compressed Payload packet");
                oss << "Type: Compressed Payload\n";
                oss << "OriginalSize: " << packet.ReadUInt32() << "\n";
                break;

            case PacketType::PT_RPC_CALL:
            case PacketType::PT_RPC_RESPONSE:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Decoding %s packet",
                              type == PacketType::PT_RPC_CALL ? "RPC Call" : "RPC Response");
                oss << "Type: "
                    << (type == PacketType::PT_RPC_CALL ? "RPC Call" : "RPC Response") << "\n";
                oss << "RPCName: " << packet.ReadString() << "\n";
                oss << "ArgsSize: " << packet.ReadBytesRemaining() << "\n";
                break;

            case PacketType::PT_CUSTOM_START:
            case PacketType::PT_MAX:
            default:
                Logger::Debug("[PacketAnalysisUtils::DecodePacketStructure] Unknown/Custom packet type, dumping raw data");
                oss << "Type: Custom/Unknown\n";
                oss << "Raw Data Hex:\n" << GenerateHexDump(packet.RawData(), false);
                break;
        }

        std::string result = oss.str();
        Logger::Info("[PacketAnalysisUtils::DecodePacketStructure] Decoded packet structure for tag='%s', output length=%zu", tagStr.c_str(), result.size());
        Logger::Trace("[PacketAnalysisUtils::DecodePacketStructure] Exit: returning decoded structure");
        return result;
    }

    bool ValidatePacketIntegrity(const std::vector<uint8_t>& data) {
        Logger::Trace("[PacketAnalysisUtils::ValidatePacketIntegrity] Entry: data.size()=%zu", data.size());
        if (data.size() < 4) {
            Logger::Debug("[PacketAnalysisUtils::ValidatePacketIntegrity] Packet too small: %zu bytes (minimum 4)", data.size());
            Logger::Trace("[PacketAnalysisUtils::ValidatePacketIntegrity] Exit: returning false (too small)");
            return false;
        }
        if (data.size() > 1024 * 1024) {
            Logger::Debug("[PacketAnalysisUtils::ValidatePacketIntegrity] Packet too large: %zu bytes (maximum 1MB)", data.size());
            Logger::Trace("[PacketAnalysisUtils::ValidatePacketIntegrity] Exit: returning false (too large)");
            return false;
        }
        Logger::Debug("[PacketAnalysisUtils::ValidatePacketIntegrity] Packet size %zu is within valid range [4, 1048576]", data.size());
        Logger::Trace("[PacketAnalysisUtils::ValidatePacketIntegrity] Exit: returning true");
        return true;
    }

    std::vector<std::string> DetectAnomalies(const std::vector<uint8_t>& data, const std::string& context) {
        Logger::Trace("[PacketAnalysisUtils::DetectAnomalies] Entry: data.size()=%zu, context='%s'", data.size(), context.c_str());
        std::vector<std::string> anomalies;
        if (data.size() > 10000) {
            anomalies.push_back("Unusually large packet size");
            Logger::Debug("[PacketAnalysisUtils::DetectAnomalies] Anomaly: unusually large packet size (%zu bytes)", data.size());
        }
        if (data.size() > 10) {
            Logger::Debug("[PacketAnalysisUtils::DetectAnomalies] Performing byte frequency analysis on %zu bytes", data.size());
            std::unordered_map<uint8_t, int> byteFreq;
            for (uint8_t byte : data) {
                byteFreq[byte]++;
            }
            Logger::Trace("[PacketAnalysisUtils::DetectAnomalies] Byte frequency map has %zu unique bytes", byteFreq.size());
            for (const auto& [byte, freq] : byteFreq) {
                if (freq > static_cast<int>(data.size() * 0.8)) {
                    anomalies.push_back("Suspicious byte pattern detected");
                    Logger::Debug("[PacketAnalysisUtils::DetectAnomalies] Anomaly: byte 0x%02X appears %d times (>80%% of %zu bytes)", byte, freq, data.size());
                    break;
                }
            }
        } else {
            Logger::Debug("[PacketAnalysisUtils::DetectAnomalies] Packet too small for byte frequency analysis (%zu bytes, minimum 11)", data.size());
        }
        Logger::Debug("[PacketAnalysisUtils::DetectAnomalies] Detected %zu anomalies for context='%s'", anomalies.size(), context.c_str());
        Logger::Trace("[PacketAnalysisUtils::DetectAnomalies] Exit: returning %zu anomalies", anomalies.size());
        return anomalies;
    }

    float CalculateCompressionRatio(const std::vector<uint8_t>& original, const std::vector<uint8_t>& compressed) {
        Logger::Trace("[PacketAnalysisUtils::CalculateCompressionRatio] Entry: original.size()=%zu, compressed.size()=%zu",
                      original.size(), compressed.size());
        if (original.empty()) {
            Logger::Warn("[PacketAnalysisUtils::CalculateCompressionRatio] Original data is empty, returning 0.0");
            Logger::Trace("[PacketAnalysisUtils::CalculateCompressionRatio] Exit: returning 0.0f");
            return 0.0f;
        }
        float ratio = static_cast<float>(compressed.size()) / static_cast<float>(original.size());
        Logger::Debug("[PacketAnalysisUtils::CalculateCompressionRatio] Compression ratio: %f (compressed=%zu / original=%zu)",
                      ratio, compressed.size(), original.size());
        Logger::Trace("[PacketAnalysisUtils::CalculateCompressionRatio] Exit: returning %f", ratio);
        return ratio;
    }
}
