// src/Game/DeploymentCoordinator.h
//
// Pure state for the retail team -> role -> deployment workflow. Networking
// owns RPC decoding and SpawnSystem owns world mutation; this coordinator only
// validates ordering, remembers a normal spawn selection, and issues at most
// one deployment authorization per client/generation.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class DeploymentCoordinator {
public:
    using ClientId = uint32_t;
    using SpawnId = uint32_t;
    using Generation = uint64_t;

    // Retail ROPlayerController net-field handles. These constants identify
    // the semantic dispatch points only; they deliberately make no claim about
    // UE3 parameter/presence-bit serialization, which belongs in Network/.
    static constexpr uint32_t kServerSetSpawnSelectHandle = 261;
    static constexpr uint32_t kServerSetReadyToSpawnHandle = 434;

    // ROUISceneSpawnSelect sends normal TeamInfo-array slots as 128 + slot.
    // ROTeamInfo exposes ten normal slots. Special selections remain explicit
    // so an unsupported special can never alias a normal server spawn.
    static constexpr uint8_t kNormalSpawnSelectionBase = 128;
    static constexpr uint8_t kNormalSpawnSlotCount = 10;
    // The scene stores these as pre-encoding values, then casts
    // SelectedSpawn+128 back to a byte for h261.  The wire values therefore
    // wrap into 237..254; 252/253/254 are the generic helicopter, tunnel and
    // squad-leader choices respectively.  Commander/vehicle-specific entries
    // occupy the rest of that reserved range.
    static constexpr uint8_t kFirstSpecialSelection = 237;
    static constexpr uint8_t kHelicopterSelection = 252;
    static constexpr uint8_t kTunnelSelection = 253;
    static constexpr uint8_t kSquadLeaderSelection = 254;

    enum class ReadyStatus : uint8_t {
        Ready = 0,
        ForceOnly = 1,
        NotReady = 2
    };

    enum class SelectionResult : uint8_t {
        Accepted,
        RoleNotFinalized,
        UnsupportedSpecialSelection,
        InvalidNormalSelection,
        SlotUnavailable,
        AlreadyAuthorized
    };

    enum class ReadyResult : uint8_t {
        StatusRecorded,
        Authorized,
        AlreadyAuthorized,
        RoleNotFinalized,
        NoSelection,
        SelectionNoLongerAvailable
    };

    struct DeploymentDecision {
        ReadyResult result = ReadyResult::StatusRecorded;
        std::optional<SpawnId> spawnId;

        bool IsNewAuthorization() const noexcept {
            return result == ReadyResult::Authorized && spawnId.has_value();
        }
    };

    struct ClientStateSnapshot {
        Generation generation = 0;
        bool roleFinalized = false;
        std::optional<uint8_t> selectedSlot;
        std::optional<SpawnId> selectedSpawnId;
        ReadyStatus readyStatus = ReadyStatus::NotReady;
        bool deploymentAuthorized = false;
    };

    explicit DeploymentCoordinator(Generation generation = 0) noexcept;

    // A map/round generation is a hard boundary: no role, slot, readiness, or
    // prior authorization may leak into the new generation. Calling this is an
    // explicit reset even when the numeric token is reused.
    void ResetForGeneration(Generation generation) noexcept;
    Generation GetGeneration() const noexcept { return m_generation; }

    // ResetClient is used for a new connection occupying an existing client id;
    // RemoveClient is the matching disconnect cleanup.
    void ResetClient(ClientId clientId);
    void RemoveClient(ClientId clientId) noexcept;

    // The final SelectRoleByClass transition must precede spawn selection.
    // Duplicate final-role notifications are harmless.
    void FinalizeRole(ClientId clientId);

    // availableSpawnIds must use the same deterministic order advertised to
    // the client. The selected id and slot are both retained so readiness can
    // revalidate against a fresh authoritative list.
    SelectionResult SelectSpawn(
        ClientId clientId,
        uint8_t encodedSelection,
        std::span<const SpawnId> availableSpawnIds);

    // Records h434 state. Ready performs the fresh slot/id revalidation and
    // yields a one-shot authorization. The caller decides when to execute it
    // (for initial deployment, DeploymentCountdown gates execution at <=8s).
    DeploymentDecision SetReadyStatus(
        ClientId clientId,
        ReadyStatus status,
        std::span<const SpawnId> availableSpawnIds);

    std::optional<ClientStateSnapshot> GetClientState(ClientId clientId) const;
    bool IsPreparedForDeployment(ClientId clientId) const noexcept;

    // Deterministic client-id order for the countdown threshold executor.
    std::vector<std::pair<ClientId, SpawnId>> GetPreparedDeployments() const;

private:
    struct ClientState {
        Generation generation = 0;
        bool roleFinalized = false;
        std::optional<uint8_t> selectedSlot;
        std::optional<SpawnId> selectedSpawnId;
        ReadyStatus readyStatus = ReadyStatus::NotReady;
        bool deploymentAuthorized = false;
    };

    ClientState& StateFor(ClientId clientId);
    static void ClearSelection(ClientState& state) noexcept;
    static bool IsUnsupportedSpecial(uint8_t encodedSelection) noexcept;

    Generation m_generation = 0;
    std::map<ClientId, ClientState> m_clients;
};

// Pure transition helper for the stock 30-second Preparation clock and final
// eight-second ROUISceneRoundStart window. It does not send RPCs or spawn pawns.
class DeploymentCountdown {
public:
    using ClientId = DeploymentCoordinator::ClientId;
    using Generation = DeploymentCoordinator::Generation;

    static constexpr uint32_t kPreparationSeconds = 30;
    static constexpr uint32_t kRoundStartScreenSeconds = 8;

    enum class Phase : uint8_t {
        Preparation,
        Active,
        PostRound
    };

    struct GlobalActions {
        // One-shot signal to execute DeploymentCoordinator's prepared entries.
        bool deployPreparedClients = false;
    };

    struct ClientActions {
        bool showRoundStartScreen = false;
        bool hideRoundStartScreen = false;
        uint32_t displaySeconds = 0;
    };

    explicit DeploymentCountdown(Generation generation = 0) noexcept;

    void ResetForGeneration(Generation generation) noexcept;
    Generation GetGeneration() const noexcept { return m_generation; }
    void RemoveClient(ClientId clientId) noexcept;

    // Advance owns the global, one-shot deployment threshold. If a coarse tick
    // jumps directly to zero/Active, it still emits the action exactly once.
    GlobalActions Advance(Phase phase, float remainingSeconds) noexcept;

    // SyncClient owns per-connection scene delivery. A reconnect during the
    // final window receives Show with the current remaining whole seconds; a
    // reconnect in Active receives one defensive Hide. Repeated sync is quiet.
    ClientActions SyncClient(
        ClientId clientId,
        Phase phase,
        float remainingSeconds) noexcept;

private:
    struct ClientScreenState {
        bool showSent = false;
        bool hideSent = false;
    };

    static bool IsFinalWindow(Phase phase, float remainingSeconds) noexcept;
    static bool HasEnded(Phase phase, float remainingSeconds) noexcept;
    static uint32_t DisplaySeconds(float remainingSeconds) noexcept;

    Generation m_generation = 0;
    bool m_deployPreparedIssued = false;
    std::map<ClientId, ClientScreenState> m_clients;
};
