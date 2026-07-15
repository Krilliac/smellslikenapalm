// src/Network/ConnectionManager.cpp

#include "Network/ConnectionManager.h"
#include "Game/GameServer.h"
#include "Game/RoleSystem.h"
#include "Game/TeamManager.h"
#include "Game/TeamMapping.h"
#include "Game/PlayerManager.h"
#include "Game/Player.h"
#include "Game/MapManager.h"
#include "Game/ObjectiveSystem.h"
#include "Game/SpawnSystem.h"
#include "Game/TicketSystem.h"
#include "Game/TerritoryMode.h"
#include "Game/SupremacyMode.h"
#include "Game/SkirmishMode.h"
#include "Game/GameState.h"
#include "Game/BotManager.h"
#include "Config/ConfigManager.h"
#include "Config/GameConfig.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Network/Packet.h"
#include "Network/BandwidthManager.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include "Network/HandshakeState.h"
#include "Network/ControlChannel.h"
#include "Network/PeerControlClose.h"
#include "Network/BitWriter.h"
#include "Network/ActorReplication.h"
#include "Network/MantleReplication.h"
#include "Network/MovementReplication.h"
#include "Network/GameplayRpcReplication.h"
#include "Network/WeaponCombatReplication.h"
#include "Network/ObjectiveReplication.h"
#include "Network/DeploymentReplication.h"
#include "Network/SpawnReplication.h"
#include "Network/RoleSelectionReplication.h"
#include "Network/RetailBootstrap.h"
#include "Physics/MovementSampleTiming.h"
#include <cstdlib>
#include "Network/BitReader.h"
#include "Network/PacketRecorder.h"
#include "../../telemetry/TelemetryManager.h"
#include <chrono>
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <algorithm>
#include <span>

namespace {

bool IsFreshControlHandshakeStart(const std::vector<uint8_t>& datagram) {
    if (datagram.empty()) return false;

    const PacketCodec::Packet pkt = PacketCodec::Decode(
        datagram.data(), datagram.size(), PacketCodec::kClientSendMaxPacketBytes);
    if (!pkt.ok || pkt.packetId != 0 || !pkt.acks.empty() || pkt.bunches.size() != 1) {
        return false;
    }

    const PacketCodec::Bunch& b = pkt.bunches.front();
    return b.bControl && b.bOpen && !b.bClose && b.bReliable &&
           b.chIndex == 0 && b.chType == PacketCodec::kControlChannelType &&
           b.chSequence == 1 && b.payloadBits == 16 && b.payload.size() >= 2 &&
           b.payload[0] == ControlChannel::Handshake::kStart && b.payload[1] == 0x01;
}

RetailBootstrap::Profile ResolveRetailBootstrapProfile(GameServer* server) {
    std::string mapUrl = "VNTE-Resort";
    std::string effectiveMode = "Territories";
    if (server) {
        if (MapManager* maps = server->GetMapManager()) {
            if (!maps->GetCurrentMapName().empty()) mapUrl = maps->GetCurrentMapName();
            if (!maps->GetCurrentMap().defaultMode.empty()) {
                effectiveMode = maps->GetCurrentMap().defaultMode;
            }
        }

        // These mutually-exclusive pointers are created by GameServer's
        // authoritative active-mode resolver. Prefer them over the map default
        // so an explicit Game.game_mode override reaches every retail field.
        if (server->GetTerritoryMode()) effectiveMode = "Territories";
        else if (server->GetSupremacyMode()) effectiveMode = "Supremacy";
        else if (server->GetSkirmishMode()) effectiveMode = "Skirmish";
    }
    return RetailBootstrap::ResolveProfile(mapUrl, effectiveMode);
}

enum class OwnedWeaponIdentity : uint8_t {
    Unknown,
    FactionPrimary,
    FactionGrenade,
};

struct OwnedWeaponChannelMetadata {
    uint16_t channel = 0;
    uint32_t classRef = 0;
    uint32_t maxHandle = GameplayRpc::kRoWeaponMaxHandle;
    OwnedWeaponIdentity identity = OwnedWeaponIdentity::Unknown;
    uint8_t attachmentSlot = 0;
    uint32_t attachmentClassRef = 0;
};

// Fixed emulator channels preserve one stable inbound-RPC surface while the
// class/loadout differs by faction. The South graph is exact retail f27394;
// the North graph is exact f63525 normalized from capture channels
// pawn94/weapons95..97/manager104 onto pawn209/weapons210,212,214/manager219.
constexpr std::array<OwnedWeaponChannelMetadata, 5> kSouthOwnedWeaponChannels{{
    {210, 286374, GameplayRpc::kRoWeaponMaxHandle,
                  OwnedWeaponIdentity::FactionPrimary, 0, 286936},
    {211, 286391, GameplayRpc::kRoWeaponMaxHandle,
                  OwnedWeaponIdentity::Unknown, 1, 286946},
    {212, 286464, GameplayRpc::kM61WeaponMaxHandle,
                  OwnedWeaponIdentity::FactionGrenade, 2, 287063},
    {213, 286109, GameplayRpc::kRoWeaponMaxHandle,
                  OwnedWeaponIdentity::Unknown, 3, 286944},
    // M18 inherits the 101-entry grenade field table just like the M61.
    {214, 286389, GameplayRpc::kM61WeaponMaxHandle,
                  OwnedWeaponIdentity::Unknown, 5, 286126},
}};

constexpr std::array<OwnedWeaponChannelMetadata, 3> kNorthOwnedWeaponChannels{{
    {210, 286271, GameplayRpc::kRoWeaponMaxHandle,
                  OwnedWeaponIdentity::FactionPrimary, 0, 286845},
    {212, 286804, GameplayRpc::kM61WeaponMaxHandle,
                  OwnedWeaponIdentity::FactionGrenade, 2, 287188},
    {214, 286758, GameplayRpc::kM61WeaponMaxHandle,
                  OwnedWeaponIdentity::Unknown, 4, 287147},
}};

struct OwnedWeaponChannelSet {
    const OwnedWeaponChannelMetadata* entries = nullptr;
    size_t count = 0;
    const OwnedWeaponChannelMetadata* begin() const { return entries; }
    const OwnedWeaponChannelMetadata* end() const { return entries + count; }
};

OwnedWeaponChannelSet OwnedWeaponChannels(bool northGraph) {
    return northGraph
        ? OwnedWeaponChannelSet{kNorthOwnedWeaponChannels.data(),
                                kNorthOwnedWeaponChannels.size()}
        : OwnedWeaponChannelSet{kSouthOwnedWeaponChannels.data(),
                                kSouthOwnedWeaponChannels.size()};
}

const OwnedWeaponChannelMetadata* FindOwnedWeaponChannel(
    uint32_t channel, bool northGraph) {
    const OwnedWeaponChannelSet channels = OwnedWeaponChannels(northGraph);
    const auto found = std::find_if(
        channels.begin(), channels.end(),
        [channel](const OwnedWeaponChannelMetadata& metadata) {
            return metadata.channel == channel;
        });
    return found == channels.end() ? nullptr : &*found;
}

std::optional<CombatRole> ResolveCompoundCombatRole(Faction faction,
                                                    uint8_t classIndex) {
    if (faction == Faction::USMC) {
        switch (classIndex) {
            case RoleSelectionRepl::kCompoundRiflemanClassIndex:
                return CombatRole::Rifleman;
            case RoleSelectionRepl::kCompoundPointmanClassIndex:
                return CombatRole::Pointman;
            case RoleSelectionRepl::kCompoundMachineGunnerClassIndex:
                return CombatRole::MachineGunner;
            case RoleSelectionRepl::kCompoundMarksmanClassIndex:
                return CombatRole::Marksman;
            case RoleSelectionRepl::kCompoundEngineerClassIndex:
                return CombatRole::CombatEngineer;
            default:
                return std::nullopt;
        }
    }

    if (faction == Faction::NLFSV) {
        switch (classIndex) {
            case RoleSelectionRepl::kCompoundRiflemanClassIndex:
                return CombatRole::Rifleman;
            // Compound's NLF class 1 is RORoleInfoNorthernScout_SK. Both that
            // cooked role and the existing Pointman abstraction use the retail
            // RORIT_Scout role type; keep the protocol class index authoritative.
            case RoleSelectionRepl::kCompoundPointmanClassIndex:
                return CombatRole::Pointman;
            case RoleSelectionRepl::kCompoundMachineGunnerClassIndex:
                return CombatRole::MachineGunner;
            case RoleSelectionRepl::kCompoundMarksmanClassIndex:
                return CombatRole::Sniper;
            case RoleSelectionRepl::kCompoundEngineerClassIndex:
                return CombatRole::Sapper;
            default:
                return std::nullopt;
        }
    }

    return std::nullopt;
}

std::vector<const SpawnLocation*> BuildAdvertisedSpawnRepresentatives(
    const std::vector<const SpawnLocation*>& available) {
    std::vector<const SpawnLocation*> advertised;
    advertised.reserve(std::min<std::size_t>(
        available.size(), DeploymentCoordinator::kNormalSpawnSlotCount));
    const bool hasMappedVolumes = std::any_of(
        available.begin(), available.end(), [](const SpawnLocation* spawn) {
            return spawn && spawn->retailSpawnVolumeRef != 0;
        });
    std::vector<uint32_t> seenVolumeRefs;
    seenVolumeRefs.reserve(advertised.capacity());

    for (const SpawnLocation* spawn : available) {
        if (!spawn) continue;
        const uint32_t volumeRef = spawn->retailSpawnVolumeRef;
        if (hasMappedVolumes && volumeRef == 0) continue;
        if (volumeRef != 0 &&
            std::find(seenVolumeRefs.begin(), seenVolumeRefs.end(), volumeRef) !=
                seenVolumeRefs.end()) {
            continue;
        }
        advertised.push_back(spawn);
        if (volumeRef != 0) seenVolumeRefs.push_back(volumeRef);
        if (advertised.size() >=
            DeploymentCoordinator::kNormalSpawnSlotCount) {
            break;
        }
    }
    return advertised;
}

} // namespace

static uint64_t NowMs();

ConnectionManager::ConnectionManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[ConnectionManager::ConnectionManager] Entry: server=%p", (void*)server);
    Logger::Debug("[ConnectionManager::ConnectionManager] ConnectionManager created with GameServer=%p", (void*)server);
    Logger::Trace("[ConnectionManager::ConnectionManager] Exit");
}

ConnectionManager::~ConnectionManager() {
    Logger::Trace("[ConnectionManager::~ConnectionManager] Entry: destructor called");
    Logger::Debug("[ConnectionManager::~ConnectionManager] Destroying ConnectionManager, %zu clients connected", m_clients.size());
    Shutdown();
    Logger::Trace("[ConnectionManager::~ConnectionManager] Exit");
}

bool ConnectionManager::Initialize(uint16_t listenPort) {
    Logger::Trace("[ConnectionManager::Initialize] Entry: listenPort=%u", listenPort);
    Logger::Info("ConnectionManager: Initializing on port %u", listenPort);
    m_socket = std::make_shared<UDPSocket>();
    Logger::Debug("[ConnectionManager::Initialize] Created UDPSocket, attempting bind on port %u", listenPort);
    if (!m_socket->Bind(listenPort)) {
        Logger::Error("ConnectionManager: Failed to bind UDP socket on port %u", listenPort);
        Logger::Trace("[ConnectionManager::Initialize] Exit: returning false (bind failed)");
        return false;
    }
    Logger::Debug("[ConnectionManager::Initialize] UDP socket bound successfully on port %u", listenPort);

    auto cfgMgr = m_server ? m_server->GetConfigManager() : nullptr;
    Logger::Debug("[ConnectionManager::Initialize] ConfigManager=%p", (void*)cfgMgr.get());
    const auto gameConfig = m_server ? m_server->GetGameConfig() : nullptr;
    m_waitForReadyPlayer = gameConfig
        ? gameConfig->WaitForReadyPlayer()
        : true;
    Logger::Info(
        "ConnectionManager: preparation waits for a ready retail player: %s",
        m_waitForReadyPlayer ? "enabled" : "disabled");
    m_bandwidthLimit = cfgMgr ? (uint32_t)cfgMgr->GetInt("Network.bandwidth_limit", 65536) : 65536;
    Logger::Debug("[ConnectionManager::Initialize] Bandwidth limit set to %u bytes/sec", m_bandwidthLimit);
    m_bwManager = std::make_unique<BandwidthManager>(m_bandwidthLimit);
    Logger::Debug("[ConnectionManager::Initialize] BandwidthManager created with limit=%u", m_bandwidthLimit);

    // AntiCheat.max_speed is expressed in displayed metres/second while retail
    // movement locations are Unreal units (50 UU/m). Config validation already
    // bounds the optional setting to [1,20]; use the safe upper bound when it is
    // absent or malformed. Acceleration/turn thresholds remain effectively
    // unbounded until retail traces calibrate them, avoiding speculative kicks.
    constexpr float kUuPerMeter = 50.0f;
    float maximumSpeedMetersPerSecond =
        cfgMgr ? cfgMgr->GetFloat("AntiCheat.max_speed", 20.0f) : 20.0f;
    if (!std::isfinite(maximumSpeedMetersPerSecond) ||
        maximumSpeedMetersPerSecond < 1.0f ||
        maximumSpeedMetersPerSecond > 20.0f) {
        maximumSpeedMetersPerSecond = 20.0f;
    }
    MovementValidator::Config movementConfig;
    movementConfig.maxSpeed = maximumSpeedMetersPerSecond * kUuPerMeter;
    movementConfig.maxAccel = std::numeric_limits<float>::max();
    movementConfig.maxTurnRateDeg = std::numeric_limits<float>::max();
    movementConfig.maxTeleportDistance = 10000.0f; // 200m packet displacement cap
    movementConfig.maxUpdateInterval = std::chrono::seconds(120);
    movementConfig.maxClients = ActorRepl::kDynamicChannelMax - 1u;
    movementConfig.duplicateEpsilon = 0.001f;
    m_movementValidator =
        std::make_unique<MovementValidator>(movementConfig);
    if (!m_movementValidator->IsConfigured()) {
        Logger::Error(
            "[ConnectionManager::Initialize] movement authority configuration "
            "is invalid; client position updates will fail closed");
    } else {
        Logger::Info(
            "[ConnectionManager::Initialize] movement authority ready "
            "(maxSpeed=%.1f UU/s, teleportCap=%.1f UU)",
            movementConfig.maxSpeed, movementConfig.maxTeleportDistance);
    }

    Logger::Info("ConnectionManager: Initialized successfully");
    Logger::Trace("[ConnectionManager::Initialize] Exit: returning true");
    return true;
}

void ConnectionManager::Shutdown() {
    Logger::Trace("[ConnectionManager::Shutdown] Entry");
    Logger::Info("ConnectionManager: Shutting down");
    if (m_socket) {
        Logger::Debug("[ConnectionManager::Shutdown] Closing socket and resetting");
        m_socket->Close();
        m_socket.reset();
    } else {
        Logger::Debug("[ConnectionManager::Shutdown] Socket already null");
    }
    Logger::Debug("[ConnectionManager::Shutdown] Clearing %zu client connections", m_clients.size());
    m_clients.clear();
    // Shutdown may run without the ordinary stale/disconnect retirement path.
    // Drop every receive cursor and buffered actor bunch with its connection so a
    // later Initialize cannot inherit stale reliable-channel state.
    m_handshakes.clear();
    m_controlState.clear();
    UpdateTelemetryPlayerCounts();
    if (m_movementValidator) {
        m_movementValidator->Clear();
        m_movementValidator.reset();
    }
    Logger::Debug("[ConnectionManager::Shutdown] Resetting BandwidthManager");
    m_bwManager.reset();
    Logger::Info("[ConnectionManager::Shutdown] Shutdown complete");
    Logger::Trace("[ConnectionManager::Shutdown] Exit");
}

void ConnectionManager::SetPacketCallback(PacketCallback cb) {
    Logger::Trace("[ConnectionManager::SetPacketCallback] Entry: cb=%s", cb ? "non-null" : "null");
    m_packetCallback = std::move(cb);
    Logger::Debug("[ConnectionManager::SetPacketCallback] Packet callback set");
    Logger::Trace("[ConnectionManager::SetPacketCallback] Exit");
}

void ConnectionManager::PumpNetwork() {
    Logger::Trace("[ConnectionManager::PumpNetwork] Entry");
    if (!m_socket || !m_socket->IsOpen()) {
        Logger::Debug("[ConnectionManager::PumpNetwork] Socket not available or not open, returning early");
        Logger::Trace("[ConnectionManager::PumpNetwork] Exit: socket not ready");
        return;
    }

    // Drain all available packets from the socket
    Logger::Debug("[ConnectionManager::PumpNetwork] Draining packets (max 256 per pump)");
    int packetCount = 0;
    for (int i = 0; i < 256; ++i) {
        std::vector<uint8_t> buffer(1500);
        std::string srcIp;
        uint16_t srcPort = 0;
        int len = m_socket->ReceiveFrom(srcIp, srcPort, buffer.data(), (int)buffer.size());
        if (len <= 0) {
            Logger::Trace("[ConnectionManager::PumpNetwork] ReceiveFrom returned %d at iteration %d, stopping drain", len, i);
            break;
        }

        // Defensive: recvfrom must never report more than the buffer it was handed,
        // but clamp before resize() so a misbehaving socket layer can't make us grow
        // the buffer past the bytes actually written (which would expose uninitialized
        // memory to the parse path). Valid datagrams are always <= buffer.size().
        if (len > static_cast<int>(buffer.size())) {
            Logger::Warn("[ConnectionManager::PumpNetwork] ReceiveFrom reported %d bytes > buffer %zu from %s:%u, clamping",
                         len, buffer.size(), srcIp.c_str(), srcPort);
            len = static_cast<int>(buffer.size());
        }

        buffer.resize(len);
        ClientAddress addr{srcIp, srcPort};
        Logger::Debug("[ConnectionManager::PumpNetwork] Received %d bytes from %s:%u (iteration %d)",
                      len, srcIp.c_str(), srcPort, i);
        // Bandwidth check
        if (m_bwManager && !m_bwManager->CanReceivePacket(addr, (uint32_t)len)) {
            Logger::Warn("[ConnectionManager::PumpNetwork] Bandwidth limit exceeded for %s:%u, dropping %d byte packet",
                         srcIp.c_str(), srcPort, len);
            TELEMETRY_INCREMENT_PACKETS_DROPPED();
            continue;
        }

        Logger::Debug("[ConnectionManager::PumpNetwork] Bandwidth check passed, handling packet from %s:%u",
                      srcIp.c_str(), srcPort);
        HandleIncomingPacket(buffer, addr);
        packetCount++;
    }
    Logger::Debug("[ConnectionManager::PumpNetwork] Drained %d packets this pump cycle", packetCount);

    // Flush queued acks coalesced (one throttled ack-only datagram per client), instead of
    // one per received packet - the per-packet version was an ack-storm that dropped reliables.
    // This MUST run before RetransmitTick: PacketAssembler piggybacks every pending ACK on
    // the next data packet. During the client's long map load a full pump can queue 256 ACKs;
    // attaching those (~482 bytes) to a ~900-byte actor retransmit exceeds the retail
    // client's receive buffer and produces WSAEMSGSIZE (10040). Ack-only first keeps both
    // datagrams safely below that limit.
    FlushPendingAcks();

    // Resend any reliable bunches the client hasn't acked within the timeout. Runs every
    // pump cycle (the poll loop is tight) so a dropped bootstrap open is recovered in
    // ~250ms instead of stalling the channel forever (the soft-lock).
    RetransmitTick();

    // Remote pawn opens/death/close transitions and ordinary unreliable
    // movement snapshots share the authoritative human/bot positions used by
    // hit geometry. The per-viewer scheduler is bounded to at most 10 Hz.
    ReplicateRemoteParticipantPawnsTick();

    // UE3 FlushNet emits a PacketId+terminator-only datagram when the connection
    // has otherwise been idle for KeepAliveTime. The official capture sends the
    // same two-byte shape at ~1 Hz; it keeps the retail 60-second receive timeout
    // from firing without consuming reliable ch0 sequence or touching auth state.
    TransportKeepAliveTick();

    Logger::Trace("[ConnectionManager::PumpNetwork] Exit");
}

void ConnectionManager::HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr) {
    Logger::Trace("[ConnectionManager::HandleIncomingPacket] Entry: addr=%s:%u, data size=%zu",
                  addr.ip.c_str(), addr.port, data.size());

    // Steam/UE3 often reuses the same UDP socket for an immediate reconnect.  If
    // its old session is still Joined, feeding the new ch0 seq=1
    // open to the old reassembler silently deduplicates it and the client stays
    // trapped until the whole server restarts.  Retire that established session
    // before resolving/creating the client for this datagram.
    ResetEstablishedSessionForFreshHandshake(data, addr);

    // Allocation is an admission decision, not a side effect of receiving any
    // UDP datagram.  Retail can send a final pure-ACK roughly one timeout after
    // it has abandoned a connection (observed after package bootstrap); if the old
    // session was already retired, accepting that ACK here creates a phantom
    // client with no handshake.  The same is true for late keepalives, actor
    // bunches, and retransmitted closes.  Only the exact capture-grounded fresh
    // ch0 HandshakeStart may allocate an unknown/closed endpoint.  An active
    // endpoint remains admitted and continues through the normal packet path.
    const auto existing = m_clients.find(addr);
    const bool endpointKnown = existing != m_clients.end();
    const bool endpointAdmitted =
        endpointKnown && existing->second &&
        !existing->second->IsDisconnected();
    if (!endpointAdmitted) {
        const bool freshHandshakeStart = IsFreshControlHandshakeStart(data);
        const bool nullEntry =
            existing != m_clients.end() && !existing->second;

        // Quarantine an impossible null address entry before a valid start tries
        // CreateOrGetClient(): that routine quite reasonably dereferences every
        // existing entry.  Invalid traffic must not leave the poisoned entry
        // consuming capacity either.
        if (nullEntry) {
            RemoveClientSession(addr, "quarantined before endpoint admission");
        }

        if (!freshHandshakeStart) {
            const PacketCodec::Packet candidate = PacketCodec::Decode(
                data.data(), data.size(),
                PacketCodec::kClientSendMaxPacketBytes);
            const PacketCodec::PeerCloseClassification close =
                PacketCodec::ClassifyPeerClose(candidate);
            Logger::Debug(
                "[ConnectionManager::HandleIncomingPacket] ignored pre-admission "
                "datagram from %s endpoint %s:%u (bytes=%zu, close=%u)",
                endpointKnown ? "closed" : "unknown",
                addr.ip.c_str(), addr.port, data.size(),
                static_cast<unsigned>(close));
            TELEMETRY_INCREMENT_PACKETS_DROPPED();
            return;
        }
    }

    uint32_t clientId = CreateOrGetClient(addr.ip, addr.port);
    if (clientId == UINT32_MAX) {
        Logger::Warn("[ConnectionManager::HandleIncomingPacket] CreateOrGetClient returned UINT32_MAX for %s:%u, dropping packet",
                     addr.ip.c_str(), addr.port);
        Logger::Trace("[ConnectionManager::HandleIncomingPacket] Exit: client creation failed");
        return;
    }
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] clientId=%u for %s:%u", clientId, addr.ip.c_str(), addr.port);

    // Count accepted wire datagrams at the shared UE3/legacy admission point.
    // NetworkManager::OnPacketReceived sees only decoded legacy packets, so
    // incrementing there left every accepted retail UE3 packet invisible and
    // made the packet-loss denominator report zero processed traffic. Rejected
    // pre-admission and bandwidth-limited datagrams take the dropped paths above.
    TELEMETRY_INCREMENT_PACKETS_PROCESSED();

    // GLOBAL PACKET RECORDER: every inbound datagram (C2S) -> packetlog/ sniff.
    net::PacketRecorder::Instance().RecordDatagram(
        net::PktDir::C2S, clientId, addr.ip + ":" + std::to_string(addr.port),
        data.data(), data.size());

    // HANDSHAKE WIRE TRACE: dump small inbound datagrams (the handshake/control
    // packets are tiny) so we can see exactly what the real client sends. Skipped
    // for large datagrams to avoid spamming gameplay traffic.
    if (data.size() <= 80) {
        std::string hex; hex.reserve(data.size() * 2);
        static const char* H = "0123456789abcdef";
        for (uint8_t b : data) { hex += H[b >> 4]; hex += H[b & 0xF]; }
        Logger::Debug("[WIRE<-] client %u %zuB: %s", clientId, data.size(), hex.c_str());
    }

    // CreateOrGetClient above guarantees this entry exists, but look it up with a
    // checked find() rather than operator[]: operator[] would silently default-insert
    // a null shared_ptr if the entry were ever missing, and the very next line would
    // dereference it. Reject safely instead of crashing on attacker traffic.
    auto connIt = m_clients.find(addr);
    if (connIt == m_clients.end() || !connIt->second) {
        Logger::Warn("[ConnectionManager::HandleIncomingPacket] no connection object for %s:%u (clientId=%u), dropping packet",
                     addr.ip.c_str(), addr.port, clientId);
        net::RecordNetNull("HandleIncomingPacket/connLookup",
                           addr.ip + ":" + std::to_string(addr.port) + " has no connection object",
                           clientId);
        return;
    }
    auto conn = connIt->second;
    conn->UpdateLastHeartbeat();
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Updated heartbeat for client %u", clientId);

    // ---- UE3 control-channel path (EXCLUSIVE) ----
    // A well-formed UE3 packet is handled ONLY here. Running it through the legacy
    // Packet pipeline below would mis-parse the UE3 bytes as a tagged Packet
    // (Packet::FromBuffer) and enqueue garbage into the game queue (observed as a
    // flood of "Rejecting malformed buffer" warns + bogus callback dispatches).
    if (ParseIncomingControl(clientId, data)) {
        Logger::Trace("[ConnectionManager::HandleIncomingPacket] client %u: handled as UE3 control packet", clientId);
        return;
    }

    // Protocol classification is sticky. Once an endpoint has supplied genuine
    // UE3 framing, a later malformed datagram must never be reinterpreted as the
    // emulator's legacy tagged-Packet format. In particular, PacketCodec rejects
    // a zero-tailed datagram because it has no UE3 terminator; without this gate
    // an established retail client could wrap an arbitrary legacy gameplay tag,
    // append a zero byte, and reach the parallel GameServer dispatcher below.
    if (conn->IsUE3Client()) {
        Logger::Debug(
            "[ConnectionManager::HandleIncomingPacket] client %u is already "
            "classified as UE3; dropping %zu-byte non-UE3/malformed datagram",
            clientId, data.size());
        TELEMETRY_INCREMENT_PACKETS_DROPPED();
        return;
    }

    // The reverse-engineering observer's UE3Protocol parser models an obsolete,
    // byte-aligned synthetic header and cannot decode retail's bit-packed UE3
    // framing. Do not feed real UE3 datagrams to it: PacketCodec above is the
    // production decoder and PacketRecorder already preserves the raw evidence.

    // ---- legacy / non-UE3 path (internal Packet format: in-process tests, tools) ----
    PacketMetadata meta;
    meta.clientId = clientId;
    Packet pkt = Packet::FromBuffer(data, meta);
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Parsed packet: tag='%s', clientId=%u, payloadSize=%u",
                  pkt.GetTag().c_str(), clientId, pkt.GetPayloadSize());

    // Feed parsed packet to protocol decoder for structure analysis
    GetProtocolDecoder().OnPacketReceived(clientId, pkt.RawData(), pkt.GetTag());
    Logger::Debug("[ConnectionManager::HandleIncomingPacket] Fed parsed packet (tag='%s') to protocol decoder", pkt.GetTag().c_str());

    // Dispatch via callback (to NetworkManager -> GameServer) or directly
    if (m_packetCallback) {
        Logger::Debug("[ConnectionManager::HandleIncomingPacket] Dispatching via packet callback for client %u, tag='%s'",
                      clientId, pkt.GetTag().c_str());
        m_packetCallback(clientId, pkt, meta);
    } else if (m_server) {
        Logger::Debug("[ConnectionManager::HandleIncomingPacket] Dispatching directly to GameServer for client %u, tag='%s'",
                      clientId, pkt.GetTag().c_str());
        m_server->OnPacketReceived(clientId, pkt, meta);
    } else {
        Logger::Warn("[ConnectionManager::HandleIncomingPacket] No callback and no server set, packet tag='%s' from client %u dropped",
                     pkt.GetTag().c_str(), clientId);
    }
    Logger::Trace("[ConnectionManager::HandleIncomingPacket] Exit");
}

bool ConnectionManager::SendToClient(uint32_t clientId, const Packet& pkt) {
    Logger::Trace("[ConnectionManager::SendToClient] Entry: clientId=%u, tag='%s'", clientId, pkt.GetTag().c_str());
    auto conn = GetConnection(clientId);
    if (!conn) {
        Logger::Error("[ConnectionManager::SendToClient] No connection found for clientId=%u", clientId);
        Logger::Trace("[ConnectionManager::SendToClient] Exit: returning false (no connection)");
        return false;
    }
    Logger::Debug("[ConnectionManager::SendToClient] Found connection for client %u (%s:%u), sending packet tag='%s'",
                  clientId, conn->GetIP().c_str(), conn->GetPort(), pkt.GetTag().c_str());
    bool result = conn->SendPacket(pkt);
    Logger::Debug("[ConnectionManager::SendToClient] SendPacket returned %s for client %u", result ? "true" : "false", clientId);
    Logger::Trace("[ConnectionManager::SendToClient] Exit: returning %s", result ? "true" : "false");
    return result;
}

void ConnectionManager::Broadcast(const Packet& pkt) {
    Logger::Trace("[ConnectionManager::Broadcast] Entry: tag='%s'", pkt.GetTag().c_str());
    auto connections = GetAllConnections();
    Logger::Debug("[ConnectionManager::Broadcast] Broadcasting packet tag='%s' to %zu clients",
                  pkt.GetTag().c_str(), connections.size());
    for (size_t i = 0; i < connections.size(); ++i) {
        auto& conn = connections[i];
        Logger::Trace("[ConnectionManager::Broadcast] Sending to client %u (%s:%u) [%zu/%zu]",
                      conn->GetClientId(), conn->GetIP().c_str(), conn->GetPort(), i + 1, connections.size());
        conn->SendPacket(pkt);
    }
    Logger::Info("[ConnectionManager::Broadcast] Broadcast complete: tag='%s' sent to %zu clients",
                 pkt.GetTag().c_str(), connections.size());
    Logger::Trace("[ConnectionManager::Broadcast] Exit");
}

void ConnectionManager::BroadcastRetailObjectiveState() {
    for (const auto& kv : m_clients) {
        const auto& conn = kv.second;
        if (!conn || conn->IsDisconnected() || !conn->IsUE3Client() ||
            !conn->IsHandshakeComplete()) {
            continue;
        }
        SendRetailObjectiveState(conn->GetClientId(), /*baseline=*/false);
    }
}

std::shared_ptr<ClientConnection> ConnectionManager::GetConnection(uint32_t clientId) const {
    Logger::Trace("[ConnectionManager::GetConnection] Entry: clientId=%u", clientId);
    for (auto& kv : m_clients) {
        if (kv.second->GetClientId() == clientId) {
            Logger::Debug("[ConnectionManager::GetConnection] Found connection for client %u at %s:%u",
                          clientId, kv.second->GetIP().c_str(), kv.second->GetPort());
            Logger::Trace("[ConnectionManager::GetConnection] Exit: returning valid connection");
            return kv.second;
        }
    }
    Logger::Debug("[ConnectionManager::GetConnection] No connection found for clientId=%u (searched %zu entries)",
                  clientId, m_clients.size());
    Logger::Trace("[ConnectionManager::GetConnection] Exit: returning nullptr");
    return nullptr;
}

std::vector<std::shared_ptr<ClientConnection>> ConnectionManager::GetAllConnections() const {
    Logger::Trace("[ConnectionManager::GetAllConnections] Entry");
    std::vector<std::shared_ptr<ClientConnection>> list;
    list.reserve(m_clients.size());
    for (auto& kv : m_clients) {
        list.push_back(kv.second);
    }
    Logger::Debug("[ConnectionManager::GetAllConnections] Collected %zu connections", list.size());
    Logger::Trace("[ConnectionManager::GetAllConnections] Exit: returning %zu connections", list.size());
    return list;
}

uint32_t ConnectionManager::FindClientByAddress(const std::string& ip, uint16_t port) const {
    Logger::Trace("[ConnectionManager::FindClientByAddress] Entry: ip='%s', port=%u", ip.c_str(), port);
    ClientAddress addr{ip, port};
    auto it = m_clients.find(addr);
    uint32_t result = it != m_clients.end() ? it->second->GetClientId() : UINT32_MAX;
    if (result != UINT32_MAX) {
        Logger::Debug("[ConnectionManager::FindClientByAddress] Found client %u at %s:%u", result, ip.c_str(), port);
    } else {
        Logger::Debug("[ConnectionManager::FindClientByAddress] No client found at %s:%u", ip.c_str(), port);
    }
    Logger::Trace("[ConnectionManager::FindClientByAddress] Exit: returning %u", result);
    return result;
}

uint32_t ConnectionManager::FindClientBySteamID(const std::string& steamId) const {
    Logger::Trace("[ConnectionManager::FindClientBySteamID] Entry: steamId='%s'", steamId.c_str());
    for (auto& kv : m_clients) {
        if (kv.second->GetSteamID() == steamId) {
            uint32_t clientId = kv.second->GetClientId();
            Logger::Debug("[ConnectionManager::FindClientBySteamID] Found client %u with steamId='%s'", clientId, steamId.c_str());
            Logger::Trace("[ConnectionManager::FindClientBySteamID] Exit: returning %u", clientId);
            return clientId;
        }
    }
    Logger::Debug("[ConnectionManager::FindClientBySteamID] No client found with steamId='%s' (searched %zu entries)",
                  steamId.c_str(), m_clients.size());
    Logger::Trace("[ConnectionManager::FindClientBySteamID] Exit: returning UINT32_MAX");
    return UINT32_MAX;
}

uint32_t ConnectionManager::CreateOrGetClient(const std::string& ip, uint16_t port) {
    Logger::Trace("[ConnectionManager::CreateOrGetClient] Entry: ip='%s', port=%u", ip.c_str(), port);
    ClientAddress addr{ip, port};
    auto it = m_clients.find(addr);
    if (it != m_clients.end()) {
        uint32_t existingId = it->second->GetClientId();
        Logger::Debug("[ConnectionManager::CreateOrGetClient] Existing client found: clientId=%u for %s:%u",
                      existingId, ip.c_str(), port);
        Logger::Trace("[ConnectionManager::CreateOrGetClient] Exit: returning existing clientId=%u", existingId);
        return existingId;
    }
    Logger::Debug("[ConnectionManager::CreateOrGetClient] No existing client for %s:%u, checking capacity (%zu/%zu)",
                  ip.c_str(), port, m_clients.size(), m_maxClients);
    bool useTravelHeadroom = false;
    if (m_clients.size() >= m_maxClients) {
        // ClientTravel may reconnect from a new UDP source port while the old
        // endpoint is intentionally retained to drain its reliable RPC. Permit
        // at most one provisional same-address endpoint per pending session so
        // the transport cap cannot make that reconnect path unreachable. The
        // IP constraint and per-pending lease count keep this headroom bounded
        // before the client-presented Steam64 is available at Login.
        size_t pendingFromAddress = 0;
        size_t leasedToAddress = 0;
        for (const auto& [existingAddress, connection] : m_clients) {
            if (!connection || connection->IsDisconnected() ||
                existingAddress.ip != ip) {
                continue;
            }
            const auto stateIt = m_controlState.find(connection->GetClientId());
            if (stateIt == m_controlState.end()) continue;
            pendingFromAddress +=
                static_cast<size_t>(stateIt->second.mapTravelPending);
            leasedToAddress += static_cast<size_t>(
                stateIt->second.travelReconnectHeadroomLease);
        }
        useTravelHeadroom = m_maxClients != 0 &&
            leasedToAddress < pendingFromAddress;
    }
    if (m_clients.size() >= m_maxClients && !useTravelHeadroom) {
        Logger::Warn("ConnectionManager: Max clients reached, rejecting new client from %s:%u", ip.c_str(), port);
        Logger::Debug("[ConnectionManager::CreateOrGetClient] Max clients %zu reached, cannot create new client",
                      m_maxClients);
        Logger::Trace("[ConnectionManager::CreateOrGetClient] Exit: returning UINT32_MAX (max clients)");
        return UINT32_MAX;
    }
    uint32_t clientId = m_nextClientId++;
    Logger::Debug("[ConnectionManager::CreateOrGetClient] Assigned new clientId=%u (nextClientId now=%u)",
                  clientId, m_nextClientId);
    auto conn = std::make_shared<ClientConnection>(clientId, ip, port, m_socket, this);
    m_clients[addr] = conn;
    m_controlState[clientId].travelReconnectHeadroomLease =
        useTravelHeadroom;
    m_deploymentCoordinator.ResetClient(clientId);
    m_deploymentCountdown.RemoveClient(clientId);
    Logger::Info("ConnectionManager: New client %u from %s:%u%s", clientId,
                 ip.c_str(), port,
                 useTravelHeadroom ? " (bounded travel reconnect headroom)" : "");
    Logger::Debug("[ConnectionManager::CreateOrGetClient] Total clients now: %zu", m_clients.size());

    // Admission creates a live transport, not an authenticated player.  The
    // latter gauge advances only after the accepted login callback.
    UpdateTelemetryPlayerCounts();

    // Notify protocol decoder of new client connection
    GetProtocolDecoder().OnClientConnected(clientId, ip);
    Logger::Debug("[ConnectionManager::CreateOrGetClient] Protocol decoder notified of new client %u", clientId);

    Logger::Trace("[ConnectionManager::CreateOrGetClient] Exit: returning new clientId=%u", clientId);
    return clientId;
}

void ConnectionManager::RemoveClientSession(const ClientAddress& addr, const char* reason) {
    auto it = m_clients.find(addr);
    if (it == m_clients.end()) return;

    // A null entry should be impossible through CreateOrGetClient(), but this is
    // the final quarantine path for a partially constructed/corrupted session.
    // Leaving it in m_clients permanently consumes capacity and makes periodic
    // housekeeping dereference null on every pass.  There is no clientId and
    // therefore no protocol/game callback that can be invoked safely; erase only
    // the poisoned address entry and restore the transport-capacity accounting.
    if (!it->second) {
        Logger::Warn(
            "[ConnectionManager] Removing null client entry at %s:%u (%s)",
            addr.ip.c_str(), static_cast<unsigned>(addr.port),
            reason ? reason : "invalid session");
        m_clients.erase(it);
        NormalizeTravelReconnectHeadroomLeases();
        UpdateTelemetryPlayerCounts();
        return;
    }

    const std::shared_ptr<ClientConnection> conn = it->second;
    const uint32_t clientId = conn->GetClientId();
    Logger::Info("[ConnectionManager] Removing client %u at %s:%u (%s)",
                 clientId, addr.ip.c_str(), addr.port, reason ? reason : "session ended");

    // Suppress every send path before callbacks or cross-client actor retirement
    // can observe this connection.  Peer control-channel close is terminal and
    // must not provoke an ACK, retry, or teardown RPC back to that endpoint.
    conn->MarkDisconnected();
    GetProtocolDecoder().OnClientDisconnected(clientId);
    if (m_movementValidator) {
        (void)m_movementValidator->RemoveState(clientId);
    }
    // Close this human's non-owning PRI/pawn actors before their authoritative
    // Player/Team state is removed.  Viewer channel pairs remain tombstoned
    // until that viewer disconnects, preventing delayed close aliasing.
    RetireRemoteParticipantFromViewers(ParticipantId::Human(clientId));
    if (m_server) {
        m_server->OnClientDisconnected(clientId);
        // RoleSystem removal may promote a remaining member into retail slot
        // zero. Publish every repaired coordinate before discarding this
        // connection's control state; the departing connection is already
        // marked disconnected and is therefore excluded from the pass.
        SynchronizeRetailSquadAssignments();
    }

    // These maps contain every rolling value which must restart with a new UE3
    // connection: handshake phase, inbound reliable reorder window, outbound
    // PacketId/ChSequence, pending acks/retransmits, and spawn/menu guards.
    m_handshakes.erase(clientId);
    m_controlState.erase(clientId);
    m_deploymentCoordinator.RemoveClient(clientId);
    m_deploymentCountdown.RemoveClient(clientId);
    m_clients.erase(it);
    NormalizeTravelReconnectHeadroomLeases();

    UpdateTelemetryPlayerCounts();
}

// The friend lifecycle harness calls this out of line to verify gauge
// transitions. MSVC may otherwise inline every production call and omit the
// private symbol entirely, making the test target's legitimate friend call
// fail with LNK2019 after unrelated optimizer-shaping edits.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void ConnectionManager::UpdateTelemetryPlayerCounts() const {
    uint64_t activeConnections = 0;
    uint64_t authenticatedPlayers = 0;

    for (const auto& [address, connection] : m_clients) {
        (void)address;
        if (!connection || connection->IsDisconnected()) continue;
        ++activeConnections;

        const uint32_t clientId = connection->GetClientId();
        const auto controlIt = m_controlState.find(clientId);
        if (controlIt != m_controlState.end() &&
            controlIt->second.mapTravelPending) {
            // ClientTravel intentionally retains this endpoint only to drain
            // ACKs/reliable control traffic.  Its Player and Security records
            // already belong to the retired world.
            continue;
        }

        bool acceptedLogin = connection->IsHandshakeComplete();
        const auto handshakeIt = m_handshakes.find(clientId);
        if (handshakeIt != m_handshakes.end() && handshakeIt->second) {
            const HandshakePhase phase = handshakeIt->second->Phase();
            acceptedLogin = acceptedLogin ||
                phase == HandshakePhase::WelcomeSent ||
                phase == HandshakePhase::Joined;
        }
        authenticatedPlayers += static_cast<uint64_t>(acceptedLogin);
    }

    TELEMETRY_UPDATE_PLAYER_COUNTS(activeConnections,
                                   authenticatedPlayers);
}

void ConnectionManager::NormalizeTravelReconnectHeadroomLeases() {
    const size_t allowedExcess = m_clients.size() > m_maxClients
        ? m_clients.size() - m_maxClients
        : 0u;
    size_t leased = 0;
    for (const auto& [clientId, state] : m_controlState) {
        (void)clientId;
        leased += static_cast<size_t>(state.travelReconnectHeadroomLease);
    }
    if (leased <= allowedExcess) return;

    size_t promoteToBaseCapacity = leased - allowedExcess;
    for (auto& [clientId, state] : m_controlState) {
        (void)clientId;
        if (!state.travelReconnectHeadroomLease) continue;
        state.travelReconnectHeadroomLease = false;
        if (--promoteToBaseCapacity == 0) break;
    }
}

size_t ConnectionManager::ExpirePendingTravelSessions(uint64_t nowMs) {
    std::vector<ClientAddress> expired;
    expired.reserve(m_clients.size());

    for (const auto& [address, connection] : m_clients) {
        if (!connection) continue;
        const auto stateIt = m_controlState.find(connection->GetClientId());
        if (stateIt == m_controlState.end() ||
            !stateIt->second.mapTravelPending) {
            continue;
        }

        const uint64_t startedAt = stateIt->second.mapTravelStartedMs;
        const bool validUnexpiredClock = startedAt != 0 &&
            nowMs >= startedAt && nowMs - startedAt < kMapTravelTimeoutMs;
        if (validUnexpiredClock) continue;

        Logger::Warn(
            "[ClientTravel] client %u did not establish a fresh admitted "
            "session within %llu ms; retiring stale drain-only transport",
            connection->GetClientId(),
            static_cast<unsigned long long>(kMapTravelTimeoutMs));
        expired.push_back(address);
    }

    for (const ClientAddress& address : expired) {
        RemoveClientSession(address, "ClientTravel reconnect deadline expired");
    }
    return expired.size();
}

size_t ConnectionManager::RetireSupersededTravelSessions(
    uint32_t newClientId, uint64_t presentedSteamId) {
    if (presentedSteamId == 0) return 0;

    const std::shared_ptr<ClientConnection> newConnection =
        GetConnection(newClientId);
    if (!newConnection || newConnection->GetIP().empty()) return 0;

    const std::string steamId = std::to_string(presentedSteamId);
    std::vector<ClientAddress> staleAddresses;
    for (const auto& [address, connection] : m_clients) {
        if (!connection || connection->GetClientId() == newClientId ||
            connection->IsDisconnected() ||
            connection->GetIP() != newConnection->GetIP() ||
            !connection->HasPresentedSteamID() ||
            connection->GetSteamID() != steamId) {
            continue;
        }

        const auto stateIt = m_controlState.find(connection->GetClientId());
        if (stateIt == m_controlState.end() ||
            !stateIt->second.mapTravelPending) {
            // Never evict an active session merely because another endpoint
            // claimed the same Steam64. Only a server-issued travel state is
            // proof that replacement is expected.
            continue;
        }
        staleAddresses.push_back(address);
    }

    if (staleAddresses.size() > 1) {
        Logger::Warn(
            "[ClientTravel] reconnect client %u found %zu same-address stale "
            "travel sessions for SteamID %s; retiring every duplicate",
            newClientId, staleAddresses.size(), steamId.c_str());
    }
    for (const ClientAddress& address : staleAddresses) {
        Logger::Info(
            "[ClientTravel] same-address reconnect client %u supersedes "
            "travel session at %s:%u for SteamID %s",
            newClientId, address.ip.c_str(), address.port, steamId.c_str());
        RemoveClientSession(address,
                            "superseded by admitted same-address travel reconnect");
    }
    return staleAddresses.size();
}

void ConnectionManager::ResetEstablishedSessionForFreshHandshake(
    const std::vector<uint8_t>& data, const ClientAddress& addr) {
    auto clientIt = m_clients.find(addr);
    if (clientIt == m_clients.end() || !clientIt->second) return;

    const uint32_t clientId = clientIt->second->GetClientId();
    bool replaceable = clientIt->second->IsDisconnected();
    auto hsIt = m_handshakes.find(clientId);
    if (hsIt != m_handshakes.end() && hsIt->second) {
        const HandshakePhase phase = hsIt->second->Phase();
        // Do not reset WelcomeSent: the original ch0 open may still arrive late
        // while the retail client spends tens of seconds loading packages.  A
        // genuine user reconnect from the trapped gameplay state is Joined (or
        // explicitly disconnected/rejected), which is safe to supersede.
        replaceable = replaceable || phase == HandshakePhase::Joined ||
                      phase == HandshakePhase::Rejected;
    }

    if (!replaceable || !IsFreshControlHandshakeStart(data)) return;

    Logger::Info("[ConnectionManager] Fresh HandshakeStart reused established endpoint %s:%u; "
                 "resetting old client %u instead of feeding its Joined reliable stream",
                 addr.ip.c_str(), addr.port, clientId);
    RemoveClientSession(addr, "superseded by fresh HandshakeStart");
}

void ConnectionManager::RemoveStaleConnections() {
    Logger::Trace("[ConnectionManager::RemoveStaleConnections] Entry: %zu clients", m_clients.size());
    auto now = std::chrono::steady_clock::now();
    // Acknowledging ClientTravel removes it from pendingReliable, and the old
    // endpoint can keep sending ACKs/keepalives forever. Expire that drain-only
    // incarnation on its own finite clock before applying the generic heartbeat
    // timeout to the remaining sessions.
    ExpirePendingTravelSessions(NowMs());
    auto cfgMgr = m_server ? m_server->GetConfigManager() : nullptr;
    // 120s (was 30s): 30 was too aggressive - a player pausing in the menu, or a brief
    // reliable-channel stall, would be dropped and surface as a client-side "connection
    // timed out". Real RS2 servers are far more lenient. Override via Network.timeout_seconds.
    int timeoutSecs = cfgMgr ? cfgMgr->GetInt("Network.timeout_seconds", 120) : 120;
    Logger::Debug("[ConnectionManager::RemoveStaleConnections] Timeout threshold: %d seconds", timeoutSecs);

    std::vector<ClientAddress> toRemove;
    for (auto& kv : m_clients) {
        auto conn = kv.second;
        if (!conn) {
            Logger::Warn(
                "[ConnectionManager::RemoveStaleConnections] Null client entry "
                "at %s:%u; quarantining it without callbacks",
                kv.first.ip.c_str(),
                static_cast<unsigned>(kv.first.port));
            toRemove.push_back(kv.first);
            continue;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn->GetLastHeartbeat()).count();
        Logger::Trace("[ConnectionManager::RemoveStaleConnections] Client %u (%s:%u): elapsed=%lld s since last heartbeat",
                      conn->GetClientId(), conn->GetIP().c_str(), conn->GetPort(), (long long)elapsed);
        if (conn->IsDisconnected() || elapsed > timeoutSecs) {
            Logger::Info("ConnectionManager: Retiring client %u (%s)", conn->GetClientId(),
                         conn->IsDisconnected() ? "already disconnected" : "inactivity timeout");
            toRemove.push_back(kv.first);
        }
    }
    Logger::Debug("[ConnectionManager::RemoveStaleConnections] Removing %zu stale connections", toRemove.size());
    for (auto& addr : toRemove) {
        RemoveClientSession(addr, "disconnected or inactivity timeout");
    }
    Logger::Debug("[ConnectionManager::RemoveStaleConnections] After cleanup: %zu clients remaining", m_clients.size());
    Logger::Trace("[ConnectionManager::RemoveStaleConnections] Exit");
}

void ConnectionManager::UpdateBandwidthWindows() {
    Logger::Trace("[ConnectionManager::UpdateBandwidthWindows] Entry");
    if (m_bwManager) {
        Logger::Debug("[ConnectionManager::UpdateBandwidthWindows] Calling BandwidthManager::Update()");
        m_bwManager->Update();
    } else {
        Logger::Debug("[ConnectionManager::UpdateBandwidthWindows] No BandwidthManager set, skipping");
    }
    Logger::Trace("[ConnectionManager::UpdateBandwidthWindows] Exit");
}

uint32_t ConnectionManager::GetBandwidthLimit() const {
    Logger::Trace("[ConnectionManager::GetBandwidthLimit] Entry/Exit: returning %u", m_bandwidthLimit);
    return m_bandwidthLimit;
}

void ConnectionManager::SetMaxClients(size_t maxClients) {
    Logger::Trace("[ConnectionManager::SetMaxClients] Entry: maxClients=%zu", maxClients);
    size_t previous = m_maxClients;
    m_maxClients = maxClients;
    NormalizeTravelReconnectHeadroomLeases();
    Logger::Debug("[ConnectionManager::SetMaxClients] Max clients changed: %zu -> %zu", previous, m_maxClients);
    Logger::Info("[ConnectionManager::SetMaxClients] Max clients set to %zu", maxClients);
    Logger::Trace("[ConnectionManager::SetMaxClients] Exit");
}

size_t ConnectionManager::GetMaxClients() const {
    Logger::Trace("[ConnectionManager::GetMaxClients] Entry/Exit: returning %zu", m_maxClients);
    return m_maxClients;
}

bool ConnectionManager::ResetRetailMovementValidation(
    uint32_t clientId, const Vector3& authoritativePosition) {
    if (!m_movementValidator || clientId == 0) return false;

    Vector3 authoritativeForward = Vector3::Forward();
    const auto stateIt = m_controlState.find(clientId);
    if (stateIt != m_controlState.end() && stateIt->second.latestViewValid) {
        const GameplayRpc::AimDirection facing =
            GameplayRpc::DirectionFromPackedView(
                stateIt->second.latestPackedView);
        if (facing.valid) authoritativeForward = facing.value;
    }

    const bool reset = m_movementValidator->ResetStateAt(
        clientId, authoritativePosition, authoritativeForward,
        MovementValidator::Clock::now());
    if (!reset) {
        Logger::Warn(
            "[MovementAuthority] failed authoritative reset for client %u "
            "at (%.1f,%.1f,%.1f)",
            clientId, authoritativePosition.x, authoritativePosition.y,
            authoritativePosition.z);
    }
    return reset;
}

// ===========================================================================
//  Game-facing handshake observer interface
// ===========================================================================

void ConnectionManager::SetClientLoggedInCallback(ClientLoggedInCallback cb) {
    Logger::Debug("[ConnectionManager::SetClientLoggedInCallback] %s", cb ? "set" : "cleared");
    m_clientLoggedInCb = std::move(cb);
}

void ConnectionManager::SetClientJoinedCallback(ClientJoinedCallback cb) {
    Logger::Debug("[ConnectionManager::SetClientJoinedCallback] %s", cb ? "set" : "cleared");
    m_clientJoinedCb = std::move(cb);
}

void ConnectionManager::FireClientLoggedIn(const ClientLoggedInEvent& ev) {
    Logger::Info("[ConnectionManager::FireClientLoggedIn] client %u logged in (steamId=%llu, name='%s')",
                 ev.clientId, (unsigned long long)ev.steamId, ev.options.PlayerName().c_str());
    // Mirror the parsed player name onto the connection for convenience.
    if (auto conn = GetConnection(ev.clientId)) {
        if (!ev.options.PlayerName().empty()) conn->SetPlayerName(ev.options.PlayerName());
    }

    // Run the game-layer login gate FIRST. The subscriber (ConnectionLoginBridge::
    // OnClientLoggedIn -> PreLogin) enforces capacity / password / ban and, on
    // rejection, MarkDisconnected()s the connection. PreLogin is synchronous, so
    // this still completes within FireClientLoggedIn - the bootstrap below goes out
    // in the same call, immediately after NMT_Welcome, before the client's
    // "packages verified" reply - but only for an ACCEPTED login.
    const bool gameAdmissionRan = static_cast<bool>(m_clientLoggedInCb);
    if (m_clientLoggedInCb) {
        m_clientLoggedInCb(ev);
    } else {
        Logger::Debug("[ConnectionManager::FireClientLoggedIn] no Game subscriber for ClientLoggedIn (client %u)",
                      ev.clientId);
    }

    // World-replication bootstrap goes out HERE - immediately after NMT_Welcome -
    // exactly as the official server does (capture: Welcome f162 -> PackageMap
    // f167-f185, BEFORE the client's "packages verified" reply at f227). Sending it
    // on Join instead would deadlock a real client: it won't send Join/ready until
    // it has reconciled the PackageMap. See docs/RS2V_PostJoin_Replication_7258.md.
    //
    // Gate it on the login being accepted: a rejected client (full / wrong password
    // / banned) was MarkDisconnected() by the gate above, and must not receive any
    // post-login world state. (SendRaw also drops on m_disconnected as defence in
    // depth, but skipping the work here avoids a pointless bootstrap burst and
    // leaving a rejected peer parked in WelcomeSent.)
    const std::shared_ptr<ClientConnection> acceptedConnection =
        GetConnection(ev.clientId);
    if (!acceptedConnection || acceptedConnection->IsDisconnected()) {
        Logger::Info("[ConnectionManager::FireClientLoggedIn] login for client %u was rejected by the game "
                     "layer; suppressing replication bootstrap", ev.clientId);
        UpdateTelemetryPlayerCounts();
        return;
    }

    // Only an admitted endpoint may retire the old travel ledger. Steam64 is
    // client-presented in this emulator, so doing this before password/ban/
    // capacity admission would let a rejected same-NAT peer erase a valid
    // ClientTravel retransmission session. Old gameplay Player state was
    // already removed when travel was queued and therefore does not count in
    // the game-layer capacity gate.
    const size_t retiredTravelSessions = gameAdmissionRan
        ? RetireSupersededTravelSessions(ev.clientId, ev.steamId)
        : 0u;
    if (retiredTravelSessions != 0) {
        const auto stateIt = m_controlState.find(ev.clientId);
        if (stateIt != m_controlState.end()) {
            stateIt->second.travelReconnectHeadroomLease = false;
        }
    }
    UpdateTelemetryPlayerCounts();
    SendReplicationBootstrap(ev.clientId);
}

void ConnectionManager::FireClientJoined(const ClientJoinedEvent& ev) {
    Logger::Info("[ConnectionManager::FireClientJoined] client %u joined", ev.clientId);
    // The UE3 handshake is complete: from here the game layer may send to this
    // client (until now SendPacket was suppressed to keep the handshake's wire
    // stream clean).
    if (auto conn = GetConnection(ev.clientId)) {
        // A client rejected at PreLogin was MarkDisconnected() but not necessarily
        // erased yet; don't let a Join that races the teardown drive actor bootstrap.
        if (conn->IsDisconnected()) {
            Logger::Info("[ConnectionManager::FireClientJoined] client %u is disconnected (rejected login); "
                         "suppressing actor bootstrap", ev.clientId);
            return;
        }
        conn->SetHandshakeComplete(true);
    }
    UpdateTelemetryPlayerCounts();
    // The PackageMap export went out earlier (on ClientLoggedIn / right after
    // Welcome). Now that the client has Joined, open the bootstrap ACTOR channels
    // (ROGameReplicationInfo, TeamInfo, the local PlayerController, PRIs) so it can
    // build the world and spawn. Best-effort verbatim replay of the official f231
    // burst - see SendActorBootstrap.
    SendActorBootstrap(ev.clientId);

    if (m_clientJoinedCb) {
        m_clientJoinedCb(ev);
    } else {
        Logger::Debug("[ConnectionManager::FireClientJoined] no Game subscriber for ClientJoined (client %u)",
                      ev.clientId);
    }
}

bool ConnectionManager::SendRawToClient(uint32_t clientId, const std::vector<uint8_t>& bytes) {
    auto conn = GetConnection(clientId);
    if (!conn) {
        Logger::Error("[ConnectionManager::SendRawToClient] No connection for client %u (%zu bytes dropped)",
                      clientId, bytes.size());
        return false;
    }
    // `bytes` is a control-channel MESSAGE payload (a ControlChannel::Build*
    // result: <BYTE NMT><fields>). Frame it into UE3 packets - reliable control
    // bunch(es) with a rolling PacketId/ChSequence, fragmented to MaxPacket, with
    // any pending acks drained onto the first packet - then encode and send each.
    // The BunchDataBits SerializeInt bound is phase-dependent on the wire and MUST
    // match what the client decodes with, or the client mis-reads the bunch (it
    // still acks at the packet level, masking the bug). During the StatelessConnect
    // We are the SERVER: encode S2C bunches with the server's MaxPacket (~1500,
    // bound ~12000) from the FIRST packet (including the HandshakeChallenge) - the
    // client decodes server bunches at that bound from the start. (Asymmetric vs the
    // client's 1280 that we DECODE inbound with - see PacketCodec.h. There is NO
    // small-bound handshake phase; the old bound-64 encode made our challenge
    // unparseable to the real client, stalling it on the loading screen.)
    const uint32_t maxPacketBytes = PacketCodec::kServerSendMaxPacketBytes;
    const uint32_t maxBunchDataBits = maxPacketBytes * 8u - 1u;

    ControlState& cs = GetControlState(clientId);
    bool ok = true;
    for (const PacketCodec::Packet& pkt :
         cs.outbound.BuildControlMessagePackets(bytes, maxBunchDataBits)) {
        const std::vector<uint8_t> wire = PacketCodec::Encode(pkt, maxPacketBytes);
        if (wire.size() <= 80) {  // HANDSHAKE WIRE TRACE (small control sends)
            std::string hex; hex.reserve(wire.size() * 2);
            static const char* H = "0123456789abcdef";
            for (uint8_t b : wire) { hex += H[b >> 4]; hex += H[b & 0xF]; }
            Logger::Debug("[WIRE->] client %u %zuB: %s", clientId, wire.size(), hex.c_str());
        }
        const uint64_t sentAtMs = NowMs();
        if (!conn->SendRaw(wire.data(), wire.size())) {
            ok = false;
        } else {
            cs.lastServerSendMs = sentAtMs;
        }

        // BuildControlMessagePackets emits reliable ch0 bunches, but this path
        // historically bypassed the retransmission ledger used by actor sends.
        // A single dropped PackageMap tail then left the retail client waiting
        // forever before NMT_Join. Record EACH packet independently: ACKing one
        // fragment must never retire a sibling fragment that rode in another
        // PacketId. Cold clients may take ~20s to answer the first challenge, so
        // control traffic uses a deliberately slower/longer retry schedule than
        // the latency-sensitive actor defaults. A cold Steam/EAC launch plus a
        // first Compound load has been observed to spend more than 64 seconds
        // between PackageMap delivery and NMT_Join, so retain almost three
        // minutes of bounded headroom before retiring only this session.
        std::vector<PacketCodec::Bunch> reliable;
        for (const PacketCodec::Bunch& bunch : pkt.bunches) {
            if (bunch.bReliable) reliable.push_back(bunch);
        }
        if (!reliable.empty()) {
            ControlState::SentReliable pending;
            pending.packetIds.push_back(pkt.packetId);
            pending.lastSendMs = sentAtMs;
            pending.retryDelayMs = 8000;
            pending.maxResends = 22;  // 176s bounded cold-load headroom.
            pending.bunches = std::move(reliable);
            cs.pendingReliable.push_back(std::move(pending));
        }
    }
    return ok;
}

HandshakeState& ConnectionManager::GetOrCreateHandshake(uint32_t clientId) {
    auto it = m_handshakes.find(clientId);
    if (it != m_handshakes.end()) return *it->second;

    // The handshake's raw-send callback funnels outbound control bytes back to
    // this connection. Capturing `this` + clientId is safe: the handshake is
    // owned by m_handshakes and erased no later than this ConnectionManager.
    auto hs = std::make_unique<HandshakeState>(
        clientId,
        [this, clientId](const std::vector<uint8_t>& payload) {
            this->SendRawToClient(clientId, payload);
        },
        [this](const ClientLoggedInEvent& ev) { this->FireClientLoggedIn(ev); },
        [this](const ClientJoinedEvent& ev)   { this->FireClientJoined(ev); });
    auto* raw = hs.get();
    m_handshakes[clientId] = std::move(hs);
    Logger::Debug("[ConnectionManager::GetOrCreateHandshake] Created handshake for client %u", clientId);
    return *raw;
}

ConnectionManager::ControlState& ConnectionManager::GetControlState(uint32_t clientId) {
    ControlState& cs = m_controlState[clientId];
    if (!cs.reassembler) {
        // Reassembled control messages are dispatched straight into the client's
        // handshake state machine. Capturing `this` + clientId is safe: both maps
        // outlive no later than this ConnectionManager.
        cs.reassembler = std::make_unique<PacketCodec::ControlReassembler>(
            [this, clientId](const std::vector<uint8_t>& payload) {
                this->GetOrCreateHandshake(clientId).HandleControlMessage(payload);
            });
    }
    return cs;
}

const RetailBootstrap::Profile& ConnectionManager::GetRetailBootstrapProfile(uint32_t clientId) {
    ControlState& cs = GetControlState(clientId);
    if (!cs.retailBootstrapProfile) {
        cs.retailBootstrapProfile = ResolveRetailBootstrapProfile(m_server);
    }
    return *cs.retailBootstrapProfile;
}

const std::optional<RetailBootstrap::ArtifactSelection>&
ConnectionManager::GetRetailArtifactSelection(uint32_t clientId) {
    ControlState& cs = GetControlState(clientId);
    if (!cs.retailArtifactSelectionResolved) {
        const char* requestedEnvironment =
            std::getenv(RetailBootstrap::kArtifactVariantEnvironment.data());
        const std::string_view requestedVariant = requestedEnvironment
            ? std::string_view(requestedEnvironment)
            : std::string_view{};
        std::string error;
        cs.retailArtifactSelection = RetailBootstrap::ResolveArtifactSelection(
            requestedVariant, error);
        cs.retailArtifactSelectionResolved = true;
        if (!cs.retailArtifactSelection) {
            Logger::Error(
                "[ReplicationBootstrap] client %u rejected %s: %s; "
                "ROGame replication disabled",
                clientId,
                RetailBootstrap::kArtifactVariantEnvironment.data(),
                error.c_str());
        } else {
            Logger::Info(
                "[ReplicationBootstrap] client %u froze variant='%.*s' "
                "artifact='%.*s' ROGame ObjectBase=%u",
                clientId,
                static_cast<int>(cs.retailArtifactSelection->variant.size()),
                cs.retailArtifactSelection->variant.data(),
                static_cast<int>(cs.retailArtifactSelection->path.size()),
                cs.retailArtifactSelection->path.data(),
                cs.retailArtifactSelection->roGame.actualObjectBase);
        }
    }
    return cs.retailArtifactSelection;
}

void ConnectionManager::SendEncodedPacket(uint32_t clientId, const PacketCodec::Packet& pkt) {
    auto conn = GetConnection(clientId);
    if (!conn) {
        return;
    }
    // Ack-only packets carry no bunches, so the BunchDataBits bound is moot here,
    // but keep the server-send MaxPacket for consistency (always, no phase).
    const std::vector<uint8_t> wire =
        PacketCodec::Encode(pkt, PacketCodec::kServerSendMaxPacketBytes);
    if (conn->SendRaw(wire.data(), wire.size())) {
        GetControlState(clientId).lastServerSendMs = NowMs();
    }
}

namespace {
// Build and cache one record stream per artifact/map/GameInfo profile. The
// canonical Resort capture remains the default; one exact environment opt-in
// selects the separate installed-package GUID candidate. RetailBootstrap
// structurally decodes the map package and Welcome before changing them,
// retaining opaque session bytes. std::map gives returned value references stable
// addresses as later profiles are inserted. A map rotation or test-time artifact
// selection therefore receives a distinct key instead of being trapped behind a
// previous process-lifetime once_flag.
const std::vector<std::vector<uint8_t>>& GetReplicationBootstrapRecords(
    const RetailBootstrap::Profile& profile,
    const RetailBootstrap::ArtifactSelection& selection) {
    static std::mutex cacheMutex;
    static std::map<std::string, std::vector<std::vector<uint8_t>>> cache;

    const std::string key = profile.mapUrl + "\x1f" + profile.gameClassPath +
        "\x1f" + std::string(selection.path);

    std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;

    std::vector<std::vector<uint8_t>> generated;
    const std::string selectedPath(selection.path);
    Logger::Info(
        "[ReplicationBootstrap] selected variant='%.*s' artifact='%s'",
        static_cast<int>(selection.variant.size()), selection.variant.data(),
        selectedPath.c_str());
    std::ifstream file(selectedPath, std::ios::binary);
    if (!file) {
        Logger::Info("[ReplicationBootstrap] '%s' not present - post-Join replication disabled",
                     selectedPath.c_str());
        return cache.emplace(key, std::move(generated)).first->second;
    }

    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    RetailBootstrap::Document canonical;
    RetailBootstrap::Document variant;
    std::string error;
    if (!RetailBootstrap::ValidateReplicationArtifact(
            bytes, selection.variant, error) ||
        !RetailBootstrap::Parse(bytes, canonical, error) ||
        !RetailBootstrap::BuildVariant(canonical, profile, variant, error)) {
        Logger::Error(
            "[ReplicationBootstrap] cannot build variant='%.*s' profile "
            "map='%s' game='%s': %s",
            static_cast<int>(selection.variant.size()), selection.variant.data(),
            profile.mapUrl.c_str(), profile.gameClassPath.c_str(), error.c_str());
        return cache.emplace(key, std::move(generated)).first->second;
    }

    generated = std::move(variant.records);
    Logger::Info("[ReplicationBootstrap] prepared %zu records (%zu source bytes) from '%s' "
                 "for map='%s' game='%s'%s",
                 generated.size(), bytes.size(), selectedPath.c_str(),
                 profile.mapUrl.c_str(), profile.gameClassPath.c_str(),
                 profile.experimental ? " [EXPERIMENTAL reconstructed profile]" : "");
    return cache.emplace(key, std::move(generated)).first->second;
}
} // namespace

namespace {
// One bootstrap actor bunch descriptor parsed from data/actor_bootstrap.bin.
struct ActorBunchRecord {
    uint16_t chIndex = 0;
    uint8_t  chType = 0;
    bool     bOpen = false, bClose = false, bReliable = false, bControl = false;
    uint16_t chSequence = 0;
    uint32_t bunchDataBits = 0;       // EXACT bit count (payload is ceil/8 bytes)
    std::vector<uint8_t> payload;
};

const std::vector<ActorBunchRecord>& GetActorBootstrapRecords(
    const RetailBootstrap::ArtifactSelection& selection) {
    static std::mutex cacheMutex;
    static std::map<std::string, std::vector<ActorBunchRecord>> cache;
    const std::string key(selection.variant);

    std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;

    std::vector<ActorBunchRecord> records;
    const char* kPath = "data/actor_bootstrap.bin";
    std::ifstream f(kPath, std::ios::binary);
    if (!f) {
        Logger::Info(
            "[ActorBootstrap] '%s' not present - bootstrap actor channels disabled",
            kPath);
        return cache.emplace(key, std::move(records)).first->second;
    }
    const std::vector<uint8_t> canonical((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
    std::vector<uint8_t> all;
    std::string error;
    if (!RetailBootstrap::BuildActorArtifactVariant(
            canonical, selection.variant, all, error) ||
        !RetailBootstrap::ValidateActorArtifactFraming(all, error)) {
        Logger::Error(
            "[ActorBootstrap] refused variant='%.*s' derived from '%s': %s; "
            "entire actor replication stream disabled",
            static_cast<int>(selection.variant.size()), selection.variant.data(),
            kPath, error.c_str());
        return cache.emplace(key, std::move(records)).first->second;
    }

    size_t off = 0;
    // Validation above proves that every descriptor and payload is complete;
    // never retain a parsed prefix if the on-disk stream has a malformed tail.
    while (off < all.size()) {
        ActorBunchRecord r;
        r.chIndex = static_cast<uint16_t>(all[off] | (all[off + 1] << 8));
        r.chType = all[off + 2];
        const uint8_t flags = all[off + 3];
        r.bOpen = flags & 0x1; r.bClose = flags & 0x2;
        r.bReliable = flags & 0x4; r.bControl = flags & 0x8;
        r.chSequence = static_cast<uint16_t>(all[off + 4] | (all[off + 5] << 8));
        r.bunchDataBits = static_cast<uint32_t>(all[off + 6]) |
                          (static_cast<uint32_t>(all[off + 7]) << 8) |
                          (static_cast<uint32_t>(all[off + 8]) << 16) |
                          (static_cast<uint32_t>(all[off + 9]) << 24);
        const size_t len = static_cast<size_t>(r.bunchDataBits / 8u) +
            (r.bunchDataBits % 8u != 0u ? 1u : 0u);
        off += 10u;
        r.payload.assign(all.begin() + static_cast<std::ptrdiff_t>(off),
                         all.begin() + static_cast<std::ptrdiff_t>(off + len));
        off += len;
        records.push_back(std::move(r));
    }
    Logger::Info(
        "[ActorBootstrap] loaded %zu actor bunch descriptors from '%s' "
        "with variant='%.*s'",
        records.size(), kPath, static_cast<int>(selection.variant.size()),
        selection.variant.data());
    return cache.emplace(key, std::move(records)).first->second;
}
} // namespace

void ConnectionManager::SendActorBootstrap(uint32_t clientId) {
    // LIVE per-session replication (milestone 1). The canned actor_bootstrap.bin
    // replay carries another session's actor state (stale GUIDs / PRI / position),
    // which the retail client tears down (ch3..ch140 closed with empty bClose) - so
    // it never has a real GRI/PRI and the team menu can't function. Build the
    // menu-critical actors live instead. Resort retains the captured replay by
    // default for A/B continuity; reconstructed non-Resort profiles are forced
    // through the map/mode-aware live builder below.
    const RetailBootstrap::Profile& profile =
        GetRetailBootstrapProfile(clientId);
    if (profile.usedFallback) {
        Logger::Error(
            "[ActorBootstrap] client %u rejected unsupported map profile; "
            "refusing to replay the Resort actor cohort into another world",
            clientId);
        return;
    }

    const std::optional<RetailBootstrap::ArtifactSelection>& selectedArtifact =
        GetRetailArtifactSelection(clientId);
    if (!selectedArtifact) return;
    const RetailBootstrap::ArtifactSelection& artifact = *selectedArtifact;

    const char* replayWorldEnv = std::getenv("RS2V_REPLAY_CAPTURE_WORLD");
    const bool replayCapturedWorld = replayWorldEnv &&
        (replayWorldEnv[0] == '1' || replayWorldEnv[0] == 'y' ||
         replayWorldEnv[0] == 'Y');
    if (replayCapturedWorld &&
        !artifact.roGame.capturedWorldReplayGrounded) {
        Logger::Error(
            "[ActorBootstrap] client %u rejected variant='%.*s' with "
            "RS2V_REPLAY_CAPTURE_WORLD=1: only the menu-critical Resort actor "
            "cohort is migrated; full 139-record replay disabled",
            clientId, static_cast<int>(artifact.variant.size()),
            artifact.variant.data());
        return;
    }

    {
        // Default = canned replay. UE3-source + client-log evidence: the canned
        // bootstrap triggers local-PC adoption on Resort. The minimal live open now
        // does too when its class ref comes from the selected PackageMap layout
        // (Compound dogfood: SetPlayer plus normal inbound ch2 RPCs). Keep the opt-in
        // for comparing the two Resort paths while non-Resort profiles require live.
        const char* lr = std::getenv("RS2V_LIVE_REPL");
        const bool liveRequested = lr &&
            (lr[0] == '1' || lr[0] == 'y' || lr[0] == 'Y');
        // The canned actor file is a Resort/Territories capture whose embedded
        // GRI h33 cannot represent Supremacy or Skirmish. Experimental map
        // profiles therefore require the live actor builder, which consumes the
        // same frozen profile as PackageMap/Welcome and ChangedTeams.
        const bool useLive = liveRequested || profile.experimental;
        if (useLive) {
            if (profile.experimental && !liveRequested) {
                Logger::Info("[ActorBootstrap] forcing live actor bootstrap for experimental "
                             "profile %s / %s; canned actors are Resort-specific",
                             profile.mapUrl.c_str(), profile.gameClassPath.c_str());
            }
            SendLiveActorBootstrap(clientId);
            return;
        }
    }

    const std::vector<ActorBunchRecord>& records =
        GetActorBootstrapRecords(artifact);
    if (records.empty()) {
        return;
    }
    auto conn = GetConnection(clientId);
    if (!conn) {
        return;
    }
    ControlState& cs = GetControlState(clientId);
    // The captured Resort bootstrap opens retail NVA TeamInfo on ch76 and US
    // TeamInfo on ch56. h59 deltas must target the already-open team actor.
    cs.teamInfoChannels = {76u, 56u};

    // Capture frame 1484 orders the local PlayerController open first, then one NMT
    // 0x24 (payload int32 LE = 1; bytes 24 01 00 00 00), then the remaining actor
    // opens. Preserve that cross-channel order below; sending the state marker before
    // the owning PC reverses the official transition.
    static const std::vector<uint8_t> kPreActorNmt24 = {0x24, 0x01, 0x00, 0x00, 0x00};

    // actor_bootstrap.bin was extracted from a populated retail match. Replaying every
    // captured open makes the client instantiate that match's pawns and helicopters, but
    // this server never owns or updates them. The result is exactly what it looks like:
    // stale players drifting through the air and vehicles falling under client physics.
    //
    // The captured PC open is still required for HandleClientPlayer/local-PC adoption,
    // so keep only the menu-critical singleton/team/local-player actors by default. The
    // full captured world remains available as an explicit RE diagnostic, never as the
    // gameplay default.
    auto shouldReplay = [replayCapturedWorld](uint16_t chIndex) {
        if (replayCapturedWorld || chIndex == 0) return true;
        switch (chIndex) {
            case 2:   // owning ROPlayerController
            case 21:  // ROTeamInfo
            case 26:  // owning ROPlayerReplicationInfo
            case 54:  // ROGameReplicationInfo
            case 56:  // ROTeamInfo
            case 76:  // ROTeamInfo
                return true;
            default:
                return false;
        }
    };
    const size_t replayCount = static_cast<size_t>(std::count_if(
        records.begin(), records.end(),
        [&](const ActorBunchRecord& r) { return shouldReplay(r.chIndex); }));
    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 54 && shouldReplay(r.chIndex)) {
            cs.griChannel = r.chIndex;
            cs.griOutReliable = r.chSequence;
            break;
        }
    }

    Logger::Info("[ConnectionManager::SendActorBootstrap] client %u: opening local PC, NMT 0x24, then %zu/%zu bootstrap records (%s)",
                 clientId, replayCount, records.size(),
                 replayCapturedWorld ? "full captured-world RE replay" : "menu/local-player only");
    // BATCH the actor opens into MaxPacket-sized packets instead of one datagram each.
    // Sending 139 back-to-back single-bunch datagrams overflows the client's UDP receive
    // buffer (even on loopback) and intermittently drops the ch2 open, so the client's
    // PlayerController never binds (no menu). Packing ~10-14 opens per ~1500-byte packet
    // cuts the burst to ~12 packets and makes binding reliable - and matches how the real
    // server frames its open burst (multiple bunches per packet).
    std::vector<PacketCodec::Bunch> batch;
    size_t batchBits = 0;
    // Budget must keep each datagram UNDER the retail client's UDP receive buffer, or the
    // client drops the whole packet with WSAEMSGSIZE (10040) and the actors in it are NEVER
    // created. CLIENT LOG PROOF (DevNetTraffic): an ~1337-byte actor-open batch carrying
    // ch25-34 (incl. ch26 = the local PRI) was dropped -> "Created channel 24 ... recvfrom
    // error 10040 ... Created channel 35" (25-34 missing) -> the PC->PRI link could not
    // resolve ch26 -> role-select crash. The 1261-byte PackageMap chunks DO arrive, so the
    // client's buffer is ~1280; budget to 8192 bits (~1024 B) for safe margin. Reliable
    // retransmit resends the same batch, so an oversized packet fails forever - keep it small.
    constexpr size_t kBatchBitBudget = 8192;
    auto flushBatch = [&]() {
        if (batch.empty()) return;
        SendReliableBunches(clientId, batch);  // sent + recorded for retransmission
        batch.clear();
        batchBits = 0;
    };
    // Deliver the PlayerController channel (ch2) FIRST, in its own packet, before the
    // rest of the flood. The client adopts ch2 (NetPlayerIndex==0) as its LOCAL
    // PlayerController via HandleClientPlayer - and the team menu only opens when that
    // adoption succeeds (ShowTeamSelect's LocalPlayer(Player)!=none gate). Burying the
    // ch2 open in the middle of 138 other opens makes the adoption intermittent; giving
    // it a clean, standalone packet up front makes it reliable.
    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 2 && shouldReplay(r.chIndex)) {
            if (!r.bOpen || !r.bReliable || r.bClose) {
                Logger::Error(
                    "[ConnectionManager::SendActorBootstrap] client %u "
                    "captured ch2 record is not a reliable open; failing closed",
                    clientId);
                conn->MarkDisconnected();
                return;
            }
            PacketCodec::Bunch pcb;
            pcb.bControl = r.bControl; pcb.bOpen = r.bOpen; pcb.bClose = r.bClose;
            pcb.bReliable = r.bReliable; pcb.chIndex = r.chIndex; pcb.chType = r.chType;
            pcb.chSequence = r.chSequence; pcb.payload = r.payload;
            pcb.payloadBits = r.bunchDataBits;
            const auto adopted = cs.ch2Reliable.Adopt(pcb.chSequence);
            if (!adopted) {
                Logger::Error(
                    "[ConnectionManager::SendActorBootstrap] client %u could "
                    "not adopt captured ch2 reliable sequence %u (error=%u)",
                    clientId, pcb.chSequence,
                    static_cast<unsigned>(adopted.error()));
                conn->MarkDisconnected();
                return;
            }
            const size_t pendingBefore = cs.pendingReliable.size();
            (void)SendReliableBunches(
                clientId, {pcb}); // ch2 standalone, recorded for retransmit
            if (cs.pendingReliable.size() == pendingBefore) {
                Logger::Error(
                    "[ConnectionManager::SendActorBootstrap] client %u could "
                    "not queue the adopted ch2 open; failing closed",
                    clientId);
                conn->MarkDisconnected();
                return;
            }
            break;
        }
    }

    // Official f1484: ch2 OPEN precedes this control message.
    SendRawToClient(clientId, kPreActorNmt24);

    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 2) continue;   // already sent first, standalone
        if (!shouldReplay(r.chIndex)) continue;
        if (r.chIndex == 0) {
            // A ch0 control bunch in the burst rides the normal control path; flush the
            // pending actor batch first so ordering is preserved.
            flushBatch();
            SendRawToClient(clientId, r.payload);
            continue;
        }
        PacketCodec::Bunch b;
        b.bControl = r.bControl;
        b.bOpen = r.bOpen;
        b.bClose = r.bClose;
        b.bReliable = r.bReliable;
        b.chIndex = r.chIndex;
        b.chType = r.chType;
        b.chSequence = r.chSequence;
        b.payload = r.payload;
        b.payloadBits = r.bunchDataBits;  // exact bit count (not byte-padded)
        const size_t est = r.bunchDataBits + 64;  // payload + bunch-header allowance
        if (batchBits + est > kBatchBitBudget) {
            flushBatch();
        }
        batch.push_back(std::move(b));
        batchBits += est;
    }
    flushBatch();

    // The captured ch54 open contains a mid-match objective snapshot. Replace it
    // immediately with this map/session's authoritative slot mapping and state.
    SendRetailObjectiveState(clientId, /*baseline=*/true);

    // ---- Open the team-select menu: ClientShowTeamSelect() on ch2 (the PC) -----
    // The live retail client reaches the world but sits in spectator/preload with no
    // team. ROGameInfo.uc:2631 shows the real server calls ROPC.ClientShowTeamSelect()
    // on a fresh joiner to pop the team-select menu (the if-branch at 2621 is
    // ChangedTeams() for players who already have a team). ClientShowTeamSelect is a
    // `reliable client` function taking NO parameters, so the actor-channel bunch body
    // is exactly one field handle: SerializeInt(handle, maxHandle), nothing after.
    // ShowTeamSelect() is safe against our opens-only (empty) GRI: its server-only
    // guard (WorldInfo.Game!=none) is false on the client, it opens the scene from the
    // default TeamSelectSceneTemplate, and InitTeamSelect tolerates an empty
    // GRI.Teams (None.NumPlayers == 0 in UnrealScript). See ROPlayerController.uc:5818.
    //
    // handle / maxHandle from the compiled .u packages via UELib, sorted by the real
    // engine NetIndex (tools/netfields_from_u.ps1) - this is GROUND TRUTH, not the
    // decompiled .uc declaration order (which is reordered and gave a wrong handle).
    // Triple-confirmed: (1) NetIndex sort -> handle 206; (2) decoding the real-server
    // capture's 20571 S2C ch2 bunches with this map yields ZERO Server-function
    // mismatches; (3) SerializeInt(206,531) = bytes CE 00 = the exact `ce00` bunch the
    // official server sends at capture frame f1637 (its own ClientShowTeamSelect).
    constexpr uint32_t kROPlayerControllerMaxHandle = 531;
    constexpr uint32_t kClientShowTeamSelectHandle  = 206;

    const ActorBunchRecord* pcRec = nullptr;
    for (const ActorBunchRecord& r : records) {
        if (r.chIndex == 2) { pcRec = &r; break; }
    }
    if (pcRec) {
        cs.actorChType    = pcRec->chType;
        // Establish the local PC->PRI link (handle 23 -> ch26) so ROPC.PlayerReplicationInfo
        // is non-none before the role/unit-select UI ever opens. Unreliable, so send a few.
        SendLocalPriLink(clientId, 5);

        BitWriter fw;
        fw.SerializeInt(kClientShowTeamSelectHandle, kROPlayerControllerMaxHandle);
        const bool teamSelectQueued = SendCh2Rpc(
            clientId, fw.GetBytes(), static_cast<uint32_t>(fw.NumBits()),
            "ClientShowTeamSelect");

        // The official fresh-join sequence immediately follows h206 with
        // PlayerController.ClientGotoState (h41). Its two FName parameters are the
        // capture-verified 22-bit payload below (29 ce 1c); without this, the menu
        // scene can be requested while the PC remains in its pretransition state.
        static const std::vector<uint8_t> kClientGotoStatePayload = {0x29, 0xCE, 0x1C};
        const bool gotoStateQueued = teamSelectQueued && SendCh2Rpc(
            clientId, kClientGotoStatePayload, 22, "ClientGotoState");
        if (!gotoStateQueued) {
            Logger::Error(
                "[ConnectionManager::SendActorBootstrap] client %u could not "
                "queue the load-bearing team-select transition; failing closed",
                clientId);
            if (conn) conn->MarkDisconnected();
            return;
        }
    }

}

// Live per-session actor bootstrap (milestone 1): open the menu-critical actors -
// GameReplicationInfo, two TeamInfos, the owning client's PlayerController (ch2,
// NetPlayerIndex 0) and its PlayerReplicationInfo (ch26) - with THIS session's
// values, then pop the team-select menu. Every static class ref is selected from
// the same frozen canonical/installed ROGame layout as this session's PackageMap.
// maxHandle per class = the NetFieldTable maxIndex loaded at startup.
void ConnectionManager::SendLiveActorBootstrap(uint32_t clientId) {
    using ActorRepl::ActorOpenHeader;
    using ActorRepl::NetGUIDRef;
    using ActorRepl::MakeOpeningActorBunch;

    auto conn = GetConnection(clientId);
    if (!conn) return;
    ControlState& cs = GetControlState(clientId);

    const std::optional<RetailBootstrap::ArtifactSelection>& selectedArtifact =
        GetRetailArtifactSelection(clientId);
    if (!selectedArtifact) return;
    const RetailBootstrap::RoGameLayout& layout = selectedArtifact->roGame;
    if (layout.playerControllerClassRef == 0 ||
        layout.playerReplicationInfoClassRef == 0 ||
        layout.gameReplicationInfoClassRef == 0 ||
        layout.teamInfoClassRef == 0) {
        Logger::Error(
            "[ConnectionManager::SendLiveActorBootstrap] client %u has an "
            "incomplete ROGame actor layout; live bootstrap disabled",
            clientId);
        return;
    }

    const RetailBootstrap::Profile& bootstrapProfile =
        GetRetailBootstrapProfile(clientId);
    const std::optional<uint32_t> gameClassRef =
        RetailBootstrap::ResolveGameClassRef(
            *selectedArtifact, bootstrapProfile.gameClassPath);
    if (!gameClassRef) {
        Logger::Error(
            "[ConnectionManager::SendLiveActorBootstrap] client %u cannot "
            "resolve GameClass '%s' through variant='%.*s'; live bootstrap disabled",
            clientId, bootstrapProfile.gameClassPath.c_str(),
            static_cast<int>(selectedArtifact->variant.size()),
            selectedArtifact->variant.data());
        return;
    }

    const uint32_t kClsPC = layout.playerControllerClassRef;
    const uint32_t kClsPRI = layout.playerReplicationInfoClassRef;
    const uint32_t kClsGRI = layout.gameReplicationInfoClassRef;
    const uint32_t kClsTeam = layout.teamInfoClassRef;
    constexpr uint32_t kMaxGRI = 184, kMaxPC = 531, kMaxPRI = 98, kMaxTeam = 78;
    constexpr uint32_t kChGRI = 3, kChTeam0 = 4, kChTeam1 = 5, kChPC = 2, kChPRI = 26;
    cs.teamInfoChannels = {kChTeam0, kChTeam1};
    const uint32_t kGameClassIx = *gameClassRef; // GRI.GameClass h33
    uint8_t maxPlayers = 64;
    if (m_server) {
        if (const std::shared_ptr<ServerConfig> cfg = m_server->GetServerConfig()) {
            maxPlayers = static_cast<uint8_t>(std::clamp(cfg->GetMaxPlayers(), 1, 128));
        }
    }

    // The real f1484 ordering is owning-PC OPEN, then NMT 0x24, then other actors.
    static const std::vector<uint8_t> kPreActorNmt24 = {0x24, 0x01, 0x00, 0x00, 0x00};

    auto hdrFor = [](uint32_t cls, bool isPC) {
        ActorOpenHeader h;
        h.classRef = NetGUIDRef{false, cls};
        h.isPlayerController = isPC;
        h.netPlayerIndex = 0;          // owning client's PC (UnConn HandleClientPlayer)
        return h;
    };

    // Deliver the PlayerController (ch2) FIRST in its own reliable packet. The
    // header-only open is sufficient for ownership: once the class ref matches the
    // selected PackageMap, NetPlayerIndex=0 makes HandleClientPlayer bind it to
    // LocalPlayer_0. Retail Compound dogfood on 2026-07-14 confirmed SetPlayer plus
    // normal inbound h104/h37 traffic with no captured PC property tail. Keeping
    // the tail live/minimal avoids replaying Resort location and RPC parameters.
    {
        auto pc = MakeOpeningActorBunch(kChPC, 1, hdrFor(kClsPC, true), nullptr);
        const auto adopted = cs.ch2Reliable.Adopt(pc.chSequence);
        if (!adopted) {
            Logger::Error(
                "[ConnectionManager::SendLiveActorBootstrap] client %u could "
                "not adopt live ch2 reliable sequence %u (error=%u)",
                clientId, pc.chSequence,
                static_cast<unsigned>(adopted.error()));
            conn->MarkDisconnected();
            return;
        }
        cs.actorChType    = 2;
        const size_t pendingBefore = cs.pendingReliable.size();
        (void)SendReliableBunches(clientId, {pc});
        if (cs.pendingReliable.size() == pendingBefore) {
            Logger::Error(
                "[ConnectionManager::SendLiveActorBootstrap] client %u could "
                "not queue the adopted ch2 open; failing closed",
                clientId);
            conn->MarkDisconnected();
            return;
        }
    }

    SendRawToClient(clientId, kPreActorNmt24);

    // BATCH the remaining opens (GRI, TeamInfo x2, PRI) into ONE reliable packet.
    // Sending each as its own packet made all five retransmit independently until
    // acked, which the client saw as a rapid open/close churn (~7x). One batched
    // packet is acked once and the channels settle. (All five payloads are tiny, so
    // the datagram stays well under the client's receive buffer.)
    std::string name = conn->GetPlayerName();
    if (name.empty()) name = "Player" + std::to_string(clientId);
    const int32_t playerId = static_cast<int32_t>(clientId);

    const auto wireReinforcements = [this](uint8_t retailTeam) {
        uint32_t current = 0;
        uint32_t initial = 0;
        if (m_server) {
            if (const TicketSystem* tickets = m_server->GetTicketSystem()) {
                const uint32_t serverTeam =
                    TeamMapping::RetailToServer(retailTeam);
                current = tickets->GetTickets(serverTeam);
                initial = tickets->GetInitialTickets(serverTeam);
            }
        }
        return SpawnRepl::ResolveWireReinforcementCount(current, initial);
    };
    const int32_t team0Reinforcements =
        wireReinforcements(TeamMapping::kRetailNva);
    const int32_t team1Reinforcements =
        wireReinforcements(TeamMapping::kRetailUs);

    std::vector<PacketCodec::Bunch> batch;
    batch.push_back(MakeOpeningActorBunch(kChGRI, 1, hdrFor(kClsGRI, false), [&](BitWriter& w) {
        ActorRepl::WritePropObject(w, 33, kMaxGRI, NetGUIDRef{false, kGameClassIx});  // GameClass
        // ROPC.IsTeamFull uses bBalanceTeams and MaxTeamDifference, while
        // ClientShowTeamSelect initializes the population UI from MaxPlayers.
        // Their cooked zero defaults make both empty teams render as FULL on
        // the live bootstrap path, so publish the authoritative menu scalars.
        ActorRepl::WritePropBool(w, ObjectiveRepl::kBalanceTeams, kMaxGRI, true);
        ActorRepl::WritePropByte(w, ObjectiveRepl::kMaxTeamDifference, kMaxGRI, 2);
        ActorRepl::WritePropByte(w, ObjectiveRepl::kMaxPlayers, kMaxGRI, maxPlayers);
    }));
    batch.push_back(MakeOpeningActorBunch(kChTeam0, 1, hdrFor(kClsTeam, false), [&](BitWriter& w) {
        ActorRepl::WritePropInt(w, 23, kMaxTeam, 0);   // TeamIndex 0
        SpawnRepl::WriteReinforcementsRemaining(w, team0Reinforcements);
    }));
    batch.push_back(MakeOpeningActorBunch(kChTeam1, 1, hdrFor(kClsTeam, false), [&](BitWriter& w) {
        ActorRepl::WritePropInt(w, 23, kMaxTeam, 1);   // TeamIndex 1
        SpawnRepl::WriteReinforcementsRemaining(w, team1Reinforcements);
    }));
    batch.push_back(MakeOpeningActorBunch(kChPRI, 1, hdrFor(kClsPRI, false), [&](BitWriter& w) {
        ActorRepl::WritePropInt   (w, 36, kMaxPRI, playerId);   // PlayerID
        ActorRepl::WritePropString(w, 37, kMaxPRI, name);       // PlayerName
    }));
    SendReliableBunches(clientId, batch);
    cs.griChannel = kChGRI;
    cs.griOutReliable = 1;
    SendRetailObjectiveState(clientId, /*baseline=*/true);

    Logger::Info("[ConnectionManager::SendLiveActorBootstrap] client %u: opened live GRI(ch%u) "
                 "TeamInfo(ch%u,ch%u) PC(ch2) PRI(ch26); popping team-select",
                 clientId, kChGRI, kChTeam0, kChTeam1);

    // Pop the team-select menu on the now-owned ch2 (ClientShowTeamSelect, handle 206),
    // then assert the PC->PRI link so the role/unit-select UI has a non-none LocalPRI.
    {
        SendLocalPriLink(clientId, 5);

        BitWriter fw;
        fw.SerializeInt(206, kMaxPC);
        const bool teamSelectQueued = SendCh2Rpc(
            clientId, fw.GetBytes(), static_cast<uint32_t>(fw.NumBits()),
            "ClientShowTeamSelect");
        static const std::vector<uint8_t> kClientGotoStatePayload = {0x29, 0xCE, 0x1C};
        const bool gotoStateQueued = teamSelectQueued && SendCh2Rpc(
            clientId, kClientGotoStatePayload, 22, "ClientGotoState");
        if (!gotoStateQueued) {
            Logger::Error(
                "[ConnectionManager::SendLiveActorBootstrap] client %u could "
                "not queue the load-bearing team-select transition; failing closed",
                clientId);
            conn->MarkDisconnected();
            return;
        }
    }

}

void ConnectionManager::SendRetailObjectiveState(uint32_t clientId, bool baseline) {
    if (!m_server) return;
    ObjectiveSystem* objectives = m_server->GetObjectiveSystem();
    if (!objectives) return;

    ControlState& cs = GetControlState(clientId);
    if (cs.griChannel == 0) {
        Logger::Debug("[ObjectiveReplication] client %u has no open GRI actor channel; skipping",
                      clientId);
        return;
    }

    std::array<bool, 16> mapped{};
    std::array<uint8_t, 16> repIndex{};
    std::array<uint8_t, 16> connected{};
    std::array<uint8_t, 16> capProgress{};
    std::array<uint8_t, 16> forceRatio{};
    std::array<uint8_t, 16> status{};
    std::array<uint8_t, 16> cappersTeam0{};
    std::array<uint8_t, 16> cappersTeam1{};
    std::array<std::string, 16> objectiveNames{};
    uint8_t playerObjectiveSlot = 0xFF;
    repIndex.fill(0xFF);

    TeamManager* teams = m_server->GetTeamManager();
    const TerritoryMode* territory = m_server->GetTerritoryMode();
    const SupremacyMode* supremacy = m_server->GetSupremacyMode();
    const SkirmishMode* skirmish = m_server->GetSkirmishMode();
    const bool hasTerritoryRoles = territory != nullptr;
    const bool alliesAreAttacking = hasTerritoryRoles &&
        TeamMapping::ServerToRetail(territory->GetAttackingTeam()) ==
            TeamMapping::kRetailUs;
    const uint8_t retailDefender = hasTerritoryRoles
        ? TeamMapping::ServerToRetail(territory->GetDefendingTeam())
        : TeamMapping::kRetailNeutral;
    const bool territoryRolesChanged = hasTerritoryRoles &&
        (!cs.territoryRoleCacheValid ||
         alliesAreAttacking != cs.territoryAlliesAreAttacking ||
         retailDefender != cs.territoryDefendingTeam);
    const bool hasGriTimer = territory != nullptr || supremacy != nullptr || skirmish != nullptr;
    float griDurationSeconds = 0.0f;
    float griRemainingSeconds = 0.0f;
    uint16_t griPhaseKey = 0;
    if (territory) {
        griDurationSeconds = territory->GetPhaseDuration();
        griRemainingSeconds = territory->GetRoundTimeRemaining();
        griPhaseKey = static_cast<uint16_t>(
            0x100u | static_cast<uint8_t>(territory->GetPhase()));
    } else if (supremacy) {
        griDurationSeconds = supremacy->GetPhaseDuration();
        griRemainingSeconds = supremacy->GetPhaseTimeRemaining();
        griPhaseKey = static_cast<uint16_t>(
            0x200u | static_cast<uint8_t>(supremacy->GetPhase()));
    } else if (skirmish) {
        griDurationSeconds = skirmish->GetPhaseDuration();
        griRemainingSeconds = skirmish->GetPhaseTimeRemaining();
        griPhaseKey = static_cast<uint16_t>(
            0x300u | static_cast<uint8_t>(skirmish->GetPhase()));
    }
    const int32_t griTimeLimit = static_cast<int32_t>(
        std::ceil(std::max(0.0f, griDurationSeconds)));
    const int32_t griRemaining = static_cast<int32_t>(
        std::ceil(std::max(0.0f, griRemainingSeconds)));
    const int32_t griElapsed = std::max(0, griTimeLimit - griRemaining);
    const bool griTimeLimitChanged = hasGriTimer &&
        (!cs.griTimerCacheValid || griTimeLimit != cs.griTimeLimit);
    const bool griPhaseChanged = hasGriTimer &&
        (!cs.griTimerCacheValid || griPhaseKey != cs.griPhaseKey);
    // ROGameReplicationInfo's client-local Timer decrements RemainingTime every
    // second. The stock dedicated server dirties RemainingMinute every five
    // seconds to correct drift; a phase transition also needs an immediate sync.
    const bool griTimerSyncDue = hasGriTimer &&
        (!cs.griTimerCacheValid || griPhaseChanged || griTimeLimitChanged ||
         (griRemaining != cs.griRemainingSync && griRemaining % 5 == 0));
    size_t mappedCount = 0;
    for (const CaptureZone* zone : objectives->GetAllObjectives()) {
        if (!zone || zone->clientSlot >= 16 || zone->cookedRepIndex == 0xFF) continue;
        const uint8_t slot = zone->clientSlot;
        if (mapped[slot]) {
            Logger::Warn("[ObjectiveReplication] duplicate runtime objective slot %u; keeping first",
                         static_cast<unsigned>(slot));
            continue;
        }
        mapped[slot] = true;
        ++mappedCount;
        repIndex[slot] = zone->cookedRepIndex;
        objectiveNames[slot] = zone->name;
        connected[slot] = ResolveObjectiveConnectedToBase(
            zone->connectedToBase, supremacy, zone->id,
            zone->controllingTeam) ? 1u : 0u;
        capProgress[slot] = ObjectiveRepl::QuantizeProgress(zone->captureProgress);

        uint32_t team0Cappers = 0;
        uint32_t team1Cappers = 0;
        auto countCapper = [&](uint32_t playerId) {
            if (!teams) return;
            const uint32_t serverTeam = teams->GetPlayerTeam(playerId);
            const uint8_t retailTeam = TeamMapping::ServerToRetail(serverTeam);
            if (retailTeam == TeamMapping::kRetailNva) ++team0Cappers;
            else if (retailTeam == TeamMapping::kRetailUs) ++team1Cappers;
        };
        for (uint32_t id : zone->attackerIds) countCapper(id);
        for (uint32_t id : zone->defenderIds) countCapper(id);
        const float team0Strength = static_cast<float>(team0Cappers) +
            std::max(0.0f, zone->botCaptureWeightByTeam[2]); // retail NVA
        const float team1Strength = static_cast<float>(team1Cappers) +
            std::max(0.0f, zone->botCaptureWeightByTeam[1]); // retail US
        const uint32_t displayedTeam0 = static_cast<uint32_t>(std::clamp(
            std::lround(team0Strength), 0l, 255l));
        const uint32_t displayedTeam1 = static_cast<uint32_t>(std::clamp(
            std::lround(team1Strength), 0l, 255l));
        cappersTeam0[slot] = static_cast<uint8_t>(displayedTeam0);
        cappersTeam1[slot] = static_cast<uint8_t>(displayedTeam1);
        forceRatio[slot] = ObjectiveRepl::QuantizeForceRatio(
            displayedTeam0, displayedTeam1);

        const auto containsClient = [clientId](const std::vector<uint32_t>& ids) {
            return std::find(ids.begin(), ids.end(), clientId) != ids.end();
        };
        if (zone->isActive &&
            (containsClient(zone->attackerIds) || containsClient(zone->defenderIds)) &&
            (playerObjectiveSlot == 0xFF || slot > playerObjectiveSlot)) {
            playerObjectiveSlot = slot;
        }

        uint8_t cappingTeam = zone->cappingTeam;
        if (cappingTeam > 1) {
            if (team0Strength > team1Strength) cappingTeam = 0;
            else if (team1Strength > team0Strength) cappingTeam = 1;
        }
        const bool capping = zone->isActive && cappingTeam <= 1 &&
            (team0Strength + team1Strength) > 0.0f &&
            (zone->state == CaptureState::Capturing || zone->state == CaptureState::Contested);
        status[slot] = ObjectiveRepl::PackStatus(
            ObjectiveRepl::RetailOwner(zone->controllingTeam), capping,
            zone->isActive, cappingTeam, zone->enabled,
            /*satchel=*/false);
    }

    if (mappedCount == 0) {
        if (baseline) {
            Logger::Info("[ObjectiveReplication] client %u: map has no cooked objective mappings; "
                         "retail HUD baseline omitted", clientId);
        }
        return;
    }

    std::vector<ObjectiveRepl::ArrayElement> fields;
    fields.reserve(mappedCount * (baseline ? 6u : 4u));
    if (baseline) {
        // Rep-index mapping MUST precede status so ReplicatedEvent can resolve the
        // cooked ROObjective actor before the HUD consumes its state.
        for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
            fields.push_back({ObjectiveRepl::kRepIndices, slot, repIndex[slot]});
        }
        // Clear every captured-midmatch byte, including zero values. The canned
        // ch54 open otherwise leaves stale progress/force/satchel data behind.
        for (uint32_t handle : {ObjectiveRepl::kConnectedToBase,
                                ObjectiveRepl::kSatchelProgress,
                                ObjectiveRepl::kCapProgress,
                                ObjectiveRepl::kForceRatio,
                                ObjectiveRepl::kStatus}) {
            for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
                uint8_t value = 0;
                if (handle == ObjectiveRepl::kConnectedToBase) value = connected[slot];
                else if (handle == ObjectiveRepl::kCapProgress) value = capProgress[slot];
                else if (handle == ObjectiveRepl::kForceRatio) value = forceRatio[slot];
                else if (handle == ObjectiveRepl::kStatus) value = status[slot];
                fields.push_back({handle, slot, value});
            }
        }
    } else if (cs.objectiveCacheValid) {
        for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
            if (connected[slot] != cs.objectiveConnected[slot]) {
                fields.push_back({ObjectiveRepl::kConnectedToBase, slot, connected[slot]});
            }
        }
        for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
            if (capProgress[slot] != cs.objectiveCapProgress[slot]) {
                fields.push_back({ObjectiveRepl::kCapProgress, slot, capProgress[slot]});
            }
        }
        for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
            if (forceRatio[slot] != cs.objectiveForceRatio[slot]) {
                fields.push_back({ObjectiveRepl::kForceRatio, slot, forceRatio[slot]});
            }
        }
        for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
            if (status[slot] != cs.objectiveStatus[slot]) {
                fields.push_back({ObjectiveRepl::kStatus, slot, status[slot]});
            }
        }
    } else {
        // A connection should receive a baseline at actor-open time. If a caller
        // reaches us without one, self-heal rather than emit unmapped statuses.
        SendRetailObjectiveState(clientId, /*baseline=*/true);
        return;
    }

    std::vector<uint8_t> capperSlots;
    capperSlots.reserve(mappedCount);
    for (uint8_t slot = 0; slot < 16; ++slot) if (mapped[slot]) {
        if (baseline || cappersTeam0[slot] != cs.objectiveCappersTeam0[slot] ||
            cappersTeam1[slot] != cs.objectiveCappersTeam1[slot]) {
            capperSlots.push_back(slot);
        }
    }

    const bool pcObjectiveChanged = !cs.pcObjectiveCacheValid ||
                                    playerObjectiveSlot != cs.pcObjectiveSlot;
    BitWriter pcObjectiveWriter;
    if (pcObjectiveChanged) {
        if (playerObjectiveSlot != 0xFF) {
            // Retail CaptureTimer assigns the index before the name. The name's
            // RepNotify is what refreshes the owning player's objective widget.
            ObjectiveRepl::WriteObjectiveIndex(pcObjectiveWriter, playerObjectiveSlot);
            ObjectiveRepl::WriteObjectiveName(
                pcObjectiveWriter, objectiveNames[playerObjectiveSlot]);
        } else {
            // Leaving a zone clears only ObjectiveName; ObjectiveIndex retains
            // the last valid slot on the stock server.
            ObjectiveRepl::WriteObjectiveName(pcObjectiveWriter, "");
        }
    }

    BitWriter objectiveWriter;
    const bool retailMatchActive =
        GetDeploymentPhaseState().phase == DeploymentCountdown::Phase::Active;
    const bool publishActiveMatchState = retailMatchActive &&
        (baseline || !cs.griActiveStatePublished);
    if (publishActiveMatchState) {
        // Official active GRI f1489 starts with this exact order. h31's
        // RepNotify calls WorldInfo.NotifyMatchStarted; h32=false lets the
        // client's local GameReplicationInfo timer advance.
        ActorRepl::WritePropBool(objectiveWriter,
                                 ObjectiveRepl::kStopCountDown,
                                 ObjectiveRepl::kGriMaxHandle,
                                 false);
        ActorRepl::WritePropBool(objectiveWriter,
                                 ObjectiveRepl::kMatchHasBegun,
                                 ObjectiveRepl::kGriMaxHandle,
                                 true);
    }
    const std::array<int32_t, 2> supremacyPointsHeld = supremacy
        ? std::array<int32_t, 2>{
              supremacy->GetNorthConnectedObjectiveValue(),
              supremacy->GetSouthConnectedObjectiveValue()}
        : std::array<int32_t, 2>{};
    const int32_t supremacyCurrentScore = supremacy ? supremacy->GetScore() : 0;
    const int32_t supremacyTargetScore = supremacy ? supremacy->GetScoreTarget() : 0;
    SkirmishMode::RetailState skirmishState;
    if (skirmish) skirmishState = skirmish->GetRetailState();
    const auto writeSupremacyState = [&](bool force) {
        if (!supremacy) return;
        for (uint8_t retailTeam = 0; retailTeam < supremacyPointsHeld.size(); ++retailTeam) {
            if (force || !cs.supremacyCacheValid ||
                supremacyPointsHeld[retailTeam] != cs.supremacyPointsHeld[retailTeam]) {
                ObjectiveRepl::WriteIntArrayElement(
                    objectiveWriter, ObjectiveRepl::kSuPointsHeld,
                    retailTeam, supremacyPointsHeld[retailTeam]);
            }
        }
        if (force || !cs.supremacyCacheValid ||
            supremacyCurrentScore != cs.supremacyCurrentScore) {
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kSuCurrentScore,
                                    ObjectiveRepl::kGriMaxHandle,
                                    supremacyCurrentScore);
        }
        if (force || !cs.supremacyCacheValid ||
            supremacyTargetScore != cs.supremacyTargetScore) {
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kSuTargetScore,
                                    ObjectiveRepl::kGriMaxHandle,
                                    supremacyTargetScore);
        }
    };
    const auto writeSkirmishState = [&](bool force) {
        if (!skirmish) return;
        for (uint8_t slot = 0; slot < skirmishState.allSpawnWindows.size(); ++slot) {
            if (force || !cs.skirmishCacheValid ||
                skirmishState.allSpawnWindows[slot] != cs.skirmishSpawnWindows[slot]) {
                ObjectiveRepl::WriteIntArrayElement(
                    objectiveWriter, ObjectiveRepl::kAllSpawnWindows,
                    slot, skirmishState.allSpawnWindows[slot]);
            }
        }
        for (uint8_t retailTeam = 0;
             retailTeam < skirmishState.spawnWindowCloseTime.size(); ++retailTeam) {
            if (force || !cs.skirmishCacheValid ||
                skirmishState.spawnWindowCloseTime[retailTeam] !=
                    cs.skirmishSpawnWindowCloseTime[retailTeam]) {
                ObjectiveRepl::WriteIntArrayElement(
                    objectiveWriter, ObjectiveRepl::kSpawnWindowCloseTime,
                    retailTeam, skirmishState.spawnWindowCloseTime[retailTeam]);
            }
        }
        const auto writeIntIfChanged = [&](uint32_t handle, int32_t value,
                                           int32_t cached) {
            if (force || !cs.skirmishCacheValid || value != cached) {
                ActorRepl::WritePropInt(objectiveWriter, handle,
                                        ObjectiveRepl::kGriMaxHandle, value);
            }
        };
        writeIntIfChanged(ObjectiveRepl::kPlayedRoundsCount,
                          skirmishState.playedRoundsCount,
                          cs.skirmishPlayedRounds);
        writeIntIfChanged(ObjectiveRepl::kRoundTeamScoreLimit,
                          skirmishState.roundTeamScoreLimit,
                          cs.skirmishRoundScoreLimit);
        writeIntIfChanged(ObjectiveRepl::kRoundLimit,
                          skirmishState.roundLimit,
                          cs.skirmishRoundLimit);
        writeIntIfChanged(ObjectiveRepl::kNextLockDownTime,
                          skirmishState.nextLockDownTime,
                          cs.skirmishNextLockdownTime);
        if (force || !cs.skirmishCacheValid ||
            skirmishState.suddenDeath != cs.skirmishSuddenDeath) {
            ActorRepl::WritePropBool(objectiveWriter,
                                     ObjectiveRepl::kSuddenDeath,
                                     ObjectiveRepl::kGriMaxHandle,
                                     skirmishState.suddenDeath);
        }
        if (force || !cs.skirmishCacheValid ||
            skirmishState.overTime != cs.skirmishOvertime) {
            ActorRepl::WritePropBool(objectiveWriter,
                                     ObjectiveRepl::kOverTime,
                                     ObjectiveRepl::kGriMaxHandle,
                                     skirmishState.overTime);
        }
        if (force || !cs.skirmishCacheValid ||
            skirmishState.teamWithOvertimeAdvantage !=
                cs.skirmishOvertimeAdvantage) {
            ActorRepl::WritePropByte(objectiveWriter,
                                     ObjectiveRepl::kOvertimeAdvantage,
                                     ObjectiveRepl::kGriMaxHandle,
                                     skirmishState.teamWithOvertimeAdvantage);
        }
        for (uint8_t retailTeam = 0;
             retailTeam < skirmishState.playersAliveCount.size(); ++retailTeam) {
            if (force || !cs.skirmishCacheValid ||
                skirmishState.playersAliveCount[retailTeam] !=
                    cs.skirmishPlayersAlive[retailTeam]) {
                ObjectiveRepl::WriteArrayElement(
                    objectiveWriter, ObjectiveRepl::kPlayersAliveCount,
                    retailTeam, skirmishState.playersAliveCount[retailTeam]);
            }
        }
    };
    if (baseline) {
        // The captured GRI actor open contains the official session's timer
        // snapshot. Replace it immediately with this server's phase clock.
        // h25/h29/h28 are the exact bNetInitial scalar set in
        // Engine.GameReplicationInfo; later corrections use h27 below.
        if (hasGriTimer) {
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kTimeLimit,
                                    ObjectiveRepl::kGriMaxHandle,
                                    griTimeLimit);
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kRemainingTime,
                                    ObjectiveRepl::kGriMaxHandle,
                                    griRemaining);
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kElapsedTime,
                                    ObjectiveRepl::kGriMaxHandle,
                                    griElapsed);
        }

        writeSupremacyState(/*force=*/true);
        writeSkirmishState(/*force=*/true);

        // The captured GRI open comes from a mid-match snapshot. Explicitly
        // clear its bDisableObjectiveOverview bit before mapping/status fields
        // so a stale capture flag cannot suppress the retail objective HUD.
        ActorRepl::WritePropBool(objectiveWriter,
                                 ObjectiveRepl::kDisableObjectiveOverview,
                                 ObjectiveRepl::kGriMaxHandle,
                                 false);

        // TeamManager intentionally uses the inverse numeric ids from retail:
        // server 1=US/Allies and 2=NVA/Axis, while RO/UE3 uses 1=US and 0=NVA.
        // Replicate both GRI team-role fields at the same boundary as objective
        // owner/capper state so the HUD never interprets Beach as US-owned.
        if (hasTerritoryRoles) {
            ActorRepl::WritePropBool(objectiveWriter,
                                     ObjectiveRepl::kAlliesAreAttacking,
                                     ObjectiveRepl::kGriMaxHandle,
                                     alliesAreAttacking);
            ActorRepl::WritePropByte(objectiveWriter,
                                     ObjectiveRepl::kDefendingTeam,
                                     ObjectiveRepl::kGriMaxHandle,
                                     retailDefender);
        }

        // Mappings must exist before capper/status state refers to the slots.
        for (const ObjectiveRepl::ArrayElement& field : fields) {
            if (field.handle == ObjectiveRepl::kRepIndices) {
                ObjectiveRepl::WriteArrayElement(objectiveWriter, field.handle,
                                                 field.slot, field.value);
            }
        }
    } else {
        if (griTimeLimitChanged) {
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kTimeLimit,
                                    ObjectiveRepl::kGriMaxHandle,
                                    griTimeLimit);
        }
        if (griTimerSyncDue) {
            ActorRepl::WritePropInt(objectiveWriter,
                                    ObjectiveRepl::kRemainingMinute,
                                    ObjectiveRepl::kGriMaxHandle,
                                    griRemaining);
        }
        writeSupremacyState(/*force=*/false);
        writeSkirmishState(/*force=*/false);
        if (territoryRolesChanged) {
            // Existing clients need the role flip at halftime too; the objective
            // cache alone does not dirty these scalar GRI properties.
            ActorRepl::WritePropBool(objectiveWriter,
                                     ObjectiveRepl::kAlliesAreAttacking,
                                     ObjectiveRepl::kGriMaxHandle,
                                     alliesAreAttacking);
            ActorRepl::WritePropByte(objectiveWriter,
                                     ObjectiveRepl::kDefendingTeam,
                                     ObjectiveRepl::kGriMaxHandle,
                                     retailDefender);
        }
    }
    // ObjCappers is a 4-byte struct array with its own 47-bit wire layout.
    // Emit it before progress/status dirty fields like the retail CaptureTimer.
    for (uint8_t slot : capperSlots) {
        ObjectiveRepl::WriteCappersElement(
            objectiveWriter, slot, cappersTeam0[slot], cappersTeam1[slot]);
    }
    for (const ObjectiveRepl::ArrayElement& field : fields) {
        if (!baseline || field.handle != ObjectiveRepl::kRepIndices) {
            ObjectiveRepl::WriteArrayElement(objectiveWriter, field.handle,
                                             field.slot, field.value);
        }
    }

    cs.objectiveRepIndex = repIndex;
    cs.objectiveConnected = connected;
    cs.objectiveCapProgress = capProgress;
    cs.objectiveForceRatio = forceRatio;
    cs.objectiveStatus = status;
    cs.objectiveCappersTeam0 = cappersTeam0;
    cs.objectiveCappersTeam1 = cappersTeam1;
    cs.objectiveCacheValid = true;
    if (hasTerritoryRoles) {
        cs.territoryAlliesAreAttacking = alliesAreAttacking;
        cs.territoryDefendingTeam = retailDefender;
        cs.territoryRoleCacheValid = true;
    } else {
        cs.territoryRoleCacheValid = false;
    }
    if (hasGriTimer) {
        cs.griPhaseKey = griPhaseKey;
        cs.griTimeLimit = griTimeLimit;
        cs.griElapsedTime = griElapsed;
        if (baseline || griTimerSyncDue) {
            cs.griRemainingSync = griRemaining;
        }
        cs.griTimerCacheValid = true;
    } else {
        cs.griTimerCacheValid = false;
    }
    if (supremacy) {
        cs.supremacyPointsHeld = supremacyPointsHeld;
        cs.supremacyCurrentScore = supremacyCurrentScore;
        cs.supremacyTargetScore = supremacyTargetScore;
        cs.supremacyCacheValid = true;
    } else {
        cs.supremacyCacheValid = false;
    }
    if (skirmish) {
        cs.skirmishSpawnWindows = skirmishState.allSpawnWindows;
        cs.skirmishSpawnWindowCloseTime = skirmishState.spawnWindowCloseTime;
        cs.skirmishPlayedRounds = skirmishState.playedRoundsCount;
        cs.skirmishRoundScoreLimit = skirmishState.roundTeamScoreLimit;
        cs.skirmishRoundLimit = skirmishState.roundLimit;
        cs.skirmishNextLockdownTime = skirmishState.nextLockDownTime;
        cs.skirmishSuddenDeath = skirmishState.suddenDeath;
        cs.skirmishOvertime = skirmishState.overTime;
        cs.skirmishOvertimeAdvantage = skirmishState.teamWithOvertimeAdvantage;
        cs.skirmishPlayersAlive = skirmishState.playersAliveCount;
        cs.skirmishCacheValid = true;
    } else {
        cs.skirmishCacheValid = false;
    }

    std::vector<PacketCodec::Bunch> bunches;
    const uint32_t objectiveBits = static_cast<uint32_t>(objectiveWriter.NumBits());
    if (objectiveBits > 0) {
        PacketCodec::Bunch griBunch;
        griBunch.bReliable = baseline;
        griBunch.chIndex = cs.griChannel;
        griBunch.chType = 2; // CHTYPE_Actor (serialized for the reliable baseline)
        griBunch.chSequence = baseline ? ++cs.griOutReliable : 0;
        griBunch.payload = objectiveWriter.GetBytes();
        griBunch.payloadBits = objectiveBits;
        bunches.push_back(std::move(griBunch));
    }

    const uint32_t pcObjectiveBits = static_cast<uint32_t>(pcObjectiveWriter.NumBits());
    if (pcObjectiveBits > 0) {
        PacketCodec::Bunch pcBunch;
        pcBunch.bReliable = false; // capture-matched property delta
        pcBunch.chIndex = 2;
        pcBunch.chType = cs.actorChType;
        pcBunch.chSequence = 0;
        pcBunch.payload = pcObjectiveWriter.GetBytes();
        pcBunch.payloadBits = pcObjectiveBits;
        bunches.push_back(std::move(pcBunch));
    }

    // Keep the initial PC cache invalid so the first regular CaptureTimer-style
    // update repeats the unreliable context after the reliable actor baseline.
    if (!baseline && pcObjectiveChanged) {
        cs.pcObjectiveSlot = playerObjectiveSlot;
        cs.pcObjectiveCacheValid = true;
    }
    if (bunches.empty()) return;
    SendReliableBunches(clientId, bunches);
    if (publishActiveMatchState) {
        cs.griActiveStatePublished = true;
    }

    const char* const updateKind = baseline ? "baseline" : "dirty delta";
    const char* const rolesState =
        (baseline && hasTerritoryRoles) || territoryRolesChanged ? "sent" : "cached";
    const char* const timerState =
        (baseline && hasGriTimer) || griTimerSyncDue ? "sent" : "cached";
    if (baseline) {
        Logger::Info("[ObjectiveReplication] client %u: sent %s on GRI ch%u "
                     "(%zu byte fields, %zu capper structs, roles=%s, timer=%s, "
                     "%u GRI bits, %u PC bits)",
                     clientId, updateKind, cs.griChannel, fields.size(),
                     capperSlots.size(), rolesState, timerState,
                     objectiveBits, pcObjectiveBits);
    } else {
        // Capture progress is intentionally quantized and published at the game
        // tick cadence for a smooth HUD.  Keep that high-frequency trace at
        // debug level so normal server logs remain operationally useful.
        Logger::Debug("[ObjectiveReplication] client %u: sent %s on GRI ch%u "
                      "(%zu byte fields, %zu capper structs, roles=%s, timer=%s, "
                      "%u GRI bits, %u PC bits)",
                      clientId, updateKind, cs.griChannel, fields.size(),
                      capperSlots.size(), rolesState, timerState,
                      objectiveBits, pcObjectiveBits);
    }
}

// PlayerController (ROPlayerController) ClassNetCache maxHandle - see SendActorBootstrap.
static constexpr uint32_t kRoPcMaxHandle = 531;

static uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

ConnectionManager::PossessionRecoveryDecision
ConnectionManager::EvaluatePossessionRecovery(
    ControlState& state, bool exactStandaloneRequest,
    uint64_t expectedPawnGeneration, uint64_t nowMs) {
    if (!exactStandaloneRequest) {
        return PossessionRecoveryDecision::Malformed;
    }
    if (!state.spawned || !state.pawnGraphOpen || !state.owningPawnAlive) {
        return PossessionRecoveryDecision::Ineligible;
    }
    if (expectedPawnGeneration == 0u ||
        state.owningPawnGeneration != expectedPawnGeneration ||
        state.pawnGraphGeneration != expectedPawnGeneration ||
        state.possessionRecoveryGeneration != expectedPawnGeneration) {
        return PossessionRecoveryDecision::StaleGeneration;
    }
    if (state.possessionAckedGeneration == expectedPawnGeneration) {
        return PossessionRecoveryDecision::Ineligible;
    }
    // SendGivePawn allocates exactly three reliable ch2 bunches. Allocation
    // backpressure is transient: an ACK may free the window, so it must not
    // consume or permanently suppress this generation's recovery budget.
    if (!state.ch2Reliable.IsInitialized() ||
        state.ch2Reliable.AvailableCapacity() < 3u) {
        return PossessionRecoveryDecision::Backpressured;
    }
    if (state.possessionRecoveryResponses >=
        kMaxPossessionRecoveryResponses) {
        if (!state.possessionRecoveryLimitLogged) {
            state.possessionRecoveryLimitLogged = true;
            return PossessionRecoveryDecision::LimitReached;
        }
        return PossessionRecoveryDecision::Suppressed;
    }
    if (state.possessionRecoveryResponses != 0u &&
        (nowMs < state.lastPossessionRecoveryResponseMs ||
         nowMs - state.lastPossessionRecoveryResponseMs <
             kPossessionRecoveryIntervalMs)) {
        return PossessionRecoveryDecision::RateLimited;
    }

    return PossessionRecoveryDecision::Respond;
}

bool ConnectionManager::CommitPossessionRecoveryResponse(
    ControlState& state, uint64_t expectedPawnGeneration, uint64_t nowMs) {
    if (!HasLiveOwningPawnGeneration(state, expectedPawnGeneration) ||
        !state.spawned ||
        state.possessionAckedGeneration == expectedPawnGeneration ||
        state.possessionRecoveryResponses >=
            kMaxPossessionRecoveryResponses) {
        return false;
    }
    ++state.possessionRecoveryResponses;
    state.lastPossessionRecoveryResponseMs = nowMs;
    return true;
}

uint64_t ConnectionManager::AdvanceOwningPawnGeneration(ControlState& state) {
    if (state.owningPawnGeneration ==
        std::numeric_limits<uint64_t>::max()) {
        state.owningPawnGeneration = 1u;
    } else {
        ++state.owningPawnGeneration;
        if (state.owningPawnGeneration == 0u) {
            state.owningPawnGeneration = 1u;
        }
    }
    return state.owningPawnGeneration;
}

uint64_t ConnectionManager::AnticipatedOwningPawnGeneration(
    const ControlState& state) noexcept {
    if (state.owningPawnAlive && state.owningPawnGeneration != 0u) {
        return state.owningPawnGeneration;
    }
    return state.owningPawnGeneration ==
            std::numeric_limits<uint64_t>::max()
        ? 1u
        : std::max<uint64_t>(1u, state.owningPawnGeneration + 1u);
}

bool ConnectionManager::HasLiveOwningPawnGeneration(
    const ControlState& state, uint64_t expectedPawnGeneration) {
    return expectedPawnGeneration != 0u && state.owningPawnAlive &&
           state.pawnGraphOpen &&
           state.owningPawnGeneration == expectedPawnGeneration &&
           state.pawnGraphGeneration == expectedPawnGeneration &&
           state.possessionRecoveryGeneration == expectedPawnGeneration;
}

void ConnectionManager::BindPossessionRecovery(
    ControlState& state, uint64_t pawnGeneration) {
    state.pawnGraphGeneration = pawnGeneration;
    state.possessionAckedGeneration = 0u;
    state.possessionRecoveryGeneration = pawnGeneration;
    ResetPossessionRecovery(state);
}

void ConnectionManager::InvalidatePossessionRecovery(ControlState& state) {
    state.pawnGraphGeneration = 0u;
    state.possessionAckedGeneration = 0u;
    state.possessionRecoveryGeneration = 0u;
    ResetPossessionRecovery(state);
}

void ConnectionManager::ResetPossessionRecovery(ControlState& state) {
    state.possessionRecoveryResponses = 0;
    state.lastPossessionRecoveryResponseMs = 0;
    state.possessionRecoveryLimitLogged = false;
}

std::optional<size_t> ConnectionManager::OwningPawnGraphChannelIndex(
    uint32_t channel) {
    const auto found = std::find(
        kOwningPawnGraphChannels.begin(), kOwningPawnGraphChannels.end(),
        channel);
    if (found == kOwningPawnGraphChannels.end()) return std::nullopt;
    return static_cast<size_t>(
        std::distance(kOwningPawnGraphChannels.begin(), found));
}

PacketCodec::OutboundReliableSequencer*
ConnectionManager::OwningPawnGraphSequencer(ControlState& state,
                                             uint32_t channel) {
    const auto index = OwningPawnGraphChannelIndex(channel);
    return index ? &state.owningPawnGraphReliable[*index] : nullptr;
}

const PacketCodec::OutboundReliableSequencer*
ConnectionManager::OwningPawnGraphSequencer(const ControlState& state,
                                             uint32_t channel) {
    const auto index = OwningPawnGraphChannelIndex(channel);
    return index ? &state.owningPawnGraphReliable[*index] : nullptr;
}

void ConnectionManager::FailOwningPawnGraph(uint32_t clientId,
                                             const char* context) {
    ControlState& state = GetControlState(clientId);
    state.pawnGraphPhase = OwningPawnGraphPhase::Broken;
    state.pawnGraphOpen = false;
    state.spawned = false;
    state.owningPawnAlive = false;
    state.deferredOwningPawnGraphDeployment.reset();
    state.activeWeaponChannel = 0u;
    state.weaponIntent.fill({});
    state.movementInputValid = false;
    state.useHeld = false;
    state.mantleAttemptPending = false;
    state.mantlePawnStarted = false;
    state.specialMoveActive = false;
    state.specialMove = 0u;
    InvalidatePossessionRecovery(state);
    if (m_server) m_server->CancelRetailGrenadeCook(clientId);
    const std::shared_ptr<ClientConnection> connection =
        GetConnection(clientId);
    if (connection && !connection->IsDisconnected()) {
        connection->MarkDisconnected();
    }
    Logger::Error(
        "[OwningPawnGraph] client %u fail-closed after %s",
        clientId, context ? context : "an inconsistent graph transition");
}

bool ConnectionManager::EnsureOwningPawnGraphSequencers(
    uint32_t clientId, ControlState& state) {
    for (size_t index = 0; index < kOwningPawnGraphChannels.size(); ++index) {
        auto& sequencer = state.owningPawnGraphReliable[index];
        if (sequencer.IsInitialized()) continue;
        const auto seeded = sequencer.Seed(0u);
        if (!seeded) {
            Logger::Error(
                "[OwningPawnGraph] client %u could not seed ch%u reliable "
                "cursor (error=%u)",
                clientId, kOwningPawnGraphChannels[index],
                static_cast<unsigned>(seeded.error()));
            FailOwningPawnGraph(clientId, "fixed-channel cursor seed");
            return false;
        }
    }
    return true;
}

bool ConnectionManager::QueueOwningPawnGraphClose(uint32_t clientId) {
    ControlState& state = GetControlState(clientId);
    if (state.pawnGraphPhase != OwningPawnGraphPhase::Open ||
        !state.pawnGraphOpen ||
        state.owningPawnGraphActiveChannels.none() ||
        !EnsureOwningPawnGraphSequencers(clientId, state)) {
        return false;
    }

    using Reservation =
        PacketCodec::OutboundReliableSequencer::Reservation;
    struct ReservedClose {
        uint32_t channel = 0;
        Reservation reservation;
    };
    std::vector<ReservedClose> reserved;
    reserved.reserve(kOwningPawnGraphChannels.size());

    // Close inventory leaves before their manager and pawn. This deterministic
    // whole-graph barrier is an emulator safety policy, not a captured retail
    // faction-switch order.
    static constexpr std::array<uint32_t, 7> kCloseOrder{
        210u, 211u, 212u, 213u, 214u, 219u, 209u};
    for (const uint32_t channel : kCloseOrder) {
        if (!state.owningPawnGraphActiveChannels.test(channel)) continue;
        auto* sequencer = OwningPawnGraphSequencer(state, channel);
        auto reservation = sequencer
            ? sequencer->ReserveBatch(1u)
            : PacketCodec::OutboundReliableSequencer::ReservationResult(
                  std::unexpected(
                      PacketCodec::OutboundReliableSequenceError::
                          Uninitialized));
        if (!reservation) {
            for (auto prior = reserved.rbegin(); prior != reserved.rend();
                 ++prior) {
                if (auto* priorSequencer =
                        OwningPawnGraphSequencer(state, prior->channel)) {
                    (void)priorSequencer->CancelBatch(prior->reservation);
                }
            }
            Logger::Warn(
                "[OwningPawnGraph] client %u could not reserve close on ch%u "
                "(error=%u)",
                clientId, channel,
                static_cast<unsigned>(reservation.error()));
            return false;
        }
        reserved.push_back({channel, std::move(*reservation)});
    }
    if (reserved.empty()) return false;

    std::vector<PacketCodec::Bunch> closes;
    closes.reserve(reserved.size());
    for (const ReservedClose& item : reserved) {
        PacketCodec::Bunch close;
        close.bControl = true;
        close.bClose = true;
        close.bReliable = true;
        close.chIndex = item.channel;
        close.chType = 2u;
        close.chSequence = item.reservation.front();
        closes.push_back(std::move(close));
    }

    const size_t pendingBefore = state.pendingReliable.size();
    (void)SendReliableBunches(clientId, closes);
    if (state.pendingReliable.size() == pendingBefore) {
        for (auto item = reserved.rbegin(); item != reserved.rend(); ++item) {
            if (auto* sequencer =
                    OwningPawnGraphSequencer(state, item->channel)) {
                (void)sequencer->CancelBatch(item->reservation);
            }
        }
        return false;
    }

    for (const ReservedClose& item : reserved) {
        auto* sequencer = OwningPawnGraphSequencer(state, item.channel);
        const auto committed = sequencer
            ? sequencer->CommitBatch(item.reservation)
            : PacketCodec::OutboundReliableSequencer::MutationResult(
                  std::unexpected(
                      PacketCodec::OutboundReliableSequenceError::
                          Uninitialized));
        if (!committed) {
            Logger::Error(
                "[OwningPawnGraph] client %u queued close ch%u seq%u but "
                "could not commit it (error=%u)",
                clientId, item.channel, item.reservation.front(),
                static_cast<unsigned>(committed.error()));
            FailOwningPawnGraph(clientId,
                                "published close reservation commit");
            return false;
        }
    }

    state.pawnGraphPhase = OwningPawnGraphPhase::Closing;
    state.pawnGraphOpen = false;
    state.owningPawnGraphClosingChannels =
        state.owningPawnGraphActiveChannels;
    state.owningPawnGraphCloseAcknowledged.reset();
    InvalidatePossessionRecovery(state);
    Logger::Info(
        "[OwningPawnGraph] client %u queued %zu empty reliable close(s) "
        "for team %u graph",
        clientId, closes.size(), state.pawnGraphTeamId);
    return true;
}

ConnectionManager::OwningPawnGraphGateResult
ConnectionManager::GateOwningPawnGraphForDeployment(
    uint32_t clientId, uint32_t teamId, uint32_t spawnId) {
    ControlState& state = GetControlState(clientId);
    if (!TeamMapping::IsPlayableServerTeam(teamId) || spawnId == 0u) {
        return OwningPawnGraphGateResult::Failed;
    }

    switch (state.pawnGraphPhase) {
        case OwningPawnGraphPhase::Unopened:
        case OwningPawnGraphPhase::Closed:
            state.deferredOwningPawnGraphDeployment.reset();
            return OwningPawnGraphGateResult::Ready;
        case OwningPawnGraphPhase::Open:
            if (state.pawnGraphOpen && state.pawnGraphTeamId == teamId) {
                state.deferredOwningPawnGraphDeployment.reset();
                return OwningPawnGraphGateResult::Ready;
            }
            if (!QueueOwningPawnGraphClose(clientId)) {
                FailOwningPawnGraph(clientId,
                                    "opposite-faction close publication");
                return OwningPawnGraphGateResult::Failed;
            }
            break;
        case OwningPawnGraphPhase::Closing:
            break;
        case OwningPawnGraphPhase::Broken:
            return OwningPawnGraphGateResult::Failed;
    }

    state.deferredOwningPawnGraphDeployment =
        DeferredOwningPawnGraphDeployment{
            m_deploymentGeneration, spawnId, teamId};
    return OwningPawnGraphGateResult::Deferred;
}

void ConnectionManager::CompleteOwningPawnGraphClose(
    uint32_t clientId,
    std::optional<uint32_t> inboundBarrierPacketId) {
    auto stateIt = m_controlState.find(clientId);
    if (stateIt == m_controlState.end()) return;
    ControlState& state = stateIt->second;
    if (state.pawnGraphPhase != OwningPawnGraphPhase::Closing) return;

    for (const uint32_t channel : kOwningPawnGraphChannels) {
        if (!state.owningPawnGraphClosingChannels.test(channel) ||
            !state.owningPawnGraphCloseAcknowledged.test(channel)) {
            continue;
        }
        const bool channelStillPending = std::any_of(
            state.pendingReliable.begin(), state.pendingReliable.end(),
            [channel](const ControlState::SentReliable& reliable) {
                return std::any_of(
                    reliable.bunches.begin(), reliable.bunches.end(),
                    [channel](const PacketCodec::Bunch& bunch) {
                        return bunch.bReliable &&
                               bunch.chIndex == channel;
                    });
            });
        const auto* sequencer = OwningPawnGraphSequencer(state, channel);
        if (channelStillPending || !sequencer ||
            sequencer->OutstandingCount() != 0u ||
            sequencer->IssuanceWindowSize() != 0u) {
            continue;
        }

        state.outboundActorChannels.reset(channel);
        state.owningPawnGraphActiveChannels.reset(channel);
        state.owningPawnGraphClosingChannels.reset(channel);
        state.owningPawnGraphCloseAcknowledged.reset(channel);
    }
    if (state.owningPawnGraphClosingChannels.any()) return;

    // Packet ACKs may already have stopped retransmission of old reliable RPCs
    // that were buffered behind a missing sequence. Retire the entire known
    // old range before opening another actor incarnation; delayed missing
    // predecessors are then stale, while later old packets are semantically
    // suppressed as they advance the persistent receive cursor.
    for (size_t index = 0;
         index < kOwningPawnGraphChannels.size(); ++index) {
        const uint32_t channel = kOwningPawnGraphChannels[index];
        const size_t retired =
            state.actorReliableInbound.RetirePending(channel);
        state.owningPawnGraphSuppressedInboundReliable[index].reset();
        if (retired != 0u) {
            Logger::Info(
                "[OwningPawnGraph] client %u retired %zu buffered old "
                "inbound reliable(s) on ch%u at close boundary",
                clientId, retired, channel);
        }
    }

    state.pawnGraphPhase = OwningPawnGraphPhase::Closed;
    state.pawnGraphOpen = false;
    state.pawnGraphTeamId = 0u;
    state.owningPawnGraphActiveChannels.reset();
    InvalidatePossessionRecovery(state);
    if (inboundBarrierPacketId &&
        *inboundBarrierPacketId < static_cast<uint32_t>(kMaxPacketId)) {
        state.owningPawnGraphInboundPacketFloorValid = true;
        state.owningPawnGraphInboundPacketFloor =
            *inboundBarrierPacketId;
    }

    const auto deferred = state.deferredOwningPawnGraphDeployment;
    state.deferredOwningPawnGraphDeployment.reset();
    Logger::Info(
        "[OwningPawnGraph] client %u close cohort fully ACKed/drained",
        clientId);
    if (!deferred ||
        deferred->deploymentGeneration != m_deploymentGeneration ||
        state.mapTravelPending || state.spawned) {
        return;
    }

    const auto connection = GetConnection(clientId);
    const auto deployment = m_deploymentCoordinator.GetClientState(clientId);
    const TeamManager* teams = m_server ? m_server->GetTeamManager() : nullptr;
    if (!connection || connection->IsDisconnected() || !deployment ||
        !deployment->deploymentAuthorized ||
        deployment->generation != deferred->deploymentGeneration ||
        deployment->selectedSpawnId !=
            std::optional<uint32_t>{deferred->spawnId} ||
        !teams || teams->GetPlayerTeam(clientId) != deferred->teamId) {
        return;
    }

    (void)ExecutePreparedDeployment(clientId, deferred->spawnId);
}

void ConnectionManager::TransportKeepAliveTick() {
    const uint64_t now = NowMs();
    constexpr uint64_t kKeepAliveMs = 1000;

    for (auto& entry : m_controlState) {
        const uint32_t clientId = entry.first;
        ControlState& cs = entry.second;
        auto hsIt = m_handshakes.find(clientId);
        if (hsIt == m_handshakes.end() || !hsIt->second || !hsIt->second->IsJoined()) {
            continue;
        }
        auto conn = GetConnection(clientId);
        if (!conn || conn->IsDisconnected() || !conn->IsHandshakeComplete()) {
            continue;
        }

        if (cs.lastServerSendMs == 0) {
            cs.lastServerSendMs = now;
            continue;
        }
        if (now - cs.lastServerSendMs < kKeepAliveMs) {
            continue;
        }

        // FlushPendingAcks ran earlier in this pump, so this is normally a truly
        // empty packet. BuildAckOnlyPacket is still the correct allocator: it
        // advances the connection's shared outbound PacketId exactly once.
        SendEncodedPacket(clientId, cs.outbound.BuildAckOnlyPacket());
        Logger::Trace("[ConnectionManager::TransportKeepAliveTick] client %u: sent empty UE3 idle keepalive",
                      clientId);
    }
}

bool ConnectionManager::SendReliableBunches(
    uint32_t clientId, const std::vector<PacketCodec::Bunch>& bunches) {
    auto conn = GetConnection(clientId);
    if (!conn || conn->IsDisconnected() || bunches.empty()) return false;
    ControlState& cs = GetControlState(clientId);
    // Once ClientTravel is queued this incarnation is drain-only. Earlier
    // reliable actor bunches remain in pendingReliable because ch2 ordering
    // requires them to arrive before ClientTravel, but no new old-world actor
    // state may be appended behind the travel RPC.
    if (cs.mapTravelPending &&
        std::any_of(bunches.begin(), bunches.end(),
                    [](const PacketCodec::Bunch& bunch) {
                        return bunch.chIndex >= 2u;
                    })) {
        Logger::Trace(
            "[ConnectionManager::SendReliableBunches] client %u is awaiting "
            "map travel; suppressed %zu new actor bunch(es)",
            clientId, bunches.size());
        return false;
    }
    for (const PacketCodec::Bunch& bunch : bunches) {
        if (bunch.bOpen && !bunch.bClose && bunch.chIndex >= 2u &&
            bunch.chIndex < ActorRepl::kDynamicChannelMax) {
            cs.outboundActorChannels.set(bunch.chIndex);
        }
    }
    const PacketCodec::Packet pkt = cs.outbound.BuildRawBunchesPacket(bunches);
    const std::vector<uint8_t> wire =
        PacketCodec::Encode(pkt, PacketCodec::kServerSendMaxPacketBytes);
    const bool sent = conn->SendRaw(wire.data(), wire.size());
    if (sent) {
        cs.lastServerSendMs = NowMs();
    }
    // Record the reliable bunches so we can retransmit until the client acks this packet.
    std::vector<PacketCodec::Bunch> rel;
    for (const auto& b : bunches) if (b.bReliable) rel.push_back(b);
    if (!rel.empty()) {
        ControlState::SentReliable sr;
        sr.packetIds.push_back(pkt.packetId);
        sr.lastSendMs = NowMs();
        sr.resendCount = 0;
        sr.bunches = std::move(rel);
        cs.pendingReliable.push_back(std::move(sr));
    }
    return sent;
}

std::optional<PacketCodec::OutboundReliableSequencer::Reservation>
ConnectionManager::ReserveCh2Reliable(ControlState& state, uint32_t clientId,
                                      size_t count, const char* context) {
    if (!state.outboundActorChannels.test(2u)) {
        Logger::Warn(
            "[OutboundReliable] client %u cannot reserve ch2 sequence(s) "
            "for %s after the PlayerController channel closed",
            clientId, context ? context : "unknown");
        return std::nullopt;
    }
    auto reservation = state.ch2Reliable.ReserveBatch(count);
    if (!reservation) {
        Logger::Warn(
            "[OutboundReliable] client %u could not reserve %zu ch2 "
            "sequence(s) for %s (error=%u, outstanding=%zu, window=%zu)",
            clientId, count, context ? context : "unknown",
            static_cast<unsigned>(reservation.error()),
            state.ch2Reliable.OutstandingCount(),
            state.ch2Reliable.IssuanceWindowSize());
        return std::nullopt;
    }
    return std::move(*reservation);
}

bool ConnectionManager::SendReservedCh2Bunches(
    uint32_t clientId, const std::vector<PacketCodec::Bunch>& bunches,
    const PacketCodec::OutboundReliableSequencer::Reservation& reservation,
    const char* context) {
    ControlState& state = GetControlState(clientId);
    size_t reservationIndex = 0u;
    for (const PacketCodec::Bunch& bunch : bunches) {
        if (!bunch.bReliable || bunch.chIndex != 2u) continue;
        if (reservationIndex >= reservation.size() ||
            bunch.chSequence != reservation[reservationIndex]) {
            Logger::Error(
                "[OutboundReliable] client %u %s bunch/reservation mismatch; "
                "cancelling unpublished ch2 batch",
                clientId, context ? context : "unknown");
            const auto cancelled = state.ch2Reliable.CancelBatch(reservation);
            if (!cancelled) {
                Logger::Error(
                    "[OutboundReliable] client %u could not cancel mismatched "
                    "ch2 batch (error=%u)",
                    clientId, static_cast<unsigned>(cancelled.error()));
                FailCloseCh2Publication(clientId,
                                        "ch2 reservation mismatch rollback");
            }
            return false;
        }
        ++reservationIndex;
    }
    if (reservationIndex != reservation.size()) {
        Logger::Error(
            "[OutboundReliable] client %u %s did not consume all reserved ch2 "
            "sequences; cancelling unpublished batch",
            clientId, context ? context : "unknown");
        const auto cancelled = state.ch2Reliable.CancelBatch(reservation);
        if (!cancelled) {
            Logger::Error(
                "[OutboundReliable] client %u could not cancel unused ch2 "
                "batch (error=%u)",
                clientId, static_cast<unsigned>(cancelled.error()));
            FailCloseCh2Publication(clientId,
                                    "unused ch2 reservation rollback");
        }
        return false;
    }

    const size_t pendingBefore = state.pendingReliable.size();
    (void)SendReliableBunches(clientId, bunches);
    if (state.pendingReliable.size() > pendingBefore) {
        const auto committed = state.ch2Reliable.CommitBatch(reservation);
        if (!committed) {
            Logger::Error(
                "[OutboundReliable] client %u queued %s but could not commit "
                "its ch2 reservation (error=%u)",
                clientId, context ? context : "unknown",
                static_cast<unsigned>(committed.error()));
            FailCloseCh2Publication(clientId,
                                    "queued ch2 reservation commit");
            return false;
        }
        return true;
    }

    const auto cancelled = state.ch2Reliable.CancelBatch(reservation);
    if (!cancelled) {
        Logger::Error(
            "[OutboundReliable] client %u could not roll back rejected %s "
            "ch2 reservation (error=%u)",
            clientId, context ? context : "unknown",
            static_cast<unsigned>(cancelled.error()));
        FailCloseCh2Publication(clientId,
                                "rejected ch2 reservation rollback");
    }
    return false;
}

void ConnectionManager::FailCloseCh2Publication(uint32_t clientId,
                                                  const char* context) {
    const std::shared_ptr<ClientConnection> connection =
        GetConnection(clientId);
    if (connection && !connection->IsDisconnected()) {
        connection->MarkDisconnected();
    }
    Logger::Error(
        "[OutboundReliable] client %u fail-closed after %s could not be "
        "published consistently",
        clientId, context ? context : "a load-bearing ch2 transition");
}

void ConnectionManager::OnClientAck(uint32_t clientId, uint32_t ackedPacketId) {
    auto it = m_controlState.find(clientId);
    if (it == m_controlState.end()) return;
    ControlState& cs = it->second;
    auto& pending = cs.pendingReliable;
    auto packetWasAcked = [ackedPacketId](
                              const ControlState::SentReliable& reliable) {
        return std::find(reliable.packetIds.begin(), reliable.packetIds.end(),
                         ackedPacketId) != reliable.packetIds.end();
    };
    for (const ControlState::SentReliable& reliable : pending) {
        if (!packetWasAcked(reliable)) continue;
        for (const PacketCodec::Bunch& bunch : reliable.bunches) {
            if (bunch.bReliable && bunch.chIndex == 2u) {
                const auto released = cs.ch2Reliable.Release(bunch.chSequence);
                if (!released) {
                    Logger::Warn(
                        "[OutboundReliable] client %u ACKed untracked ch2 "
                        "sequence %u (error=%u)",
                        clientId, bunch.chSequence,
                        static_cast<unsigned>(released.error()));
                }
            }
            if (bunch.bReliable) {
                if (auto* sequencer =
                        OwningPawnGraphSequencer(cs, bunch.chIndex);
                    sequencer &&
                    sequencer->IsInFlight(bunch.chSequence)) {
                    const auto released = sequencer->Release(
                        bunch.chSequence);
                    if (!released) {
                        Logger::Warn(
                            "[OwningPawnGraph] client %u ACKed untracked "
                            "ch%u sequence %u (error=%u)",
                            clientId, bunch.chIndex, bunch.chSequence,
                            static_cast<unsigned>(released.error()));
                    }
                }
            }
            if (bunch.bReliable && bunch.bClose &&
                cs.pawnGraphPhase == OwningPawnGraphPhase::Closing &&
                bunch.chIndex < ActorRepl::kDynamicChannelMax &&
                cs.owningPawnGraphClosingChannels.test(bunch.chIndex)) {
                cs.owningPawnGraphCloseAcknowledged.set(bunch.chIndex);
            }
            if (bunch.bReliable && bunch.bClose &&
                bunch.chIndex < ActorRepl::kDynamicChannelMax &&
                cs.m61Visuals.IsCloseQueued(bunch.chIndex)) {
                cs.m61CloseAcknowledged.set(bunch.chIndex);
            }
            if (bunch.bReliable && bunch.bClose &&
                bunch.chIndex < ActorRepl::kDynamicChannelMax) {
                const ParticipantActorChannelBinding* binding =
                    cs.remoteParticipants.FindByChannel(bunch.chIndex);
                if (binding && binding->pawnChannel == bunch.chIndex &&
                    binding->pawnState ==
                        ParticipantActorOpenState::Closing) {
                    cs.participantPawnCloseAcknowledged.set(bunch.chIndex);
                }
            }
        }
    }
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 packetWasAcked),
                  pending.end());

    // A packet-level ACK for the close can arrive while the actor channel is
    // still sequence-buffering an earlier open. Release only when both facts
    // are true: the close was ACKed and no reliable bunch for that channel is
    // left in our retransmit ledger.
    for (uint32_t channel = WeaponCombatRepl::kM61VisualFirstChannel;
         channel <= WeaponCombatRepl::kM61VisualLastChannel; ++channel) {
        if (!cs.m61CloseAcknowledged.test(channel)) continue;
        const bool channelStillPending = std::any_of(
            pending.begin(), pending.end(),
            [channel](const ControlState::SentReliable& reliable) {
                return std::any_of(
                    reliable.bunches.begin(), reliable.bunches.end(),
                    [channel](const PacketCodec::Bunch& bunch) {
                        return bunch.bReliable && bunch.chIndex == channel;
                    });
            });
        if (channelStillPending) continue;
        if (cs.m61Visuals.AcknowledgeClose(channel)) {
            cs.outboundActorChannels.reset(channel);
            cs.m61CloseAcknowledged.reset(channel);
        }
    }

    // Participant pawn channels obey the same two-part release contract as
    // M61: ACKing the close packet is insufficient while any earlier reliable
    // on that channel remains in the retransmit ledger. Reliable cursors stay
    // in ParticipantActorChannelMap and therefore survive the next incarnation.
    for (uint32_t channel = ParticipantActorChannelMap::kFirstChannel;
         channel <= ParticipantActorChannelMap::kLastChannel; ++channel) {
        if (!cs.participantPawnCloseAcknowledged.test(channel)) continue;
        const bool channelStillPending = std::any_of(
            pending.begin(), pending.end(),
            [channel](const ControlState::SentReliable& reliable) {
                return std::any_of(
                    reliable.bunches.begin(), reliable.bunches.end(),
                    [channel](const PacketCodec::Bunch& bunch) {
                        return bunch.bReliable && bunch.chIndex == channel;
                    });
            });
        if (channelStillPending) continue;
        if (cs.remoteParticipants.AcknowledgePawnClose(channel)) {
            cs.outboundActorChannels.reset(channel);
            cs.participantPawnCloseAcknowledged.reset(channel);
        }
    }

    if (cs.pawnGraphPhase == OwningPawnGraphPhase::Closing) {
        if (cs.inboundPacketDispatchActive) {
            cs.owningPawnGraphCompletionDeferred = true;
        } else {
            CompleteOwningPawnGraphClose(clientId);
        }
    }
}

void ConnectionManager::RetransmitTick() {
    const uint64_t now = NowMs();
    for (auto& kv : m_controlState) {
        ControlState& cs = kv.second;
        if (cs.pendingReliable.empty()) continue;
        auto conn = GetConnection(kv.first);
        if (!conn) continue;
        for (auto& sr : cs.pendingReliable) {
            if (now - sr.lastSendMs < sr.retryDelayMs) continue;
            if (sr.resendCount >= sr.maxResends) {
                // Leaving an exhausted reliable set in the ledger forever
                // permanently stalls every later bunch on any affected UE3
                // channel. A client can keep sending heartbeats in that state,
                // so the ordinary inactivity timeout never repairs it and the
                // user remains trapped until the whole server restarts. Retire
                // just this session; the next datagram from the same endpoint
                // starts a fresh handshake through CreateOrGetClient().
                Logger::Error(
                    "[ConnectionManager::RetransmitTick] client %u: reliable "
                    "delivery exhausted after %d retries; retiring protocol "
                    "session",
                    kv.first, sr.maxResends);
                conn->MarkDisconnected();
                break;
            }
            // Resend the SAME reliable bunches (verbatim, same per-channel ChSequence) in a
            // NEW packet (new PacketId). The client fills the gap or ignores the duplicate.
            const PacketCodec::Packet pkt = cs.outbound.BuildRawBunchesPacket(sr.bunches);
            const std::vector<uint8_t> wire =
                PacketCodec::Encode(pkt, PacketCodec::kServerSendMaxPacketBytes);
            if (conn->SendRaw(wire.data(), wire.size())) {
                cs.lastServerSendMs = now;
            }
            sr.packetIds.push_back(pkt.packetId);
            sr.lastSendMs = now;
            ++sr.resendCount;
            Logger::Debug("[ConnectionManager::RetransmitTick] client %u: resent %zu reliable bunch(es) attempt %d (pkt %u)",
                          kv.first, sr.bunches.size(), sr.resendCount, pkt.packetId);
        }
    }
}

void ConnectionManager::FlushPendingAcks() {
    const uint64_t now = NowMs();
    for (auto& kv : m_controlState) {
        ControlState& cs = kv.second;
        const size_t nAcks = cs.outbound.PendingAckCount();
        if (nAcks == 0) continue;
        // Coalesce: at most one standalone ack-only datagram per ~20ms per client (acks also
        // piggyback on any data packet we send). Escape the throttle if acks pile up (>=32)
        // so a fast client's reliable window never stalls. This replaces the per-received
        // -packet ack-only send that flooded loopback (~4000 ack datagrams) and dropped the
        // pawn open + other reliables.
        if (now - cs.lastAckFlushMs < 20 && nAcks < 32) continue;
        auto conn = GetConnection(kv.first);
        if (!conn) continue;
        SendEncodedPacket(kv.first, cs.outbound.BuildAckOnlyPacket());
        cs.lastAckFlushMs = now;
    }
}

bool ConnectionManager::SendCh2Rpc(uint32_t clientId,
                                   const std::vector<uint8_t>& payload,
                                   uint32_t payloadBits, const char* name) {
    ControlState& cs = GetControlState(clientId);
    const auto reservation =
        ReserveCh2Reliable(cs, clientId, 1u, name);
    if (!reservation) return false;
    PacketCodec::Bunch b;
    b.bControl   = false;
    b.bOpen      = false;
    b.bClose     = false;
    b.bReliable  = true;
    b.chIndex    = 2;
    b.chType     = cs.actorChType;
    b.chSequence = reservation->front();
    b.payload    = payload;
    b.payloadBits = payloadBits;
    if (!SendReservedCh2Bunches(clientId, {b}, *reservation, name)) {
        return false;
    }
    Logger::Info("[ConnectionManager::SendCh2Rpc] client %u: queued %s on ch2 seq %u (%u bits)",
                 clientId, name, b.chSequence, payloadBits);
    return true;
}

bool ConnectionManager::CanBroadcastRetailClientTravel(
    const ClientTravelRepl::EncodedRpc& rpc,
    const std::string& mapUrl,
    size_t* eligibleClients) const {
    if (eligibleClients) *eligibleClients = 0;
    if (!ClientTravelRepl::IsValid(rpc)) {
        Logger::Error(
            "[ClientTravel] preflight rejected invalid encoded RPC for map '%s'",
            mapUrl.c_str());
        return false;
    }

    size_t eligible = 0;
    for (const std::shared_ptr<ClientConnection>& connection :
         GetAllConnections()) {
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() ||
            !connection->IsHandshakeComplete()) {
            continue;
        }

        ++eligible;
        const uint32_t clientId = connection->GetClientId();
        const auto stateIt = m_controlState.find(clientId);
        if (stateIt == m_controlState.end()) {
            Logger::Error(
                "[ClientTravel] preflight failed for '%s': joined client %u "
                "has no control state",
                mapUrl.c_str(), clientId);
            return false;
        }
        if (stateIt->second.mapTravelPending) {
            Logger::Error(
                "[ClientTravel] preflight failed for '%s': client %u already "
                "has travel pending",
                mapUrl.c_str(), clientId);
            return false;
        }
        if (!stateIt->second.ch2Reliable.IsInitialized() ||
            stateIt->second.ch2Reliable.AvailableCapacity() < 1u ||
            !stateIt->second.outboundActorChannels.test(2u)) {
            Logger::Error(
                "[ClientTravel] preflight failed for '%s': joined client %u "
                "has no live PlayerController ch2 actor channel",
                mapUrl.c_str(), clientId);
            return false;
        }
    }

    if (eligibleClients) *eligibleClients = eligible;
    return true;
}

size_t ConnectionManager::BroadcastRetailClientTravel(
    const ClientTravelRepl::EncodedRpc& rpc,
    const std::string& mapUrl) {
    size_t eligible = 0;
    if (!CanBroadcastRetailClientTravel(rpc, mapUrl, &eligible)) {
        return 0;
    }

    // Snapshot and revalidate the complete recipient set before the first
    // send. The server loop is single-threaded, but this two-phase shape keeps
    // future callbacks/refactors from turning a late failure into split-world
    // travel.
    struct TravelRecipient {
        uint32_t clientId = 0;
        std::shared_ptr<ClientConnection> connection;
        ControlState* state = nullptr;
        PacketCodec::OutboundReliableSequencer::Reservation reservation;
    };
    std::vector<TravelRecipient> recipients;
    recipients.reserve(eligible);
    for (const std::shared_ptr<ClientConnection>& connection :
         GetAllConnections()) {
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() ||
            !connection->IsHandshakeComplete()) {
            continue;
        }
        const uint32_t clientId = connection->GetClientId();
        const auto stateIt = m_controlState.find(clientId);
        if (stateIt == m_controlState.end() ||
            stateIt->second.mapTravelPending ||
            !stateIt->second.ch2Reliable.IsInitialized() ||
            stateIt->second.ch2Reliable.AvailableCapacity() < 1u ||
            !stateIt->second.outboundActorChannels.test(2u)) {
            Logger::Error(
                "[ClientTravel] recipient set changed after preflight for '%s'; "
                "nothing was queued",
                mapUrl.c_str());
            for (TravelRecipient& recipient : recipients) {
                (void)recipient.state->ch2Reliable.CancelBatch(
                    recipient.reservation);
            }
            return 0;
        }
        auto reservation = ReserveCh2Reliable(
            stateIt->second, clientId, 1u, "ClientTravel cohort");
        if (!reservation) {
            for (TravelRecipient& recipient : recipients) {
                (void)recipient.state->ch2Reliable.CancelBatch(
                    recipient.reservation);
            }
            Logger::Error(
                "[ClientTravel] reliable reservation failed for '%s'; "
                "nothing was queued",
                mapUrl.c_str());
            return 0;
        }
        recipients.push_back(TravelRecipient{
            clientId, connection, &stateIt->second, std::move(*reservation)});
    }
    if (recipients.size() != eligible) {
        Logger::Error(
            "[ClientTravel] recipient count changed after preflight for '%s' "
            "(%zu -> %zu); nothing was queued",
            mapUrl.c_str(), eligible, recipients.size());
        for (TravelRecipient& recipient : recipients) {
            (void)recipient.state->ch2Reliable.CancelBatch(
                recipient.reservation);
        }
        return 0;
    }

    const std::string rpcName = "ClientTravel(" + mapUrl + ")";
    // No operation in this loop invokes a game callback or mutates
    // m_controlState. Queue the reliable RPC for the entire frozen cohort
    // before changing any session to drain-only, leaving no fallible lookup or
    // observable half-pending state in the commit phase.
    for (size_t recipientIndex = 0u;
         recipientIndex < recipients.size(); ++recipientIndex) {
        TravelRecipient& recipient = recipients[recipientIndex];
        PacketCodec::Bunch bunch;
        bunch.bReliable = true;
        bunch.chIndex = 2u;
        bunch.chType = recipient.state->actorChType;
        bunch.chSequence = recipient.reservation.front();
        bunch.payload = rpc.payload;
        bunch.payloadBits = rpc.payloadBits;
        if (!SendReservedCh2Bunches(
                recipient.clientId, {std::move(bunch)},
                recipient.reservation, rpcName.c_str())) {
            for (size_t pendingIndex = recipientIndex + 1u;
                 pendingIndex < recipients.size(); ++pendingIndex) {
                (void)recipients[pendingIndex]
                    .state->ch2Reliable.CancelBatch(
                        recipients[pendingIndex].reservation);
            }
            // A subset may already own a queued travel RPC. Do not let that
            // cohort continue in split worlds: fail closed and require every
            // member to establish a fresh session.
            for (TravelRecipient& frozenRecipient : recipients) {
                if (frozenRecipient.connection) {
                    frozenRecipient.connection->MarkDisconnected();
                }
            }
            Logger::Error(
                "[ClientTravel] cohort queue failed for '%s'; travel state "
                "was not committed and the frozen cohort was disconnected",
                mapUrl.c_str());
            return 0;
        }
    }
    // Zero is reserved as the invalid/uninitialized sentinel used by the
    // fail-closed expiry path. A steady clock can theoretically report zero
    // during process startup, so normalize that one instant explicitly.
    const uint64_t travelStartedAt = std::max<uint64_t>(1u, NowMs());
    for (const TravelRecipient& recipient : recipients) {
        recipient.state->mapTravelPending = true;
        recipient.state->mapTravelStartedMs = travelStartedAt;
        recipient.state->spawned = false;
        recipient.state->deferredOwningPawnGraphDeployment.reset();
        recipient.state->owningPawnAlive = false;
        InvalidatePossessionRecovery(*recipient.state);
        // ClientTravel keeps the transport/reliable ledger alive, but every
        // remote actor belongs to the old PackageMap. Tombstone all used pairs
        // now and discard visual snapshots. They must not be reopened or reused
        // until the fresh handshake constructs a new ControlState namespace.
        (void)recipient.state->remoteParticipants.RetireAll();
        recipient.state->remotePawnViews.clear();
        recipient.state->participantPawnCloseAcknowledged.reset();
        recipient.state->lastRemotePawnReplicationMs = 0;
    }

    // Keep only each endpoint's reliable/control transport ledger alive. Game
    // Player/Team/role/combat state belongs to the old world and must not count
    // in the new one while the client loads and reconnects.
    for (const TravelRecipient& recipient : recipients) {
        m_deploymentCoordinator.RemoveClient(recipient.clientId);
        m_deploymentCountdown.RemoveClient(recipient.clientId);
        if (m_server) {
            m_server->OnClientMapTravelQueued(recipient.clientId);
        }
    }
    UpdateTelemetryPlayerCounts();

    Logger::Info(
        "[ClientTravel] queued relative travel to '%s' for %zu joined retail "
        "client(s)",
        mapUrl.c_str(), recipients.size());
    return recipients.size();
}

ConnectionManager::DeploymentPhaseState
ConnectionManager::GetDeploymentPhaseState() const {
    DeploymentPhaseState state;
    if (!m_server) return state;

    if (const auto* territory = m_server->GetTerritoryMode()) {
        state.remainingSeconds = territory->GetRoundTimeRemaining();
        switch (territory->GetPhase()) {
            case TerritoryMode::Phase::Preparation:
                state.phase = DeploymentCountdown::Phase::Preparation;
                break;
            case TerritoryMode::Phase::Active:
            case TerritoryMode::Phase::Overtime:
            case TerritoryMode::Phase::Lockdown:
            case TerritoryMode::Phase::SuddenDeath:
                state.phase = DeploymentCountdown::Phase::Active;
                break;
            default:
                state.phase = DeploymentCountdown::Phase::PostRound;
                break;
        }
        return state;
    }

    if (const auto* supremacy = m_server->GetSupremacyMode()) {
        state.remainingSeconds = supremacy->GetPhaseTimeRemaining();
        switch (supremacy->GetPhase()) {
            case SupremacyMode::Phase::Preparation:
                state.phase = DeploymentCountdown::Phase::Preparation;
                break;
            case SupremacyMode::Phase::Active:
            case SupremacyMode::Phase::SuddenDeath:
                state.phase = DeploymentCountdown::Phase::Active;
                break;
            default:
                state.phase = DeploymentCountdown::Phase::PostRound;
                break;
        }
        return state;
    }

    if (const auto* skirmish = m_server->GetSkirmishMode()) {
        state.remainingSeconds = skirmish->GetPhaseTimeRemaining();
        switch (skirmish->GetPhase()) {
            case SkirmishMode::Phase::Preparation:
                state.phase = DeploymentCountdown::Phase::Preparation;
                break;
            case SkirmishMode::Phase::Active:
            case SkirmishMode::Phase::InstantDeath:
                state.phase = DeploymentCountdown::Phase::Active;
                break;
            default:
                state.phase = DeploymentCountdown::Phase::PostRound;
                break;
        }
        return state;
    }

    if (const auto* gameState = m_server->GetGameState()) {
        state.remainingSeconds = static_cast<float>(gameState->GetRemainingTime().count());
        switch (gameState->GetPhase()) {
            case GamePhase::Preparation:
                state.phase = DeploymentCountdown::Phase::Preparation;
                break;
            case GamePhase::Active:
                state.phase = DeploymentCountdown::Phase::Active;
                break;
            default:
                state.phase = DeploymentCountdown::Phase::PostRound;
                break;
        }
    }
    return state;
}

bool ConnectionManager::IsDeploymentWindowOpen(
    const DeploymentPhaseState& state) noexcept {
    if (state.phase == DeploymentCountdown::Phase::Active) return true;
    return state.phase == DeploymentCountdown::Phase::Preparation &&
        std::isfinite(state.remainingSeconds) &&
        state.remainingSeconds >= 0.0f &&
        state.remainingSeconds <= static_cast<float>(
            DeploymentCountdown::kRoundStartScreenSeconds);
}

void ConnectionManager::RevokePreparedDeploymentAuthorization(
    uint32_t clientId) {
    const auto deployment =
        m_deploymentCoordinator.GetClientState(clientId);
    const bool roleWasFinalized =
        deployment.has_value() && deployment->roleFinalized;
    auto stateIt = m_controlState.find(clientId);
    if (stateIt != m_controlState.end()) {
        ControlState& state = stateIt->second;
        state.spawned = false;
        state.deferredOwningPawnGraphDeployment.reset();
        state.owningPawnAlive = false;
        InvalidatePossessionRecovery(state);
    }

    m_deploymentCoordinator.ResetClient(clientId);
    if (roleWasFinalized ||
        (stateIt != m_controlState.end() &&
         stateIt->second.roleFinalized)) {
        m_deploymentCoordinator.FinalizeRole(clientId);
    }
    if (m_server) {
        if (PlayerManager* players = m_server->GetPlayerManager()) {
            if (const std::shared_ptr<Player> player =
                    players->GetPlayer(clientId)) {
                player->SetReadyToSpawn(false);
            }
        }
    }
}

std::vector<uint32_t> ConnectionManager::GetAvailableSpawnIds(uint32_t clientId) const {
    std::vector<uint32_t> ids;
    if (!m_server) return ids;
    const SpawnSystem* spawns = m_server->GetSpawnSystem();
    if (!spawns) return ids;
    const auto available = spawns->GetAvailableSpawns(clientId);
    ids.reserve(available.size());
    for (const SpawnLocation* spawn : available) {
        if (spawn) ids.push_back(spawn->id);
    }
    return ids;
}

std::vector<uint32_t> ConnectionManager::GetCurrentAdvertisedSpawnIds(
    uint32_t clientId) const {
    std::vector<uint32_t> ids;
    if (!m_server) return ids;
    const SpawnSystem* spawns = m_server->GetSpawnSystem();
    if (!spawns) return ids;
    const auto advertised = BuildAdvertisedSpawnRepresentatives(
        spawns->GetAvailableSpawns(clientId));
    ids.reserve(advertised.size());
    for (const SpawnLocation* spawn : advertised) {
        ids.push_back(spawn->id);
    }
    return ids;
}

std::vector<uint32_t> ConnectionManager::GetAdvertisedSpawnIds(uint32_t clientId) const {
    std::vector<uint32_t> ids;
    const auto it = m_controlState.find(clientId);
    if (it == m_controlState.end()) return ids;
    const ControlState& cs = it->second;
    ids.reserve(cs.advertisedSpawnCount);
    for (uint8_t slot = 0; slot < cs.advertisedSpawnCount; ++slot) {
        ids.push_back(cs.advertisedSpawnIds[slot]);
    }
    return ids;
}

void ConnectionManager::RefreshAdvertisedSpawnIds(uint32_t clientId) {
    ControlState& cs = GetControlState(clientId);
    cs.advertisedSpawnIds.fill(0);
    cs.advertisedSpawnVolumeRefs.fill(0);
    cs.advertisedSpawnCount = 0;
    if (!m_server) return;
    const SpawnSystem* spawns = m_server->GetSpawnSystem();
    if (!spawns) return;
    const auto advertised = BuildAdvertisedSpawnRepresentatives(
        spawns->GetAvailableSpawns(clientId));
    for (const SpawnLocation* spawn : advertised) {
        const uint32_t volumeRef = spawn->retailSpawnVolumeRef;
        const uint8_t slot = cs.advertisedSpawnCount++;
        cs.advertisedSpawnIds[slot] = spawn->id;
        cs.advertisedSpawnVolumeRefs[slot] = volumeRef;
    }
}

bool ConnectionManager::SendRetailSpawnLocations(uint32_t clientId) {
    if (!m_server) return false;
    TeamManager* teams = m_server->GetTeamManager();
    if (!teams) return false;

    const std::optional<RetailBootstrap::ArtifactSelection>& artifact =
        GetRetailArtifactSelection(clientId);
    if (!artifact) {
        Logger::Warn("[SpawnReplication] client %u has no resolved PackageMap "
                     "artifact; not sending h59", clientId);
        return false;
    }

    ControlState& cs = GetControlState(clientId);
    const uint8_t retailTeam = TeamMapping::ServerToRetail(
        teams->GetPlayerTeam(clientId));
    if (retailTeam >= cs.teamInfoChannels.size()) {
        Logger::Warn("[SpawnReplication] client %u has no playable retail team; "
                     "not sending h59", clientId);
        return false;
    }

    const uint32_t teamChannel = cs.teamInfoChannels[retailTeam];
    if (teamChannel == 0) {
        Logger::Warn("[SpawnReplication] client %u has no open TeamInfo channel for "
                     "retail team %u", clientId, static_cast<unsigned>(retailTeam));
        return false;
    }

    std::array<uint32_t, SpawnRepl::kAvailableSpawnLocationCount> wireRefs{};
    bool hasMappedLocation = false;
    for (uint8_t slot = 0; slot < cs.advertisedSpawnCount; ++slot) {
        const uint32_t canonicalRef = cs.advertisedSpawnVolumeRefs[slot];
        const std::optional<uint32_t> objectRef =
            SpawnRepl::RebaseStaticObjectRef(
                canonicalRef, artifact->mapObjectRefOffset);
        if (!objectRef) {
            Logger::Warn("[SpawnReplication] client %u slot %u cannot rebase "
                         "canonical static object ref %u by %u; suppressing h59 "
                         "transaction", clientId, static_cast<unsigned>(slot),
                         canonicalRef, artifact->mapObjectRefOffset);
            return false;
        }
        wireRefs[slot] = *objectRef;
        hasMappedLocation = hasMappedLocation || *objectRef != 0;
    }
    if (!hasMappedLocation) {
        Logger::Warn("[SpawnReplication] client %u has no PackageMap-backed spawn "
                     "locations for the active map; not fabricating h59 refs", clientId);
        return false;
    }

    BitWriter writer;
    for (uint8_t slot = 0; slot < SpawnRepl::kAvailableSpawnLocationCount; ++slot) {
        const uint32_t objectRef = slot < cs.advertisedSpawnCount
            ? wireRefs[slot]
            : 0u;
        if (!SpawnRepl::WriteAvailableSpawnLocation(writer, slot, objectRef)) {
            Logger::Warn("[SpawnReplication] client %u failed to encode h59 slot %u",
                         clientId, static_cast<unsigned>(slot));
            return false;
        }
    }

    PacketCodec::Bunch bunch;
    bunch.bReliable = false; // capture: h59 is an unreliable TeamInfo delta
    bunch.chIndex = teamChannel;
    bunch.chType = cs.actorChType;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    if (!SendReliableBunches(clientId, {bunch})) {
        Logger::Warn("[SpawnReplication] client %u failed to send h59; "
                     "suppressing spawn-selection authorization", clientId);
        return false;
    }
    Logger::Info("[SpawnReplication] client %u advertised %u spawn slots on "
                 "TeamInfo ch%u (retail team %u, artifact=%.*s, map-ref +%u)",
                 clientId,
                 static_cast<unsigned>(cs.advertisedSpawnCount), teamChannel,
                 static_cast<unsigned>(retailTeam),
                 static_cast<int>(artifact->variant.size()),
                 artifact->variant.data(), artifact->mapObjectRefOffset);
    return true;
}

void ConnectionManager::SendOwnerPriClassIndex(uint32_t clientId,
                                               uint8_t classIndex) {
    BitWriter writer;
    RoleSelectionRepl::WriteOwnerPriClassIndex(writer, classIndex);

    PacketCodec::Bunch bunch;
    bunch.bReliable = false; // capture f2350: owning PRI role delta is unreliable
    bunch.chIndex = 26;
    bunch.chType = 2;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    SendReliableBunches(clientId, {bunch});
}

void ConnectionManager::SendOwnerPriRoleAssignment(uint32_t clientId,
                                                    uint8_t squadIndex,
                                                    uint8_t roleIndex) {
    BitWriter writer;
    RoleSelectionRepl::WriteOwnerPriRoleAssignment(writer, squadIndex,
                                                   roleIndex);

    PacketCodec::Bunch bunch;
    bunch.bReliable = false; // capture f62024: ordinary owner-PRI property delta
    bunch.chIndex = 26;
    bunch.chType = 2;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    SendReliableBunches(clientId, {bunch});
}

bool ConnectionManager::SendChangedSquadAssignment(
    uint32_t clientId,
    const RoleSelectionRepl::ChangedSquadEvidence& evidence) {
    const auto stateIt = m_controlState.find(clientId);
    const std::shared_ptr<ClientConnection> connection =
        GetConnection(clientId);
    if (stateIt == m_controlState.end() || !connection ||
        connection->IsDisconnected() || !connection->IsUE3Client() ||
        !connection->IsHandshakeComplete()) {
        return false;
    }

    ControlState& cs = stateIt->second;
    const auto reservation = ReserveCh2Reliable(
        cs, clientId, 1u, "ChangedSquad");
    if (!reservation) return false;
    uint32_t changedSquadBits = 0;
    const std::vector<uint8_t> changedSquad =
        RoleSelectionRepl::EncodeChangedSquad(evidence,
                                               changedSquadBits);

    PacketCodec::Bunch changedSquadBunch;
    changedSquadBunch.bReliable = true;
    changedSquadBunch.chIndex = 2;
    changedSquadBunch.chType = cs.actorChType;
    changedSquadBunch.chSequence = reservation->front();
    changedSquadBunch.payload = changedSquad;
    changedSquadBunch.payloadBits = changedSquadBits;

    BitWriter assignment;
    RoleSelectionRepl::WriteOwnerPriRoleAssignment(
        assignment, evidence.squadIndex, evidence.roleIndex);

    PacketCodec::Bunch priBunch;
    priBunch.bReliable = false;
    priBunch.chIndex = 26;
    priBunch.chType = 2;
    priBunch.payload = assignment.GetBytes();
    priBunch.payloadBits = static_cast<uint32_t>(assignment.NumBits());

    // A standalone retail ChangedSquad must be followed by the owner's PRI
    // h81/h80 confirmation in the same packet. This lets a promoted squad
    // leader observe the repaired role index atomically instead of retaining
    // the assignment cached before another member disconnected.
    if (!SendReservedCh2Bunches(
            clientId, {changedSquadBunch, priBunch}, *reservation,
            "ChangedSquad")) {
        return false;
    }
    Logger::Info(
        "[RoleSelection] client %u synchronized ChangedSquad(%u,%u) seq %u "
        "+ owner PRI h81/h80",
        clientId, static_cast<unsigned>(evidence.squadIndex),
        static_cast<unsigned>(evidence.roleIndex),
        changedSquadBunch.chSequence);
    return true;
}

void ConnectionManager::SynchronizeRetailSquadAssignments() {
    RoleSystem* roles = m_server ? m_server->GetRoleSystem() : nullptr;
    TeamManager* teams = m_server ? m_server->GetTeamManager() : nullptr;
    if (!roles) return;

    for (auto& [clientId, cs] : m_controlState) {
        const std::shared_ptr<ClientConnection> connection =
            GetConnection(clientId);
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() ||
            !connection->IsHandshakeComplete() || cs.mapTravelPending ||
            !cs.roleSelectionAccepted) {
            continue;
        }

        const std::optional<RetailSquadAssignment> assignment =
            roles->GetRetailSquadAssignment(clientId);
        if (!assignment ||
            (teams && teams->GetPlayerTeam(clientId) != assignment->teamId)) {
            continue;
        }
        if (cs.selectedRoleSquadIndex == assignment->squadIndex &&
            cs.selectedRoleIndex == assignment->roleIndex) {
            continue;
        }

        const RoleSelectionRepl::ChangedSquadEvidence evidence{
            assignment->squadIndex, assignment->roleIndex};
        if (!SendChangedSquadAssignment(clientId, evidence)) {
            // Squad repair is an event-driven one-shot. Leaving the old cache
            // in place is necessary for correctness, but no later callback is
            // guaranteed to retry it; require a fresh session instead of
            // letting the role UI retain a stale authoritative assignment.
            FailCloseCh2Publication(clientId, "ChangedSquad synchronization");
            continue;
        }
        cs.selectedRoleSquadIndex = assignment->squadIndex;
        cs.selectedRoleIndex = assignment->roleIndex;
        if (cs.selectedChangedRole.has_value() &&
            cs.selectedChangedRole->followingChangedSquad.has_value()) {
            cs.selectedChangedRole->followingChangedSquad = evidence;
        }
    }
}

bool ConnectionManager::SendChangedRoleSpawnSelect(
    uint32_t clientId, bool includeOwnerPriAssignment,
    bool includeTempStopAutoSpawn) {
    ControlState& cs = GetControlState(clientId);
    if (!cs.selectedChangedRole.has_value()) {
        Logger::Warn(
            "[RoleSelection] client %u cannot send ChangedRole: no exact "
            "capture-grounded h210 tuple is available",
            clientId);
        return false;
    }
    const RoleSelectionRepl::ChangedRoleEvidence& evidence =
        *cs.selectedChangedRole;
    const auto reservation = ReserveCh2Reliable(
        cs, clientId, includeTempStopAutoSpawn ? 2u : 1u,
        includeTempStopAutoSpawn
            ? "ClientTempStopAutoSpawn + ChangedRole"
            : "ChangedRole");
    if (!reservation) return false;

    // Freeze the exact normal-slot order before the UI opens. h261 is decoded
    // against this table, never against a newly-compressed live list.
    RefreshAdvertisedSpawnIds(clientId);
    if (!SendRetailSpawnLocations(clientId)) {
        const auto cancelled = cs.ch2Reliable.CancelBatch(*reservation);
        if (!cancelled) {
            Logger::Error(
                "[OutboundReliable] client %u could not cancel ChangedRole "
                "reservation after h59 publication failure (error=%u)",
                clientId, static_cast<unsigned>(cancelled.error()));
            FailCloseCh2Publication(
                clientId, "ChangedRole h59 rollback");
        }
        cs.advertisedSpawnIds.fill(0);
        cs.advertisedSpawnVolumeRefs.fill(0);
        cs.advertisedSpawnCount = 0;
        Logger::Warn(
            "[RoleSelection] client %u spawn-selection transition held because "
            "the authoritative h59 slot table was not published",
            clientId);
        return false;
    }

    uint32_t changedRoleBits = 0;
    const std::vector<uint8_t> changedRole =
        RoleSelectionRepl::EncodeChangedRoleTransition(evidence,
                                                       changedRoleBits);

    size_t reservationIndex = 0u;
    std::vector<PacketCodec::Bunch> ordered;
    ordered.reserve(1u + (includeTempStopAutoSpawn ? 1u : 0u) +
                    (includeOwnerPriAssignment ? 1u : 0u));
    if (includeTempStopAutoSpawn) {
        // ROPlayerController.uc ClientTempStopAutoSpawn sets the client's
        // SpawnReadyStatus to ESRS_NotReady. Without this source-grounded RPC,
        // a prior h434 Ready can make ChangedRole's ShowSpawnSelect early-out.
        BitWriter stopAutoSpawn;
        stopAutoSpawn.SerializeInt(262u, kRoPcMaxHandle);

        PacketCodec::Bunch stop;
        stop.bReliable = true;
        stop.chIndex = 2u;
        stop.chType = cs.actorChType;
        stop.chSequence = (*reservation)[reservationIndex++];
        stop.payload = stopAutoSpawn.GetBytes();
        stop.payloadBits = static_cast<uint32_t>(stopAutoSpawn.NumBits());
        ordered.push_back(std::move(stop));
    }

    PacketCodec::Bunch changedRoleBunch;
    changedRoleBunch.bReliable = true;
    changedRoleBunch.chIndex = 2;
    changedRoleBunch.chType = cs.actorChType;
    changedRoleBunch.chSequence = (*reservation)[reservationIndex++];
    changedRoleBunch.payload = changedRole;
    changedRoleBunch.payloadBits = changedRoleBits;

    ordered.push_back(changedRoleBunch);
    if (includeOwnerPriAssignment) {
        BitWriter assignment;
        RoleSelectionRepl::WriteOwnerPriRoleAssignment(
            assignment, cs.selectedRoleSquadIndex, cs.selectedRoleIndex);

        PacketCodec::Bunch priBunch;
        priBunch.bReliable = false;
        priBunch.chIndex = 26;
        priBunch.chType = 2;
        priBunch.payload = assignment.GetBytes();
        priBunch.payloadBits = static_cast<uint32_t>(assignment.NumBits());
        // Capture f2537 processes reliable ch2 h210 first, then owner PRI h81
        // SquadIndex and h80 RoleIndex in this exact order in the same packet.
        ordered.push_back(std::move(priBunch));
    }

    if (!SendReservedCh2Bunches(
            clientId, ordered, *reservation, "ChangedRole")) {
        return false;
    }
    Logger::Info(
        "[RoleSelection] client %u sent ChangedRole(squad=%u,class=%u)%s seq %u%s",
        clientId, static_cast<unsigned>(evidence.squadIndex),
        static_cast<unsigned>(evidence.classIndex),
        evidence.followingChangedSquad.has_value() ? " + ChangedSquad" : "",
        changedRoleBunch.chSequence,
        includeOwnerPriAssignment ? " + owner PRI h81/h80" : "");
    return true;
}

void ConnectionManager::SendPriSpawnSelection(uint32_t clientId,
                                              uint8_t encodedSelection) {
    constexpr uint32_t kPriMaxHandle = 98;
    BitWriter writer;
    ActorRepl::WritePropByte(writer, 77, kPriMaxHandle, encodedSelection);

    PacketCodec::Bunch bunch;
    bunch.bReliable = false;
    bunch.chIndex = 26;
    bunch.chType = 2;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    SendReliableBunches(clientId, {bunch});
}

bool ConnectionManager::SendShowRoundStartScreen(uint32_t clientId,
                                                 uint32_t displaySeconds) {
    BitWriter writer;
    writer.SerializeInt(225, kRoPcMaxHandle);
    if (displaySeconds != 0) {
        writer.WriteBit(true); // TimeDelay differs from its zero default
        writer.WriteInt32(static_cast<int32_t>(displaySeconds));
    } else {
        writer.WriteBit(false);
    }
    return SendCh2Rpc(clientId, writer.GetBytes(),
                      static_cast<uint32_t>(writer.NumBits()),
                      "ClientShowRoundStartScreen");
}

bool ConnectionManager::SendHideRoundStartScreen(uint32_t clientId) {
    BitWriter writer;
    writer.SerializeInt(226, kRoPcMaxHandle);
    return SendCh2Rpc(clientId, writer.GetBytes(),
                      static_cast<uint32_t>(writer.NumBits()),
                      "ClientHideRoundStartScreen");
}

bool ConnectionManager::ExecutePreparedDeployment(uint32_t clientId,
                                                   uint32_t spawnId) {
    ControlState& cs = GetControlState(clientId);
    if (!m_server) return false;
    const std::shared_ptr<ClientConnection> connection =
        GetConnection(clientId);
    if (!connection || connection->IsDisconnected() ||
        !connection->IsUE3Client() ||
        !connection->IsHandshakeComplete() || cs.mapTravelPending) {
        Logger::Info(
            "[Deployment] client %u no longer owns a live joined retail "
            "publication session",
            clientId);
        cs.deferredOwningPawnGraphDeployment.reset();
        return false;
    }
    if (cs.spawned) return true;

    const DeploymentPhaseState phase = GetDeploymentPhaseState();
    if (!IsDeploymentWindowOpen(phase)) {
        Logger::Info(
            "[Deployment] client %u authorization expired outside the "
            "active/final-preparation deployment window",
            clientId);
        RevokePreparedDeploymentAuthorization(clientId);
        return false;
    }

    const TeamManager* teams = m_server->GetTeamManager();
    const uint32_t deploymentTeam =
        teams ? teams->GetPlayerTeam(clientId) : 0u;
    if (!TeamMapping::IsPlayableServerTeam(deploymentTeam)) {
        Logger::Warn(
            "[Deployment] client %u has no playable team at commit time",
            clientId);
        RevokePreparedDeploymentAuthorization(clientId);
        FailOwningPawnGraph(
            clientId, "invalid authoritative deployment team");
        return false;
    }
    if (TicketSystem* tickets = m_server->GetTicketSystem()) {
        const bool depleted =
            tickets->GetInitialTickets(deploymentTeam) > 0u &&
            !tickets->HasTickets(deploymentTeam);
        if (depleted) {
            Logger::Info(
                "[Deployment] client %u team %u has no reinforcement "
                "tickets; revoking this authorization for a fresh retry",
                clientId, deploymentTeam);
            RevokePreparedDeploymentAuthorization(clientId);
            // The retail client still believes its last h434 Ready was
            // accepted. Resetting only the server coordinator strands it in a
            // Ready state that will not automatically reopen spawn selection
            // when tickets recover. Publish the same capture-grounded h210/h59
            // recovery used by other precommit failures.
            if (!SendChangedRoleSpawnSelect(
                    clientId, /*includeOwnerPriAssignment=*/false,
                    /*includeTempStopAutoSpawn=*/true)) {
                FailCloseCh2Publication(
                    clientId,
                    "ticket-depletion ChangedRole recovery");
            }
            return false;
        }
    }

    const std::vector<uint32_t> available = GetAvailableSpawnIds(clientId);
    if (std::find(available.begin(), available.end(), spawnId) == available.end()) {
        Logger::Warn("[Deployment] client %u selected spawn %u is no longer available; "
                     "reopening spawn selection", clientId, spawnId);
        m_deploymentCoordinator.ResetClient(clientId);
        m_deploymentCoordinator.FinalizeRole(clientId);
        if (!SendChangedRoleSpawnSelect(clientId)) {
            FailCloseCh2Publication(
                clientId, "invalid-spawn ChangedRole recovery");
        }
        return false;
    }

    SpawnSystem* spawns = m_server->GetSpawnSystem();
    uint32_t resolvedSpawnId = spawnId;
    if (spawns) {
        const SpawnLocation* representative = spawns->GetSpawnLocation(spawnId);
        if (representative && representative->retailSpawnVolumeRef != 0) {
            std::vector<uint32_t> groupRows;
            for (const SpawnLocation* candidate : spawns->GetAvailableSpawns(clientId)) {
                if (candidate && candidate->retailSpawnVolumeRef ==
                                     representative->retailSpawnVolumeRef) {
                    groupRows.push_back(candidate->id);
                }
            }
            if (!groupRows.empty()) {
                const std::size_t choice = static_cast<std::size_t>(
                    (static_cast<uint64_t>(clientId) + m_deploymentGeneration) %
                    groupRows.size());
                resolvedSpawnId = groupRows[choice];
            }
        }
    }

    const OwningPawnGraphGateResult graphGate =
        GateOwningPawnGraphForDeployment(
            clientId, deploymentTeam, spawnId);
    if (graphGate == OwningPawnGraphGateResult::Deferred) {
        Logger::Info(
            "[Deployment] client %u deferred spawn %u until the prior "
            "faction graph close cohort drains",
            clientId, spawnId);
        return false;
    }
    if (graphGate == OwningPawnGraphGateResult::Failed) {
        Logger::Error(
            "[Deployment] client %u could not prepare the owning pawn "
            "graph for team %u",
            clientId, deploymentTeam);
        return false;
    }

    const auto reopenAfterPrecommitFailure =
        [&](const char* context) {
        RevokePreparedDeploymentAuthorization(clientId);
        if (!SendChangedRoleSpawnSelect(clientId)) {
            FailCloseCh2Publication(
                clientId, context);
        }
    };

    if (!spawns) {
        Logger::Warn(
            "[Deployment] client %u has no SpawnSystem at commit time",
            clientId);
        reopenAfterPrecommitFailure(
            "missing-spawn-system ChangedRole recovery");
        return false;
    }
    const auto preparedSpawn =
        spawns->PreparePlayerSpawn(clientId, resolvedSpawnId);
    if (!preparedSpawn) {
        Logger::Warn(
            "[Deployment] client %u could not prepare authoritative spawn "
            "%u; reopening spawn selection",
            clientId, spawnId);
        reopenAfterPrecommitFailure(
            "failed-spawn-plan ChangedRole recovery");
        return false;
    }

    const uint64_t anticipatedPawnGeneration =
        AnticipatedOwningPawnGeneration(cs);
    if (!PreflightPawnSpawn(
            clientId, anticipatedPawnGeneration,
            preparedSpawn->GetPosition())) {
        Logger::Warn(
            "[Deployment] client %u could not preflight the owning pawn "
            "publication before spawn %u; authority remains unmodified",
            clientId, spawnId);
        reopenAfterPrecommitFailure(
            "pawn-graph-preflight ChangedRole recovery");
        return false;
    }

    if (!spawns->CommitPreparedPlayerSpawn(*preparedSpawn)) {
        Logger::Warn(
            "[Deployment] client %u spawn %u changed after publication "
            "preflight; authority remains unmodified",
            clientId, spawnId);
        reopenAfterPrecommitFailure(
            "stale-spawn-plan ChangedRole recovery");
        return false;
    }

    Logger::Info("[Deployment] client %u deploying from selected group row %u "
                 "at authoritative PlayerStart %u", clientId, spawnId,
                 resolvedSpawnId);
    const uint64_t pawnGeneration = cs.owningPawnGeneration;
    if (pawnGeneration != anticipatedPawnGeneration ||
        !cs.owningPawnAlive ||
        !SendPawnSpawn(clientId, pawnGeneration)) {
        // Every ordinary fallible step completed in preflight. A failure after
        // SpawnSystem commit is therefore an internal publication invariant,
        // not a recoverable gameplay death. The client may have observed part
        // of the reliable cohort, so fail the connection closed instead of
        // fabricating a rollback that cannot retract wire state.
        FailOwningPawnGraph(
            clientId, "post-commit pawn publication invariant");
        Logger::Error(
            "[Deployment] client %u committed spawn %u but could not publish "
            "the preflighted owning graph; session retired",
            clientId, spawnId);
        return false;
    }

    if (auto* players = m_server->GetPlayerManager()) {
        if (auto player = players->GetPlayer(clientId)) {
            player->SetReadyToSpawn(true);
        }
    }
    BindPossessionRecovery(cs, pawnGeneration);
    cs.spawned = true;
    return true;
}

void ConnectionManager::BeginDeploymentGeneration() {
    ++m_deploymentGeneration;
    m_deploymentCoordinator.ResetForGeneration(m_deploymentGeneration);
    m_deploymentCountdown.ResetForGeneration(m_deploymentGeneration);

    for (const auto& connection : GetAllConnections()) {
        if (!connection || !connection->IsUE3Client() ||
            !connection->IsHandshakeComplete()) {
            continue;
        }

        const uint32_t clientId = connection->GetClientId();
        const auto stateIt = m_controlState.find(clientId);
        if (stateIt == m_controlState.end() ||
            stateIt->second.mapTravelPending) {
            continue;
        }
        ControlState& cs = stateIt->second;
        cs.spawned = false;
        cs.deferredOwningPawnGraphDeployment.reset();
        cs.owningPawnAlive = false;
        cs.possessionAckedGeneration = 0u;
        cs.possessionRecoveryGeneration = 0u;
        ResetPossessionRecovery(cs);
        cs.movementInputValid = false;
        cs.latestMovementHandle = 0;
        cs.latestMoveFlags = 0;
        cs.latestViewValid = false;
        cs.latestPackedView = 0;
        cs.useHeld = false;
        cs.mantleAttemptPending = false;
        cs.mantlePawnStarted = false;
        cs.specialMoveActive = false;
        cs.specialMove = 0;
        cs.activeWeaponChannel = 0;
        cs.griActiveStatePublished = false;
        cs.weaponIntent.fill({});
        if (m_server) {
            m_server->CancelRetailGrenadeCook(clientId);
            if (auto* players = m_server->GetPlayerManager()) {
                if (auto player = players->GetPlayer(clientId)) {
                    player->SetReadyToSpawn(false);
                    player->SetState(PlayerState::Dead);
                    (void)ResetRetailMovementValidation(
                        clientId, player->GetPosition());
                }
            }
        }

        m_deploymentCoordinator.ResetClient(clientId);
        if (cs.teamSelected && cs.roleFinalized) {
            m_deploymentCoordinator.FinalizeRole(clientId);
            if (!SendChangedRoleSpawnSelect(clientId)) {
                FailCloseCh2Publication(
                    clientId, "round-generation ChangedRole transition");
            }
        }
    }

    Logger::Info("[Deployment] reset workflow for round generation %llu",
                 static_cast<unsigned long long>(m_deploymentGeneration));
}

void ConnectionManager::UpdateRetailDeploymentCountdown() {
    const DeploymentPhaseState phase = GetDeploymentPhaseState();
    const bool enteredActive =
        phase.phase == DeploymentCountdown::Phase::Active &&
        (!m_lastDeploymentPhase.has_value() ||
         *m_lastDeploymentPhase != DeploymentCountdown::Phase::Active);

    // A fresh Preparation following any other phase is a new authorization
    // generation. Existing one-shot approvals must never carry into a round.
    if (m_lastDeploymentPhase.has_value() &&
        *m_lastDeploymentPhase != DeploymentCountdown::Phase::Preparation &&
        phase.phase == DeploymentCountdown::Phase::Preparation) {
        BeginDeploymentGeneration();
    }
    m_lastDeploymentPhase = phase.phase;

    const auto global = m_deploymentCountdown.Advance(
        phase.phase, phase.remainingSeconds);
    if (global.deployPreparedClients) {
        for (const auto& [clientId, spawnId] :
             m_deploymentCoordinator.GetPreparedDeployments()) {
            ExecutePreparedDeployment(clientId, spawnId);
        }
    }

    for (const auto& connection : GetAllConnections()) {
        if (!connection || !connection->IsUE3Client() ||
            !connection->IsHandshakeComplete()) {
            continue;
        }
        const uint32_t clientId = connection->GetClientId();
        const auto stateIt = m_controlState.find(clientId);
        if (stateIt == m_controlState.end() ||
            stateIt->second.mapTravelPending) {
            continue;
        }
        if (enteredActive) {
            // The objective broadcaster is event-driven, so a quiet objective
            // system cannot be trusted to publish the match-start transition.
            SendRetailObjectiveState(clientId, /*baseline=*/false);
        }
        if (!stateIt->second.spawned) continue;

        const auto actions = m_deploymentCountdown.SyncClient(
            clientId, phase.phase, phase.remainingSeconds);
        if (actions.showRoundStartScreen) {
            if (!SendShowRoundStartScreen(clientId,
                                          actions.displaySeconds)) {
                FailCloseCh2Publication(
                    clientId, "one-shot round-start screen show");
                continue;
            }
        }
        if (actions.hideRoundStartScreen) {
            if (!SendHideRoundStartScreen(clientId)) {
                FailCloseCh2Publication(
                    clientId, "one-shot round-start screen hide");
            }
        }
    }
}

bool ConnectionManager::ShouldAdvanceRetailRoundClock() const {
    const DeploymentCountdown::Phase phase = GetDeploymentPhaseState().phase;
    bool hasJoinedRetailClient = false;
    bool hasReadyRetailClient = false;
    for (const auto& [address, connection] : m_clients) {
        (void)address;
        if (!connection || !connection->IsUE3Client() ||
            !connection->IsHandshakeComplete()) {
            continue;
        }
        const auto controlIt = m_controlState.find(connection->GetClientId());
        if (controlIt == m_controlState.end() ||
            controlIt->second.mapTravelPending) {
            continue;
        }
        hasJoinedRetailClient = true;
        const auto state = m_deploymentCoordinator.GetClientState(
            connection->GetClientId());
        if (state.has_value() && state->roleFinalized) {
            hasReadyRetailClient = true;
            break;
        }
    }
    return EvaluateRetailRoundClockPolicy(
        phase, m_waitForReadyPlayer, hasJoinedRetailClient,
        hasReadyRetailClient);
}

bool ConnectionManager::EvaluateRetailRoundClockPolicy(
    DeploymentCountdown::Phase phase, bool waitForReadyPlayer,
    bool hasJoinedRetailClient, bool hasReadyRetailClient) {
    if (phase != DeploymentCountdown::Phase::Preparation) return true;
    if (hasJoinedRetailClient && hasReadyRetailClient) return true;

    // A joined player always gets time to finish team and role selection. When
    // enabled, the same gate also keeps an empty server at the start of its
    // preparation phase so bots cannot consume objectives before the first
    // retail participant arrives. Disabling the empty-server gate preserves
    // the legacy headless bot-simulation behavior.
    if (hasJoinedRetailClient) return false;
    return !waitForReadyPlayer;
}

bool ConnectionManager::ResolveObjectiveConnectedToBase(
    bool authoredConnectedToBase, const SupremacyMode* supremacy,
    uint32_t objectiveId, uint32_t controllingTeam) {
    // Retail skips its entire connectivity clear/recompute pass when either
    // home-base marker is absent. Preserve the map-authored bit in that mode;
    // it is ignored for scoring but remains part of the replicated objective
    // state. With both bases present, publish the live supply-line result.
    if (!supremacy || !supremacy->UsesSupplyLines() ||
        (controllingTeam != SupremacyMode::kSouthTeamId &&
         controllingTeam != SupremacyMode::kNorthTeamId)) {
        return authoredConnectedToBase;
    }
    return supremacy->IsObjectiveLinked(objectiveId, controllingTeam);
}

bool ConnectionManager::IsRetailGameplayActive(uint32_t clientId) const {
    const auto it = m_controlState.find(clientId);
    if (it == m_controlState.end()) return false;
    const ControlState& state = it->second;
    const std::shared_ptr<ClientConnection> connection =
        GetConnection(clientId);
    if (!connection || connection->IsDisconnected() ||
        !connection->IsUE3Client() ||
        !connection->IsHandshakeComplete() || state.mapTravelPending ||
        !state.spawned || !state.pawnGraphOpen ||
        state.pawnGraphPhase != OwningPawnGraphPhase::Open ||
        !HasLiveOwningPawnGeneration(
            state, state.owningPawnGeneration)) {
        return false;
    }
    return GetDeploymentPhaseState().phase == DeploymentCountdown::Phase::Active;
}

bool ConnectionManager::BuildRemoteParticipantInitialState(
    const ParticipantId& participant,
    const DeploymentRepl::RetailParticipantCombatState* combatOverride,
    DeploymentRepl::RetailParticipantInitialState& output) const {
    if (!participant.IsValid() ||
        (combatOverride && combatOverride->participant != participant) ||
        !m_server) {
        return false;
    }

    DeploymentRepl::RetailParticipantInitialState state;
    state.combat.participant = participant;
    if (participant.IsHuman()) {
        const std::shared_ptr<ClientConnection> connection =
            GetConnection(participant.value);
        PlayerManager* players = m_server->GetPlayerManager();
        const std::shared_ptr<Player> player =
            players ? players->GetPlayer(participant.value) : nullptr;
        if (!connection || connection->IsDisconnected() || !player) return false;

        const uint32_t playerTeam = player->GetTeam();
        if (playerTeam != 1u && playerTeam != 2u) return false;
        state.serverTeamId = static_cast<uint8_t>(playerTeam);
        state.playerName = connection->GetPlayerName();
        if (state.playerName.empty()) {
            state.playerName = "Player" + std::to_string(participant.value);
        }
        state.combat.health = std::clamp(player->GetHealth(), 0, 100);
        state.combat.kills = players->GetPlayerKills(participant.value);
        state.combat.deaths = players->GetPlayerDeaths(participant.value);
        state.combat.score = players->GetPlayerScore(participant.value);
        state.combat.dead = !player->IsAlive();
        state.positionUu = player->GetPosition();
        const auto participantState = m_controlState.find(participant.value);
        state.pawnPresent = player->IsAlive() &&
            participantState != m_controlState.end() &&
            participantState->second.pawnGraphOpen &&
            IsRetailGameplayActive(participant.value);
    } else if (participant.IsBot()) {
        BotManager* bots = m_server->GetBotManager();
        const BotSnapshot* bot = bots ? bots->FindBot(participant) : nullptr;
        if (!bot || !std::isfinite(bot->health)) return false;

        state.serverTeamId = bot->teamId;
        state.playerName = bot->name.empty()
            ? ("Bot" + std::to_string(participant.value))
            : bot->name;
        state.combat.health = std::clamp(
            static_cast<int>(std::lround(bot->health)), 0, 100);
        state.combat.kills = 0;
        state.combat.deaths = static_cast<int>(std::min<std::uint32_t>(
            bot->deathSequence, static_cast<std::uint32_t>(INT_MAX)));
        state.combat.score = 0;
        state.combat.dead = bot->lifecycle != BotLifecycle::Alive;
        state.positionUu = bot->position;
        state.pawnPresent = bot->lifecycle == BotLifecycle::Alive &&
            GetDeploymentPhaseState().phase ==
                DeploymentCountdown::Phase::Active;
    } else {
        return false;
    }

    // Retail FString is ANSI and actor-open bursts must remain comfortably
    // below the client's small receive buffer.  Names are already validated at
    // login, but bound again at this protocol boundary.
    const std::size_t embeddedNull = state.playerName.find('\0');
    if (embeddedNull != std::string::npos) state.playerName.resize(embeddedNull);
    if (state.playerName.size() >
        DeploymentRepl::kMaximumRetailPlayerNameBytes) {
        state.playerName.resize(
            DeploymentRepl::kMaximumRetailPlayerNameBytes);
    }
    if (state.playerName.empty()) return false;

    if (combatOverride) state.combat = *combatOverride;
    if (!DeploymentRepl::IsValidRetailParticipantInitialState(state)) {
        return false;
    }
    output = std::move(state);
    return true;
}

ConnectionManager::RemotePriOpenResult
ConnectionManager::QueueRemoteParticipantPriOpen(
    uint32_t viewerClientId,
    const DeploymentRepl::RetailParticipantInitialState& participant,
    std::vector<PacketCodec::Bunch>& output) {
    const auto connection = GetConnection(viewerClientId);
    const auto stateIt = m_controlState.find(viewerClientId);
    if (!connection || connection->IsDisconnected() ||
        !connection->IsUE3Client() || !connection->IsHandshakeComplete() ||
        stateIt == m_controlState.end() || stateIt->second.mapTravelPending ||
        !stateIt->second.teamSelected ||
        !DeploymentRepl::IsValidRetailParticipantInitialState(participant) ||
        (participant.combat.participant.IsHuman() &&
         participant.combat.participant.value == viewerClientId)) {
        return RemotePriOpenResult::Failed;
    }

    const std::optional<RetailBootstrap::ArtifactSelection>& selectedArtifact =
        GetRetailArtifactSelection(viewerClientId);
    if (!selectedArtifact ||
        selectedArtifact->roGame.playerReplicationInfoClassRef == 0) {
        Logger::Warn(
            "[ParticipantReplication] viewer %u has no grounded ROGame PRI "
            "class for its frozen artifact; remote PRI open rejected",
            viewerClientId);
        return RemotePriOpenResult::Failed;
    }

    ControlState& cs = stateIt->second;
    ParticipantActorChannelBinding* binding =
        cs.remoteParticipants.Ensure(participant.combat.participant);
    if (!binding) {
        Logger::Warn(
            "[ParticipantReplication] viewer %u exhausted or rejected remote "
            "participant channels for %s %u",
            viewerClientId,
            participant.combat.participant.IsBot() ? "bot" : "human",
            participant.combat.participant.value);
        return RemotePriOpenResult::Failed;
    }
    const uint8_t retailTeam =
        TeamMapping::ServerToRetail(participant.serverTeamId);
    if (retailTeam > TeamMapping::kRetailUs) {
        return RemotePriOpenResult::Failed;
    }
    const uint32_t teamInfoChannel = cs.teamInfoChannels[retailTeam];
    BitWriter teamProperties;
    if (!DeploymentRepl::WriteRemotePriTeam(
            teamProperties, teamInfoChannel)) {
        return RemotePriOpenResult::Failed;
    }

    if (binding->priState == ParticipantActorOpenState::Open) {
        if (binding->priTeamWireValid &&
            binding->priTeamInfoChannel == teamInfoChannel) {
            return RemotePriOpenResult::AlreadyOpen;
        }

        const std::optional<uint32_t> teamSequence =
            cs.remoteParticipants.NextPriReliableSequence(
                participant.combat.participant);
        if (!teamSequence) return RemotePriOpenResult::Failed;

        PacketCodec::Bunch team;
        team.bReliable = true;
        team.chIndex = binding->priChannel;
        team.chType = cs.actorChType;
        team.chSequence = *teamSequence;
        team.payload = teamProperties.GetBytes();
        team.payloadBits = static_cast<uint32_t>(teamProperties.NumBits());
        output.push_back(std::move(team));
        binding->priTeamWireValid = true;
        binding->priTeamInfoChannel = teamInfoChannel;
        return RemotePriOpenResult::UpdateQueued;
    }
    if (binding->priState == ParticipantActorOpenState::Closed) {
        return RemotePriOpenResult::Failed;
    }

    const std::optional<uint32_t> openSequence =
        cs.remoteParticipants.NextPriReliableSequence(
            participant.combat.participant);
    const std::optional<uint32_t> teamSequence =
        cs.remoteParticipants.NextPriReliableSequence(
            participant.combat.participant);
    if (!openSequence || !teamSequence) return RemotePriOpenResult::Failed;

    ActorRepl::ActorOpenHeader header;
    header.classRef = ActorRepl::NetGUIDRef{
        /*isDynamic=*/false,
        selectedArtifact->roGame.playerReplicationInfoClassRef};
    PacketCodec::Bunch open = ActorRepl::MakeOpeningActorBunch(
        binding->priChannel, *openSequence, header,
        [&](BitWriter& writer) {
            (void)DeploymentRepl::WriteRemotePriInitial(writer, participant);
        });

    PacketCodec::Bunch team;
    team.bReliable = true;
    team.chIndex = binding->priChannel;
    team.chType = cs.actorChType;
    team.chSequence = *teamSequence;
    team.payload = teamProperties.GetBytes();
    team.payloadBits = static_cast<uint32_t>(teamProperties.NumBits());

    output.push_back(std::move(open));
    output.push_back(std::move(team));
    if (!cs.remoteParticipants.MarkPriOpen(
            participant.combat.participant)) {
        output.resize(output.size() - 2u);
        return RemotePriOpenResult::Failed;
    }
    cs.remoteParticipants.SetDead(
        participant.combat.participant, participant.combat.dead);
    // The initial PRI property tail above always contains h61. Seed the
    // viewer-local wire cache now that the reliable open is queued; later
    // combat snapshots must publish h61 only when its value transitions.
    binding->priDeadWireValid = true;
    binding->priDeadWireValue = participant.combat.dead;
    binding->priTeamWireValid = true;
    binding->priTeamInfoChannel = teamInfoChannel;
    return RemotePriOpenResult::OpenQueued;
}

ConnectionManager::RemotePriOpenResult
ConnectionManager::QueueRemoteParticipantPawnOpen(
    uint32_t viewerClientId,
    const DeploymentRepl::RetailParticipantInitialState& participant,
    std::vector<PacketCodec::Bunch>& output) {
    const auto connection = GetConnection(viewerClientId);
    const auto stateIt = m_controlState.find(viewerClientId);
    if (!connection || connection->IsDisconnected() ||
        !connection->IsUE3Client() || !connection->IsHandshakeComplete() ||
        stateIt == m_controlState.end() || stateIt->second.mapTravelPending ||
        !stateIt->second.teamSelected ||
        !DeploymentRepl::IsValidRetailParticipantInitialState(participant) ||
        (participant.combat.participant.IsHuman() &&
         participant.combat.participant.value == viewerClientId)) {
        return RemotePriOpenResult::Failed;
    }

    ControlState& cs = stateIt->second;
    ParticipantActorChannelBinding* binding =
        cs.remoteParticipants.Find(participant.combat.participant);
    if (!binding ||
        binding->priState != ParticipantActorOpenState::Open) {
        return RemotePriOpenResult::Failed;
    }
    if (binding->pawnState == ParticipantActorOpenState::Open) {
        if (binding->pawnServerTeamValid &&
            binding->pawnServerTeamId == participant.serverTeamId) {
            return RemotePriOpenResult::AlreadyOpen;
        }

        const std::optional<uint32_t> closeSequence =
            cs.remoteParticipants.NextPawnReliableSequence(
                participant.combat.participant);
        if (!closeSequence) return RemotePriOpenResult::Failed;
        std::optional<PacketCodec::Bunch> close =
            DeploymentRepl::MakeRemotePawnCloseBunch(
                binding->pawnChannel, *closeSequence);
        if (!close || !cs.remoteParticipants.MarkPawnClosing(
                          participant.combat.participant)) {
            return RemotePriOpenResult::Failed;
        }

        (void)cs.remoteParticipants.SetDead(
            participant.combat.participant, true);
        output.push_back(std::move(*close));
        return RemotePriOpenResult::UpdateQueued;
    }
    if (participant.combat.dead || !participant.pawnPresent) {
        return RemotePriOpenResult::Failed;
    }
    if (!DeploymentRepl::kRemotePawnVisualTemplatesGrounded ||
        !DeploymentRepl::RemotePawnArchetypeForServerTeam(
            participant.serverTeamId)) {
        return RemotePriOpenResult::Failed;
    }
    if (binding->pawnState != ParticipantActorOpenState::Unopened) {
        return RemotePriOpenResult::Failed;
    }

    DeploymentRepl::RetailRemotePawnSnapshot snapshot;
    snapshot.participant = participant.combat.participant;
    snapshot.positionUu = participant.positionUu;
    snapshot.health = participant.combat.health;
    if (!DeploymentRepl::IsValidRetailRemotePawnSnapshot(snapshot)) {
        return RemotePriOpenResult::Failed;
    }

    const std::optional<uint32_t> sequence =
        cs.remoteParticipants.NextPawnReliableSequence(
            participant.combat.participant);
    if (!sequence) return RemotePriOpenResult::Failed;

    std::optional<PacketCodec::Bunch> open =
        DeploymentRepl::MakeRemotePawnOpeningBunch(
            binding->pawnChannel, *sequence, participant.serverTeamId,
            binding->priChannel, snapshot);
    if (!open) return RemotePriOpenResult::Failed;

    const uint32_t generation = binding->pawnGeneration;
    if (!cs.remoteParticipants.MarkPawnOpen(
            participant.combat.participant, generation)) {
        return RemotePriOpenResult::Failed;
    }
    output.push_back(std::move(*open));

    ControlState::RemotePawnViewState& view =
        cs.remotePawnViews[participant.combat.participant];
    view.lastSnapshot = snapshot;
    view.lastMovementSendMs = NowMs();
    view.closeDueMs = 0;
    view.snapshotValid = true;
    view.deathCoreSent = false;
    (void)cs.remoteParticipants.SetDead(
        participant.combat.participant, false);
    binding->pawnServerTeamValid = true;
    binding->pawnServerTeamId = participant.serverTeamId;
    return RemotePriOpenResult::OpenQueued;
}

bool ConnectionManager::QueueRemoteParticipantPawnDeath(
    uint32_t viewerClientId, const ParticipantId& participant,
    uint64_t nowMs, std::vector<PacketCodec::Bunch>& output) {
    const auto stateIt = m_controlState.find(viewerClientId);
    if (stateIt == m_controlState.end() || !participant.IsValid()) return false;

    ControlState& cs = stateIt->second;
    ParticipantActorChannelBinding* binding =
        cs.remoteParticipants.Find(participant);
    if (!binding ||
        binding->pawnState != ParticipantActorOpenState::Open) {
        return false;
    }

    ControlState::RemotePawnViewState& view =
        cs.remotePawnViews[participant];
    if (view.deathCoreSent) return false;

    // Resolver visibility is revoked before any death bytes are exposed to the
    // client, so a same-pump hit cannot target a dying pawn.
    if (!cs.remoteParticipants.SetDead(participant, true)) return false;

    std::optional<PacketCodec::Bunch> death =
        DeploymentRepl::MakeRemotePawnDeathBunch(binding->pawnChannel);
    if (!death) return false;
    output.push_back(std::move(*death));

    view.deathCoreSent = true;
    view.closeDueMs = nowMs + DeploymentRepl::kRemotePawnCloseDelayMs;
    return true;
}

void ConnectionManager::SynchronizeRemoteParticipantPris(
    uint32_t viewerClientId) {
    const auto stateIt = m_controlState.find(viewerClientId);
    if (stateIt == m_controlState.end() || stateIt->second.mapTravelPending) {
        return;
    }

    // Keep well below the retail ~1280-byte receive ceiling.  Flush after a
    // participant pushes this estimate over 768 bytes; the largest bounded
    // name/open pair still leaves ample bunch-header margin.
    constexpr size_t kBatchBitBudget = 6144;
    std::vector<PacketCodec::Bunch> batch;
    size_t batchBits = 0;
    size_t openedPriCount = 0;
    size_t openedPawnCount = 0;
    auto flush = [&]() {
        if (batch.empty()) return;
        SendReliableBunches(viewerClientId, batch);
        batch.clear();
        batchBits = 0;
    };
    auto append = [&](const ParticipantId& id) {
        DeploymentRepl::RetailParticipantInitialState initial;
        if (!BuildRemoteParticipantInitialState(id, nullptr, initial)) return;
        const size_t before = batch.size();
        const RemotePriOpenResult priResult =
            QueueRemoteParticipantPriOpen(viewerClientId, initial, batch);
        if (priResult == RemotePriOpenResult::Failed) return;
        if (priResult == RemotePriOpenResult::OpenQueued) ++openedPriCount;

        const RemotePriOpenResult pawnResult =
            QueueRemoteParticipantPawnOpen(viewerClientId, initial, batch);
        if (pawnResult == RemotePriOpenResult::OpenQueued) ++openedPawnCount;
        for (size_t i = before; i < batch.size(); ++i) {
            batchBits += static_cast<size_t>(batch[i].payloadBits) + 64u;
        }
        if (batchBits >= kBatchBitBudget) flush();
    };

    for (const auto& entry : m_clients) {
        const std::shared_ptr<ClientConnection>& participant = entry.second;
        if (!participant || participant->IsDisconnected()) continue;
        append(ParticipantId::Human(participant->GetClientId()));
    }
    if (m_server) {
        if (BotManager* bots = m_server->GetBotManager()) {
            for (const BotSnapshot& bot : bots->GetBots()) append(bot.id);
        }
    }
    flush();

    if (openedPriCount != 0 || openedPawnCount != 0) {
        Logger::Info(
            "[ParticipantReplication] viewer %u queued %zu remote PRI and %zu "
            "pawn actor open(s); pawn visuals remain gated until complete captured "
            "role/class templates are grounded",
            viewerClientId, openedPriCount, openedPawnCount);
    }
}

void ConnectionManager::SynchronizeAllRemoteParticipantPris() {
    for (const auto& entry : m_clients) {
        const std::shared_ptr<ClientConnection>& viewer = entry.second;
        if (!viewer || viewer->IsDisconnected() || !viewer->IsUE3Client() ||
            !viewer->IsHandshakeComplete()) {
            continue;
        }
        SynchronizeRemoteParticipantPris(viewer->GetClientId());
    }
}

void ConnectionManager::ReplicateRemoteParticipantPawnsTick() {
    const uint64_t now = NowMs();
    constexpr double kTwoPi = 6.28318530717958647692;

    for (auto& [viewerClientId, cs] : m_controlState) {
        const auto viewer = GetConnection(viewerClientId);
        if (!viewer || viewer->IsDisconnected() || !viewer->IsUE3Client() ||
            !viewer->IsHandshakeComplete() || cs.mapTravelPending ||
            !cs.teamSelected) {
            continue;
        }
        if (cs.lastRemotePawnReplicationMs != 0 &&
            now - cs.lastRemotePawnReplicationMs <
                DeploymentRepl::kRemotePawnMovementIntervalMs) {
            continue;
        }
        cs.lastRemotePawnReplicationMs = now;

        // This also discovers participants added after the viewer selected a
        // role. Existing bindings make it a no-op except for a pending pawn
        // incarnation whose PRI dependency is already open.
        SynchronizeRemoteParticipantPris(viewerClientId);

        std::vector<ParticipantId> tracked;
        tracked.reserve(cs.remotePawnViews.size());
        for (const auto& [participant, view] : cs.remotePawnViews) {
            (void)view;
            tracked.push_back(participant);
        }

        constexpr size_t kSnapshotBatchBitBudget = 6144;
        std::vector<PacketCodec::Bunch> outbound;
        size_t outboundBits = 0;
        auto flushOutbound = [&]() {
            if (outbound.empty()) return;
            SendReliableBunches(viewerClientId, outbound);
            outbound.clear();
            outboundBits = 0;
        };
        auto accountAdded = [&](size_t firstAdded) {
            for (size_t i = firstAdded; i < outbound.size(); ++i) {
                outboundBits +=
                    static_cast<size_t>(outbound[i].payloadBits) + 64u;
            }
            if (outboundBits >= kSnapshotBatchBitBudget) flushOutbound();
        };
        for (const ParticipantId& participant : tracked) {
            DeploymentRepl::RetailParticipantInitialState authoritative;
            if (!BuildRemoteParticipantInitialState(
                    participant, nullptr, authoritative)) {
                continue;
            }

            ParticipantActorChannelBinding* binding =
                cs.remoteParticipants.Find(participant);
            auto viewIt = cs.remotePawnViews.find(participant);
            if (!binding || viewIt == cs.remotePawnViews.end()) continue;
            ControlState::RemotePawnViewState& view = viewIt->second;

            // Once death is published, the actor incarnation must close even
            // if authoritative respawn occurs before the 4.7-second delay.
            // Respawn remains parked behind Closing until the ACK/drain gate.
            if (view.deathCoreSent &&
                binding->pawnState == ParticipantActorOpenState::Open &&
                now >= view.closeDueMs) {
                const std::optional<uint32_t> closeSequence =
                    cs.remoteParticipants.NextPawnReliableSequence(
                        participant);
                std::optional<PacketCodec::Bunch> close;
                if (closeSequence) {
                    close = DeploymentRepl::MakeRemotePawnCloseBunch(
                        binding->pawnChannel, *closeSequence);
                }
                if (close &&
                    cs.remoteParticipants.MarkPawnClosing(participant)) {
                    const size_t before = outbound.size();
                    outbound.push_back(std::move(*close));
                    accountAdded(before);
                }
                continue;
            }

            if (authoritative.combat.dead) {
                const size_t before = outbound.size();
                if (!QueueRemoteParticipantPawnDeath(
                        viewerClientId, participant, now, outbound)) {
                    (void)cs.remoteParticipants.SetDead(participant, true);
                }
                accountAdded(before);
                continue;
            }

            if (binding->dead) {
                if (binding->pawnState != ParticipantActorOpenState::Closed) {
                    continue; // death close not yet safely ACKed/drained
                }
                if (!cs.remoteParticipants.BeginPawnIncarnation(participant)) {
                    continue;
                }
                std::vector<PacketCodec::Bunch> reopen;
                if (QueueRemoteParticipantPawnOpen(
                        viewerClientId, authoritative, reopen) ==
                    RemotePriOpenResult::OpenQueued) {
                    const size_t before = outbound.size();
                    outbound.insert(outbound.end(),
                                    std::make_move_iterator(reopen.begin()),
                                    std::make_move_iterator(reopen.end()));
                    accountAdded(before);
                }
                continue;
            }

            if (binding->pawnState == ParticipantActorOpenState::Unopened) {
                std::vector<PacketCodec::Bunch> open;
                if (QueueRemoteParticipantPawnOpen(
                        viewerClientId, authoritative, open) ==
                    RemotePriOpenResult::OpenQueued) {
                    const size_t before = outbound.size();
                    outbound.insert(outbound.end(),
                                    std::make_move_iterator(open.begin()),
                                    std::make_move_iterator(open.end()));
                    accountAdded(before);
                }
                continue;
            }
            if (binding->pawnState != ParticipantActorOpenState::Open ||
                !IsRetailGameplayActive(viewerClientId)) {
                continue;
            }

            DeploymentRepl::RetailRemotePawnSnapshot snapshot;
            snapshot.participant = participant;
            snapshot.positionUu = authoritative.positionUu;
            snapshot.health = authoritative.combat.health;
            if (view.snapshotValid && view.lastMovementSendMs < now) {
                const float seconds = static_cast<float>(
                    static_cast<double>(now - view.lastMovementSendMs) /
                    1000.0);
                snapshot.velocityUuPerSecond = {
                    (snapshot.positionUu.x - view.lastSnapshot.positionUu.x) /
                        seconds,
                    (snapshot.positionUu.y - view.lastSnapshot.positionUu.y) /
                        seconds,
                    (snapshot.positionUu.z - view.lastSnapshot.positionUu.z) /
                        seconds};
                const double horizontal = std::hypot(
                    static_cast<double>(snapshot.velocityUuPerSecond.x),
                    static_cast<double>(snapshot.velocityUuPerSecond.y));
                snapshot.yaw = view.lastSnapshot.yaw;
                if (horizontal > 0.5) {
                    double radians = std::atan2(
                        static_cast<double>(snapshot.velocityUuPerSecond.y),
                        static_cast<double>(snapshot.velocityUuPerSecond.x));
                    if (radians < 0.0) radians += kTwoPi;
                    const uint32_t units = static_cast<uint32_t>(
                        std::lround(radians * 65536.0 / kTwoPi));
                    snapshot.yaw = static_cast<uint16_t>(units & 0xFFFFu);
                }
            }

            std::optional<PacketCodec::Bunch> movement =
                DeploymentRepl::MakeRemotePawnMovementBunch(
                    binding->pawnChannel, snapshot);
            if (!movement) {
                Logger::Warn(
                    "[ParticipantReplication] viewer %u skipped invalid remote "
                    "movement for %s %u",
                    viewerClientId, participant.IsBot() ? "bot" : "human",
                    participant.value);
                continue;
            }
            const size_t before = outbound.size();
            outbound.push_back(std::move(*movement));
            accountAdded(before);
            view.lastSnapshot = snapshot;
            view.lastMovementSendMs = now;
            view.snapshotValid = true;
        }

        flushOutbound();
    }
}

void ConnectionManager::RetireRemoteParticipantFromViewers(
    const ParticipantId& participant) {
    if (!participant.IsValid()) return;
    for (auto& entry : m_controlState) {
        const uint32_t viewerClientId = entry.first;
        ControlState& cs = entry.second;
        const ParticipantActorChannelBinding* found =
            cs.remoteParticipants.Find(participant);
        if (!found) continue;
        const ParticipantActorChannelBinding binding = *found;

        std::vector<PacketCodec::Bunch> closes;
        auto appendClose = [&](uint32_t channel, bool open, bool pri) {
            if (!open) return;
            const std::optional<uint32_t> sequence = pri
                ? cs.remoteParticipants.NextPriReliableSequence(participant)
                : cs.remoteParticipants.NextPawnReliableSequence(participant);
            if (!sequence) return;
            PacketCodec::Bunch close;
            close.bControl = true;
            close.bClose = true;
            close.bReliable = true;
            close.chIndex = channel;
            close.chType = cs.actorChType;
            close.chSequence = *sequence;
            closes.push_back(std::move(close));
        };
        appendClose(binding.priChannel,
                    binding.priState == ParticipantActorOpenState::Open, true);
        appendClose(binding.pawnChannel,
                    binding.pawnState == ParticipantActorOpenState::Open, false);

        const auto viewer = GetConnection(viewerClientId);
        if (!closes.empty() && viewer && !viewer->IsDisconnected() &&
            viewer->IsUE3Client() && viewer->IsHandshakeComplete()) {
            SendReliableBunches(viewerClientId, closes);
        }
        cs.participantPawnCloseAcknowledged.reset(binding.pawnChannel);
        cs.remotePawnViews.erase(participant);
        cs.remoteParticipants.Retire(participant);
    }
}

void ConnectionManager::RemoveRetailParticipant(
    const ParticipantId& participant) {
    RetireRemoteParticipantFromViewers(participant);
}

void ConnectionManager::BroadcastRetailM61Spawn(
    uint64_t projectileKey, uint32_t shooterClientId,
    const WeaponCombatRepl::M61VisualSnapshot& sourceSnapshot) {
    if (projectileKey == 0 || shooterClientId == 0) return;
    for (const auto& connection : GetAllConnections()) {
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() || !connection->IsHandshakeComplete()) {
            continue;
        }
        const uint32_t viewerClientId = connection->GetClientId();
        const auto stateIt = m_controlState.find(viewerClientId);
        if (stateIt == m_controlState.end() ||
            !stateIt->second.teamSelected ||
            !stateIt->second.pawnGraphOpen ||
            !IsRetailGameplayActive(viewerClientId)) {
            continue;
        }

        ControlState& state = stateIt->second;
        WeaponCombatRepl::M61VisualSnapshot snapshot = sourceSnapshot;
        snapshot.instigator.reset();
        if (viewerClientId == shooterClientId && state.pawnGraphOpen) {
            snapshot.instigator = ActorRepl::NetGUIDRef{
                /*isDynamic=*/true, kLocalPawnChannel};
        } else if (const ParticipantActorChannelBinding* shooter =
                       state.remoteParticipants.Find(
                           ParticipantId::Human(shooterClientId));
                   shooter &&
                   shooter->pawnState == ParticipantActorOpenState::Open) {
            snapshot.instigator = ActorRepl::NetGUIDRef{
                /*isDynamic=*/true, shooter->pawnChannel};
        }

        WeaponCombatRepl::M61VisualBatchResult encoded =
            state.m61Visuals.Spawn(
                projectileKey, snapshot,
                [&state](uint32_t channel) {
                    return channel < ActorRepl::kDynamicChannelMax &&
                           !state.outboundActorChannels.test(channel);
                });
        if (!encoded.valid()) {
            Logger::Warn(
                "[M61Visual] viewer %u rejected projectile %llu spawn "
                "(error %u)",
                viewerClientId,
                static_cast<unsigned long long>(projectileKey),
                static_cast<unsigned>(encoded.error));
            continue;
        }
        SendReliableBunches(viewerClientId, encoded.bunches);
    }
}

void ConnectionManager::BroadcastRetailM61Update(
    uint64_t projectileKey,
    const WeaponCombatRepl::M61VisualSnapshot& sourceSnapshot) {
    if (projectileKey == 0) return;
    for (auto& [viewerClientId, state] : m_controlState) {
        if (!state.m61Visuals.ChannelFor(projectileKey)) continue;
        const auto connection = GetConnection(viewerClientId);
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() || !connection->IsHandshakeComplete()) {
            continue;
        }
        WeaponCombatRepl::M61VisualSnapshot snapshot = sourceSnapshot;
        snapshot.instigator.reset();
        WeaponCombatRepl::M61VisualBatchResult encoded =
            state.m61Visuals.Update(projectileKey, snapshot);
        if (!encoded.valid()) {
            Logger::Warn(
                "[M61Visual] viewer %u rejected projectile %llu update "
                "(error %u)",
                viewerClientId,
                static_cast<unsigned long long>(projectileKey),
                static_cast<unsigned>(encoded.error));
            continue;
        }
        SendReliableBunches(viewerClientId, encoded.bunches);
    }
}

void ConnectionManager::BroadcastRetailM61Detonate(uint64_t projectileKey,
                                                    float fuseSeconds) {
    if (projectileKey == 0) return;
    for (auto& [viewerClientId, state] : m_controlState) {
        if (!state.m61Visuals.ChannelFor(projectileKey)) continue;
        const auto connection = GetConnection(viewerClientId);
        WeaponCombatRepl::M61VisualBatchResult encoded =
            state.m61Visuals.DetonateAndClose(projectileKey, fuseSeconds);
        if (!encoded.valid()) {
            Logger::Warn(
                "[M61Visual] viewer %u rejected projectile %llu detonation "
                "(error %u)",
                viewerClientId,
                static_cast<unsigned long long>(projectileKey),
                static_cast<unsigned>(encoded.error));
            continue;
        }
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() || !connection->IsHandshakeComplete()) {
            continue;
        }
        SendReliableBunches(viewerClientId, encoded.bunches);
    }
}

void ConnectionManager::BroadcastRetailM61Remove(uint64_t projectileKey) {
    if (projectileKey == 0) return;
    for (auto& [viewerClientId, state] : m_controlState) {
        if (!state.m61Visuals.ChannelFor(projectileKey)) continue;
        const auto connection = GetConnection(viewerClientId);
        WeaponCombatRepl::M61VisualBatchResult encoded =
            state.m61Visuals.Close(projectileKey);
        if (!encoded.valid()) continue;
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() || !connection->IsHandshakeComplete()) {
            continue;
        }
        SendReliableBunches(viewerClientId, encoded.bunches);
    }
}

void ConnectionManager::ReplicateRetailCombatState(
    uint32_t clientId, int health, int kills, int deaths, int score,
    bool isDead, bool sendHealth, bool sendDeathRpc) {
    ReplicateRetailParticipantCombatState(
        ParticipantId::Human(clientId), health, kills, deaths, score, isDead,
        sendHealth, sendDeathRpc);
}

void ConnectionManager::ReplicateRetailParticipantCombatState(
    const ParticipantId& participant, int health, int kills, int deaths,
    int score, bool isDead, bool sendHealth, bool sendDeathRpc) {
    DeploymentRepl::RetailParticipantCombatState combat{
        participant, health, kills, deaths, score, isDead};

    // A death/respawn RPC marks an authoritative pawn-lifecycle boundary in
    // both directions. Re-anchor even when the current SpawnSystem still uses
    // the same coordinates; stale velocity must not cross lives.
    if (participant.IsHuman() && sendDeathRpc && m_server) {
        if (auto* players = m_server->GetPlayerManager()) {
            if (auto player = players->GetPlayer(participant.value)) {
                (void)ResetRetailMovementValidation(
                    participant.value, player->GetPosition());
            }
        }
    }

    // An accepted death/respawn callback is the authoritative owning-pawn life
    // boundary. The fixed ch209 graph may survive it, but recovery and
    // possession acknowledgements never do. Bind a fresh non-zero generation
    // before any new-life delta can be emitted on the reused graph.
    if (participant.IsHuman() && sendDeathRpc) {
        const auto ownerState = m_controlState.find(participant.value);
        if (ownerState != m_controlState.end()) {
            ControlState& owner = ownerState->second;
            if (isDead) {
                if (owner.owningPawnAlive) {
                    owner.spawned = false;
                    owner.deferredOwningPawnGraphDeployment.reset();
                    owner.owningPawnAlive = false;
                    owner.possessionAckedGeneration = 0u;
                    owner.possessionRecoveryGeneration = 0u;
                    ResetPossessionRecovery(owner);
                } else {
                    Logger::Trace(
                        "[PawnLifecycle] client %u ignored duplicate dead "
                        "callback while generation %llu was already dead",
                        participant.value,
                        static_cast<unsigned long long>(
                            owner.owningPawnGeneration));
                }
            } else if (!owner.owningPawnAlive) {
                const uint64_t generation =
                    AdvanceOwningPawnGeneration(owner);
                owner.owningPawnAlive = true;
                if (owner.pawnGraphOpen) {
                    owner.spawned = true;
                    BindPossessionRecovery(owner, generation);
                } else {
                    owner.spawned = false;
                    InvalidatePossessionRecovery(owner);
                }
            } else {
                Logger::Trace(
                    "[PawnLifecycle] client %u ignored duplicate alive "
                    "callback for generation %llu",
                    participant.value,
                    static_cast<unsigned long long>(
                        owner.owningPawnGeneration));
            }
        }
    }

    // Owning gameplay intent belongs only to a human's fixed local graph.
    if (participant.IsHuman() && isDead) {
        const auto ownerState = m_controlState.find(participant.value);
        if (ownerState != m_controlState.end()) {
            ownerState->second.activeWeaponChannel = 0;
            ownerState->second.weaponIntent.fill({});
        }
        if (m_server) m_server->CancelRetailGrenadeCook(participant.value);
    }

    // Lifecycle advancement is authoritative and must not depend on whether
    // optional scoreboard fields are currently wire-encodable. A negative
    // script-adjusted score, for example, suppresses this delta but cannot
    // prevent the Dead -> Alive pawn generation from advancing after spawn.
    if (!DeploymentRepl::IsValidRetailParticipantCombatState(combat)) {
        Logger::Warn(
            "[PawnLifecycle] suppressed invalid retail combat delta for %s "
            "%u after applying its authoritative life boundary",
            participant.IsHuman() ? "client" : "bot", participant.value);
        return;
    }

    for (const auto& entry : m_clients) {
        const std::shared_ptr<ClientConnection>& connection = entry.second;
        if (!connection || connection->IsDisconnected() ||
            !connection->IsUE3Client() || !connection->IsHandshakeComplete()) {
            continue;
        }
        const uint32_t viewerClientId = connection->GetClientId();
        const auto stateIt = m_controlState.find(viewerClientId);
        if (stateIt == m_controlState.end()) continue;
        ControlState& cs = stateIt->second;

        const bool owningHuman = participant.IsHuman() &&
            participant.value == viewerClientId;
        std::vector<PacketCodec::Bunch> deltas;

        if (owningHuman) {
            const bool graphMatchesCurrentGeneration =
                cs.pawnGraphOpen && cs.pawnGraphGeneration != 0u &&
                cs.pawnGraphGeneration == cs.owningPawnGeneration;
            if (sendHealth && graphMatchesCurrentGeneration) {
                BitWriter pawn;
                if (DeploymentRepl::WriteRemotePawnHealth(pawn, combat)) {
                    PacketCodec::Bunch bunch;
                    bunch.chIndex = kLocalPawnChannel;
                    bunch.chType = cs.actorChType;
                    bunch.payload = pawn.GetBytes();
                    bunch.payloadBits = static_cast<uint32_t>(pawn.NumBits());
                    deltas.push_back(std::move(bunch));
                }
            }

            BitWriter pri;
            if (DeploymentRepl::WriteRemotePriCombat(pri, combat)) {
                PacketCodec::Bunch priBunch;
                priBunch.chIndex = 26;
                priBunch.chType = cs.actorChType;
                priBunch.payload = pri.GetBytes();
                priBunch.payloadBits = static_cast<uint32_t>(pri.NumBits());
                deltas.push_back(std::move(priBunch));
            }
            if (!deltas.empty()) SendReliableBunches(viewerClientId, deltas);

            // ClientOnDead is local-HUD/controller state.  Never send it to a
            // non-owning viewer for another human or bot.
            if (sendDeathRpc && graphMatchesCurrentGeneration) {
                BitWriter onDead;
                onDead.SerializeInt(151, kRoPcMaxHandle);
                onDead.WriteBit(isDead);
                if (!SendCh2Rpc(
                        viewerClientId, onDead.GetBytes(),
                        static_cast<uint32_t>(onDead.NumBits()),
                        isDead ? "ClientOnDead(combat)"
                               : "ClientOnDead(respawn)")) {
                    // This callback is the only causal HUD/life transition for
                    // the owning retail controller. The combat event is not
                    // replayed, so backpressure must establish a fresh session
                    // rather than leave client and authority on different lives.
                    FailCloseCh2Publication(
                        viewerClientId,
                        isDead ? "one-shot ClientOnDead combat transition"
                               : "one-shot ClientOnDead respawn transition");
                }
            }
            continue;
        }

        ParticipantActorChannelBinding* binding =
            cs.remoteParticipants.Find(participant);
        if (!binding ||
            binding->priState == ParticipantActorOpenState::Unopened) {
            DeploymentRepl::RetailParticipantInitialState initial;
            if (!BuildRemoteParticipantInitialState(
                    participant, &combat, initial)) {
                continue;
            }
            std::vector<PacketCodec::Bunch> opens;
            const RemotePriOpenResult priOpened = QueueRemoteParticipantPriOpen(
                viewerClientId, initial, opens);
            if (priOpened == RemotePriOpenResult::Failed) continue;
            if (!isDead) {
                (void)QueueRemoteParticipantPawnOpen(
                    viewerClientId, initial, opens);
            }
            if (!opens.empty()) SendReliableBunches(viewerClientId, opens);
            if (priOpened == RemotePriOpenResult::OpenQueued) {
                continue; // initial blocks already contain this combat state
            }
            binding = cs.remoteParticipants.Find(participant);
        }
        if (!binding ||
            binding->priState != ParticipantActorOpenState::Open) {
            continue;
        }

        if (isDead) {
            // QueueRemoteParticipantPawnDeath revokes resolver visibility before
            // appending the death core. A participant without an open pawn must
            // still be marked dead before any later h56 can inspect the binding.
            if (!QueueRemoteParticipantPawnDeath(
                    viewerClientId, participant, NowMs(), deltas)) {
                (void)cs.remoteParticipants.SetDead(participant, true);
            }
        } else if (binding->dead) {
            if (binding->pawnState == ParticipantActorOpenState::Unopened) {
                DeploymentRepl::RetailParticipantInitialState initial;
                if (BuildRemoteParticipantInitialState(
                        participant, &combat, initial)) {
                    std::vector<PacketCodec::Bunch> open;
                    if (QueueRemoteParticipantPawnOpen(
                            viewerClientId, initial, open) ==
                        RemotePriOpenResult::OpenQueued) {
                        SendReliableBunches(viewerClientId, open);
                    }
                }
                binding = cs.remoteParticipants.Find(participant);
            } else if (binding->pawnState ==
                       ParticipantActorOpenState::Closed) {
                DeploymentRepl::RetailParticipantInitialState initial;
                if (BuildRemoteParticipantInitialState(
                        participant, &combat, initial) &&
                    cs.remoteParticipants.BeginPawnIncarnation(participant)) {
                    std::vector<PacketCodec::Bunch> reopen;
                    if (QueueRemoteParticipantPawnOpen(
                            viewerClientId, initial, reopen) ==
                        RemotePriOpenResult::OpenQueued) {
                        SendReliableBunches(viewerClientId, reopen);
                    }
                }
                binding = cs.remoteParticipants.Find(participant);
            }
            if (!binding || binding->dead) {
                // Closing and unacknowledged-closed incarnations cannot respawn.
                const bool includeDead = binding &&
                    (!binding->priDeadWireValid ||
                     binding->priDeadWireValue != combat.dead);
                BitWriter pri;
                if (binding && DeploymentRepl::WriteRemotePriCombat(
                                   pri, combat, includeDead)) {
                    PacketCodec::Bunch priBunch;
                    priBunch.chIndex = binding->priChannel;
                    priBunch.chType = cs.actorChType;
                    priBunch.chSequence = 0;
                    priBunch.payload = pri.GetBytes();
                    priBunch.payloadBits =
                        static_cast<uint32_t>(pri.NumBits());
                    deltas.push_back(std::move(priBunch));
                }
                if (!deltas.empty()) {
                    const bool sent =
                        SendReliableBunches(viewerClientId, deltas);
                    if (sent && includeDead && binding) {
                        binding->priDeadWireValid = true;
                        binding->priDeadWireValue = combat.dead;
                    }
                }
                continue;
            }
        } else if (binding->pawnState ==
                   ParticipantActorOpenState::Unopened) {
            DeploymentRepl::RetailParticipantInitialState initial;
            if (BuildRemoteParticipantInitialState(
                    participant, &combat, initial)) {
                std::vector<PacketCodec::Bunch> open;
                if (QueueRemoteParticipantPawnOpen(
                        viewerClientId, initial, open) ==
                    RemotePriOpenResult::OpenQueued) {
                    SendReliableBunches(viewerClientId, open);
                }
            }
            binding = cs.remoteParticipants.Find(participant);
        }
        if (!binding) continue;
        if (!isDead) {
            (void)cs.remoteParticipants.SetDead(participant, false);
        }

        BitWriter pri;
        const bool includeDead =
            !binding->priDeadWireValid ||
            binding->priDeadWireValue != combat.dead;
        if (DeploymentRepl::WriteRemotePriCombat(
                pri, combat, includeDead)) {
            PacketCodec::Bunch priBunch;
            priBunch.chIndex = binding->priChannel;
            priBunch.chType = cs.actorChType;
            priBunch.payload = pri.GetBytes();
            priBunch.payloadBits = static_cast<uint32_t>(pri.NumBits());
            deltas.push_back(std::move(priBunch));
        }
        if (!isDead && sendHealth &&
            binding->pawnState == ParticipantActorOpenState::Open) {
            BitWriter pawn;
            if (DeploymentRepl::WriteRemotePawnHealth(pawn, combat)) {
                PacketCodec::Bunch pawnBunch;
                pawnBunch.chIndex = binding->pawnChannel;
                pawnBunch.chType = cs.actorChType;
                pawnBunch.payload = pawn.GetBytes();
                pawnBunch.payloadBits =
                    static_cast<uint32_t>(pawn.NumBits());
                deltas.push_back(std::move(pawnBunch));
            }
        }
        if (!deltas.empty()) {
            const bool sent = SendReliableBunches(viewerClientId, deltas);
            if (sent && includeDead) {
                binding->priDeadWireValid = true;
                binding->priDeadWireValue = combat.dead;
            }
        }
    }
}

// Build the (unreliable) PC.PlayerReplicationInfo link bunch on ch2: handle 23 +
// dynamic object-ref to ch26 (= "176a00"). Shared by SendLocalPriLink and the ordered
// role-advance packet so the bytes are identical.
static PacketCodec::Bunch BuildPriLinkBunch() {
    BitWriter lw;
    lw.SerializeInt(23, kRoPcMaxHandle);   // Controller.PlayerReplicationInfo
    lw.WriteBit(true);                      // dynamic objref selector
    lw.SerializeInt(26, ActorRepl::kDynamicChannelMax); // local PRI channel index
    PacketCodec::Bunch b;
    b.bReliable = false; b.chIndex = 2; b.chType = 2; b.chSequence = 0;  // chType ignored (unreliable)
    b.payload = lw.GetBytes(); b.payloadBits = static_cast<uint32_t>(lw.NumBits());
    return b;
}

// Build the (unreliable) PRI spectator-clear bunch on ch26: bWaitingPlayer(31) /
// bOnlySpectator(32) / bIsSpectator(33) = 0 (PRI maxHandle 98).
static PacketCodec::Bunch BuildClearSpectatorBunch() {
    constexpr uint32_t kPriMaxHandle = 98;
    BitWriter pw;
    pw.SerializeInt(31, kPriMaxHandle); pw.WriteBit(false);
    pw.SerializeInt(32, kPriMaxHandle); pw.WriteBit(false);
    pw.SerializeInt(33, kPriMaxHandle); pw.WriteBit(false);
    PacketCodec::Bunch b;
    b.bReliable = false; b.chIndex = 26; b.chType = 2; b.chSequence = 0;
    b.payload = pw.GetBytes(); b.payloadBits = static_cast<uint32_t>(pw.NumBits());
    return b;
}

void ConnectionManager::SendLocalPriLink(uint32_t clientId, int repeats) {
    // handle 23 (Controller.PlayerReplicationInfo) + dynamic object-ref to ch26 (local PRI):
    //   SerializeInt(23,531) = 9 bits, selector bit 1 (dynamic), SerializeInt(26,1024).
    // = the real server's exact 20-bit "176a00" bunch (decoded + confirmed by RE).
    // Sent UNRELIABLE (bReliable=0, no chSequence/chType written by the encoder) exactly
    // as the real server streams it (docs/re/pc_ch2_postjoin_timeline.md §C). Repeated for
    // delivery since it is unreliable; the client latches ROPC.PlayerReplicationInfo on
    // receipt, fixing the role-UI NULL deref (VNGame.exe+0xbbf712).
    // Build correct-by-construction: SerializeInt(23,531) [9b] + dynamic-ref selector bit [1b]
    // + SerializeInt(26, 1024) [10b] = the capture-exact 20-bit `17 6a 00`.
    // RS2's cooked channel bound is 1024 here (even though a generic UE3 source tree
    // may default MAX_NET_CHANNELS to 2048); emitting 21 bits leaves a stray bit that
    // the client reads as the start of another property handle.
    BitWriter lw;
    lw.SerializeInt(23, kRoPcMaxHandle);   // Controller.PlayerReplicationInfo handle
    lw.WriteBit(true);                      // object-ref selector = dynamic (1)
    lw.SerializeInt(26, ActorRepl::kDynamicChannelMax); // local PRI channel index
    const std::vector<uint8_t> linkPayload = lw.GetBytes();
    const uint32_t linkBits = static_cast<uint32_t>(lw.NumBits());
    ControlState& cs = GetControlState(clientId);
    for (int i = 0; i < repeats; ++i) {
        PacketCodec::Bunch b;
        b.bControl   = false;
        b.bOpen      = false;
        b.bClose     = false;
        b.bReliable  = false;            // UNRELIABLE delta (chSeq=0, not retransmitted)
        b.chIndex    = 2;
        b.chType     = cs.actorChType;   // ignored on the wire for unreliable bunches
        b.chSequence = 0;
        b.payload    = linkPayload;
        b.payloadBits = linkBits;
        SendReliableBunches(clientId, { b });   // sends; not recorded (bReliable=false)
    }
    Logger::Info("[ConnectionManager::SendLocalPriLink] client %u: sent PC.PlayerReplicationInfo"
                 "->ch26 link (handle 23) x%d (unreliable 176a00)", clientId, repeats);
}

void ConnectionManager::SendClearSpectator(uint32_t clientId, int repeats) {
    // Our ch26 PRI open carries bWaitingPlayer(31)=bOnlySpectator(32)=bIsSpectator(33)=1
    // (the captured local player was in the spectator/waiting state). After a team pick the
    // player is no longer a spectator; replicate those flags = 0 so ShowRoleSelectScene does
    // not early-return at ROPlayerController.uc:5932. Property bunch = ascending handle order,
    // each bool = SerializeInt(handle,98) + 1 value bit. UNRELIABLE ch26 delta, repeated.
    constexpr uint32_t kPriMaxHandle = 98;
    BitWriter pw;
    pw.SerializeInt(31, kPriMaxHandle); pw.WriteBit(false);  // bWaitingPlayer = 0
    pw.SerializeInt(32, kPriMaxHandle); pw.WriteBit(false);  // bOnlySpectator = 0
    pw.SerializeInt(33, kPriMaxHandle); pw.WriteBit(false);  // bIsSpectator   = 0
    const std::vector<uint8_t> payload = pw.GetBytes();
    const uint32_t bits = static_cast<uint32_t>(pw.NumBits());
    for (int i = 0; i < repeats; ++i) {
        PacketCodec::Bunch b;
        b.bControl   = false;
        b.bOpen      = false;
        b.bClose     = false;
        b.bReliable  = false;            // UNRELIABLE delta (no chSeq/chType on the wire)
        b.chIndex    = 26;               // local player's PRI channel
        b.chType     = 2;                // CHTYPE_Actor (ignored for unreliable)
        b.chSequence = 0;
        b.payload    = payload;
        b.payloadBits = bits;
        SendReliableBunches(clientId, { b });   // sends; not recorded (bReliable=false)
    }
    Logger::Info("[ConnectionManager::SendClearSpectator] client %u: cleared PRI spectator flags "
                 "on ch26 (h31/32/33=0, %u bits) x%d", clientId, bits, repeats);
}

static std::vector<PacketCodec::Bunch> BuildGivePawnBunches(
    uint32_t actorChType, uint16_t pawnChannel,
    std::span<const uint32_t> reliableSequences) {
    if (reliableSequences.size() != 3u) return {};

    BitWriter pawnProperty;
    ActorRepl::WritePropObject(
        pawnProperty, 24, kRoPcMaxHandle,
        ActorRepl::NetGUIDRef{
            /*isDynamic=*/true, pawnChannel});
    PacketCodec::Bunch prop;
    prop.bReliable = false;
    prop.chIndex = 2;
    prop.chType = actorChType;
    prop.chSequence = 0;
    prop.payload = pawnProperty.GetBytes();
    prop.payloadBits = static_cast<uint32_t>(pawnProperty.NumBits());

    BitWriter givePawn;
    givePawn.SerializeInt(43, kRoPcMaxHandle); // GivePawn(Pawn NewPawn)
    givePawn.WriteBit(true);                   // NewPawn presence bit
    ActorRepl::WriteNetGUID(
        givePawn,
        ActorRepl::NetGUIDRef{
            /*isDynamic=*/true, pawnChannel});
    PacketCodec::Bunch rpc;
    rpc.bReliable = true;
    rpc.chIndex = 2;
    rpc.chType = actorChType;
    rpc.chSequence = reliableSequences[0];
    rpc.payload = givePawn.GetBytes();
    rpc.payloadBits = static_cast<uint32_t>(givePawn.NumBits());

    BitWriter clientOnPossess;
    clientOnPossess.SerializeInt(150, kRoPcMaxHandle);
    clientOnPossess.WriteBit(true);
    ActorRepl::WriteNetGUID(
        clientOnPossess,
        ActorRepl::NetGUIDRef{
            /*isDynamic=*/true, pawnChannel});
    PacketCodec::Bunch onPossess;
    onPossess.bReliable = true;
    onPossess.chIndex = 2;
    onPossess.chType = actorChType;
    onPossess.chSequence = reliableSequences[1];
    onPossess.payload = clientOnPossess.GetBytes();
    onPossess.payloadBits =
        static_cast<uint32_t>(clientOnPossess.NumBits());

    BitWriter clientOnDead;
    clientOnDead.SerializeInt(151, kRoPcMaxHandle);
    clientOnDead.WriteBit(false);
    PacketCodec::Bunch onDead;
    onDead.bReliable = true;
    onDead.chIndex = 2;
    onDead.chType = actorChType;
    onDead.chSequence = reliableSequences[2];
    onDead.payload = clientOnDead.GetBytes();
    onDead.payloadBits = static_cast<uint32_t>(clientOnDead.NumBits());

    return {std::move(prop), std::move(rpc), std::move(onPossess),
            std::move(onDead)};
}

bool ConnectionManager::PreflightPawnSpawn(
    uint32_t clientId, uint64_t expectedPawnGeneration,
    const Vector3& spawnLocation) {
    return ProcessPawnSpawn(
        clientId, expectedPawnGeneration, spawnLocation,
        /*preflightOnly=*/true);
}

bool ConnectionManager::SendPawnSpawn(uint32_t clientId,
                                      uint64_t expectedPawnGeneration) {
    PlayerManager* players =
        m_server ? m_server->GetPlayerManager() : nullptr;
    const std::shared_ptr<Player> player =
        players ? players->GetPlayer(clientId) : nullptr;
    if (!player) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u has no "
            "authoritative Player position; captured coordinates are not a "
            "valid spawn fallback",
            clientId);
        return false;
    }
    return ProcessPawnSpawn(
        clientId, expectedPawnGeneration, player->GetPosition(),
        /*preflightOnly=*/false);
}

bool ConnectionManager::ProcessPawnSpawn(
    uint32_t clientId, uint64_t expectedPawnGeneration,
    const Vector3& spawnLocation, bool preflightOnly) {
    ControlState& cs = GetControlState(clientId);
    constexpr uint32_t kPawnMaxHandle = 168; // netfields_u_ROPawn handles 0..167
    constexpr uint32_t kPawnCh = kLocalPawnChannel; // fresh channel above bootstrap range

    const bool generationMatches = preflightOnly
        ? expectedPawnGeneration != 0u &&
            AnticipatedOwningPawnGeneration(cs) ==
                expectedPawnGeneration
        : expectedPawnGeneration != 0u && cs.owningPawnAlive &&
            cs.owningPawnGeneration == expectedPawnGeneration;
    if (!generationMatches) {
        Logger::Warn(
            "[ConnectionManager::SendPawnSpawn] client %u owning-pawn "
            "generation changed before graph %s (expected=%llu, "
            "current=%llu, anticipated=%llu, alive=%s)",
            clientId,
            preflightOnly ? "preflight" : "publication",
            static_cast<unsigned long long>(expectedPawnGeneration),
            static_cast<unsigned long long>(cs.owningPawnGeneration),
            static_cast<unsigned long long>(
                AnticipatedOwningPawnGeneration(cs)),
            cs.owningPawnAlive ? "true" : "false");
        return false;
    }

    uint32_t graphTeamId = 0;
    if (m_server) {
        if (const TeamManager* teams = m_server->GetTeamManager()) {
            graphTeamId = teams->GetPlayerTeam(clientId);
        }
    }
    if (graphTeamId != TeamMapping::kServerUs &&
        graphTeamId != TeamMapping::kServerNva) {
        Logger::Warn(
            "[ConnectionManager::SendPawnSpawn] client %u has no playable "
            "authoritative team; refusing to choose an owning pawn template",
            clientId);
        return false;
    }
    const bool northGraph = graphTeamId == TeamMapping::kServerNva;
    const OwnedWeaponChannelSet weaponChannels =
        OwnedWeaponChannels(northGraph);

    // The actor templates were captured against the canonical PackageMap, but
    // this client may have frozen the grounded installed artifact. Resolve every
    // known static reference before mutating or sending any part of the owning
    // graph. Combat authority deliberately continues to use canonical class
    // identities; only wire references are artifact-specific.
    const std::optional<RetailBootstrap::ArtifactSelection>& selectedArtifact =
        GetRetailArtifactSelection(clientId);
    if (!selectedArtifact) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u has no valid frozen "
            "replication artifact; refusing owning pawn graph",
            clientId);
        return false;
    }
    const RetailBootstrap::ArtifactSelection& artifact = *selectedArtifact;
    constexpr uint32_t kCanonicalInventoryManagerClassRef = 82735u;
    const uint32_t canonicalPawnClassRef = northGraph ? 286147u : 286151u;
    const std::optional<uint32_t> wirePawnClassRef =
        RetailBootstrap::ResolveRoGameContentWireRef(
            artifact, canonicalPawnClassRef);
    if (!wirePawnClassRef ||
        artifact.owningPawn.inventoryManagerArchetypeRef == 0u ||
        artifact.owningPawn.inventoryManagerArchetypeRef >=
            ActorRepl::kStaticObjectMax) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u artifact '%.*s' "
            "has an invalid owning-pawn PackageMap layout",
            clientId, static_cast<int>(artifact.variant.size()),
            artifact.variant.data());
        return false;
    }

    struct WireWeaponChannelRefs {
        uint16_t channel = 0;
        uint32_t classRef = 0;
        uint32_t attachmentClassRef = 0;
    };
    std::vector<WireWeaponChannelRefs> wireWeaponRefs;
    wireWeaponRefs.reserve(weaponChannels.count);
    for (const OwnedWeaponChannelMetadata& metadata : weaponChannels) {
        const std::optional<uint32_t> classRef =
            RetailBootstrap::ResolveRoGameContentWireRef(
                artifact, metadata.classRef);
        const std::optional<uint32_t> attachmentClassRef =
            RetailBootstrap::ResolveRoGameContentWireRef(
                artifact, metadata.attachmentClassRef);
        if (!classRef || !attachmentClassRef) {
            Logger::Error(
                "[ConnectionManager::SendPawnSpawn] client %u artifact '%.*s' "
                "cannot resolve canonical ROGameContent refs for actor ch%u",
                clientId, static_cast<int>(artifact.variant.size()),
                artifact.variant.data(),
                static_cast<unsigned>(metadata.channel));
            return false;
        }
        wireWeaponRefs.push_back(
            {metadata.channel, *classRef, *attachmentClassRef});
    }
    const auto findWireWeaponRefs =
        [&wireWeaponRefs](uint16_t channel) -> const WireWeaponChannelRefs* {
            const auto found = std::find_if(
                wireWeaponRefs.begin(), wireWeaponRefs.end(),
                [channel](const WireWeaponChannelRefs& refs) {
                    return refs.channel == channel;
                });
            return found == wireWeaponRefs.end() ? nullptr : &*found;
        };

    // ExecutePreparedDeployment must pass the explicit graph gate before world
    // mutation. Reaching this point with the opposite graph still open means a
    // caller bypassed that lifecycle; never reuse actor classes in place.
    if (cs.pawnGraphOpen && cs.pawnGraphTeamId != graphTeamId) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u changed from team %u "
            "to %u without draining the owning graph close generation",
            clientId, cs.pawnGraphTeamId, graphTeamId);
        return false;
    }
    if (cs.pawnGraphPhase == OwningPawnGraphPhase::Closing ||
        cs.pawnGraphPhase == OwningPawnGraphPhase::Broken) {
        return false;
    }

    // SpawnSystem is authoritative for the gameplay position. Relocate every
    // captured actor-open below to that same planned point so the retail pawn,
    // inventory graph, movement authority and objective-zone checks begin in
    // one place. Preflight receives the exact immutable SpawnSystem plan;
    // publication receives the committed Player position.
    const auto validSpawnComponent = [](float value) {
        if (!std::isfinite(value)) return false;
        const double rounded = std::floor(static_cast<double>(value) + 0.5);
        return rounded >= -1048576.0 && rounded <= 1048575.0;
    };
    if (!validSpawnComponent(spawnLocation.x) ||
        !validSpawnComponent(spawnLocation.y) ||
        !validSpawnComponent(spawnLocation.z)) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u has an invalid "
            "authoritative spawn location (%.1f, %.1f, %.1f)",
            clientId, spawnLocation.x, spawnLocation.y, spawnLocation.z);
        return false;
    }
    if (!preflightOnly) {
        // SpawnSystem has committed this authoritative discontinuity. The first
        // client ServerMove must be measured from the actual PlayerStart, not
        // from a prior life or a missing validator baseline.
        (void)ResetRetailMovementValidation(clientId, spawnLocation);

        // Every deployment starts on the faction primary at stable ch210. The
        // faction grenade is stable ch212 and remains inactive until selected.
        cs.activeWeaponChannel = 0;
        const OwnedWeaponChannelMetadata* primaryMetadata =
            FindOwnedWeaponChannel(210u, northGraph);
        if (m_server && primaryMetadata &&
            m_server->SelectCombatWeaponChannel(
                clientId, 210u, primaryMetadata->classRef)) {
            cs.activeWeaponChannel = 210u;
        } else {
            Logger::Warn(
                "[CombatAuthority] client %u could not activate faction "
                "primary for pawn spawn",
                clientId);
        }
    }

    // The first deployment opens the owning pawn/inventory graph. Later round
    // generations reuse those still-resolved channels: teleport the existing
    // pawn and run the stock GivePawn/ClientRestart recovery path instead of
    // illegally opening ch209..219 a second time.
    if (cs.pawnGraphOpen) {
        if (preflightOnly) {
            // The synchronous SpawnPlayer callback publishes one reliable
            // ClientOnDead(false) on a reused live graph before the five-bunch
            // redeploy cohort. Validate both without holding an unpublished
            // reservation across that callback.
            if (!cs.outboundActorChannels.test(2u)) return false;
            const auto capacity = cs.ch2Reliable.CanReserveBatch(6u);
            if (!capacity) {
                Logger::Warn(
                    "[OwningPawnGraph] client %u cannot preflight six ch2 "
                    "reliables for respawn callback + graph reuse (error=%u)",
                    clientId, static_cast<unsigned>(capacity.error()));
                return false;
            }
            return true;
        }
        const auto reservation = ReserveCh2Reliable(
            cs, clientId, 5u, "owning-pawn redeploy");
        if (!reservation) return false;

        std::vector<PacketCodec::Bunch> redeployBunches;
        redeployBunches.reserve(6u); // PC.Pawn is the one unreliable member.

        BitWriter relocate;
        relocate.SerializeInt(25, kRoPcMaxHandle); // ClientSetLocation
        relocate.WriteBit(true);                    // NewLocation present
        ActorRepl::WriteCompressedVector(
            relocate, spawnLocation.x, spawnLocation.y, spawnLocation.z);
        relocate.WriteBit(true);                    // NewRotation present
        ActorRepl::WriteCompressedRotator(relocate, 0, 0, 0);
        PacketCodec::Bunch relocateBunch;
        relocateBunch.bReliable = true;
        relocateBunch.chIndex = 2;
        relocateBunch.chType = cs.actorChType;
        relocateBunch.chSequence = (*reservation)[0];
        relocateBunch.payload = relocate.GetBytes();
        relocateBunch.payloadBits =
            static_cast<uint32_t>(relocate.NumBits());
        redeployBunches.push_back(std::move(relocateBunch));

        std::vector<PacketCodec::Bunch> givePawnBunches =
            BuildGivePawnBunches(
                cs.actorChType, kLocalPawnChannel,
                std::span<const uint32_t>{
                    reservation->SequenceValues().data() + 1u, 3u});
        redeployBunches.insert(
            redeployBunches.end(),
            std::make_move_iterator(givePawnBunches.begin()),
            std::make_move_iterator(givePawnBunches.end()));

        BitWriter switchBestWeapon;
        switchBestWeapon.SerializeInt(28, kRoPcMaxHandle);
        switchBestWeapon.WriteBit(true); // bForceNewWeapon
        PacketCodec::Bunch switchBunch;
        switchBunch.bReliable = true;
        switchBunch.chIndex = 2;
        switchBunch.chType = cs.actorChType;
        switchBunch.chSequence = (*reservation)[4];
        switchBunch.payload = switchBestWeapon.GetBytes();
        switchBunch.payloadBits =
            static_cast<uint32_t>(switchBestWeapon.NumBits());
        redeployBunches.push_back(std::move(switchBunch));

        if (!SendReservedCh2Bunches(
                clientId, redeployBunches, *reservation,
                "owning-pawn redeploy")) {
            return false;
        }
        Logger::Info("[ConnectionManager::SendPawnSpawn] client %u: reused owning pawn graph "
                     "and redeployed at (%.1f, %.1f, %.1f)", clientId,
                     spawnLocation.x, spawnLocation.y, spawnLocation.z);
        return true;
    }

    // Verbatim LOCAL-OWNER South pawn open from official f27394. It already
    // uses the emulator's stable ch2/ch26 references.
    static const char* kSouthPawnOpenHex =
        "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
        "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
        "19648419158a190684196784176189003";
    constexpr uint32_t kSouthPawnOpenBits = 763;

    // Verbatim local North pawn open from official f63525. Capture ch94 carried
    // owner/controller->ch2 and PRI->ch4. Only the typed h32 PRI reference is
    // retargeted to stable local PRI ch26 before the opaque tail is relocated.
    static const char* kNorthPawnOpenHex =
        "86bb08008d4b1d5d27011904655000041d8224805600984400b2c400b24401b2c401b24402b2c402"
        "b244038fc403b244048ac4048544058fc405b244068ac40685448791c407b244088ac40885448991"
        "c409b2440a8ac40a85440bb2c40bb2c3801c";
    constexpr uint32_t kNorthPawnOpenBits = 782;
    auto decodeHex = [](const char* hex, std::vector<uint8_t>& bytes) {
        bytes.clear();
        auto nib = [](char c)->int {
            if (c>='0'&&c<='9') return c-'0';
            if (c>='a'&&c<='f') return c-'a'+10;
            if (c>='A'&&c<='F') return c-'A'+10;
            return -1;
        };
        if (!hex || !hex[0]) return false;
        const char* end = hex;
        while (*end) ++end;
        if (((end - hex) & 1) != 0) return false;
        bytes.reserve(static_cast<size_t>(end - hex) / 2u);
        for (const char* h = hex; h != end; h += 2) {
            const int high = nib(h[0]);
            const int low = nib(h[1]);
            if (high < 0 || low < 0) {
                bytes.clear();
                return false;
            }
            bytes.push_back(
                static_cast<uint8_t>((high << 4) | low));
        }
        return true;
    };
    const char* pawnOpenHex = northGraph
        ? kNorthPawnOpenHex : kSouthPawnOpenHex;
    const uint32_t capturedPawnOpenBits = northGraph
        ? kNorthPawnOpenBits : kSouthPawnOpenBits;
    std::vector<uint8_t> capturedPawnOpen;
    if (!decodeHex(pawnOpenHex, capturedPawnOpen)) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u: invalid %s pawn "
            "capture template; refusing spawn",
            clientId, northGraph ? "North" : "South");
        return false;
    }
    std::vector<uint8_t> normalizedPawnOpen = capturedPawnOpen;
    if (northGraph && !ActorRepl::RewriteCapturedDynamicChannelRefs(
            capturedPawnOpen.data(), capturedPawnOpen.size(),
            capturedPawnOpenBits,
            {{146u, 4u, 26u}}, normalizedPawnOpen)) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u: North pawn "
            "capture template drifted at h32 PRI reference; refusing spawn",
            clientId);
        return false;
    }
    std::vector<uint8_t> pawnOpen;
    uint32_t pawnOpenBits = 0;
    if (!ActorRepl::RewriteCapturedActorOpenClassAndLocation(
            normalizedPawnOpen.data(), normalizedPawnOpen.size(),
            capturedPawnOpenBits,
            canonicalPawnClassRef, *wirePawnClassRef,
            spawnLocation.x, spawnLocation.y, spawnLocation.z,
            pawnOpen, pawnOpenBits)) {
        Logger::Error(
            "[ConnectionManager::SendPawnSpawn] client %u: could not rebase "
            "and relocate pawn actor-open class %u->%u to (%.1f, %.1f, %.1f); "
            "refusing spawn",
            clientId, canonicalPawnClassRef, *wirePawnClassRef,
            spawnLocation.x, spawnLocation.y, spawnLocation.z);
        return false;
    }

    // Matching capture-pinned inventory graphs. South already used our stable
    // channels. North is retargeted field-by-field from f63525's ch94/95/96/97/104
    // to stable pawn209/weapons210,212,214/manager219. Every offset below was
    // decoded as h6 Owner, h4 Instigator, h25 NewOwner, or manager h23
    // InventoryChain; a drifted old value aborts the whole unsent packet.
    struct SpawnOpenTemplate {
        uint16_t channel;
        uint32_t bits;
        const char* hex;
        const ActorRepl::CapturedDynamicChannelRewrite* rewrites;
        size_t rewriteCount;
    };
    static constexpr SpawnOpenTemplate kSouthInventoryOpens[] = {
        {210, 301, "4cbd0800bdce01f8eef70ca3218cc6912b010000b03d000000c8fa010000648ec65c02000000", nullptr, 0},
        {211, 301, "6ebd0800bdce01f8eef70ca3218cc6917b000000b015000000c83a000000648ec6fc00000000", nullptr, 0},
        {212, 301, "00be0800bdce01f8eef70ca3218cc6911b000000b015000000c806000000648ec63c00000000", nullptr, 0},
        {213, 152, "3abb0800bdce01f8eef70ca3218cc631733466", nullptr, 0},
        {214, 262, "6abd0800bdce01f8eef70ca3218cc6911b000000b00d000000c81c8d7900000000", nullptr, 0},
        {219, 405, "5e860200bdce01f8eef7cc68c868dc5b9a0e000000f8d3030000007fba00000000501f000000fce9050000803ffda0aaaa1008", nullptr, 0},
    };
    static constexpr ActorRepl::CapturedDynamicChannelRewrite kNorthAkRefs[] = {
        {88, 94, 209}, {106, 94, 209}, {250, 94, 209},
    };
    static constexpr ActorRepl::CapturedDynamicChannelRewrite kNorthType67Refs[] = {
        {88, 94, 209}, {106, 94, 209}, {250, 94, 209},
    };
    static constexpr ActorRepl::CapturedDynamicChannelRewrite kNorthPunjiRefs[] = {
        {88, 94, 209}, {106, 94, 209}, {211, 94, 209},
    };
    static constexpr ActorRepl::CapturedDynamicChannelRewrite kNorthManagerRefs[] = {
        {86, 94, 209}, {102, 94, 209}, {124, 95, 210},
    };
    static constexpr SpawnOpenTemplate kNorthInventoryOpens[] = {
        {210, 301, "7ebc08008d4b1d5d27010dbd20f4c291eb010000b01d000000c86a01000064f6c2dc03000000", kNorthAkRefs, 3},
        {212, 301, "a8c008008d4b1d5d27010dbd20f4c2911b000000b015000000c80600000064f6c23c00000000", kNorthType67Refs, 3},
        {214, 262, "4cc008008d4b1d5d27010dbd20f4c2911b000000b00d000000c8ec857900000000", kNorthPunjiRefs, 3},
        {219, 315, "5e8602008d4b1d5d27014d2f482fdcfb8b0e000000f8d305000000803a01000000503f000000fe01", kNorthManagerRefs, 3},
    };
    const SpawnOpenTemplate* inventoryOpens = northGraph
        ? kNorthInventoryOpens : kSouthInventoryOpens;
    const size_t inventoryOpenCount = northGraph
        ? (sizeof(kNorthInventoryOpens) / sizeof(kNorthInventoryOpens[0]))
        : (sizeof(kSouthInventoryOpens) / sizeof(kSouthInventoryOpens[0]));

    struct PreparedSpawnOpen {
        uint16_t channel = 0;
        std::vector<uint8_t> payload;
        uint32_t payloadBits = 0;
    };
    std::vector<PreparedSpawnOpen> preparedInventoryOpens;
    preparedInventoryOpens.reserve(inventoryOpenCount);
    for (size_t itemIndex = 0; itemIndex < inventoryOpenCount; ++itemIndex) {
        const SpawnOpenTemplate& item = inventoryOpens[itemIndex];
        std::vector<uint8_t> capturedOpen;
        if (!decodeHex(item.hex, capturedOpen)) {
            Logger::Error(
                "[ConnectionManager::SendPawnSpawn] client %u: invalid %s "
                "capture template on actor ch%u; refusing spawn",
                clientId, northGraph ? "North" : "South",
                static_cast<unsigned>(item.channel));
            return false;
        }

        std::vector<uint8_t> normalizedOpen = capturedOpen;
        if (item.rewriteCount != 0) {
            const std::vector<ActorRepl::CapturedDynamicChannelRewrite> rewrites(
                item.rewrites, item.rewrites + item.rewriteCount);
            if (!ActorRepl::RewriteCapturedDynamicChannelRefs(
                    capturedOpen.data(), capturedOpen.size(), item.bits,
                    rewrites, normalizedOpen)) {
                Logger::Error(
                    "[ConnectionManager::SendPawnSpawn] client %u: North "
                    "capture template drifted on actor ch%u; refusing spawn",
                    clientId, static_cast<unsigned>(item.channel));
                return false;
            }
        }

        PreparedSpawnOpen prepared;
        prepared.channel = item.channel;
        const bool isInventoryManager = item.channel == 219u;
        const OwnedWeaponChannelMetadata* canonicalMetadata =
            isInventoryManager
                ? nullptr
                : FindOwnedWeaponChannel(item.channel, northGraph);
        const WireWeaponChannelRefs* wireMetadata =
            isInventoryManager ? nullptr : findWireWeaponRefs(item.channel);
        const uint32_t expectedClassRef = isInventoryManager
            ? kCanonicalInventoryManagerClassRef
            : (canonicalMetadata ? canonicalMetadata->classRef : 0u);
        const uint32_t replacementClassRef = isInventoryManager
            ? artifact.owningPawn.inventoryManagerArchetypeRef
            : (wireMetadata ? wireMetadata->classRef : 0u);
        if (expectedClassRef == 0u || replacementClassRef == 0u ||
            !ActorRepl::RewriteCapturedActorOpenClassAndLocation(
                normalizedOpen.data(), normalizedOpen.size(), item.bits,
                expectedClassRef, replacementClassRef,
                spawnLocation.x, spawnLocation.y, spawnLocation.z,
                prepared.payload, prepared.payloadBits)) {
            Logger::Error(
                "[ConnectionManager::SendPawnSpawn] client %u: could not "
                "rebase and relocate %s inventory actor ch%u class %u->%u "
                "to (%.1f, %.1f, %.1f); refusing spawn",
                clientId, northGraph ? "North" : "South",
                static_cast<unsigned>(item.channel), expectedClassRef,
                replacementClassRef, spawnLocation.x, spawnLocation.y,
                spawnLocation.z);
            return false;
        }
        preparedInventoryOpens.push_back(std::move(prepared));
    }

    if (preflightOnly) {
        if (!cs.outboundActorChannels.test(2u)) return false;
        const auto ch2Capacity = cs.ch2Reliable.CanReserveBatch(
            1u + weaponChannels.count);
        if (!ch2Capacity) {
            Logger::Warn(
                "[OwningPawnGraph] client %u cannot preflight %zu ch2 "
                "reliables for initial/reopened graph (error=%u)",
                clientId, 1u + weaponChannels.count,
                static_cast<unsigned>(ch2Capacity.error()));
            return false;
        }
    }

    if ((cs.pawnGraphPhase != OwningPawnGraphPhase::Unopened &&
         cs.pawnGraphPhase != OwningPawnGraphPhase::Closed) ||
        !EnsureOwningPawnGraphSequencers(clientId, cs)) {
        Logger::Error(
            "[OwningPawnGraph] client %u cannot open a graph from phase %u",
            clientId, static_cast<unsigned>(cs.pawnGraphPhase));
        return false;
    }

    if (preflightOnly) {
        const auto canReserveGraph =
            [&](uint32_t channel, size_t count) {
                const auto* sequencer =
                    OwningPawnGraphSequencer(cs, channel);
                const auto capacity = sequencer
                    ? sequencer->CanReserveBatch(count)
                    : PacketCodec::OutboundReliableSequencer::MutationResult(
                          std::unexpected(
                              PacketCodec::OutboundReliableSequenceError::
                                  Uninitialized));
                if (capacity) return true;
                Logger::Warn(
                    "[OwningPawnGraph] client %u cannot preflight %zu "
                    "sequence(s) on ch%u (error=%u)",
                    clientId, count, channel,
                    static_cast<unsigned>(capacity.error()));
                return false;
            };
        if (!canReserveGraph(kPawnCh, 5u)) return false;
        for (const OwnedWeaponChannelMetadata& metadata : weaponChannels) {
            if (!canReserveGraph(metadata.channel, 2u)) return false;
        }
        if (!canReserveGraph(219u, 1u)) return false;
        return true;
    }

    using GraphReservation =
        PacketCodec::OutboundReliableSequencer::Reservation;
    struct ReservedGraphChannel {
        uint32_t channel = 0;
        GraphReservation reservation;
    };
    std::vector<ReservedGraphChannel> graphReservations;
    graphReservations.reserve(2u + weaponChannels.count);
    auto cancelGraphReservations = [&]() {
        for (auto item = graphReservations.rbegin();
             item != graphReservations.rend(); ++item) {
            if (auto* sequencer =
                    OwningPawnGraphSequencer(cs, item->channel)) {
                (void)sequencer->CancelBatch(item->reservation);
            }
        }
    };
    auto reserveGraphChannel = [&](uint32_t channel, size_t count) {
        auto* sequencer = OwningPawnGraphSequencer(cs, channel);
        if (!sequencer) return false;
        auto reservation = sequencer->ReserveBatch(count);
        if (!reservation) {
            Logger::Warn(
                "[OwningPawnGraph] client %u could not reserve %zu "
                "sequence(s) on ch%u (error=%u)",
                clientId, count, channel,
                static_cast<unsigned>(reservation.error()));
            return false;
        }
        graphReservations.push_back(
            {channel, std::move(*reservation)});
        return true;
    };
    if (!reserveGraphChannel(kPawnCh, 5u)) {
        cancelGraphReservations();
        return false;
    }
    for (const OwnedWeaponChannelMetadata& metadata : weaponChannels) {
        if (!reserveGraphChannel(metadata.channel, 2u)) {
            cancelGraphReservations();
            return false;
        }
    }
    if (!reserveGraphChannel(219u, 1u)) {
        cancelGraphReservations();
        return false;
    }
    const auto graphReservation =
        [&graphReservations](uint32_t channel)
            -> const GraphReservation* {
            const auto found = std::find_if(
                graphReservations.begin(), graphReservations.end(),
                [channel](const ReservedGraphChannel& item) {
                    return item.channel == channel;
                });
            return found == graphReservations.end()
                ? nullptr : &found->reservation;
        };
    const GraphReservation* pawnReservation = graphReservation(kPawnCh);
    if (!pawnReservation) {
        cancelGraphReservations();
        return false;
    }

    // Reserve every reliable PlayerController bunch before publishing any
    // actor open. The graph contains one possession burst plus one weapon-
    // selection/tail bunch per owned weapon.
    const auto graphCh2Reservation = ReserveCh2Reliable(
        cs, clientId, 1u + weaponChannels.count,
        "initial owning-pawn graph");
    if (!graphCh2Reservation) {
        cancelGraphReservations();
        return false;
    }
    size_t graphCh2ReservationIndex = 0u;

    auto pawnDelta = [&](std::vector<uint8_t> bytes, uint32_t bits, uint32_t sequence) {
        PacketCodec::Bunch b;
        b.bReliable = true; b.chIndex = kPawnCh; b.chType = 2; b.chSequence = sequence;
        b.payload = std::move(bytes); b.payloadBits = bits;
        return b;
    };
    auto ch2Bunch = [&](std::vector<uint8_t> bytes, uint32_t bits, bool reliable) {
        PacketCodec::Bunch b;
        b.bReliable = reliable; b.chIndex = 2; b.chType = cs.actorChType;
        b.chSequence = reliable
            ? (*graphCh2Reservation)[graphCh2ReservationIndex++]
            : 0u;
        b.payload = std::move(bytes); b.payloadBits = bits;
        return b;
    };

    // PACKET 1 - capture ordering: pawn OPEN first, corrected pawn back-refs next,
    // ClientRestart(NewPawn) last, all in the SAME datagram. A dynamic object RPC cannot
    // safely race its actor open in another UDP datagram: if ClientRestart arrives first,
    // SerializeObject resolves NewPawn=None permanently. Make every load-bearing bunch
    // reliable so retransmission preserves the full mapping sequence. The open MUST set
    // bControl=true because PacketCodec writes bOpen/bClose only for control bunches.
    {
        std::vector<PacketCodec::Bunch> openPkt;

        PacketCodec::Bunch open;
        open.bControl = true; open.bOpen = true; open.bReliable = true;
        open.chIndex = kPawnCh; open.chType = 2;
        open.chSequence = (*pawnReservation)[0];
        open.payload = pawnOpen; open.payloadBits = pawnOpenBits;
        openPkt.push_back(std::move(open));
        // Never hand-pack these fields. SerializeInt is value-dependent: h52 consumes
        // seven bits at maxHandle 168, while h32 consumes eight. The old fixed-width
        // literals shifted the object selector and decoded h52 as a static object.
        BitWriter controllerRef;
        ActorRepl::WritePropObject(controllerRef, 52, kPawnMaxHandle,
                                   ActorRepl::NetGUIDRef{/*isDynamic=*/true, 2u});
        openPkt.push_back(pawnDelta(controllerRef.GetBytes(),
                                    static_cast<uint32_t>(controllerRef.NumBits()),
                                    (*pawnReservation)[1]));

        BitWriter priRef;
        ActorRepl::WritePropObject(priRef, 32, kPawnMaxHandle,
                                   ActorRepl::NetGUIDRef{/*isDynamic=*/true, 26u});
        openPkt.push_back(pawnDelta(priRef.GetBytes(),
                                    static_cast<uint32_t>(priRef.NumBits()),
                                    (*pawnReservation)[2]));

        // ROPawn.PossessedBy sends this no-parameter client RPC for pawn-specific
        // local setup (trap arrays, mesh/role state). It belongs on the pawn channel.
        BitWriter clientPossessed;
        clientPossessed.SerializeInt(57, kPawnMaxHandle);
        openPkt.push_back(pawnDelta(clientPossessed.GetBytes(),
                                    static_cast<uint32_t>(clientPossessed.NumBits()),
                                    (*pawnReservation)[3]));

        // Capture-exact retail possession sequence (official f27394/f33544), authored
        // structurally so channel references remain correct. The six official RPCs
        // share one reliable 323-bit ch2 bunch in retail order:
        //   ClientRestart -> ClientSetViewTarget -> ClientSetCameraMode ->
        //   SetHUDSpawnPenalty -> ClientOnPossess -> ClientOnDead.
        BitWriter possessBurst;
        possessBurst.SerializeInt(85, kRoPcMaxHandle);
        possessBurst.WriteBit(true); // NewPawn present
        ActorRepl::WriteNetGUID(possessBurst,
                                ActorRepl::NetGUIDRef{/*isDynamic=*/true, kPawnCh});

        possessBurst.SerializeInt(87, kRoPcMaxHandle);
        possessBurst.WriteBit(true); // view-target Actor present
        ActorRepl::WriteNetGUID(possessBurst,
                                ActorRepl::NetGUIDRef{/*isDynamic=*/true, kPawnCh});
        possessBurst.WriteBit(true); // ViewTargetTransitionParams present
        possessBurst.WriteFloat(0.0f);       // BlendTime
        possessBurst.SerializeInt(1, 6);     // VTBlend_Cubic (enum includes VTBlend_MAX)
        possessBurst.WriteFloat(2.0f);       // BlendExp
        possessBurst.WriteBit(false);        // bLockOutgoing

        possessBurst.SerializeInt(61, kRoPcMaxHandle);
        possessBurst.WriteBit(true);          // FName parameter present
        possessBurst.WriteBit(false);         // not a hardcoded name: serialize FString
        possessBurst.WriteString("FirstPerson");
        possessBurst.WriteInt32(0);           // FName Number

        possessBurst.SerializeInt(265, kRoPcMaxHandle);
        possessBurst.WriteBit(false);         // SpawnPenalty=0 omitted (default)

        possessBurst.SerializeInt(150, kRoPcMaxHandle);
        possessBurst.WriteBit(true);          // Pawn parameter present
        ActorRepl::WriteNetGUID(possessBurst,
                                ActorRepl::NetGUIDRef{/*isDynamic=*/true, kPawnCh});

        possessBurst.SerializeInt(151, kRoPcMaxHandle);
        possessBurst.WriteBit(false);         // freshly spawned pawn is alive

        if (northGraph) {
            // Exact f63525 continuation after ClientOnDead: h26
            // ClientSetRotation((0,40960,0), true). The pawn open carries the
            // same yaw=160 high byte, so controller and pawn begin aligned.
            possessBurst.SerializeInt(26, kRoPcMaxHandle);
            possessBurst.WriteBit(true); // NewRotation present
            ActorRepl::WriteCompressedRotator(
                possessBurst, 0u, static_cast<uint16_t>(160u << 8u), 0u);
            possessBurst.WriteBit(true); // bResetCamera
        }

        openPkt.push_back(ch2Bunch(possessBurst.GetBytes(),
                                   static_cast<uint32_t>(possessBurst.NumBits()),
                                   /*reliable=*/true));

        // Open the pawn's capture-matched inventory graph. The raw capture starts
        // ClientGivenTo inside each weapon open before its ROInventoryManager actor
        // (ch219) opens. A normal server already has Pawn.InvManager and the native
        // inventory list backing those RPCs; this emulator must recreate those
        // authority-side references explicitly after all channel mappings exist.
        for (const PreparedSpawnOpen& prepared : preparedInventoryOpens) {
            PacketCodec::Bunch inventoryOpen;
            inventoryOpen.bControl = true;
            inventoryOpen.bOpen = true;
            inventoryOpen.bReliable = true;
            inventoryOpen.chIndex = prepared.channel;
            inventoryOpen.chType = 2;
            inventoryOpen.chSequence =
                graphReservation(prepared.channel)->front();
            inventoryOpen.payload = prepared.payload;
            inventoryOpen.payloadBits = prepared.payloadBits;
            openPkt.push_back(std::move(inventoryOpen));
        }

        // Pawn.InvManager -> ch219. This is the missing load-bearing edge from
        // the previous implementation: without it Weapon.ClientGivenTo assigns
        // InvManager=None and remains in PendingClientWeaponSet forever.
        BitWriter pawnInvManager;
        ActorRepl::WritePropObject(pawnInvManager, 27, kPawnMaxHandle,
                                   ActorRepl::NetGUIDRef{/*isDynamic=*/true, 219u});
        openPkt.push_back(pawnDelta(pawnInvManager.GetBytes(),
                                    static_cast<uint32_t>(pawnInvManager.NumBits()),
                                    (*pawnReservation)[4]));

        for (size_t i = 0; i < weaponChannels.count; ++i) {
            const OwnedWeaponChannelMetadata& metadata =
                weaponChannels.entries[i];
            const uint16_t weaponCh = metadata.channel;
            const uint16_t nextWeaponCh =
                (i + 1 < weaponChannels.count)
                    ? weaponChannels.entries[i + 1].channel
                    : 0;

            // Complete InventoryManager.InventoryChain:
            // manager h23 -> ch210, then ch210..213 h24 -> their successor.
            // Retail f27395 omits final ch214 h24 and relies on the actor default.
            // Also set each weapon's h23 InvManager directly before replaying
            // ClientGivenTo(h25), which calls ClientWeaponSet on the client.
            BitWriter weaponGraph;
            ActorRepl::WritePropObject(weaponGraph, 23, metadata.maxHandle,
                                       ActorRepl::NetGUIDRef{/*isDynamic=*/true, 219u});
            if (nextWeaponCh != 0) {
                ActorRepl::WritePropObject(
                    weaponGraph, 24, metadata.maxHandle,
                    ActorRepl::NetGUIDRef{/*isDynamic=*/true, nextWeaponCh});
            }
            weaponGraph.SerializeInt(25, metadata.maxHandle); // ClientGivenTo
            weaponGraph.WriteBit(true);                       // NewOwner present
            ActorRepl::WriteNetGUID(weaponGraph,
                                    ActorRepl::NetGUIDRef{/*isDynamic=*/true, kPawnCh});
            weaponGraph.WriteBit(false);                      // bDoNotActivate

            PacketCodec::Bunch graphBunch;
            graphBunch.bReliable = true;
            graphBunch.chIndex = weaponCh;
            graphBunch.chType = 2;
            graphBunch.chSequence =
                (*graphReservation(weaponCh))[1];
            graphBunch.payload = weaponGraph.GetBytes();
            graphBunch.payloadBits = static_cast<uint32_t>(weaponGraph.NumBits());
            openPkt.push_back(std::move(graphBunch));

            // Official f27394 repeats this after each weapon open. Keep that
            // ordering after our completed graph so at least the first armed
            // weapon can be selected immediately and later items refresh HUD.
            BitWriter switchBestWeapon;
            switchBestWeapon.SerializeInt(28, kRoPcMaxHandle);
            switchBestWeapon.WriteBit(false); // optional bForceNewWeapon
            if (i + 1 == weaponChannels.count) {
                // Both captures repeat h28 after the final actor. Their
                // post-loadout tails then diverge by faction.
                switchBestWeapon.SerializeInt(28, kRoPcMaxHandle);
                switchBestWeapon.WriteBit(false);
                if (northGraph) {
                    // Exact f63525 seq70 tail (51 bits total): h28(false),
                    // h28(false), ClientSpawned, ClientHideRoundStartScreen,
                    // then ClientSetCinematicMode(false,true,false,false).
                    switchBestWeapon.SerializeInt(390, kRoPcMaxHandle);
                    switchBestWeapon.SerializeInt(226, kRoPcMaxHandle);
                    switchBestWeapon.SerializeInt(101, kRoPcMaxHandle);
                    switchBestWeapon.WriteBit(false); // bInCinematicMode
                    switchBestWeapon.WriteBit(true);  // bAffectsMovement
                    switchBestWeapon.WriteBit(false); // bAffectsTurning
                    switchBestWeapon.WriteBit(false); // bAffectsHUD
                } else {
                    // Exact f27394 ch2 seq681 tail (119 bits total).
                    switchBestWeapon.SerializeInt(168, kRoPcMaxHandle); // ClientCameraReset
                    switchBestWeapon.SerializeInt(87, kRoPcMaxHandle);  // ClientSetViewTarget
                    switchBestWeapon.WriteBit(true);                    // Actor present
                    ActorRepl::WriteNetGUID(
                        switchBestWeapon,
                        ActorRepl::NetGUIDRef{/*isDynamic=*/true, 2u});
                    switchBestWeapon.WriteBit(true);                    // transition present
                    switchBestWeapon.WriteFloat(0.0f);                  // BlendTime
                    switchBestWeapon.SerializeInt(1, 6);                // VTBlend_Cubic
                    switchBestWeapon.WriteFloat(2.0f);                  // BlendExp
                    switchBestWeapon.WriteBit(false);                   // bLockOutgoing
                }
            }
            openPkt.push_back(ch2Bunch(switchBestWeapon.GetBytes(),
                                       static_cast<uint32_t>(switchBestWeapon.NumBits()),
                                       /*reliable=*/true));
        }
        const size_t pendingBefore = cs.pendingReliable.size();
        const bool queuedCh2 = SendReservedCh2Bunches(
            clientId, openPkt, *graphCh2Reservation,
            "initial owning-pawn graph");
        const bool graphPublished =
            cs.pendingReliable.size() > pendingBefore;
        if (!graphPublished) {
            cancelGraphReservations();
            return false;
        }

        for (const ReservedGraphChannel& item : graphReservations) {
            auto* sequencer =
                OwningPawnGraphSequencer(cs, item.channel);
            const auto committed = sequencer
                ? sequencer->CommitBatch(item.reservation)
                : PacketCodec::OutboundReliableSequencer::MutationResult(
                      std::unexpected(
                          PacketCodec::OutboundReliableSequenceError::
                              Uninitialized));
            if (!committed) {
                Logger::Error(
                    "[OwningPawnGraph] client %u queued open ch%u but could "
                    "not commit its reliable reservation (error=%u)",
                    clientId, item.channel,
                    static_cast<unsigned>(committed.error()));
                FailOwningPawnGraph(
                    clientId, "published open reservation commit");
                return false;
            }
        }
        if (!queuedCh2) return false;

        cs.owningPawnGraphActiveChannels.reset();
        for (const ReservedGraphChannel& item : graphReservations) {
            cs.owningPawnGraphActiveChannels.set(item.channel);
        }
        cs.owningPawnGraphClosingChannels.reset();
        cs.owningPawnGraphCloseAcknowledged.reset();
        cs.pawnGraphPhase = OwningPawnGraphPhase::Open;
        cs.pawnGraphOpen = true;
        cs.pawnGraphTeamId = graphTeamId;

        // Retail f27395 publishes the loadout paperdoll as five unreliable
        // ROPawn h167 static-array records after all actor mappings exist. Each
        // record is exactly 49 bits: handle, raw array index, alt flag, class.
        BitWriter attachmentList;
        for (size_t i = 0; i < weaponChannels.count; ++i) {
            const OwnedWeaponChannelMetadata& metadata =
                weaponChannels.entries[i];
            // wireWeaponRefs is prepared transactionally from this exact ordered
            // channel set before any graph bunch is emitted.
            const WireWeaponChannelRefs& wireMetadata = wireWeaponRefs[i];
            attachmentList.SerializeInt(167, kPawnMaxHandle);
            attachmentList.WriteByte(metadata.attachmentSlot);
            attachmentList.WriteBit(false); // bAltState
            ActorRepl::WriteNetGUID(
                attachmentList,
                ActorRepl::NetGUIDRef{/*isDynamic=*/false,
                                      wireMetadata.attachmentClassRef});
        }
        if (northGraph) {
            // The exact f63525 pawn delta ends with h148 Encumbrance=9.92
            // after the three attachment records (206 bits including h27,
            // which was sent reliably above).
            ActorRepl::WritePropFloat(
                attachmentList, 148, kPawnMaxHandle, 9.92f);
        }
        PacketCodec::Bunch attachmentBunch;
        attachmentBunch.bReliable = false;
        attachmentBunch.chIndex = kPawnCh;
        attachmentBunch.chType = 2;
        attachmentBunch.chSequence = 0;
        attachmentBunch.payload = attachmentList.GetBytes();
        attachmentBunch.payloadBits =
            static_cast<uint32_t>(attachmentList.NumBits());
        SendReliableBunches(clientId, {attachmentBunch});
    }

    // PACKET 2 - re-assert the replicated PC.Pawn property after the mapping packet.
    // The official capture also sends this property on the following tick; ClientRestart
    // is the causal possession trigger and already ran after the open in packet 1.
    {
        BitWriter pawnProperty;
        if (northGraph) {
            // Exact f63525 post-open PC delta normalized from pawn94->209:
            // Actor.bCollideWorld=false, Controller.Pawn, and the retail
            // NextRespawnTime sentinel. It consumes all 72 capture bits.
            ActorRepl::WritePropBool(
                pawnProperty, 17, kRoPcMaxHandle, false);
            ActorRepl::WritePropObject(
                pawnProperty, 24, kRoPcMaxHandle,
                ActorRepl::NetGUIDRef{/*isDynamic=*/true, kPawnCh});
            ActorRepl::WritePropInt(
                pawnProperty, 316, kRoPcMaxHandle, 9999999);
        } else {
            ActorRepl::WritePropObject(
                pawnProperty, 24, kRoPcMaxHandle,
                ActorRepl::NetGUIDRef{/*isDynamic=*/true, kPawnCh});
        }
        SendReliableBunches(clientId, {
            ch2Bunch(pawnProperty.GetBytes(),
                     static_cast<uint32_t>(pawnProperty.NumBits()),
                     /*reliable=*/false)
        });
    }
    Logger::Info("[ConnectionManager::SendPawnSpawn] client %u: owning %s ROPawn ch%u "
                 "(class %u) + %zu weapons on stable ch210..214 + ROInventoryManager ch219 + pawn/inventory-chain back-refs + "
                 "ClientGivenTo(h25) + ClientPossessed(h57) + retail possession burst "
                 "(h85/h87/h61/h265/h150/h151) + h167 attachment list + repeated ClientSwitchToBestWeapon(h28) "
                 "+ faction capture tail [pkt1], then "
                 "PC.Pawn(h24) [pkt2] at (%.1f, %.1f, %.1f) - expecting ServerMove(h65)",
                 clientId, northGraph ? "North" : "South", kPawnCh,
                  *wirePawnClassRef, weaponChannels.count,
                  spawnLocation.x, spawnLocation.y, spawnLocation.z);
    return true;
}

void ConnectionManager::SendOwningPawnCurrentAttachment(
    uint32_t clientId, uint32_t weaponChannel) {
    ControlState& cs = GetControlState(clientId);
    if (!cs.pawnGraphOpen) return;

    ActorRepl::NetGUIDRef attachmentRef{/*isDynamic=*/true, 0u};
    if (weaponChannel != 0) {
        const bool northGraph =
            cs.pawnGraphTeamId == TeamMapping::kServerNva;
        const OwnedWeaponChannelMetadata* metadata =
            FindOwnedWeaponChannel(weaponChannel, northGraph);
        if (!metadata || metadata->attachmentClassRef == 0) {
            Logger::Warn(
                "[GameplayRPC] client %u: no captured attachment class for ch%u",
                clientId, weaponChannel);
            return;
        }
        const std::optional<RetailBootstrap::ArtifactSelection>& artifact =
            GetRetailArtifactSelection(clientId);
        const std::optional<uint32_t> wireAttachmentRef = artifact
            ? RetailBootstrap::ResolveRoGameContentWireRef(
                  *artifact, metadata->attachmentClassRef)
            : std::nullopt;
        if (!wireAttachmentRef) {
            Logger::Error(
                "[GameplayRPC] client %u: cannot resolve attachment class %u "
                "for frozen PackageMap",
                clientId, metadata->attachmentClassRef);
            return;
        }
        attachmentRef = ActorRepl::NetGUIDRef{
            /*isDynamic=*/false, *wireAttachmentRef};
    }

    BitWriter currentAttachment;
    ActorRepl::WritePropObject(currentAttachment, 147, 168, attachmentRef);

    PacketCodec::Bunch bunch;
    bunch.bReliable = false; // capture-matched pawn property delta
    bunch.chIndex = kLocalPawnChannel;
    bunch.chType = 2;
    bunch.chSequence = 0;
    bunch.payload = currentAttachment.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(currentAttachment.NumBits());
    SendReliableBunches(clientId, {bunch});

    Logger::Debug(
        "[GameplayRPC] client %u: pawn current attachment -> %s%u (%u bits)",
        clientId, attachmentRef.isDynamic ? "dynamic ch" : "static ",
        attachmentRef.index, bunch.payloadBits);
}

bool ConnectionManager::SendGivePawn(uint32_t clientId,
                                     uint64_t expectedPawnGeneration) {
    ControlState& cs = GetControlState(clientId);
    if (!HasLiveOwningPawnGeneration(cs, expectedPawnGeneration)) {
        Logger::Warn(
            "[ConnectionManager::SendGivePawn] client %u owning-pawn "
            "generation changed before recovery/redeploy (expected=%llu, "
            "current=%llu, graph=%llu)",
            clientId,
            static_cast<unsigned long long>(expectedPawnGeneration),
            static_cast<unsigned long long>(cs.owningPawnGeneration),
            static_cast<unsigned long long>(cs.pawnGraphGeneration));
        return false;
    }

    const auto reservation = ReserveCh2Reliable(
        cs, clientId, 3u, "GivePawn recovery");
    if (!reservation) return false;
    std::vector<PacketCodec::Bunch> bunches =
        BuildGivePawnBunches(
            cs.actorChType, kLocalPawnChannel,
            reservation->SequenceValues());
    if (!SendReservedCh2Bunches(
            clientId, bunches, *reservation, "GivePawn recovery")) {
        return false;
    }
    Logger::Info("[ConnectionManager::SendGivePawn] client %u: answered AskForPawn with "
                  "PC.Pawn(h24)->ch%u + GivePawn(h43) seq %u + ClientOnPossess(h150) seq %u + alive h151 seq %u",
                  clientId, kLocalPawnChannel, (*reservation)[0],
                  (*reservation)[1], (*reservation)[2]);
    return true;
}

// Names for the handful of ROPlayerController net-field handles we recognise on the
// wire (ground truth from tools/netfields_from_u.ps1). For logging/dispatch only.
static const char* RoPcHandleName(uint32_t h) {
    switch (h) {
        case 37:  return "ServerShortTimeout";
        case 42:  return "AskForPawn";
        case 44:  return "ServerAcknowledgePossession";
        case 63:  return "DualServerMove";
        case 64:  return "OldServerMove";
        case 65:  return "ServerMove";
        case 89:  return "ServerSetSpectatorLocation";
        case 104: return "ServerUpdateLevelVisibility";
        case 152: return "ChangeVivoxChannelsState";
        case 280: return "ServerAttemptMantle";
        case 297: return "MantleServerMove";
        case 170: return "SelectTeam";
        case 172: return "ChangedTeams";
        case 175: return "SelectRoleByClass";
        case 206: return "ClientShowTeamSelect";
        case 207: return "ClientShowRoleSelect";
        case 208: return "ServerReOpenSpawnSelect";
        case 209: return "ClientReOpenSpawnSelect";
        case 210: return "ChangedRole";
        case 225: return "ClientShowRoundStartScreen";
        case 226: return "ClientHideRoundStartScreen";
        case 261: return "ServerSetSpawnSelect";
        case 370: return "ServerSetSpawnVolumeViewTarget";
        case 434: return "ServerSetReadyToSpawn";
        case 82:  return "ServerChangeTeam";
        case 80:  return "ServerSuicide";
        case 27:  return "ServerRestartPlayer";
        default:  return "?";
    }
}

void ConnectionManager::DispatchInboundActorBunch(
    uint32_t clientId, const PacketCodec::Bunch& bunch,
    bool suppressOwningGraphSemantics,
    bool suppressReleasedCohort) {
    // ch0 has different semantics: ControlReassembler concatenates ordered
    // reliable fragments into complete NMT messages. Never let it enter the
    // actor sequencer, even if a future caller bypasses ParseIncomingControl.
    if (bunch.chIndex == 0) {
        Logger::Warn(
            "[ActorReliable] client %u: refused control-channel bunch in actor "
            "dispatcher",
            clientId);
        return;
    }
    if (bunch.chIndex >= static_cast<uint32_t>(kMaxChannels)) {
        Logger::Warn(
            "[ActorReliable] client %u: dropped actor bunch with invalid channel "
            "%u",
            clientId, bunch.chIndex);
        return;
    }

    ControlState& cs = GetControlState(clientId);
    if (bunch.bClose && bunch.chIndex == 2u) {
        // The owning PlayerController channel is never reused within one UE3
        // session. Treat even an out-of-order close as terminal immediately;
        // otherwise a buffered close plus a historical reliable cursor could
        // pass map-travel preflight on a channel that cannot accept the RPC.
        cs.outboundActorChannels.reset(2u);
    }
    if (!suppressOwningGraphSemantics && bunch.bClose &&
        OwningPawnGraphChannelIndex(bunch.chIndex).has_value() &&
        (cs.pawnGraphPhase == OwningPawnGraphPhase::Open ||
         cs.pawnGraphPhase == OwningPawnGraphPhase::Closing)) {
        // A peer actor close is rejection/failure, not acknowledgement of the
        // server's close bunch. The fixed channel cannot be reused safely in
        // this connection after the client has independently torn it down.
        FailOwningPawnGraph(clientId, "peer-originated fixed-channel close");
        cs.actorReliableInbound.DiscardPending(bunch.chIndex);
        return;
    }
    auto suppressM61VisualChannel = [&cs](uint32_t channel) {
        if (!cs.m61Visuals.SuppressChannel(channel)) return false;

        // A peer close/failure means this client will not accept more traffic
        // for this actor incarnation. Keep the channel quarantined in the pool,
        // but remove its bunches from mixed retransmit sets so they cannot leak
        // forever or repeatedly provoke the same rejection. Packet ids remain
        // valid for any other bunches that shared the original packet.
        auto& pending = cs.pendingReliable;
        for (auto reliable = pending.begin(); reliable != pending.end();) {
            auto& bunches = reliable->bunches;
            bunches.erase(
                std::remove_if(
                    bunches.begin(), bunches.end(),
                    [channel](const PacketCodec::Bunch& queued) {
                        return queued.chIndex == channel;
                    }),
                bunches.end());
            if (bunches.empty()) {
                reliable = pending.erase(reliable);
            } else {
                ++reliable;
            }
        }
        cs.m61CloseAcknowledged.reset(channel);
        return true;
    };

    // Unreliable actor traffic intentionally retains packet/datagram order. It
    // must never wait behind a missing reliable bunch on the same actor channel.
    if (!bunch.bReliable) {
        if (suppressOwningGraphSemantics) {
            Logger::Info(
                "[OwningPawnGraph] client %u suppressed delayed unreliable "
                "ch%u semantics from a prior incarnation",
                clientId, bunch.chIndex);
            return;
        }
        DecodeInboundActorBunch(clientId, bunch);
        if (bunch.bClose) {
            if (cs.remoteParticipants.MarkChannelClosed(bunch.chIndex)) {
                Logger::Warn(
                    "[ParticipantReplication] client %u rejected/closed remote "
                    "actor channel %u; suppressing further deltas",
                    clientId, bunch.chIndex);
            }
            if (suppressM61VisualChannel(bunch.chIndex)) {
                Logger::Warn(
                    "[M61Visual] client %u rejected/closed projectile actor "
                    "channel %u; quarantining it for this connection",
                    clientId, bunch.chIndex);
            }
            cs.actorReliableInbound.DiscardPending(bunch.chIndex);
            Logger::Debug(
                "[ActorReliable] client %u: discarded pending ch%u traffic after "
                "unreliable close while preserving its reliable cursor",
                clientId, bunch.chIndex);
        }
        return;
    }

    // Normally actor channels start at reliable sequence 1. If a legitimate
    // actor-open is the first inbound evidence for a reused/adopted channel,
    // seed the cursor from that opening sequence. Never reseed a channel which
    // already has pending traffic or has advanced, so an opening retransmit is
    // still classified as a duplicate/stale bunch rather than delivered twice.
    if (bunch.bOpen &&
        cs.actorReliableInbound.NextSequence(bunch.chIndex) ==
            PacketCodec::ActorReliableSequencer::kDefaultFirstSequence &&
        cs.actorReliableInbound.PendingBunchCount(bunch.chIndex) == 0u) {
        cs.actorReliableInbound.ResetChannel(bunch.chIndex, bunch.chSequence);
    }

    PacketCodec::ActorReliableSequenceResult result =
        cs.actorReliableInbound.Push(bunch);
    const std::optional<size_t> owningGraphIndex =
        OwningPawnGraphChannelIndex(bunch.chIndex);
    const auto failAckedReliableReceiveHole =
        [&](const char* context) {
            if (owningGraphIndex) {
                FailOwningPawnGraph(clientId, context);
                return;
            }
            const std::shared_ptr<ClientConnection> connection =
                GetConnection(clientId);
            if (connection && !connection->IsDisconnected()) {
                connection->MarkDisconnected();
            }
            Logger::Error(
                "[ActorReliable] client %u fail-closed after %s; its packet "
                "was ACKed and the reliable receive cursor cannot recover",
                clientId, context ? context : "an inbound receive hole");
        };
    using Status = PacketCodec::ActorReliableSequenceStatus;
    switch (result.status) {
        case Status::Released: {
            if (result.released.empty()) {
                Logger::Warn(
                    "[ActorReliable] client %u: ch%u sequence %u reported "
                    "released without a bunch; dropping",
                    clientId, bunch.chIndex, bunch.chSequence);
                return;
            }

            for (size_t i = 0; i < result.released.size(); ++i) {
                const PacketCodec::Bunch& ready = result.released[i];
                bool suppressReady =
                    suppressOwningGraphSemantics &&
                    (suppressReleasedCohort ||
                     (ready.chIndex == bunch.chIndex &&
                      ready.chSequence == bunch.chSequence));
                if (const std::optional<size_t> readyIndex =
                        OwningPawnGraphChannelIndex(ready.chIndex);
                    readyIndex &&
                    ready.chSequence < PacketCodec::kMaxChSequence &&
                    cs.owningPawnGraphSuppressedInboundReliable[*readyIndex]
                        .test(ready.chSequence)) {
                    suppressReady = true;
                    cs.owningPawnGraphSuppressedInboundReliable[*readyIndex]
                        .reset(ready.chSequence);
                }
                if (suppressReady) {
                    Logger::Info(
                        "[OwningPawnGraph] client %u retired delayed "
                        "prior-incarnation ch%u reliable seq%u without "
                        "semantic dispatch",
                        clientId, ready.chIndex, ready.chSequence);
                    continue;
                }
                DecodeInboundActorBunch(clientId, ready);
                if (!ready.bClose) continue;

                if (cs.remoteParticipants.MarkChannelClosed(ready.chIndex)) {
                    Logger::Warn(
                        "[ParticipantReplication] client %u rejected/closed remote "
                        "actor channel %u at sequence %u; suppressing further deltas",
                        clientId, ready.chIndex, ready.chSequence);
                }
                if (suppressM61VisualChannel(ready.chIndex)) {
                    Logger::Warn(
                        "[M61Visual] client %u rejected/closed projectile actor "
                        "channel %u at sequence %u; quarantining it for this "
                        "connection",
                        clientId, ready.chIndex, ready.chSequence);
                }

                // A close ends this incarnation of the channel. Any successors
                // buffered behind it cannot belong to the closed incarnation,
                // but UE3's InReliable[ChIndex] cursor persists across channel
                // reuse. Preserve that cursor/history so a delayed close
                // retransmit cannot close or stall the next actor incarnation.
                const size_t discarded = result.released.size() - i - 1u;
                cs.actorReliableInbound.DiscardPending(ready.chIndex);
                Logger::Debug(
                    "[ActorReliable] client %u: closed ch%u at sequence %u; "
                    "discarded %zu buffered successor(s)",
                    clientId, ready.chIndex, ready.chSequence, discarded);
                break;
            }
            return;
        }
        case Status::Buffered:
            if (suppressOwningGraphSemantics && owningGraphIndex &&
                bunch.chSequence < PacketCodec::kMaxChSequence) {
                cs.owningPawnGraphSuppressedInboundReliable[
                    *owningGraphIndex].set(bunch.chSequence);
            }
            Logger::Trace(
                "[ActorReliable] client %u: buffered ch%u sequence %u (next %u, "
                "pending %zu)",
                clientId, bunch.chIndex, bunch.chSequence,
                cs.actorReliableInbound.NextSequence(bunch.chIndex),
                cs.actorReliableInbound.PendingBunchCount(bunch.chIndex));
            return;
        case Status::Duplicate:
            Logger::Debug(
                "[ActorReliable] client %u: dropped duplicate ch%u sequence %u",
                clientId, bunch.chIndex, bunch.chSequence);
            return;
        case Status::Stale:
            Logger::Debug(
                "[ActorReliable] client %u: dropped stale ch%u sequence %u "
                "(next %u)",
                clientId, bunch.chIndex, bunch.chSequence,
                cs.actorReliableInbound.NextSequence(bunch.chIndex));
            return;
        case Status::GapOverflow:
            failAckedReliableReceiveHole(
                suppressOwningGraphSemantics
                    ? "prior-incarnation inbound reliable gap"
                    : "inbound reliable gap beyond UE3 RELIABLE_BUFFER");
            Logger::Warn(
                "[ActorReliable] client %u: dropped ch%u sequence %u beyond the "
                "bounded reorder window (next %u, window %u)",
                clientId, bunch.chIndex, bunch.chSequence,
                cs.actorReliableInbound.NextSequence(bunch.chIndex),
                cs.actorReliableInbound.ReorderWindow());
            return;
        case Status::CapacityExceeded:
            failAckedReliableReceiveHole(
                suppressOwningGraphSemantics
                    ? "prior-incarnation inbound reliable capacity"
                    : "inbound reliable reorder capacity exhaustion");
            Logger::Warn(
                "[ActorReliable] client %u: dropped ch%u sequence %u because "
                "the connection reorder buffer is full (%zu/%zu bunches, "
                "%zu/%zu payload bytes)",
                clientId, bunch.chIndex, bunch.chSequence,
                cs.actorReliableInbound.PendingBunchCount(),
                cs.actorReliableInbound.MaximumPendingBunches(),
                cs.actorReliableInbound.PendingPayloadBytes(),
                cs.actorReliableInbound.MaximumPendingPayloadBytes());
            return;
        case Status::InvalidBunch:
            failAckedReliableReceiveHole(
                suppressOwningGraphSemantics
                    ? "malformed prior-incarnation reliable"
                    : "malformed inbound reliable actor bunch");
            Logger::Warn(
                "[ActorReliable] client %u: dropped malformed reliable actor "
                "bunch ch%u sequence %u",
                clientId, bunch.chIndex, bunch.chSequence);
            return;
        case Status::Unreliable:
            // The flag was checked above; fail closed if the helper contract is
            // ever changed rather than accidentally delivering a reliable RPC.
            Logger::Warn(
                "[ActorReliable] client %u: unexpected unreliable status for "
                "reliable ch%u sequence %u; dropping",
                clientId, bunch.chIndex, bunch.chSequence);
            return;
    }
}

void ConnectionManager::DecodeInboundActorBunch(uint32_t clientId,
                                                 const PacketCodec::Bunch& bunch) {
    // (Removed the old "proof-of-life re-send" of ClientShowTeamSelect: it sent a NEW
    // bunch at ch2 seq+1, manufacturing a sequence GAP if the original seq was dropped ->
    // ch2 stall -> soft-lock. Reliable retransmission now redelivers the original
    // ClientShowTeamSelect (same ChSequence) until the client acks it.)

    if (bunch.bClose) {
        const auto stateIt = m_controlState.find(clientId);
        if (stateIt != m_controlState.end() &&
            bunch.chIndex < ActorRepl::kDynamicChannelMax) {
            stateIt->second.outboundActorChannels.reset(bunch.chIndex);
            Logger::Debug(
                "[ActorLifecycle] client %u closed server-opened actor ch%u; "
                "marked it unavailable for future RPCs",
                clientId, bunch.chIndex);
        }
        return;
    }
    if (bunch.bControl || bunch.bOpen || bunch.payloadBits == 0) return;

    ControlState& gameplay = GetControlState(clientId);
    const bool gameplayActive = IsRetailGameplayActive(clientId);
    const bool northOwningGraph =
        gameplay.pawnGraphOpen &&
        gameplay.pawnGraphTeamId == TeamMapping::kServerNva;

    // ChangeVivoxChannelsState(OtherPlayerROPRI, LocalPlayerROPC) is an
    // informational PlayerController callback. Probe it on the owning channel
    // before the general PC multi-schema walker. Other actor classes retain
    // their own field tables; the existing channel dispatch rejects a PC RPC
    // there without ever treating it as h152. Retail commonly concatenates
    // many exact 33-bit records.
    if (bunch.chIndex == 2u) {
        BitReader vivoxProbe(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        const uint32_t vivoxHandle =
            vivoxProbe.SerializeInt(kRoPcMaxHandle);
        if (!vivoxProbe.IsOverflowed() &&
            vivoxHandle == DeploymentRepl::kChangeVivoxChannelsStateHandle) {
            auto rejectVivox = [&](const char* reason) {
                ++gameplay.vivoxBunchesRejected;
                Logger::Warn(
                    "[VoiceRPC] client %u rejected ChangeVivoxChannelsState "
                    "batch on ch%u (%s, %u bits)",
                    clientId, bunch.chIndex, reason, bunch.payloadBits);
            };
            if (!bunch.bReliable) {
                rejectVivox("requires reliable owning PlayerController ch2");
                return;
            }

            const auto decoded =
                DeploymentRepl::DecodeChangeVivoxChannelsStateBunch(
                    bunch.payload.data(), bunch.payload.size(),
                    bunch.payloadBits);
            if (!decoded.valid()) {
                rejectVivox("malformed or unsupported wire schema");
                return;
            }

            for (size_t i = 0; i < decoded.bunch.recordCount; ++i) {
                const auto& record = decoded.bunch.records[i];
                if (record.localPlayerControllerChannel != 2u) {
                    rejectVivox("LocalPlayerROPC is not owning ch2");
                    return;
                }
                if (record.otherPlayerPriChannel == 26u) {
                    continue; // this connection's fixed local PRI
                }
                const ParticipantActorChannelBinding* remote =
                    gameplay.remoteParticipants.FindByChannel(
                        record.otherPlayerPriChannel);
                if (!remote ||
                    remote->priChannel != record.otherPlayerPriChannel ||
                    remote->priState != ParticipantActorOpenState::Open) {
                    rejectVivox("OtherPlayerROPRI is not an open viewer PRI");
                    return;
                }
            }

            gameplay.vivoxNoOpRecordsAccepted += decoded.bunch.recordCount;
            Logger::Trace(
                "[VoiceRPC] client %u accepted %zu "
                "ChangeVivoxChannelsState no-op record(s)",
                clientId, decoded.bunch.recordCount);
            return;
        }
    }

    if (bunch.chIndex == kLocalPawnChannel) {
        if (!gameplayActive) {
            gameplay.mantlePawnStarted = false;
            return;
        }
        const auto decoded = GameplayRpc::DecodePawn(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (decoded.complete && !decoded.events.empty()) {
            for (const auto& event : decoded.events) {
                if (event.kind == GameplayRpc::PawnKind::MantleStarted) {
                    gameplay.mantlePawnStarted = true;
                    Logger::Info("[GameplayRPC] client %u: pawn h85 mantle start observed; no position snap without authoritative DynamicMantleInfo",
                                 clientId);
                } else {
                    Logger::Debug("[GameplayRPC] client %u: pawn h77 forced-crouch request observed",
                                  clientId);
                }
            }
        }
        return; // never reinterpret a pawn payload with the PC field table
    }

    if (bunch.chIndex >= 210 && bunch.chIndex <= 214) {
        if (!gameplay.pawnGraphOpen) {
            Logger::Warn(
                "[GameplayRPC] client %u sent weapon RPC on unopened owning ch%u",
                clientId, bunch.chIndex);
            return;
        }
        const OwnedWeaponChannelMetadata* metadata =
            FindOwnedWeaponChannel(bunch.chIndex, northOwningGraph);
        if (!metadata) return;
        const size_t weaponSlot = bunch.chIndex - 210u;
        if (!gameplayActive) {
            gameplay.weaponIntent[weaponSlot] = {};
            if (bunch.chIndex == 212u) {
                if (m_server) m_server->CancelRetailGrenadeCook(clientId);
            }
            return;
        }

        // h56 has a larger, source-confirmed schema than the start/stop/reload
        // walker below. Probe and dispatch it first so an exact hit report is
        // never reinterpreted as a generic/unknown weapon RPC. Dynamic object
        // references are connection-local actor channels: only the owning pawn
        // and a currently open/alive remote pawn identify a participant. A PRI,
        // closing/dead pawn, or unknown dynamic channel fails closed. Static
        // package-map actors remain valid world impacts without a participant.
        BitReader weaponProbe(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        const uint32_t firstWeaponHandle = weaponProbe.SerializeInt(
            metadata->maxHandle);
        if (!weaponProbe.IsOverflowed() &&
            firstWeaponHandle ==
                WeaponCombatRepl::kServerHandleClientHitsOne) {
            const uint32_t authorizedChannel = gameplay.activeWeaponChannel;
            if (metadata->identity != OwnedWeaponIdentity::FactionPrimary ||
                !bunch.bReliable || bunch.chIndex != authorizedChannel) {
                Logger::Warn(
                    "[CombatAuthority] client %u: rejected h56 on %s weapon ch%u "
                    "(authorized ch%u)",
                    clientId, bunch.bReliable ? "inactive" : "unreliable",
                    bunch.chIndex, authorizedChannel);
                return;
            }

            const auto resolver = [clientId, &gameplay](
                const ActorRepl::NetGUIDRef& reference) {
                WeaponCombatRepl::ActorResolution resolution;
                if (reference.isDynamic) {
                    if (reference.index == kLocalPawnChannel) {
                        resolution.known = true;
                        resolution.participantId =
                            ParticipantId::Human(clientId);
                        return resolution;
                    }
                    const std::optional<ParticipantId> participant =
                        gameplay.remoteParticipants.ResolveOpenLivingPawn(
                            reference.index);
                    if (participant) {
                        resolution.known = true;
                        resolution.participantId = *participant;
                    }
                    return resolution;
                }
                // A non-zero static index names a world/package-map object. It
                // can validate a miss/impact but is never promoted to a human.
                resolution.known = reference.index != 0;
                return resolution;
            };
            const auto decoded =
                WeaponCombatRepl::DecodeServerHandleClientHitsOne(
                    bunch.payload.data(), bunch.payload.size(),
                    bunch.payloadBits, resolver);
            if (!decoded.valid()) {
                Logger::Warn(
                    "[CombatAuthority] client %u: rejected malformed h56 on ch%u "
                    "(decode error %u, consumed %zu/%u bits)",
                    clientId, bunch.chIndex,
                    static_cast<unsigned>(decoded.error),
                    decoded.consumedBits, bunch.payloadBits);
                return;
            }

            gameplay.weaponIntent[weaponSlot].fireMode =
                decoded.rpc.firedMode;
            const bool accepted = m_server &&
                m_server->HandleRetailCombatHit(clientId, decoded.rpc);
            Logger::Info(
                "[CombatAuthority] client %u: h56 on weapon ch%u %s",
                clientId, bunch.chIndex,
                accepted ? "accepted" : "rejected by authority");
            return;
        }

        const auto decoded = GameplayRpc::DecodeWeapon(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits,
            metadata->maxHandle);
        const bool hasGrenadeCookTransition =
            metadata->identity == OwnedWeaponIdentity::FactionGrenade &&
            std::any_of(
                decoded.events.begin(), decoded.events.end(),
                [](const GameplayRpc::WeaponEvent& event) {
                    return event.kind == GameplayRpc::WeaponKind::StartFire ||
                        event.kind == GameplayRpc::WeaponKind::StopFire;
                });
        if (hasGrenadeCookTransition) {
            // M61 and Type67 inherit the same exact ROExplosiveWeapon h29/h30
            // field-table tail. Both captures send one reliable RPC per actor
            // bunch. Keep that boundary strict: accepting a manufactured
            // multi-transition bunch would allow one ChSequence to represent
            // several throws.
            const bool capturedGrenadeClass =
                metadata->classRef == 286464u || // South M61
                metadata->classRef == 286804u;   // North Type67
            const char* grenadeName = northOwningGraph ? "Type67" : "M61";
            if (!decoded.complete || decoded.events.size() != 1u ||
                !bunch.bReliable || gameplay.activeWeaponChannel != 212u ||
                bunch.chIndex != 212u || !capturedGrenadeClass ||
                metadata->maxHandle != GameplayRpc::kM61WeaponMaxHandle ||
                !m_server) {
                Logger::Warn(
                    "[CombatAuthority] client %u rejected %s cook RPC on ch%u "
                    "(reliable=%u active=%u complete=%u events=%zu)",
                    clientId, grenadeName, bunch.chIndex,
                    bunch.bReliable ? 1u : 0u,
                    gameplay.activeWeaponChannel, decoded.complete ? 1u : 0u,
                    decoded.events.size());
                return;
            }

            const GameplayRpc::WeaponEvent& event = decoded.events.front();
            if (event.fireMode != GameplayRpc::kM61OverhandFireMode &&
                event.fireMode != GameplayRpc::kM61TossFireMode) {
                Logger::Warn(
                    "[CombatAuthority] client %u rejected %s fire mode %u",
                    clientId, grenadeName, event.fireMode);
                return;
            }
            auto& intent = gameplay.weaponIntent[weaponSlot];
            intent.fireMode = event.fireMode;
            if (event.kind == GameplayRpc::WeaponKind::StartFire) {
                if (!m_server->BeginRetailGrenadeCook(
                        clientId, event.fireMode)) {
                    Logger::Warn(
                        "[CombatAuthority] client %u %s cook rejected by "
                        "authority",
                        clientId, grenadeName);
                    return;
                }
                intent.firing = true;
                Logger::Info(
                    "[CombatAuthority] client %u began %s cook on owned ch%u "
                    "(class %u, mode %u)",
                    clientId, grenadeName, bunch.chIndex, metadata->classRef,
                    event.fireMode);
                return;
            }

            // Consume the state before entering GameServer. Any rejected throw
            // still requires a fresh h29; a retransmitted h30 cannot retry it.
            intent.firing = false;

            GameplayRpc::AimDirection aim;
            if (gameplay.latestViewValid) {
                aim = GameplayRpc::DirectionFromPackedView(
                    gameplay.latestPackedView);
            }
            std::shared_ptr<Player> player;
            if (const auto* players = m_server->GetPlayerManager()) {
                player = players->GetPlayer(clientId);
            }
            if (!aim.valid && player) {
                aim = GameplayRpc::NormalizeAimDirection(
                    player->GetOrientation());
            }
            if (!aim.valid || !player) {
                m_server->CancelRetailGrenadeCook(clientId);
                Logger::Warn(
                    "[CombatAuthority] client %u rejected %s release without "
                    "a finite validated aim/player",
                    clientId, grenadeName);
                return;
            }

            const bool accepted = m_server->ReleaseRetailGrenade(
                clientId, event.fireMode, aim.value);
            Logger::Info(
                "[CombatAuthority] client %u released %s mode %u: %s",
                clientId, grenadeName, event.fireMode,
                accepted ? "launched" : "rejected by authority");
            return;
        }

        if (decoded.complete && !decoded.events.empty()) {
            auto& intent = gameplay.weaponIntent[weaponSlot];
            for (const auto& event : decoded.events) {
                switch (event.kind) {
                    case GameplayRpc::WeaponKind::StartFire:
                        intent.firing = true;
                        intent.fireMode = event.fireMode;
                        Logger::Info("[GameplayRPC] client %u: weapon ch%u start-fire mode %u (intent; awaiting exact h56 hit report)",
                                     clientId, bunch.chIndex, event.fireMode);
                        break;
                    case GameplayRpc::WeaponKind::StopFire:
                        intent.firing = false;
                        intent.fireMode = event.fireMode;
                        Logger::Debug("[GameplayRPC] client %u: weapon ch%u stop-fire mode %u",
                                      clientId, bunch.chIndex, event.fireMode);
                        break;
                    case GameplayRpc::WeaponKind::RequestReload:
                        intent.reloadRequested = true;
                        {
                            const uint32_t authorizedChannel =
                                gameplay.activeWeaponChannel;
                            const bool accepted =
                                bunch.chIndex == authorizedChannel && m_server &&
                                m_server->RequestCombatReload(clientId);
                            Logger::Info(
                                "[CombatAuthority] client %u: weapon ch%u reload %s",
                                clientId, bunch.chIndex,
                                accepted ? "accepted" : "rejected");
                        }
                        break;
                }
            }
        }
        return; // never reinterpret a weapon payload with the PC field table
    }

    if (bunch.chIndex == 219) {
        if (!gameplay.pawnGraphOpen) {
            Logger::Warn(
                "[GameplayRPC] client %u sent inventory RPC on unopened ch219",
                clientId);
            return;
        }
        if (!gameplayActive) {
            gameplay.activeWeaponChannel = 0u;
            gameplay.weaponIntent.fill({});
            if (m_server) m_server->CancelRetailGrenadeCook(clientId);
            return;
        }
        const auto decoded = GameplayRpc::DecodeInventoryManager(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (decoded.complete && !decoded.events.empty()) {
            const auto& event = decoded.events.back();
            const auto& weapon = event.desiredWeapon;
            if (!event.hasDesiredWeapon) {
                gameplay.activeWeaponChannel = 0;
                if (m_server) m_server->CancelRetailGrenadeCook(clientId);
                SendOwningPawnCurrentAttachment(clientId, 0);
                Logger::Debug("[GameplayRPC] client %u: active weapon cleared", clientId);
            } else if (weapon.isDynamic) {
                const OwnedWeaponChannelMetadata* selected =
                    FindOwnedWeaponChannel(weapon.index, northOwningGraph);
                const bool identified = selected &&
                    selected->identity != OwnedWeaponIdentity::Unknown;
                const bool activated = identified && m_server &&
                    m_server->SelectCombatWeaponChannel(
                        clientId, weapon.index, selected->classRef);
                if (activated) {
                    gameplay.activeWeaponChannel = weapon.index;
                    SendOwningPawnCurrentAttachment(clientId, weapon.index);
                    Logger::Info(
                        "[GameplayRPC] client %u: active combat weapon -> ch%u "
                        "(maxHandle %u)",
                        clientId, weapon.index, selected->maxHandle);
                } else {
                    gameplay.activeWeaponChannel = 0;
                    if (m_server) m_server->CancelRetailGrenadeCook(clientId);
                    SendOwningPawnCurrentAttachment(clientId, 0);
                    Logger::Warn(
                        "[GameplayRPC] client %u: rejected unidentified or "
                        "unavailable owned weapon ch%u",
                        clientId, weapon.index);
                }
            } else {
                gameplay.activeWeaponChannel = 0;
                if (m_server) m_server->CancelRetailGrenadeCook(clientId);
                SendOwningPawnCurrentAttachment(clientId, 0);
                Logger::Warn("[GameplayRPC] client %u: rejected non-owned active weapon reference (dynamic=%u index=%u)",
                             clientId, weapon.isDynamic ? 1u : 0u, weapon.index);
            }
        }
        return;
    }

    if (bunch.chIndex != 2) return;
    const MovementValidator::TimePoint movementReceiptTime =
        MovementValidator::Clock::now();

    // First give the bounded multi-RPC walker all evidenced gameplay schemas.
    // If the first handle is a menu/possession RPC it decodes no events and the
    // established reliable PC dispatcher below remains responsible for it.
    const auto pc = GameplayRpc::DecodePlayerController(
        bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
    if (!pc.events.empty() && !pc.complete) {
        Logger::Debug("[GameplayRPC] client %u: dropped incomplete PC multi-RPC bunch%s at h%u after %zu bits",
                      clientId, pc.stoppedOnUnknown ? " (unknown)" : "",
                      pc.unknownHandle, pc.consumedBits);
        return;
    }
    if (pc.complete && !pc.events.empty()) {
        // These stock PlayerController calls are connection housekeeping. Log
        // their decoded values for capture parity, but never feed them into
        // movement, spawn, objective, or other gameplay authority.
        const auto isHousekeepingEvent = [](const auto& event) {
            return event.kind == GameplayRpc::PcKind::ShortTimeout ||
                   event.kind == GameplayRpc::PcKind::SetSpectatorLocation ||
                   event.kind == GameplayRpc::PcKind::UpdateLevelVisibility;
        };
        for (const auto& event : pc.events) {
            switch (event.kind) {
                case GameplayRpc::PcKind::ShortTimeout:
                    Logger::Debug(
                        "[GameplayRPC] client %u: h37 ServerShortTimeout observed",
                        clientId);
                    break;
                case GameplayRpc::PcKind::SetSpectatorLocation:
                    Logger::Debug(
                        "[GameplayRPC] client %u: h89 spectator location "
                        "present=%u (%.0f,%.0f,%.0f)",
                        clientId,
                        event.spectatorLocation.hasLocation ? 1u : 0u,
                        event.spectatorLocation.location.x,
                        event.spectatorLocation.location.y,
                        event.spectatorLocation.location.z);
                    break;
                case GameplayRpc::PcKind::UpdateLevelVisibility:
                    if (!event.levelVisibility.hasPackageName) {
                        Logger::Debug(
                            "[GameplayRPC] client %u: h104 level visibility "
                            "PackageName=None visible=%u",
                            clientId,
                            event.levelVisibility.visible ? 1u : 0u);
                    } else if (event.levelVisibility.packageName.hardcoded) {
                        Logger::Debug(
                            "[GameplayRPC] client %u: h104 level visibility "
                            "PackageName=EName[%u] visible=%u",
                            clientId,
                            event.levelVisibility.packageName.hardcodedIndex,
                            event.levelVisibility.visible ? 1u : 0u);
                    } else {
                        Logger::Debug(
                            "[GameplayRPC] client %u: h104 level visibility "
                            "PackageName=%s Number=%d visible=%u",
                            clientId,
                            event.levelVisibility.packageName.text.c_str(),
                            event.levelVisibility.packageName.number,
                            event.levelVisibility.visible ? 1u : 0u);
                    }
                    break;
                default:
                    break;
            }
        }
        const auto firstGameplayEvent = std::find_if(
            pc.events.begin(), pc.events.end(),
            [&isHousekeepingEvent](const auto& event) {
                return !isHousekeepingEvent(event);
            });
        if (firstGameplayEvent == pc.events.end()) {
            return;
        }
        if (!gameplayActive) {
            gameplay.movementInputValid = false;
            gameplay.useHeld = false;
            gameplay.mantleAttemptPending = false;
            return;
        }
        const auto& firstEvent = *firstGameplayEvent;
        const size_t mantleEventCount = static_cast<size_t>(std::count_if(
            pc.events.begin(), pc.events.end(), [](const auto& event) {
                return event.kind == GameplayRpc::PcKind::AttemptMantle;
            }));
        if (mantleEventCount > 1u ||
            (mantleEventCount == 1u &&
             firstEvent.kind != GameplayRpc::PcKind::AttemptMantle)) {
            Logger::Warn(
                "[MantleAuthority] client %u: dropped ambiguous h280 RPC order "
                "(%zu attempts)",
                clientId, mantleEventCount);
            gameplay.mantleAttemptPending = false;
            return;
        }

        // Retail emits h280 first and may append a movement RPC. Ask the game
        // authority before applying any appended client location, then suppress
        // that location for this transaction so it cannot overwrite an accepted
        // server destination or smuggle movement through a rejected mantle.
        const bool mantleTransaction =
            firstEvent.kind == GameplayRpc::PcKind::AttemptMantle;
        bool mantleAccepted = false;
        uint8_t mantleSpecialMove = 0;
        MantleRepl::DynamicInfo mantleDynamic;
        if (mantleTransaction && m_server) {
            mantleAccepted = m_server->RequestRetailMantle(
                clientId, firstEvent.mantle, mantleSpecialMove,
                mantleDynamic);
        }

        std::vector<MovementSampleTiming::Sample> positionSamples;
        for (const auto& event : pc.events) {
            switch (event.kind) {
                case GameplayRpc::PcKind::Movement:
                    gameplay.movementInputValid = true;
                    gameplay.latestMovementHandle = event.movement.handle;
                    // An omitted replicated byte is its logical default, zero;
                    // never retain crouch/jump flags from the preceding move.
                    gameplay.latestMoveFlags = event.movement.moveFlags;
                    // Normal/Dual/specialized ServerMove schemas all carry the
                    // packed View parameter. UE3 omits a default-zero value on
                    // the wire, so zero is still a validated view for those
                    // schemas; OldServerMove is the only evidenced exception.
                    if (event.movement.handle !=
                        MovementRepl::kOldServerMove) {
                        gameplay.latestViewValid = true;
                        gameplay.latestPackedView = event.movement.view;
                    }
                    if (event.movement.hasClientLocation && m_server &&
                        !mantleTransaction) {
                        const GameplayRpc::AimDirection facing =
                            GameplayRpc::DirectionFromPackedView(
                                event.movement.view);
                        MovementSampleTiming::Sample sample;
                        sample.position = event.movement.clientLocation;
                        // Invalid decoded view data is retained as a zero vector
                        // so MovementValidator rejects it without mutating state.
                        sample.forward = facing.valid
                            ? facing.value
                            : Vector3::Zero();
                        sample.hasClientTimestamp =
                            event.movement.hasTimestamp;
                        sample.clientTimestampSeconds =
                            event.movement.timestamp;
                        positionSamples.push_back(sample);
                    }
                    break;
                case GameplayRpc::PcKind::Use:
                    gameplay.useHeld = true;
                    Logger::Debug("[GameplayRPC] client %u: h79 use intent observed; world-use authority unavailable",
                                  clientId);
                    break;
                case GameplayRpc::PcKind::UseRelease:
                    gameplay.useHeld = false;
                    break;
                case GameplayRpc::PcKind::ShortTimeout:
                case GameplayRpc::PcKind::SetSpectatorLocation:
                case GameplayRpc::PcKind::UpdateLevelVisibility:
                    break; // observational connection housekeeping only
                case GameplayRpc::PcKind::AttemptMantle:
                    gameplay.mantleAttemptPending = true;
                    gameplay.mantleWantsToClimb = event.mantle.wantsToClimb;
                    gameplay.mantleHadLastGoodTrace =
                        event.mantle.hasLastGoodInfo && event.mantle.lastGood.valid;
                    Logger::Debug("[GameplayRPC] client %u: h280 mantle intent (climb=%u lastGood=%u); h344 requires the bounded LastGood validator",
                                  clientId, gameplay.mantleWantsToClimb ? 1u : 0u,
                                  gameplay.mantleHadLastGoodTrace ? 1u : 0u);
                    break;
                case GameplayRpc::PcKind::DoSpecialMove:
                    gameplay.specialMove = event.specialMove.move;
                    gameplay.specialMoveActive = event.specialMove.move != 0;
                    Logger::Debug("[GameplayRPC] client %u: h306 special move %u confirmed",
                                  clientId, event.specialMove.move);
                    break;
                case GameplayRpc::PcKind::EndSpecialMove:
                    gameplay.specialMoveActive = false;
                    gameplay.specialMove = 0;
                    gameplay.mantleAttemptPending = false;
                    gameplay.mantlePawnStarted = false;
                    break;
                case GameplayRpc::PcKind::ResetTeamSwapDelay:
                    break;
            }
        }

        if (!positionSamples.empty()) {
            if (!m_movementValidator || !m_server) {
                Logger::Warn(
                    "[MovementAuthority] client %u position batch dropped: "
                    "validator/server unavailable",
                    clientId);
            } else {
                const MovementSampleTiming::Plan plan =
                    MovementSampleTiming::BuildPlan(
                        positionSamples, movementReceiptTime,
                        // Captured Dual/Old+Server samples are 16-32ms apart.
                        // One second is deliberately generous while preventing
                        // a client-authored 120s intra-bunch time budget.
                        std::chrono::seconds(1),
                        m_movementValidator->GetConfig().duplicateEpsilon);
                if (!plan.valid) {
                    Logger::Warn(
                        "[MovementAuthority] client %u rejected %zu-sample "
                        "movement timing plan (reason=%u)",
                        clientId, positionSamples.size(),
                        static_cast<unsigned>(plan.failure));
                } else if (auto* pm = m_server->GetPlayerManager()) {
                    if (auto player = pm->GetPlayer(clientId)) {
                        for (const auto& planned : plan.samples) {
                            const MovementValidator::Result result =
                                m_movementValidator->ValidateMovementDetailed(
                                    clientId, planned.sample.position,
                                    planned.sample.forward,
                                    planned.timestamp);
                            if (result.accepted) {
                                player->SetPosition(planned.sample.position);
                            } else {
                                Logger::Debug(
                                    "[MovementAuthority] client %u position "
                                    "unchanged after rejected sample "
                                    "(reason=%u)",
                                    clientId,
                                    static_cast<unsigned>(result.failure));
                            }
                        }
                    }
                }
            }
        }

        if (mantleTransaction) {
            gameplay.mantleAttemptPending = mantleAccepted;
            if (mantleAccepted) {
                (void)ResetRetailMovementValidation(
                    clientId, mantleDynamic.endLocation);
                uint32_t responseBits = 0;
                std::vector<uint8_t> response =
                    MantleRepl::EncodeClientStartDynamicMantle(
                        mantleSpecialMove, mantleDynamic, responseBits);
                if (!response.empty() && responseBits > 0) {
                    if (!SendCh2Rpc(clientId, response, responseBits,
                                    "ClientStartDynamicMantle")) {
                        gameplay.mantleAttemptPending = false;
                        Logger::Warn(
                            "[MantleAuthority] client %u accepted mantle could "
                            "not be published on ch2",
                            clientId);
                        return;
                    }
                    gameplay.specialMove = mantleSpecialMove;
                    gameplay.specialMoveActive = true;
                    Logger::Info(
                        "[MantleAuthority] client %u: accepted %s from "
                        "(%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)",
                        clientId,
                        firstEvent.mantle.wantsToClimb ? "climb" : "vault",
                        mantleDynamic.startLocation.x,
                        mantleDynamic.startLocation.y,
                        mantleDynamic.startLocation.z,
                        mantleDynamic.endLocation.x,
                        mantleDynamic.endLocation.y,
                        mantleDynamic.endLocation.z);
                } else {
                    Logger::Error(
                        "[MantleAuthority] client %u: accepted traversal "
                        "could not be encoded as h344",
                        clientId);
                }
            }
        }
        return;
    }

    BitReader probe(bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
    const uint32_t firstHandle = probe.SerializeInt(kRoPcMaxHandle);
    if (probe.IsOverflowed()) return;

    bool reliableStartsWithMovement = false;
    if (bunch.bReliable) {
        reliableStartsWithMovement = !probe.IsOverflowed() &&
            (firstHandle == MovementRepl::kDualServerMove ||
             firstHandle == MovementRepl::kOldServerMove ||
             firstHandle == MovementRepl::kServerMove ||
             firstHandle == MovementRepl::kMantleServerMove);
    }

    if (!bunch.bReliable || reliableStartsWithMovement) {
        const MovementRepl::DecodeResult movement =
            MovementRepl::DecodeRoPlayerControllerMoves(
                bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (!movement.valid) {
            if (!bunch.bReliable) {
                return; // unknown unreliable field or malformed parameters
            }
            // Preserve the existing reliable-RPC path if the bounded movement
            // decoder rejects a future mixed payload.
        } else {
            // The class-aware walker above is the only path that retains every
            // Rpc's View and TimeStamp. A future schema reaching this aggregate
            // fallback may still be observed, but must not bypass movement
            // validation with a location stripped of its sample context.
            if (gameplayActive && movement.hasClientLocation) {
                Logger::Warn(
                    "[MovementAuthority] client %u decoded aggregate movement "
                    "without per-sample context; position unchanged",
                    clientId);
            }
            Logger::Trace("[ConnectionManager::DecodeInboundActorBunch] client %u: decoded %u movement RPC(s), latest=(%.0f,%.0f,%.0f)",
                          clientId, movement.rpcCount,
                          movement.latestClientLocation.x,
                          movement.latestClientLocation.y,
                          movement.latestClientLocation.z);
            return;
        }
    }

    if (!bunch.bReliable) return;

    BitReader r(bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
    const uint32_t handle = r.SerializeInt(kRoPcMaxHandle);
    if (r.IsOverflowed()) return;
    Logger::Info("[ConnectionManager::DecodeInboundActorBunch] client %u: ch%u inbound RPC handle %u (%s), %u bits",
                 clientId, bunch.chIndex, handle, RoPcHandleName(handle), bunch.payloadBits);

    // ServerReOpenSpawnSelect() is the retail console/menu recovery path when
    // the player has no pawn. It is parameterless and must remain on the
    // owning reliable PlayerController channel. Reopen only a finalized,
    // published deployment domain; never use this RPC to bypass role or h59
    // authority.
    if (handle == 208) {
        if (bunch.chIndex != 2u || r.BitPos() != bunch.payloadBits) {
            Logger::Warn(
                "[Deployment] client %u rejected malformed "
                "ServerReOpenSpawnSelect on ch%u (%u bits)",
                clientId, bunch.chIndex, bunch.payloadBits);
            return;
        }
        if (!gameplay.teamSelected || !gameplay.roleFinalized ||
            gameplay.spawned || gameplay.mapTravelPending ||
            gameplay.advertisedSpawnCount == 0) {
            Logger::Warn(
                "[Deployment] client %u rejected ServerReOpenSpawnSelect "
                "outside a published pre-spawn deployment state",
                clientId);
            return;
        }

        BitWriter reopen;
        reopen.SerializeInt(209, kRoPcMaxHandle); // ClientReOpenSpawnSelect()
        if (!SendCh2Rpc(clientId, reopen.GetBytes(),
                        static_cast<uint32_t>(reopen.NumBits()),
                        "ClientReOpenSpawnSelect")) {
            Logger::Warn(
                "[Deployment] client %u could not queue "
                "ClientReOpenSpawnSelect; client may retry h208",
                clientId);
        }
        return;
    }

    // AskForPawn has no parameters. It is the client's standard recovery request when
    // ClientRestart could not resolve NewPawn. Accept only the canonical standalone
    // reliable 9-bit h42 on the owning PlayerController channel. Reliable resend already
    // protects the original possession graph, so rate-limit and cap fresh GivePawn bursts
    // from a client which keeps polling while its pawn class remains unresolved.
    if (handle == 42) {
        ControlState& cs = GetControlState(clientId);
        const uint64_t pawnGeneration = cs.owningPawnGeneration;
        const uint64_t nowMs = NowMs();
        const bool exactStandaloneRequest =
            bunch.bReliable && bunch.chIndex == 2u &&
            bunch.payloadBits == kAskForPawnPayloadBits &&
            !r.IsOverflowed() && r.BitPos() == kAskForPawnPayloadBits;
        switch (EvaluatePossessionRecovery(
            cs, exactStandaloneRequest, pawnGeneration, nowMs)) {
            case PossessionRecoveryDecision::Respond:
                if (!SendGivePawn(clientId, pawnGeneration) ||
                    !CommitPossessionRecoveryResponse(
                        cs, pawnGeneration, nowMs)) {
                    Logger::Warn(
                        "[PossessionRecovery] client %u recovery generation "
                        "%llu changed or backpressured before queueing",
                        clientId,
                        static_cast<unsigned long long>(pawnGeneration));
                }
                break;
            case PossessionRecoveryDecision::Malformed:
                Logger::Warn(
                    "[PossessionRecovery] client %u rejected malformed "
                    "AskForPawn on ch%u (reliable=%s, %u bits)",
                    clientId, bunch.chIndex,
                    bunch.bReliable ? "true" : "false", bunch.payloadBits);
                break;
            case PossessionRecoveryDecision::Ineligible:
                Logger::Trace(
                    "[PossessionRecovery] client %u ignored AskForPawn outside "
                    "an unresolved live owning-pawn graph",
                    clientId);
                break;
            case PossessionRecoveryDecision::StaleGeneration:
                Logger::Warn(
                    "[PossessionRecovery] client %u ignored AskForPawn for a "
                    "stale/unbound owning-pawn generation (live=%llu, graph=%llu)",
                    clientId,
                    static_cast<unsigned long long>(cs.owningPawnGeneration),
                    static_cast<unsigned long long>(cs.pawnGraphGeneration));
                break;
            case PossessionRecoveryDecision::Backpressured:
                Logger::Trace(
                    "[PossessionRecovery] client %u deferred AskForPawn: ch2 "
                    "reliable window has %zu slot(s), needs three",
                    clientId, cs.ch2Reliable.AvailableCapacity());
                break;
            case PossessionRecoveryDecision::RateLimited:
                Logger::Trace(
                    "[PossessionRecovery] client %u rate-limited AskForPawn",
                    clientId);
                break;
            case PossessionRecoveryDecision::LimitReached:
                Logger::Warn(
                    "[PossessionRecovery] client %u exhausted the %u-response "
                    "AskForPawn budget for this deployment; suppressing repeats",
                    clientId,
                    static_cast<unsigned>(kMaxPossessionRecoveryResponses));
                break;
            case PossessionRecoveryDecision::Suppressed:
                break;
        }
        return;
    }

    // ServerAcknowledgePossession(Pawn P): non-bool params carry a presence bit,
    // followed by the dynamic actor reference when present. Record only an ack for
    // the pawn channel we actually opened; an omitted/None parameter is not success.
    if (handle == 44) {
        if (!bunch.bReliable) {
            Logger::Warn(
                "[ConnectionManager] client %u rejected unreliable "
                "ServerAcknowledgePossession",
                clientId);
            return;
        }
        const bool hasPawn = r.ReadBit();
        ActorRepl::NetGUIDRef pawnRef{/*isDynamic=*/true, 0u};
        if (hasPawn) pawnRef = ActorRepl::ReadNetGUID(r);
        ControlState& cs = GetControlState(clientId);
        const uint64_t pawnGeneration = cs.owningPawnGeneration;
        const bool acknowledgedCurrentGeneration =
            hasPawn && !r.IsOverflowed() && pawnRef.isDynamic &&
            pawnRef.index == kLocalPawnChannel &&
            HasLiveOwningPawnGeneration(cs, pawnGeneration);
        if (acknowledgedCurrentGeneration) {
            cs.possessionAckedGeneration = pawnGeneration;
            ResetPossessionRecovery(cs);
        }
        Logger::Info("[ConnectionManager] client %u: ServerAcknowledgePossession(%s%u) -> %s",
                     clientId,
                     hasPawn && pawnRef.isDynamic ? "ch" : "None/",
                     hasPawn ? pawnRef.index : 0u,
                     acknowledgedCurrentGeneration ? "POSSESSED"
                                                   : "not our live generation");
        return;
    }

    // SelectTeam(byte TeamID) - the client clicked a team in the team-select menu.
    // ROPlayerController.uc:3440 (reliable server). On the real server this assigns the
    // team then calls ChangedTeams() to open role select. We advance the client to the
    // role-select scene via ClientShowRoleSelect (handle 207, optional bool).
    if (handle == 170) {
        // UE3 function-call params carry a per-param "Send" PRESENCE BIT for NON-bool
        // params (UnScript.cpp InternalProcessRemoteFunction:2980-3010; receive side
        // UnChan.cpp:1628-1640): read the 1-bit Send flag first; the byte value follows
        // ONLY if Send==1. Send==0 means the value equals its default and is OMITTED.
        // SelectTeam(byte TeamID): TeamID==0 sends Send=0 and NO byte; reading the byte
        // raw would overflow and silently drop the team-0 pick (team-1 previously only
        // "worked" by a &1 masking accident). This is the real team-0 selection bug.
        const bool hasTeamId = r.ReadBit();
        uint8_t teamId = 0;                  // Send==0 -> default 0 (a VALID selection)
        if (hasTeamId) {
            teamId = r.ReadByte();
            if (r.IsOverflowed()) {
                Logger::Warn("[ConnectionManager::DecodeInboundActorBunch] client %u: SelectTeam truncated before TeamID byte, ignoring",
                             clientId);
                return;
            }
        }
        if (teamId > 1) teamId = 1;          // RS2 has two playable teams (0/1)
        ControlState& cs = GetControlState(clientId);
        // ChangedTeams is the one-shot client-side half of SelectTeam. Preflight
        // every fallible ch2 input before changing controller, team, squad, or
        // deployment authority so backpressure cannot create a split state.
        const RetailBootstrap::Profile& bootstrapProfile =
            GetRetailBootstrapProfile(clientId);
        const std::optional<RetailBootstrap::ArtifactSelection>&
            selectedArtifact = GetRetailArtifactSelection(clientId);
        const std::optional<uint32_t> selectedGameClass = selectedArtifact
            ? RetailBootstrap::ResolveGameClassRef(
                  *selectedArtifact, bootstrapProfile.gameClassPath)
            : std::nullopt;
        if (!selectedGameClass) {
            Logger::Warn(
                "[ConnectionManager] client %u: SelectTeam rejected before "
                "authority mutation because GameTypeClass '%s' is not "
                "grounded for the frozen artifact",
                clientId, bootstrapProfile.gameClassPath.c_str());
            FailCloseCh2Publication(
                clientId, "ChangedTeams GameTypeClass preflight");
            return;
        }
        const auto changedTeamsReservation = ReserveCh2Reliable(
            cs, clientId, 1u, "ChangedTeams role-select advance");
        if (!changedTeamsReservation) {
            Logger::Warn(
                "[ConnectionManager] client %u: SelectTeam rejected before "
                "authority mutation because role-select advance is "
                "backpressured",
                clientId);
            FailCloseCh2Publication(
                clientId, "one-shot ChangedTeams reservation");
            return;
        }
        Logger::Info("[ConnectionManager] client %u: SelectTeam(TeamID=%u) -> JoinTeam (clear spectator + Team) then ChangedTeams",
                     clientId, teamId);
        // --- ADVANCE TO ROLE-SELECT: ChangedTeams (handle 172) on ch2 -------------------
        // ROPlayerController.uc:3533
        //   reliable client simulated function ChangedTeams(byte TeamIndex,
        //       bool bShowRoleSelection, optional Class<GameInfo> GameTypeClass,
        //       optional bool bTeamBalancing, optional bool bShowLobby)
        //
        // The retail client's ChangedTeams() does the team->role transition itself:
        //   * line 3618: PlayerReplicationInfo.Team = WorldInfo.GRI.Teams[TeamIndex]
        //                -> it BINDS PRI.Team from GRI.Teams[] (already populated by our
        //                   TeamInfo opens on ch21/56/76), so NO PRI.Team delta is needed.
        //   * line 3627: ShowRoleSelectScene(GameTypeClass, TeamIndex, ...)
        //                -> uses the GameTypeClass PARAM directly. If it is none, the
        //                   client SKIPS InitSquadsForGametype (ROPlayerController.uc:5941),
        //                   the squad/role tables stay empty, GetSelectedRoleInfoClass()
        //                   returns none, and the role UI does RoleClass.default.X on a
        //                   null class-default object -> EXCEPTION_ACCESS_VIOLATION (the
        //                   VNGame.exe+0xbbf712 crash we saw). The fix is to SEND a real,
        //                   client-resolvable GameTypeClass so squads build locally from
        //                   the client's own loaded ROMapInfo (the map data is NOT
        //                   replicated - confirmed: ROMI = ROMapInfo(WorldInfo.GetMapInfo)).
        //
        // GameTypeClass is encoded as the SAME static PackageMap class index sent
        // as GRI.GameClass (h33). RetailBootstrap resolves that shared value from
        // the active map/mode profile so Territories, Supremacy and Skirmish do
        // not diverge between the bootstrap, GRI and this RPC.
        // See docs/re/CLIENT_CRASH_team_select.md + docs/re/open_bunch_structure.md:185.
        //
        // Param wire layout (UE3 UnScript.cpp:2980-3010, validated against UE3-src):
        //   non-bool param -> [Send presence bit][value iff Send] ; bool -> bare value bit.
        // ROLE-SELECT ADVANCE GATE (default OFF).
        // Packet-recorder ground truth (packetlog/, session 1782498358588) + crash dump
        // VNGame.exe.1152.dmp PROVED sending ChangedTeams crashes the retail client: the
        // client processes it, opens the role/unit-select UI, and the role UI dereferences a
        // NULL game-state object (VNGame.exe+0xbbf712: `mov rax,[rdx]; call [rax+0x2b8]` with
        // rdx = a cached local-player ref at [r12+0xf0] == NULL) -> AV. Adding the resolvable
        // GameTypeClass param did NOT fix it (the role UI reads GRI/role state directly, not
        // our param). Until the role-select state layer is actually replicated (identify +
        // populate the null object the role UI compares against), DO NOT send the advance -
        // it is a guaranteed client crash. The team is still recorded server-side above.
        // Re-enable by flipping this flag once role-state replication lands.
        // Re-enabled. The bind failure was an OVERSIZED-PACKET DROP, not width/timing: the
        // client dropped our ~1337-byte actor-open batch (WSAEMSGSIZE) so ch26 (local PRI) was
        // never created, and the PC->PRI link could not resolve. Fixed by shrinking the
        // actor-open batch budget (see kBatchBitBudget) so every datagram fits the client's
        // recv buffer. With ch26 delivered, the link binds ROPC.PlayerReplicationInfo, and with
        // the spectator flags cleared the role-select scene opens. (Client-log confirmed.)
        constexpr bool kEnableRoleSelectAdvance = true;
        if (kEnableRoleSelectAdvance) {
            // Clear the local PRI's spectator flags FIRST so ShowRoleSelectScene does not
            // early-return at uc:5932 (if PRI.bOnlySpectator return) - the "no crash but no
            // advance" gate. Then re-assert the PC->PRI link (handle 23 -> ch26): the
            // unit-select scene's PostInitialize caches LocalPRI = ROPC.PlayerReplicationInfo,
            // so that ref MUST be set when ChangedTeams runs the role scene.
            SendClearSpectator(clientId, 3);
            SendLocalPriLink(clientId, 3);
            constexpr uint32_t kChangedTeamsHandle           = 172;
            const uint32_t gameClassIndex = *selectedGameClass;
            BitWriter fw;
            fw.SerializeInt(kChangedTeamsHandle, kRoPcMaxHandle);      // handle (maxHandle 531)
            // param 1: byte TeamIndex  -> Send only if != default(0)
            if (teamId != 0) { fw.WriteBit(true); fw.WriteByte(teamId); }
            else             { fw.WriteBit(false); }
            // param 2: bool bShowRoleSelection = true  -> open the role-select scene
            fw.WriteBit(true);
            // param 3: Class<GameInfo> GameTypeClass = ROGameInfoTerritories  -> Send + objref
            fw.WriteBit(true);
            // Object-ref (UPackageMapLevel::SerializeObject), STATIC class path: selector bit 0
            // then SerializeInt(index, MAX_OBJECT_INDEX=0x80000000). Bit-identical to
            // ActorRepl::WriteNetGUID(NetGUIDRef{false,idx}); inlined to avoid a cross-TU link
            // dep on src/Network/ActorReplication.cpp (obj-name collision, see HARDENING_LOG).
            fw.WriteBit(false);                                            // selector = static
            fw.SerializeInt(gameClassIndex, 0x80000000u);                 // static class index
            fw.WriteBit(false);                                            // param 4: bTeamBalancing
            fw.WriteBit(false);                                            // param 5: bShowLobby
            // ORDERED SINGLE-PACKET ADVANCE: clear-spectator + PC->PRI link + ChangedTeams in
            // ONE packet, in that order. Within a packet the client processes bunches in order,
            // so bOnlySpectator=0 and the PRI link are applied BEFORE ShowRoleSelectScene runs -
            // eliminating the intermittent first-click race (localhost can reorder/drop the
            // separate unreliable bunches under the bootstrap burst). ChangedTeams is reliable
            // (retransmitted until acked); the redundant clear/link sends above cover a drop of
            // this packet (only ChangedTeams is retransmitted, but the separate sends already
            // delivered the clear/link).
            PacketCodec::Bunch ctBunch;
            ctBunch.bReliable   = true;
            ctBunch.chIndex     = 2;
            ctBunch.chType      = cs.actorChType;
            ctBunch.chSequence  = changedTeamsReservation->front();
            ctBunch.payload     = fw.GetBytes();
            ctBunch.payloadBits = static_cast<uint32_t>(fw.NumBits());
            if (!SendReservedCh2Bunches(
                    clientId,
                    {BuildClearSpectatorBunch(), BuildPriLinkBunch(), ctBunch},
                    *changedTeamsReservation,
                    "ChangedTeams role-select advance")) {
                Logger::Warn(
                    "[ConnectionManager] client %u: ChangedTeams role-select "
                    "advance was not queued",
                    clientId);
                FailCloseCh2Publication(
                    clientId, "one-shot ChangedTeams cohort");
                return;
            }
            Logger::Info("[ConnectionManager] client %u: SelectTeam(TeamID=%u) -> ordered "
                         "[clear-spectator + PRI-link + ChangedTeams] advance cohort queued "
                         "(GameTypeClass %s idx %u)", clientId, teamId,
                         bootstrapProfile.gameClassPath.c_str(), gameClassIndex);
        } else {
            const auto cancelled =
                cs.ch2Reliable.CancelBatch(*changedTeamsReservation);
            if (!cancelled) {
                Logger::Error(
                    "[OutboundReliable] client %u could not cancel disabled "
                    "ChangedTeams reservation (error=%u)",
                    clientId, static_cast<unsigned>(cancelled.error()));
                FailCloseCh2Publication(
                    clientId, "disabled ChangedTeams rollback");
                return;
            }
            Logger::Info("[ConnectionManager] client %u: SelectTeam(TeamID=%u) recorded server-side; "
                         "role-select advance HELD (ChangedTeams crashes the client against "
                         "unreplicated role-state - see packetlog + VNGame.exe+0xbbf712 crash)",
                         clientId, teamId);
        }

        // Commit the authoritative half only after the complete ordered cohort
        // is owned by the retransmission ledger. The server loop is
        // single-threaded, so no inbound role request can observe this brief
        // publication-before-commit interval.
        cs.teamSelected = true;
        cs.roleFinalized = false;
        cs.roleSelectionAccepted = false;
        cs.roleClassReplicated = false;
        cs.selectedRoleInfoObjectRef = 0;
        cs.selectedRoleClassIndex = 255;
        cs.selectedChangedRole.reset();
        cs.selectedRoleSquadIndex = 255;
        cs.selectedRoleIndex = 255;
        cs.spawned = false;
        cs.deferredOwningPawnGraphDeployment.reset();
        cs.owningPawnAlive = false;
        InvalidatePossessionRecovery(cs);
        cs.activeWeaponChannel = 0;
        cs.weaponIntent.fill({});
        cs.latestViewValid = false;
        cs.latestPackedView = 0;
        if (m_server) m_server->CancelRetailGrenadeCook(clientId);
        m_deploymentCoordinator.ResetClient(clientId);

        // Persist the team SERVER-SIDE (the real JoinTeam result). Retail 0 is
        // NVA/Axis and retail 1 is US/Allies, while TeamManager deliberately
        // uses 1=US and 2=NVA. Never convert with +1: that swaps the factions.
        if (m_server) {
            TeamManager* teamManager = m_server->GetTeamManager();
            // Team selection starts a new deployment life. Do not leave the
            // authoritative Player alive while the old faction graph drains:
            // OnPlayerSpawn would then reject the later Dead -> Alive boundary
            // and the post-commit pawn generation could not advance. This is a
            // menu transition, not a combat kill, so no score/ticket callbacks
            // run. Remove the old combat participant until deployment rebuilds
            // it with the newly selected immutable team.
            if (PlayerManager* players = m_server->GetPlayerManager()) {
                if (const std::shared_ptr<Player> player =
                        players->GetPlayer(clientId);
                    player) {
                    player->SetReadyToSpawn(false);
                    if (player->GetState() != PlayerState::Dead) {
                        if (player->GetState() == PlayerState::Alive) {
                            players->OnPlayerDeath(clientId);
                        } else {
                            player->SetHealth(0);
                        }
                    }
                }
            }
            m_server->RemoveCombatParticipant(clientId);
            if (auto* roleSystem = m_server->GetRoleSystem()) {
                roleSystem->ReleaseRetailSquadAssignment(clientId);
            }
            if (teamManager) {
                teamManager->AddPlayerToTeam(
                    clientId, TeamMapping::RetailToServer(teamId));
            }
            // The emulator's fixed per-team fill policy must reconcile at the
            // team-admission boundary, not on the later world tick, because h170
            // and h175 can arrive in one packet/poll. BotManager's synchronous
            // callbacks update the tagged RoleSystem occupancy without
            // Human(n)/Bot(n) aliasing. Retail source establishes that bots
            // consume normal squad slots, but does not establish this emulator-
            // specific fill-eviction timing.
            if (BotManager* bots = m_server->GetBotManager();
                bots && teamManager) {
                bots->SetHumanTeamCounts(
                    teamManager->GetTeamPlayers(BotManager::kTeamOne).size(),
                    teamManager->GetTeamPlayers(BotManager::kTeamTwo).size());
            }
            // Releasing an old-team slot can promote another member. Publish
            // each repaired human assignment before advancing its local cache.
            SynchronizeRetailSquadAssignments();
        }

        // Team is required to bind a remote PRI to an already-open TeamInfo.
        // Queue remote actors only after the load-bearing ChangedTeams advance;
        // team selection is also proof that the fixed bootstrap actors resolved.
        SynchronizeAllRemoteParticipantPris();
    }

    // SelectRoleByClass (handle 175) first commits the client's role class and,
    // when bCloseMenu is true, advances to the map's spawn-selection scene. The
    // capture sends compound tails after h175, so the complete payload must be
    // validated transactionally rather than treating its final bit as the flag.
    if (handle == 175) {
        ControlState& cs = GetControlState(clientId);
        if (bunch.chIndex != 2u) {
            Logger::Warn(
                "[RoleSelection] client %u rejected h175 on non-owning ch%u",
                clientId, bunch.chIndex);
            return;
        }

        const RoleSelectionRepl::DecodeResult decoded =
            RoleSelectionRepl::DecodeRoleSelectionBunch(
                bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (!decoded.valid()) {
            Logger::Warn(
                "[RoleSelection] client %u rejected malformed/unsupported h175 "
                "(decode error %u)",
                clientId, static_cast<unsigned>(decoded.error));
            return;
        }

        if (!cs.teamSelected || !m_server) {
            Logger::Warn(
                "[RoleSelection] client %u rejected h175 before a server-owned "
                "team/session was available",
                clientId);
            return;
        }

        TeamManager* teams = m_server->GetTeamManager();
        RoleSystem* roles = m_server->GetRoleSystem();
        if (!teams || !roles) {
            Logger::Warn(
                "[RoleSelection] client %u rejected h175 because team/role "
                "authority is unavailable",
                clientId);
            return;
        }

        // Evaluate the live-player guard without lazily freezing any session
        // metadata. Runtime-squad h175 paths currently apply the role
        // immediately; accepting one from a pawn already in play would mutate
        // authority while deliberately skipping the pre-spawn h210 publication.
        // Keep both exact profiles read-only until next-life role transitions
        // are independently grounded.
        const RetailBootstrap::Profile liveGuardProfile =
            cs.retailBootstrapProfile.has_value()
                ? *cs.retailBootstrapProfile
                : ResolveRetailBootstrapProfile(m_server);
        const bool compoundSession =
            liveGuardProfile.mapUrl == "VNSK-Compound" &&
            liveGuardProfile.modeName == "Skirmish";
        const bool cuChiSession =
            liveGuardProfile.mapUrl == "VNTE-CuChi" &&
            liveGuardProfile.modeName == "Territories";
        if (compoundSession || cuChiSession) {
            bool alreadyLive = cs.spawned;
            if (PlayerManager* players = m_server->GetPlayerManager()) {
                if (const std::shared_ptr<Player> player =
                        players->GetPlayer(clientId)) {
                    alreadyLive = alreadyLive || player->IsAlive();
                }
            }
            if (alreadyLive) {
                Logger::Warn(
                    "[RoleSelection] client %u rejected %s h175 while already "
                    "spawned/live; role, squad and PRI state unchanged",
                    clientId, compoundSession ? "Compound" : "Cu Chi");
                return;
            }
        }

        const RetailBootstrap::Profile& profile =
            GetRetailBootstrapProfile(clientId);
        if (profile.usedFallback) {
            Logger::Warn(
                "[RoleSelection] client %u rejected h175 on a fallback map "
                "profile; exact role metadata is required",
                clientId);
            return;
        }
        const std::optional<RetailBootstrap::ArtifactSelection>&
            selectedArtifact = GetRetailArtifactSelection(clientId);
        if (!selectedArtifact) {
            Logger::Warn(
                "[RoleSelection] client %u rejected h175 because no frozen "
                "PackageMap artifact is available",
                clientId);
            return;
        }

        const bool isCompoundRoleProfile =
            profile.mapUrl == "VNSK-Compound" && profile.modeName == "Skirmish";
        const bool isCuChiRoleProfile =
            profile.mapUrl == "VNTE-CuChi" &&
            profile.modeName == "Territories";
        // The installed artifact's legacy Resort/Cu Chi registry remains
        // intentionally fail-closed. Compound is a narrower exception: all ten
        // h175 UClass refs were re-extracted from the currently installed
        // ROGame.u and are gated below by exact variant, ObjectBase, map, mode,
        // team, class and weapon-selection evidence.
        if (!isCompoundRoleProfile &&
            !selectedArtifact->roGame.roleRegistryGrounded) {
            Logger::Warn(
                "[RoleSelection] client %u rejected h175 for variant='%.*s': "
                "the installed ROGame role CDO registry is not fully migrated; "
                "legacy role grounding remains fail-closed",
                clientId,
                static_cast<int>(selectedArtifact->variant.size()),
                selectedArtifact->variant.data());
            return;
        }
        const uint32_t serverTeam = teams->GetPlayerTeam(clientId);
        const RoleSelectionRepl::GroundingResult grounded =
            isCompoundRoleProfile
                ? RoleSelectionRepl::ResolveGroundedCompoundRole(
                      decoded.rpc, profile.mapUrl, profile.modeName,
                      selectedArtifact->variant,
                      selectedArtifact->roGame.actualObjectBase, serverTeam)
                : isCuChiRoleProfile
                      ? RoleSelectionRepl::ResolveGroundedCuChiInfantry(
                            decoded.rpc, profile.mapUrl, profile.modeName,
                            profile.roGameObjectBase, serverTeam)
                      : RoleSelectionRepl::ResolveGroundedResortInfantry(
                            decoded.rpc, profile.mapUrl, serverTeam);
        if (!grounded.valid()) {
            Logger::Warn(
                "[RoleSelection] client %u rejected unsupported map/team/loadout "
                "selection (grounding error %u, team=%u, map=%s)",
                clientId, static_cast<unsigned>(grounded.error), serverTeam,
                profile.mapUrl.c_str());
            return;
        }

        if (isCompoundRoleProfile || isCuChiRoleProfile) {
            const char* roleProfileName =
                isCompoundRoleProfile ? "Compound" : "Cu Chi";
            const bool finalAutoSelect =
                decoded.following ==
                    RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad ||
                decoded.following == RoleSelectionRepl::FollowingRpcPattern::
                                         FinalAutoSelectSquadAndDefaultSpectatorLocation;
            if (decoded.rpc.closeMenu != finalAutoSelect) {
                Logger::Warn(
                    "[RoleSelection] client %u rejected %s h175: final "
                    "close and ServerAutoSelectSquad must arrive together",
                    clientId, roleProfileName);
                return;
            }

            const Faction expectedSouthFaction =
                isCompoundRoleProfile ? Faction::USMC : Faction::USArmy;
            const Faction southFaction = roles->GetTeamFaction(
                RoleSelectionRepl::kCompoundUsServerTeam);
            const Faction northFaction = roles->GetTeamFaction(
                RoleSelectionRepl::kCompoundNlfServerTeam);
            if (southFaction != expectedSouthFaction ||
                northFaction != Faction::NLFSV) {
                Logger::Warn(
                    "[RoleSelection] client %u rejected %s h175: map role "
                    "authority has factions team1=%u/team2=%u instead of "
                    "%u/NLFSV",
                    clientId, roleProfileName,
                    static_cast<unsigned>(southFaction),
                    static_cast<unsigned>(northFaction),
                    static_cast<unsigned>(expectedSouthFaction));
                return;
            }

            const Faction configuredFaction =
                roles->GetTeamFaction(serverTeam);
            const std::optional<CombatRole> combatRole =
                isCompoundRoleProfile
                    ? ResolveCompoundCombatRole(configuredFaction,
                                                grounded.role.classIndex)
                    : (grounded.role.classIndex ==
                               RoleSelectionRepl::kCuChiInfantryClassIndex
                           ? std::optional<CombatRole>{CombatRole::Rifleman}
                           : std::nullopt);
            if (!combatRole) {
                Logger::Warn(
                    "[RoleSelection] client %u rejected %s class %u: no "
                    "authoritative combat-role mapping",
                    clientId, roleProfileName,
                    static_cast<unsigned>(grounded.role.classIndex));
                return;
            }

            const bool sameClass =
                cs.roleSelectionAccepted &&
                cs.selectedRoleClassIndex == grounded.role.classIndex;
            if (isCompoundRoleProfile && !sameClass &&
                grounded.role.roleLimit !=
                    RoleSelectionRepl::kCompoundRiflemanLimit) {
                size_t occupiedClassSlots = 0;
                for (const auto& [otherClientId, otherState] : m_controlState) {
                    if (otherClientId == clientId ||
                        !otherState.roleSelectionAccepted ||
                        otherState.selectedRoleClassIndex !=
                            grounded.role.classIndex ||
                        teams->GetPlayerTeam(otherClientId) != serverTeam) {
                        continue;
                    }
                    ++occupiedClassSlots;
                }
                if (occupiedClassSlots >= grounded.role.roleLimit) {
                    Logger::Warn(
                        "[RoleSelection] client %u denied %s class %u: "
                        "human limit %u reached on team %u",
                        clientId, roleProfileName,
                        static_cast<unsigned>(grounded.role.classIndex),
                        static_cast<unsigned>(grounded.role.roleLimit),
                        serverTeam);
                    return;
                }
            }

            std::optional<RetailSquadAssignment> currentAssignment;
            RetailSquadAssignment assignment;
            bool allocatedForRequest = false;

            // Secure the final squad slot before changing the combat role or
            // publishing h79. AutoAssignRetailSquad owns the exact active,
            // unlocked, non-full preflight transaction; network dispatch is
            // single-threaded, so it cannot race another role request.
            if (finalAutoSelect) {
                currentAssignment = roles->GetRetailSquadAssignment(clientId);
                if (currentAssignment &&
                    currentAssignment->teamId != serverTeam) {
                    Logger::Error(
                        "[RoleSelection] client %u has stale retail squad "
                        "authority on team %u while selecting team %u; final "
                        "role held without mutation",
                        clientId, currentAssignment->teamId, serverTeam);
                    return;
                }
                assignment =
                    roles->AutoAssignRetailSquad(clientId, serverTeam);
                if (!assignment.IsValid()) {
                    Logger::Warn(
                        "[RoleSelection] client %u denied final %s role: no "
                        "active, unlocked retail squad slot on team %u; role "
                        "and h79 held",
                        clientId, roleProfileName, serverTeam);
                    return;
                }
                allocatedForRequest = !currentAssignment.has_value();
            }

            if (!roles->AssignRole(clientId, *combatRole)) {
                if (allocatedForRequest &&
                    roles->ReleaseRetailSquadAssignment(clientId)) {
                    SynchronizeRetailSquadAssignments();
                }
                Logger::Warn(
                    "[RoleSelection] client %u grounded %s class %u but "
                    "RoleSystem denied %s",
                    clientId, roleProfileName,
                    static_cast<unsigned>(grounded.role.classIndex),
                    roles->GetRoleName(*combatRole).c_str());
                return;
            }

            const uint8_t previousClass = cs.selectedRoleClassIndex;
            cs.roleSelectionAccepted = true;
            cs.selectedRoleInfoObjectRef = grounded.role.roleInfoObjectRef;
            cs.selectedRoleClassIndex = grounded.role.classIndex;
            cs.selectedChangedRole.reset();

            if (!cs.roleClassReplicated || previousClass != grounded.role.classIndex) {
                SendOwnerPriClassIndex(clientId, grounded.role.classIndex);
                cs.roleClassReplicated = true;
            }

            if (!finalAutoSelect) {
                // An interim class choice supersedes any pre-spawn deployment
                // authorization from an earlier menu visit. The existing
                // retail squad slot is intentionally retained: native
                // ServerAutoSelectSquad is idempotent once SquadIndex != 255.
                if (!cs.spawned) {
                    cs.roleFinalized = false;
                    m_deploymentCoordinator.ResetClient(clientId);
                }
                Logger::Info(
                    "[RoleSelection] client %u accepted interim %s class "
                    "%u object %u for team %u; owner PRI h79 published",
                    clientId, roleProfileName,
                    static_cast<unsigned>(grounded.role.classIndex),
                    grounded.role.roleInfoObjectRef, serverTeam);
                return;
            }

            RoleSelectionRepl::ChangedRoleEvidence transition;
            // ROPlayerReplicationInfo.ServerAutoSelectSquad only publishes
            // ChangedSquad when it allocates a previously unassigned player.
            // A later final-role request keeps the existing squad and passes
            // that index directly to ChangedRole instead (retail
            // ROPlayerReplicationInfo.uc 740-741,987-1057 and
            // ROPlayerController.uc 27164-27167).
            transition.squadIndex = allocatedForRequest
                ? static_cast<uint8_t>(255u)
                : assignment.squadIndex;
            transition.classIndex = grounded.role.classIndex;
            transition.showLobby = false;
            transition.showSpawnSelect = true;
            if (allocatedForRequest) {
                transition.followingChangedSquad =
                    RoleSelectionRepl::ChangedSquadEvidence{
                        assignment.squadIndex, assignment.roleIndex};
            }
            cs.selectedChangedRole = transition;
            cs.selectedRoleSquadIndex = assignment.squadIndex;
            cs.selectedRoleIndex = assignment.roleIndex;

            if (auto* pm = m_server->GetPlayerManager()) {
                if (auto player = pm->GetPlayer(clientId)) {
                    player->SetReadyToSpawn(false);
                }
            }
            if (!cs.spawned) {
                cs.roleFinalized = true;
                // Every accepted final role for an unspawned client starts a
                // fresh spawn-selection transaction. Squad/role ownership
                // lives in RoleSystem and survives this reset; stale slot,
                // Ready, and authorization do not.
                m_deploymentCoordinator.ResetClient(clientId);
                m_deploymentCoordinator.FinalizeRole(clientId);
                if (!SendChangedRoleSpawnSelect(
                        clientId,
                        /*includeOwnerPriAssignment=*/false)) {
                    FailCloseCh2Publication(
                        clientId, "runtime-squad ChangedRole transition");
                    return;
                }
                SendOwnerPriRoleAssignment(
                    clientId, assignment.squadIndex, assignment.roleIndex);
            }
            Logger::Info(
                "[RoleSelection] client %u finalized %s class %u -> "
                "squad %u slot %u and spawn selection",
                clientId, roleProfileName,
                static_cast<unsigned>(grounded.role.classIndex),
                static_cast<unsigned>(assignment.squadIndex),
                static_cast<unsigned>(assignment.roleIndex));
            return;
        }

        // RoleSystem mutation happens only after the whole bunch, compound tail,
        // map, team, class and weapon selection have been validated.
        if (!roles->AssignRole(clientId, CombatRole::Rifleman)) {
            Logger::Warn(
                "[RoleSelection] client %u grounded Resort infantry was denied "
                "by authoritative role availability",
                clientId);
            return;
        }

        cs.roleSelectionAccepted = true;
        cs.selectedRoleInfoObjectRef = grounded.role.roleInfoObjectRef;
        cs.selectedRoleClassIndex = grounded.role.classIndex;
        cs.selectedChangedRole = grounded.role.changedRole;
        cs.selectedRoleSquadIndex = grounded.role.squadIndex;
        cs.selectedRoleIndex = grounded.role.roleIndex;

        // Retail f2350 publishes h79 before the final ChangedRole packet. This
        // also makes a direct-final client safe without requiring an interim RPC.
        if (!cs.roleClassReplicated) {
            SendOwnerPriClassIndex(clientId, cs.selectedRoleClassIndex);
            cs.roleClassReplicated = true;
        }

        if (!decoded.rpc.closeMenu) {
            Logger::Info(
                "[RoleSelection] client %u accepted interim Resort infantry "
                "role object %u for server team %u; owner PRI h79 published",
                clientId, cs.selectedRoleInfoObjectRef, serverTeam);
            return;
        }

        if (auto* pm = m_server->GetPlayerManager()) {
            if (auto pl = pm->GetPlayer(clientId)) {
                pl->SetReadyToSpawn(false);
            }
        }
        if (!cs.spawned) {
            cs.roleFinalized = true;
            // Resort reaches the same fresh unspawned role-selection boundary:
            // preserve its accepted role ledger while discarding the prior
            // deployment transaction.
            m_deploymentCoordinator.ResetClient(clientId);
            m_deploymentCoordinator.FinalizeRole(clientId);
            Logger::Info(
                "[RoleSelection] client %u final Resort infantry for server "
                "team %u -> captured controller transition + spawn selection",
                clientId, serverTeam);
            // South f2537 carries owner PRI h81/h80 after h210 in the same
            // packet. North f61989 instead carries h211 after h210; h211 itself
            // updates the local PRI, and the property confirmation is later
            // (f62024), so do not manufacture a same-packet North PRI delta.
            const bool includeOwnerPriAssignment =
                grounded.role.changedRole.has_value() &&
                !grounded.role.changedRole->followingChangedSquad.has_value();
            if (!SendChangedRoleSpawnSelect(
                    clientId, includeOwnerPriAssignment)) {
                FailCloseCh2Publication(
                    clientId, "Resort ChangedRole transition");
                return;
            }
            if (grounded.role.changedRole.has_value() &&
                grounded.role.changedRole->followingChangedSquad.has_value()) {
                const RoleSelectionRepl::ChangedSquadEvidence& squad =
                    *grounded.role.changedRole->followingChangedSquad;
                // North f61989 keeps h210+h211 together in one reliable ch2
                // bunch. Its owner-PRI h81/h80 confirmation appears later in
                // f62024 as an unreliable property delta. No client action
                // occurs between those frames, so preserve the proven packet
                // boundary with a distinct send without inventing a causal
                // half-second timer.
                SendOwnerPriRoleAssignment(clientId, squad.squadIndex,
                                           squad.roleIndex);
            }
        }
        return;
    }

    const auto applySpawnSelection = [&](uint8_t encodedSelection) {
        const std::vector<uint32_t> available = GetAdvertisedSpawnIds(clientId);
        const auto result = m_deploymentCoordinator.SelectSpawn(
            clientId, encodedSelection, available);
        if (result == DeploymentCoordinator::SelectionResult::Accepted) {
            SendPriSpawnSelection(clientId, encodedSelection);
            Logger::Info(
                "[Deployment] client %u selected normal slot %u (encoded %u)",
                clientId,
                static_cast<unsigned>(encodedSelection -
                    DeploymentCoordinator::kNormalSpawnSelectionBase),
                static_cast<unsigned>(encodedSelection));
        } else if (result ==
                   DeploymentCoordinator::SelectionResult::AlreadyAuthorized) {
            // Retail repeats ServerSetSpawnSelect around its ready-to-spawn
            // sequence. Once that sequence authorizes deployment the repeat is
            // idempotent: preserve authorization and do not publish a duplicate
            // PRI confirmation.
            Logger::Info(
                "[Deployment] client %u repeated authorized spawn selection %u; "
                "duplicate ignored and authorization preserved",
                clientId, static_cast<unsigned>(encodedSelection));
        } else {
            Logger::Warn(
                "[Deployment] client %u spawn selection %u rejected (reason %u, "
                "%zu available)",
                clientId, static_cast<unsigned>(encodedSelection),
                static_cast<unsigned>(result), available.size());
        }
        return result;
    };

    const auto applyReadyStatus =
        [&](uint8_t rawStatus, uint8_t index, uint8_t count,
            std::optional<uint32_t>& newlyAuthorizedSpawn) {
            const auto status =
                static_cast<DeploymentCoordinator::ReadyStatus>(rawStatus);
            // Revalidate in the same coalesced ROVolumePlayerStartGroup domain
            // that was shown to the client. The raw PlayerStart list can
            // contain multiple rows for one retail button and is therefore not
            // slot-comparable to the frozen h59 advertisement.
            const std::vector<uint32_t> available =
                GetCurrentAdvertisedSpawnIds(clientId);
            const auto decision = m_deploymentCoordinator.SetReadyStatus(
                clientId, status, available);
            Logger::Info(
                "[Deployment] client %u ready status %u (%u/%u) -> decision %u",
                clientId, static_cast<unsigned>(rawStatus),
                static_cast<unsigned>(index + 1u),
                static_cast<unsigned>(count),
                static_cast<unsigned>(decision.result));

            // A Territory objective can advance between the player's h261
            // click and Ready. Re-open the scene with a freshly frozen list;
            // never reinterpret the old slot against a new deployment domain.
            if (decision.result == DeploymentCoordinator::ReadyResult::
                                       SelectionNoLongerAvailable) {
                m_deploymentCoordinator.ResetClient(clientId);
                m_deploymentCoordinator.FinalizeRole(clientId);
                if (!SendChangedRoleSpawnSelect(clientId)) {
                    FailCloseCh2Publication(
                        clientId, "stale-spawn ChangedRole recovery");
                    return false;
                }
                Logger::Info(
                    "[Deployment] client %u spawn list changed before Ready; "
                    "reopened selection with the current phase list",
                    clientId);
                return false;
            }

            if (decision.IsNewAuthorization()) {
                newlyAuthorizedSpawn = decision.spawnId;
            } else if (status != DeploymentCoordinator::ReadyStatus::Ready) {
                // A later ForceOnly/NotReady in the same validated bunch wins
                // and revokes any transient Ready authorization.
                newlyAuthorizedSpawn.reset();
            }
            return true;
        };

    const auto executeNewAuthorization =
        [&](const std::optional<uint32_t>& newlyAuthorizedSpawn) {
            // Do not spawn from a transient Ready earlier in a compound
            // sequence. Only a still-authorized final state may execute after
            // the complete bunch has been decoded and applied in wire order.
            if (!newlyAuthorizedSpawn.has_value() ||
                !m_deploymentCoordinator.IsPreparedForDeployment(clientId)) {
                return;
            }
            const DeploymentPhaseState phase = GetDeploymentPhaseState();
            if (IsDeploymentWindowOpen(phase)) {
                ExecutePreparedDeployment(clientId, *newlyAuthorizedSpawn);
            }
        };

    // Compound's Skirmish spawn scene prefixes its automatic deployment calls
    // with ServerSetSpawnVolumeViewTarget(CameraActor). Validate either exact
    // capture-grounded h370-led bunch in full before applying its ordered
    // h261/h434 actions; h370 itself is camera bookkeeping only.
    if (handle == DeploymentRepl::kServerSetSpawnVolumeViewTargetHandle) {
        const auto decoded = DeploymentRepl::DecodeSpawnVolumeViewTargetBunch(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (!decoded.valid()) {
            Logger::Warn(
                "[Deployment] client %u rejected malformed h370 deployment "
                "bunch (decode error %u)",
                clientId, static_cast<unsigned>(decoded.error));
            return;
        }

        std::optional<uint32_t> newlyAuthorizedSpawn;
        for (uint8_t index = 0; index < decoded.bunch.actionCount; ++index) {
            const DeploymentRepl::SpawnVolumeDeploymentAction& action =
                decoded.bunch.actions[index];
            if (action.type == DeploymentRepl::
                                   SpawnVolumeDeploymentActionType::SpawnSelect) {
                const auto selection = applySpawnSelection(
                    action.spawnSelect.encodedSelection);
                if (selection !=
                        DeploymentCoordinator::SelectionResult::Accepted &&
                    selection != DeploymentCoordinator::SelectionResult::
                                     AlreadyAuthorized) {
                    Logger::Warn(
                        "[Deployment] client %u aborted validated h370 pattern "
                        "%u after rejected spawn action %u",
                        clientId, static_cast<unsigned>(decoded.bunch.pattern),
                        static_cast<unsigned>(index));
                    return;
                }
                continue;
            }
            if (!applyReadyStatus(action.readyToSpawn.status, index,
                                  decoded.bunch.actionCount,
                                  newlyAuthorizedSpawn)) {
                return;
            }
        }
        executeNewAuthorization(newlyAuthorizedSpawn);
        Logger::Info(
            "[Deployment] client %u accepted h370 camera %u pattern %u with "
            "%u ordered deployment action(s)",
            clientId, decoded.bunch.cameraTargetRef,
            static_cast<unsigned>(decoded.bunch.pattern),
            static_cast<unsigned>(decoded.bunch.actionCount));
        return;
    }

    // ServerSetSpawnSelect(byte NewSpawnSelect). Normal TeamInfo array slots
    // arrive as 128+slot; encoded 237..254 are special commander/vehicle/
    // helicopter/tunnel/squad choices and remain fail-closed until those
    // actor-backed paths exist.
    if (handle == DeploymentCoordinator::kServerSetSpawnSelectHandle) {
        const auto decoded = DeploymentRepl::DecodeServerSetSpawnSelect(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (!decoded.valid()) {
            Logger::Warn("[Deployment] client %u rejected malformed h261 "
                         "(decode error %u)", clientId,
                         static_cast<unsigned>(decoded.error));
            return;
        }
        const uint8_t encodedSelection = decoded.rpc.encodedSelection;
        (void)applySpawnSelection(encodedSelection);
        return;
    }

    // ServerSetReadyToSpawn(ESpawnReadyStatus). Retail frequently coalesces
    // exact companion RPCs in the same reliable bunch, including a transient
    // Ready followed by ForceOnly as the spawn scene opens. Decode the whole
    // allowlisted sequence before applying any readiness transition.
    if (handle == DeploymentCoordinator::kServerSetReadyToSpawnHandle) {
        const auto decoded = DeploymentRepl::DecodeServerSetReadyToSpawnBunch(
            bunch.payload.data(), bunch.payload.size(), bunch.payloadBits);
        if (!decoded.valid()) {
            Logger::Warn("[Deployment] client %u rejected malformed h434 "
                         "(decode error %u)", clientId,
                         static_cast<unsigned>(decoded.error));
            return;
        }

        std::optional<uint32_t> newlyAuthorizedSpawn;
        for (uint8_t index = 0;
             index < decoded.bunch.transitionCount; ++index) {
            const uint8_t rawStatus =
                decoded.bunch.transitions[index].status;
            if (!applyReadyStatus(rawStatus, index,
                                  decoded.bunch.transitionCount,
                                  newlyAuthorizedSpawn)) {
                return;
            }
        }

        if (decoded.bunch.acknowledgedPawnChannel.has_value()) {
            ControlState& cs = GetControlState(clientId);
            const uint64_t pawnGeneration = cs.owningPawnGeneration;
            const bool acknowledgedCurrentGeneration =
                bunch.bReliable &&
                *decoded.bunch.acknowledgedPawnChannel == kLocalPawnChannel &&
                HasLiveOwningPawnGeneration(cs, pawnGeneration) && cs.spawned;
            if (acknowledgedCurrentGeneration) {
                cs.possessionAckedGeneration = pawnGeneration;
                ResetPossessionRecovery(cs);
            }
            Logger::Info(
                "[Deployment] client %u compound possession ack ch%u -> %s",
                clientId, *decoded.bunch.acknowledgedPawnChannel,
                acknowledgedCurrentGeneration ? "POSSESSED"
                                               : "not our live generation");
        }

        executeNewAuthorization(newlyAuthorizedSpawn);
        return;
    }
}

void ConnectionManager::SendReplicationBootstrap(uint32_t clientId) {
    const RetailBootstrap::Profile& profile = GetRetailBootstrapProfile(clientId);
    if (profile.usedFallback) {
        Logger::Error(
            "[ConnectionManager::SendReplicationBootstrap] current map has no "
            "bounded retail profile; refusing the canonical Resort fallback to "
            "avoid loading a client into a different world");
        return;
    }
    const std::optional<RetailBootstrap::ArtifactSelection>& selection =
        GetRetailArtifactSelection(clientId);
    if (!selection) {
        return;
    }
    const std::vector<std::vector<uint8_t>>& records =
        GetReplicationBootstrapRecords(profile, *selection);
    if (records.empty()) {
        return;
    }
    Logger::Info("[ConnectionManager::SendReplicationBootstrap] client %u: sending %zu replication "
                 "bootstrap messages for %s / %s",
                 clientId, records.size(), profile.mapUrl.c_str(),
                 profile.gameClassPath.c_str());
    size_t sent = 0;
    for (const std::vector<uint8_t>& msg : records) {
        if (SendRawToClient(clientId, msg)) {
            ++sent;
        }
    }
    Logger::Info("[ConnectionManager::SendReplicationBootstrap] client %u: sent %zu/%zu messages",
                 clientId, sent, records.size());
}

bool ConnectionManager::ParseIncomingControl(uint32_t clientId, const std::vector<uint8_t>& datagram) {
    Logger::Trace("[ConnectionManager::ParseIncomingControl] Entry: client=%u, %zu bytes",
                  clientId, datagram.size());
    if (datagram.empty()) {
        Logger::Trace("[ConnectionManager::ParseIncomingControl] empty datagram, ignoring");
        return false;
    }

    // Decode the UE3 packet framing (PacketId, acks, bunches). MaxPacket sets the
    // BunchDataBits SerializeInt bound and must match the peer's direction-specific
    // connection value. See docs/RS2V_ControlChannel_WireSpec_7258.md.
    // Retail C2S frames BunchDataBits with MaxPacket=1280 (bound 10240) from the
    // first packet onward. Small handshake messages are ambiguous across several
    // 14-bit bounds; the saturated post-login NMT_Have batches pin 1280 exactly.
    // Using 2048 here desynchronizes those batches into phantom actor channels and
    // reliable ch0 gaps, so the later real NMT_Join can never be reassembled.
    const uint32_t maxPacketBytes = PacketCodec::kClientSendMaxPacketBytes;
    PacketCodec::Packet pkt =
        PacketCodec::Decode(datagram.data(), datagram.size(), maxPacketBytes);
    if (!pkt.ok) {
        Logger::Debug("[ConnectionManager::ParseIncomingControl] client %u: %zu bytes are not a decodable UE3 packet, ignoring",
                      clientId, datagram.size());
        return false;
    }

    // A close on ch0 ends the entire UE3 connection, unlike actor-channel
    // closes.  Classify it before UE3 marking, ACK queuing, reliable-ack
    // processing, or reassembly so teardown can never emit more bytes or treat a
    // close payload as an NMT message.  Malformed ch0 closes consume the packet
    // but leave the existing session intact; malformed dominates mixed packets.
    const PacketCodec::PeerCloseClassification close =
        PacketCodec::ClassifyPeerClose(pkt);
    if (close == PacketCodec::PeerCloseClassification::MalformedControlClose) {
        Logger::Warn(
            "[ConnectionManager::ParseIncomingControl] client %u: dropped malformed "
            "control-channel close packet %u without ACK or dispatch",
            clientId, pkt.packetId);
        return true;
    }
    if (close == PacketCodec::PeerCloseClassification::GracefulControlClose) {
        auto closing = std::find_if(
            m_clients.begin(), m_clients.end(),
            [clientId](const auto& entry) {
                return entry.second && entry.second->GetClientId() == clientId;
            });
        if (closing == m_clients.end()) {
            Logger::Debug(
                "[ConnectionManager::ParseIncomingControl] duplicate control-channel "
                "close for retired client %u ignored",
                clientId);
            return true;
        }

        const ClientAddress closingAddress = closing->first;
        RemoveClientSession(closingAddress, "peer closed UE3 control channel");
        return true;
    }

    // PacketCodec::Decode is intentionally LENIENT: it sets ok once it can read a
    // PacketId, so a legacy Packet::Serialize() datagram (e.g. a non-empty
    // CHAT_MESSAGE whose final payload byte is non-zero) can decode as ok with no
    // actual UE3 content. Claiming such a datagram here - marking the peer UE3 and
    // consuming it before the legacy Packet path runs - breaks the legacy/tools
    // path the surrounding code still supports. Only treat this as UE3 when there
    // is real UE3 evidence: at least one bunch or ack, OR the peer was already
    // established as UE3 by an earlier genuine packet. Otherwise fall through and
    // let the legacy Packet pipeline handle it.
    const bool hasUE3Content = !pkt.bunches.empty() || !pkt.acks.empty();
    auto conn = GetConnection(clientId);
    const bool alreadyUE3 = conn && conn->IsUE3Client();
    if (!hasUE3Content && !alreadyUE3) {
        Logger::Debug("[ConnectionManager::ParseIncomingControl] client %u: decodable but no UE3 bunch/ack and peer "
                      "not yet UE3; deferring to legacy Packet path", clientId);
        return false;
    }

    // This peer speaks UE3: mark it so the legacy emulator-Packet send path is
    // suppressed for it (a UE3 client mis-parses that format - see SendPacket).
    if (conn) {
        conn->SetUE3Client(true);
    }

    ControlState& cs = GetControlState(clientId);
    cs.inboundPacketDispatchActive = true;

    // Keep a bounded modular PacketId floor for the current fixed-channel
    // incarnation. The close-ACK packet establishes the initial floor. As
    // newer traffic advances, retain a normal reordering window while making
    // progress across PacketId wrap without ever admitting packets from the
    // prior incarnation.
    constexpr uint32_t kOwningGraphInboundPacketReorderWindow = 64u;
    constexpr uint32_t kPacketIdModulus =
        static_cast<uint32_t>(kMaxPacketId);
    const auto packetForwardDistance = [](uint32_t from, uint32_t to) {
        constexpr uint32_t modulus = static_cast<uint32_t>(kMaxPacketId);
        return (to + modulus - from) % modulus;
    };
    if (cs.owningPawnGraphInboundPacketFloorValid) {
        const uint32_t distance = packetForwardDistance(
            cs.owningPawnGraphInboundPacketFloor, pkt.packetId);
        if (distance > kOwningGraphInboundPacketReorderWindow &&
            distance < kPacketIdModulus / 2u) {
            cs.owningPawnGraphInboundPacketFloor =
                (pkt.packetId + kPacketIdModulus -
                 kOwningGraphInboundPacketReorderWindow) %
                kPacketIdModulus;
        }
    }

    // Acknowledge this received packet ONLY if it carried bunch data. Acking a
    // pure-ack packet would make the peer ack our ack, and us ack that, forever
    // (an infinite ack ping-pong with no data - observed against the live client).
    // UE3 only acks packets that delivered bunches. The ack rides on the next
    // outbound packet (e.g. the handshake response), or a standalone ack below.
    if (!pkt.bunches.empty()) {
        cs.outbound.QueueAck(pkt.packetId);
    }

    // pkt.acks confirm OUR reliable bunches arrived: clear any pending reliable
    // bunch-set that rode in an acked packet so RetransmitTick stops resending it.
    for (uint32_t ackedId : pkt.acks) {
        OnClientAck(clientId, ackedId);
    }

    // Feed control-channel bunches to the reassembler. Complete messages are
    // dispatched to the handshake, which may emit responses via SendRawToClient
    // (draining the queued ack onto the response packet).
    // Per-packet dispatch backstop. The bunch count is already bounded by the
    // datagram size (a single inbound datagram is <= the receive buffer), but cap
    // the dispatch loop explicitly so a pathological packet can't drive an outsized
    // amount of work. The cap is far above any decodable datagram's real bunch count,
    // so valid handshake/bootstrap/actor traffic is never truncated.
    constexpr size_t kMaxBunchesPerPacket = 4096;
    size_t processed = 0;
    for (const PacketCodec::Bunch& b : pkt.bunches) {
        if (++processed > kMaxBunchesPerPacket) {
            Logger::Warn("[ConnectionManager::ParseIncomingControl] client %u: packet %u carried %zu bunches (> cap %zu), dropping remainder",
                         clientId, pkt.packetId, pkt.bunches.size(), kMaxBunchesPerPacket);
            break;
        }
        const bool fixedGraphChannel =
            OwningPawnGraphChannelIndex(b.chIndex).has_value();
        const uint32_t graphPacketDistance =
            cs.owningPawnGraphInboundPacketFloorValid
            ? packetForwardDistance(
                  cs.owningPawnGraphInboundPacketFloor, pkt.packetId)
            : 1u;
        const bool fixedGraphSemanticsRetiredInThisPacket =
            fixedGraphChannel && cs.owningPawnGraphCompletionDeferred;
        const bool fixedGraphPacketAtOrBeforeFloor =
            fixedGraphChannel &&
            cs.owningPawnGraphInboundPacketFloorValid &&
            (graphPacketDistance == 0u ||
             graphPacketDistance >= kPacketIdModulus / 2u);
        if (fixedGraphSemanticsRetiredInThisPacket ||
            fixedGraphPacketAtOrBeforeFloor) {
            if (b.bReliable) {
                DispatchInboundActorBunch(
                    clientId, b,
                    /*suppressOwningGraphSemantics=*/true,
                    /*suppressReleasedCohort=*/
                        fixedGraphSemanticsRetiredInThisPacket);
            } else {
                Logger::Info(
                    "[OwningPawnGraph] client %u dropped retired-incarnation "
                    "unreliable ch%u bunch from packet %u (floor %u, "
                    "close ACK in packet=%u)",
                    clientId, b.chIndex, pkt.packetId,
                    cs.owningPawnGraphInboundPacketFloor,
                    fixedGraphSemanticsRetiredInThisPacket ? 1u : 0u);
            }
            if (conn && conn->IsDisconnected()) break;
            continue;
        }
        if (b.chIndex == 0) {
            cs.reassembler->OnBunch(b);          // control channel (handshake/NMT)
        } else if (cs.mapTravelPending) {
            // Keep packet ACK and ch0 processing alive so ClientTravel and every
            // earlier reliable can drain. Actor RPCs still describe the old
            // world, however, and must not mutate the freshly loaded server map.
            // ResetEstablishedSessionForFreshHandshake runs before this decode;
            // a genuine reconnect therefore owns a new ControlState with this
            // flag clear and is not suppressed here.
            Logger::Trace(
                "[ClientTravel] client %u sent old-world ch%u traffic while "
                "travel is pending; acknowledged packet but ignored bunch",
                clientId, b.chIndex);
        } else if (b.chIndex == 1) {
            // Channel 1 is reserved/non-actor in UE3. Preserve the previous
            // direct dispatch behavior, but never give it actor sequencing state.
            DecodeInboundActorBunch(clientId, b);
        } else {
            DispatchInboundActorBunch(clientId, b); // ch>=2 actor-channel RPCs
        }
        // Fail-closed handlers deliberately invalidate the entire session. Do
        // not let later bunches in the same datagram commit unrelated menu,
        // team, role, or combat mutations after that terminal boundary.
        if (conn && conn->IsDisconnected()) break;
    }

    cs.inboundPacketDispatchActive = false;
    if (cs.owningPawnGraphCompletionDeferred) {
        cs.owningPawnGraphCompletionDeferred = false;
        CompleteOwningPawnGraphClose(clientId, pkt.packetId);
    }

    // NOTE: acks are NOT flushed here per-packet (that produced an S2C ack-storm that
    // congested loopback and dropped reliable packets). They stay queued and are flushed
    // coalesced once per pump cycle by FlushPendingAcks() (and piggyback on any data packet
    // we send in response). The handshake responses above already drain the queued ack.

    Logger::Trace("[ConnectionManager::ParseIncomingControl] Exit: client=%u now in state %s",
                  clientId, HandshakePhaseName(GetOrCreateHandshake(clientId).Phase()));
    return true;
}
