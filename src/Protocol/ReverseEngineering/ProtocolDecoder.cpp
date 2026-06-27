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

namespace {
// Explicit little-endian readers. The UE3 wire format is little-endian; reading
// via these (rather than a host-endian memcpy into a wider int) makes the byte
// order intentional and correct on a big-endian host too.
inline uint16_t ReadLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t ReadLE64(const uint8_t* p) {
    return static_cast<uint64_t>(ReadLE32(p)) |
           (static_cast<uint64_t>(ReadLE32(p + 4)) << 32);
}
inline float ReadLEFloat(const uint8_t* p) {
    uint32_t bits = ReadLE32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
} // namespace

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

        // Load per-class net-field handle tables as decoding PRIORS. These turn
        // the bunch path from byte-guessing into named property decode.
        if (m_config.decodeBunchProperties) {
            size_t n = m_netFields.LoadDirectory(m_config.netfieldsDir);
            m_propDecoder = std::make_unique<BunchPropertyDecoder>(m_config.maxChannels);
            if (n == 0) {
                Logger::Warn("ProtocolDecoder: no net-field tables in '%s' — bunch "
                             "property decoding will be name-less", m_config.netfieldsDir.c_str());
            }
        }

        // Merge cumulative per-tag stats from a prior run.
        if (m_config.persistState) {
            LoadPersistentState();
        }
    }

    m_initialized = true;

    // Start the async analysis worker (off the network hot path).
    if (m_config.enabled && m_config.asyncAnalysis) {
        m_workerRunning = true;
        m_worker = std::thread(&ProtocolDecoder::WorkerLoop, this);
    }

    Logger::Info("ProtocolDecoder initialized (enabled=%s, async=%s, classes=%zu, output=%s)",
                 m_config.enabled ? "true" : "false",
                 m_config.asyncAnalysis ? "true" : "false",
                 m_netFields.ClassCount(),
                 m_config.outputDirectory.c_str());
}

