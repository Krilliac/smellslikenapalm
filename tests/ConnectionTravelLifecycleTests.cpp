#include "TestFramework.h"

#include "Network/ClientTravelReplication.h"
#include "Network/ActorReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/ConnectionManager.h"
#include "Network/ControlChannel.h"
#include "Network/PacketCodec.h"
#include "Network/SpawnReplication.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ConnectionTravelLifecycleTestHarness {
public:
    static std::shared_ptr<ClientConnection> AddClient(
        ConnectionManager& manager, uint32_t clientId,
        const std::string& ip, uint16_t port, bool joined,
        uint32_t ch2Reliable, bool travelPending,
        const std::string& presentedSteamId = {}) {
        auto connection = std::make_shared<ClientConnection>(
            clientId, ip, port, nullptr, &manager);
        connection->SetUE3Client(true);
        connection->SetHandshakeComplete(joined);
        if (!presentedSteamId.empty()) {
            connection->SetPresentedSteamID(presentedSteamId);
        }
        manager.m_clients.emplace(ClientAddress{ip, port}, connection);
        auto& state = manager.m_controlState[clientId];
        state.ch2OutReliable = ch2Reliable;
        state.mapTravelPending = travelPending;
        state.outboundActorChannels.set(2u, ch2Reliable != 0u);
        manager.m_nextClientId = std::max(manager.m_nextClientId, clientId + 1u);
        return connection;
    }

    static void AddNullClientEntry(ConnectionManager& manager,
                                   const std::string& ip, uint16_t port) {
        manager.m_clients.emplace(ClientAddress{ip, port}, nullptr);
    }

    static void ReconcileTelemetry(const ConnectionManager& manager) {
        manager.UpdateTelemetryPlayerCounts();
    }

    static size_t ClientEntryCount(const ConnectionManager& manager) {
        return manager.m_clients.size();
    }

    static uint32_t NextClientId(const ConnectionManager& manager) {
        return manager.m_nextClientId;
    }

    static size_t HandshakeCount(const ConnectionManager& manager) {
        return manager.m_handshakes.size();
    }

    static void Deliver(ConnectionManager& manager,
                        const std::vector<uint8_t>& datagram,
                        const std::string& ip, uint16_t port) {
        manager.HandleIncomingPacket(datagram, ClientAddress{ip, port});
    }

    static bool SeedJoinedHandshake(ConnectionManager& manager,
                                    uint32_t clientId) {
        auto handshake = std::make_unique<HandshakeState>(
            clientId, [](const std::vector<uint8_t>&) {});
        handshake->HandleControlMessage(
            std::vector<uint8_t>{ControlChannel::Handshake::kStart, 0x01u});
        handshake->HandleControlMessage(
            std::vector<uint8_t>{ControlChannel::Handshake::kResponse});
        handshake->HandleControlMessage(
            std::vector<uint8_t>{NMTByte(NMT::Hello), 0x01u});

        ControlChannel::LoginMessage login;
        login.url = "VNTE-Resort?Name=Reconnect";
        handshake->HandleControlMessage(ControlChannel::BuildLogin(login));
        handshake->HandleControlMessage(
            ControlChannel::BuildJoin(ControlChannel::JoinMessage{}));
        const bool joined = handshake->Phase() == HandshakePhase::Joined;
        manager.m_handshakes[clientId] = std::move(handshake);
        return joined;
    }

    static void SetRoleReady(ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.teamSelected = true;
        state.roleFinalized = true;
    }

    static void SetPublishedSpawnSelectionState(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.teamSelected = true;
        state.roleFinalized = true;
        state.spawned = false;
        state.advertisedSpawnCount = 1;
        state.advertisedSpawnIds[0] = 7001u;
        state.advertisedSpawnVolumeRefs[0] = 289134u;
    }

    enum class RecoveryDecision : uint8_t {
        Respond,
        Malformed,
        Ineligible,
        SequenceExhausted,
        RateLimited,
        LimitReached,
        Suppressed,
    };

    static void SetPossessionRecoveryEligibility(
        ConnectionManager& manager, uint32_t clientId, bool spawned,
        bool pawnGraphOpen, bool possessionAcked) {
        auto& state = manager.m_controlState.at(clientId);
        state.spawned = spawned;
        state.pawnGraphOpen = pawnGraphOpen;
        state.possessionAcked = possessionAcked;
    }

    static RecoveryDecision EvaluatePossessionRecovery(
        ConnectionManager& manager, uint32_t clientId,
        bool exactStandaloneRequest, uint64_t nowMs) {
        const auto decision = ConnectionManager::EvaluatePossessionRecovery(
            manager.m_controlState.at(clientId), exactStandaloneRequest, nowMs);
        return static_cast<RecoveryDecision>(static_cast<uint8_t>(decision));
    }

    static void ResetPossessionRecovery(ConnectionManager& manager,
                                        uint32_t clientId) {
        ConnectionManager::ResetPossessionRecovery(
            manager.m_controlState.at(clientId));
    }

    static uint8_t PossessionRecoveryResponses(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .possessionRecoveryResponses;
    }

    static uint64_t LastPossessionRecoveryResponseMs(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .lastPossessionRecoveryResponseMs;
    }

    static bool PossessionRecoveryLimitLogged(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .possessionRecoveryLimitLogged;
    }

    static bool PossessionAcked(const ConnectionManager& manager,
                                uint32_t clientId) {
        return manager.m_controlState.at(clientId).possessionAcked;
    }

    static uint32_t Ch2OutboundReliable(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId).ch2OutReliable;
    }

    static uint32_t LocalPawnChannel() {
        return ConnectionManager::kLocalPawnChannel;
    }

    static void BeginDeploymentGeneration(ConnectionManager& manager) {
        manager.BeginDeploymentGeneration();
    }

    static std::optional<DeploymentCoordinator::ClientStateSnapshot>
    DeploymentState(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_deploymentCoordinator.GetClientState(clientId);
    }

    static bool AuthorizePublishedSpawn(ConnectionManager& manager,
                                        uint32_t clientId) {
        const std::array<uint32_t, 1> available{{7001u}};
        if (manager.m_deploymentCoordinator.SelectSpawn(
                clientId, 128u, available) !=
            DeploymentCoordinator::SelectionResult::Accepted) {
            return false;
        }
        return manager.m_deploymentCoordinator.SetReadyStatus(
                   clientId, DeploymentCoordinator::ReadyStatus::Ready,
                   available).IsNewAuthorization();
    }

    static uint32_t NextOutboundPacketId(const ConnectionManager& manager,
                                         uint32_t clientId) {
        return manager.m_controlState.at(clientId).outbound.NextPacketId();
    }

    static void RetireSuperseded(ConnectionManager& manager,
                                 uint32_t newClientId,
                                 uint64_t steamId) {
        manager.RetireSupersededTravelSessions(newClientId, steamId);
    }

    static void PeerClosePlayerController(ConnectionManager& manager,
                                          uint32_t clientId) {
        PacketCodec::Bunch close;
        close.bClose = true;
        close.chIndex = 2;
        manager.DecodeInboundActorBunch(clientId, close);
    }

    static bool TravelPending(const ConnectionManager& manager,
                              uint32_t clientId) {
        return manager.m_controlState.at(clientId).mapTravelPending;
    }

    static void SetTravelPending(ConnectionManager& manager,
                                 uint32_t clientId,
                                 uint64_t startedAtMs = 1) {
        auto& state = manager.m_controlState.at(clientId);
        state.mapTravelPending = true;
        state.mapTravelStartedMs = startedAtMs;
    }

    static uint64_t TravelStartedMs(const ConnectionManager& manager,
                                    uint32_t clientId) {
        return manager.m_controlState.at(clientId).mapTravelStartedMs;
    }

    static uint64_t TravelTimeoutMs() {
        return ConnectionManager::kMapTravelTimeoutMs;
    }

    static bool EvaluateRetailRoundClockPolicy(
        DeploymentCountdown::Phase phase,
        bool waitForReadyPlayer,
        bool hasJoinedRetailClient,
        bool hasReadyRetailClient) {
        return ConnectionManager::EvaluateRetailRoundClockPolicy(
            phase, waitForReadyPlayer, hasJoinedRetailClient,
            hasReadyRetailClient);
    }

    static size_t ExpirePendingTravelSessions(ConnectionManager& manager,
                                              uint64_t nowMs) {
        return manager.ExpirePendingTravelSessions(nowMs);
    }

    static bool HasTravelHeadroomLease(const ConnectionManager& manager,
                                       uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .travelReconnectHeadroomLease;
    }

    static size_t PendingReliableCount(const ConnectionManager& manager,
                                       uint32_t clientId) {
        return manager.m_controlState.at(clientId).pendingReliable.size();
    }

    static void SendPriorCh2Reliable(ConnectionManager& manager,
                                     uint32_t clientId) {
        manager.SendCh2Rpc(clientId, {0x01u}, 1u, "PriorReliableForTest");
    }

    static void SendControlReliable(ConnectionManager& manager,
                                    uint32_t clientId,
                                    const std::vector<uint8_t>& payload) {
        (void)manager.SendRawToClient(clientId, payload);
    }

    static uint32_t FirstPendingPacketId(const ConnectionManager& manager,
                                         uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .pendingReliable.front().packetIds.front();
    }

    static size_t FirstPendingBunchCount(const ConnectionManager& manager,
                                         uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .pendingReliable.front().bunches.size();
    }

    static void Acknowledge(ConnectionManager& manager, uint32_t clientId,
                            uint32_t packetId) {
        manager.OnClientAck(clientId, packetId);
    }

    static DeploymentRepl::RetailParticipantInitialState RemoteParticipant(
        const ParticipantId& participant, uint8_t serverTeamId = 1) {
        DeploymentRepl::RetailParticipantInitialState state;
        state.combat.participant = participant;
        state.combat.health = 100;
        state.serverTeamId = serverTeamId;
        state.playerName = participant.IsBot() ? "TravelBot" : "TravelHuman";
        state.positionUu = Vector3(100.0f, 200.0f, 300.0f);
        state.pawnPresent = true;
        return state;
    }

    static bool QueueRemotePri(
        ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant, uint8_t serverTeamId = 1) {
        auto& state = manager.m_controlState.at(viewerClientId);
        state.teamSelected = true;
        state.teamInfoChannels = {76u, 56u};
        std::vector<PacketCodec::Bunch> output;
        return manager.QueueRemoteParticipantPriOpen(
                   viewerClientId,
                   RemoteParticipant(participant, serverTeamId), output) ==
               ConnectionManager::RemotePriOpenResult::OpenQueued;
    }

    static std::optional<uint32_t> RemotePriChannel(
        const ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        const auto* binding = manager.m_controlState.at(viewerClientId)
                                  .remoteParticipants.Find(participant);
        if (!binding) return std::nullopt;
        return binding->priChannel;
    }

    static std::optional<bool> RemotePriDeadWireValue(
        const ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        const auto* binding = manager.m_controlState.at(viewerClientId)
                                  .remoteParticipants.Find(participant);
        if (!binding || !binding->priDeadWireValid) return std::nullopt;
        return binding->priDeadWireValue;
    }

    static void DeliverActorBunch(ConnectionManager& manager,
                                  uint32_t clientId,
                                  const PacketCodec::Bunch& bunch) {
        manager.DecodeInboundActorBunch(clientId, bunch);
    }

    static uint64_t VivoxAcceptedRecords(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId).vivoxNoOpRecordsAccepted;
    }

    static uint64_t VivoxRejectedBunches(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId).vivoxBunchesRejected;
    }

    static std::optional<uint32_t> QueueRemotePriClassRef(
        ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant, std::string_view variant) {
        std::string error;
        const auto selection = RetailBootstrap::ResolveArtifactSelection(
            variant, error);
        if (!selection) return std::nullopt;

        auto& state = manager.m_controlState.at(viewerClientId);
        state.teamSelected = true;
        state.teamInfoChannels = {76u, 56u};
        state.retailArtifactSelectionResolved = true;
        state.retailArtifactSelection = selection;
        std::vector<PacketCodec::Bunch> output;
        if (manager.QueueRemoteParticipantPriOpen(
                viewerClientId, RemoteParticipant(participant), output) !=
                ConnectionManager::RemotePriOpenResult::OpenQueued ||
            output.empty()) {
            return std::nullopt;
        }

        BitReader reader(output.front().payload.data(),
                         output.front().payload.size(),
                         output.front().payloadBits);
        const ActorRepl::NetGUIDRef classRef =
            ActorRepl::ReadNetGUID(reader);
        if (reader.IsOverflowed() || classRef.isDynamic) return std::nullopt;
        return classRef.index;
    }

    static bool FreezeRetailBootstrap(
        ConnectionManager& manager, uint32_t clientId,
        std::string_view variant) {
        std::string error;
        const auto selection = RetailBootstrap::ResolveArtifactSelection(
            variant, error);
        if (!selection) return false;

        auto& state = manager.m_controlState.at(clientId);
        state.retailArtifactSelectionResolved = true;
        state.retailArtifactSelection = selection;
        state.retailBootstrapProfile = RetailBootstrap::CanonicalProfile();
        return true;
    }

    static void SendLiveActorBootstrap(ConnectionManager& manager,
                                       uint32_t clientId) {
        manager.SendLiveActorBootstrap(clientId);
    }

    static void SendActorBootstrap(ConnectionManager& manager,
                                   uint32_t clientId) {
        manager.SendActorBootstrap(clientId);
    }

    static std::vector<PacketCodec::Bunch> QueuedReliableBunches(
        const ConnectionManager& manager, uint32_t clientId) {
        std::vector<PacketCodec::Bunch> bunches;
        for (const auto& pending :
             manager.m_controlState.at(clientId).pendingReliable) {
            bunches.insert(bunches.end(), pending.bunches.begin(),
                           pending.bunches.end());
        }
        return bunches;
    }

    static bool QueueRemotePawn(
        ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant, uint8_t serverTeamId = 1) {
        std::vector<PacketCodec::Bunch> output;
        return manager.QueueRemoteParticipantPawnOpen(
                   viewerClientId,
                   RemoteParticipant(participant, serverTeamId), output) ==
                   ConnectionManager::RemotePriOpenResult::OpenQueued &&
               !output.empty();
    }

    static ParticipantActorOpenState RemotePawnState(
        const ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        const auto* binding = manager.m_controlState.at(viewerClientId)
                                  .remoteParticipants.Find(participant);
        return binding ? binding->pawnState
                       : ParticipantActorOpenState::Closed;
    }

    static size_t RemoteParticipantCount(
        const ConnectionManager& manager, uint32_t viewerClientId) {
        return manager.m_controlState.at(viewerClientId)
            .remoteParticipants.Size();
    }

    static size_t RetiredRemoteParticipantCount(
        const ConnectionManager& manager, uint32_t viewerClientId) {
        return manager.m_controlState.at(viewerClientId)
            .remoteParticipants.RetiredCount();
    }

    static void SeedRemotePawnView(
        ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        manager.m_controlState.at(viewerClientId)
            .remotePawnViews[participant] = {};
    }

    static size_t RemotePawnViewCount(
        const ConnectionManager& manager, uint32_t viewerClientId) {
        return manager.m_controlState.at(viewerClientId)
            .remotePawnViews.size();
    }
};

