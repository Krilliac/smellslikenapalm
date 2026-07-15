#include "TestFramework.h"

#include "Network/ClientTravelReplication.h"
#include "Network/ActorReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/ConnectionManager.h"
#include "Network/ControlChannel.h"
#include "Network/ObjectiveReplication.h"
#include "Network/PacketCodec.h"
#include "Network/SpawnReplication.h"
#include "Network/RoleSelectionReplication.h"
#include "Network/SocketFactory.h"
#include "Network/UDPSocket.h"
#include "Game/GameServer.h"
#include "Game/BotManager.h"
#include "Game/ObjectiveSystem.h"
#include "Game/PlayerManager.h"
#include "Game/RoleSystem.h"
#include "Game/SpawnSystem.h"
#include "Game/SupremacyMode.h"
#include "Game/TeamManager.h"
#include "Game/TeamMapping.h"
#include "Game/TerritoryMode.h"
#include "Game/TicketSystem.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

class ConnectionTravelLifecycleTestHarness {
public:
    static std::shared_ptr<ClientConnection> AddClient(
        ConnectionManager& manager, uint32_t clientId,
        const std::string& ip, uint16_t port, bool joined,
        uint32_t ch2Reliable, bool travelPending,
        const std::string& presentedSteamId = {},
        std::shared_ptr<UDPSocket> socket = nullptr) {
        auto connection = std::make_shared<ClientConnection>(
            clientId, ip, port, std::move(socket), &manager);
        connection->SetUE3Client(true);
        connection->SetHandshakeComplete(joined);
        if (!presentedSteamId.empty()) {
            connection->SetPresentedSteamID(presentedSteamId);
        }
        manager.m_clients.emplace(ClientAddress{ip, port}, connection);
        auto& state = manager.m_controlState[clientId];
        if (ch2Reliable != 0u) {
            (void)state.ch2Reliable.Seed(ch2Reliable);
        }
        state.mapTravelPending = travelPending;
        state.outboundActorChannels.set(2u, ch2Reliable != 0u);
        manager.m_nextClientId = std::max(manager.m_nextClientId, clientId + 1u);
        return connection;
    }

    static void InstallRoleSelectionRuntime(GameServer& server) {
        server.m_playerManager = std::make_unique<PlayerManager>(&server);
        server.m_playerManager->Initialize();
        server.m_teamManager = std::make_unique<TeamManager>(&server);
        server.m_teamManager->Initialize();
        server.m_roleSystem = std::make_unique<RoleSystem>(&server);
        server.m_roleSystem->Initialize();
        server.m_roleSystem->ConfigureRetailSquads("Territories", 64);
        server.m_roleSystem->SetTeamFaction(1u, Faction::USArmy);
        server.m_roleSystem->SetTeamFaction(2u, Faction::NLFSV);
        server.m_spawnSystem = std::make_unique<SpawnSystem>(&server);
        server.m_spawnSystem->Initialize();

        SpawnLocation south;
        south.type = SpawnType::BaseSpawn;
        south.name = "Cu Chi South base";
        south.teamId = 1u;
        south.retailSpawnVolumeRef = 299107u;
        (void)server.m_spawnSystem->AddSpawnLocation(south);

        SpawnLocation north = south;
        north.name = "Cu Chi North base";
        north.teamId = 2u;
        north.retailSpawnVolumeRef = 299103u;
        (void)server.m_spawnSystem->AddSpawnLocation(north);
    }

    static void ResetRoleSelectionRuntime(GameServer& server) {
        server.m_territoryMode.reset();
        server.m_supremacyMode.reset();
        server.m_ticketSystem.reset();
        server.m_objectiveSystem.reset();
        server.m_botManager.reset();
        server.m_spawnSystem.reset();
        server.m_roleSystem.reset();
        // PlayerManager shutdown consults TeamManager, so preserve that order.
        server.m_playerManager.reset();
        server.m_teamManager.reset();
    }

    static void InstallProductionBotFill(GameServer& server) {
        server.InitializeBotsForCurrentMap();
    }

    static SupremacyMode* InstallActiveSupremacyMode(GameServer& server) {
        server.m_supremacyMode = std::make_unique<SupremacyMode>(&server);
        server.m_supremacyMode->Initialize();
        server.m_supremacyMode->StartRound();
        server.m_supremacyMode->Update(
            server.m_supremacyMode->GetPhaseTimeRemaining());
        return server.m_supremacyMode.get();
    }

    static void InstallTerritoryObjective(GameServer& server) {
        server.m_objectiveSystem =
            std::make_unique<ObjectiveSystem>(&server);
        server.m_objectiveSystem->Initialize();
        CaptureZone objective;
        objective.name = "Cu Chi deployment phase";
        objective.type = ObjectiveType::Territory;
        objective.territoryOrder = 0;
        const uint32_t objectiveId =
            server.m_objectiveSystem->AddObjective(objective);
        server.m_objectiveSystem->SetTerritoryOrder({objectiveId});
    }

    static TerritoryMode* InstallActiveTerritoryMode(
        GameServer& server, float roundSeconds = 120.0f) {
        InstallTerritoryObjective(server);
        server.m_territoryMode = std::make_unique<TerritoryMode>(&server);
        server.m_territoryMode->Initialize();
        server.m_territoryMode->SetRoundTime(roundSeconds);
        server.m_territoryMode->SetPreparationTime(0.0f);
        server.m_territoryMode->StartRound();
        server.m_territoryMode->Update(0.0f);
        return server.m_territoryMode.get();
    }

    static TerritoryMode* InstallPreparationTerritoryMode(
        GameServer& server, float preparationSeconds) {
        InstallTerritoryObjective(server);
        server.m_territoryMode = std::make_unique<TerritoryMode>(&server);
        server.m_territoryMode->Initialize();
        server.m_territoryMode->SetPreparationTime(preparationSeconds);
        server.m_territoryMode->StartRound();
        return server.m_territoryMode.get();
    }

    static TicketSystem* InstallTicketSystem(
        GameServer& server, uint32_t southTickets,
        uint32_t northTickets) {
        server.m_ticketSystem = std::make_unique<TicketSystem>(&server);
        server.m_ticketSystem->Initialize(
            southTickets, northTickets);
        return server.m_ticketSystem.get();
    }

    static bool AttachRoleSelectionPlayer(
        GameServer& server, const std::shared_ptr<ClientConnection>& connection,
        uint32_t teamId) {
        if (!connection || !server.m_playerManager || !server.m_teamManager) {
            return false;
        }
        connection->SetPlayerName("CuChiRoleTest");
        server.m_playerManager->OnPlayerConnect(connection);
        server.m_teamManager->AddPlayerToTeam(connection->GetClientId(), teamId);
        return server.m_teamManager->GetPlayerTeam(connection->GetClientId()) ==
               teamId;
    }

    static bool FreezeCanonicalCuChiSession(ConnectionManager& manager,
                                            uint32_t clientId) {
        std::string error;
        const auto artifact =
            RetailBootstrap::ResolveArtifactSelection({}, error);
        const auto profile = RetailBootstrap::ResolveExactProfile(
            "VNTE-CuChi", "Territories");
        if (!artifact || !profile) return false;

        auto& state = manager.m_controlState.at(clientId);
        state.retailArtifactSelectionResolved = true;
        state.retailArtifactSelection = artifact;
        state.retailBootstrapProfile = profile;
        state.teamSelected = true;
        state.teamInfoChannels = {4u, 5u};
        return true;
    }

    static bool FreezeInstalledCuChiSession(ConnectionManager& manager,
                                            uint32_t clientId) {
        std::string error;
        const auto artifact =
            RetailBootstrap::ResolveArtifactSelection("installed", error);
        const auto profile = RetailBootstrap::ResolveExactProfile(
            "VNTE-CuChi", "Territories");
        if (!artifact || !profile) return false;

        auto& state = manager.m_controlState.at(clientId);
        state.retailArtifactSelectionResolved = true;
        state.retailArtifactSelection = artifact;
        state.retailBootstrapProfile = profile;
        state.teamSelected = true;
        state.teamInfoChannels = {4u, 5u};
        return true;
    }

    struct RoleLedgerSnapshot {
        bool accepted = false;
        bool finalized = false;
        bool priClassReplicated = false;
        uint32_t roleInfoObjectRef = 0;
        uint8_t classIndex = 255;
        uint8_t squadIndex = 255;
        uint8_t roleIndex = 255;
        std::optional<RoleSelectionRepl::ChangedRoleEvidence> changedRole;
    };

    static RoleLedgerSnapshot RoleLedger(const ConnectionManager& manager,
                                         uint32_t clientId) {
        const auto& state = manager.m_controlState.at(clientId);
        return RoleLedgerSnapshot{
            state.roleSelectionAccepted,
            state.roleFinalized,
            state.roleClassReplicated,
            state.selectedRoleInfoObjectRef,
            state.selectedRoleClassIndex,
            state.selectedRoleSquadIndex,
            state.selectedRoleIndex,
            state.selectedChangedRole,
        };
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

    static void InstallThrowingControlReassembler(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.reassembler =
            std::make_unique<PacketCodec::ControlReassembler>(
                [](const std::vector<uint8_t>&) {
                    throw std::runtime_error("test control dispatch failure");
                });
    }

    static bool ParseIncomingControl(
        ConnectionManager& manager, uint32_t clientId,
        const std::vector<uint8_t>& datagram) {
        return manager.ParseIncomingControl(clientId, datagram);
    }

    static void SetOwningPawnGraphCompletionDeferred(
        ConnectionManager& manager, uint32_t clientId) {
        manager.m_controlState.at(clientId)
            .owningPawnGraphCompletionDeferred = true;
    }

    static bool InboundPacketDispatchActive(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .inboundPacketDispatchActive;
    }

    static bool OwningPawnGraphCompletionDeferred(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .owningPawnGraphCompletionDeferred;
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
        StaleGeneration,
        Backpressured,
        RateLimited,
        LimitReached,
        Suppressed,
    };

    static void SetPossessionRecoveryEligibility(
        ConnectionManager& manager, uint32_t clientId, bool spawned,
        bool pawnGraphOpen, bool possessionAcked,
        uint64_t pawnGeneration = 1u) {
        auto& state = manager.m_controlState.at(clientId);
        state.spawned = spawned;
        state.pawnGraphOpen = pawnGraphOpen;
        state.owningPawnAlive = spawned;
        state.owningPawnGeneration = pawnGeneration;
        state.pawnGraphGeneration = pawnGraphOpen ? pawnGeneration : 0u;
        state.possessionRecoveryGeneration =
            spawned && pawnGraphOpen ? pawnGeneration : 0u;
        state.possessionAckedGeneration =
            possessionAcked ? pawnGeneration : 0u;
    }

    static RecoveryDecision EvaluatePossessionRecovery(
        ConnectionManager& manager, uint32_t clientId,
        bool exactStandaloneRequest, uint64_t nowMs) {
        auto& state = manager.m_controlState.at(clientId);
        const uint64_t pawnGeneration = state.owningPawnGeneration;
        const auto decision = ConnectionManager::EvaluatePossessionRecovery(
            state, exactStandaloneRequest, pawnGeneration, nowMs);
        if (decision ==
            ConnectionManager::PossessionRecoveryDecision::Respond) {
            (void)ConnectionManager::CommitPossessionRecoveryResponse(
                state, pawnGeneration, nowMs);
        }
        return static_cast<RecoveryDecision>(static_cast<uint8_t>(decision));
    }

    static std::vector<uint32_t> ReservePublishedCh2Reliables(
        ConnectionManager& manager, uint32_t clientId, size_t count) {
        auto& sequencer = manager.m_controlState.at(clientId).ch2Reliable;
        auto reservation = sequencer.ReserveBatch(count);
        if (!reservation) return {};
        if (!sequencer.CommitBatch(*reservation)) return {};
        return reservation->SequenceValues();
    }

    static bool ReleaseCh2Reliable(ConnectionManager& manager,
                                   uint32_t clientId,
                                   uint32_t sequence) {
        return manager.m_controlState.at(clientId)
            .ch2Reliable.Release(sequence).has_value();
    }

    static void SetPawnGenerationBindings(
        ConnectionManager& manager, uint32_t clientId,
        uint64_t owningGeneration, uint64_t graphGeneration,
        uint64_t recoveryGeneration, bool alive) {
        auto& state = manager.m_controlState.at(clientId);
        state.spawned = true;
        state.pawnGraphOpen = true;
        state.owningPawnAlive = alive;
        state.owningPawnGeneration = owningGeneration;
        state.pawnGraphGeneration = graphGeneration;
        state.possessionRecoveryGeneration = recoveryGeneration;
        state.possessionAckedGeneration = 0u;
        ConnectionManager::ResetPossessionRecovery(state);
    }

    static uint64_t AdvanceAndBindLivePawnGeneration(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.spawned = true;
        state.pawnGraphOpen = true;
        state.owningPawnAlive = true;
        const uint64_t generation =
            ConnectionManager::AdvanceOwningPawnGeneration(state);
        ConnectionManager::BindPossessionRecovery(state, generation);
        return generation;
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
        const auto& state = manager.m_controlState.at(clientId);
        return state.owningPawnGeneration != 0u &&
               state.possessionAckedGeneration ==
                   state.owningPawnGeneration;
    }

    static bool TeamSelected(const ConnectionManager& manager,
                             uint32_t clientId) {
        return manager.m_controlState.at(clientId).teamSelected;
    }

    static bool Spawned(const ConnectionManager& manager,
                        uint32_t clientId) {
        return manager.m_controlState.at(clientId).spawned;
    }

    enum class OwningGraphPhase : uint8_t {
        Unopened,
        Open,
        Closing,
        Closed,
        Broken,
    };

    static OwningGraphPhase PawnGraphPhase(
        const ConnectionManager& manager, uint32_t clientId) {
        return static_cast<OwningGraphPhase>(static_cast<uint8_t>(
            manager.m_controlState.at(clientId).pawnGraphPhase));
    }

    static uint32_t PawnGraphTeam(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId).pawnGraphTeamId;
    }

    static uint32_t NextOwningGraphReliable(
        const ConnectionManager& manager, uint32_t clientId,
        uint32_t channel) {
        const auto* sequencer = ConnectionManager::OwningPawnGraphSequencer(
            manager.m_controlState.at(clientId), channel);
        return sequencer
            ? sequencer->NextSequence().value_or(
                  PacketCodec::kMaxChSequence)
            : PacketCodec::kMaxChSequence;
    }

    static std::vector<uint32_t> PendingOwningGraphSequences(
        const ConnectionManager& manager, uint32_t clientId,
        uint32_t channel, bool openOnly = false, bool closeOnly = false) {
        std::vector<uint32_t> sequences;
        for (const auto& pending :
             manager.m_controlState.at(clientId).pendingReliable) {
            for (const PacketCodec::Bunch& bunch : pending.bunches) {
                if (!bunch.bReliable || bunch.chIndex != channel ||
                    (openOnly && !bunch.bOpen) ||
                    (closeOnly && !bunch.bClose)) {
                    continue;
                }
                sequences.push_back(bunch.chSequence);
            }
        }
        return sequences;
    }

    static std::optional<uint32_t> PendingOwningGraphPacket(
        const ConnectionManager& manager, uint32_t clientId,
        uint32_t channel, bool open, bool close) {
        for (const auto& pending :
             manager.m_controlState.at(clientId).pendingReliable) {
            const bool matches = std::any_of(
                pending.bunches.begin(), pending.bunches.end(),
                [channel, open, close](const PacketCodec::Bunch& bunch) {
                    return bunch.bReliable && bunch.chIndex == channel &&
                           (!open || bunch.bOpen) &&
                           (!close || bunch.bClose);
                });
            if (matches && !pending.packetSerials.empty()) {
                return static_cast<uint32_t>(
                    pending.packetSerials.front() %
                    static_cast<int64_t>(kMaxPacketId));
            }
        }
        return std::nullopt;
    }

    static void PrepareOwningPawnLifeForFactionSwitch(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.spawned = false;
        state.owningPawnAlive = false;
        state.deferredOwningPawnGraphDeployment.reset();
        ConnectionManager::InvalidatePossessionRecovery(state);
        (void)ConnectionManager::AdvanceOwningPawnGeneration(state);
        state.owningPawnAlive = true;
    }

    static void ModelOwningPawnSpawnCallback(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.owningPawnAlive = false;
        (void)ConnectionManager::AdvanceOwningPawnGeneration(state);
        state.owningPawnAlive = true;
    }

    static std::optional<uint32_t> PrepareDeploymentForCurrentTeam(
        ConnectionManager& manager, uint32_t clientId) {
        const std::vector<uint32_t> available =
            manager.GetAvailableSpawnIds(clientId);
        if (available.empty()) return std::nullopt;
        manager.m_deploymentCoordinator.ResetClient(clientId);
        manager.m_deploymentCoordinator.FinalizeRole(clientId);
        if (manager.m_deploymentCoordinator.SelectSpawn(
                clientId, 128u, available) !=
            DeploymentCoordinator::SelectionResult::Accepted) {
            return std::nullopt;
        }
        const auto ready = manager.m_deploymentCoordinator.SetReadyStatus(
            clientId, DeploymentCoordinator::ReadyStatus::Ready,
            available);
        return ready.IsNewAuthorization() ? ready.spawnId : std::nullopt;
    }

    static bool ExecutePreparedDeployment(ConnectionManager& manager,
                                           uint32_t clientId,
                                           uint32_t spawnId) {
        return manager.ExecutePreparedDeployment(clientId, spawnId);
    }

    static std::optional<int32_t> RetailRemainingSecond(
        float remainingSeconds) {
        return ConnectionManager::RetailRemainingSecond(remainingSeconds);
    }

    static std::optional<int32_t> CalculateActiveDeploymentDeadline(
        int32_t remainingSeconds, uint32_t serverTeamId) {
        return ConnectionManager::CalculateActiveDeploymentDeadline(
            remainingSeconds, serverTeamId);
    }

    static std::optional<int32_t> RebaseActiveDeploymentDeadline(
        int32_t previousRemainingSeconds,
        int32_t previousDeadlineRemainingSeconds,
        int32_t currentRemainingSeconds) {
        return ConnectionManager::RebaseActiveDeploymentDeadline(
            previousRemainingSeconds, previousDeadlineRemainingSeconds,
            currentRemainingSeconds);
    }

    static bool HasReachedActiveDeploymentDeadline(
        int32_t remainingSeconds, int32_t deadlineRemainingSeconds) {
        return ConnectionManager::HasReachedActiveDeploymentDeadline(
            remainingSeconds, deadlineRemainingSeconds);
    }

    static std::optional<int32_t> ActiveDeploymentDeadline(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .activeDeploymentDeadlineRemainingSeconds;
    }