void ProtocolDecoder::Shutdown() {
    // Stop the worker FIRST (outside m_mutex) so no analysis runs during teardown.
    if (m_workerRunning.exchange(false)) {
        m_queueCv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;

    // Final export + persist before shutdown (lock already held — no dance).
    if (m_config.exportJsonDefinitions && !m_decodedStructures.empty()) {
        ExportProtocolDefinitionsLocked("");
    }
    if (m_config.persistState) {
        SavePersistentState();
    }

    Logger::Info("ProtocolDecoder shutdown — analyzed %llu packets, decoded %llu structures "
                 "(%llu events dropped under load)",
                 (unsigned long long)m_stats.totalPacketsAnalyzed,
                 (unsigned long long)m_stats.structuresDecoded,
                 (unsigned long long)m_droppedEvents);

    m_clientSessions.clear();
    m_decodedStructures.clear();
    m_bunchHistory.clear();
    m_channelStats.clear();
    m_initialized = false;
}

// ---- Async pipeline: enqueue on the hot path, analyze on the worker ----

void ProtocolDecoder::Enqueue(CaptureEvent&& ev) {
    {
        std::lock_guard<std::mutex> qlock(m_queueMutex);
        // Bounded queue: under a flood we drop the OLDEST event and count it,
        // so capture memory is capped and the socket thread never blocks.
        if (m_queue.size() >= m_config.maxAnalysisQueue) {
            m_queue.pop_front();
            ++m_droppedEvents;
        }
        m_queue.push_back(std::move(ev));
    }
    m_queueCv.notify_one();
}

void ProtocolDecoder::WorkerLoop() {
    while (true) {
        std::deque<CaptureEvent> batch;
        {
            std::unique_lock<std::mutex> qlock(m_queueMutex);
            m_queueCv.wait(qlock, [this]{ return !m_queue.empty() || !m_workerRunning; });
            if (!m_workerRunning && m_queue.empty()) break;
            batch.swap(m_queue);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& ev : batch) ProcessEvent(ev);
    }
}

void ProtocolDecoder::ProcessEvent(const CaptureEvent& ev) {
    switch (ev.kind) {
        case CaptureKind::Connected:    HandleConnected(ev.clientId, ev.ip); break;
        case CaptureKind::Disconnected: HandleDisconnected(ev.clientId); break;
        case CaptureKind::PacketRecv:   HandlePacket(ev.clientId, ev.data, ev.tag, true); break;
        case CaptureKind::PacketSent:   HandlePacket(ev.clientId, ev.data, ev.tag, false); break;
        case CaptureKind::RawUDP:       HandleRawUDP(ev.clientId, ev.data); break;
    }
}

// ---- Client Session Management ----

void ProtocolDecoder::OnClientConnected(uint32_t clientId, const std::string& ip) {
    if (!m_config.enabled) return;
    if (m_config.asyncAnalysis) {
        CaptureEvent ev; ev.kind = CaptureKind::Connected; ev.clientId = clientId; ev.ip = ip;
        Enqueue(std::move(ev));
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        HandleConnected(clientId, ip);
    }
}

void ProtocolDecoder::OnClientDisconnected(uint32_t clientId) {
    if (!m_config.enabled) return;
    if (m_config.asyncAnalysis) {
        CaptureEvent ev; ev.kind = CaptureKind::Disconnected; ev.clientId = clientId;
        Enqueue(std::move(ev));
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        HandleDisconnected(clientId);
    }
}

void ProtocolDecoder::OnPacketReceived(uint32_t clientId, const std::vector<uint8_t>& rawData,
                                       const std::string& tag) {
    if (!m_config.enabled || rawData.empty()) return;
    if (m_config.asyncAnalysis) {
        CaptureEvent ev; ev.kind = CaptureKind::PacketRecv; ev.clientId = clientId;
        ev.tag = tag; ev.data = rawData;
        Enqueue(std::move(ev));
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        HandlePacket(clientId, rawData, tag, true);
    }
}

void ProtocolDecoder::OnPacketSent(uint32_t clientId, const std::vector<uint8_t>& rawData,
                                    const std::string& tag) {
    if (!m_config.enabled || rawData.empty()) return;
    if (m_config.asyncAnalysis) {
        CaptureEvent ev; ev.kind = CaptureKind::PacketSent; ev.clientId = clientId;
        ev.tag = tag; ev.data = rawData;
        Enqueue(std::move(ev));
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        HandlePacket(clientId, rawData, tag, false);
    }
}

void ProtocolDecoder::OnRawUDPReceived(uint32_t clientId, const uint8_t* data, size_t len) {
    if (!m_config.enabled || !m_config.detectUE3Bunches || len < 8) return;
    if (m_config.asyncAnalysis) {
        CaptureEvent ev; ev.kind = CaptureKind::RawUDP; ev.clientId = clientId;
        ev.data.assign(data, data + len);
        Enqueue(std::move(ev));
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<uint8_t> copy(data, data + len);
        HandleRawUDP(clientId, copy);
    }
}

// ---- Analysis bodies (always invoked with m_mutex held) ----

void ProtocolDecoder::HandleConnected(uint32_t clientId, const std::string& ip) {
    auto& session = m_clientSessions[clientId];
    session = ClientAnalysisSession{};
    session.clientId = clientId;
    session.clientIP = ip;
    session.connectTime = std::chrono::steady_clock::now();

    m_stats.activeClients = m_clientSessions.size();

    Logger::Info("ProtocolDecoder: Client %u connected from %s — beginning protocol capture",
                 clientId, ip.c_str());
}

void ProtocolDecoder::HandleDisconnected(uint32_t clientId) {
    auto it = m_clientSessions.find(clientId);
    if (it != m_clientSessions.end()) {
        auto& session = it->second;
        Logger::Info("ProtocolDecoder: Client %u disconnected — captured %llu recv / %llu sent packets",
                     clientId,
                     (unsigned long long)session.totalPacketsReceived,
                     (unsigned long long)session.totalPacketsSent);

        if (m_config.exportJsonDefinitions) {
            ExportClientCaptureLocked(clientId, "");
        }
        m_clientSessions.erase(it);
    }
    m_stats.activeClients = m_clientSessions.size();
}

void ProtocolDecoder::HandlePacket(uint32_t clientId, const std::vector<uint8_t>& rawData,
                                   const std::string& tag, bool inbound) {
    m_stats.totalPacketsAnalyzed++;
    m_stats.totalBytesAnalyzed += rawData.size();

    auto it = m_clientSessions.find(clientId);
    if (it != m_clientSessions.end()) {
        auto& session = it->second;
        if (inbound) { session.totalPacketsReceived++; session.totalBytesReceived += rawData.size(); }
        else         { session.totalPacketsSent++;     session.totalBytesSent += rawData.size(); }
        session.tagCounts[tag]++;

        // Rolling window of the MOST RECENT packets (deque: pop oldest when full).
        if (m_config.logRawPackets) {
            session.recentPackets.push_back({
                std::chrono::steady_clock::now(), tag, rawData, inbound });
            while (session.recentPackets.size() > m_config.maxRawPacketsPerClient)
                session.recentPackets.pop_front();
        }

        if (inbound && !session.handshakeComplete && session.totalPacketsReceived <= 10) {
            AnalyzeHandshake(clientId, rawData);
        }
    }

    AnalyzePacketPayload(tag, rawData, inbound);
    MaybeAutoExport();
}

void ProtocolDecoder::HandleRawUDP(uint32_t /*clientId*/, const std::vector<uint8_t>& data) {
    AnalyzeUE3Bunch(data.data(), data.size());
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

    // Field-type detection: buffers early samples, then infers the layout from
    // the modal payload size (and refines) once enough evidence is in.
    DetectFieldTypes(structure, data);
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
            // Defensive (trust boundary): ParseBunch returns a pointer/length into
            // the attacker-supplied UDP buffer. Verify the reported payload range
            // lies fully within [data, data+len) before copying, so a bad
            // length/offset can never drive an out-of-bounds read here.
            if (payload >= data && payload <= data + len &&
                payloadLen <= static_cast<size_t>((data + len) - payload)) {
                ba.rawPayload.assign(payload, payload + payloadLen);
            } else {
                Logger::Warn("ProtocolDecoder: UE3 bunch payload out of bounds "
                             "(payloadLen=%zu, bufLen=%zu) — skipping payload copy",
                             payloadLen, len);
                ba.payloadLength = 0;
            }
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

        // Decode the bit-packed property stream against the known class tables.
        if (m_config.decodeBunchProperties && m_propDecoder &&
            !m_bunchHistory.empty() && m_bunchHistory.back().payloadLength > 0) {
            const auto& rp = m_bunchHistory.back().rawPayload;
            DecodeBunchProperties(header, rp.data(), rp.size());
        }
    }
}