namespace {

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::string name, std::string value)
        : m_name(std::move(name)) {
        if (const char* previous = std::getenv(m_name.c_str())) {
            m_previous = previous;
        }
        Set(value);
    }

    ~ScopedEnvironmentVariable() {
        if (m_previous) Set(*m_previous);
        else Clear();
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    void Set(const std::string& value) const {
#ifdef _WIN32
        (void)_putenv_s(m_name.c_str(), value.c_str());
#else
        (void)setenv(m_name.c_str(), value.c_str(), 1);
#endif
    }

    void Clear() const {
#ifdef _WIN32
        (void)_putenv_s(m_name.c_str(), "");
#else
        (void)unsetenv(m_name.c_str());
#endif
    }

    std::string m_name;
    std::optional<std::string> m_previous;
};

const PacketCodec::Bunch* FindQueuedOpen(
    const std::vector<PacketCodec::Bunch>& bunches, uint32_t channel) {
    const auto found = std::find_if(
        bunches.begin(), bunches.end(),
        [channel](const PacketCodec::Bunch& bunch) {
            return bunch.bOpen && !bunch.bClose && bunch.chIndex == channel;
        });
    return found == bunches.end() ? nullptr : &*found;
}

ActorRepl::NetGUIDRef DecodeLiveOpenClass(const PacketCodec::Bunch& bunch,
                                          bool playerController,
                                          bool& overflowed) {
    BitReader reader(bunch.payload.data(), bunch.payload.size(),
                     bunch.payloadBits);
    const ActorRepl::NetGUIDRef classRef = ActorRepl::ReadNetGUID(reader);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    ActorRepl::ReadCompressedVector(reader, x, y, z);
    if (playerController) {
        (void)reader.ReadByte();
    }
    overflowed = reader.IsOverflowed();
    return classRef;
}