    static std::optional<int32_t> PublishedNextRespawnTime(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId).publishedNextRespawnTime;
    }

    static bool ActiveDeploymentPolicyIsClosed(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.GetActiveDeploymentPolicy(clientId) ==
            ConnectionManager::ActiveDeploymentPolicy::Closed;
    }

    static void UpdateRetailDeploymentCountdown(ConnectionManager& manager) {
        manager.UpdateRetailDeploymentCountdown();
    }

    static void SynchronizeRetailTeamReinforcements(
        ConnectionManager& manager) {
        manager.SynchronizeRetailTeamReinforcements();
    }

    static void MarkTeamInfoChannelsOpen(ConnectionManager& manager,
                                         uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        for (const uint32_t channel : state.teamInfoChannels) {
            if (channel != 0u && channel < state.outboundActorChannels.size()) {
                state.outboundActorChannels.set(channel);
            }
        }
    }

    static void SeedPendingTeamInfoOpen(ConnectionManager& manager,
                                        uint32_t clientId,
                                        uint32_t channel) {
        ConnectionManager::ControlState::SentReliable pending;
        pending.packetSerials.push_back(900);
        PacketCodec::Bunch open;
        open.bOpen = true;
        open.bReliable = true;
        open.chIndex = channel;
        open.chType = 2u;
        open.chSequence = 1u;
        pending.bunches.push_back(std::move(open));
        manager.m_controlState.at(clientId).pendingReliable.push_back(
            std::move(pending));
    }

    static void ClearPendingReliables(ConnectionManager& manager,
                                      uint32_t clientId) {
        manager.m_controlState.at(clientId).pendingReliable.clear();
    }

    static void UseCapturedTeamInfoChannels(ConnectionManager& manager,
                                            uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        state.teamInfoChannels = {76u, 56u};
        state.publishedTeamReinforcements.fill(std::nullopt);
    }

    static std::array<std::optional<int32_t>, 2>
    PublishedTeamReinforcements(const ConnectionManager& manager,
                                uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .publishedTeamReinforcements;
    }

    static std::array<std::optional<int32_t>, 2>
    PendingTeamReinforcements(const ConnectionManager& manager,
                              uint32_t clientId) {
        std::array<std::optional<int32_t>, 2> values{};
        const auto& pending = manager.m_controlState.at(clientId)
                                  .pendingTeamReinforcements;
        for (size_t team = 0; team < pending.size(); ++team) {
            if (pending[team]) values[team] = pending[team]->wireValue;
        }
        return values;
    }

    static void ExpireTeamReinforcementRetry(ConnectionManager& manager,
                                             uint32_t clientId,
                                             uint8_t retailTeam) {
        auto& pending = manager.m_controlState.at(clientId)
                            .pendingTeamReinforcements.at(retailTeam);
        if (pending) pending->lastSendMs = 0u;
    }

    static uint64_t OwningPawnGeneration(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId).owningPawnGeneration;
    }

    static bool OwningPawnAlive(const ConnectionManager& manager,
                                uint32_t clientId) {
        return manager.m_controlState.at(clientId).owningPawnAlive;
    }

    static bool HasDeferredOwningPawnDeployment(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .deferredOwningPawnGraphDeployment.has_value();
    }

    static bool DeferredOwningPawnHasRoundStartAuthorization(
        const ConnectionManager& manager, uint32_t clientId) {
        const auto& deferred = manager.m_controlState.at(clientId)
            .deferredOwningPawnGraphDeployment;
        return deferred.has_value() && deferred->roundStartAuthorization;
    }

    static void UseCanonicalResortProfile(
        ConnectionManager& manager, uint32_t clientId) {
        manager.m_controlState.at(clientId).retailBootstrapProfile =
            RetailBootstrap::CanonicalProfile();
    }

    static uint32_t NextCh2Reliable(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .ch2Reliable.NextSequence()
            .value_or(PacketCodec::kMaxChSequence);
    }

    static size_t Ch2OutstandingCount(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .ch2Reliable.OutstandingCount();
    }

    static std::vector<uint32_t> QueuedCh2ReliableSequences(
        const ConnectionManager& manager, uint32_t clientId) {
        std::vector<uint32_t> sequences;
        for (const auto& pending :
             manager.m_controlState.at(clientId).pendingReliable) {
            for (const PacketCodec::Bunch& bunch : pending.bunches) {
                if (bunch.bReliable && bunch.chIndex == 2u) {
                    sequences.push_back(bunch.chSequence);
                }
            }
        }
        return sequences;
    }

    static uint32_t LocalPawnChannel() {
        return ConnectionManager::kLocalPawnChannel;
    }

    static void BeginDeploymentGeneration(ConnectionManager& manager) {
        manager.BeginDeploymentGeneration();
    }

    static void ResetDetachedDeploymentClient(ConnectionManager& manager,
                                              uint32_t clientId) {
        // Handler-only fixtures have no RoleSystem evidence from which the
        // production round-generation path can republish ChangedRole. Reset
        // just the coordinator so these tests isolate h261/h434 wire order.
        manager.m_deploymentCoordinator.ResetClient(clientId);
        manager.m_deploymentCoordinator.FinalizeRole(clientId);
    }

    static std::optional<DeploymentCoordinator::ClientStateSnapshot>
    DeploymentState(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_deploymentCoordinator.GetClientState(clientId);
    }

    static bool DeploymentPrepared(const ConnectionManager& manager,
                                   uint32_t clientId) {
        return manager.m_deploymentCoordinator.IsPreparedForDeployment(
            clientId);
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

    static bool ResolveObjectiveConnectedToBase(
        bool authoredConnectedToBase, const SupremacyMode* supremacy,
        uint32_t objectiveId, uint32_t controllingTeam) {
        return ConnectionManager::ResolveObjectiveConnectedToBase(
            authoredConnectedToBase, supremacy, objectiveId, controllingTeam);
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

    static bool SendPriorCh2Reliable(ConnectionManager& manager,
                                     uint32_t clientId) {
        return manager.SendCh2Rpc(
            clientId, {0x01u}, 1u, "PriorReliableForTest");
    }

    static bool SendControlReliable(ConnectionManager& manager,
                                    uint32_t clientId,
                                    const std::vector<uint8_t>& payload) {
        return manager.SendRawToClient(clientId, payload);
    }

    static size_t DeferredControlMessageCount(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .deferredControlMessages.size();
    }

    static size_t DeferredControlBytes(const ConnectionManager& manager,
                                       uint32_t clientId) {
        return manager.m_controlState.at(clientId).deferredControlBytes;
    }

    static bool JoinCompletionDeferred(const ConnectionManager& manager,
                                       uint32_t clientId) {
        return manager.m_controlState.at(clientId).joinCompletionDeferred;
    }

    static bool OutboundActorChannelOpen(const ConnectionManager& manager,
                                         uint32_t clientId,
                                         uint32_t channel) {
        const auto& channels =
            manager.m_controlState.at(clientId).outboundActorChannels;
        return channel < channels.size() && channels.test(channel);
    }

    static std::vector<uint32_t> PendingReliablePacketIds(
        const ConnectionManager& manager, uint32_t clientId) {
        std::vector<uint32_t> packetIds;
        for (const auto& pending :
             manager.m_controlState.at(clientId).pendingReliable) {
            if (pending.packetSerials.empty()) return {};
            packetIds.push_back(static_cast<uint32_t>(
                pending.packetSerials.front() %
                static_cast<int64_t>(kMaxPacketId)));
        }
        return packetIds;
    }

    static uint32_t LastPendingReliableSequence(
        const ConnectionManager& manager, uint32_t clientId) {
        const auto& pending =
            manager.m_controlState.at(clientId).pendingReliable.back();
        return pending.bunches.front().chSequence;
    }

    static std::vector<uint8_t> LastPendingReliablePayload(
        const ConnectionManager& manager, uint32_t clientId) {
        const auto& pending =
            manager.m_controlState.at(clientId).pendingReliable.back();
        return pending.bunches.front().payload;
    }

    static void AllocateAndResolveUntrackedOutboundPacket(
        ConnectionManager& manager, uint32_t clientId) {
        auto& outbound = manager.m_controlState.at(clientId).outbound;
        const auto built = outbound.BuildAckOnlyPacket();
        ASSERT_TRUE(built.has_value());
        const auto resolved = outbound.ResolveOutboundAck(built->packetId);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*resolved, built->outboundPacketSerial);
    }

    static void AllocateLocallyRetiredBunchlessPacket(
        ConnectionManager& manager, uint32_t clientId) {
        auto& outbound = manager.m_controlState.at(clientId).outbound;
        const auto built = outbound.BuildAckOnlyPacket();
        ASSERT_TRUE(built.has_value());
    }

    static bool ConsumeOutboundPacketCapacityLeavingOneSlot(
        ConnectionManager& manager, uint32_t clientId) {
        auto& outbound = manager.m_controlState.at(clientId).outbound;
        constexpr int64_t halfRange =
            static_cast<int64_t>(kMaxPacketId) / 2;
        PacketCodec::Bunch untracked;
        untracked.chIndex = 2u;

        while (outbound.NextPacketSerial() -
                   outbound.AckUnwrapReferenceSerial() <
               halfRange - 1) {
            if (!outbound.BuildRawBunchPacket(untracked)) return false;
        }
        return outbound.HasPacketIdCapacity() &&
               outbound.NextPacketSerial() -
                       outbound.AckUnwrapReferenceSerial() ==
                   halfRange - 1;
    }

    static bool EnsureBunchlessAckReferenceSafe(
        ConnectionManager& manager, uint32_t clientId) {
        return manager.EnsureBunchlessAckReferenceSafe(
            clientId, "lifecycle regression test");
    }

    static bool ReleaseFirstPendingCh0SequenceOutOfBand(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        if (state.pendingReliable.empty() ||
            state.pendingReliable.front().bunches.empty()) {
            return false;
        }
        return state.outbound.AcknowledgeControlSequence(
            state.pendingReliable.front().bunches.front().chSequence)
            .has_value();
    }

    static uint32_t RetransmitFirstPendingReliableForTest(
        ConnectionManager& manager, uint32_t clientId) {
        auto& state = manager.m_controlState.at(clientId);
        auto& pending = state.pendingReliable.front();
        const auto built =
            state.outbound.BuildRawBunchesPacket(pending.bunches);
        if (!built) return kMaxPacketId;
        pending.packetSerials.push_back(built->outboundPacketSerial);
        return built->packetId;
    }

    static void ExpireAndRetransmitFirstPendingReliable(
        ConnectionManager& manager, uint32_t clientId) {
        auto& pending =
            manager.m_controlState.at(clientId).pendingReliable.front();
        pending.lastSendMs = 0u;
        pending.retryDelayMs = 0u;
        manager.RetransmitTick();
    }

    static std::vector<int64_t> FirstPendingPacketSerials(
        const ConnectionManager& manager, uint32_t clientId) {
        return manager.m_controlState.at(clientId)
            .pendingReliable.front()
            .packetSerials;
    }

    static uint32_t FirstPendingPacketId(const ConnectionManager& manager,
                                         uint32_t clientId) {
        return static_cast<uint32_t>(
            manager.m_controlState.at(clientId)
                .pendingReliable.front().packetSerials.front() %
            static_cast<int64_t>(kMaxPacketId));
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

    static void AcknowledgePackets(
        ConnectionManager& manager, uint32_t clientId,
        const std::vector<PacketCodec::Packet>& packets) {
        for (const PacketCodec::Packet& packet : packets) {
            manager.OnClientAck(clientId, packet.packetId);
        }
    }

    static void AcknowledgeAllPendingReliables(
        ConnectionManager& manager, uint32_t clientId) {
        auto& pending = manager.m_controlState.at(clientId).pendingReliable;
        while (!pending.empty() && !pending.front().packetSerials.empty()) {
            manager.OnClientAck(
                clientId,
                static_cast<uint32_t>(
                    pending.front().packetSerials.front() %
                    static_cast<int64_t>(kMaxPacketId)));
        }
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

    static void SynchronizeRemoteParticipantPris(
        ConnectionManager& manager, uint32_t viewerClientId) {
        manager.SynchronizeRemoteParticipantPris(viewerClientId);
    }

    static std::optional<uint32_t> RemotePawnChannel(
        const ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        const auto* binding = manager.m_controlState.at(viewerClientId)
                                  .remoteParticipants.Find(participant);
        if (!binding) return std::nullopt;
        return binding->pawnChannel;
    }

    static std::optional<uint32_t> RemotePriTeamInfoChannel(
        const ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        const auto* binding = manager.m_controlState.at(viewerClientId)
                                  .remoteParticipants.Find(participant);
        if (!binding || !binding->priTeamWireValid) return std::nullopt;
        return binding->priTeamInfoChannel;
    }

    static std::optional<uint32_t> RemotePawnServerTeam(
        const ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        const auto* binding = manager.m_controlState.at(viewerClientId)
                                  .remoteParticipants.Find(participant);
        if (!binding || !binding->pawnServerTeamValid) return std::nullopt;
        return binding->pawnServerTeamId;
    }

    static std::optional<uint32_t> SeedRemotePawnBindingOpen(
        ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant, uint32_t serverTeamId) {
        auto& remotes = manager.m_controlState.at(viewerClientId)
                            .remoteParticipants;
        ParticipantActorChannelBinding* binding = remotes.Find(participant);
        if (!binding || binding->priState != ParticipantActorOpenState::Open ||
            binding->pawnState != ParticipantActorOpenState::Unopened) {
            return std::nullopt;
        }
        const std::optional<uint32_t> openingSequence =
            remotes.NextPawnReliableSequence(participant);
        if (!openingSequence) return std::nullopt;
        const uint32_t generation = binding->pawnGeneration;
        if (!remotes.MarkPawnOpen(participant, generation)) return std::nullopt;
        binding = remotes.Find(participant);
        if (!binding) return std::nullopt;
        binding->pawnServerTeamValid = true;
        binding->pawnServerTeamId = serverTeamId;
        return openingSequence;
    }

    static uint32_t NextInboundActorReliable(
        const ConnectionManager& manager, uint32_t clientId,
        uint32_t channel) {
        return manager.m_controlState.at(clientId)
            .actorReliableInbound.NextSequence(channel);
    }

    static bool MantlePawnStarted(const ConnectionManager& manager,
                                  uint32_t clientId) {
        return manager.m_controlState.at(clientId).mantlePawnStarted;
    }

    static bool WeaponFiring(const ConnectionManager& manager,
                             uint32_t clientId, uint32_t channel) {
        if (channel < 210u || channel > 214u) return false;
        return manager.m_controlState.at(clientId)
            .weaponIntent[channel - 210u].firing;
    }

    static uint32_t ActiveWeaponChannel(const ConnectionManager& manager,
                                        uint32_t clientId) {
        return manager.m_controlState.at(clientId).activeWeaponChannel;
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

    static std::optional<PacketCodec::Bunch> QueueAuthoritativeRemotePri(
        ConnectionManager& manager, uint32_t viewerClientId,
        const ParticipantId& participant) {
        DeploymentRepl::RetailParticipantInitialState initial;
        if (!manager.BuildRemoteParticipantInitialState(
                participant, nullptr, initial)) {
            return std::nullopt;
        }
        std::vector<PacketCodec::Bunch> output;
        if (manager.QueueRemoteParticipantPriOpen(
                viewerClientId, initial, output) !=
            ConnectionManager::RemotePriOpenResult::OpenQueued) {
            return std::nullopt;
        }
        const auto open = std::find_if(
            output.begin(), output.end(),
            [](const PacketCodec::Bunch& bunch) {
                return bunch.bOpen && !bunch.bClose;
            });
        return open == output.end()
            ? std::nullopt
            : std::optional<PacketCodec::Bunch>(*open);
    }

    static bool FreezeRetailBootstrap(
        ConnectionManager& manager, uint32_t clientId,
        std::string_view variant,
        std::string_view mapUrl = "VNTE-Resort",
        std::string_view mode = "Territories") {
        std::string error;
        const auto selection = RetailBootstrap::ResolveArtifactSelection(
            variant, error);
        const auto profile = RetailBootstrap::ResolveExactProfile(mapUrl, mode);
        if (!selection || !profile) return false;

        auto& state = manager.m_controlState.at(clientId);
        state.retailArtifactSelectionResolved = true;
        state.retailArtifactSelection = selection;
        state.retailBootstrapProfile = profile;
        return true;
    }

    static bool ClearLiveTeamInfoClassRef(ConnectionManager& manager,
                                          uint32_t clientId) {
        auto& selection =
            manager.m_controlState.at(clientId).retailArtifactSelection;
        if (!selection) return false;
        selection->roGame.teamInfoClassRef = 0u;
        return true;
    }

    static uint32_t GriChannel(const ConnectionManager& manager,
                               uint32_t clientId) {
        return manager.m_controlState.at(clientId).griChannel;
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

PacketCodec::Bunch MakePossessionAckBunch(uint32_t pawnChannel,
                                          bool reliable = true) {
    BitWriter writer;
    writer.SerializeInt(
        DeploymentRepl::kServerAcknowledgePossessionHandle,
        DeploymentRepl::kRoPlayerControllerMaxHandle);
    writer.WriteBit(true);
    ActorRepl::WriteNetGUID(
        writer,
        ActorRepl::NetGUIDRef{/*isDynamic=*/true, pawnChannel});

    PacketCodec::Bunch bunch;
    bunch.bReliable = reliable;
    bunch.chIndex = 2u;
    bunch.chType = 2u;
    bunch.chSequence = 1u;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

PacketCodec::Bunch MakeSelectTeamBunch(uint8_t retailTeam) {
    BitWriter writer;
    writer.SerializeInt(170u,
                        DeploymentRepl::kRoPlayerControllerMaxHandle);
    if (retailTeam == 0u) {
        writer.WriteBit(false);
    } else {
        writer.WriteBit(true);
        writer.WriteByte(retailTeam);
    }

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

std::vector<uint8_t> ZeroTailedLegacyDatagram(
    const std::string& tag, const std::vector<uint8_t>& payload = {}) {
    Packet packet(tag, payload);
    std::vector<uint8_t> datagram = packet.Serialize();
    // PacketCodec requires a non-zero final byte containing the UE3 terminator.
    // Packet::FromBuffer historically tolerates this byte after its declared
    // payload, making this a valid probe for protocol-confusion fallback.
    datagram.push_back(0u);
    return datagram;
}

std::vector<uint8_t> LegacyStringPayload(const std::string& value) {
    Packet payload;
    payload.WriteString(value);
    return payload.RawData();
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

class ConnectionTravelCuChiRoleIntegrationTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(SocketFactory::Initialize());
        receiverPorts_[0] = BindReceiver(receiverOne_);
        receiverPorts_[1] = BindReceiver(receiverTwo_);
        ASSERT_NE(receiverPorts_[0], 0u);
        ASSERT_NE(receiverPorts_[1], 0u);

        sender_ = std::make_shared<UDPSocket>();
        ASSERT_TRUE(sender_->Bind(0));
        ConnectionTravelLifecycleTestHarness::InstallRoleSelectionRuntime(
            server_);
    }

    void TearDown() override {
        ConnectionTravelLifecycleTestHarness::ResetRoleSelectionRuntime(
            server_);
        if (sender_) sender_->Close();
        sender_.reset();
        receiverOne_.Close();
        receiverTwo_.Close();
        SocketFactory::Shutdown();
    }

    std::shared_ptr<ClientConnection> Connect(size_t receiverIndex,
                                              uint32_t clientId,
                                              uint32_t teamId) {
        if (receiverIndex >= receiverPorts_.size()) return nullptr;
        auto connection = ConnectionTravelLifecycleTestHarness::AddClient(
            manager_, clientId, "127.0.0.1", receiverPorts_[receiverIndex],
            true, 7u, false, {}, sender_);
        if (!ConnectionTravelLifecycleTestHarness::AttachRoleSelectionPlayer(
                server_, connection, teamId) ||
            !ConnectionTravelLifecycleTestHarness::FreezeCanonicalCuChiSession(
                manager_, clientId)) {
            return nullptr;
        }
        return connection;
    }

    static PacketCodec::Bunch CuChiFinalRoleBunch(bool south) {
        return MakeCapturedPcBunch(
            south
                ? std::vector<uint8_t>{
                      0xaf, 0x26, 0x5c, 0x15, 0x00, 0x80, 0xc3, 0x01}
                : std::vector<uint8_t>{
                      0xaf, 0x64, 0x56, 0x15, 0x00, 0x80, 0xc3, 0x01},
            57u);
    }

    static PacketCodec::Bunch SpawnSelectBunch(uint8_t slot = 0u) {
        BitWriter writer;
        writer.SerializeInt(
            DeploymentRepl::kServerSetSpawnSelectHandle,
            DeploymentRepl::kRoPlayerControllerMaxHandle);
        writer.WriteBit(true);
        writer.WriteByte(static_cast<uint8_t>(
            DeploymentCoordinator::kNormalSpawnSelectionBase + slot));
        return MakeCapturedPcBunch(
            writer.GetBytes(), static_cast<uint32_t>(writer.NumBits()));
    }

    static PacketCodec::Bunch ReadyBunch() {
        BitWriter writer;
        writer.SerializeInt(
            DeploymentRepl::kServerSetReadyToSpawnHandle,
            DeploymentRepl::kRoPlayerControllerMaxHandle);
        writer.WriteBit(false); // Default-omitted enum value is Ready.
        return MakeCapturedPcBunch(
            writer.GetBytes(), static_cast<uint32_t>(writer.NumBits()));
    }

    static PacketCodec::Bunch ForceOnlyBunch() {
        BitWriter writer;
        writer.SerializeInt(
            DeploymentRepl::kServerSetReadyToSpawnHandle,
            DeploymentRepl::kRoPlayerControllerMaxHandle);
        writer.WriteBit(true);
        writer.WriteBits(static_cast<uint8_t>(
            DeploymentCoordinator::ReadyStatus::ForceOnly),
            DeploymentRepl::kReadyStatusBits);
        return MakeCapturedPcBunch(
            writer.GetBytes(), static_cast<uint32_t>(writer.NumBits()));
    }

    std::vector<PacketCodec::Packet> DrainDecodedPackets(
        size_t receiverIndex) {
        UDPSocket* receiver = Receiver(receiverIndex);
        if (!receiver) return {};

        std::vector<PacketCodec::Packet> packets;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(50);
        size_t idlePollsAfterTraffic = 0u;
        while (packets.size() < 32u &&
               std::chrono::steady_clock::now() < deadline) {
            std::string sourceIp;
            uint16_t sourcePort = 0u;
            std::vector<uint8_t> bytes(
                PacketCodec::kServerSendMaxPacketBytes + 64u);
            const int received = receiver->ReceiveFrom(
                sourceIp, sourcePort, bytes.data(),
                static_cast<int>(bytes.size()));
            if (received <= 0) {
                if (!packets.empty() && ++idlePollsAfterTraffic >= 3u) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            idlePollsAfterTraffic = 0u;
            bytes.resize(static_cast<size_t>(received));
            packets.push_back(PacketCodec::Decode(
                bytes.data(), bytes.size(),
                PacketCodec::kServerSendMaxPacketBytes));
        }
        return packets;
    }

    static std::vector<PacketCodec::Bunch> FlattenBunches(
        const std::vector<PacketCodec::Packet>& packets) {
        std::vector<PacketCodec::Bunch> bunches;
        for (const PacketCodec::Packet& packet : packets) {
            bunches.insert(bunches.end(), packet.bunches.begin(),
                           packet.bunches.end());
        }
        return bunches;
    }

    static PacketCodec::Bunch ExpectedOwnerPriClass(uint8_t classIndex) {
        BitWriter writer;
        RoleSelectionRepl::WriteOwnerPriClassIndex(writer, classIndex);

        PacketCodec::Bunch bunch;
        bunch.bReliable = false;
        bunch.chIndex = 26u;
        bunch.payload = writer.GetBytes();
        bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
        return bunch;
    }

    static PacketCodec::Bunch ExpectedChangedRole(
        const RoleSelectionRepl::ChangedRoleEvidence& evidence) {
        PacketCodec::Bunch bunch;
        bunch.bReliable = true;
        bunch.chIndex = 2u;
        bunch.payload = RoleSelectionRepl::EncodeChangedRoleTransition(
            evidence, bunch.payloadBits);
        return bunch;
    }

    static PacketCodec::Bunch ExpectedTempStopAutoSpawn() {
        BitWriter writer;
        writer.SerializeInt(262u,
                            DeploymentRepl::kRoPlayerControllerMaxHandle);
        PacketCodec::Bunch bunch;
        bunch.bReliable = true;
        bunch.chIndex = 2u;
        bunch.payload = writer.GetBytes();
        bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
        return bunch;
    }

    static PacketCodec::Bunch ExpectedClientOnDead(bool isDead) {
        BitWriter writer;
        writer.SerializeInt(151u,
                            DeploymentRepl::kRoPlayerControllerMaxHandle);
        writer.WriteBit(isDead);
        PacketCodec::Bunch bunch;
        bunch.bReliable = true;
        bunch.chIndex = 2u;
        bunch.payload = writer.GetBytes();
        bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
        return bunch;
    }

    static PacketCodec::Bunch ExpectedOwnerNextRespawnTime(
        int32_t nextRespawnTime) {
        BitWriter writer;
        DeploymentRepl::WriteOwnerNextRespawnTime(
            writer, nextRespawnTime);

        PacketCodec::Bunch bunch;
        bunch.bReliable = false;
        bunch.chIndex = 2u;
        bunch.payload = writer.GetBytes();
        bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
        return bunch;
    }

    static PacketCodec::Bunch ExpectedTeamReinforcements(
        uint32_t teamInfoChannel, int32_t reinforcements) {
        BitWriter writer;
        SpawnRepl::WriteReinforcementsRemaining(writer, reinforcements);

        PacketCodec::Bunch bunch;
        bunch.bReliable = false;
        bunch.chIndex = teamInfoChannel;
        bunch.payload = writer.GetBytes();
        bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
        return bunch;
    }

    bool PublishCuChiSpawnSelection(size_t receiverIndex,
                                    uint32_t clientId,
                                    bool south) {
        ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
            manager_, clientId, CuChiFinalRoleBunch(south));
        (void)DrainDecodedPackets(receiverIndex);
        ConnectionTravelLifecycleTestHarness::AcknowledgeAllPendingReliables(
            manager_, clientId);

        ConnectionTravelLifecycleTestHarness::DeliverActorBunch(
            manager_, clientId, SpawnSelectBunch());
        (void)DrainDecodedPackets(receiverIndex);
        const auto deployment =
            ConnectionTravelLifecycleTestHarness::DeploymentState(
                manager_, clientId);
        return deployment.has_value() &&
            deployment->selectedSpawnId.has_value() &&
            !deployment->deploymentAuthorized;
    }

    static PacketCodec::Bunch ExpectedOwnerPriAssignment(
        uint8_t squadIndex, uint8_t roleIndex) {
        BitWriter writer;
        RoleSelectionRepl::WriteOwnerPriRoleAssignment(
            writer, squadIndex, roleIndex);

        PacketCodec::Bunch bunch;
        bunch.bReliable = false;
        bunch.chIndex = 26u;
        bunch.payload = writer.GetBytes();
        bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
        return bunch;
    }

    static size_t FindWireBunch(
        const std::vector<PacketCodec::Bunch>& bunches,
        const PacketCodec::Bunch& expected, size_t begin = 0u) {
        const auto found = std::find_if(
            bunches.begin() + static_cast<std::ptrdiff_t>(
                                  std::min(begin, bunches.size())),
            bunches.end(),
            [&expected](const PacketCodec::Bunch& bunch) {
                return bunch.bReliable == expected.bReliable &&
                       bunch.chIndex == expected.chIndex &&
                       bunch.payloadBits == expected.payloadBits &&
                       bunch.payload == expected.payload;
            });
        return found == bunches.end()
            ? bunches.size()
            : static_cast<size_t>(std::distance(bunches.begin(), found));
    }

    GameServer server_;
    ConnectionManager manager_{&server_};

    void CloseSenderSocketForTest() {
        if (sender_) sender_->Close();
    }

    bool RebindSenderSocketForTest() {
        return sender_ && sender_->Bind(0u);
    }

private:
    UDPSocket* Receiver(size_t receiverIndex) {
        if (receiverIndex == 0u) return &receiverOne_;
        if (receiverIndex == 1u) return &receiverTwo_;
        return nullptr;
    }

    static uint16_t BindReceiver(UDPSocket& receiver) {
        constexpr uint32_t kFirstPort = 32000u;
        constexpr uint32_t kPortCount = 16000u;
        const auto seed = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const uint32_t start = static_cast<uint32_t>(seed % kPortCount);
        for (uint32_t offset = 0; offset < 2048u; ++offset) {
            const auto port = static_cast<uint16_t>(
                kFirstPort + ((start + offset) % kPortCount));
            if (receiver.Bind(port)) return port;
        }
        return 0u;
    }

    UDPSocket receiverOne_;
    UDPSocket receiverTwo_;
    std::array<uint16_t, 2> receiverPorts_{};
    std::shared_ptr<UDPSocket> sender_;
};

} // namespace

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       AuthoritativeReinforcementsReachLiveAndCapturedTeamInfoChannels) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kLiveViewer = 90u;
    constexpr uint32_t kCapturedViewer = 91u;

    TicketSystem* tickets = Harness::InstallTicketSystem(
        server_, /*south/US=*/300u, /*north/NVA=*/200u);
    ASSERT_TRUE(tickets != nullptr);
    ASSERT_TRUE(Connect(0u, kLiveViewer, TeamMapping::kServerUs) != nullptr);
    ASSERT_TRUE(Connect(1u, kCapturedViewer, TeamMapping::kServerNva) !=
                nullptr);
    Harness::UseCapturedTeamInfoChannels(manager_, kCapturedViewer);
    Harness::MarkTeamInfoChannelsOpen(manager_, kLiveViewer);
    Harness::MarkTeamInfoChannelsOpen(manager_, kCapturedViewer);

    // Exercise the production end-of-tick seam, not only the private helper.
    Harness::UpdateRetailDeploymentCountdown(manager_);
    const std::vector<PacketCodec::Packet> liveBaselinePackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Packet> capturedBaselinePackets =
        DrainDecodedPackets(1u);
    const std::vector<PacketCodec::Bunch> liveBaseline =
        FlattenBunches(liveBaselinePackets);
    const std::vector<PacketCodec::Bunch> capturedBaseline =
        FlattenBunches(capturedBaselinePackets);
    // The public countdown seam may also publish unrelated phase/UI state;
    // isolate the two exact h62 bunches within that complete end-of-tick batch.
    ASSERT_GE(liveBaseline.size(), static_cast<size_t>(2));
    ASSERT_GE(capturedBaseline.size(), static_cast<size_t>(2));
    EXPECT_LT(FindWireBunch(
                  liveBaseline, ExpectedTeamReinforcements(4u, 200)),
              liveBaseline.size());
    EXPECT_LT(FindWireBunch(
                  liveBaseline, ExpectedTeamReinforcements(5u, 300)),
              liveBaseline.size());
    EXPECT_LT(FindWireBunch(
                  capturedBaseline, ExpectedTeamReinforcements(76u, 200)),
              capturedBaseline.size());
    EXPECT_LT(FindWireBunch(
                  capturedBaseline, ExpectedTeamReinforcements(56u, 300)),
              capturedBaseline.size());
    Harness::AcknowledgePackets(
        manager_, kLiveViewer, liveBaselinePackets);
    Harness::AcknowledgePackets(
        manager_, kCapturedViewer, capturedBaselinePackets);
    EXPECT_EQ(Harness::PublishedTeamReinforcements(
                  manager_, kLiveViewer),
              (std::array<std::optional<int32_t>, 2>{200, 300}));

    const uint32_t livePacketAfterBaseline =
        Harness::NextOutboundPacketId(manager_, kLiveViewer);
    const uint32_t capturedPacketAfterBaseline =
        Harness::NextOutboundPacketId(manager_, kCapturedViewer);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    EXPECT_EQ(Harness::NextOutboundPacketId(manager_, kLiveViewer),
              livePacketAfterBaseline);
    EXPECT_EQ(Harness::NextOutboundPacketId(manager_, kCapturedViewer),
              capturedPacketAfterBaseline);
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    EXPECT_TRUE(DrainDecodedPackets(1u).empty());

    // Server team 1 is retail US/ch5 or ch56; server team 2 is retail
    // NVA/ch4 or ch76. Commit both mutations before the sync and require each
    // viewer to receive one final snapshot of both pools.
    tickets->OnPlayerKilled(TeamMapping::kServerUs);
    tickets->SetTickets(TeamMapping::kServerNva, 0u);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> liveDepletionPackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Packet> capturedDepletionPackets =
        DrainDecodedPackets(1u);
    const std::vector<PacketCodec::Bunch> liveDepletion =
        FlattenBunches(liveDepletionPackets);
    const std::vector<PacketCodec::Bunch> capturedDepletion =
        FlattenBunches(capturedDepletionPackets);
    ASSERT_EQ(liveDepletion.size(), static_cast<size_t>(2));
    ASSERT_EQ(capturedDepletion.size(), static_cast<size_t>(2));
    EXPECT_LT(FindWireBunch(
                  liveDepletion, ExpectedTeamReinforcements(4u, 0)),
              liveDepletion.size());
    EXPECT_LT(FindWireBunch(
                  liveDepletion, ExpectedTeamReinforcements(5u, 299)),
              liveDepletion.size());
    EXPECT_LT(FindWireBunch(
                  capturedDepletion, ExpectedTeamReinforcements(76u, 0)),
              capturedDepletion.size());
    EXPECT_LT(FindWireBunch(
                  capturedDepletion, ExpectedTeamReinforcements(56u, 299)),
              capturedDepletion.size());
    Harness::AcknowledgePackets(
        manager_, kLiveViewer, liveDepletionPackets);
    Harness::AcknowledgePackets(
        manager_, kCapturedViewer, capturedDepletionPackets);

    tickets->AddTickets(TeamMapping::kServerNva, 7u);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> liveRefillPackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Packet> capturedRefillPackets =
        DrainDecodedPackets(1u);
    const std::vector<PacketCodec::Bunch> liveRefill =
        FlattenBunches(liveRefillPackets);
    const std::vector<PacketCodec::Bunch> capturedRefill =
        FlattenBunches(capturedRefillPackets);
    ASSERT_EQ(liveRefill.size(), static_cast<size_t>(1));
    ASSERT_EQ(capturedRefill.size(), static_cast<size_t>(1));
    EXPECT_LT(FindWireBunch(
                  liveRefill, ExpectedTeamReinforcements(4u, 7)),
              liveRefill.size());
    EXPECT_LT(FindWireBunch(
                  capturedRefill, ExpectedTeamReinforcements(76u, 7)),
              capturedRefill.size());
    Harness::AcknowledgePackets(
        manager_, kLiveViewer, liveRefillPackets);
    Harness::AcknowledgePackets(
        manager_, kCapturedViewer, capturedRefillPackets);

    tickets->Reset();
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> liveResetPackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Packet> capturedResetPackets =
        DrainDecodedPackets(1u);
    const std::vector<PacketCodec::Bunch> liveReset =
        FlattenBunches(liveResetPackets);
    const std::vector<PacketCodec::Bunch> capturedReset =
        FlattenBunches(capturedResetPackets);
    ASSERT_EQ(liveReset.size(), static_cast<size_t>(2));
    ASSERT_EQ(capturedReset.size(), static_cast<size_t>(2));
    EXPECT_LT(FindWireBunch(
                  liveReset, ExpectedTeamReinforcements(4u, 200)),
              liveReset.size());
    EXPECT_LT(FindWireBunch(
                  liveReset, ExpectedTeamReinforcements(5u, 300)),
              liveReset.size());
    EXPECT_LT(FindWireBunch(
                  capturedReset, ExpectedTeamReinforcements(76u, 200)),
              capturedReset.size());
    EXPECT_LT(FindWireBunch(
                  capturedReset, ExpectedTeamReinforcements(56u, 300)),
              capturedReset.size());
    Harness::AcknowledgePackets(manager_, kLiveViewer, liveResetPackets);
    Harness::AcknowledgePackets(
        manager_, kCapturedViewer, capturedResetPackets);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ReinforcementSyncHonorsUnlimitedPoolsOpenChannelsAndTravelBoundary) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 92u;

    TicketSystem* tickets = Harness::InstallTicketSystem(
        server_, /*south/US unlimited=*/0u, /*north/NVA=*/5u);
    ASSERT_TRUE(tickets != nullptr);
    ASSERT_TRUE(Connect(0u, kClientId, TeamMapping::kServerUs) != nullptr);

    // Frozen channel numbers alone are insufficient: no delta may target a
    // TeamInfo actor whose reliable open was never queued.
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));

    Harness::MarkTeamInfoChannelsOpen(manager_, kClientId);
    CloseSenderSocketForTest();
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));
    EXPECT_EQ(Harness::PendingTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));
    ASSERT_TRUE(RebindSenderSocketForTest());

    Harness::SeedPendingTeamInfoOpen(manager_, kClientId, 4u);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> partialBaselinePackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Bunch> partialBaseline =
        FlattenBunches(partialBaselinePackets);
    ASSERT_EQ(partialBaselinePackets.size(), static_cast<size_t>(1));
    ASSERT_EQ(partialBaseline.size(), static_cast<size_t>(1));
    EXPECT_LT(FindWireBunch(
                  partialBaseline,
                  ExpectedTeamReinforcements(
                      5u, SpawnRepl::kUnlimitedReinforcementsDisplay)),
              partialBaseline.size());
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));
    EXPECT_EQ(Harness::PendingTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt,
                  SpawnRepl::kUnlimitedReinforcementsDisplay}));

    // The first wire-unreliable packet is intentionally left unacknowledged.
    // Expiring its retirement timer must re-emit the same dirty property, and
    // an ACK for either carrying packet retires the value exactly once.
    Harness::ExpireTeamReinforcementRetry(
        manager_, kClientId, TeamMapping::kRetailUs);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> retryPackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Bunch> retryBunches =
        FlattenBunches(retryPackets);
    ASSERT_EQ(retryPackets.size(), static_cast<size_t>(1));
    ASSERT_EQ(retryBunches.size(), static_cast<size_t>(1));
    EXPECT_NE(retryPackets.front().packetId,
              partialBaselinePackets.front().packetId);
    EXPECT_LT(FindWireBunch(
                  retryBunches,
                  ExpectedTeamReinforcements(
                      5u, SpawnRepl::kUnlimitedReinforcementsDisplay)),
              retryBunches.size());
    Harness::Acknowledge(
        manager_, kClientId, partialBaselinePackets.front().packetId);
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt,
                  SpawnRepl::kUnlimitedReinforcementsDisplay}));
    EXPECT_EQ(Harness::PendingTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));

    // Once the reliable open retires, the still-dirty finite pool publishes
    // without redundantly resending the already-synchronized other team.
    Harness::ClearPendingReliables(manager_, kClientId);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> completedBaselinePackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Bunch> completedBaseline =
        FlattenBunches(completedBaselinePackets);
    ASSERT_EQ(completedBaseline.size(), static_cast<size_t>(1));
    EXPECT_LT(FindWireBunch(
                  completedBaseline, ExpectedTeamReinforcements(4u, 5)),
              completedBaseline.size());
    Harness::AcknowledgePackets(
        manager_, kClientId, completedBaselinePackets);

    // Returning authority to the last ACKed value cannot merely discard a
    // conflicting in-flight datagram: that old zero may still arrive later.
    // Force a new current-value write, ignore the delayed old ACK, and retire
    // only when a packet carrying the corrective five is acknowledged.
    tickets->SetTickets(TeamMapping::kServerNva, 0u);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> staleZeroPackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Bunch> staleZeroBunches =
        FlattenBunches(staleZeroPackets);
    ASSERT_EQ(staleZeroPackets.size(), static_cast<size_t>(1));
    EXPECT_LT(FindWireBunch(
                  staleZeroBunches, ExpectedTeamReinforcements(4u, 0)),
              staleZeroBunches.size());

    tickets->SetTickets(TeamMapping::kServerNva, 5u);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    const std::vector<PacketCodec::Packet> correctivePackets =
        DrainDecodedPackets(0u);
    const std::vector<PacketCodec::Bunch> correctiveBunches =
        FlattenBunches(correctivePackets);
    ASSERT_EQ(correctivePackets.size(), static_cast<size_t>(1));
    EXPECT_LT(FindWireBunch(
                  correctiveBunches,
                  ExpectedTeamReinforcements(4u, 5)),
              correctiveBunches.size());
    Harness::Acknowledge(
        manager_, kClientId, staleZeroPackets.front().packetId);
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt,
                  SpawnRepl::kUnlimitedReinforcementsDisplay}));
    EXPECT_EQ(Harness::PendingTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  5, std::nullopt}));
    Harness::AcknowledgePackets(manager_, kClientId, correctivePackets);
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  5, SpawnRepl::kUnlimitedReinforcementsDisplay}));

    // Diagnostic writes to an initial-zero pool never alter its retail wire
    // sentinel and therefore must not create a redundant property delta.
    tickets->SetTickets(TeamMapping::kServerUs, 123u);
    const uint32_t packetBeforeUnlimitedNoop =
        Harness::NextOutboundPacketId(manager_, kClientId);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    EXPECT_EQ(Harness::NextOutboundPacketId(manager_, kClientId),
              packetBeforeUnlimitedNoop);
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());

    const ClientTravelRepl::EncodedRpc travel = MakeTravelRpc();
    ASSERT_TRUE(ClientTravelRepl::IsValid(travel));
    ASSERT_EQ(manager_.BroadcastRetailClientTravel(
                  travel, "VNSK-Compound"),
              static_cast<size_t>(1));
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));
    EXPECT_EQ(Harness::PendingTeamReinforcements(manager_, kClientId),
              (std::array<std::optional<int32_t>, 2>{
                  std::nullopt, std::nullopt}));
    (void)DrainDecodedPackets(0u);

    tickets->SetTickets(TeamMapping::kServerNva, 4u);
    Harness::SynchronizeRetailTeamReinforcements(manager_);
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
}