// Verified RS2-7258 static class export indices -> class name (the names match
// the loaded netfields_u_<Class> tables). Source: docs/re/open_bunch_structure.md
// §1, bit-exact against the capture.
static std::string ResolveActorClassIndex(uint32_t idx) {
    switch (idx) {
        case 57520: return "ROPlayerController";
        case 86701: return "ROPlayerReplicationInfo";
        case 90245: return "ROTeamInfo";
        case 70887: return "ROGameReplicationInfo";
        default:    return std::string();
    }
}

// Decode the bit-packed property record stream inside an actor-channel bunch.
// For an OPEN bunch we parse the SerializeNewActor header to identify the actor's
// class EXACTLY (by its static export index) and to find where the property block
// begins; the channel is then bound to that class. Non-open bunches reuse the
// bound class. A best-fit scan remains only as a fallback for bunches seen before
// their open. This recovers real UE3 property NAMES + VALUES via the handle
// tables instead of guessing a byte layout.
void ProtocolDecoder::DecodeBunchProperties(const UE3BunchHeader& header,
                                            const uint8_t* payload, size_t payloadLen) {
    if (!payload || payloadLen == 0) return;

    auto& chan = m_channelStats[header.ChannelIndex];
    const size_t validBits = payloadLen * 8;

    auto applyDecode = [&](const BunchDecodeResult& res) {
        chan.bunchesDecoded++;
        for (const auto& p : res.properties) {
            if (!p.valueDecoded) continue;
            auto& agg = chan.properties[p.name];
            agg.count++;
            agg.handle = p.handle;
            agg.lastValue = p.valueSummary;
        }
    };

    // OPEN bunch: identify the class exactly and skip its SerializeNewActor header.
    size_t startBit = 0;
    if (header.Open) {
        auto hdr = m_propDecoder->ParseOpenHeader(payload, payloadLen, validBits,
                                                  &ResolveActorClassIndex);
        if (hdr.ok && !hdr.className.empty()) {
            chan.className = hdr.className;
            chan.bestFitScore = 1.0;        // exact identification, not a guess
            startBit = hdr.headerBits;
            Logger::Debug("ProtocolDecoder: ch%u open -> class %s (idx=%u), property "
                          "block at bit %zu", header.ChannelIndex,
                          hdr.className.c_str(), hdr.classIndex, hdr.headerBits);
        }
    }

    // Decode with the bound class (exact when identified via the open header).
    if (!chan.className.empty()) {
        if (const NetFieldTable* t = m_netFields.GetClass(chan.className)) {
            applyDecode(m_propDecoder->Decode(*t, payload, payloadLen, validBits, startBit));
            return;
        }
    }

    // Fallback: a non-open bunch on a channel whose open we never saw. Try the
    // value-typed classes and keep the best fit (no header to skip here).
    const NetFieldTable* bestTable = nullptr;
    BunchDecodeResult bestRes;
    double bestScore = 0.0;
    for (const NetFieldTable* t : m_netFields.AllClasses()) {
        if (!t->HasValueTypes()) continue;
        BunchDecodeResult res = m_propDecoder->Decode(*t, payload, payloadLen);
        double score = res.FitScore();
        if (score > bestScore && res.properties.size() >= 2) {
            bestScore = score;
            bestTable = t;
            bestRes = std::move(res);
        }
    }
    if (bestTable && bestScore >= 0.6) {
        chan.className = bestTable->ClassName();
        chan.bestFitScore = bestScore;
        applyDecode(bestRes);
    }
}

