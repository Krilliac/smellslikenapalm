// src/Network/ConnectionManager.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <array>
#include <bitset>
#include <map>
#include <optional>
#include "Network/ClientConnection.h"
#include "Network/UDPSocket.h"
#include "Network/BandwidthManager.h"
#include "Network/HandshakeState.h"
#include "Network/PacketCodec.h"
#include "Game/TeamMapping.h"
#include "Network/PacketAssembler.h"
#include "Network/ControlReassembler.h"
#include "Network/ActorReliableSequencer.h"
#include "Network/OutboundReliableSequencer.h"
#include "Network/ClientTravelReplication.h"
#include "Network/RetailBootstrap.h"
#include "Network/RoleSelectionReplication.h"
#include "Game/DeploymentCoordinator.h"
#include "Game/ParticipantRoster.h"
#include "Network/DeploymentReplication.h"
#include "Network/WeaponCombatReplication.h"
#include "Physics/MovementValidator.h"

class GameServer;
class SupremacyMode;

static_assert(ParticipantActorChannelMap::kLastChannel <
                  WeaponCombatRepl::kM61VisualFirstChannel,
              "participant and projectile actor-channel pools must not overlap");

class ConnectionManager {
public:
    using PacketCallback = std::function<void(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta)>;

    explicit ConnectionManager(GameServer* server);
    ~ConnectionManager();

    // Initialize networking subsystems
    bool Initialize(uint16_t listenPort);
    void Shutdown();

    // Set callback for received packets (used by NetworkManager)
    void SetPacketCallback(PacketCallback cb);

    // ---- Game-facing handshake observer interface --------------------------
    // The Game layer (Stream B) subscribes to these to react to control-channel
    // handshake progress WITHOUT the Network layer depending on Game/. The
    // handshake state machine calls FireClientLoggedIn / FireClientJoined.
    void SetClientLoggedInCallback(ClientLoggedInCallback cb);
    void SetClientJoinedCallback(ClientJoinedCallback cb);
    void FireClientLoggedIn(const ClientLoggedInEvent& ev);
    void FireClientJoined(const ClientJoinedEvent& ev);

    // Send raw control-channel bytes (a ControlChannel::Build* payload) to a
    // client without the Packet tag/serialize wrapper. Used by HandshakeState.
    bool SendRawToClient(uint32_t clientId, const std::vector<uint8_t>& bytes);

    // Main loop: receive raw data and dispatch to handlers
    void PumpNetwork();

    // Send utilities
    bool SendToClient(uint32_t clientId, const Packet& pkt);
    void Broadcast(const Packet& pkt);

    // Stream dirty ROGameReplicationInfo objective-array fields to every joined
    // retail UE3 client. The initial per-client baseline is sent automatically
    // after its GRI actor channel opens.
    void BroadcastRetailObjectiveState();

    // Queue one reliable PlayerController.ClientTravel RPC for every fully
    // joined retail session whose ch2 actor channel is open. Each queued
    // session enters a drain-only state: existing reliables may retransmit and
    // control/ACK traffic continues, but no new old-world actor traffic starts.
    // Returns the number of sessions for which the RPC was queued.
    size_t BroadcastRetailClientTravel(
        const ClientTravelRepl::EncodedRpc& rpc,
        const std::string& mapUrl);

    // Validate the complete joined-retail recipient set without sending or
    // mutating it. GameServer calls this before LoadMap so one malformed or
    // half-open PlayerController cannot strand only part of the population in
    // the old world. Zero eligible clients is a successful headless preflight.
    bool CanBroadcastRetailClientTravel(
        const ClientTravelRepl::EncodedRpc& rpc,
        const std::string& mapUrl,
        size_t* eligibleClients = nullptr) const;

    // Drive the retail preparation/final-eight transition plus the supported
    // once-per-RemainingTime-second active reinforcement scan. GameServer calls
    // this after the authoritative mode clock advances so UI and deployment
    // authority share one countdown coordinate.
    void UpdateRetailDeploymentCountdown();

    // WarmUp/PostRound transitions must continue without a selected role, but
    // an already-entered Preparation countdown remains parked until a retail
    // client has finalized its role. This prevents idle countdown consumption
    // without deadlocking WarmUp -> Preparation.
    bool ShouldAdvanceRetailRoundClock() const;

    // Mirror one authoritative human participant onto this connection's
    // owning pawn/PRI graph. Health and death RPCs are independently gated so
    // a killer-stat update never falsely toggles the killer's death state.
    void ReplicateRetailCombatState(uint32_t clientId, int health, int kills,
                                    int deaths, int score, bool isDead,
                                    bool sendHealth, bool sendDeathRpc);

    // Re-anchor client movement validation after a server-authoritative
    // discontinuity such as spawn, death, mantle, map transition, or admin
    // teleport. This never changes Player position itself.
    bool ResetRetailMovementValidation(
        uint32_t clientId, const Vector3& authoritativePosition);

    // Tagged form used by non-owning human and headless-bot replication.  The
    // owning Human path retains its fixed ch26/ch209 graph; every other viewer
    // uses its private ParticipantActorChannelMap and fails closed until the
    // corresponding PRI/pawn actor has actually been opened.
    void ReplicateRetailParticipantCombatState(
        const ParticipantId& participant, int health, int kills, int deaths,
        int score, bool isDead, bool sendHealth, bool sendDeathRpc);
    void RemoveRetailParticipant(const ParticipantId& participant);