TEST(ConnectionTravelLifecycle,
     ActiveTerritoryDeadlineUsesRetailIntegerCoordinateAndCarriesResidual) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    EXPECT_EQ(Harness::RetailRemainingSecond(100.0f),
              std::optional<int32_t>{100});
    EXPECT_EQ(Harness::RetailRemainingSecond(100.01f),
              std::optional<int32_t>{101});
    EXPECT_FALSE(Harness::RetailRemainingSecond(-0.01f).has_value());
    EXPECT_FALSE(Harness::RetailRemainingSecond(
        std::numeric_limits<float>::infinity()).has_value());

    EXPECT_EQ(Harness::CalculateActiveDeploymentDeadline(
                  100, TeamMapping::kServerUs),
              std::optional<int32_t>{85});
    EXPECT_EQ(Harness::CalculateActiveDeploymentDeadline(
                  100, TeamMapping::kServerNva),
              std::optional<int32_t>{80});
    EXPECT_FALSE(Harness::CalculateActiveDeploymentDeadline(
        100, 0u).has_value());

    // Seven seconds remained when the old coordinate ended. A phase clock
    // reset to 180 therefore carries the deadline to 173 rather than granting
    // a fresh 15/20-second interval.
    EXPECT_EQ(Harness::RebaseActiveDeploymentDeadline(
                  /*previousRemainingSeconds=*/2,
                  /*previousDeadlineRemainingSeconds=*/-5,
                  /*currentRemainingSeconds=*/180),
              std::optional<int32_t>{173});
    EXPECT_FALSE(Harness::HasReachedActiveDeploymentDeadline(174, 173));
    EXPECT_TRUE(Harness::HasReachedActiveDeploymentDeadline(173, 173));
    EXPECT_TRUE(Harness::HasReachedActiveDeploymentDeadline(172, 173));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryReadyDefersSouthAndNorthWithOwnerOnlyH316) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kSouthClient = 70u;
    constexpr uint32_t kNorthClient = 71u;
    constexpr uint32_t kSouthTeam = TeamMapping::kServerUs;
    constexpr uint32_t kNorthTeam = TeamMapping::kServerNva;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 120.0f);
    ASSERT_TRUE(territory != nullptr);
    ASSERT_EQ(territory->GetPhase(), TerritoryMode::Phase::Active);
    // Prime the manager's phase edge before either client can hold a prepared
    // deployment. The initial-round batch is a distinct protocol path.
    Harness::UpdateRetailDeploymentCountdown(manager_);

    ASSERT_TRUE(Connect(0u, kSouthClient, kSouthTeam) != nullptr);
    ASSERT_TRUE(Connect(1u, kNorthClient, kNorthTeam) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kSouthClient, /*south=*/true));
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        1u, kNorthClient, /*south=*/false));
    (void)DrainDecodedPackets(0u);
    (void)DrainDecodedPackets(1u);

    Harness::DeliverActorBunch(manager_, kSouthClient, ReadyBunch());
    const std::vector<PacketCodec::Bunch> southWire =
        FlattenBunches(DrainDecodedPackets(0u));
    const PacketCodec::Bunch expectedSouth =
        ExpectedOwnerNextRespawnTime(105);
    EXPECT_TRUE(FindWireBunch(southWire, expectedSouth) < southWire.size());
    EXPECT_EQ(expectedSouth.payloadBits,
              DeploymentRepl::kNextRespawnTimePropertyBits);
    EXPECT_TRUE(DrainDecodedPackets(1u).empty());
    EXPECT_EQ(Harness::ActiveDeploymentDeadline(manager_, kSouthClient),
              std::optional<int32_t>{105});
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kSouthClient),
              std::optional<int32_t>{105});
    EXPECT_TRUE(Harness::DeploymentPrepared(manager_, kSouthClient));
    EXPECT_FALSE(Harness::Spawned(manager_, kSouthClient));

    Harness::DeliverActorBunch(manager_, kNorthClient, ReadyBunch());
    const std::vector<PacketCodec::Bunch> northWire =
        FlattenBunches(DrainDecodedPackets(1u));
    const PacketCodec::Bunch expectedNorth =
        ExpectedOwnerNextRespawnTime(100);
    EXPECT_TRUE(FindWireBunch(northWire, expectedNorth) < northWire.size());
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    EXPECT_EQ(Harness::ActiveDeploymentDeadline(manager_, kNorthClient),
              std::optional<int32_t>{100});
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kNorthClient),
              std::optional<int32_t>{100});
    EXPECT_TRUE(Harness::DeploymentPrepared(manager_, kNorthClient));
    EXPECT_FALSE(Harness::Spawned(manager_, kNorthClient));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryTeamSwitchReplacesOwnerDeadlineAfterTeamCommit) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kOwnerClient = 81u;
    constexpr uint32_t kObserverClient = 82u;
    constexpr uint32_t kSouthTeam = TeamMapping::kServerUs;
    constexpr uint32_t kNorthTeam = TeamMapping::kServerNva;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 120.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(0u, kOwnerClient, kSouthTeam) != nullptr);
    ASSERT_TRUE(Connect(1u, kObserverClient, kSouthTeam) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kOwnerClient, /*south=*/true));

    Harness::DeliverActorBunch(manager_, kOwnerClient, ReadyBunch());
    (void)DrainDecodedPackets(0u);
    (void)DrainDecodedPackets(1u);
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(teams != nullptr);
    ASSERT_EQ(teams->GetPlayerTeam(kOwnerClient), kSouthTeam);
    ASSERT_EQ(Harness::ActiveDeploymentDeadline(manager_, kOwnerClient),
              std::optional<int32_t>{105});
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kOwnerClient));
    ASSERT_FALSE(Harness::Spawned(manager_, kOwnerClient));

    // Retail team zero is NLF/NVA. h170 must first commit that authoritative
    // numeric team mutation, then replace the old South interval with the
    // North 20-second owner deadline in the same RemainingTime coordinate.
    Harness::DeliverActorBunch(
        manager_, kOwnerClient, MakeSelectTeamBunch(/*retail NLF=*/0u));

    EXPECT_EQ(teams->GetPlayerTeam(kOwnerClient), kNorthTeam);
    EXPECT_EQ(Harness::ActiveDeploymentDeadline(manager_, kOwnerClient),
              std::optional<int32_t>{100});
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kOwnerClient),
              std::optional<int32_t>{100});
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kOwnerClient));
    EXPECT_FALSE(Harness::Spawned(manager_, kOwnerClient));
    const auto deployment =
        Harness::DeploymentState(manager_, kOwnerClient);
    ASSERT_TRUE(deployment.has_value());
    EXPECT_FALSE(deployment->deploymentAuthorized);

    const std::vector<PacketCodec::Bunch> ownerWire =
        FlattenBunches(DrainDecodedPackets(0u));
    const PacketCodec::Bunch expectedNorthDeadline =
        ExpectedOwnerNextRespawnTime(100);
    EXPECT_TRUE(FindWireBunch(ownerWire, expectedNorthDeadline) <
                ownerWire.size());
    const std::vector<PacketCodec::Bunch> observerWire =
        FlattenBunches(DrainDecodedPackets(1u));
    EXPECT_EQ(FindWireBunch(observerWire, expectedNorthDeadline),
              observerWire.size());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryScanReleasesExactlyOnceAtOwnerDeadline) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 72u;
    constexpr uint32_t kSouthTeam = TeamMapping::kServerUs;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 120.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(0u, kClientId, kSouthTeam) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    (void)DrainDecodedPackets(0u);
    ASSERT_EQ(Harness::ActiveDeploymentDeadline(manager_, kClientId),
              std::optional<int32_t>{105});
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));
    ASSERT_FALSE(Harness::Spawned(manager_, kClientId));
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);

    territory->Update(14.0f);
    ASSERT_EQ(territory->GetRoundTimeRemaining(), 106.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    const std::vector<PacketCodec::Bunch> republishedAt106 =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        republishedAt106, ExpectedOwnerNextRespawnTime(105)) <
        republishedAt106.size());
    const uint32_t packetAt106 =
        Harness::NextOutboundPacketId(manager_, kClientId);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_EQ(Harness::NextOutboundPacketId(manager_, kClientId),
              packetAt106);

    territory->Update(1.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Harness::Spawned(manager_, kClientId));
    ASSERT_FALSE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kClientId),
              std::optional<int32_t>{
                  DeploymentRepl::kNoPendingRespawnTime});
    const std::vector<PacketCodec::Bunch> releaseWire =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        releaseWire,
        ExpectedOwnerNextRespawnTime(
            DeploymentRepl::kNoPendingRespawnTime)) < releaseWire.size());
    const uint32_t packetAfterRelease =
        Harness::NextOutboundPacketId(manager_, kClientId);
    const size_t pendingAfterRelease =
        Harness::PendingReliableCount(manager_, kClientId);

    // Re-running both the same integer second and a fractional tick that still
    // ceil-replicates as 105 cannot publish a second owning graph.
    Harness::UpdateRetailDeploymentCountdown(manager_);
    territory->Update(0.25f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_EQ(Harness::NextOutboundPacketId(manager_, kClientId),
              packetAfterRelease);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId),
              pendingAfterRelease);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryDeadlineCarriesIntoOvertimeAndReleasesOnResidual) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 80u;
    constexpr uint32_t kSouthTeam = TeamMapping::kServerUs;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 10.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(0u, kClientId, kSouthTeam) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    (void)DrainDecodedPackets(0u);
    ASSERT_EQ(Harness::ActiveDeploymentDeadline(manager_, kClientId),
              std::optional<int32_t>{-5});
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);

    ObjectiveSystem* objectives = server_.GetObjectiveSystem();
    ASSERT_TRUE(objectives != nullptr);
    const uint32_t attackingTeam = territory->GetAttackingTeam();
    objectives->SetBotCaptureWeightProvider(
        [attackingTeam](uint32_t, uint32_t teamId) {
            return teamId == attackingTeam ? 1.0f : 0.0f;
        });
    objectives->RefreshPlayerZones();

    territory->Update(10.0f);
    ASSERT_EQ(territory->GetPhase(), TerritoryMode::Phase::Overtime);
    ASSERT_FLOAT_EQ(
        territory->GetPreviousPhaseRemainingAtTransition(), 0.0f);
    ASSERT_FLOAT_EQ(territory->GetRoundTimeRemaining(), 180.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);

    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_EQ(Harness::ActiveDeploymentDeadline(manager_, kClientId),
              std::optional<int32_t>{175});
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kClientId),
              std::optional<int32_t>{175});
    const std::vector<PacketCodec::Bunch> transitionWire =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        transitionWire, ExpectedOwnerNextRespawnTime(175)) <
        transitionWire.size());

    territory->Update(4.0f);
    ASSERT_FLOAT_EQ(territory->GetRoundTimeRemaining(), 176.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    (void)DrainDecodedPackets(0u);

    territory->Update(1.0f);
    ASSERT_FLOAT_EQ(territory->GetRoundTimeRemaining(), 175.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       NonCuChiTerritoryPreservesEstablishedImmediateActiveDeployment) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 78u;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 120.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(
        0u, kClientId, TeamMapping::kServerUs) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));
    Harness::UseCanonicalResortProfile(manager_, kClientId);
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());

    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());
    const std::vector<PacketCodec::Bunch> wire =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        wire, ExpectedOwnerNextRespawnTime(105)) == wire.size());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       FinalPreparationFactionCloseRetainsRoundStartAuthorization) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 79u;
    constexpr uint32_t kSouthTeam = TeamMapping::kServerUs;
    constexpr uint32_t kNorthTeam = TeamMapping::kServerNva;

    TerritoryMode* territory =
        Harness::InstallPreparationTerritoryMode(server_, 8.0f);
    ASSERT_TRUE(territory != nullptr);
    ASSERT_TRUE(Connect(0u, kClientId, kSouthTeam) != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);

    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> southSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(southSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *southSpawn));
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    ASSERT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kSouthTeam);

    players->OnPlayerDeath(kClientId);
    teams->AddPlayerToTeam(kClientId, kNorthTeam);
    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> northSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(northSpawn.has_value());
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *northSpawn));
    ASSERT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    ASSERT_TRUE(Harness::DeferredOwningPawnHasRoundStartAuthorization(
        manager_, kClientId));

    territory->Update(8.0f);
    ASSERT_EQ(territory->GetPhase(), TerritoryMode::Phase::Active);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_FALSE(Harness::Spawned(manager_, kClientId));
    ASSERT_TRUE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());

    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);

    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_TRUE(player->IsAlive());
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kNorthTeam);
    EXPECT_FALSE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryTicketDepletionRequiresFreshSelectAndReady) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 73u;
    constexpr uint32_t kSouthTeam = TeamMapping::kServerUs;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 120.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(0u, kClientId, kSouthTeam) != nullptr);
    TicketSystem* tickets = Harness::InstallTicketSystem(
        server_, /*southTickets=*/1u, /*northTickets=*/1u);
    ASSERT_TRUE(tickets != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));
    const Harness::RoleLedgerSnapshot role =
        Harness::RoleLedger(manager_, kClientId);
    ASSERT_TRUE(role.changedRole.has_value());

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    (void)DrainDecodedPackets(0u);
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);
    tickets->SetTickets(kSouthTeam, 0u);
    territory->Update(15.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);

    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    const auto depleted = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(depleted.has_value());
    EXPECT_TRUE(depleted->roleFinalized);
    EXPECT_FALSE(depleted->selectedSpawnId.has_value());
    EXPECT_EQ(depleted->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(depleted->deploymentAuthorized);
    const std::vector<PacketCodec::Bunch> recovery =
        FlattenBunches(DrainDecodedPackets(0u));
    const PacketCodec::Bunch expectedStop = ExpectedTempStopAutoSpawn();
    const PacketCodec::Bunch expectedRole =
        ExpectedChangedRole(*role.changedRole);
    const size_t stopIndex = FindWireBunch(recovery, expectedStop);
    const size_t roleIndex = FindWireBunch(recovery, expectedRole);
    ASSERT_TRUE(stopIndex < roleIndex);
    ASSERT_TRUE(roleIndex < recovery.size());
    EXPECT_EQ(recovery[roleIndex].chSequence,
              recovery[stopIndex].chSequence + 1u);
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);

    tickets->AddTickets(kSouthTeam, 1u);
    territory->Update(1.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));
    const std::vector<PacketCodec::Bunch> unpreparedRetry =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        unpreparedRetry, ExpectedOwnerNextRespawnTime(105)) <
        unpreparedRetry.size());

    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));
    territory->Update(1.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));
    (void)DrainDecodedPackets(0u);

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));
    territory->Update(1.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryForceOnlyRemainsRevokedAtDeadline) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 74u;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 60.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(
        0u, kClientId, TeamMapping::kServerUs) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    (void)DrainDecodedPackets(0u);
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));
    ASSERT_EQ(Harness::ActiveDeploymentDeadline(manager_, kClientId),
              std::optional<int32_t>{45});
    Harness::DeliverActorBunch(manager_, kClientId, ForceOnlyBunch());
    const auto revoked = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(revoked.has_value());
    EXPECT_EQ(revoked->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(revoked->deploymentAuthorized);
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));

    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);
    territory->Update(15.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    const auto stillRevoked = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(stillRevoked.has_value());
    EXPECT_EQ(stillRevoked->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(stillRevoked->deploymentAuthorized);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       TerritorySuddenDeathClearsArmedDeadlineAndClosesExecution) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 75u;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 60.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(
        0u, kClientId, TeamMapping::kServerUs) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));
    const auto prepared = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->selectedSpawnId.has_value());
    const uint32_t spawnId = *prepared->selectedSpawnId;

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));
    ASSERT_EQ(Harness::ActiveDeploymentDeadline(manager_, kClientId),
              std::optional<int32_t>{45});
    (void)DrainDecodedPackets(0u);

    territory->OnTicketsDepleted(TeamMapping::kServerUs);
    ASSERT_EQ(territory->GetPhase(), TerritoryMode::Phase::SuddenDeath);
    ASSERT_TRUE(Harness::ActiveDeploymentPolicyIsClosed(
        manager_, kClientId));
    Harness::UpdateRetailDeploymentCountdown(manager_);
    const std::vector<PacketCodec::Bunch> wire =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        wire, ExpectedOwnerNextRespawnTime(
                  DeploymentRepl::kNoPendingRespawnTime)) < wire.size());
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kClientId),
              std::optional<int32_t>{
                  DeploymentRepl::kNoPendingRespawnTime});

    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, spawnId));
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       FinalEightPreparationSecondsStillDeployImmediately) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 76u;

    TerritoryMode* territory =
        Harness::InstallPreparationTerritoryMode(server_, 8.0f);
    ASSERT_TRUE(territory != nullptr);
    ASSERT_EQ(territory->GetPhase(), TerritoryMode::Phase::Preparation);
    ASSERT_EQ(territory->GetRoundTimeRemaining(), 8.0f);
    ASSERT_TRUE(Connect(
        0u, kClientId, TeamMapping::kServerUs) != nullptr);
    ASSERT_TRUE(PublishCuChiSpawnSelection(
        0u, kClientId, /*south=*/true));
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    const std::vector<PacketCodec::Bunch> wire =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(Harness::ActiveDeploymentDeadline(
        manager_, kClientId).has_value());
    EXPECT_TRUE(FindWireBunch(
        wire, ExpectedOwnerNextRespawnTime(-7)) == wire.size());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ActiveTerritoryDeathRevokesReadyAndArmsFreshOwnerDeadline) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 77u;

    TerritoryMode* territory =
        Harness::InstallActiveTerritoryMode(server_, 120.0f);
    ASSERT_TRUE(territory != nullptr);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    ASSERT_TRUE(Connect(
        0u, kClientId, TeamMapping::kServerUs) != nullptr);
    const std::optional<uint32_t> selectedSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(selectedSpawn.has_value());
    ASSERT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));

    PlayerManager* players = server_.GetPlayerManager();
    ASSERT_TRUE(players != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);
    players->OnPlayerSpawn(kClientId);
    player->SetReadyToSpawn(true);
    Harness::SetPossessionRecoveryEligibility(
        manager_, kClientId, true, true, false, 1u);
    ASSERT_TRUE(player->IsAlive());
    ASSERT_TRUE(player->IsReadyToSpawn());
    (void)DrainDecodedPackets(0u);

    players->OnPlayerDeath(kClientId);
    // A script-adjusted negative score is not wire-encodable, but the
    // authoritative death boundary must still publish ClientOnDead, revoke
    // Ready, and arm the next owner deadline.
    manager_.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(kClientId), 0, 0, 1, -1,
        /*isDead=*/true, /*sendHealth=*/false, /*sendDeathRpc=*/true);

    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(player->IsAlive());
    EXPECT_FALSE(player->IsReadyToSpawn());
    const auto revoked = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(revoked.has_value());
    EXPECT_TRUE(revoked->roleFinalized);
    EXPECT_FALSE(revoked->selectedSpawnId.has_value());
    EXPECT_EQ(revoked->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(revoked->deploymentAuthorized);
    EXPECT_EQ(Harness::ActiveDeploymentDeadline(manager_, kClientId),
              std::optional<int32_t>{105});
    EXPECT_EQ(Harness::PublishedNextRespawnTime(manager_, kClientId),
              std::optional<int32_t>{105});
    const std::vector<PacketCodec::Bunch> deathWire =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        deathWire, ExpectedClientOnDead(true)) < deathWire.size());
    EXPECT_TRUE(FindWireBunch(
        deathWire, ExpectedOwnerNextRespawnTime(105)) < deathWire.size());

    territory->Update(1.0f);
    Harness::UpdateRetailDeploymentCountdown(manager_);
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));
    const std::vector<PacketCodec::Bunch> unpreparedRetry =
        FlattenBunches(DrainDecodedPackets(0u));
    EXPECT_TRUE(FindWireBunch(
        unpreparedRetry, ExpectedOwnerNextRespawnTime(105)) <
        unpreparedRetry.size());
}

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

