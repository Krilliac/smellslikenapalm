// src/Game/DeploymentCoordinator.cpp

#include "Game/DeploymentCoordinator.h"

#include <algorithm>
#include <cmath>

DeploymentCoordinator::DeploymentCoordinator(Generation generation) noexcept
    : m_generation(generation) {}

void DeploymentCoordinator::ResetForGeneration(Generation generation) noexcept {
    m_generation = generation;
    m_clients.clear();
}

void DeploymentCoordinator::ResetClient(ClientId clientId) {
    ClientState state;
    state.generation = m_generation;
    m_clients.insert_or_assign(clientId, state);
}

void DeploymentCoordinator::RemoveClient(ClientId clientId) noexcept {
    m_clients.erase(clientId);
}

void DeploymentCoordinator::FinalizeRole(ClientId clientId) {
    ClientState& state = StateFor(clientId);
    if (state.deploymentAuthorized || state.roleFinalized) return;

    state.roleFinalized = true;
    // The retail spawn-select scene enters ForceOnly while the player chooses a
    // deployment location. A later Ready transition must authorize deployment.
    state.readyStatus = ReadyStatus::ForceOnly;
}

DeploymentCoordinator::SelectionResult DeploymentCoordinator::SelectSpawn(
    ClientId clientId,
    uint8_t encodedSelection,
    std::span<const SpawnId> availableSpawnIds) {
    ClientState& state = StateFor(clientId);
    if (!state.roleFinalized) return SelectionResult::RoleNotFinalized;
    if (state.deploymentAuthorized) return SelectionResult::AlreadyAuthorized;

    if (IsUnsupportedSpecial(encodedSelection)) {
        ClearSelection(state);
        return SelectionResult::UnsupportedSpecialSelection;
    }

    const uint16_t normalEnd = static_cast<uint16_t>(kNormalSpawnSelectionBase) +
                               static_cast<uint16_t>(kNormalSpawnSlotCount);
    if (encodedSelection < kNormalSpawnSelectionBase ||
        static_cast<uint16_t>(encodedSelection) >= normalEnd) {
        ClearSelection(state);
        return SelectionResult::InvalidNormalSelection;
    }

    const uint8_t slot = static_cast<uint8_t>(
        encodedSelection - kNormalSpawnSelectionBase);
    if (static_cast<std::size_t>(slot) >= availableSpawnIds.size()) {
        ClearSelection(state);
        return SelectionResult::SlotUnavailable;
    }

    state.selectedSlot = slot;
    state.selectedSpawnId = availableSpawnIds[slot];
    // Require a fresh Ready after every accepted selection. This also makes an
    // out-of-order Ready-before-selection sequence fail closed.
    state.readyStatus = ReadyStatus::ForceOnly;
    return SelectionResult::Accepted;
}

DeploymentCoordinator::DeploymentDecision DeploymentCoordinator::SetReadyStatus(
    ClientId clientId,
    ReadyStatus status,
    std::span<const SpawnId> availableSpawnIds) {
    ClientState& state = StateFor(clientId);
    if (state.deploymentAuthorized) {
        // Duplicate Ready is idempotent. A later ForceOnly/NotReady is a real
        // state transition (for example, reopening spawn select) and revokes a
        // not-yet-executed preparation authorization.
        if (status != ReadyStatus::Ready) {
            state.deploymentAuthorized = false;
            state.readyStatus = status;
            return {ReadyResult::StatusRecorded, std::nullopt};
        }
        return {ReadyResult::AlreadyAuthorized, std::nullopt};
    }

    state.readyStatus = status;
    if (status != ReadyStatus::Ready) {
        return {ReadyResult::StatusRecorded, std::nullopt};
    }
    if (!state.roleFinalized) {
        return {ReadyResult::RoleNotFinalized, std::nullopt};
    }
    if (!state.selectedSlot.has_value() || !state.selectedSpawnId.has_value()) {
        return {ReadyResult::NoSelection, std::nullopt};
    }

    const std::size_t slot = static_cast<std::size_t>(*state.selectedSlot);
    if (slot >= availableSpawnIds.size() ||
        availableSpawnIds[slot] != *state.selectedSpawnId) {
        // A phase/team/list change invalidates the client-visible slot. Force a
        // new h261 selection rather than silently deploying to a reused index.
        ClearSelection(state);
        return {ReadyResult::SelectionNoLongerAvailable, std::nullopt};
    }

    state.deploymentAuthorized = true;
    return {ReadyResult::Authorized, state.selectedSpawnId};
}