    // Persistent retail M61 actor lifecycle. Projectile keys are server-local
    // authoritative ids; each viewer receives its own collision-free dynamic
    // channel and reliable sequence history.
    void BroadcastRetailM61Spawn(
        uint64_t projectileKey, uint32_t shooterClientId,
        const WeaponCombatRepl::M61VisualSnapshot& snapshot);
    void BroadcastRetailM61Update(
        uint64_t projectileKey,
        const WeaponCombatRepl::M61VisualSnapshot& snapshot);
    void BroadcastRetailM61Detonate(uint64_t projectileKey,
                                    float fuseSeconds);
    void BroadcastRetailM61Remove(uint64_t projectileKey);

    // Client management
    std::shared_ptr<ClientConnection> GetConnection(uint32_t clientId) const;
    std::vector<std::shared_ptr<ClientConnection>> GetAllConnections() const;
    uint32_t FindClientByAddress(const std::string& ip, uint16_t port) const;
    uint32_t FindClientBySteamID(const std::string& steamId) const;
    uint32_t CreateOrGetClient(const std::string& ip, uint16_t port);

    // Periodic housekeeping
    void RemoveStaleConnections();
    void UpdateBandwidthWindows();

    // Bandwidth limit query (used by ClientConnection)
    uint32_t GetBandwidthLimit() const;

    // Configuration
    void SetMaxClients(size_t maxClients);
    size_t GetMaxClients() const;

private:
    friend class ConnectionTravelLifecycleTestHarness;

    // UE3 owns one reliable cursor per connection/channel and does not reset it
    // when an actor channel closes. The whole-graph barrier below is an emulator
    // coherence policy for switching between the capture-grounded South/North
    // loadouts; the captures do not contain a same-connection faction switch.
    enum class OwningPawnGraphPhase : uint8_t {
        Unopened,
        Open,
        Closing,
        Closed,
        Broken,
    };
    enum class OwningPawnGraphGateResult : uint8_t {
        Ready,
        Deferred,
        Failed,
    };
    enum class ActiveDeploymentPhase : uint8_t {
        None,
        TerritoryActive,
        TerritoryOvertime,
        TerritoryLockdown,
    };
    enum class ActiveDeploymentPolicy : uint8_t {
        Immediate,
        Timed,
        Closed,
    };
    struct DeferredOwningPawnGraphDeployment {
        uint64_t deploymentGeneration = 0;
        uint32_t spawnId = 0;
        uint32_t teamId = 0;
        bool roundStartAuthorization = false;
    };
    static constexpr std::array<uint32_t, 7> kOwningPawnGraphChannels{
        209u, 210u, 211u, 212u, 213u, 214u, 219u};

    static bool EvaluateRetailRoundClockPolicy(
        DeploymentCountdown::Phase phase, bool waitForReadyPlayer,
        bool hasJoinedRetailClient, bool hasReadyRetailClient);
    static bool ResolveObjectiveConnectedToBase(
        bool authoredConnectedToBase, const SupremacyMode* supremacy,
        uint32_t objectiveId, uint32_t controllingTeam);

    GameServer* m_server;
    std::shared_ptr<UDPSocket> m_socket;
    PacketCallback m_packetCallback;

    // Game-facing handshake observers (set by Stream B; null = not subscribed).
    ClientLoggedInCallback m_clientLoggedInCb;
    ClientJoinedCallback   m_clientJoinedCb;

    // Retail deployment is a staged protocol, not an immediate side effect of
    // role selection: final role -> normal spawn slot -> ready -> countdown
    // authorization. These pure state machines are reset at round boundaries.
    uint64_t m_deploymentGeneration = 1;
    DeploymentCoordinator m_deploymentCoordinator{m_deploymentGeneration};
    DeploymentCountdown m_deploymentCountdown{m_deploymentGeneration};
    std::optional<DeploymentCountdown::Phase> m_lastDeploymentPhase;
    ActiveDeploymentPhase m_lastActiveDeploymentPhase =
        ActiveDeploymentPhase::None;
    std::optional<int32_t> m_lastActiveDeploymentRemainingSeconds;
    std::optional<int32_t> m_lastActiveDeploymentScanSecond;
    bool m_waitForReadyPlayer = true;

    // Client lookup by address
    std::unordered_map<ClientAddress, std::shared_ptr<ClientConnection>> m_clients;

    // Per-connection control-channel handshake state machines, keyed by clientId.
    std::unordered_map<uint32_t, std::unique_ptr<HandshakeState>> m_handshakes;

    // Per-connection UE3 control-channel framing state, keyed by clientId:
    //   * outbound    - assigns PacketId/ChSequence, fragments + acks (send side).
    //   * reassembler - orders/dedups inbound reliable control bunches and peels
    //                   complete messages into the handshake (receive side).
    struct ControlState {
        struct PendingTeamReinforcementPublication {
            int32_t wireValue = 0;
            std::vector<uint32_t> packetIds;
            uint64_t lastSendMs = 0;
        };