ClientTravelRepl::EncodedRpc MakeTravelRpc() {
    ClientTravelRepl::Request request;
    request.url = "VNSK-Compound";
    request.travelType = ClientTravelRepl::TravelType::Relative;
    ClientTravelRepl::EncodedRpc rpc;
    std::string error;
    if (!ClientTravelRepl::Encode(request, rpc, error)) return {};
    return rpc;
}

PacketCodec::Bunch MakeVivoxStateChangeBunch(
    const std::vector<std::pair<uint32_t, uint32_t>>& channels,
    uint32_t actorChannel = 2u, bool reliable = true) {
    BitWriter writer;
    for (const auto& [otherPri, localPc] : channels) {
        writer.SerializeInt(
            DeploymentRepl::kChangeVivoxChannelsStateHandle,
            DeploymentRepl::kRoPlayerControllerMaxHandle);
        writer.WriteBit(true);
        ActorRepl::WriteNetGUID(
            writer, ActorRepl::NetGUIDRef{/*isDynamic=*/true, otherPri});
        writer.WriteBit(true);
        ActorRepl::WriteNetGUID(
            writer, ActorRepl::NetGUIDRef{/*isDynamic=*/true, localPc});
    }

    PacketCodec::Bunch bunch;
    bunch.bReliable = reliable;
    bunch.chIndex = actorChannel;
    bunch.chType = 2u;
    bunch.chSequence = 1u;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

PacketCodec::Bunch MakeParameterlessPcBunch(uint32_t handle,
                                              bool trailingBit = false) {
    BitWriter writer;
    writer.SerializeInt(handle,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    if (trailingBit) writer.WriteBit(false);

    PacketCodec::Bunch bunch;
    bunch.bReliable = true;
    bunch.chIndex = 2u;
    bunch.chType = 2u;
    bunch.chSequence = 1u;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

PacketCodec::Bunch MakePossessionAckBunch(uint32_t pawnChannel) {
    BitWriter writer;
    writer.SerializeInt(
        DeploymentRepl::kServerAcknowledgePossessionHandle,
        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(true);
    ActorRepl::WriteNetGUID(
        writer,
        ActorRepl::NetGUIDRef{/*isDynamic=*/true, pawnChannel});

    PacketCodec::Bunch bunch;
    bunch.bReliable = true;
    bunch.chIndex = 2u;
    bunch.chType = 2u;
    bunch.chSequence = 1u;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

PacketCodec::Bunch MakeCapturedPcBunch(std::vector<uint8_t> payload,
                                       uint32_t payloadBits) {
    PacketCodec::Bunch bunch;
    bunch.bReliable = true;
    bunch.chIndex = 2u;
    bunch.chType = 2u;
    bunch.chSequence = 1u;
    bunch.payload = std::move(payload);
    bunch.payloadBits = payloadBits;
    return bunch;
}

PacketCodec::Packet FreshHandshakeStartPacket() {
    PacketCodec::Bunch start;
    start.bControl = true;
    start.bOpen = true;
    start.bReliable = true;
    start.chIndex = 0;
    start.chType = PacketCodec::kControlChannelType;
    start.chSequence = 1;
    start.payload = {ControlChannel::Handshake::kStart, 0x01u};
    start.payloadBits = 16;

    PacketCodec::Packet packet;
    packet.packetId = 0;
    packet.bunches.push_back(start);
    return packet;
}

std::vector<uint8_t> EncodeClientPacket(const PacketCodec::Packet& packet) {
    return PacketCodec::Encode(packet,
                               PacketCodec::kClientSendMaxPacketBytes);
}

std::vector<uint8_t> PureAckDatagram() {
    PacketCodec::Packet packet;
    packet.packetId = 7;
    packet.acks = {3u, 4u, 5u, 6u, 7u, 8u, 9u};
    return EncodeClientPacket(packet);
}

std::vector<uint8_t> KeepAliveDatagram() {
    PacketCodec::Packet packet;
    packet.packetId = 8;
    return EncodeClientPacket(packet);
}

std::vector<uint8_t> ActorDatagram() {
    PacketCodec::Bunch actor;
    actor.bReliable = true;
    actor.chIndex = 2;
    actor.chSequence = 1;
    actor.payload = {0x01u};
    actor.payloadBits = 1;

    PacketCodec::Packet packet;
    packet.packetId = 9;
    packet.bunches.push_back(actor);
    return EncodeClientPacket(packet);
}

std::vector<uint8_t> ControlCloseDatagram() {
    PacketCodec::Bunch close;
    close.bControl = true;
    close.bClose = true;
    close.bReliable = true;
    close.chIndex = 0;
    close.chSequence = 17;
    close.chType = PacketCodec::kControlChannelType;

    PacketCodec::Packet packet;
    packet.packetId = 41;
    packet.bunches.push_back(close);
    return EncodeClientPacket(packet);
}

} // namespace

TEST(ConnectionTravelLifecycle, RetailRoundClockPolicyNeverParksOutsidePreparation) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_TRUE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Active, true, false, false));
    EXPECT_TRUE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::PostRound, true, true, false));
}

TEST(ConnectionTravelLifecycle,
     EnabledEmptyServerParksRetailRoundClockInPreparation) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_FALSE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, true, false, false));
}

TEST(ConnectionTravelLifecycle,
     IncompleteRetailClientIsEquivalentToNoJoinedRetailClient) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    // The caller excludes an incomplete handshake from the joined summary.
    EXPECT_FALSE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, true, false, false));
}

TEST(ConnectionTravelLifecycle,
     JoinedRetailClientWithoutFinalRoleParksRoundClock) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_FALSE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, true, true, false));
}

TEST(ConnectionTravelLifecycle,
     TravelPendingReadyClientIsModeledAsNotReadyForCurrentWorld) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    // The caller excludes a map-travel-pending client from both current-world
    // summaries, even if its deployment state was previously ready.
    EXPECT_FALSE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, true, false, false));
}

TEST(ConnectionTravelLifecycle,
     EligibleReadyRetailClientAdvancesPreparationClock) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_TRUE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, true, true, true));
}

TEST(ConnectionTravelLifecycle,
     InconsistentReadyWithoutJoinedSnapshotFailsClosed) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_FALSE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, true, false, true));
}

TEST(ConnectionTravelLifecycle,
     DisabledWaitGateAllowsEmptyHeadlessPreparationClock) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_TRUE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, false, false, false));
}

TEST(ConnectionTravelLifecycle,
     DisabledWaitGateStillParksForJoinedNotReadyClient) {
    using Phase = DeploymentCountdown::Phase;
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_FALSE(Harness::EvaluateRetailRoundClockPolicy(
        Phase::Preparation, false, true, false));
}

TEST(ConnectionTravelLifecycle, HeadlessAndCompleteRecipientSetsPreflight) {
    ConnectionManager manager(nullptr);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));

    size_t eligible = 99;
    EXPECT_TRUE(manager.CanBroadcastRetailClientTravel(
        rpc, "VNSK-Compound", &eligible));
    EXPECT_EQ(eligible, static_cast<size_t>(0));

    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30001, true, 7, false);
    EXPECT_TRUE(manager.CanBroadcastRetailClientTravel(
        rpc, "VNSK-Compound", &eligible));
    EXPECT_EQ(eligible, static_cast<size_t>(1));

    // A not-yet-joined endpoint is not part of the world-transition cohort.
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 2, "127.0.0.1", 30002, false, 0, false);
    EXPECT_TRUE(manager.CanBroadcastRetailClientTravel(
        rpc, "VNSK-Compound", &eligible));
    EXPECT_EQ(eligible, static_cast<size_t>(1));
}