// Byte width implied by a classified field at `offset`. Returns 0 for a blob
// (caller treats as "consume the rest"). Shared by layout inference.
static size_t FieldSizeFor(DetectedFieldType& type, const std::vector<uint8_t>& data,
                           size_t offset, size_t remaining,
                           size_t& outMinLen, size_t& outMaxLen) {
    switch (type) {
        case DetectedFieldType::String: {
            if (remaining >= 4) {
                uint32_t strLen = ReadLE32(&data[offset]);
                if (strLen < 10000 && offset + 4 + strLen <= data.size()) {
                    outMinLen = outMaxLen = strLen;
                    return 4 + strLen;
                }
                type = DetectedFieldType::UInt32;
                return 4;
            }
            type = DetectedFieldType::Unknown;
            return remaining;
        }
        case DetectedFieldType::Vector3:  return 12;
        case DetectedFieldType::Float32:  return 4;
        case DetectedFieldType::UInt32:
        case DetectedFieldType::EntityId: return 4;
        case DetectedFieldType::UInt16:
        case DetectedFieldType::Enum16:   return 2;
        case DetectedFieldType::UInt8:
        case DetectedFieldType::Boolean:
        case DetectedFieldType::Enum8:    return 1;
        case DetectedFieldType::UInt64:
        case DetectedFieldType::Timestamp:return 8;
        default:
            type = DetectedFieldType::BlobData;
            return remaining;
    }
}

void ProtocolDecoder::DetectFieldTypes(DecodedPacketStructure& structure, const std::vector<uint8_t>& data) {
    if (data.empty()) return;

    if (!structure.layoutFinalized) {
        // Buffer the earliest samples; defer layout until we can pick the MODAL
        // payload size, so an atypical first packet can't lock a bad layout.
        if (structure.sampleBuffer.size() < m_config.maxSampleBuffer)
            structure.sampleBuffer.push_back(data);

        if (structure.totalSamples >= m_config.minSamplesForConfidence ||
            structure.sampleBuffer.size() >= m_config.maxSampleBuffer) {
            InferLayoutFromSamples(structure);
        }
        return;
    }

    // Layout finalized: fold this sample's values into the per-field statistics
    // (only when it matches the inferred layout width).
    if (m_config.trackFieldStatistics) {
        size_t offset = 0;
        for (auto& field : structure.fields) {
            if (offset + field.size <= data.size()) {
                field.sampleCount++;
                if (field.type != DetectedFieldType::BlobData)
                    UpdateFieldStatistics(field, data, offset);
                offset += field.size;
            } else {
                break;
            }
        }
    }
}