TEST(ConnectionTravelLifecycle,
     SupremacyConnectivityPreservesAuthoredBitsWithoutBothHeadquarters) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    SupremacyMode mode(nullptr);
    mode.ClearObjectives();
    mode.SetObjectiveMetadata(0, SupremacyMode::kSouthTeamId, 1);
    mode.SetObjectiveMetadata(1, SupremacyMode::kNorthTeamId, 1);
    mode.SetObjectiveMetadata(2, SupremacyMode::kSouthTeamId, 3);
    mode.SetObjectiveLinks({
        {0, {1}},
        {1, {0}},
        {2, {1}}, // Non-isolated but unreachable outward from South HQ.
    });

    ASSERT_FALSE(mode.UsesSupplyLines());
    EXPECT_TRUE(Harness::ResolveObjectiveConnectedToBase(
        true, &mode, 2, SupremacyMode::kSouthTeamId));
    EXPECT_FALSE(Harness::ResolveObjectiveConnectedToBase(
        false, &mode, 2, SupremacyMode::kSouthTeamId));

    mode.SetTeamHQ(SupremacyMode::kSouthTeamId, 0);
    mode.SetTeamHQ(SupremacyMode::kNorthTeamId, 1);
    ASSERT_TRUE(mode.UsesSupplyLines());
    EXPECT_FALSE(Harness::ResolveObjectiveConnectedToBase(
        true, &mode, 2, SupremacyMode::kSouthTeamId));
    EXPECT_TRUE(Harness::ResolveObjectiveConnectedToBase(
        false, &mode, 0, SupremacyMode::kSouthTeamId));
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
     ClosedPlayerControllerChannelRejectsNewReliableAllocation) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30112, true, 7, false);
    const uint32_t nextBeforeClose = Harness::NextCh2Reliable(manager, 1);

    Harness::PeerClosePlayerController(manager, 1);

    EXPECT_FALSE(Harness::SendPriorCh2Reliable(manager, 1));
    EXPECT_EQ(Harness::NextCh2Reliable(manager, 1), nextBeforeClose);
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1), 0u);
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
     FullControlWindowDefersRequiredMessageUntilOldestGapIsAcked) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30126u, false, 0u, false);

    for (uint32_t sequence = 1u;
         sequence <= PacketCodec::kReliableBuffer - 1u;
         ++sequence) {
        ASSERT_TRUE(Harness::SendControlReliable(
            manager, 1u, {NMTByte(NMT::Uses),
                           static_cast<uint8_t>(sequence)}));
    }
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 127u);
    const std::vector<uint32_t> packetIds =
        Harness::PendingReliablePacketIds(manager, 1u);
    ASSERT_EQ(packetIds.size(), 127u);

    const std::vector<uint8_t> deferredPayload = {
        NMTByte(NMT::Uses), 0xFEu};
    EXPECT_TRUE(Harness::SendControlReliable(
        manager, 1u, deferredPayload));
    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 127u);
    EXPECT_EQ(Harness::DeferredControlMessageCount(manager, 1u), 1u);
    EXPECT_EQ(Harness::DeferredControlBytes(manager, 1u),
              deferredPayload.size());

    // Successor ACKs become tombstones and cannot reopen capacity while the
    // oldest sequence remains missing.
    for (size_t index = 1u; index < packetIds.size(); ++index) {
        Harness::Acknowledge(manager, 1u, packetIds[index]);
    }
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    EXPECT_EQ(Harness::DeferredControlMessageCount(manager, 1u), 1u);

    Harness::Acknowledge(manager, 1u, packetIds.front());
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    EXPECT_EQ(Harness::DeferredControlMessageCount(manager, 1u), 0u);
    EXPECT_EQ(Harness::DeferredControlBytes(manager, 1u), 0u);
    EXPECT_EQ(Harness::LastPendingReliableSequence(manager, 1u), 128u);
    EXPECT_EQ(Harness::LastPendingReliablePayload(manager, 1u),
              deferredPayload);
}

TEST(ConnectionTravelLifecycle,
     JoinedBootstrapAndGameCallbackWaitForEarlierControlDrain) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30129u, false, 0u, false);
    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, {NMTByte(NMT::Uses), 0x44u}));
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    const uint32_t earlierControlPacket =
        Harness::FirstPendingPacketId(manager, 1u);

    int joinedCallbacks = 0;
    manager.SetClientJoinedCallback(
        [&](const ClientJoinedEvent&) { ++joinedCallbacks; });
    manager.FireClientJoined(ClientJoinedEvent{1u});

    EXPECT_TRUE(Harness::JoinCompletionDeferred(manager, 1u));
    EXPECT_EQ(joinedCallbacks, 0);
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    EXPECT_FALSE(connection->IsHandshakeComplete());
    EXPECT_FALSE(Harness::OutboundActorChannelOpen(manager, 1u, 2u));

    Harness::Acknowledge(manager, 1u, earlierControlPacket);
    EXPECT_FALSE(Harness::JoinCompletionDeferred(manager, 1u));
    EXPECT_EQ(joinedCallbacks, 1);
    EXPECT_TRUE(connection->IsHandshakeComplete());
    EXPECT_TRUE(Harness::OutboundActorChannelOpen(manager, 1u, 2u));
    ASSERT_EQ(Harness::FirstPendingBunchCount(manager, 1u), 2u);
    const std::vector<PacketCodec::Bunch> bootstrap =
        Harness::QueuedReliableBunches(manager, 1u);
    ASSERT_GE(bootstrap.size(), 2u);
    EXPECT_EQ(bootstrap[0].chIndex, 2u);
    EXPECT_TRUE(bootstrap[0].bOpen);
    EXPECT_TRUE(bootstrap[0].bReliable);
    EXPECT_EQ(bootstrap[1].chIndex, 0u);
    EXPECT_TRUE(bootstrap[1].bReliable);
    EXPECT_EQ(bootstrap[1].payload,
              (std::vector<uint8_t>{0x24u, 0x01u, 0x00u, 0x00u, 0x00u}));
    EXPECT_EQ(bootstrap[1].payloadBits, 40u);
}

TEST(ConnectionTravelLifecycle,
     OversizedControlMessageFailClosesInsteadOfAdvancingLifecycle) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30127u, false, 0u, false);

    EXPECT_FALSE(Harness::SendControlReliable(
        manager, 1u, std::vector<uint8_t>(1499u, 0x5Au)));
    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 0u);
    EXPECT_EQ(Harness::DeferredControlMessageCount(manager, 1u), 0u);
}

TEST(ConnectionTravelLifecycle,
     OversizedControlMessageBehindDeferredWorkFailsImmediately) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30130u, false, 0u, false);

    for (uint32_t sequence = 1u;
         sequence <= PacketCodec::kReliableBuffer - 1u;
         ++sequence) {
        ASSERT_TRUE(Harness::SendControlReliable(
            manager, 1u, {NMTByte(NMT::Uses),
                           static_cast<uint8_t>(sequence)}));
    }
    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, {NMTByte(NMT::Uses), 0xEEu}));
    ASSERT_EQ(Harness::DeferredControlMessageCount(manager, 1u), 1u);

    EXPECT_FALSE(Harness::SendControlReliable(
        manager, 1u, std::vector<uint8_t>(1499u, 0x5Au)));
    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::DeferredControlMessageCount(manager, 1u), 1u);
}

TEST(ConnectionTravelLifecycle,
     AckAllocatorFailureRetainsRetryLedgerAndFailsClosed) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30131u, false, 0u, false);
    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, {NMTByte(NMT::Uses), 0x55u}));
    const uint32_t packetId = Harness::FirstPendingPacketId(manager, 1u);
    ASSERT_TRUE(Harness::ReleaseFirstPendingCh0SequenceOutOfBand(
        manager, 1u));

    Harness::Acknowledge(manager, 1u, packetId);

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
}

TEST(ConnectionTravelLifecycle,
     AckAllocatorFailureStopsLaterBunchDispatchInSamePacket) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30134u, false, 0u, false);
    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, {NMTByte(NMT::Uses), 0x56u}));
    const uint32_t packetId = Harness::FirstPendingPacketId(manager, 1u);
    ASSERT_TRUE(Harness::ReleaseFirstPendingCh0SequenceOutOfBand(
        manager, 1u));

    PacketCodec::Bunch actor;
    actor.bReliable = true;
    actor.chIndex = 2u;
    actor.chType = 2u;
    actor.chSequence = 1u;
    actor.payload = {0u};
    actor.payloadBits = 1u;
    PacketCodec::Packet mixed;
    mixed.packetId = 7u;
    mixed.acks.push_back(packetId);
    mixed.bunches.push_back(std::move(actor));

    EXPECT_TRUE(Harness::ParseIncomingControl(
        manager, 1u, EncodeClientPacket(mixed)));

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    EXPECT_EQ(Harness::NextInboundActorReliable(manager, 1u, 2u), 1u);
    EXPECT_FALSE(Harness::InboundPacketDispatchActive(manager, 1u));
}

TEST(ConnectionTravelLifecycle,
     BunchlessReferenceCannotOutrunLatestReliableAttemptByHalfRange) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30132u, false, 0u, false);
    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, {NMTByte(NMT::Uses), 0x66u}));

    constexpr int64_t halfRange =
        static_cast<int64_t>(kMaxPacketId) / 2;
    for (int64_t serial = 1; serial < halfRange; ++serial) {
        Harness::AllocateLocallyRetiredBunchlessPacket(manager, 1u);
    }
    EXPECT_FALSE(Harness::EnsureBunchlessAckReferenceSafe(manager, 1u));
    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
}

TEST(ConnectionTravelLifecycle,
     WrappedWireAckRetiresOnlyMatchingMonotonicPacketGeneration) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30128u, false, 0u, false);

    const std::vector<uint8_t> firstPayload = {
        NMTByte(NMT::Uses), 0x01u};
    ASSERT_TRUE(Harness::SendControlReliable(manager, 1u, firstPayload));
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);

    // Advance and ACK ordinary packet identities without ACKing reliable serial
    // zero. This carries UE3's ACK generation cleanly across the 14-bit wrap.
    for (uint32_t serial = 1u;
         serial < kMaxPacketId;
         ++serial) {
        Harness::AllocateAndResolveUntrackedOutboundPacket(
            manager, 1u);
    }

    const std::vector<uint8_t> wrappedPayload = {
        NMTByte(NMT::Uses), 0x02u};
    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, wrappedPayload));
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 2u);

    // Wire PacketId zero now identifies serial 16384, not the old serial zero.
    Harness::Acknowledge(manager, 1u, 0u);
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    EXPECT_EQ(Harness::LastPendingReliablePayload(manager, 1u),
              firstPayload);

    // The old reliable remains deliverable through a new-generation retry.
    const uint32_t retryPacket =
        Harness::RetransmitFirstPendingReliableForTest(manager, 1u);
    ASSERT_LT(retryPacket, kMaxPacketId);
    Harness::Acknowledge(manager, 1u, retryPacket);
    EXPECT_EQ(Harness::PendingReliableCount(manager, 1u), 0u);
    EXPECT_FALSE(connection->IsDisconnected());
}

TEST(ConnectionTravelLifecycle,
     ReliableRetransmitOwnsAttemptBeforeFailedSocketHandoff) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30133u, false, 0u, false);

    ASSERT_TRUE(Harness::SendControlReliable(
        manager, 1u, {NMTByte(NMT::Uses), 0x42u}));
    const std::vector<int64_t> initialAttempts =
        Harness::FirstPendingPacketSerials(manager, 1u);
    ASSERT_EQ(initialAttempts.size(), 1u);

    // AddClient deliberately has no socket. The retry is still owned before
    // SendRaw reports failure, so either packet identity may later retire the
    // reliable ledger and another timeout remains eligible.
    Harness::ExpireAndRetransmitFirstPendingReliable(manager, 1u);

    const std::vector<int64_t> attempts =
        Harness::FirstPendingPacketSerials(manager, 1u);
    ASSERT_EQ(attempts.size(), 2u);
    EXPECT_EQ(attempts.front(), initialAttempts.front());
    EXPECT_GT(attempts.back(), attempts.front());
    EXPECT_FALSE(connection->IsDisconnected());
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
        Harness::NextCh2Reliable(manager, 1);
    Harness::DeliverActorBunch(manager, 1, exact);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 1u);
    EXPECT_EQ(Harness::NextCh2Reliable(manager, 1),
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
    EXPECT_EQ(Harness::NextCh2Reliable(manager, 1),
              initialReliable + 3u);

    Harness::ResetPossessionRecovery(manager, 1);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, false, false);
    Harness::DeliverActorBunch(manager, 1, exact);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, true);
    Harness::DeliverActorBunch(manager, 1, exact);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::NextCh2Reliable(manager, 1),
              initialReliable + 3u);
}

TEST(ConnectionTravelLifecycle,
     AskForPawnRecoveryWrapsModuloSequenceSpace) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    PacketCodec::Bunch exact = MakeParameterlessPcBunch(42u);
    ASSERT_EQ(exact.payloadBits, 9u);

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30145, true,
        PacketCodec::kMaxChSequence - 2u, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false);
    Harness::DeliverActorBunch(manager, 1, exact);

    const std::vector<uint32_t> expected{
        PacketCodec::kMaxChSequence - 1u, 0u, 1u};
    EXPECT_EQ(Harness::QueuedCh2ReliableSequences(manager, 1), expected);
    EXPECT_EQ(Harness::NextCh2Reliable(manager, 1), 2u);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 1u);
}

TEST(ConnectionTravelLifecycle,
     PossessionRecoveryBackpressureDoesNotConsumeBudget) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    using Decision = Harness::RecoveryDecision;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30146, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 17u);

    const std::vector<uint32_t> occupied =
        Harness::ReservePublishedCh2Reliables(
            manager, 1,
            PacketCodec::OutboundReliableSequencer::kMaximumOutstanding -
                2u);
    ASSERT_EQ(
        occupied.size(),
        PacketCodec::OutboundReliableSequencer::kMaximumOutstanding - 2u);

    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1000u),
              Decision::Backpressured);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 0u);
    EXPECT_FALSE(Harness::PossessionRecoveryLimitLogged(manager, 1));

    ASSERT_TRUE(Harness::ReleaseCh2Reliable(
        manager, 1, occupied.front()));
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1000u),
              Decision::Respond);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 1u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 1000u);
}

TEST(ConnectionTravelLifecycle,
     PossessionRecoveryRejectsStaleAndDeadPawnGenerations) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    using Decision = Harness::RecoveryDecision;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30147, true, 7, false);

    Harness::SetPawnGenerationBindings(
        manager, 1, 9u, 8u, 9u, true);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1000u),
              Decision::StaleGeneration);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);

    Harness::SetPawnGenerationBindings(
        manager, 1, 10u, 10u, 10u, false);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 2000u),
              Decision::Ineligible);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
}

TEST(ConnectionTravelLifecycle,
     NewPawnGenerationClearsPriorAckAndRecoveryBudget) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    using Decision = Harness::RecoveryDecision;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30148, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 20u);

    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 1000u),
              Decision::Respond);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 2000u),
              Decision::Respond);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 3000u),
              Decision::Respond);
    ASSERT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 3u);

    EXPECT_EQ(Harness::AdvanceAndBindLivePawnGeneration(manager, 1), 21u);
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::LastPossessionRecoveryResponseMs(manager, 1), 0u);
    EXPECT_FALSE(Harness::PossessionAcked(manager, 1));

    Harness::DeliverActorBunch(
        manager, 1,
        MakePossessionAckBunch(Harness::LocalPawnChannel()));
    ASSERT_TRUE(Harness::PossessionAcked(manager, 1));

    EXPECT_EQ(Harness::AdvanceAndBindLivePawnGeneration(manager, 1), 22u);
    EXPECT_FALSE(Harness::PossessionAcked(manager, 1));
    EXPECT_EQ(Harness::PossessionRecoveryResponses(manager, 1), 0u);
    EXPECT_EQ(Harness::EvaluatePossessionRecovery(
                  manager, 1, true, 4000u),
              Decision::Respond);
}