std::optional<DeploymentCoordinator::ClientStateSnapshot>
DeploymentCoordinator::GetClientState(ClientId clientId) const {
    const auto it = m_clients.find(clientId);
    if (it == m_clients.end()) return std::nullopt;

    const ClientState& state = it->second;
    return ClientStateSnapshot{
        state.generation,
        state.roleFinalized,
        state.selectedSlot,
        state.selectedSpawnId,
        state.readyStatus,
        state.deploymentAuthorized
    };
}

bool DeploymentCoordinator::IsPreparedForDeployment(ClientId clientId) const noexcept {
    const auto it = m_clients.find(clientId);
    return it != m_clients.end() && it->second.deploymentAuthorized;
}

std::vector<std::pair<DeploymentCoordinator::ClientId, DeploymentCoordinator::SpawnId>>
DeploymentCoordinator::GetPreparedDeployments() const {
    std::vector<std::pair<ClientId, SpawnId>> prepared;
    for (const auto& [clientId, state] : m_clients) {
        if (state.deploymentAuthorized && state.selectedSpawnId.has_value()) {
            prepared.emplace_back(clientId, *state.selectedSpawnId);
        }
    }
    return prepared;
}

DeploymentCoordinator::ClientState& DeploymentCoordinator::StateFor(ClientId clientId) {
    auto [it, inserted] = m_clients.try_emplace(clientId);
    if (inserted || it->second.generation != m_generation) {
        it->second = ClientState{};
        it->second.generation = m_generation;
    }
    return it->second;
}

void DeploymentCoordinator::ClearSelection(ClientState& state) noexcept {
    state.selectedSlot.reset();
    state.selectedSpawnId.reset();
    state.readyStatus = ReadyStatus::ForceOnly;
}

bool DeploymentCoordinator::IsUnsupportedSpecial(uint8_t encodedSelection) noexcept {
    return encodedSelection >= kFirstSpecialSelection &&
           encodedSelection <= kSquadLeaderSelection;
}

DeploymentCountdown::DeploymentCountdown(Generation generation) noexcept
    : m_generation(generation) {}

void DeploymentCountdown::ResetForGeneration(Generation generation) noexcept {
    m_generation = generation;
    m_deployPreparedIssued = false;
    m_clients.clear();
}

void DeploymentCountdown::RemoveClient(ClientId clientId) noexcept {
    m_clients.erase(clientId);
}

DeploymentCountdown::GlobalActions DeploymentCountdown::Advance(
    Phase phase,
    float remainingSeconds) noexcept {
    GlobalActions actions;
    if (!m_deployPreparedIssued &&
        (IsFinalWindow(phase, remainingSeconds) ||
         HasEnded(phase, remainingSeconds))) {
        m_deployPreparedIssued = true;
        actions.deployPreparedClients = true;
    }
    return actions;
}

DeploymentCountdown::ClientActions DeploymentCountdown::SyncClient(
    ClientId clientId,
    Phase phase,
    float remainingSeconds) noexcept {
    ClientActions actions;
    ClientScreenState& state = m_clients[clientId];

    if (HasEnded(phase, remainingSeconds)) {
        // Retail sends ClientHideRoundStartScreen to active/reconnecting
        // players even when this server instance did not previously send Show.
        if (!state.hideSent) {
            state.hideSent = true;
            actions.hideRoundStartScreen = true;
        }
        return actions;
    }

    if (IsFinalWindow(phase, remainingSeconds) &&
        !state.showSent && !state.hideSent) {
        state.showSent = true;
        actions.showRoundStartScreen = true;
        actions.displaySeconds = DisplaySeconds(remainingSeconds);
    }
    return actions;
}

bool DeploymentCountdown::IsFinalWindow(
    Phase phase,
    float remainingSeconds) noexcept {
    return phase == Phase::Preparation &&
           std::isfinite(remainingSeconds) &&
           remainingSeconds > 0.0f &&
           remainingSeconds <= static_cast<float>(kRoundStartScreenSeconds);
}

bool DeploymentCountdown::HasEnded(
    Phase phase,
    float remainingSeconds) noexcept {
    if (phase != Phase::Preparation) return true;
    return std::isfinite(remainingSeconds) && remainingSeconds <= 0.0f;
}

uint32_t DeploymentCountdown::DisplaySeconds(float remainingSeconds) noexcept {
    if (!std::isfinite(remainingSeconds) || remainingSeconds <= 0.0f) return 0;
    const float bounded = std::min(
        remainingSeconds, static_cast<float>(kRoundStartScreenSeconds));
    return static_cast<uint32_t>(std::ceil(bounded));
}