void ProtocolDecoder::InferLayoutFromSamples(DecodedPacketStructure& structure) {
    if (structure.sampleBuffer.empty()) return;

    // Choose the MODAL payload size and a representative sample of it.
    std::map<size_t, int> sizeCounts;
    for (const auto& s : structure.sampleBuffer) sizeCounts[s.size()]++;
    size_t modalSize = 0; int bestCount = -1;
    for (const auto& [sz, c] : sizeCounts) if (c > bestCount) { bestCount = c; modalSize = sz; }

    const std::vector<uint8_t>* rep = nullptr;
    for (const auto& s : structure.sampleBuffer)
        if (s.size() == modalSize) { rep = &s; break; }
    if (!rep) return;

    // Build the field layout from the representative sample.
    structure.fields.clear();
    size_t offset = 0, fieldIndex = 0;
    while (offset < rep->size()) {
        size_t remaining = rep->size() - offset;
        DetectedField field;
        field.offset = offset;
        field.type = ClassifyFieldAt(*rep, offset, remaining);
        field.size = FieldSizeFor(field.type, *rep, offset, remaining,
                                  field.minLength, field.maxLength);
        field.suggestedName = GenerateFieldName(field.type, fieldIndex);
        field.confidence = 0.3f;
        if (field.size > remaining) field.size = remaining;
        if (field.size == 0) {
            Logger::Warn("ProtocolDecoder: zero-width field at offset %zu in '%s' — "
                         "stopping field scan", offset, structure.packetTag.c_str());
            break;
        }
        structure.fields.push_back(std::move(field));
        offset += structure.fields.back().size;
        fieldIndex++;
    }

    // Replay every buffered modal-size sample through the statistics so the
    // inferred layout starts with real distributions, not just one packet.
    for (const auto& s : structure.sampleBuffer) {
        if (s.size() != modalSize) continue;
        size_t off = 0;
        for (auto& field : structure.fields) {
            if (off + field.size <= s.size()) {
                field.sampleCount++;
                if (m_config.trackFieldStatistics && field.type != DetectedFieldType::BlobData)
                    UpdateFieldStatistics(field, s, off);
                off += field.size;
            } else {
                break;
            }
        }
    }

    structure.layoutFinalized = true;
    structure.sampleBuffer.clear();
    structure.sampleBuffer.shrink_to_fit();
    RefineFieldDetection(structure);

    Logger::Info("ProtocolDecoder: Structure for '%s' layout inferred from modal size %zu "
                 "(%zu fields, %zu samples buffered)",
                 structure.packetTag.c_str(), modalSize, structure.fields.size(),
                 (size_t)structure.totalSamples);
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
            uint16_t v = ReadLE16(&data[offset]);
            value = v;
            field.observedValues[v]++;
            break;
        }

        case DetectedFieldType::UInt32:
        case DetectedFieldType::EntityId: {
            uint32_t v = ReadLE32(&data[offset]);
            value = v;
            if (field.observedValues.size() < 1000) { // cap map size
                field.observedValues[v]++;
            }
            break;
        }

        case DetectedFieldType::Float32: {
            value = ReadLEFloat(&data[offset]);
            break;
        }

        case DetectedFieldType::UInt64:
        case DetectedFieldType::Timestamp: {
            uint64_t v = ReadLE64(&data[offset]);
            value = static_cast<double>(v);
            break;
        }

        case DetectedFieldType::String: {
            if (offset + 4 <= data.size()) {
                uint32_t strLen = ReadLE32(&data[offset]);
                field.minLength = std::min(field.minLength, static_cast<size_t>(strLen));
                field.maxLength = std::max(field.maxLength, static_cast<size_t>(strLen));
            }
            return; // no numeric stats for strings
        }

        default:
            return;
    }

    // Running mean + variance (Welford). m2 is the running sum of squared deltas;
    // variance is the population variance m2/n (NOT m2 itself — the previous code
    // stored m2 in the 'variance' field, mislabelling it).
    double n = static_cast<double>(field.sampleCount);
    if (field.sampleCount == 1) {
        field.minValue = value;
        field.maxValue = value;
        field.avgValue = value;
        field.m2 = 0.0;
        field.variance = 0.0;
    } else {
        field.minValue = std::min(field.minValue, value);
        field.maxValue = std::max(field.maxValue, value);
        double oldAvg = field.avgValue;
        field.avgValue += (value - oldAvg) / n;
        field.m2 += (value - oldAvg) * (value - field.avgValue);
        field.variance = field.m2 / n;
    }
}