TEST(ConnectionTravelLifecycle,
     StaleCleanupQuarantinesNullEntryAndPreservesHealthySession) {
    ConnectionManager manager(nullptr);
    const auto healthy = ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 7, "127.0.0.1", 30007, true, 3, false);
    ConnectionTravelLifecycleTestHarness::AddNullClientEntry(
        manager, "127.0.0.1", 30008);
    ASSERT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(2));

    manager.RemoveStaleConnections();

    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(1));
    EXPECT_EQ(manager.GetConnection(7), healthy);
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30007), 7u);

    // The quarantine path is idempotent and must not retire the surviving peer
    // on later housekeeping passes.
    manager.RemoveStaleConnections();
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(1));
    EXPECT_EQ(manager.GetConnection(7), healthy);
}

TEST(ConnectionTravelLifecycle, AnyHalfOpenOrAlreadyPendingRecipientFailsAll) {
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));

    {
        ConnectionManager manager(nullptr);
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "127.0.0.1", 30101, true, 3, false);
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 2, "127.0.0.1", 30102, true, 0, false);
        size_t eligible = 0;
        EXPECT_FALSE(manager.CanBroadcastRetailClientTravel(
            rpc, "VNSK-Compound", &eligible));
    }

    // Isolate this branch so the earlier missing-cursor peer cannot mask a
    // regression in mapTravelPending admission.
    {
        ConnectionManager manager(nullptr);
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 3, "127.0.0.1", 30103, true, 4, true);
        size_t eligible = 0;
        EXPECT_FALSE(manager.CanBroadcastRetailClientTravel(
            rpc, "VNSK-Compound", &eligible));
    }
}

TEST(ConnectionTravelLifecycle, ClosedPlayerControllerChannelFailsPreflight) {
    ConnectionManager manager(nullptr);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30111, true, 7, false);

    ConnectionTravelLifecycleTestHarness::PeerClosePlayerController(manager, 1);
    size_t eligible = 0;
    EXPECT_FALSE(manager.CanBroadcastRetailClientTravel(
        rpc, "VNSK-Compound", &eligible));
}

TEST(ConnectionTravelLifecycle,
     BroadcastQueuesWholeCohortBeforeMakingItDrainOnly) {
    ConnectionManager manager(nullptr);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30121, true, 7, false);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 2, "127.0.0.1", 30122, true, 9, false);
    ConnectionTravelLifecycleTestHarness::SendPriorCh2Reliable(manager, 1);

    EXPECT_EQ(manager.BroadcastRetailClientTravel(rpc, "VNSK-Compound"),
              static_cast<size_t>(2));
    EXPECT_TRUE(
        ConnectionTravelLifecycleTestHarness::TravelPending(manager, 1));
    EXPECT_TRUE(
        ConnectionTravelLifecycleTestHarness::TravelPending(manager, 2));
    EXPECT_NE(ConnectionTravelLifecycleTestHarness::TravelStartedMs(manager, 1),
              0u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::TravelStartedMs(manager, 1),
              ConnectionTravelLifecycleTestHarness::TravelStartedMs(manager, 2));
    // The pre-existing reliable and ClientTravel both remain in client 1's
    // retransmission ledger; client 2 has its ClientTravel entry.
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(2));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 2),
              static_cast<size_t>(1));
}

TEST(ConnectionTravelLifecycle,
     TravelKeepsTransportGaugeButDropsAuthenticatedGaugeUntilRetirement) {
    auto& metrics =
        Telemetry::TelemetryManager::Instance().GetCustomMetrics();
    metrics.UpdatePlayerCounts(0, 0);

    ConnectionManager manager(nullptr);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 9, "127.0.0.1", 30129, true, 7, false);
    ConnectionTravelLifecycleTestHarness::ReconcileTelemetry(manager);
    EXPECT_EQ(metrics.activeConnections.load(), 1u);
    EXPECT_EQ(metrics.authenticatedPlayers.load(), 1u);

    ASSERT_EQ(manager.BroadcastRetailClientTravel(rpc, "VNSK-Compound"),
              static_cast<size_t>(1));
    EXPECT_EQ(metrics.activeConnections.load(), 1u);
    EXPECT_EQ(metrics.authenticatedPlayers.load(), 0u);

    constexpr uint64_t startedAt = 9000;
    ConnectionTravelLifecycleTestHarness::SetTravelPending(
        manager, 9, startedAt);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ExpirePendingTravelSessions(
                  manager,
                  startedAt +
                      ConnectionTravelLifecycleTestHarness::TravelTimeoutMs()),
              static_cast<size_t>(1));
    EXPECT_EQ(metrics.activeConnections.load(), 0u);
    EXPECT_EQ(metrics.authenticatedPlayers.load(), 0u);
}

TEST(ConnectionTravelLifecycle,
     AcknowledgedTravelStillExpiresOnIndependentFiniteDeadline) {
    ConnectionManager manager(nullptr);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30123, true, 7, false);

    ASSERT_EQ(manager.BroadcastRetailClientTravel(rpc, "VNSK-Compound"),
              static_cast<size_t>(1));
    ASSERT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(1));
    const uint32_t travelPacket =
        ConnectionTravelLifecycleTestHarness::FirstPendingPacketId(manager, 1);
    ConnectionTravelLifecycleTestHarness::Acknowledge(
        manager, 1, travelPacket);
    ASSERT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(0));

    constexpr uint64_t startedAt = 5000;
    ConnectionTravelLifecycleTestHarness::SetTravelPending(
        manager, 1, startedAt);
    const uint64_t timeout =
        ConnectionTravelLifecycleTestHarness::TravelTimeoutMs();
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ExpirePendingTravelSessions(
                  manager, startedAt + timeout - 1),
              static_cast<size_t>(0));
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30123), 1u);

    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ExpirePendingTravelSessions(
                  manager, startedAt + timeout),
              static_cast<size_t>(1));
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30123),
              std::numeric_limits<uint32_t>::max());
}

TEST(ConnectionTravelLifecycle,
     MissingOrInvalidTravelClockFailsClosedWithoutTouchingHealthyPeer) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30124, true, 7, true);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 2, "127.0.0.1", 30125, true, 7, false);

    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ExpirePendingTravelSessions(
                  manager, 1000),
              static_cast<size_t>(1));
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30124),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30125), 2u);
}

TEST(ConnectionTravelLifecycle,
     GracefulPeerCloseEndsPendingTravelBeforeItsDeadline) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30126, true, 7, false);
    ConnectionTravelLifecycleTestHarness::SetTravelPending(
        manager, 1, 5000);

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, ControlCloseDatagram(), "127.0.0.1", 30126);

    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30126),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ExpirePendingTravelSessions(
                  manager, 5001),
              static_cast<size_t>(0));
}

TEST(ConnectionTravelLifecycle,
     ReliableControlPacketsEnterAndLeaveRetransmissionLedgerIndependently) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30125, false, 0, false);

    ConnectionTravelLifecycleTestHarness::SendControlReliable(
        manager, 1, {NMTByte(NMT::Uses), 0x01u});
    ConnectionTravelLifecycleTestHarness::SendControlReliable(
        manager, 1, {NMTByte(NMT::Uses), 0x02u});

    ASSERT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(2));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::FirstPendingBunchCount(
                  manager, 1),
              static_cast<size_t>(1));
    const uint32_t firstPacket =
        ConnectionTravelLifecycleTestHarness::FirstPendingPacketId(manager, 1);
    ConnectionTravelLifecycleTestHarness::Acknowledge(
        manager, 1, firstPacket);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(1));
}

TEST(ConnectionTravelLifecycle,
     UngroundedRemotePawnTemplateCannotOpenGhostVisual) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30131, true, 7, false);
    const ParticipantId bot = ParticipantId::Bot(9);

    EXPECT_FALSE(DeploymentRepl::kRemotePawnVisualTemplatesGrounded);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::QueueRemotePri(
        manager, 1, bot));
    EXPECT_FALSE(ConnectionTravelLifecycleTestHarness::QueueRemotePawn(
        manager, 1, bot));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::RemotePawnState(
                  manager, 1, bot),
              ParticipantActorOpenState::Unopened);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::RemotePawnViewCount(
                  manager, 1),
              static_cast<size_t>(0));
}