TEST(ConnectionTravelLifecycle,
     DuplicateAliveCallbackPreservesPawnGenerationAndAck) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30149, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 33u);
    Harness::DeliverActorBunch(
        manager, 1,
        MakePossessionAckBunch(Harness::LocalPawnChannel()));
    ASSERT_TRUE(Harness::PossessionAcked(manager, 1));

    manager.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(1u), 100, 0, 0, 0,
        /*isDead=*/false, /*sendHealth=*/false,
        /*sendDeathRpc=*/true);

    EXPECT_EQ(Harness::OwningPawnGeneration(manager, 1), 33u);
    EXPECT_TRUE(Harness::PossessionAcked(manager, 1));
}

TEST(ConnectionTravelLifecycle,
     UnreliableStandalonePossessionAckCannotAcknowledgeLivePawn) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30152, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 40u);

    Harness::DeliverActorBunch(
        manager, 1,
        MakePossessionAckBunch(Harness::LocalPawnChannel(),
                               /*reliable=*/false));

    EXPECT_FALSE(Harness::PossessionAcked(manager, 1));
}

TEST(ConnectionTravelLifecycle,
     UnreliableCompoundReadyAckCannotAcknowledgeLivePawn) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30153, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 41u);

    PacketCodec::Bunch readyAck = MakeCapturedPcBunch(
        {0xb2, 0xd1, 0x92, 0x3d, 0x16, 0x47, 0xc3, 0x11}, 62u);
    readyAck.bReliable = false;
    Harness::DeliverActorBunch(manager, 1, readyAck);

    EXPECT_FALSE(Harness::PossessionAcked(manager, 1));
}

TEST(ConnectionTravelLifecycle,
     ReliableCompoundReadyAckAcknowledgesLivePawn) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    Harness::AddClient(
        manager, 1, "127.0.0.1", 30156, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 41u);

    const PacketCodec::Bunch readyAck = MakeCapturedPcBunch(
        {0xb2, 0xd1, 0x92, 0x3d, 0x16, 0x47, 0xc3, 0x11}, 62u);
    Harness::DeliverActorBunch(manager, 1, readyAck);

    EXPECT_TRUE(Harness::PossessionAcked(manager, 1));
}

TEST(ConnectionTravelLifecycle,
     ClientOnDeadBackpressureFailClosesOwningConnection) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1, "127.0.0.1", 30154, true, 7, false);
    Harness::SetPossessionRecoveryEligibility(
        manager, 1, true, true, false, 42u);
    ASSERT_EQ(Harness::ReservePublishedCh2Reliables(
                  manager, 1,
                  PacketCodec::OutboundReliableSequencer::kMaximumOutstanding)
                  .size(),
              PacketCodec::OutboundReliableSequencer::kMaximumOutstanding);

    manager.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(1u), 0, 0, 1, 0,
        /*isDead=*/true, /*sendHealth=*/false,
        /*sendDeathRpc=*/true);

    EXPECT_TRUE(connection->IsDisconnected());
}