// ---- Heuristic Field Detection ----

// A float we'd plausibly see as game data: finite, and either exactly zero or in
// a sane magnitude band. Crucially this REJECTS denormals/near-zero noise, which
// is what small little-endian integers look like when misread as float — so an
// int blob no longer masquerades as a float/Vector3 and desyncs the layout.
static bool PlausibleFloat(float v) {
    if (std::isnan(v) || std::isinf(v)) return false;
    if (v == 0.0f) return true;
    float a = std::fabs(v);
    return a >= 1e-3f && a <= 1e9f;
}

DetectedFieldType ProtocolDecoder::ClassifyFieldAt(const std::vector<uint8_t>& data, size_t offset, size_t remaining) {
    // Most structurally specific patterns first. Ordering matters: a wrong guess
    // consumes the wrong number of bytes and desyncs every following field.

    // String: 4-byte length prefix pointing at in-buffer printable bytes.
    if (remaining >= 4 && LooksLikeString(data, offset)) {
        return DetectedFieldType::String;
    }

    // Vector3 (12 bytes: 3x float32) — stricter than before (see LooksLikeVector3).
    if (remaining >= 12 && LooksLikeVector3(data, offset)) {
        return DetectedFieldType::Vector3;
    }

    // Float32 (4 bytes, plausible non-zero game float).
    if (remaining >= 4 && LooksLikeFloat(data, offset)) {
        return DetectedFieldType::Float32;
    }

    // EntityId / UInt32 (4 bytes, reasonable ID range).
    if (remaining >= 4 && LooksLikeEntityId(data, offset)) {
        return DetectedFieldType::EntityId;
    }

    // Timestamp is the WEAKEST signal for this (UE3) protocol, which uses no
    // epoch-ms field; keep it last so it never steals a float/id/uint match.
    if (remaining >= 8 && LooksLikeTimestamp(data, offset)) {
        return DetectedFieldType::Timestamp;
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

    uint32_t strLen = ReadLE32(&data[offset]);

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

    float v = ReadLEFloat(&data[offset]);

    // Plausible game float, excluding exact zero (ambiguous with a uint32 0) and
    // denormal noise (a small LE integer misread as float). Range covers game
    // positions/rotations/velocities.
    if (v == 0.0f) return false;
    if (!PlausibleFloat(v)) return false;
    return (v > -100000.0f && v < 100000.0f);
}

bool ProtocolDecoder::LooksLikeVector3(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 12 > data.size()) return false;

    float x = ReadLEFloat(&data[offset]);
    float y = ReadLEFloat(&data[offset + 4]);
    float z = ReadLEFloat(&data[offset + 8]);

    // Every component must be a PLAUSIBLE float (rejects denormal noise — i.e. a
    // run of small integers that would otherwise pass the old range-only test and
    // get mis-claimed as a 12-byte vector, desyncing the rest of the packet).
    if (!PlausibleFloat(x) || !PlausibleFloat(y) || !PlausibleFloat(z)) return false;

    bool xValid = (x > -100000.0f && x < 100000.0f);
    bool yValid = (y > -100000.0f && y < 100000.0f);
    bool zValid = (z > -100000.0f && z < 100000.0f);
    if (!(xValid && yValid && zValid)) return false;

    // Need real spatial scale: at least two non-zero components AND at least one
    // with magnitude >= 1 (a true coordinate), not three sub-unit values.
    int nonZero = (x != 0.0f) + (y != 0.0f) + (z != 0.0f);
    bool hasScale = std::fabs(x) >= 1.0f || std::fabs(y) >= 1.0f || std::fabs(z) >= 1.0f;
    return nonZero >= 2 && hasScale;
}

bool ProtocolDecoder::LooksLikeTimestamp(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 8 > data.size()) return false;

    uint64_t v = ReadLE64(&data[offset]);

    // Epoch-millis window (≈2020-2033). Such values occupy ~41 bits, so the top
    // two bytes are zero; requiring that rejects most random 8-byte windows. This
    // is a deliberately weak/rare signal — UE3/RS2V does not field epoch time.
    if ((v >> 48) != 0) return false;
    return (v > 1577836800000ULL && v < 2000000000000ULL);
}