        PacketCodec::PacketAssembler outbound;
        std::unique_ptr<PacketCodec::ControlReassembler> reassembler;
        // Reliable actor traffic has an independent ChSequence cursor per actor
        // channel. Keep it separate from ch0's message reassembler: actor bunches
        // are released whole, while control bunches are reassembled into NMT
        // messages. Unreliable actor traffic deliberately bypasses this object.
        PacketCodec::ActorReliableSequencer actorReliableInbound;
        // Per-channel state for the owning client's PlayerController (ch2). The actor
        // bootstrap opens ch2 (seq 1) then sends ClientShowTeamSelect (seq 2).
        // Sequence zero is valid after modulo wrap, so channel openness is kept
        // separately and this allocator owns cursor/in-flight state only.
        PacketCodec::OutboundReliableSequencer ch2Reliable;
        // actorChType is ch2's ChType (CHTYPE_Actor) reused for later bunches.
        // teamSelected guards the SelectTeam->role-select advance.
        uint32_t actorChType = 2;
        uint32_t griChannel = 0;       // captured bootstrap ch54; live bootstrap ch3
        uint32_t griOutReliable = 0;   // reliable sequence seeded by the GRI open
        // Retail TeamInfo actor channels indexed by retail team (0=NVA, 1=US).
        // Captured Resort bootstrap uses {76,56}; the live builder uses {4,5}.
        std::array<uint32_t, 2> teamInfoChannels{};
        // Last h62 values packet-ACKed by this connection. Retail property
        // deltas remain wire-unreliable, but unretired values are resent until
        // one carrying packet is acknowledged, mirroring UE3's dirty-property
        // retirement instead of mistaking local UDP handoff for delivery.
        std::array<std::optional<int32_t>, 2>
            publishedTeamReinforcements{};
        std::array<std::optional<PendingTeamReinforcementPublication>, 2>
            pendingTeamReinforcements{};
        bool     teamSelected = false;
        bool     roleFinalized = false; // persists across deployment generations
        bool     roleSelectionAccepted = false;
        bool     roleClassReplicated = false;
        uint32_t selectedRoleInfoObjectRef = 0;
        uint8_t  selectedRoleClassIndex = 255;
        std::optional<RoleSelectionRepl::ChangedRoleEvidence>
            selectedChangedRole;
        uint8_t  selectedRoleSquadIndex = 255;
        uint8_t  selectedRoleIndex = 255;
        bool     menuResent = false;   // re-sent ClientShowTeamSelect on client proof-of-life
        bool     spawned = false;      // sent the pawn-spawn + possession once (SelectRoleByClass)
        // ROPlayerController.NextRespawnTime is expressed in the same integer
        // RemainingTime coordinate as the owning client's GRI.  It is armed at
        // a source-grounded active Territory death/team boundary and survives
        // spawn-scene Ready/ForceOnly churn until the next life begins.
        std::optional<int32_t> activeDeploymentDeadlineRemainingSeconds;
        ActiveDeploymentPhase activeDeploymentDeadlinePhase =
            ActiveDeploymentPhase::None;
        std::optional<int32_t> publishedNextRespawnTime;
        std::optional<int32_t> nextRespawnLastPublishScanSecond;
        bool     pawnGraphOpen = false; // true only while the owning graph is reusable/open
        uint32_t pawnGraphTeamId = 0;  // immutable faction of the open ch209..219 graph
        OwningPawnGraphPhase pawnGraphPhase = OwningPawnGraphPhase::Unopened;
        std::array<PacketCodec::OutboundReliableSequencer,
                   kOwningPawnGraphChannels.size()>
            owningPawnGraphReliable;
        std::bitset<ActorRepl::kDynamicChannelMax>
            owningPawnGraphActiveChannels;
        std::bitset<ActorRepl::kDynamicChannelMax>
            owningPawnGraphClosingChannels;
        std::bitset<ActorRepl::kDynamicChannelMax>
            owningPawnGraphCloseAcknowledged;
        std::optional<DeferredOwningPawnGraphDeployment>
            deferredOwningPawnGraphDeployment;
        // ACK processing precedes bunch dispatch inside one UE3 packet. Defer
        // graph completion until packet tail so a later ready/team RPC can
        // revoke the deployment before authority commits. The ACK-bearing
        // client PacketId then fences delayed prior-incarnation actor RPCs.
        bool inboundPacketDispatchActive = false;
        bool owningPawnGraphCompletionDeferred = false;
        bool owningPawnGraphInboundPacketFloorValid = false;
        uint32_t owningPawnGraphInboundPacketFloor = 0u;
        std::array<std::bitset<PacketCodec::kMaxChSequence>,
                   kOwningPawnGraphChannels.size()>
            owningPawnGraphSuppressedInboundReliable;
        // The fixed owning actor channels can survive multiple pawn lives. Keep
        // the authoritative life and the graph/recovery binding explicit so a
        // delayed possession request cannot revive stale per-life state.
        uint64_t owningPawnGeneration = 0; // increments on each accepted Dead->Alive transition
        uint64_t pawnGraphGeneration = 0;  // generation currently represented by ch209..219
        bool     owningPawnAlive = false;
        uint64_t possessionAckedGeneration = 0;
        uint64_t possessionRecoveryGeneration = 0;
        // AskForPawn is only a recovery hint; reliable retransmission already
        // carries the original possession graph. Bound fresh GivePawn bursts so
        // a client which cannot resolve its pawn cannot exhaust ch2 sequence
        // space by polling at frame rate.
        uint8_t  possessionRecoveryResponses = 0;
        uint64_t lastPossessionRecoveryResponseMs = 0;
        bool     possessionRecoveryLimitLogged = false;
        std::array<uint32_t, DeploymentCoordinator::kNormalSpawnSlotCount>
            advertisedSpawnIds{};
        std::array<uint32_t, DeploymentCoordinator::kNormalSpawnSlotCount>
            advertisedSpawnVolumeRefs{};
        uint8_t advertisedSpawnCount = 0;
        uint64_t lastAckFlushMs = 0;   // ack-storm throttle: last standalone ack-only datagram sent
        uint64_t lastServerSendMs = 0; // canonical idle keepalive is due 1s after any S2C send