TEST(ConnectionTravelLifecycle,
     RemotePriOpenSeedsExplicitDeadWireCache) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30136, true, 7, false);
    const ParticipantId bot = ParticipantId::Bot(90);

    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::QueueRemotePri(
        manager, 1, bot));
    EXPECT_EQ(
        ConnectionTravelLifecycleTestHarness::RemotePriDeadWireValue(
            manager, 1, bot),
              std::optional<bool>(false));
}

TEST(ConnectionTravelLifecycle,
     PriSetHonorLevelDoesNotEnterPlayerControllerVivoxProbe) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30137, true, 7, false);

    // Live C2S session_1784090912848 seq137 carries this exact unreliable
    // ch26 payload once per second. Under the local PRI class table it is h83
    // SetHonorLevel(byte): seven handle bits followed by Send=0, so the byte
    // parameter takes its default value. It is a complete eight-bit RPC, not a
    // truncated ROPlayerController field.
    PacketCodec::Bunch honor;
    honor.bReliable = false;
    honor.chIndex = 26u;
    honor.chType = 2u;
    honor.payload = {0x53u};
    honor.payloadBits = 8u;

    BitReader priReader(honor.payload.data(), honor.payload.size(),
                        honor.payloadBits);
    EXPECT_EQ(priReader.SerializeInt(
                  DeploymentRepl::kRoPlayerReplicationInfoMaxHandle),
              83u);
    EXPECT_FALSE(priReader.IsOverflowed());
    EXPECT_EQ(priReader.BitPos(), static_cast<size_t>(7));
    EXPECT_FALSE(priReader.ReadBit());
    EXPECT_FALSE(priReader.IsOverflowed());
    EXPECT_EQ(priReader.BitPos(), static_cast<size_t>(8));

    // The h152 probe belongs exclusively to owning PlayerController ch2. A
    // regression that applies its maxHandle=531 table to this PRI bunch asks
    // for a ninth bit and emits the live "pos 8 / 8 readable" warning.
    Logger::Initialize();
    Logger::SetLevel(LogLevel::Warn);
    std::ostringstream captured;
    std::streambuf* original = std::cout.rdbuf(captured.rdbuf());
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1, honor);
    std::cout.rdbuf(original);
    Logger::Shutdown();

    EXPECT_EQ(captured.str().find("[BitReader] overflow"),
              std::string::npos);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxAcceptedRecords(
                  manager, 1),
              0u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxRejectedBunches(
                  manager, 1),
              0u);
}

TEST(ConnectionTravelLifecycle,
     VivoxStateChangeHandlerAcceptsOnlyOwningKnownPriBatches) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30137, true, 7, false);
    const ParticipantId bot = ParticipantId::Bot(91);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::QueueRemotePri(
        manager, 1, bot));
    const std::optional<uint32_t> remotePri =
        ConnectionTravelLifecycleTestHarness::RemotePriChannel(
            manager, 1, bot);
    ASSERT_TRUE(remotePri.has_value());

    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeVivoxStateChangeBunch({{26u, 2u}, {*remotePri, 2u}}));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxAcceptedRecords(
                  manager, 1),
              2u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxRejectedBunches(
                  manager, 1),
              0u);

    // A mixed batch is transactional: its valid first record does not count
    // when a later record names an unknown viewer-local actor channel.
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeVivoxStateChangeBunch({{*remotePri, 2u}, {700u, 2u}}));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxAcceptedRecords(
                  manager, 1),
              2u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxRejectedBunches(
                  manager, 1),
              1u);

    // A remote pawn channel is not interchangeable with its open PRI, and the
    // second object must resolve to this connection's owning PC channel.
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeVivoxStateChangeBunch({{*remotePri + 1u, 2u}}));
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeVivoxStateChangeBunch({{*remotePri, 3u}}));
    // A PC-shaped payload on another actor channel is dropped by class/channel
    // dispatch before it can enter h152 accounting.
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeVivoxStateChangeBunch({{*remotePri, 2u}}, 3u));
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeVivoxStateChangeBunch({{*remotePri, 2u}}, 2u,
                                  /*reliable=*/false));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxAcceptedRecords(
                  manager, 1),
              2u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::VivoxRejectedBunches(
                  manager, 1),
              4u);
}

TEST(ConnectionTravelLifecycle,
     ReopenSpawnSelectRequiresExactPublishedPreSpawnState) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30138, true, 7, false);

    // The parameterless h208 grammar is exact; a trailing bit cannot be
    // reinterpreted as a valid recovery request.
    ConnectionTravelLifecycleTestHarness::SetPublishedSpawnSelectionState(
        manager, 1);
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1, MakeParameterlessPcBunch(208u, true));
    EXPECT_TRUE(
        ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(manager, 1)
            .empty());

    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1, MakeParameterlessPcBunch(208u));
    const std::vector<PacketCodec::Bunch> queued =
        ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(manager, 1);
    ASSERT_EQ(queued.size(), static_cast<size_t>(1));
    const PacketCodec::Bunch& response = queued.front();
    EXPECT_TRUE(response.bReliable);
    EXPECT_EQ(response.chIndex, 2u);
    BitReader reader(response.payload.data(), response.payload.size(),
                     response.payloadBits);
    EXPECT_EQ(reader.SerializeInt(
                  DeploymentRepl::kRoPlayerControllerMaxHandle),
              209u);
    EXPECT_FALSE(reader.IsOverflowed());
    EXPECT_EQ(reader.BitPos(), response.payloadBits);

    ConnectionManager unready(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        unready, 2, "127.0.0.1", 30139, true, 7, false);
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        unready, 2, MakeParameterlessPcBunch(208u));
    EXPECT_TRUE(ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(
                    unready, 2)
                    .empty());
}

TEST(ConnectionTravelLifecycle,
     PossessionRecoveryGateRateLimitsAndCapsPerDeployment) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    using Decision = Harness::RecoveryDecision;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30142, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);

    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1000u),
              Decision::Respond);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1999u),
              Decision::RateLimited);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 2000u),
              Decision::Respond);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 3000u),
              Decision::Respond);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 3u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 3000u);

    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 4000u),
              Decision::LimitReached);
    EXPECT_TRUE(Harness::PossessionRecoveryLimitLogged(manager, 1));
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 5000u),
              Decision::Suppressed);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 3u);

    Harness::ResetPossessionRecovery(manager, 1);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 0u);
    EXPECT_FALSE(Harness::PossessionRecoveryLimitLogged(manager, 1));
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 5000u),
              Decision::Respond);
}

TEST(ConnectionTravelLifecycle,
     AskForPawnRequiresExactReliableStandaloneOwningChannelBunch) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30143, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);

    PacketCodec::Bunch exact = MakeParameterlessPcBunch(42u);
    ASSERT_EQ(exact.payloadBits, 9u);
    const uint32_t initialReliable =
        Harness::Ch2OutboundReliable(manager, 1);
    Harness::DeliverActorBunch(manager, 1, exact);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 1u);
    EXPECT_EQ(Harness::Ch2OutboundReliable(manager, 1),
              initialReliable + 3u);

    PacketCodec::Bunch trailing = MakeParameterlessPcBunch(42u, true);
    ASSERT_EQ(trailing.payloadBits, 10u);
    Harness::DeliverActorBunch(manager, 1, trailing);
    PacketCodec::Bunch wrongChannel = exact;
    wrongChannel.chIndex = 3u;
    Harness::DeliverActorBunch(manager, 1, wrongChannel);
    PacketCodec::Bunch unreliable = exact;
    unreliable.bReliable = false;
    Harness::DeliverActorBunch(manager, 1, unreliable);

    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 1u);
    EXPECT_EQ(Harness::Ch2OutboundReliable(manager, 1),
              initialReliable + 3u);

    Harness::ResetPossessionRecovery(manager, 1);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, false, false);
    Harness::DeliverActorBunch(manager, 1, exact);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, true);
    Harness::DeliverActorBunch(manager, 1, exact);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::Ch2OutboundReliable(manager, 1),
              initialReliable + 3u);
}