TEST(ConnectionTravelLifecycle,
     ChangedTeamsBackpressureDoesNotCommitSelectionAuthority) {
    using Harness = ConnectionTravelLifecycleTestHarness;

    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1, "127.0.0.1", 30155, true, 7, false);
    ASSERT_TRUE(Harness::FreezeRetailBootstrap(manager, 1, {}));
    ASSERT_EQ(Harness::ReservePublishedCh2Reliables(
                  manager, 1,
                  PacketCodec::OutboundReliableSequencer::kMaximumOutstanding)
                  .size(),
              PacketCodec::OutboundReliableSequencer::kMaximumOutstanding);

    Harness::DeliverActorBunch(manager, 1, MakeSelectTeamBunch(1u));

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_FALSE(Harness::TeamSelected(manager, 1));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       RemoteHumanPriUsesLoginBridgePlayerIdInsteadOfTransportId) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    const std::shared_ptr<ClientConnection> target =
        Connect(0u, 41u, TeamMapping::kServerUs);
    const std::shared_ptr<ClientConnection> viewer =
        Connect(1u, 42u, TeamMapping::kServerNva);
    ASSERT_TRUE(target != nullptr);
    ASSERT_TRUE(viewer != nullptr);
    target->SetRetailPlayerId(73);

    const std::optional<PacketCodec::Bunch> remotePri =
        Harness::QueueAuthoritativeRemotePri(
            manager_, viewer->GetClientId(),
            ParticipantId::Human(target->GetClientId()));
    ASSERT_TRUE(remotePri.has_value());

    BitReader reader(remotePri->payload.data(), remotePri->payload.size(),
                     remotePri->payloadBits);
    (void)ActorRepl::ReadNetGUID(reader);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    ActorRepl::ReadCompressedVector(reader, x, y, z);
    constexpr uint32_t maxHandle =
        DeploymentRepl::kRoPlayerReplicationInfoMaxHandle;
    EXPECT_EQ(reader.SerializeInt(maxHandle), 24u);
    (void)reader.ReadInt32();
    EXPECT_EQ(reader.SerializeInt(maxHandle), 28u);
    (void)reader.ReadBit();
    EXPECT_EQ(reader.SerializeInt(maxHandle), 31u);
    (void)reader.ReadBit();
    EXPECT_EQ(reader.SerializeInt(maxHandle), 32u);
    (void)reader.ReadBit();
    EXPECT_EQ(reader.SerializeInt(maxHandle), 33u);
    (void)reader.ReadBit();
    EXPECT_EQ(reader.SerializeInt(maxHandle), 36u);
    EXPECT_EQ(reader.ReadInt32(), 73);
    EXPECT_FALSE(reader.IsOverflowed());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       CanonicalSouthAndNorthFinalRequestsCommitRuntimeSquadAndRoleLedger) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    struct RoleCase {
        uint32_t clientId;
        uint32_t teamId;
        bool south;
        uint32_t roleInfoRef;
    };
    const std::array<RoleCase, 2> cases{{
        {1u, RoleSelectionRepl::kCuChiUsServerTeam, true,
         RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef},
        {2u, RoleSelectionRepl::kCuChiNlfServerTeam, false,
         RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef},
    }};

    RoleSystem* roles = server_.GetRoleSystem();
    ASSERT_TRUE(roles != nullptr);
    for (size_t index = 0; index < cases.size(); ++index) {
        const RoleCase& entry = cases[index];
        ASSERT_TRUE(Connect(index, entry.clientId, entry.teamId) != nullptr);
        const uint32_t nextReliableBefore =
            Harness::NextCh2Reliable(manager_, entry.clientId);

        Harness::DeliverActorBunch(
            manager_, entry.clientId, CuChiFinalRoleBunch(entry.south));

        const std::vector<PacketCodec::Packet> packets =
            DrainDecodedPackets(index);
        ASSERT_FALSE(packets.empty());
        for (const PacketCodec::Packet& packet : packets) {
            ASSERT_TRUE(packet.ok);
        }
        const std::vector<PacketCodec::Bunch> wireBunches =
            FlattenBunches(packets);
        const PacketCodec::Bunch expectedClass = ExpectedOwnerPriClass(
            RoleSelectionRepl::kCuChiInfantryClassIndex);
        const PacketCodec::Bunch expectedTransition = ExpectedChangedRole(
            RoleSelectionRepl::ChangedRoleEvidence{
                255u, RoleSelectionRepl::kCuChiInfantryClassIndex,
                false, true,
                RoleSelectionRepl::ChangedSquadEvidence{0u, 0u}});
        ASSERT_EQ(expectedTransition.payloadBits, 32u);
        EXPECT_EQ(expectedTransition.payload,
                  (std::vector<uint8_t>{0xd2, 0xfe, 0x73, 0x1a}));
        const PacketCodec::Bunch expectedAssignment =
            ExpectedOwnerPriAssignment(0u, 0u);
        const size_t classPosition =
            FindWireBunch(wireBunches, expectedClass);
        const size_t transitionPosition =
            FindWireBunch(wireBunches, expectedTransition);
        const size_t assignmentPosition =
            FindWireBunch(wireBunches, expectedAssignment);
        ASSERT_LT(classPosition, wireBunches.size());
        ASSERT_LT(transitionPosition, wireBunches.size());
        ASSERT_LT(assignmentPosition, wireBunches.size());
        EXPECT_LT(classPosition, transitionPosition);
        EXPECT_LT(transitionPosition, assignmentPosition);
        EXPECT_EQ(wireBunches[transitionPosition].chSequence,
                  nextReliableBefore);

        BitReader classReader(
            wireBunches[classPosition].payload.data(),
            wireBunches[classPosition].payload.size(),
            wireBunches[classPosition].payloadBits);
        EXPECT_EQ(classReader.SerializeInt(
                      RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
                  RoleSelectionRepl::kPriClassIndexHandle);
        EXPECT_EQ(classReader.ReadBits(8),
                  RoleSelectionRepl::kCuChiInfantryClassIndex);
        EXPECT_FALSE(classReader.IsOverflowed());
        EXPECT_EQ(classReader.BitPos(),
                  wireBunches[classPosition].payloadBits);

        BitReader assignmentReader(
            wireBunches[assignmentPosition].payload.data(),
            wireBunches[assignmentPosition].payload.size(),
            wireBunches[assignmentPosition].payloadBits);
        EXPECT_EQ(assignmentReader.SerializeInt(
                      RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
                  RoleSelectionRepl::kPriSquadIndexHandle);
        EXPECT_EQ(assignmentReader.ReadBits(8), 0u);
        EXPECT_EQ(assignmentReader.SerializeInt(
                      RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
                  RoleSelectionRepl::kPriRoleIndexHandle);
        EXPECT_EQ(assignmentReader.ReadBits(8), 0u);
        EXPECT_FALSE(assignmentReader.IsOverflowed());
        EXPECT_EQ(assignmentReader.BitPos(),
                  wireBunches[assignmentPosition].payloadBits);

        const auto assignment =
            roles->GetRetailSquadAssignment(entry.clientId);
        ASSERT_TRUE(assignment.has_value());
        EXPECT_EQ(assignment->teamId, entry.teamId);
        EXPECT_EQ(assignment->squadIndex, 0u);
        EXPECT_EQ(assignment->roleIndex, 0u);
        EXPECT_EQ(roles->GetRoleCount(entry.teamId, CombatRole::Rifleman), 1);

        const Harness::RoleLedgerSnapshot ledger =
            Harness::RoleLedger(manager_, entry.clientId);
        EXPECT_TRUE(ledger.accepted);
        EXPECT_TRUE(ledger.finalized);
        EXPECT_TRUE(ledger.priClassReplicated);
        EXPECT_EQ(ledger.roleInfoObjectRef, entry.roleInfoRef);
        EXPECT_EQ(ledger.classIndex,
                  RoleSelectionRepl::kCuChiInfantryClassIndex);
        EXPECT_EQ(ledger.squadIndex, 0u);
        EXPECT_EQ(ledger.roleIndex, 0u);
        ASSERT_TRUE(ledger.changedRole.has_value());
        EXPECT_EQ(ledger.changedRole->squadIndex, 255u);
        EXPECT_EQ(ledger.changedRole->classIndex,
                  RoleSelectionRepl::kCuChiInfantryClassIndex);
        EXPECT_FALSE(ledger.changedRole->showLobby);
        EXPECT_TRUE(ledger.changedRole->showSpawnSelect);
        ASSERT_TRUE(ledger.changedRole->followingChangedSquad.has_value());
        EXPECT_EQ(ledger.changedRole->followingChangedSquad->squadIndex, 0u);
        EXPECT_EQ(ledger.changedRole->followingChangedSquad->roleIndex, 0u);

        const auto deployment =
            Harness::DeploymentState(manager_, entry.clientId);
        ASSERT_TRUE(deployment.has_value());
        EXPECT_TRUE(deployment->roleFinalized);
        EXPECT_FALSE(deployment->selectedSlot.has_value());
        EXPECT_FALSE(deployment->selectedSpawnId.has_value());
        EXPECT_FALSE(deployment->deploymentAuthorized);
        EXPECT_FALSE(Harness::DeploymentPrepared(manager_, entry.clientId));
        EXPECT_EQ(Harness::NextCh2Reliable(manager_, entry.clientId),
                  nextReliableBefore + 1u);
        EXPECT_EQ(Harness::PendingReliableCount(manager_, entry.clientId),
                  static_cast<size_t>(1));
    }
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       SameConnectionFactionSwitchDrainsGraphAndPreservesReliableCursors) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 31u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    ASSERT_TRUE(Connect(0u, kClientId, kSouthTeam) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    SupremacyMode* supremacy =
        Harness::InstallActiveSupremacyMode(server_);
    ASSERT_TRUE(supremacy != nullptr);
    ASSERT_EQ(supremacy->GetPhase(), SupremacyMode::Phase::Active);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);
    EXPECT_FALSE(player->IsAlive());

    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> initialSouthSpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(initialSouthSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *initialSouthSpawn));
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kSouthTeam);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_TRUE(player->IsAlive());

    const auto expectPendingSequences =
        [this](uint32_t channel,
               std::initializer_list<uint32_t> expected) {
            EXPECT_EQ(
                ConnectionTravelLifecycleTestHarness::
                    PendingOwningGraphSequences(
                        manager_, kClientId, channel),
                std::vector<uint32_t>(expected));
        };
    const auto expectPendingClose =
        [this](uint32_t channel,
               std::initializer_list<uint32_t> expected) {
            EXPECT_EQ(
                ConnectionTravelLifecycleTestHarness::
                    PendingOwningGraphSequences(
                        manager_, kClientId, channel,
                        /*openOnly=*/false, /*closeOnly=*/true),
                std::vector<uint32_t>(expected));
        };

    // Current-emitter oracle: these values are derived from the persistent
    // per-channel sequencers. They are emulator invariants, not constants
    // copied from a retail same-connection faction switch (none was captured).
    expectPendingSequences(209u, {1u, 2u, 3u, 4u, 5u});
    for (const uint32_t channel :
         std::array<uint32_t, 5>{210u, 211u, 212u, 213u, 214u}) {
        expectPendingSequences(channel, {1u, 2u});
        EXPECT_EQ(Harness::NextOwningGraphReliable(
                      manager_, kClientId, channel),
                  3u);
    }
    expectPendingSequences(219u, {1u});
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 209u),
              6u);
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 219u),
              2u);
    const std::optional<uint32_t> southOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(southOpenPacket.has_value());
    ASSERT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);

    players->OnPlayerDeath(kClientId);
    ASSERT_FALSE(player->IsAlive());
    teams->AddPlayerToTeam(kClientId, kNorthTeam);
    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> northSpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(northSpawn.has_value());

    // The opposite graph cannot mutate authoritative spawn state until every
    // prior reliable and every close in the South cohort has drained.
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *northSpawn));
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kSouthTeam);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    ASSERT_EQ(Harness::PendingReliableCount(manager_, kClientId), 2u);
    expectPendingClose(209u, {6u});
    for (const uint32_t channel :
         std::array<uint32_t, 5>{210u, 211u, 212u, 213u, 214u}) {
        expectPendingClose(channel, {3u});
    }
    expectPendingClose(219u, {2u});
    const std::optional<uint32_t> southClosePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(southClosePacket.has_value());
    EXPECT_NE(*southClosePacket, *southOpenPacket);

    // ACKing the newer close datagram first records the close receipt, but the
    // older open ledger still pins every per-channel issuance window.
    Harness::Acknowledge(manager_, kClientId, *southClosePacket);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
    EXPECT_EQ(Harness::PendingOwningGraphSequences(
                  manager_, kClientId, 209u,
                  /*openOnly=*/true, /*closeOnly=*/false),
              (std::vector<uint32_t>{1u}));

    // The older ACK completes the whole barrier and resumes the deferred North
    // deployment exactly once.
    Harness::Acknowledge(manager_, kClientId, *southOpenPacket);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kNorthTeam);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_TRUE(player->IsAlive());
    ASSERT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
    const std::optional<uint32_t> northOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(northOpenPacket.has_value());
    EXPECT_NE(*northOpenPacket, *southOpenPacket);
    EXPECT_NE(*northOpenPacket, *southClosePacket);

    expectPendingSequences(209u, {7u, 8u, 9u, 10u, 11u});
    expectPendingSequences(210u, {4u, 5u});
    expectPendingSequences(212u, {4u, 5u});
    expectPendingSequences(214u, {4u, 5u});
    expectPendingSequences(219u, {3u});
    expectPendingSequences(211u, {});
    expectPendingSequences(213u, {});
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 211u),
              4u);
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 213u),
              4u);

    const size_t northPendingBeforeDuplicate =
        Harness::PendingReliableCount(manager_, kClientId);
    Harness::Acknowledge(manager_, kClientId, *southClosePacket);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId),
              northPendingBeforeDuplicate);
    EXPECT_EQ(Harness::PendingOwningGraphPacket(
                  manager_, kClientId, 209u,
                  /*open=*/true, /*close=*/false),
              northOpenPacket);

    Harness::Acknowledge(manager_, kClientId, *northOpenPacket);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 0u);

    // North deliberately omits ch211/ch213. Switching back to South proves
    // those dormant cursors continue at four while active North channels have
    // advanced through their own close generation.
    players->OnPlayerDeath(kClientId);
    ASSERT_FALSE(player->IsAlive());
    teams->AddPlayerToTeam(kClientId, kSouthTeam);
    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> southSpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(southSpawn.has_value());
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *southSpawn));
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
    expectPendingClose(209u, {12u});
    expectPendingClose(210u, {6u});
    expectPendingClose(212u, {6u});
    expectPendingClose(214u, {6u});
    expectPendingClose(219u, {4u});
    expectPendingClose(211u, {});
    expectPendingClose(213u, {});
    const std::optional<uint32_t> northClosePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(northClosePacket.has_value());

    Harness::Acknowledge(manager_, kClientId, *northClosePacket);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kSouthTeam);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_TRUE(player->IsAlive());
    ASSERT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
    const std::optional<uint32_t> southReopenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(southReopenPacket.has_value());

    expectPendingSequences(209u, {13u, 14u, 15u, 16u, 17u});
    expectPendingSequences(210u, {7u, 8u});
    expectPendingSequences(211u, {4u, 5u});
    expectPendingSequences(212u, {7u, 8u});
    expectPendingSequences(213u, {4u, 5u});
    expectPendingSequences(214u, {7u, 8u});
    expectPendingSequences(219u, {5u});

    Harness::Acknowledge(manager_, kClientId, *northClosePacket);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
    EXPECT_EQ(Harness::PendingOwningGraphPacket(
                  manager_, kClientId, 209u,
                  /*open=*/true, /*close=*/false),
              southReopenPacket);
    Harness::Acknowledge(manager_, kClientId, *southReopenPacket);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 0u);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       LiveH170UpdatesRemotePriAndRetiresSeededPawnBeforeNorthRedeploy) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kViewerId = 51u;
    constexpr uint32_t kSwitcherId = 52u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    const std::shared_ptr<ClientConnection> viewer =
        Connect(0u, kViewerId, kSouthTeam);
    const std::shared_ptr<ClientConnection> switcher =
        Connect(1u, kSwitcherId, kSouthTeam);
    ASSERT_TRUE(viewer != nullptr);
    ASSERT_TRUE(switcher != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kSwitcherId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kSwitcherId);
    const std::optional<uint32_t> southSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kSwitcherId);
    ASSERT_TRUE(southSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kSwitcherId, *southSpawn));
    const std::optional<uint32_t> southOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kSwitcherId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(southOpenPacket.has_value());
    Harness::Acknowledge(manager_, kSwitcherId, *southOpenPacket);
    player->SetReadyToSpawn(true);
    ASSERT_TRUE(player->IsAlive());
    ASSERT_TRUE(player->IsReadyToSpawn());

    const ParticipantId remoteSwitcher = ParticipantId::Human(kSwitcherId);
    Harness::SynchronizeRemoteParticipantPris(manager_, kViewerId);
    (void)DrainDecodedPackets(0u);
    (void)DrainDecodedPackets(1u);
    const std::optional<uint32_t> priChannel =
        Harness::RemotePriChannel(manager_, kViewerId, remoteSwitcher);
    const std::optional<uint32_t> pawnChannel =
        Harness::RemotePawnChannel(manager_, kViewerId, remoteSwitcher);
    ASSERT_TRUE(priChannel.has_value());
    ASSERT_TRUE(pawnChannel.has_value());
    // Remote pawn visuals remain deliberately gated until their complete
    // templates are grounded. Seed the already-open binding state to exercise
    // the h170 retirement path that becomes reachable when that gate is enabled.
    const std::optional<uint32_t> seededPawnOpenSequence =
        Harness::SeedRemotePawnBindingOpen(
            manager_, kViewerId, remoteSwitcher, kSouthTeam);
    ASSERT_TRUE(seededPawnOpenSequence.has_value());
    ASSERT_EQ(Harness::RemotePawnState(
                  manager_, kViewerId, remoteSwitcher),
              ParticipantActorOpenState::Open);
    EXPECT_EQ(Harness::RemotePriTeamInfoChannel(
                  manager_, kViewerId, remoteSwitcher),
              std::optional<uint32_t>{5u});
    EXPECT_EQ(Harness::RemotePawnServerTeam(
                  manager_, kViewerId, remoteSwitcher),
              std::optional<uint32_t>{kSouthTeam});

    Harness::DeliverActorBunch(
        manager_, kSwitcherId, MakeSelectTeamBunch(/*retail NLF=*/0u));

    EXPECT_FALSE(switcher->IsDisconnected());
    EXPECT_EQ(teams->GetPlayerTeam(kSwitcherId), kNorthTeam);
    EXPECT_EQ(player->GetTeam(), kNorthTeam);
    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(player->GetHealth(), 0);
    EXPECT_FALSE(player->IsReadyToSpawn());
    EXPECT_FALSE(Harness::Spawned(manager_, kSwitcherId));
    EXPECT_EQ(Harness::RemotePawnState(
                  manager_, kViewerId, remoteSwitcher),
              ParticipantActorOpenState::Closing);
    EXPECT_EQ(Harness::RemotePriTeamInfoChannel(
                  manager_, kViewerId, remoteSwitcher),
              std::optional<uint32_t>{4u});
    // A stale ready bit must not let PlayerManager bypass the fresh h175/h261/
    // h434 transaction while the old graph is still present.
    players->Update();
    EXPECT_FALSE(player->IsAlive());
    EXPECT_FALSE(player->IsReadyToSpawn());

    const std::vector<PacketCodec::Bunch> viewerUpdates =
        FlattenBunches(DrainDecodedPackets(0u));
    BitWriter northTeamWriter;
    ASSERT_TRUE(DeploymentRepl::WriteRemotePriTeam(
        northTeamWriter, /*North TeamInfo ch=*/4u));
    const auto teamDelta = std::find_if(
        viewerUpdates.begin(), viewerUpdates.end(),
        [&](const PacketCodec::Bunch& bunch) {
            return bunch.bReliable && bunch.chIndex == *priChannel &&
                   bunch.payloadBits == northTeamWriter.NumBits() &&
                   bunch.payload == northTeamWriter.GetBytes();
        });
    const auto pawnClose = std::find_if(
        viewerUpdates.begin(), viewerUpdates.end(),
        [&](const PacketCodec::Bunch& bunch) {
            return bunch.bReliable && bunch.bControl && bunch.bClose &&
                   bunch.chIndex == *pawnChannel;
        });
    ASSERT_TRUE(teamDelta != viewerUpdates.end());
    ASSERT_TRUE(pawnClose != viewerUpdates.end());
    EXPECT_EQ(pawnClose->chSequence,
              (*seededPawnOpenSequence + 1u) % PacketCodec::kMaxChSequence);
    EXPECT_LT(std::distance(viewerUpdates.begin(), teamDelta),
              std::distance(viewerUpdates.begin(), pawnClose));

    Harness::DeliverActorBunch(
        manager_, kSwitcherId, CuChiFinalRoleBunch(/*south=*/false));
    (void)DrainDecodedPackets(1u);
    Harness::DeliverActorBunch(manager_, kSwitcherId, SpawnSelectBunch());
    (void)DrainDecodedPackets(1u);
    Harness::DeliverActorBunch(manager_, kSwitcherId, ReadyBunch());

    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kSwitcherId),
              Harness::OwningGraphPhase::Closing);
    EXPECT_FALSE(Harness::Spawned(manager_, kSwitcherId));
    EXPECT_FALSE(player->IsAlive());
    const std::optional<uint32_t> southClosePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kSwitcherId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(southClosePacket.has_value());

    // This detached fixture has no GameServer-owned NetworkManager to relay
    // SpawnSystem's OnPlayerSpawn callback back into this ConnectionManager.
    // Model that production callback boundary before the deferred commit.
    Harness::ModelOwningPawnSpawnCallback(manager_, kSwitcherId);
    Harness::Acknowledge(manager_, kSwitcherId, *southClosePacket);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kSwitcherId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kSwitcherId), kNorthTeam);
    EXPECT_TRUE(Harness::Spawned(manager_, kSwitcherId));
    EXPECT_TRUE(player->IsAlive());
    EXPECT_FALSE(switcher->IsDisconnected());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       CloseAckPacketRunsForceOnlyButSuppressesRetiringPawnRpc) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 53u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> southSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(southSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *southSpawn));
    const std::optional<uint32_t> openPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(openPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *openPacket);

    players->OnPlayerDeath(kClientId);
    teams->AddPlayerToTeam(kClientId, kNorthTeam);
    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> northSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(northSpawn.has_value());
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *northSpawn));
    const std::optional<uint32_t> closePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(closePacket.has_value());

    PacketCodec::Bunch bufferedMantle;
    bufferedMantle.bReliable = true;
    bufferedMantle.chIndex = 209u;
    bufferedMantle.chType = 2u;
    bufferedMantle.chSequence = 2u;
    bufferedMantle.payload = {0x55u};
    bufferedMantle.payloadBits = 7u;
    PacketCodec::Packet beforeBarrier;
    beforeBarrier.packetId = 90u;
    beforeBarrier.bunches.push_back(std::move(bufferedMantle));
    Harness::Deliver(manager_, EncodeClientPacket(beforeBarrier),
                     connection->GetIP(), connection->GetPort());
    ASSERT_EQ(Harness::NextInboundActorReliable(
                  manager_, kClientId, 209u),
              1u);

    PacketCodec::Bunch mantle;
    mantle.bReliable = true;
    mantle.chIndex = 209u;
    mantle.chType = 2u;
    mantle.chSequence = 1u;
    mantle.payload = {0x55u};
    mantle.payloadBits = 7u;
    PacketCodec::Bunch forceOnly = ForceOnlyBunch();
    forceOnly.chSequence = 1u;
    PacketCodec::Packet inbound;
    inbound.packetId = 100u;
    inbound.acks.push_back(*closePacket);
    inbound.bunches.push_back(std::move(mantle));
    inbound.bunches.push_back(std::move(forceOnly));
    Harness::Deliver(manager_, EncodeClientPacket(inbound),
                     connection->GetIP(), connection->GetPort());

    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_FALSE(Harness::MantlePawnStarted(manager_, kClientId));
    EXPECT_EQ(Harness::NextInboundActorReliable(
                  manager_, kClientId, 209u),
              3u);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closed);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(player->IsAlive());
    const auto deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(deployment->deploymentAuthorized);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       PacketFloorRetiresOldReliableAndAcceptsEmptyGapFillSuccessor) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 54u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> southSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(southSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *southSpawn));
    const std::optional<uint32_t> openPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(openPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *openPacket);

    players->OnPlayerDeath(kClientId);
    teams->AddPlayerToTeam(kClientId, kNorthTeam);
    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> northSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(northSpawn.has_value());
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *northSpawn));
    const std::optional<uint32_t> closePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(closePacket.has_value());

    PacketCodec::Packet barrier;
    barrier.packetId = 200u;
    barrier.acks.push_back(*closePacket);
    Harness::Deliver(manager_, EncodeClientPacket(barrier),
                     connection->GetIP(), connection->GetPort());
    ASSERT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    ASSERT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kNorthTeam);
    ASSERT_EQ(Harness::NextInboundActorReliable(
                  manager_, kClientId, 212u),
              1u);

    PacketCodec::Bunch oldStart;
    oldStart.bReliable = true;
    oldStart.chIndex = 212u;
    oldStart.chType = 2u;
    oldStart.chSequence = 1u;
    oldStart.payload = {0x1du};
    oldStart.payloadBits = 8u;
    PacketCodec::Packet delayedOld;
    delayedOld.packetId = 199u;
    delayedOld.bunches.push_back(std::move(oldStart));
    Harness::Deliver(manager_, EncodeClientPacket(delayedOld),
                     connection->GetIP(), connection->GetPort());
    EXPECT_FALSE(Harness::WeaponFiring(manager_, kClientId, 212u));
    EXPECT_EQ(Harness::NextInboundActorReliable(
                  manager_, kClientId, 212u),
              2u);

    // UE3 SetChannelActor fills PendingOutRec sequence gaps with empty reliable
    // bunches when the actor index is reused. The successor must advance the
    // persistent cursor without reviving old-faction semantics.
    PacketCodec::Bunch gapFill;
    gapFill.bReliable = true;
    gapFill.chIndex = 212u;
    gapFill.chType = 2u;
    gapFill.chSequence = 2u;
    PacketCodec::Packet successor;
    successor.packetId = 201u;
    successor.bunches.push_back(std::move(gapFill));
    Harness::Deliver(manager_, EncodeClientPacket(successor),
                     connection->GetIP(), connection->GetPort());
    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_EQ(Harness::NextInboundActorReliable(
                  manager_, kClientId, 212u),
              3u);
    EXPECT_FALSE(Harness::WeaponFiring(manager_, kClientId, 212u));
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       PeerFixedCloseStopsLaterH170AndWeaponRpcInSamePacket) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 55u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> spawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(spawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *spawn));
    ASSERT_TRUE(player->IsAlive());

    PacketCodec::Bunch peerClose;
    peerClose.bControl = true;
    peerClose.bClose = true;
    peerClose.bReliable = true;
    peerClose.chIndex = 209u;
    peerClose.chType = 2u;
    peerClose.chSequence = 1u;
    PacketCodec::Bunch h170 = MakeSelectTeamBunch(/*retail NLF=*/0u);
    h170.chSequence = 1u;
    PacketCodec::Bunch oldStart;
    oldStart.bReliable = true;
    oldStart.chIndex = 212u;
    oldStart.chType = 2u;
    oldStart.chSequence = 1u;
    oldStart.payload = {0x1du};
    oldStart.payloadBits = 8u;
    PacketCodec::Packet inbound;
    inbound.packetId = 300u;
    inbound.bunches.push_back(std::move(peerClose));
    inbound.bunches.push_back(std::move(h170));
    inbound.bunches.push_back(std::move(oldStart));
    Harness::Deliver(manager_, EncodeClientPacket(inbound),
                     connection->GetIP(), connection->GetPort());

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Broken);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_EQ(Harness::ActiveWeaponChannel(manager_, kClientId), 0u);
    EXPECT_FALSE(Harness::WeaponFiring(manager_, kClientId, 212u));
    // The terminal close was the first bunch; the later team RPC must not run.
    EXPECT_EQ(teams->GetPlayerTeam(kClientId), kSouthTeam);
    EXPECT_EQ(player->GetTeam(), kSouthTeam);
    EXPECT_TRUE(player->IsAlive());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       DeferredFactionSwitchExpiresWhenRoundEndsBeforeCloseAck) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 32u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    ASSERT_TRUE(Connect(0u, kClientId, kSouthTeam) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    SupremacyMode* supremacy =
        Harness::InstallActiveSupremacyMode(server_);
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    ASSERT_TRUE(supremacy != nullptr);
    ASSERT_EQ(supremacy->GetPhase(), SupremacyMode::Phase::Active);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> initialSpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(initialSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *initialSpawn));
    const std::optional<uint32_t> initialOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(initialOpenPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *initialOpenPacket);
    ASSERT_EQ(Harness::PendingReliableCount(manager_, kClientId), 0u);

    players->OnPlayerDeath(kClientId);
    ASSERT_FALSE(player->IsAlive());
    teams->AddPlayerToTeam(kClientId, kNorthTeam);
    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> northSpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(northSpawn.has_value());
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *northSpawn));
    const std::optional<uint32_t> closePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(closePacket.has_value());
    ASSERT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);

    supremacy->EndRound();
    ASSERT_EQ(supremacy->GetPhase(), SupremacyMode::Phase::PostRound);
    Harness::Acknowledge(manager_, kClientId, *closePacket);

    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closed);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), 0u);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 0u);
    EXPECT_FALSE(Harness::PendingOwningGraphPacket(
        manager_, kClientId, 209u,
        /*open=*/true, /*close=*/false).has_value());
    const auto expired = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(expired.has_value());
    EXPECT_TRUE(expired->roleFinalized);
    EXPECT_FALSE(expired->selectedSpawnId.has_value());
    EXPECT_FALSE(expired->deploymentAuthorized);

    // A later legal window requires a fresh spawn selection + Ready and opens
    // from the already-drained Closed state exactly once.
    supremacy->StartRound();
    supremacy->Update(supremacy->GetPhaseTimeRemaining());
    ASSERT_EQ(supremacy->GetPhase(), SupremacyMode::Phase::Active);
    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> retrySpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(retrySpawn.has_value());
    EXPECT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *retrySpawn));
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kNorthTeam);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_TRUE(player->IsAlive());
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       TicketDepletionAtCloseAckLeavesFreshAuthorizationRetry) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 33u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    TicketSystem* tickets = Harness::InstallTicketSystem(
        server_, /*southTickets=*/1u, /*northTickets=*/1u);
    SupremacyMode* supremacy =
        Harness::InstallActiveSupremacyMode(server_);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(tickets != nullptr);
    ASSERT_TRUE(supremacy != nullptr);
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    // Drive the retail h175/h261/h434 path so the captured ChangedRole tuple is
    // available when deferred ticket depletion must recover the client UI.
    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    Harness::DeliverActorBunch(
        manager_, kClientId, CuChiFinalRoleBunch(/*south=*/true));
    (void)DrainDecodedPackets(0u);
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    ASSERT_TRUE(Harness::Spawned(manager_, kClientId));
    const std::optional<uint32_t> initialOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(initialOpenPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *initialOpenPacket);

    Harness::DeliverActorBunch(
        manager_, kClientId, MakeSelectTeamBunch(/*retail NLF=*/0u));
    (void)DrainDecodedPackets(0u);
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    ASSERT_FALSE(player->IsAlive());
    ASSERT_EQ(teams->GetPlayerTeam(kClientId), kNorthTeam);
    Harness::DeliverActorBunch(
        manager_, kClientId, CuChiFinalRoleBunch(/*south=*/false));
    (void)DrainDecodedPackets(0u);
    const Harness::RoleLedgerSnapshot northRole =
        Harness::RoleLedger(manager_, kClientId);
    ASSERT_TRUE(northRole.changedRole.has_value());
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    const std::optional<uint32_t> closePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(closePacket.has_value());

    tickets->SetTickets(kNorthTeam, 0u);
    ASSERT_FALSE(tickets->HasTickets(kNorthTeam));
    (void)DrainDecodedPackets(0u);
    Harness::Acknowledge(manager_, kClientId, *closePacket);

    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closed);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(player->IsAlive());
    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
    const auto depleted = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(depleted.has_value());
    EXPECT_TRUE(depleted->roleFinalized);
    EXPECT_FALSE(depleted->selectedSpawnId.has_value());
    EXPECT_FALSE(depleted->deploymentAuthorized);

    const std::vector<PacketCodec::Bunch> recovery =
        FlattenBunches(DrainDecodedPackets(0u));
    const PacketCodec::Bunch expectedStopAutoSpawn =
        ExpectedTempStopAutoSpawn();
    const PacketCodec::Bunch expectedRecovery =
        ExpectedChangedRole(*northRole.changedRole);
    const size_t stopIndex =
        FindWireBunch(recovery, expectedStopAutoSpawn);
    const size_t changedRoleIndex =
        FindWireBunch(recovery, expectedRecovery);
    ASSERT_TRUE(stopIndex < changedRoleIndex);
    ASSERT_TRUE(changedRoleIndex < recovery.size());
    EXPECT_EQ(recovery[changedRoleIndex].chSequence,
              recovery[stopIndex].chSequence + 1u);
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 0u);

    tickets->AddTickets(kNorthTeam, 1u);
    ASSERT_TRUE(tickets->HasTickets(kNorthTeam));
    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    Harness::ModelOwningPawnSpawnCallback(manager_, kClientId);
    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kNorthTeam);
    EXPECT_TRUE(Harness::Spawned(manager_, kClientId));
    EXPECT_TRUE(player->IsAlive());
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId), 1u);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       TicketDepletionRecoveryNeedsTwoCh2SlotsAtomically) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 36u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    TicketSystem* tickets = Harness::InstallTicketSystem(
        server_, /*southTickets=*/1u, /*northTickets=*/1u);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(tickets != nullptr);
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    // Reach a real opposite-faction close barrier with a capture-grounded
    // ChangedRole tuple available for the ticket-depletion UI recovery.
    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    Harness::DeliverActorBunch(
        manager_, kClientId, CuChiFinalRoleBunch(/*south=*/true));
    (void)DrainDecodedPackets(0u);
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    ASSERT_TRUE(Harness::Spawned(manager_, kClientId));
    const std::optional<uint32_t> initialOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(initialOpenPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *initialOpenPacket);

    Harness::DeliverActorBunch(
        manager_, kClientId, MakeSelectTeamBunch(/*retail NLF=*/0u));
    (void)DrainDecodedPackets(0u);
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    ASSERT_FALSE(player->IsAlive());
    ASSERT_EQ(teams->GetPlayerTeam(kClientId), kNorthTeam);
    Harness::DeliverActorBunch(
        manager_, kClientId, CuChiFinalRoleBunch(/*south=*/false));
    (void)DrainDecodedPackets(0u);
    ASSERT_TRUE(Harness::RoleLedger(manager_, kClientId)
                    .changedRole.has_value());
    Harness::AcknowledgeAllPendingReliables(manager_, kClientId);
    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    ASSERT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    const std::optional<uint32_t> closePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(closePacket.has_value());

    // h262 + h210 is one atomic two-sequence recovery. Leaving exactly one
    // slot must not publish h59, consume a cursor, or emit half the pair.
    const std::vector<uint32_t> occupied =
        Harness::ReservePublishedCh2Reliables(
            manager_, kClientId,
            PacketCodec::OutboundReliableSequencer::kMaximumOutstanding -
                1u);
    ASSERT_EQ(
        occupied.size(),
        PacketCodec::OutboundReliableSequencer::kMaximumOutstanding - 1u);
    const uint32_t ch2CursorBefore =
        Harness::NextCh2Reliable(manager_, kClientId);
    const size_t outstandingBefore =
        Harness::Ch2OutstandingCount(manager_, kClientId);
    const uint32_t packetBefore =
        Harness::NextOutboundPacketId(manager_, kClientId);
    (void)DrainDecodedPackets(0u);

    tickets->SetTickets(kNorthTeam, 0u);
    Harness::Acknowledge(manager_, kClientId, *closePacket);

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closed);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(Harness::NextCh2Reliable(manager_, kClientId),
              ch2CursorBefore);
    EXPECT_EQ(Harness::Ch2OutstandingCount(manager_, kClientId),
              outstandingBefore);
    EXPECT_EQ(Harness::NextOutboundPacketId(manager_, kClientId),
              packetBefore);
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    const auto depleted = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(depleted.has_value());
    EXPECT_TRUE(depleted->roleFinalized);
    EXPECT_FALSE(depleted->selectedSpawnId.has_value());
    EXPECT_FALSE(depleted->deploymentAuthorized);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       Ch2BackpressurePreflightCannotMutateAuthoritativeSpawn) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 34u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    SpawnSystem* spawns = server_.GetSpawnSystem();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(spawns != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(
        manager_, kClientId);
    const std::optional<uint32_t> selectedSpawn =
        Harness::PrepareDeploymentForCurrentTeam(
            manager_, kClientId);
    ASSERT_TRUE(selectedSpawn.has_value());
    SpawnLocation* location = spawns->GetSpawnLocation(*selectedSpawn);
    ASSERT_TRUE(location != nullptr);

    const Vector3 positionBefore = player->GetPosition();
    const Vector3 orientationBefore = player->GetOrientation();
    const PlayerState stateBefore = player->GetState();
    const int healthBefore = player->GetHealth();
    const bool readyBefore = player->IsReadyToSpawn();
    const float cooldownBefore = location->spawnCooldown;
    const uint32_t graphCursorBefore =
        Harness::NextOwningGraphReliable(
            manager_, kClientId, 209u);

    ASSERT_EQ(
        Harness::ReservePublishedCh2Reliables(
            manager_, kClientId,
            PacketCodec::OutboundReliableSequencer::
                kMaximumOutstanding).size(),
        PacketCodec::OutboundReliableSequencer::kMaximumOutstanding);
    const uint32_t ch2CursorBefore =
        Harness::NextCh2Reliable(manager_, kClientId);
    const size_t pendingBefore =
        Harness::PendingReliableCount(manager_, kClientId);

    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *selectedSpawn));

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(player->GetState(), stateBefore);
    EXPECT_EQ(player->GetHealth(), healthBefore);
    EXPECT_EQ(player->IsReadyToSpawn(), readyBefore);
    EXPECT_FLOAT_EQ(player->GetPosition().x, positionBefore.x);
    EXPECT_FLOAT_EQ(player->GetPosition().y, positionBefore.y);
    EXPECT_FLOAT_EQ(player->GetPosition().z, positionBefore.z);
    EXPECT_FLOAT_EQ(player->GetOrientation().x, orientationBefore.x);
    EXPECT_FLOAT_EQ(player->GetOrientation().y, orientationBefore.y);
    EXPECT_FLOAT_EQ(player->GetOrientation().z, orientationBefore.z);
    EXPECT_FLOAT_EQ(location->spawnCooldown, cooldownBefore);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Unopened);
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 209u),
              graphCursorBefore);
    EXPECT_EQ(Harness::NextCh2Reliable(manager_, kClientId),
              ch2CursorBefore);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId),
              pendingBefore);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       ReusedGraphNeedsSixCh2SlotsBeforeAuthoritativeRespawn) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 56u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    ASSERT_TRUE(players != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> initialSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(initialSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *initialSpawn));
    const std::optional<uint32_t> graphOpenPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(graphOpenPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *graphOpenPacket);

    players->OnPlayerDeath(kClientId);
    manager_.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(kClientId), 0, 0, 1, 0,
        /*isDead=*/true, /*sendHealth=*/false, /*sendDeathRpc=*/true);
    ASSERT_FALSE(player->IsAlive());
    ASSERT_FALSE(Harness::Spawned(manager_, kClientId));
    if (Harness::PendingReliableCount(manager_, kClientId) != 0u) {
        Harness::Acknowledge(
            manager_, kClientId,
            Harness::FirstPendingPacketId(manager_, kClientId));
    }

    const std::optional<uint32_t> respawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(respawn.has_value());
    const Vector3 positionBefore = player->GetPosition();
    const uint32_t graphCursorBefore =
        Harness::NextOwningGraphReliable(manager_, kClientId, 209u);
    ASSERT_EQ(
        Harness::ReservePublishedCh2Reliables(
            manager_, kClientId,
            PacketCodec::OutboundReliableSequencer::kMaximumOutstanding -
                5u).size(),
        PacketCodec::OutboundReliableSequencer::kMaximumOutstanding - 5u);
    const uint32_t ch2CursorBefore =
        Harness::NextCh2Reliable(manager_, kClientId);

    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *respawn));

    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(player->GetHealth(), 0);
    EXPECT_FLOAT_EQ(player->GetPosition().x, positionBefore.x);
    EXPECT_FLOAT_EQ(player->GetPosition().y, positionBefore.y);
    EXPECT_FLOAT_EQ(player->GetPosition().z, positionBefore.z);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Open);
    EXPECT_EQ(Harness::PawnGraphTeam(manager_, kClientId), kSouthTeam);
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 209u),
              graphCursorBefore);
    EXPECT_EQ(Harness::NextCh2Reliable(manager_, kClientId),
              ch2CursorBefore);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       DisconnectedTransportRejectsPreparedDeploymentWithoutMutation) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 57u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    ASSERT_TRUE(players != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);
    const std::optional<uint32_t> spawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(spawn.has_value());

    const Vector3 positionBefore = player->GetPosition();
    const uint64_t lifeBefore = player->GetLifecycleGeneration();
    const uint32_t graphCursorBefore =
        Harness::NextOwningGraphReliable(manager_, kClientId, 209u);
    connection->MarkDisconnected();

    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *spawn));
    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(player->GetLifecycleGeneration(), lifeBefore);
    EXPECT_FLOAT_EQ(player->GetPosition().x, positionBefore.x);
    EXPECT_FLOAT_EQ(player->GetPosition().y, positionBefore.y);
    EXPECT_FLOAT_EQ(player->GetPosition().z, positionBefore.z);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Unopened);
    EXPECT_EQ(Harness::NextOwningGraphReliable(
                  manager_, kClientId, 209u),
              graphCursorBefore);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       InvalidTeamAtCommitFailsClosedWithoutAuthoritativeSpawn) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 60u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);
    const std::optional<uint32_t> spawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(spawn.has_value());
    const Vector3 positionBefore = player->GetPosition();
    const uint64_t lifeBefore = player->GetLifecycleGeneration();
    teams->RemovePlayer(kClientId);
    ASSERT_EQ(teams->GetPlayerTeam(kClientId), 0u);

    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *spawn));

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_FALSE(player->IsAlive());
    EXPECT_EQ(player->GetLifecycleGeneration(), lifeBefore);
    EXPECT_FLOAT_EQ(player->GetPosition().x, positionBefore.x);
    EXPECT_FLOAT_EQ(player->GetPosition().y, positionBefore.y);
    EXPECT_FLOAT_EQ(player->GetPosition().z, positionBefore.z);
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Broken);
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       NegativeCombatScoreCannotSuppressAuthoritativeSpawnLifecycle) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 58u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    ASSERT_TRUE(players != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);
    ASSERT_FALSE(player->IsAlive());
    const uint64_t generationBefore =
        Harness::OwningPawnGeneration(manager_, kClientId);
    players->SetPlayerScore(kClientId, -1);
    players->OnPlayerSpawn(kClientId);
    ASSERT_TRUE(player->IsAlive());

    manager_.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(kClientId), player->GetHealth(), 0, 0, -1,
        /*isDead=*/false, /*sendHealth=*/true, /*sendDeathRpc=*/true);

    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_TRUE(Harness::OwningPawnAlive(manager_, kClientId));
    EXPECT_GT(Harness::OwningPawnGeneration(manager_, kClientId),
              generationBefore);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       DuplicateDeathWhileGraphClosingPreservesDeferredToken) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 59u;
    constexpr uint32_t kSouthTeam =
        RoleSelectionRepl::kCuChiUsServerTeam;
    constexpr uint32_t kNorthTeam =
        RoleSelectionRepl::kCuChiNlfServerTeam;

    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, kClientId, kSouthTeam);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(Harness::InstallActiveSupremacyMode(server_) != nullptr);
    PlayerManager* players = server_.GetPlayerManager();
    TeamManager* teams = server_.GetTeamManager();
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(teams != nullptr);
    const std::shared_ptr<Player> player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    Harness::PrepareOwningPawnLifeForFactionSwitch(manager_, kClientId);
    const std::optional<uint32_t> southSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(southSpawn.has_value());
    ASSERT_TRUE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *southSpawn));
    const std::optional<uint32_t> openPacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/true, /*close=*/false);
    ASSERT_TRUE(openPacket.has_value());
    Harness::Acknowledge(manager_, kClientId, *openPacket);

    players->OnPlayerDeath(kClientId);
    manager_.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(kClientId), 0, 0, 1, 0,
        /*isDead=*/true, /*sendHealth=*/false, /*sendDeathRpc=*/true);
    teams->AddPlayerToTeam(kClientId, kNorthTeam);
    const std::optional<uint32_t> northSpawn =
        Harness::PrepareDeploymentForCurrentTeam(manager_, kClientId);
    ASSERT_TRUE(northSpawn.has_value());
    EXPECT_FALSE(Harness::ExecutePreparedDeployment(
        manager_, kClientId, *northSpawn));
    const std::optional<uint32_t> closePacket =
        Harness::PendingOwningGraphPacket(
            manager_, kClientId, 209u,
            /*open=*/false, /*close=*/true);
    ASSERT_TRUE(closePacket.has_value());
    ASSERT_FALSE(Harness::OwningPawnAlive(manager_, kClientId));
    ASSERT_TRUE(Harness::HasDeferredOwningPawnDeployment(
        manager_, kClientId));

    manager_.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(kClientId), 0, 0, 1, 0,
        /*isDead=*/true, /*sendHealth=*/false, /*sendDeathRpc=*/true);

    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_EQ(Harness::PawnGraphPhase(manager_, kClientId),
              Harness::OwningGraphPhase::Closing);
    EXPECT_TRUE(Harness::HasDeferredOwningPawnDeployment(
        manager_, kClientId));
    EXPECT_FALSE(Harness::OwningPawnAlive(manager_, kClientId));
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    EXPECT_FALSE(player->IsAlive());
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       FreshFinalAfterDeathResetsAuthorizationAndRequiresFreshReady) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 41u;
    constexpr uint32_t kTeamId = RoleSelectionRepl::kCuChiUsServerTeam;

    RoleSystem* roles = server_.GetRoleSystem();
    PlayerManager* players = server_.GetPlayerManager();
    ASSERT_TRUE(roles != nullptr);
    ASSERT_TRUE(players != nullptr);
    ASSERT_TRUE(Connect(0u, kClientId, kTeamId) != nullptr);
    const auto player = players->GetPlayer(kClientId);
    ASSERT_TRUE(player != nullptr);

    // Establish the first complete Cu Chi role -> spawn authorization using the
    // same standalone h175, h261, and h434 RPCs sent by the retail client.
    Harness::DeliverActorBunch(manager_, kClientId, CuChiFinalRoleBunch(true));
    (void)DrainDecodedPackets(0u);
    const auto initialAssignment = roles->GetRetailSquadAssignment(kClientId);
    ASSERT_TRUE(initialAssignment.has_value());
    ASSERT_EQ(initialAssignment->teamId, kTeamId);
    ASSERT_EQ(initialAssignment->squadIndex, 0u);
    ASSERT_EQ(initialAssignment->roleIndex, 0u);

    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    auto deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    ASSERT_EQ(deployment->selectedSlot, std::optional<uint8_t>{0u});
    ASSERT_TRUE(deployment->selectedSpawnId.has_value());
    const uint32_t selectedSpawnId = *deployment->selectedSpawnId;
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(deployment->deploymentAuthorized);

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    ASSERT_EQ(deployment->selectedSlot, std::optional<uint8_t>{0u});
    ASSERT_EQ(deployment->selectedSpawnId,
              std::optional<uint32_t>{selectedSpawnId});
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::Ready);
    EXPECT_TRUE(deployment->deploymentAuthorized);
    EXPECT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));

    // The detached fixture has no live game-mode clock, so model the completed
    // authoritative spawn boundary while retaining the coordinator state that
    // ExecutePreparedDeployment intentionally keeps after a successful spawn.
    players->OnPlayerSpawn(kClientId);
    player->SetReadyToSpawn(true);
    Harness::SetPossessionRecoveryEligibility(
        manager_, kClientId, true, true, false, 1u);
    ASSERT_TRUE(player->IsAlive());
    ASSERT_TRUE(player->IsReadyToSpawn());
    ASSERT_TRUE(Harness::Spawned(manager_, kClientId));

    players->OnPlayerDeath(kClientId);
    manager_.ReplicateRetailParticipantCombatState(
        ParticipantId::Human(kClientId), 0, 0, 1, 0,
        /*isDead=*/true, /*sendHealth=*/false, /*sendDeathRpc=*/true);
    (void)DrainDecodedPackets(0u);
    EXPECT_FALSE(player->IsAlive());
    EXPECT_FALSE(Harness::Spawned(manager_, kClientId));
    // The authoritative death boundary invalidates the completed deployment
    // transaction immediately. The following same-role final is idempotent
    // over this already-clean state and must not be required for correctness.
    deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    EXPECT_TRUE(deployment->roleFinalized);
    EXPECT_FALSE(deployment->selectedSlot.has_value());
    EXPECT_FALSE(deployment->selectedSpawnId.has_value());
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(deployment->deploymentAuthorized);
    EXPECT_FALSE(player->IsReadyToSpawn());

    Harness::DeliverActorBunch(manager_, kClientId, CuChiFinalRoleBunch(true));
    (void)DrainDecodedPackets(0u);

    deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    EXPECT_TRUE(deployment->roleFinalized);
    EXPECT_FALSE(deployment->selectedSlot.has_value());
    EXPECT_FALSE(deployment->selectedSpawnId.has_value());
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(deployment->deploymentAuthorized);
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, kClientId));
    EXPECT_FALSE(player->IsReadyToSpawn());

    const auto assignmentAfterFinal =
        roles->GetRetailSquadAssignment(kClientId);
    ASSERT_TRUE(assignmentAfterFinal.has_value());
    EXPECT_EQ(assignmentAfterFinal->teamId, initialAssignment->teamId);
    EXPECT_EQ(assignmentAfterFinal->squadIndex,
              initialAssignment->squadIndex);
    EXPECT_EQ(assignmentAfterFinal->roleIndex,
              initialAssignment->roleIndex);
    EXPECT_EQ(assignmentAfterFinal->generation,
              initialAssignment->generation);
    EXPECT_EQ(roles->GetRoleCount(kTeamId, CombatRole::Rifleman), 1);

    Harness::DeliverActorBunch(manager_, kClientId, SpawnSelectBunch());
    (void)DrainDecodedPackets(0u);
    deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    ASSERT_EQ(deployment->selectedSlot, std::optional<uint8_t>{0u});
    ASSERT_EQ(deployment->selectedSpawnId,
              std::optional<uint32_t>{selectedSpawnId});
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::ForceOnly);
    EXPECT_FALSE(deployment->deploymentAuthorized);

    Harness::DeliverActorBunch(manager_, kClientId, ReadyBunch());
    deployment = Harness::DeploymentState(manager_, kClientId);
    ASSERT_TRUE(deployment.has_value());
    EXPECT_EQ(deployment->selectedSlot, std::optional<uint8_t>{0u});
    EXPECT_EQ(deployment->selectedSpawnId,
              std::optional<uint32_t>{selectedSpawnId});
    EXPECT_EQ(deployment->readyStatus,
              DeploymentCoordinator::ReadyStatus::Ready);
    EXPECT_TRUE(deployment->deploymentAuthorized);
    EXPECT_TRUE(Harness::DeploymentPrepared(manager_, kClientId));
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       SamePacketTeamThenRoleReconcilesProductionFillBots) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    Harness::InstallProductionBotFill(server_);
    RoleSystem* roles = server_.GetRoleSystem();
    BotManager* bots = server_.GetBotManager();
    ASSERT_TRUE(roles != nullptr);
    ASSERT_TRUE(bots != nullptr);
    ASSERT_EQ(bots->CountBots(BotManager::kTeamOne), 8u);
    ASSERT_EQ(bots->CountBots(BotManager::kTeamTwo), 8u);

    // Start on the other side and first reconcile its human count so h170 must
    // exercise both directions of the fixed-fill transaction: remove a team-one
    // bot and refill the vacated team-two bot. Human(1) intentionally collides
    // numerically with Bot(1).
    const std::shared_ptr<ClientConnection> connection =
        Connect(0u, 1u, BotManager::kTeamTwo);
    ASSERT_TRUE(connection != nullptr);
    bots->SetHumanTeamCounts(0u, 1u);
    EXPECT_EQ(bots->CountBots(BotManager::kTeamOne), 8u);
    EXPECT_EQ(bots->CountBots(BotManager::kTeamTwo), 7u);
    EXPECT_FALSE(roles->GetRetailSquadAssignment(
        ParticipantId::Bot(16u)).has_value());

    PacketCodec::Bunch selectTeam = MakeSelectTeamBunch(1u);
    PacketCodec::Bunch selectRole = CuChiFinalRoleBunch(true);
    selectTeam.chSequence = 1u;
    selectRole.chSequence = 2u;
    PacketCodec::Packet inbound;
    inbound.packetId = 1u;
    inbound.bunches.push_back(std::move(selectTeam));
    inbound.bunches.push_back(std::move(selectRole));
    Harness::Deliver(manager_, EncodeClientPacket(inbound),
                     connection->GetIP(), connection->GetPort());

    // BotManager applies the emulator's deterministic highest-id eviction and
    // refills the old team synchronously before the following h175 in this same
    // packet chooses a squad.
    EXPECT_EQ(bots->CountBots(BotManager::kTeamOne), 7u);
    EXPECT_EQ(bots->CountBots(BotManager::kTeamTwo), 8u);
    EXPECT_FALSE(roles->GetRetailSquadAssignment(
        ParticipantId::Bot(8u)).has_value());
    EXPECT_TRUE(roles->GetRetailSquadAssignment(
        ParticipantId::Bot(17u)).has_value());
    EXPECT_TRUE(roles->GetRetailSquadAssignment(
        ParticipantId::Bot(1u)).has_value());

    const std::vector<PacketCodec::Bunch> wireBunches =
        FlattenBunches(DrainDecodedPackets(0u));
    const PacketCodec::Bunch expectedTransition = ExpectedChangedRole(
        RoleSelectionRepl::ChangedRoleEvidence{
            255u, RoleSelectionRepl::kCuChiInfantryClassIndex,
            false, true,
            RoleSelectionRepl::ChangedSquadEvidence{1u, 1u}});
    const PacketCodec::Bunch expectedAssignment =
        ExpectedOwnerPriAssignment(1u, 1u);
    EXPECT_LT(FindWireBunch(wireBunches, expectedTransition),
              wireBunches.size());
    EXPECT_LT(FindWireBunch(wireBunches, expectedAssignment),
              wireBunches.size());

    const auto human = roles->GetRetailSquadAssignment(1u);
    const auto sameValueBot = roles->GetRetailSquadAssignment(
        ParticipantId::Bot(1u));
    ASSERT_TRUE(human.has_value());
    ASSERT_TRUE(sameValueBot.has_value());
    EXPECT_EQ(human->teamId, BotManager::kTeamOne);
    EXPECT_EQ(human->squadIndex, 1u);
    EXPECT_EQ(human->roleIndex, 1u);
    EXPECT_EQ(sameValueBot->squadIndex, 0u);
    EXPECT_EQ(sameValueBot->roleIndex, 0u);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       InstalledArtifactRejectsFinalRoleBeforeAnyAuthorityMutation) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    constexpr uint32_t kClientId = 7u;
    constexpr uint32_t kTeamId = RoleSelectionRepl::kCuChiUsServerTeam;
    ASSERT_TRUE(Connect(0u, kClientId, kTeamId) != nullptr);
    ASSERT_TRUE(Harness::FreezeInstalledCuChiSession(manager_, kClientId));

    RoleSystem* roles = server_.GetRoleSystem();
    ASSERT_TRUE(roles != nullptr);
    const Harness::RoleLedgerSnapshot before =
        Harness::RoleLedger(manager_, kClientId);
    const uint32_t nextReliableBefore =
        Harness::NextCh2Reliable(manager_, kClientId);
    const size_t pendingBefore =
        Harness::PendingReliableCount(manager_, kClientId);

    Harness::DeliverActorBunch(
        manager_, kClientId, CuChiFinalRoleBunch(true));

    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    EXPECT_FALSE(roles->GetRetailSquadAssignment(kClientId).has_value());
    EXPECT_EQ(roles->GetRoleCount(kTeamId, CombatRole::Rifleman), 0);
    const Harness::RoleLedgerSnapshot after =
        Harness::RoleLedger(manager_, kClientId);
    EXPECT_EQ(after.accepted, before.accepted);
    EXPECT_EQ(after.finalized, before.finalized);
    EXPECT_EQ(after.priClassReplicated, before.priClassReplicated);
    EXPECT_EQ(after.roleInfoObjectRef, before.roleInfoObjectRef);
    EXPECT_EQ(after.classIndex, before.classIndex);
    EXPECT_EQ(after.squadIndex, before.squadIndex);
    EXPECT_EQ(after.roleIndex, before.roleIndex);
    EXPECT_EQ(after.changedRole.has_value(), before.changedRole.has_value());
    EXPECT_EQ(Harness::NextCh2Reliable(manager_, kClientId),
              nextReliableBefore);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, kClientId),
              pendingBefore);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       SameTeamClientsUseConsecutiveSlotsAndRepeatFinalOmitsChangedSquad) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    RoleSystem* roles = server_.GetRoleSystem();
    ASSERT_TRUE(roles != nullptr);

    const auto deliverFinalPacket =
        [this](const std::shared_ptr<ClientConnection>& connection,
               uint32_t chSequence, uint32_t packetId) {
            PacketCodec::Bunch bunch = CuChiFinalRoleBunch(true);
            bunch.chSequence = chSequence;
            PacketCodec::Packet packet;
            packet.packetId = packetId;
            packet.bunches.push_back(std::move(bunch));
            ConnectionTravelLifecycleTestHarness::Deliver(
                manager_, EncodeClientPacket(packet), connection->GetIP(),
                connection->GetPort());
        };

    const std::shared_ptr<ClientConnection> first = Connect(
        0u, 21u, RoleSelectionRepl::kCuChiUsServerTeam);
    ASSERT_TRUE(first != nullptr);
    deliverFinalPacket(first, 1u, 1u);
    const std::vector<PacketCodec::Packet> firstPackets =
        DrainDecodedPackets(0u);
    ASSERT_FALSE(firstPackets.empty());
    for (const PacketCodec::Packet& packet : firstPackets) {
        ASSERT_TRUE(packet.ok);
    }

    const std::shared_ptr<ClientConnection> second = Connect(
        1u, 22u, RoleSelectionRepl::kCuChiUsServerTeam);
    ASSERT_TRUE(second != nullptr);
    deliverFinalPacket(second, 1u, 1u);
    const std::vector<PacketCodec::Packet> secondPackets =
        DrainDecodedPackets(1u);
    ASSERT_FALSE(secondPackets.empty());
    for (const PacketCodec::Packet& packet : secondPackets) {
        ASSERT_TRUE(packet.ok);
    }

    const auto firstAssignment = roles->GetRetailSquadAssignment(21u);
    const auto secondAssignment = roles->GetRetailSquadAssignment(22u);
    ASSERT_TRUE(firstAssignment.has_value());
    ASSERT_TRUE(secondAssignment.has_value());
    EXPECT_EQ(firstAssignment->squadIndex, 0u);
    EXPECT_EQ(firstAssignment->roleIndex, 0u);
    EXPECT_EQ(secondAssignment->squadIndex, 0u);
    EXPECT_EQ(secondAssignment->roleIndex, 1u);
    EXPECT_EQ(roles->GetRoleCount(
                  RoleSelectionRepl::kCuChiUsServerTeam,
                  CombatRole::Rifleman),
              2);

    const uint32_t repeatReliableBefore =
        Harness::NextCh2Reliable(manager_, 21u);
    // Sequence two is a fresh request, not a retransmission of the first h175.
    deliverFinalPacket(first, 2u, 2u);

    const std::vector<PacketCodec::Packet> repeatPackets =
        DrainDecodedPackets(0u);
    ASSERT_FALSE(repeatPackets.empty());
    for (const PacketCodec::Packet& packet : repeatPackets) {
        ASSERT_TRUE(packet.ok);
    }
    const std::vector<PacketCodec::Bunch> repeatBunches =
        FlattenBunches(repeatPackets);
    EXPECT_EQ(std::count_if(
                  repeatBunches.begin(), repeatBunches.end(),
                  [](const PacketCodec::Bunch& bunch) {
                      return bunch.bReliable && bunch.chIndex == 2u;
                  }),
              static_cast<std::ptrdiff_t>(1));

    const PacketCodec::Bunch expectedRepeat = ExpectedChangedRole(
        RoleSelectionRepl::ChangedRoleEvidence{
            0u, RoleSelectionRepl::kCuChiInfantryClassIndex,
            false, true, std::nullopt});
    const size_t repeatPosition =
        FindWireBunch(repeatBunches, expectedRepeat);
    ASSERT_LT(repeatPosition, repeatBunches.size());
    EXPECT_EQ(repeatBunches[repeatPosition].chSequence,
              repeatReliableBefore);

    BitReader repeatReader(
        repeatBunches[repeatPosition].payload.data(),
        repeatBunches[repeatPosition].payload.size(),
        repeatBunches[repeatPosition].payloadBits);
    EXPECT_EQ(repeatReader.SerializeInt(
                  RoleSelectionRepl::kRoPlayerControllerMaxHandle),
              RoleSelectionRepl::kChangedRoleHandle);
    EXPECT_FALSE(repeatReader.ReadBit()); // SquadIndex default: existing squad 0
    EXPECT_FALSE(repeatReader.ReadBit()); // ClassIndex default: infantry class 0
    EXPECT_FALSE(repeatReader.ReadBit()); // bShowRoleSelectLobby
    EXPECT_TRUE(repeatReader.ReadBit());  // bShowSpawnSelect
    EXPECT_FALSE(repeatReader.IsOverflowed());
    EXPECT_EQ(repeatReader.BitsLeft(), 0u); // no following h211 ChangedSquad

    EXPECT_EQ(Harness::NextCh2Reliable(manager_, 21u),
              repeatReliableBefore + 1u);
    const Harness::RoleLedgerSnapshot repeatLedger =
        Harness::RoleLedger(manager_, 21u);
    ASSERT_TRUE(repeatLedger.changedRole.has_value());
    EXPECT_EQ(repeatLedger.changedRole->squadIndex, 0u);
    EXPECT_FALSE(
        repeatLedger.changedRole->followingChangedSquad.has_value());

    const auto assignmentAfterRepeat =
        roles->GetRetailSquadAssignment(21u);
    ASSERT_TRUE(assignmentAfterRepeat.has_value());
    EXPECT_EQ(assignmentAfterRepeat->squadIndex, 0u);
    EXPECT_EQ(assignmentAfterRepeat->roleIndex, 0u);
    EXPECT_EQ(roles->GetRoleCount(
                  RoleSelectionRepl::kCuChiUsServerTeam,
                  CombatRole::Rifleman),
              2);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       FinalRoleBackpressureFailClosesAndCleanupReleasesAssignment) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    RoleSystem* roles = server_.GetRoleSystem();
    ASSERT_TRUE(roles != nullptr);

    const std::shared_ptr<ClientConnection> saturated = Connect(
        0u, 31u, RoleSelectionRepl::kCuChiUsServerTeam);
    ASSERT_TRUE(saturated != nullptr);
    ASSERT_EQ(Harness::ReservePublishedCh2Reliables(
                  manager_, 31u,
                  PacketCodec::OutboundReliableSequencer::
                      kMaximumOutstanding)
                  .size(),
              PacketCodec::OutboundReliableSequencer::
                  kMaximumOutstanding);

    Harness::DeliverActorBunch(manager_, 31u, CuChiFinalRoleBunch(true));

    EXPECT_TRUE(saturated->IsDisconnected());
    const auto failedAssignment = roles->GetRetailSquadAssignment(31u);
    ASSERT_TRUE(failedAssignment.has_value());
    EXPECT_EQ(failedAssignment->squadIndex, 0u);
    EXPECT_EQ(failedAssignment->roleIndex, 0u);
    auto failedDeployment = Harness::DeploymentState(manager_, 31u);
    ASSERT_TRUE(failedDeployment.has_value());
    EXPECT_TRUE(failedDeployment->roleFinalized);
    EXPECT_FALSE(failedDeployment->selectedSlot.has_value());
    EXPECT_FALSE(failedDeployment->selectedSpawnId.has_value());
    EXPECT_FALSE(failedDeployment->deploymentAuthorized);
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, 31u));

    // A disconnected endpoint cannot turn the finalized coordinator state into
    // a deployment authorization while waiting for housekeeping cleanup.
    BitWriter selectSpawnWriter;
    selectSpawnWriter.SerializeInt(
        DeploymentRepl::kServerSetSpawnSelectHandle,
        DeploymentRepl::kRoPlayerControllerMaxHandle);
    selectSpawnWriter.WriteBit(true);
    selectSpawnWriter.WriteByte(
        DeploymentCoordinator::kNormalSpawnSelectionBase);
    PacketCodec::Bunch selectSpawn;
    selectSpawn.bReliable = true;
    selectSpawn.chIndex = 2u;
    selectSpawn.chType = 2u;
    selectSpawn.chSequence = 2u;
    selectSpawn.payload = selectSpawnWriter.GetBytes();
    selectSpawn.payloadBits =
        static_cast<uint32_t>(selectSpawnWriter.NumBits());
    PacketCodec::Packet selectSpawnPacket;
    selectSpawnPacket.packetId = 2u;
    selectSpawnPacket.bunches.push_back(std::move(selectSpawn));
    Harness::Deliver(
        manager_, EncodeClientPacket(selectSpawnPacket), saturated->GetIP(),
        saturated->GetPort());

    failedDeployment = Harness::DeploymentState(manager_, 31u);
    ASSERT_TRUE(failedDeployment.has_value());
    EXPECT_FALSE(failedDeployment->selectedSlot.has_value());
    EXPECT_FALSE(failedDeployment->selectedSpawnId.has_value());
    EXPECT_FALSE(failedDeployment->deploymentAuthorized);
    EXPECT_FALSE(Harness::DeploymentPrepared(manager_, 31u));

    manager_.RemoveStaleConnections();

    EXPECT_TRUE(manager_.GetConnection(31u) == nullptr);
    EXPECT_FALSE(Harness::DeploymentState(manager_, 31u).has_value());
    EXPECT_FALSE(roles->GetRetailSquadAssignment(31u).has_value());
    EXPECT_EQ(roles->GetRoleCount(
                  RoleSelectionRepl::kCuChiUsServerTeam,
                  CombatRole::Rifleman),
              0);

    const std::shared_ptr<ClientConnection> replacement = Connect(
        0u, 32u, RoleSelectionRepl::kCuChiUsServerTeam);
    ASSERT_TRUE(replacement != nullptr);
    Harness::DeliverActorBunch(manager_, 32u, CuChiFinalRoleBunch(true));
    EXPECT_FALSE(replacement->IsDisconnected());
    const auto replacementAssignment =
        roles->GetRetailSquadAssignment(32u);
    ASSERT_TRUE(replacementAssignment.has_value());
    EXPECT_EQ(replacementAssignment->squadIndex, 0u);
    EXPECT_EQ(replacementAssignment->roleIndex, 0u);
    EXPECT_EQ(roles->GetRoleCount(
                  RoleSelectionRepl::kCuChiUsServerTeam,
                  CombatRole::Rifleman),
              1);
}