        // Frozen when this session's PackageMap is selected. GRI.GameClass and
        // ChangedTeams must use this exact same profile even if a map rotation
        // begins while the retail client is still loading.
        std::optional<RetailBootstrap::Profile> retailBootstrapProfile;
        // Frozen beside the profile so every numeric ROGame class/object ref is
        // emitted from the exact PackageMap artifact selected for this session.
        // The resolved flag distinguishes an unsupported value from not-yet-read.
        bool retailArtifactSelectionResolved = false;
        std::optional<RetailBootstrap::ArtifactSelection>
            retailArtifactSelection;
        bool mapTravelPending = false;
        // Monotonic start time for the drain-only travel incarnation. Ordinary
        // client heartbeats cannot extend this deadline: once ClientTravel is
        // ACKed there may be no reliable left to exhaust, and retaining the old
        // world session forever blocks every later map rotation.
        uint64_t mapTravelStartedMs = 0;
        // One bounded transport slot borrowed from a same-address travelling
        // session so a new source port can reach Login and prove its identity.
        // It remains leased unless that login retires a matching old session.
        bool travelReconnectHeadroomLease = false;

        // Last quantized retail objective snapshot. Baseline sends mapping plus
        // every state byte; later broadcasts emit only changed h174/h176-h178
        // array elements and never repeat h179 unless a new session/map opens.
        bool objectiveCacheValid = false;
        std::array<uint8_t, 16> objectiveRepIndex{};
        std::array<uint8_t, 16> objectiveConnected{};
        std::array<uint8_t, 16> objectiveCapProgress{};
        std::array<uint8_t, 16> objectiveForceRatio{};
        std::array<uint8_t, 16> objectiveStatus{};
        std::array<uint8_t, 16> objectiveCappersTeam0{};
        std::array<uint8_t, 16> objectiveCappersTeam1{};

        // Territory team roles use retail 0=NVA/Axis, 1=US/Allies even though
        // TeamManager uses server ids 2=NVA and 1=US. Cache them separately so
        // halftime side swaps produce a live GRI delta for connected clients.
        bool territoryRoleCacheValid = false;
        bool territoryAlliesAreAttacking = false;
        uint8_t territoryDefendingTeam = TeamMapping::kRetailNeutral;

        // GameReplicationInfo owns a client-local one-second countdown. The
        // initial h25/h28/h29 values seed it; retail then sends h27 every five
        // seconds as a correction. Cache the last transmitted values so the
        // 10 Hz objective broadcaster does not flood scalar timer deltas.
        bool griTimerCacheValid = false;
        // Active-state h32=false/h31=true is capture-pinned. Non-active values
        // remain intentionally un-authored until a round-boundary capture exists.
        bool griActiveStatePublished = false;
        uint16_t griPhaseKey = 0;
        int32_t griTimeLimit = 0;
        int32_t griRemainingSync = 0;
        int32_t griElapsedTime = 0;

        // Retail Supremacy signed-score snapshot (GRI h46-h48). Static-array
        // team order is retail 0=NVA/North, 1=US/South.
        bool supremacyCacheValid = false;
        std::array<int32_t, 2> supremacyPointsHeld{};
        int32_t supremacyCurrentScore = 0;
        int32_t supremacyTargetScore = 0;

        // Retail Skirmish round/spawn/overtime snapshot (GRI h37-h41, h67,
        // h116-h117, h126, h129). Arrays are retained in retail team order.
        bool skirmishCacheValid = false;
        std::array<int32_t, 20> skirmishSpawnWindows{};
        std::array<int32_t, 2> skirmishSpawnWindowCloseTime{};
        int32_t skirmishPlayedRounds = 0;
        int32_t skirmishRoundScoreLimit = 0;
        int32_t skirmishRoundLimit = 0;
        int32_t skirmishNextLockdownTime = -1;
        bool skirmishSuddenDeath = false;
        bool skirmishOvertime = false;
        uint8_t skirmishOvertimeAdvantage = TeamMapping::kRetailNeutral;
        std::array<uint8_t, 2> skirmishPlayersAlive{};

        // Owning ROPlayerController objective context. Territory capture code
        // sets ObjectiveIndex then ObjectiveName while inside a live zone and
        // clears only ObjectiveName on exit.
        bool pcObjectiveCacheValid = false;
        uint8_t pcObjectiveSlot = 0xFF;

        // Bounded C2S gameplay intent decoded from the owning fixed actor graph.
        // These are observations/state gates only: no hit, use, or mantle success
        // is fabricated without an authoritative world/collision query.
        bool movementInputValid = false;
        uint32_t latestMovementHandle = 0;
        uint8_t latestMoveFlags = 0;
        bool latestViewValid = false;
        int32_t latestPackedView = 0;
        bool useHeld = false;
        bool mantleAttemptPending = false;
        bool mantleWantsToClimb = false;
        bool mantleHadLastGoodTrace = false;
        bool specialMoveActive = false;
        uint8_t specialMove = 0;
        bool mantlePawnStarted = false;
        uint32_t activeWeaponChannel = 0;
        struct WeaponIntent {
            bool firing = false;
            uint8_t fireMode = 0;
            bool reloadRequested = false;
        };
        std::array<WeaponIntent, 5> weaponIntent{}; // channels 210..214
        // Local protocol gate for pairing one owned M61 h29 with its h30. Cook
        // time and projectile admission remain authoritative in GameServer.
        struct GrenadeCook {
            bool active = false;
            uint32_t channel = 0;
            uint8_t fireMode = 0;
        };
        GrenadeCook grenadeCook{};