TEST(ConnectionTravelLifecycle,
     AskForPawnRecoveryNeverOverflowsWireChSequence) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    PacketCodec::Bunch exact = MakeParameterlessPcBunch(42u);
    ASSERT_EQ(exact.payloadBits, 9u);

    ConnectionManager boundaryAllowed(nullptr);
    Harness::AddClient(
        boundaryAllowed, 1, "127.0.0.1", 30145, true,
        PacketCodec::kMaxChSequence - 4u, false);
    Harness::SetPossessionRecoveryEligibility(
        boundaryAllowed, 1, true, true, false);
    Harness::DeliverActorBunch(boundaryAllowed, 1, exact);
    EXPECT_EQ(Harness::Ch2OutboundReliable(boundaryAllowed, 1),
              PacketCodec::kMaxChSequence - 1u);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(boundaryAllowed, 1), 1u);

    ConnectionManager boundaryRejected(nullptr);
    Harness::AddClient(
        boundaryRejected, 2, "127.0.0.1", 30146, true,
        PacketCodec::kMaxChSequence - 3u, false);
    Harness::SetPossessionRecoveryEligibility(
        boundaryRejected, 2, true, true, false);
    Harness::DeliverActorBunch(boundaryRejected, 2, exact);
    EXPECT_EQ(Harness::Ch2OutboundReliable(boundaryRejected, 2),
              PacketCodec::kMaxChSequence - 3u);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(boundaryRejected, 2), 0u);
    EXPECT_TRUE(Harness::PossessionRecoveryLimitLogged(boundaryRejected, 2));
}

TEST(ConnectionTravelLifecycle,
     PossessionRecoveryResetsOnAckGenerationAndMapTravel) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    using Decision = Harness::RecoveryDecision;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30144, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);
    ASSERT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1000u),
              Decision::Respond);

    Harness::DeliverActorBunch(
        manager, 1,
        MakePossessionAckBunch(Harness::LocalPawnChannel()));
    EXPECT_TRUE(Harness::PossessionAcked(manager, 1));
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 0u);

    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);
    ASSERT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 2000u),
              Decision::Respond);
    Harness::BeginDeploymentGeneration(manager);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_FALSE(Harness::PossessionRecoveryLimitLogged(manager, 1));

    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);
    ASSERT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 3000u),
              Decision::Respond);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));
    ASSERT_EQ(manager.BroadcastRetailClientTravel(rpc, "VNSK-Compound"),
              static_cast<size_t>(1));
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 0u);
    EXPECT_FALSE(Harness::PossessionRecoveryLimitLogged(manager, 1));
}

TEST(ConnectionTravelLifecycle,
     SpawnVolumeDeploymentHandlerIsTransactionalAndAppliesWireOrder) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30140, true, 7, false);
    ConnectionTravelLifecycleTestHarness::SetPublishedSpawnSelectionState(
        manager, 1);
    ConnectionTravelLifecycleTestHarness::BeginDeploymentGeneration(manager);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::AuthorizePublishedSpawn(
        manager, 1));

    // Flipping h275's final bool makes the otherwise exact 64-bit corpus shape
    // invalid. Full-bunch validation must leave the existing authorization
    // untouched.
    std::vector<uint8_t> malformedForceOnly{
        0x72, 0x73, 0x4a, 0x23, 0x00, 0xc8, 0xde, 0xc4};
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1, MakeCapturedPcBunch(malformedForceOnly, 64u));
    auto state = ConnectionTravelLifecycleTestHarness::DeploymentState(
        manager, 1);
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->deploymentAuthorized);
    EXPECT_EQ(state->readyStatus, DeploymentCoordinator::ReadyStatus::Ready);

    // The valid first live bunch applies its sole ordered h434 ForceOnly and
    // revokes authorization only after the complete h370+h434+h275 grammar was
    // accepted.
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeCapturedPcBunch(
            {0x72, 0x73, 0x4a, 0x23, 0x00, 0xc8, 0xde, 0x44}, 64u));
    state = ConnectionTravelLifecycleTestHarness::DeploymentState(manager, 1);
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->deploymentAuthorized);
    EXPECT_EQ(state->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);

    // A fresh exact auto-select bunch (including the live h89(default)
    // spectator-location reset) applies h261 before h434. With this
    // intentionally detached test manager, current spawn revalidation is empty:
    // the h261 emits one PRI confirmation packet, then h434 fail-closes and
    // clears the selection rather than authorizing against stale h59 state.
    ConnectionTravelLifecycleTestHarness::BeginDeploymentGeneration(manager);
    const uint32_t packetBefore =
        ConnectionTravelLifecycleTestHarness::NextOutboundPacketId(manager, 1);
    ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
        manager, 1,
        MakeCapturedPcBunch(
            {0x72, 0x73, 0x4a, 0x23, 0x00, 0x14, 0x0c, 0x28,
             0x1b, 0xad, 0x82, 0x01, 0x65, 0xa3, 0x95, 0x05},
            126u));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextOutboundPacketId(
                  manager, 1),
              packetBefore + 1u);
    state = ConnectionTravelLifecycleTestHarness::DeploymentState(manager, 1);
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->selectedSlot.has_value());
    EXPECT_FALSE(state->deploymentAuthorized);
    EXPECT_EQ(state->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
}

TEST(ConnectionTravelLifecycle,
     RemotePriOpenUsesFrozenArtifactRoGameLayout) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30132, true, 7, false);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 2, "127.0.0.1", 30133, true, 7, false);

    const std::optional<uint32_t> canonical =
        ConnectionTravelLifecycleTestHarness::QueueRemotePriClassRef(
            manager, 1, ParticipantId::Bot(10), {});
    const std::optional<uint32_t> installed =
        ConnectionTravelLifecycleTestHarness::QueueRemotePriClassRef(
            manager, 2, ParticipantId::Bot(11), "installed");
    ASSERT_TRUE(canonical.has_value());
    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(*canonical, 86701u);
    EXPECT_EQ(*installed, 86704u);
}

TEST(ConnectionTravelLifecycle,
     LiveActorBootstrapQueuesClassesAndGameClassFromFrozenArtifact) {
    struct LayoutCase {
        std::string_view variant;
        uint32_t playerController;
        uint32_t gameReplicationInfo;
        uint32_t teamInfo;
        uint32_t playerReplicationInfo;
        uint32_t territoriesGameClass;
    };
    const std::vector<LayoutCase> layouts{
        {{}, 57520u, 70887u, 90245u, 86701u, 69601u},
        {"installed", 57522u, 70889u, 90248u, 86704u, 69603u},
    };

    for (const LayoutCase& expected : layouts) {
        ConnectionManager manager(nullptr);
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "127.0.0.1", 30134, true, 0, false);
        ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
            manager, 1, expected.variant));

        ConnectionTravelLifecycleTestHarness::SendLiveActorBootstrap(manager, 1);
        const std::vector<PacketCodec::Bunch> queued =
            ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(
                manager, 1);

        const PacketCodec::Bunch* pc = FindQueuedOpen(queued, 2);
        const PacketCodec::Bunch* gri = FindQueuedOpen(queued, 3);
        const PacketCodec::Bunch* team0 = FindQueuedOpen(queued, 4);
        const PacketCodec::Bunch* team1 = FindQueuedOpen(queued, 5);
        const PacketCodec::Bunch* pri = FindQueuedOpen(queued, 26);
        ASSERT_TRUE(pc != nullptr);
        ASSERT_TRUE(gri != nullptr);
        ASSERT_TRUE(team0 != nullptr);
        ASSERT_TRUE(team1 != nullptr);
        ASSERT_TRUE(pri != nullptr);

        struct OpenExpectation {
            const PacketCodec::Bunch* bunch;
            uint32_t classRef;
            bool playerController;
        };
        const std::vector<OpenExpectation> opens{
            {pc, expected.playerController, true},
            {gri, expected.gameReplicationInfo, false},
            {team0, expected.teamInfo, false},
            {team1, expected.teamInfo, false},
            {pri, expected.playerReplicationInfo, false},
        };
        for (const OpenExpectation& open : opens) {
            bool overflowed = false;
            const ActorRepl::NetGUIDRef classRef = DecodeLiveOpenClass(
                *open.bunch, open.playerController, overflowed);
            EXPECT_FALSE(overflowed);
            EXPECT_FALSE(classRef.isDynamic);
            EXPECT_EQ(classRef.index, open.classRef);
        }

        // Both playable TeamInfo opens must seed h62 before spawn selection.
        // With no GameServer/TicketSystem in this harness, initial==0 means an
        // unlimited pool and therefore the positive retail display sentinel.
        const std::array<std::pair<const PacketCodec::Bunch*, int32_t>, 2>
            teamOpens{{{team0, 0}, {team1, 1}}};
        for (const auto& [teamBunch, expectedTeam] : teamOpens) {
            BitReader teamReader(teamBunch->payload.data(),
                                 teamBunch->payload.size(),
                                 teamBunch->payloadBits);
            const ActorRepl::NetGUIDRef teamClass =
                ActorRepl::ReadNetGUID(teamReader);
            EXPECT_FALSE(teamClass.isDynamic);
            EXPECT_EQ(teamClass.index, expected.teamInfo);
            float teamX = 0.0f, teamY = 0.0f, teamZ = 0.0f;
            ActorRepl::ReadCompressedVector(teamReader, teamX, teamY, teamZ);
            EXPECT_EQ(teamReader.SerializeInt(
                          SpawnRepl::kTeamInfoMaxHandle),
                      23u);
            EXPECT_EQ(teamReader.ReadInt32(), expectedTeam);
            EXPECT_EQ(teamReader.SerializeInt(
                          SpawnRepl::kTeamInfoMaxHandle),
                      SpawnRepl::kReinforcementsRemaining);
            EXPECT_EQ(teamReader.ReadInt32(),
                      SpawnRepl::kUnlimitedReinforcementsDisplay);
            EXPECT_FALSE(teamReader.IsOverflowed());
            EXPECT_EQ(teamReader.BitPos(), teamBunch->payloadBits);
        }

        // The GRI's first bNetInitial property is h33 GameClass. Decode it from
        // the actual queued opening bunch rather than re-testing the resolver.
        BitReader griReader(gri->payload.data(), gri->payload.size(),
                            gri->payloadBits);
        (void)ActorRepl::ReadNetGUID(griReader);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        ActorRepl::ReadCompressedVector(griReader, x, y, z);
        EXPECT_EQ(griReader.SerializeInt(184u), 33u);
        const ActorRepl::NetGUIDRef gameClass =
            ActorRepl::ReadNetGUID(griReader);
        EXPECT_FALSE(griReader.IsOverflowed());
        EXPECT_FALSE(gameClass.isDynamic);
        EXPECT_EQ(gameClass.index, expected.territoriesGameClass);
    }
}