TEST_F(ConnectionTravelCuChiRoleIntegrationTest,
       LockedAndFullSquadRejectionsLeaveRoleTransactionUnchanged) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    RoleSystem* roles = server_.GetRoleSystem();
    ASSERT_TRUE(roles != nullptr);

    ASSERT_TRUE(Connect(0u, 11u, RoleSelectionRepl::kCuChiUsServerTeam) !=
                nullptr);
    for (uint8_t squad = 0; squad < roles->GetActiveRetailSquadCount();
         ++squad) {
        ASSERT_TRUE(roles->SetRetailSquadLocked(
            RoleSelectionRepl::kCuChiUsServerTeam, squad, true));
    }
    const uint32_t lockedNextReliable = Harness::NextCh2Reliable(manager_, 11u);
    Harness::DeliverActorBunch(manager_, 11u, CuChiFinalRoleBunch(true));
    EXPECT_TRUE(DrainDecodedPackets(0u).empty());
    EXPECT_FALSE(roles->GetRetailSquadAssignment(11u).has_value());
    EXPECT_EQ(roles->GetRoleCount(
                  RoleSelectionRepl::kCuChiUsServerTeam,
                  CombatRole::Rifleman),
              0);
    const Harness::RoleLedgerSnapshot locked = Harness::RoleLedger(manager_, 11u);
    EXPECT_FALSE(locked.accepted);
    EXPECT_FALSE(locked.finalized);
    EXPECT_FALSE(locked.priClassReplicated);
    EXPECT_FALSE(locked.changedRole.has_value());
    EXPECT_EQ(locked.squadIndex, 255u);
    EXPECT_EQ(locked.roleIndex, 255u);
    EXPECT_EQ(Harness::NextCh2Reliable(manager_, 11u), lockedNextReliable);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, 11u),
              static_cast<size_t>(0));

    ASSERT_TRUE(Connect(1u, 12u, RoleSelectionRepl::kCuChiNlfServerTeam) !=
                nullptr);
    const size_t northCapacity =
        static_cast<size_t>(roles->GetActiveRetailSquadCount()) *
        RetailSquad::SLOT_COUNT;
    for (size_t slot = 0; slot < northCapacity; ++slot) {
        const RetailSquadAssignment occupied = roles->AutoAssignRetailSquad(
            1000u + static_cast<uint32_t>(slot),
            RoleSelectionRepl::kCuChiNlfServerTeam);
        ASSERT_TRUE(occupied.IsValid());
    }
    const uint32_t fullNextReliable = Harness::NextCh2Reliable(manager_, 12u);
    Harness::DeliverActorBunch(manager_, 12u, CuChiFinalRoleBunch(false));
    EXPECT_TRUE(DrainDecodedPackets(1u).empty());
    EXPECT_FALSE(roles->GetRetailSquadAssignment(12u).has_value());
    EXPECT_EQ(roles->GetRoleCount(
                  RoleSelectionRepl::kCuChiNlfServerTeam,
                  CombatRole::Rifleman),
              0);
    const Harness::RoleLedgerSnapshot full = Harness::RoleLedger(manager_, 12u);
    EXPECT_FALSE(full.accepted);
    EXPECT_FALSE(full.finalized);
    EXPECT_FALSE(full.priClassReplicated);
    EXPECT_FALSE(full.changedRole.has_value());
    EXPECT_EQ(full.squadIndex, 255u);
    EXPECT_EQ(full.roleIndex, 255u);
    EXPECT_EQ(Harness::NextCh2Reliable(manager_, 12u), fullNextReliable);
    EXPECT_EQ(Harness::PendingReliableCount(manager_, 12u),
              static_cast<size_t>(0));
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
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
        manager, 1, "127.0.0.1", 30140, true, 7, false);
    ASSERT_TRUE(connection != nullptr);
    // Establish the production generation while the detached fixture has no
    // role-finalized state that would require a ChangedRole publication.
    ConnectionTravelLifecycleTestHarness::BeginDeploymentGeneration(manager);
    ConnectionTravelLifecycleTestHarness::SetPublishedSpawnSelectionState(
        manager, 1);
    ConnectionTravelLifecycleTestHarness::ResetDetachedDeploymentClient(
        manager, 1);
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
    ConnectionTravelLifecycleTestHarness::ResetDetachedDeploymentClient(
        manager, 1);
    ASSERT_FALSE(connection->IsDisconnected());
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
    EXPECT_TRUE(connection->IsDisconnected());
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
     DefaultActorBootstrapUsesLiveLayoutForEverySupportedProfile) {
    struct LayoutCase {
        std::string_view variant;
        uint32_t playerController;
        uint32_t gameReplicationInfo;
        uint32_t teamInfo;
        uint32_t playerReplicationInfo;
    };
    const std::vector<LayoutCase> layouts{
        {{}, 57520u, 70887u, 90245u, 86701u},
        {"installed", 57522u, 70889u, 90248u, 86704u},
    };
    struct ProfileCase {
        std::string_view mapUrl;
        std::string_view mode;
        uint32_t canonicalGameClass;
        uint32_t installedGameClass;
    };
    const std::vector<ProfileCase> profiles{
        {"VNTE-Resort", "Territories", 69601u, 69603u},
        {"VNTE-CuChi", "Territories", 69601u, 69603u},
        {"VNSU-HueCity", "Supremacy", 70442u, 70444u},
        {"VNSK-Compound", "Skirmish", 70363u, 70365u},
    };
    ScopedEnvironmentVariable replayWorld(
        "RS2V_REPLAY_CAPTURE_WORLD", "0");

    for (const ProfileCase& profile : profiles) {
        for (const LayoutCase& expected : layouts) {
            ConnectionManager manager(nullptr);
            const std::shared_ptr<ClientConnection> connection =
                ConnectionTravelLifecycleTestHarness::AddClient(
                    manager, 1, "127.0.0.1", 30134, true, 0, false);
            ASSERT_TRUE(
                ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
                    manager, 1, expected.variant, profile.mapUrl,
                    profile.mode));

            ConnectionTravelLifecycleTestHarness::SendActorBootstrap(
                manager, 1);
            const std::vector<PacketCodec::Bunch> queued =
                ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(
                    manager, 1);

            ASSERT_FALSE(connection->IsDisconnected());
            ASSERT_GE(queued.size(), 2u);
            EXPECT_EQ(queued[0].chIndex, 2u);
            EXPECT_TRUE(queued[0].bOpen);
            EXPECT_EQ(queued[1].chIndex, 0u);
            EXPECT_EQ(
                queued[1].payload,
                (std::vector<uint8_t>{0x24u, 0x01u, 0x00u, 0x00u, 0x00u}));
            EXPECT_EQ(queued[1].payloadBits, 40u);
            EXPECT_EQ(
                ConnectionTravelLifecycleTestHarness::FirstPendingBunchCount(
                    manager, 1),
                2u);

            std::vector<uint32_t> openedChannels;
            for (const PacketCodec::Bunch& bunch : queued) {
                if (bunch.bOpen && !bunch.bClose) {
                    openedChannels.push_back(bunch.chIndex);
                }
            }
            std::sort(openedChannels.begin(), openedChannels.end());
            openedChannels.erase(
                std::unique(openedChannels.begin(), openedChannels.end()),
                openedChannels.end());
            EXPECT_EQ(openedChannels,
                      (std::vector<uint32_t>{2u, 3u, 4u, 5u, 26u}));
            EXPECT_EQ(ConnectionTravelLifecycleTestHarness::GriChannel(
                          manager, 1),
                      3u);

            const auto priLink = std::find_if(
                queued.begin(), queued.end(),
                [](const PacketCodec::Bunch& bunch) {
                    return bunch.bReliable && !bunch.bOpen &&
                           !bunch.bClose && bunch.chIndex == 2u &&
                           bunch.payloadBits == 20u;
                });
            ASSERT_TRUE(priLink != queued.end());
            EXPECT_EQ(priLink->chSequence, 2u);
            EXPECT_EQ(priLink->payload,
                      (std::vector<uint8_t>{0x17u, 0x6au, 0x00u}));

            std::vector<uint32_t> ch2ReliableSequences;
            for (const PacketCodec::Bunch& bunch : queued) {
                if (bunch.bReliable && bunch.chIndex == 2u) {
                    ch2ReliableSequences.push_back(bunch.chSequence);
                }
            }
            EXPECT_EQ(ch2ReliableSequences,
                      (std::vector<uint32_t>{1u, 2u, 3u, 4u}));

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

            BitReader pcReader(pc->payload.data(), pc->payload.size(),
                               pc->payloadBits);
            (void)ActorRepl::ReadNetGUID(pcReader);
            float pcX = 0.0f, pcY = 0.0f, pcZ = 0.0f;
            ActorRepl::ReadCompressedVector(
                pcReader, pcX, pcY, pcZ);
            EXPECT_EQ(pcReader.ReadByte(), 0u);
            EXPECT_FALSE(pcReader.IsOverflowed());
            EXPECT_EQ(pcReader.BitPos(), pc->payloadBits);

            // Both playable TeamInfo opens must seed h62 before spawn selection.
            // With no GameServer/TicketSystem in this harness, initial==0 means an
            // unlimited pool and therefore the positive retail display sentinel.
            const std::array<
                std::pair<const PacketCodec::Bunch*, int32_t>, 2>
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
                ActorRepl::ReadCompressedVector(
                    teamReader, teamX, teamY, teamZ);
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
            EXPECT_EQ(griReader.SerializeInt(184u),
                      ObjectiveRepl::kBalanceTeams);
            EXPECT_TRUE(griReader.ReadBit());
            EXPECT_EQ(griReader.SerializeInt(184u),
                      ObjectiveRepl::kMaxTeamDifference);
            EXPECT_EQ(griReader.ReadByte(), 2u);
            EXPECT_EQ(griReader.SerializeInt(184u),
                      ObjectiveRepl::kMaxPlayers);
            EXPECT_EQ(griReader.ReadByte(), 64u);
            EXPECT_FALSE(griReader.IsOverflowed());
            EXPECT_EQ(griReader.BitPos(), gri->payloadBits);
            EXPECT_FALSE(gameClass.isDynamic);
            EXPECT_EQ(gameClass.index,
                      expected.variant.empty()
                          ? profile.canonicalGameClass
                          : profile.installedGameClass);
        }
    }
}