        // Remote actors are connection-local dynamic objects.  A tagged
        // participant can have the same numeric id as a human/bot without
        // aliasing, and retired pairs are quarantined until this ControlState
        // is destroyed at disconnect.
        ParticipantActorChannelMap remoteParticipants;
        // h152 is a validated informational no-op. Aggregate counters retain
        // diagnostics without logging every record at Info during large PRI
        // update batches.
        uint64_t vivoxNoOpRecordsAccepted = 0;
        uint64_t vivoxBunchesRejected = 0;
        struct RemotePawnViewState {
            DeploymentRepl::RetailRemotePawnSnapshot lastSnapshot{};
            uint64_t lastMovementSendMs = 0;
            uint64_t closeDueMs = 0;
            bool snapshotValid = false;
            bool deathCoreSent = false;
        };
        std::map<ParticipantId, RemotePawnViewState> remotePawnViews;
        uint64_t lastRemotePawnReplicationMs = 0;
        WeaponCombatRepl::M61VisualChannelPool m61Visuals;
        // Every outbound actor open claims this connection-local channel until
        // teardown. M61 and participant-pawn channels may be reused only after
        // their close and all earlier ordered reliable traffic are fully acked.
        std::bitset<ActorRepl::kDynamicChannelMax> outboundActorChannels;
        std::bitset<ActorRepl::kDynamicChannelMax> m61CloseAcknowledged;
        std::bitset<ActorRepl::kDynamicChannelMax>
            participantPawnCloseAcknowledged;
        // ---- Reliable retransmission (UE3 UNetConnection-style) -----------------
        // UE3 reliability: a reliable bunch must be re-sent until the client acks the
        // packet that carried it. Without this, one dropped reliable bunch in the
        // bootstrap burst stalls that channel forever -> the client soft-locks (can't
        // disconnect). We record each reliable bunch-set we send, clear it when the
        // client acks any packet it rode in, and resend (verbatim, SAME per-channel
        // ChSequence; NEW PacketId) on an ack-timeout.
        struct SentReliable {
            std::vector<uint32_t> packetIds;     // every packet this set has gone out in
            uint64_t lastSendMs = 0;
            // Control-channel messages can legitimately wait tens of seconds
            // for the cold retail client to finish loading. Actor traffic uses
            // the shorter defaults; SendRawToClient overrides these for ch0.
            uint64_t retryDelayMs = 250;
            int      maxResends = 12;
            int      resendCount = 0;
            std::vector<PacketCodec::Bunch> bunches;  // the reliable bunches, verbatim
        };
        std::vector<SentReliable> pendingReliable;
    };

    enum class PossessionRecoveryDecision : uint8_t {
        Respond,
        Malformed,
        Ineligible,
        StaleGeneration,
        Backpressured,
        RateLimited,
        LimitReached,
        Suppressed,
    };
    static constexpr uint8_t kMaxPossessionRecoveryResponses = 3;
    static constexpr uint64_t kPossessionRecoveryIntervalMs = 1000;
    static constexpr uint32_t kAskForPawnPayloadBits = 9;
    static PossessionRecoveryDecision EvaluatePossessionRecovery(
        ControlState& state, bool exactStandaloneRequest,
        uint64_t expectedPawnGeneration, uint64_t nowMs);
    static bool CommitPossessionRecoveryResponse(
        ControlState& state, uint64_t expectedPawnGeneration,
        uint64_t nowMs);
    static uint64_t AdvanceOwningPawnGeneration(ControlState& state);
    static uint64_t AnticipatedOwningPawnGeneration(
        const ControlState& state) noexcept;
    static bool HasLiveOwningPawnGeneration(
        const ControlState& state, uint64_t expectedPawnGeneration);
    static void BindPossessionRecovery(
        ControlState& state, uint64_t pawnGeneration);
    static void InvalidatePossessionRecovery(ControlState& state);
    static void ResetPossessionRecovery(ControlState& state);
    static std::optional<size_t> OwningPawnGraphChannelIndex(
        uint32_t channel);
    static PacketCodec::OutboundReliableSequencer*
    OwningPawnGraphSequencer(ControlState& state, uint32_t channel);
    static const PacketCodec::OutboundReliableSequencer*
    OwningPawnGraphSequencer(const ControlState& state, uint32_t channel);
    bool EnsureOwningPawnGraphSequencers(uint32_t clientId,
                                         ControlState& state);
    bool QueueOwningPawnGraphClose(uint32_t clientId);
    OwningPawnGraphGateResult GateOwningPawnGraphForDeployment(
        uint32_t clientId, uint32_t teamId, uint32_t spawnId,
        bool roundStartAuthorization);
    void CompleteOwningPawnGraphClose(
        uint32_t clientId,
        std::optional<uint32_t> inboundBarrierPacketId = std::nullopt);
    void FailOwningPawnGraph(uint32_t clientId, const char* context);
    bool PreflightPawnSpawn(uint32_t clientId,
                            uint64_t expectedPawnGeneration,
                            const Vector3& spawnLocation);
    bool ProcessPawnSpawn(uint32_t clientId,
                          uint64_t expectedPawnGeneration,
                          const Vector3& spawnLocation,
                          bool preflightOnly);