TEST(ConnectionTravelLifecycle,
     InstalledFullWorldReplayGateQueuesNoActorBunches) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30135, true, 0, false);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 1, "installed"));

    {
        ScopedEnvironmentVariable replayWorld(
            "RS2V_REPLAY_CAPTURE_WORLD", "1");
        // If the installed full-world gate regresses, force the deterministic
        // live path so this test cannot false-pass on a missing capture file.
        ScopedEnvironmentVariable liveReplication("RS2V_LIVE_REPL", "1");
        const char* active = std::getenv("RS2V_REPLAY_CAPTURE_WORLD");
        ASSERT_TRUE(active != nullptr);
        EXPECT_EQ(std::string(active), std::string("1"));
        ConnectionTravelLifecycleTestHarness::SendActorBootstrap(manager, 1);
    }

    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(0));
    EXPECT_TRUE(
        ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(manager, 1)
            .empty());
}

TEST(ConnectionTravelLifecycle,
     TravelTombstonesRemoteActorsAndBlocksOldWorldReopen) {
    ConnectionManager manager(nullptr);
    const ClientTravelRepl::EncodedRpc rpc = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(rpc));
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30141, true, 7, false);
    const ParticipantId bot = ParticipantId::Bot(21);
    const ParticipantId human = ParticipantId::Human(22);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::QueueRemotePri(
        manager, 1, bot));
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::QueueRemotePri(
        manager, 1, human, 2));
    ConnectionTravelLifecycleTestHarness::SeedRemotePawnView(
        manager, 1, bot);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::RemoteParticipantCount(
                  manager, 1),
              static_cast<size_t>(2));

    ASSERT_EQ(manager.BroadcastRetailClientTravel(rpc, "VNSK-Compound"),
              static_cast<size_t>(1));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::RemoteParticipantCount(
                  manager, 1),
              static_cast<size_t>(0));
    EXPECT_EQ(
        ConnectionTravelLifecycleTestHarness::RetiredRemoteParticipantCount(
            manager, 1),
        static_cast<size_t>(2));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::RemotePawnViewCount(
                  manager, 1),
              static_cast<size_t>(0));

    EXPECT_FALSE(ConnectionTravelLifecycleTestHarness::QueueRemotePri(
        manager, 1, ParticipantId::Bot(23)));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::RemoteParticipantCount(
                  manager, 1),
              static_cast<size_t>(0));
    EXPECT_EQ(
        ConnectionTravelLifecycleTestHarness::RetiredRemoteParticipantCount(
            manager, 1),
        static_cast<size_t>(2));
}

TEST(ConnectionTravelLifecycle,
     AdmittedNewEndpointSupersedesOnlySameAddressPendingIdentity) {
    ConnectionManager manager(nullptr);
    constexpr uint64_t steamId = 76561198012345678ull;
    const std::string steamText = std::to_string(steamId);

    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30201, true, 5, true, steamText);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 2, "127.0.0.1", 30202, true, 5, false, steamText);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 4, "192.0.2.44", 30204, true, 5, true, steamText);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 3, "127.0.0.1", 30203, false, 0, false);

    ConnectionTravelLifecycleTestHarness::RetireSuperseded(
        manager, 3, steamId);

    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30201),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30202), 2u);
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30203), 3u);
    // Steam64 is currently presented by the client, not verified by Steam.
    // A peer at another source address cannot evict a travelling session by
    // copying that public identifier.
    EXPECT_EQ(manager.FindClientByAddress("192.0.2.44", 30204), 4u);
}

TEST(ConnectionTravelLifecycle,
     ClientIdFallbackNeverMasqueradesAsPresentedTravelIdentity) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 42, "127.0.0.1", 30301, true, 5, true);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 43, "127.0.0.1", 30302, false, 0, false);

    ConnectionTravelLifecycleTestHarness::RetireSuperseded(manager, 43, 42);
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30301), 42u);
}

TEST(ConnectionTravelLifecycle,
     RejectedReconnectCannotRetirePendingReliableLedger) {
    ConnectionManager manager(nullptr);
    constexpr uint64_t steamId = 76561198012345678ull;
    const std::string steamText = std::to_string(steamId);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30311, true, 5, false, steamText);
    ConnectionTravelLifecycleTestHarness::SendPriorCh2Reliable(manager, 1);
    ConnectionTravelLifecycleTestHarness::SetTravelPending(manager, 1);
    const auto replacement =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 2, "127.0.0.1", 30312, false, 0, false, steamText);

    manager.SetClientLoggedInCallback(
        [replacement](const ClientLoggedInEvent&) {
            replacement->MarkDisconnected();
        });
    ClientLoggedInEvent event;
    event.clientId = 2;
    event.steamId = steamId;
    manager.FireClientLoggedIn(event);

    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30311), 1u);
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30312), 2u);
    EXPECT_TRUE(replacement->IsDisconnected());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(1));
}

TEST(ConnectionTravelLifecycle,
     TransportCapGrantsOnlyBoundedSameAddressReconnectHeadroom) {
    ConnectionManager manager(nullptr);
    manager.SetMaxClients(1);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30321, true, 5, true,
        "76561198012345678");

    const uint32_t replacement =
        manager.CreateOrGetClient("127.0.0.1", 30322);
    ASSERT_NE(replacement, std::numeric_limits<uint32_t>::max());
    EXPECT_TRUE(
        ConnectionTravelLifecycleTestHarness::HasTravelHeadroomLease(
            manager, replacement));
    EXPECT_EQ(manager.CreateOrGetClient("127.0.0.1", 30323),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(manager.CreateOrGetClient("192.0.2.55", 30324),
              std::numeric_limits<uint32_t>::max());

    ConnectionTravelLifecycleTestHarness::RetireSuperseded(
        manager, replacement, 76561198012345678ull);
    EXPECT_EQ(manager.FindClientByAddress("127.0.0.1", 30321),
              std::numeric_limits<uint32_t>::max());
    EXPECT_FALSE(
        ConnectionTravelLifecycleTestHarness::HasTravelHeadroomLease(
            manager, replacement));
}