TEST(ConnectionTravelLifecycle,
     LiveBootstrapKeepsFrozenLayoutAndLoginIdentityPerConnection) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> canonical =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 41, "127.0.0.1", 30135, true, 0, false);
    const std::shared_ptr<ClientConnection> installed =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 42, "127.0.0.1", 30136, true, 0, false);
    canonical->SetPlayerName("CanonicalLogin");
    canonical->SetRetailPlayerId(7);
    installed->SetPlayerName("InstalledReconnect");
    installed->SetRetailPlayerId(19);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 41, {}));
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 42, "installed"));

    ScopedEnvironmentVariable replayWorld(
        "RS2V_REPLAY_CAPTURE_WORLD", "0");
    ConnectionTravelLifecycleTestHarness::SendActorBootstrap(manager, 41);
    ConnectionTravelLifecycleTestHarness::SendActorBootstrap(manager, 42);

    struct ExpectedPri {
        uint32_t clientId;
        uint32_t classRef;
        int32_t playerId;
        std::string name;
    };
    const std::array<ExpectedPri, 2> expectations{{
        {41u, 86701u, 7, "CanonicalLogin"},
        {42u, 86704u, 19, "InstalledReconnect"},
    }};
    for (const ExpectedPri& expected : expectations) {
        const std::vector<PacketCodec::Bunch> queued =
            ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(
                manager, expected.clientId);
        const PacketCodec::Bunch* pri = FindQueuedOpen(queued, 26u);
        ASSERT_TRUE(pri != nullptr);

        BitReader reader(pri->payload.data(), pri->payload.size(),
                         pri->payloadBits);
        const ActorRepl::NetGUIDRef priClass =
            ActorRepl::ReadNetGUID(reader);
        EXPECT_FALSE(priClass.isDynamic);
        EXPECT_EQ(priClass.index, expected.classRef);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        ActorRepl::ReadCompressedVector(reader, x, y, z);
        EXPECT_EQ(reader.SerializeInt(98u), 36u);
        EXPECT_EQ(reader.ReadInt32(), expected.playerId);
        EXPECT_EQ(reader.SerializeInt(98u), 37u);
        EXPECT_EQ(reader.ReadString(), expected.name);
        EXPECT_FALSE(reader.IsOverflowed());
        EXPECT_EQ(reader.BitPos(), pri->payloadBits);
    }
}

TEST(ConnectionTravelLifecycle,
     CanonicalResortFullWorldReplayRequiresExplicitSwitch) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "127.0.0.1", 30135, true, 0, false);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 1, {}));

    {
        ScopedEnvironmentVariable replayWorld(
            "RS2V_REPLAY_CAPTURE_WORLD", "1");
        ConnectionTravelLifecycleTestHarness::SendActorBootstrap(manager, 1);
    }

    const std::vector<PacketCodec::Bunch> queued =
        ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(
            manager, 1);
    EXPECT_FALSE(connection->IsDisconnected());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::GriChannel(manager, 1),
              54u);
    EXPECT_GT(queued.size(), static_cast<size_t>(5));
    const PacketCodec::Bunch* capturedGri = FindQueuedOpen(queued, 54u);
    ASSERT_TRUE(capturedGri != nullptr);
    EXPECT_TRUE(capturedGri->bReliable);
    EXPECT_EQ(capturedGri->chSequence, 1u);
    EXPECT_EQ(capturedGri->payloadBits, 4662u);

    // ch140 is the last actor in the populated Resort capture. Its presence
    // proves the explicit switch replayed the full world, not merely the live
    // menu-critical ch2/ch3/ch4/ch5/ch26 cohort with a different GRI ledger.
    const PacketCodec::Bunch* capturedWorldTail =
        FindQueuedOpen(queued, 140u);
    ASSERT_TRUE(capturedWorldTail != nullptr);
    EXPECT_TRUE(capturedWorldTail->bReliable);
    EXPECT_EQ(capturedWorldTail->chSequence, 1u);
    EXPECT_EQ(capturedWorldTail->payloadBits, 999u);
}

TEST(ConnectionTravelLifecycle,
     InstalledFullWorldReplayGateFailsClosedWithoutActorBunches) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "127.0.0.1", 30136, true, 0, false);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 1, "installed"));

    {
        ScopedEnvironmentVariable replayWorld(
            "RS2V_REPLAY_CAPTURE_WORLD", "1");
        ConnectionTravelLifecycleTestHarness::SendActorBootstrap(manager, 1);
    }

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(0));
    EXPECT_TRUE(
        ConnectionTravelLifecycleTestHarness::QueuedReliableBunches(manager, 1)
            .empty());
}

TEST(ConnectionTravelLifecycle,
     NonResortFullWorldReplayGateFailsClosedWithoutActorBunches) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "127.0.0.1", 30137, true, 0, false);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 1, {}, "VNTE-CuChi", "Territories"));

    {
        ScopedEnvironmentVariable replayWorld(
            "RS2V_REPLAY_CAPTURE_WORLD", "1");
        ConnectionTravelLifecycleTestHarness::SendActorBootstrap(manager, 1);
    }

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(0));
    EXPECT_FALSE(ConnectionTravelLifecycleTestHarness::OutboundActorChannelOpen(
        manager, 1, 2u));
}

TEST(ConnectionTravelLifecycle,
     InvalidDefaultLiveLayoutFailsClosedBeforeJoinedCallback) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1, "127.0.0.1", 30138, false, 0, false);
    ASSERT_TRUE(ConnectionTravelLifecycleTestHarness::FreezeRetailBootstrap(
        manager, 1, {}));
    ASSERT_TRUE(
        ConnectionTravelLifecycleTestHarness::ClearLiveTeamInfoClassRef(
            manager, 1));

    int joinedCallbacks = 0;
    manager.SetClientJoinedCallback(
        [&](const ClientJoinedEvent&) { ++joinedCallbacks; });
    {
        ScopedEnvironmentVariable replayWorld(
            "RS2V_REPLAY_CAPTURE_WORLD", "0");
        manager.FireClientJoined(ClientJoinedEvent{1u});
    }

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(joinedCallbacks, 0);
    EXPECT_EQ(ConnectionTravelLifecycleTestHarness::PendingReliableCount(
                  manager, 1),
              static_cast<size_t>(0));
    EXPECT_FALSE(ConnectionTravelLifecycleTestHarness::OutboundActorChannelOpen(
        manager, 1, 2u));
}

TEST(ConnectionTravelLifecycle,
     LiveBootstrapCohortAllocatorFailureSuppressesJoinAndStateCommit) {
    using Harness = ConnectionTravelLifecycleTestHarness;
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection = Harness::AddClient(
        manager, 1u, "127.0.0.1", 30139u, false, 0u, false);
    ASSERT_TRUE(Harness::FreezeRetailBootstrap(manager, 1u, {}));

    // Leave room for exactly the atomic PC OPEN/NMT 0x24 entry packet. The
    // following GRI/TeamInfo/PRI + reserved ch2 PRI-link cohort must then take
    // SendReservedCh2Bunches' rejected-publication rollback path.
    ASSERT_TRUE(Harness::ConsumeOutboundPacketCapacityLeavingOneSlot(
        manager, 1u));

    int joinedCallbacks = 0;
    manager.SetClientJoinedCallback(
        [&](const ClientJoinedEvent&) { ++joinedCallbacks; });
    {
        ScopedEnvironmentVariable replayWorld(
            "RS2V_REPLAY_CAPTURE_WORLD", "0");
        manager.FireClientJoined(ClientJoinedEvent{1u});
    }

    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_EQ(joinedCallbacks, 0);
    ASSERT_EQ(Harness::PendingReliableCount(manager, 1u), 1u);
    EXPECT_EQ(Harness::FirstPendingBunchCount(manager, 1u), 2u);
    const std::vector<PacketCodec::Bunch> queued =
        Harness::QueuedReliableBunches(manager, 1u);
    EXPECT_TRUE(FindQueuedOpen(queued, 2u) != nullptr);
    EXPECT_TRUE(FindQueuedOpen(queued, 3u) == nullptr);
    EXPECT_TRUE(FindQueuedOpen(queued, 4u) == nullptr);
    EXPECT_TRUE(FindQueuedOpen(queued, 5u) == nullptr);
    EXPECT_TRUE(FindQueuedOpen(queued, 26u) == nullptr);
    EXPECT_EQ(Harness::GriChannel(manager, 1u), 0u);

    const std::array<std::optional<int32_t>, 2> noReinforcements{};
    EXPECT_EQ(Harness::PublishedTeamReinforcements(manager, 1u),
              noReinforcements);
    EXPECT_EQ(Harness::PendingTeamReinforcements(manager, 1u),
              noReinforcements);
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
     ThrowingClientLoggedInCallbackIsContainedAndDisconnectsClient) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1u, "127.0.0.1", 30303u, false, 0u, false);
    bool callbackInvoked = false;
    manager.SetClientLoggedInCallback(
        [&](const ClientLoggedInEvent&) {
            callbackInvoked = true;
            throw std::runtime_error("test login observer failure");
        });

    ClientLoggedInEvent event;
    event.clientId = 1u;
    EXPECT_NO_THROW(manager.FireClientLoggedIn(event));

    EXPECT_TRUE(callbackInvoked);
    EXPECT_TRUE(connection->IsDisconnected());
}

TEST(ConnectionTravelLifecycle,
     ThrowingClientJoinedCallbackIsContainedAndDisconnectsClient) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1u, "127.0.0.1", 30304u, false, 0u, false);
    bool callbackInvoked = false;
    manager.SetClientJoinedCallback(
        [&](const ClientJoinedEvent&) {
            callbackInvoked = true;
            throw std::runtime_error("test join observer failure");
        });

    EXPECT_NO_THROW(manager.FireClientJoined(ClientJoinedEvent{1u}));

    EXPECT_TRUE(callbackInvoked);
    EXPECT_TRUE(connection->IsDisconnected());
}

TEST(ConnectionTravelLifecycle,
     ThrowingControlDispatchClearsPacketLatchesAndDisconnectsClient) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1u, "127.0.0.1", 30305u, false, 0u, false);
    ConnectionTravelLifecycleTestHarness::InstallThrowingControlReassembler(
        manager, 1u);
    ConnectionTravelLifecycleTestHarness::
        SetOwningPawnGraphCompletionDeferred(manager, 1u);

    bool parsed = false;
    EXPECT_NO_THROW(parsed =
        ConnectionTravelLifecycleTestHarness::ParseIncomingControl(
            manager, 1u,
            EncodeClientPacket(FreshHandshakeStartPacket())));

    EXPECT_TRUE(parsed);
    EXPECT_TRUE(connection->IsDisconnected());
    EXPECT_FALSE(
        ConnectionTravelLifecycleTestHarness::InboundPacketDispatchActive(
            manager, 1u));
    EXPECT_FALSE(
        ConnectionTravelLifecycleTestHarness::
            OwningPawnGraphCompletionDeferred(manager, 1u));
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
     PreJoinUE3EndpointCannotFallBackToLegacyPacketDispatch) {
    ConnectionManager manager(nullptr);
    size_t legacyDispatches = 0;
    manager.SetPacketCallback(
        [&](uint32_t, const Packet&, const PacketMetadata&) {
            ++legacyDispatches;
        });
    const std::string ip = "192.0.2.47";
    constexpr uint16_t port = 31010;

    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, EncodeClientPacket(FreshHandshakeStartPacket()), ip, port);
    const std::shared_ptr<ClientConnection> connection =
        manager.GetConnection(1u);
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(connection->IsUE3Client());
    ASSERT_FALSE(connection->IsHandshakeComplete());

    const std::vector<uint8_t> legacy = ZeroTailedLegacyDatagram(
        "CHAT_MESSAGE", LegacyStringPayload("/votemap 1"));
    ASSERT_FALSE(PacketCodec::Decode(
        legacy.data(), legacy.size(),
        PacketCodec::kClientSendMaxPacketBytes).ok);
    ConnectionTravelLifecycleTestHarness::Deliver(manager, legacy, ip, port);

    EXPECT_EQ(legacyDispatches, static_cast<size_t>(0));
    EXPECT_TRUE(connection->IsUE3Client());
    EXPECT_FALSE(connection->IsHandshakeComplete());
}

TEST(ConnectionTravelLifecycle,
     JoinedUE3EndpointCannotFallBackToLegacyGameplayDispatch) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1u, "192.0.2.48", 31011, true, 7u, false);
    size_t legacyDispatches = 0;
    manager.SetPacketCallback(
        [&](uint32_t, const Packet&, const PacketMetadata&) {
            ++legacyDispatches;
        });

    const std::vector<uint8_t> legacy = ZeroTailedLegacyDatagram(
        "WEAPON_FIRE", std::vector<uint8_t>(30u, 0x01u));
    ASSERT_FALSE(PacketCodec::Decode(
        legacy.data(), legacy.size(),
        PacketCodec::kClientSendMaxPacketBytes).ok);
    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, legacy, "192.0.2.48", 31011);

    EXPECT_EQ(legacyDispatches, static_cast<size_t>(0));
    EXPECT_TRUE(connection->IsUE3Client());
    EXPECT_TRUE(connection->IsHandshakeComplete());
}

TEST(ConnectionTravelLifecycle,
     ExplicitNonUE3ToolEndpointRetainsLegacyPacketDispatch) {
    ConnectionManager manager(nullptr);
    const std::shared_ptr<ClientConnection> connection =
        ConnectionTravelLifecycleTestHarness::AddClient(
            manager, 1u, "192.0.2.49", 31012, false, 0u, false);
    connection->SetUE3Client(false);
    size_t legacyDispatches = 0;
    std::string receivedTag;
    manager.SetPacketCallback(
        [&](uint32_t, const Packet& packet, const PacketMetadata&) {
            ++legacyDispatches;
            receivedTag = packet.GetTag();
        });

    const std::vector<uint8_t> legacy = ZeroTailedLegacyDatagram(
        "CHAT_MESSAGE", LegacyStringPayload("tool probe"));
    ConnectionTravelLifecycleTestHarness::Deliver(
        manager, legacy, "192.0.2.49", 31012);

    EXPECT_EQ(legacyDispatches, static_cast<size_t>(1));
    EXPECT_EQ(receivedTag, std::string("CHAT_MESSAGE"));
    EXPECT_FALSE(connection->IsUE3Client());
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
            manager, 1, "192.0.2.44", 31005, true, 7, false);
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