    std::unordered_map<uint32_t, ControlState> m_controlState;
    uint32_t m_nextClientId{1};
    size_t m_maxClients{256};
    uint32_t m_bandwidthLimit{65536};

    // Bandwidth manager
    std::unique_ptr<BandwidthManager> m_bwManager;
    std::unique_ptr<MovementValidator> m_movementValidator;

    // Helpers
    void HandleIncomingPacket(const std::vector<uint8_t>& data, const ClientAddress& addr);

    // Tear down every protocol/game object owned by one endpoint.  Used both by
    // ordinary timeouts and when a retail client opens a brand-new control
    // channel from an endpoint whose previous session is still Joined.
    void RemoveClientSession(const ClientAddress& addr, const char* reason);

    // Travel may recreate the UE3 NetDriver on a different source port. Once
    // the new endpoint presents the same Steam64 from the same source IP,
    // retire an older drain-only session only after the new endpoint passes the
    // game-layer admission gate. Steam identity is not platform-authenticated
    // yet, so matching the source IP prevents an arbitrary remote peer from
    // evicting a travelling player by copying only its public Steam64.
    // Collection and erasure are deliberately separate so the address map is
    // never invalidated while iterating.
    size_t RetireSupersededTravelSessions(uint32_t newClientId,
                                          uint64_t presentedSteamId);
    void NormalizeTravelReconnectHeadroomLeases();

    // Retire drain-only travel incarnations which never produced a fresh,
    // admitted rejoin. This deadline is intentionally independent of the
    // ordinary heartbeat timeout because old-world ACK/keepalive traffic may
    // continue indefinitely after ClientTravel itself was acknowledged.
    static constexpr uint64_t kMapTravelTimeoutMs = 120000;
    size_t ExpirePendingTravelSessions(uint64_t nowMs);

    // A retail reconnect may reuse the exact same UDP source port.  Detect its
    // fresh reliable ch0 open/HandshakeStart before the old reassembler sees it,
    // then discard the old session so PacketId/ChSequence can restart at zero/one.
    void ResetEstablishedSessionForFreshHandshake(
        const std::vector<uint8_t>& data, const ClientAddress& addr);

    // Reconcile gauges from the transport ledger and handshake/travel state.
    // A ClientTravel endpoint remains an active UDP connection while its old-
    // world authenticated Player/Security state has already been retired.
    void UpdateTelemetryPlayerCounts() const;

    // Get-or-create the handshake state machine for a client.
    HandshakeState& GetOrCreateHandshake(uint32_t clientId);

    // Get-or-create the per-connection control-channel framing state (lazily wires
    // the reassembler's message callback to the client's handshake).
    ControlState& GetControlState(uint32_t clientId);

    // Resolve and freeze the map/mode/bootstrap identity for one UE3 session.
    const RetailBootstrap::Profile& GetRetailBootstrapProfile(uint32_t clientId);
    const std::optional<RetailBootstrap::ArtifactSelection>&
    GetRetailArtifactSelection(uint32_t clientId);

    // Frame + encode an outbound control packet and push it to the client.
    void SendEncodedPacket(uint32_t clientId, const PacketCodec::Packet& pkt);

    // Post-Join WORLD REPLICATION bootstrap. Once a client reaches Joined the
    // retail client sits on the loading screen until the server replicates the
    // world - first the PackageMap export (the map's package/NetGUID list), then
    // the bootstrap actor channels. data/replication_bootstrap.bin is the canonical
    // Resort capture ([uint32 LE len][payload] records). The exact process-start
    // opt-in RS2V_REPLICATION_BOOTSTRAP_VARIANT=installed instead selects the
    // separate GUID-rebased replication_bootstrap_installed.bin candidate and
    // derives its hash-pinned actor static-reference rebase from the canonical
    // actor_bootstrap.bin capture.
    // RetailBootstrap decodes and reconstructs the selected stream's bounded
    // map/mode profile while retaining all opaque control bytes, then the records
    // are sent in order via SendRawToClient.
    // No-op (logged) if the file is absent or structurally invalid.
    void SendReplicationBootstrap(uint32_t clientId);

    // Send a reliable server->client function-call bunch on the PlayerController
    // channel (ch2): payload = SerializeInt(handle, maxHandle) + any params, already
    // packed into `payload`/`payloadBits` by the caller. Assigns the next ch2 reliable
    // ChSequence. Used for ClientShowTeamSelect / ClientShowRoleSelect / ChangedTeams.
    bool SendCh2Rpc(uint32_t clientId, const std::vector<uint8_t>& payload,
                    uint32_t payloadBits, const char* name);

    struct DeploymentPhaseState {
        DeploymentCountdown::Phase phase = DeploymentCountdown::Phase::PostRound;
        float remainingSeconds = 0.0f;
    };

    struct ActiveDeploymentPhaseState {
        ActiveDeploymentPhase phase = ActiveDeploymentPhase::None;
        int32_t remainingSeconds = 0;
        ActiveDeploymentPhase previousPhase = ActiveDeploymentPhase::None;
        std::optional<int32_t> previousPhaseRemainingAtTransition;
    };