TEST(ConnectionTravelLifecycle,
     PendingSessionsDoNotEnterANewDeploymentGeneration) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30401, true, 5, true);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 2, "127.0.0.1", 30402, true, 5, false);
    ConnectionTravelLifecycleTestHarness::SetRoleReady(manager, 1);
    ConnectionTravelLifecycleTestHarness::SetRoleReady(manager, 2);

    ConnectionTravelLifecycleTestHarness::BeginDeploymentGeneration(manager);

    EXPECT_FALSE(ConnectionTravelLifecycleTestHarness::DeploymentState(
                     manager, 1).has_value());
    const auto active = ConnectionTravelLifecycleTestHarness::DeploymentState(
        manager, 2);
    ASSERT_TRUE(active.has_value());
    EXPECT_TRUE(active->roleFinalized);
}

TEST(ConnectionTravelLifecycle,
     UnknownEndpointRejectsEveryNonHandshakeDatagramWithoutAllocating) {
    ConnectionManager manager(nullptr);
    const std::string ip = "192.0.2.40";
    constexpr uint16_t port = 31001;

    std::vector<std::vector<uint8_t>> rejected{
        {},
        {0xffu, 0x00u, 0x7fu},
        PureAckDatagram(),
        KeepAliveDatagram(),
        ActorDatagram(),
        ControlCloseDatagram(),
    };

    PacketCodec::Packet wrongPacketId = FreshHandshakeStartPacket();
    wrongPacketId.packetId = 1;
    rejected.push_back(EncodeClientPacket(wrongPacketId));

    PacketCodec::Packet startWithAck = FreshHandshakeStartPacket();
    startWithAck.acks.push_back(0);
    rejected.push_back(EncodeClientPacket(startWithAck));

    PacketCodec::Packet wrongSequence = FreshHandshakeStartPacket();
    wrongSequence.bunches.front().chSequence = 2;
    rejected.push_back(EncodeClientPacket(wrongSequence));

    PacketCodec::Packet shortPayload = FreshHandshakeStartPacket();
    shortPayload.bunches.front().payload = {
        ControlChannel::Handshake::kStart};
    shortPayload.bunches.front().payloadBits = 8;
    rejected.push_back(EncodeClientPacket(shortPayload));

    PacketCodec::Packet wrongSentinel = FreshHandshakeStartPacket();
    wrongSentinel.bunches.front().payload[1] = 0;
    rejected.push_back(EncodeClientPacket(wrongSentinel));

    PacketCodec::Packet notReliable = FreshHandshakeStartPacket();
    notReliable.bunches.front().bReliable = false;
    rejected.push_back(EncodeClientPacket(notReliable));

    for (const std::vector<uint8_t>& datagram : rejected) {
        ConnectionTravelLifecycleTestHarness::Deliver(
            manager, datagram, ip, port);
        EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
                  static_cast<size_t>(0));
        EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager),
                  1u);
        EXPECT_EQ(ConnectionTravelLifecycleTestHarness::HandshakeCount(manager),
                  static_cast<size_t>(0));
    }
}

TEST(ConnectionTravelLifecycle,
     ExactFreshHandshakeStartAdmitsUnknownEndpoint) {
    ConnectionManager manager(nullptr);
    const std::vector<uint8_t> start =
        EncodeClientPacket(FreshHandshakeStartPacket());

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, start, "192.0.2.41", 31002);

    EXPECT_EQ(manager.FindClientByAddress("192.0.2.41", 31002), 1u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(1));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 2u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::HandshakeCount(manager),
              static_cast<size_t>(1));
    const std::shared_ptr<ClientConnection> connection = manager.GetConnection(1);
    ASSERT_TRUE(connection != nullptr);
    EXPECT_TRUE(connection->IsUE3Client());
}

TEST(ConnectionTravelLifecycle,
     ActiveEndpointStillProcessesOrdinaryUE3Traffic) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> existing =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "192.0.2.42", 31003, false, 0, false);

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, PureAckDatagram(), "192.0.2.42", 31003);

    EXPECT_TRUE(manager.GetConnection(1) == existing);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(1));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 2u);
    // ParseIncomingControl reaches the existing endpoint and lazily creates its
    // handshake ledger; a pre-admission drop would leave this at zero.
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::HandshakeCount(manager),
              static_cast<size_t>(1));
}

TEST(ConnectionTravelLifecycle,
     LateTrafficCannotReviveDisconnectedEndpoint) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> disconnected =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "192.0.2.43", 31004, true, 0, false);
    disconnected->MarkDisconnected();

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, PureAckDatagram(), "192.0.2.43", 31004);

    EXPECT_TRUE(manager.GetConnection(1) == disconnected);
    EXPECT_TRUE(disconnected->IsDisconnected());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 2u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::HandshakeCount(manager),
              static_cast<size_t>(0));
}

TEST(ConnectionTravelLifecycle,
     FreshStartReplacesJoinedSessionOnReusedEndpoint) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> oldConnection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "192.0.2.44", 31005, true, 0, false);
    ConnectionTravelLifecycleTestHarness::SetTravelPending(
        manager, 1, 5000);
    ConnectionTravelLifecycleTestHarness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);
    ASSERT_EQ(
        ConnectionTravelLifecycleTestHarness::EvaluatePossessionRecovery(
            manager, 1, true, 1000u),
        ConnectionTravelLifecycleTestHarness::RecoveryDecision::Respond);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::SeedJoinedHandshake(
        manager, 1));

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, EncodeClientPacket(FreshHandshakeStartPacket()),
        "192.0.2.44", 31005);

    EXPECT_TRUE(oldConnection->IsDisconnected());
    EXPECT_EQ(manager.FindClientByAddress("192.0.2.44", 31005), 2u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(1));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 3u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::HandshakeCount(manager),
              static_cast<size_t>(1));
    EXPECT_FALSE(
        ConnectionTravelLifecycleTestHarness::TravelPending(manager, 2));
    EXPECT_EQ(
        ConnectionTravelLifecycleTestHarness::PossessionRecoveryResponses(
            manager, 2),
        0u);
    EXPECT_FALSE(
        ConnectionTravelLifecycleTestHarness::PossessionRecoveryLimitLogged(
            manager, 2));
}

TEST(ConnectionTravelLifecycle,
     NullEndpointEntryIsQuarantinedAndOnlyFreshStartCanReplaceIt) {
    ConnectionManager manager(nullptr);
    ConnectionTravelLifecycleTestHarness::AddNullClientEntry(
        manager, "192.0.2.45", 31006);

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, PureAckDatagram(), "192.0.2.45", 31006);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(0));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 1u);

    ConnectionTravelLifecycleTestHarness::AddNullClientEntry(
        manager, "192.0.2.45", 31006);
    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, EncodeClientPacket(FreshHandshakeStartPacket()),
        "192.0.2.45", 31006);

    EXPECT_EQ(manager.FindClientByAddress("192.0.2.45", 31006), 1u);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::ClientEntryCount(manager),
              static_cast<size_t>(1));
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 2u);
}

TEST(ConnectionTravelLifecycle,
     AdmissionGatePrecedesCapacityAndTravelHeadroomAllocation) {
    ConnectionManager manager(nullptr);
    manager.SetMaxClients(1);
    ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "192.0.2.46", 31007, true, 5, true,
        "76561198012345678");

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, PureAckDatagram(), "192.0.2.46", 31008);
    EXPECT_EQ(manager.FindClientByAddress("192.0.2.46", 31008),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 2u);

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, EncodeClientPacket(FreshHandshakeStartPacket()),
        "192.0.2.46", 31008);
    EXPECT_EQ(manager.FindClientByAddress("192.0.2.46", 31008), 2u);
    EXPECT_TRUE(ConnectionTravelLifecycleTestHarness::HasTravelHeadroomLease(
        manager, 2));

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, EncodeClientPacket(FreshHandshakeStartPacket()),
        "198.51.100.46", 31009);
    EXPECT_EQ(manager.FindClientByAddress("198.51.100.46", 31009),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::NextClientId(manager), 3u);
}

RS2V_TEST_MAIN()