bool ProtocolDecoder::LooksLikeEntityId(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) return false;

    uint32_t v = ReadLE32(&data[offset]);

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
    return ExportProtocolDefinitionsLocked(outputPath);
}

bool ProtocolDecoder::ExportProtocolDefinitionsLocked(const std::string& outputPath) const {
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
    return ExportClientCaptureLocked(clientId, outputPath);
}

bool ProtocolDecoder::ExportClientCaptureLocked(uint32_t clientId, const std::string& outputPath) const {
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
    file << "  \"clientIP\": \"" << JsonEscape(session.clientIP) << "\",\n";
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
        file << "    \"" << JsonEscape(tag) << "\": " << count;
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
        file << "    {\"tag\": \"" << JsonEscape(rp.tag) << "\", \"size\": " << rp.data.size()
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

    // Decoded bit-packed bunch properties (the real RE payoff: named UE3
    // properties recovered from actor-channel bunches via the handle tables).
    if (!m_channelStats.empty()) {
        report << "\n--- Decoded Channel Properties (bit-packed) ---\n\n";
        for (const auto& [ch, cs] : m_channelStats) {
            report << "Channel " << (unsigned)ch << " -> class "
                   << (cs.className.empty() ? "(unidentified)" : cs.className)
                   << " (fit=" << std::setprecision(2) << cs.bestFitScore
                   << ", bunches=" << cs.bunchesDecoded << ")\n";
            for (const auto& [name, agg] : cs.properties) {
                report << "    [" << agg.handle << "] " << name
                       << " x" << agg.count << " last=" << agg.lastValue << "\n";
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

std::string ProtocolDecoder::JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::map<uint8_t, ChannelPropertyStats> ProtocolDecoder::GetChannelPropertyStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_channelStats;
}

// Cumulative cross-run persistence. A compact, line-based sidecar (NOT JSON) so
// it can be parsed back robustly without a hand-rolled JSON reader. Only the
// stable high-level per-tag signal is merged (sample counts, sizes, directions);
// per-run field layouts are re-inferred each run and intentionally not persisted.
void ProtocolDecoder::LoadPersistentState() {
    const std::string path = m_config.outputDirectory + "/protocol_state.tsv";
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    size_t loaded = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string tag;
        uint64_t samples = 0, c2s = 0, s2c = 0;
        size_t minSz = 0, maxSz = 0;
        double avgSz = 0.0;
        if (!(is >> tag >> samples >> minSz >> maxSz >> avgSz >> c2s >> s2c)) continue;

        auto& s = m_decodedStructures[tag];
        s.packetTag = tag;
        s.totalSamples += samples;
        s.minPayloadSize = std::min(s.minPayloadSize, minSz);
        s.maxPayloadSize = std::max(s.maxPayloadSize, maxSz);
        s.avgPayloadSize = avgSz;   // seed; refined as new samples arrive
        s.clientToServerCount += c2s;
        s.serverToClientCount += s2c;
        ++loaded;
    }
    if (loaded)
        Logger::Info("ProtocolDecoder: merged %zu prior packet-type records from '%s'",
                     loaded, path.c_str());
}

void ProtocolDecoder::SavePersistentState() const {
    const std::string path = m_config.outputDirectory + "/protocol_state.tsv";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "# tag\tsamples\tminSize\tmaxSize\tavgSize\tc2s\ts2c\n";
    for (const auto& [tag, s] : m_decodedStructures) {
        size_t minSz = (s.minPayloadSize == SIZE_MAX) ? 0 : s.minPayloadSize;
        f << tag << '\t' << s.totalSamples << '\t' << minSz << '\t'
          << s.maxPayloadSize << '\t' << s.avgPayloadSize << '\t'
          << s.clientToServerCount << '\t' << s.serverToClientCount << '\n';
    }
}

std::string ProtocolDecoder::StructureToJson(const DecodedPacketStructure& structure) const {
    std::ostringstream json;
    json << "\"" << JsonEscape(structure.packetTag) << "\": {\n";
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
        json << "          \"name\": \"" << JsonEscape(f.suggestedName) << "\",\n";
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
        // Already under m_mutex (called from the worker) — use the locked paths.
        ExportProtocolDefinitionsLocked("");
        if (m_config.persistState) SavePersistentState();
    }
}