    DeploymentPhaseState GetDeploymentPhaseState() const;
    std::optional<ActiveDeploymentPhaseState>
    GetActiveDeploymentPhaseState() const;
    ActiveDeploymentPolicy GetActiveDeploymentPolicy(
        uint32_t clientId) const;
    static std::optional<int32_t> RetailRemainingSecond(
        float remainingSeconds) noexcept;
    static std::optional<int32_t> CalculateActiveDeploymentDeadline(
        int32_t remainingSeconds, uint32_t serverTeamId) noexcept;
    static bool HasReachedActiveDeploymentDeadline(
        int32_t remainingSeconds,
        int32_t deadlineRemainingSeconds) noexcept;
    static std::optional<int32_t> RebaseActiveDeploymentDeadline(
        int32_t previousRemainingSeconds,
        int32_t previousDeadlineRemainingSeconds,
        int32_t currentRemainingSeconds) noexcept;
    bool SendOwnerNextRespawnTime(uint32_t clientId,
                                  int32_t nextRespawnTime);
    bool ArmActiveDeploymentDeadline(uint32_t clientId,
                                     bool replaceExisting = false);
    void ClearActiveDeploymentDeadline(ControlState& state) noexcept;
    void ClearAndPublishActiveDeploymentDeadline(uint32_t clientId);
    void UpdateRetailActiveDeployments();
    static bool IsDeploymentWindowOpen(
        const DeploymentPhaseState& state) noexcept;
    void RevokePreparedDeploymentAuthorization(uint32_t clientId);
    std::vector<uint32_t> GetAvailableSpawnIds(uint32_t clientId) const;
    std::vector<uint32_t> GetCurrentAdvertisedSpawnIds(uint32_t clientId) const;
    std::vector<uint32_t> GetAdvertisedSpawnIds(uint32_t clientId) const;
    void RefreshAdvertisedSpawnIds(uint32_t clientId);
    int32_t ResolveRetailWireReinforcements(uint8_t retailTeam) const noexcept;
    void SynchronizeRetailTeamReinforcements();
    bool SendRetailSpawnLocations(uint32_t clientId);
    void SendOwnerPriClassIndex(uint32_t clientId, uint8_t classIndex);
    void SendOwnerPriRoleAssignment(uint32_t clientId, uint8_t squadIndex,
                                    uint8_t roleIndex);
    bool SendChangedSquadAssignment(
        uint32_t clientId,
        const RoleSelectionRepl::ChangedSquadEvidence& evidence);
    void SynchronizeRetailSquadAssignments();
    bool SendChangedRoleSpawnSelect(uint32_t clientId,
                                    bool includeOwnerPriAssignment = false,
                                    bool includeTempStopAutoSpawn = false);
    void SendPriSpawnSelection(uint32_t clientId, uint8_t encodedSelection);
    bool SendShowRoundStartScreen(uint32_t clientId, uint32_t displaySeconds);
    bool SendHideRoundStartScreen(uint32_t clientId);
    bool ExecutePreparedDeployment(
        uint32_t clientId, uint32_t spawnId,
        bool roundStartAuthorization = false);
    void BeginDeploymentGeneration();
    bool IsRetailGameplayActive(uint32_t clientId) const;

    enum class RemotePriOpenResult : uint8_t {
        Failed,
        AlreadyOpen,
        OpenQueued,
        UpdateQueued,
    };

    bool BuildRemoteParticipantInitialState(
        const ParticipantId& participant,
        const DeploymentRepl::RetailParticipantCombatState* combatOverride,
        DeploymentRepl::RetailParticipantInitialState& output) const;
    RemotePriOpenResult QueueRemoteParticipantPriOpen(
        uint32_t viewerClientId,
        const DeploymentRepl::RetailParticipantInitialState& participant,
        std::vector<PacketCodec::Bunch>& output);
    RemotePriOpenResult QueueRemoteParticipantPawnOpen(
        uint32_t viewerClientId,
        const DeploymentRepl::RetailParticipantInitialState& participant,
        std::vector<PacketCodec::Bunch>& output);
    bool QueueRemoteParticipantPawnDeath(
        uint32_t viewerClientId, const ParticipantId& participant,
        uint64_t nowMs, std::vector<PacketCodec::Bunch>& output);
    void SynchronizeRemoteParticipantPris(uint32_t viewerClientId);
    void SynchronizeAllRemoteParticipantPris();
    void ReplicateRemoteParticipantPawnsTick();
    void RetireRemoteParticipantFromViewers(
        const ParticipantId& participant);

    // Replicate the local PlayerController's PlayerReplicationInfo (Controller handle 23)
    // as an UNRELIABLE ch2 object-ref to the local PRI channel (ch26) - the exact bytes
    // the real server streams ("176a00"). Without this link the client's
    // ROPC.PlayerReplicationInfo is none and the role/unit-select UI derefs null
    // (VNGame.exe+0xbbf712). Sent `repeats` times for unreliable delivery.
    void SendLocalPriLink(uint32_t clientId, int repeats);

    // Clear the local PRI's spectator/waiting flags on ch26 (bWaitingPlayer h31,
    // bOnlySpectator h32, bIsSpectator h33 -> 0) as an UNRELIABLE property delta, so
    // ShowRoleSelectScene does not early-return at ROPlayerController.uc:5932
    // (if PlayerReplicationInfo.bOnlySpectator return). Sent `repeats` times.
    void SendClearSpectator(uint32_t clientId, int repeats);

    // Spawn the local player's pawn + make the client possess it (the role->spawn step).
    // Opens the capture-matched owning ROPawn ch209 plus weapon graph ch210-214 and
    // ROInventoryManager ch219, fixes Controller/PRI/InvManager and inventory-chain
    // back-references, then
    // sends the retail possession/camera transition and ClientSwitchToBestWeapon(h28).
    // PC.Pawn(h24) follows on the next packet. Expected result: closed spawn UI,
    // an equipped weapon, and ServerMove(h65).
    bool SendPawnSpawn(uint32_t clientId, uint64_t expectedPawnGeneration);

    // Replicate the owning pawn's capture-pinned current attachment class after
    // ROInventoryManager h25 confirms a client weapon selection. Channel zero
    // emits UE3's dynamic-None form and clears the attachment.
    void SendOwningPawnCurrentAttachment(uint32_t clientId,
                                         uint32_t weaponChannel);

    // UE3's AskForPawn(h42) recovery path: re-assert PC.Pawn and answer with
    // GivePawn(h43, Pawn), whose client implementation calls ClientRestart itself.
    bool SendGivePawn(uint32_t clientId, uint64_t expectedPawnGeneration);

    static constexpr uint32_t kLocalPawnChannel = 209;

    // Decode one inbound actor-channel bunch using the owning actor's field table:
    // ch2 PlayerController, ch209 ROPawn, ch210-214 ROWeapon, ch219 InventoryManager.
    // Unknown schemas stop without guessing where a following RPC begins.
    void DecodeInboundActorBunch(uint32_t clientId, const PacketCodec::Bunch& bunch);

    // Dispatch one non-control-channel bunch. Reliable actor bunches are ordered
    // and deduplicated per channel before DecodeInboundActorBunch; unreliable
    // traffic retains datagram order and is never held behind a reliable gap.
    void DispatchInboundActorBunch(
        uint32_t clientId, const PacketCodec::Bunch& bunch,
        bool suppressOwningGraphSemantics = false,
        bool suppressReleasedCohort = false);

    // ---- Reliable retransmission ------------------------------------------------
    // Build ONE packet from `bunches`, send it, and record any reliable bunches for
    // retransmission until acked. The single choke-point for sending actor bunches.
    bool SendReliableBunches(
        uint32_t clientId, const std::vector<PacketCodec::Bunch>& bunches,
        uint32_t* sentPacketId = nullptr);
    std::optional<PacketCodec::OutboundReliableSequencer::Reservation>
    ReserveCh2Reliable(ControlState& state, uint32_t clientId, size_t count,
                       const char* context);
    bool SendReservedCh2Bunches(
        uint32_t clientId, const std::vector<PacketCodec::Bunch>& bunches,
        const PacketCodec::OutboundReliableSequencer::Reservation& reservation,
        const char* context);
    void FailCloseCh2Publication(uint32_t clientId, const char* context);

    // Coalesce + throttle pending acks into at most one standalone ack-only datagram per
    // client per pump cycle (~20ms). Acks also piggyback on any data packet we send. Replaces
    // the old per-received-packet ack-only send, which produced an S2C ack-storm (~4000 ack
    // datagrams during role-select spam) that congested loopback and DROPPED reliable packets
    // (the pawn open, plus the soft-locks / first-click flakiness). Called once per PumpNetwork.
    void FlushPendingAcks();
    // The client acked `ackedPacketId`: drop any pending reliable set that rode in it.
    void OnClientAck(uint32_t clientId, uint32_t ackedPacketId);
    // Per-poll: resend reliable bunch-sets the client hasn't acked within the timeout.
    void RetransmitTick();
    // Per-poll: when a Joined connection has emitted no S2C data for one second,
    // send UE3's canonical empty PacketId+terminator transport keepalive.
    void TransportKeepAliveTick();

    // Open the bootstrap ACTOR channels after the client confirms Join. Reads the
    // official captured actor burst from
    // data/actor_bootstrap.bin - a stream of full bunch descriptors
    // [u16 chIndex][u8 chType][u8 flags][u16 chSequence][u32 len][payload]. ch0
    // records ride the normal control path; ChIndex>=2 records open their own
    // channel via PacketAssembler::BuildRawBunchPacket. No-op (logged) if absent.
    // Actor payloads are session-specific, so normal gameplay filters the file to
    // the adoption/menu-critical channels {2,21,26,54,56,76}. Replaying captured
    // pawns/vehicles is opt-in via RS2V_REPLAY_CAPTURE_WORLD=1 for RE only.
    void SendActorBootstrap(uint32_t clientId);
    // Live per-session actor bootstrap (GRI + TeamInfo + local PC/PRI) — replaces the
    // canned capture replay so the client keeps real actors and the team menu works.
    void SendLiveActorBootstrap(uint32_t clientId);

    // Encode the ObjectiveSystem authority into ROGameReplicationInfo static
    // array deltas on this connection's GRI actor channel.
    void SendRetailObjectiveState(uint32_t clientId, bool baseline);

    // ------------------------------------------------------------------------
    //  CONTROL-CHANNEL INBOUND PATH.
    //
    //  Decodes a raw inbound UDP datagram as a UE3 packet (PacketCodec::Decode),
    //  acknowledges it, feeds its control-channel bunches to the per-connection
    //  reassembler (which orders/dedups them and peels complete messages into the
    //  handshake), and flushes a standalone ack if no response carried it. Wire
    //  format: docs/RS2V_ControlChannel_WireSpec_7258.md.
    // ------------------------------------------------------------------------
    // Returns true if `datagram` was a well-formed UE3 packet and was handled
    // here (so the caller must NOT also run it through the legacy Packet path).
    bool ParseIncomingControl(uint32_t clientId, const std::vector<uint8_t>& datagram);
};
