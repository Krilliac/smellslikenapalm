#include "Game/ParticipantRoster.h"

#include <cmath>
#include <limits>

bool ParticipantRoster::Upsert(const ParticipantSnapshot& participant) {
    if (!participant.id.IsValid() || !IsPlayableTeam(participant.teamId) ||
        !IsFinite(participant.position) || !std::isfinite(participant.captureWeight) ||
        participant.captureWeight < 0.0f) {
        return false;
    }

    participants_[participant.id] = participant;
    return true;
}

bool ParticipantRoster::Remove(const ParticipantId& id) {
    return participants_.erase(id) != 0;
}

void ParticipantRoster::Clear() {
    participants_.clear();
}

void ParticipantRoster::ClearKind(ParticipantKind kind) {
    for (auto it = participants_.begin(); it != participants_.end();) {
        if (it->first.kind == kind) {
            it = participants_.erase(it);
        } else {
            ++it;
        }
    }
}

const ParticipantSnapshot* ParticipantRoster::Find(const ParticipantId& id) const {
    const auto it = participants_.find(id);
    return it == participants_.end() ? nullptr : &it->second;
}

std::vector<ParticipantSnapshot> ParticipantRoster::GetAll() const {
    std::vector<ParticipantSnapshot> result;
    result.reserve(participants_.size());
    for (const auto& entry : participants_) {
        result.push_back(entry.second);
    }
    return result;
}

std::vector<ParticipantSnapshot> ParticipantRoster::GetAlive() const {
    std::vector<ParticipantSnapshot> result;
    for (const auto& entry : participants_) {
        if (entry.second.alive) {
            result.push_back(entry.second);
        }
    }
    return result;
}

std::vector<ParticipantSnapshot> ParticipantRoster::GetTeam(std::uint8_t teamId,
                                                             bool aliveOnly) const {
    std::vector<ParticipantSnapshot> result;
    if (!IsPlayableTeam(teamId)) {
        return result;
    }

    for (const auto& entry : participants_) {
        const ParticipantSnapshot& participant = entry.second;
        if (participant.teamId == teamId && (!aliveOnly || participant.alive)) {
            result.push_back(participant);
        }
    }
    return result;
}

std::size_t ParticipantRoster::CountTeam(std::uint8_t teamId, bool aliveOnly) const {
    if (!IsPlayableTeam(teamId)) {
        return 0;
    }

    std::size_t count = 0;
    for (const auto& entry : participants_) {
        const ParticipantSnapshot& participant = entry.second;
        if (participant.teamId == teamId && (!aliveOnly || participant.alive)) {
            ++count;
        }
    }
    return count;
}

bool ParticipantRoster::HasLivingParticipant(const ParticipantId& id) const {
    const ParticipantSnapshot* participant = Find(id);
    return participant != nullptr && participant->alive;
}

bool ParticipantRoster::IsPlayableTeam(std::uint8_t teamId) {
    return teamId == 1 || teamId == 2;
}

bool ParticipantRoster::IsFinite(const Vector3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
           std::isfinite(position.z);
}

std::optional<std::int32_t> ParticipantActorChannelMap::EncodeWirePlayerId(
    const ParticipantId& participant) {
    if (!participant.IsValid() || participant.value > kMaximumTaggedValue) {
        return std::nullopt;
    }

    // Humans retain their native small PlayerID.  Bots occupy the next positive
    // quarter of int32 space, so Human(7) and Bot(7) can never alias on retail.
    const std::uint32_t encoded = participant.IsBot()
        ? (0x40000000u | participant.value)
        : participant.value;
    return static_cast<std::int32_t>(encoded);
}

ParticipantActorChannelBinding* ParticipantActorChannelMap::Ensure(
    const ParticipantId& participant) {
    const auto existing = bindings_.find(participant);
    if (existing != bindings_.end()) return &existing->second;

    const std::optional<std::int32_t> wirePlayerId =
        EncodeWirePlayerId(participant);
    if (!wirePlayerId) return nullptr;

    for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
        if (slots_[slot] != SlotState::NeverUsed) continue;

        ParticipantActorChannelBinding binding;
        binding.participant = participant;
        binding.priChannel = static_cast<std::uint16_t>(
            kFirstChannel + slot * 2u);
        binding.pawnChannel = static_cast<std::uint16_t>(
            binding.priChannel + 1u);
        binding.wirePlayerId = *wirePlayerId;

        const auto inserted = bindings_.emplace(participant, binding);
        if (!inserted.second) return &inserted.first->second;
        slots_[slot] = SlotState::Active;
        return &inserted.first->second;
    }
    return nullptr;
}

const ParticipantActorChannelBinding* ParticipantActorChannelMap::Find(
    const ParticipantId& participant) const {
    const auto found = bindings_.find(participant);
    return found == bindings_.end() ? nullptr : &found->second;
}

ParticipantActorChannelBinding* ParticipantActorChannelMap::Find(
    const ParticipantId& participant) {
    const auto found = bindings_.find(participant);
    return found == bindings_.end() ? nullptr : &found->second;
}

std::optional<std::size_t> ParticipantActorChannelMap::FindSlot(
    std::uint16_t channel) const {
    if (channel < kFirstChannel || channel > kLastChannel) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(channel - kFirstChannel) / 2u;
}

const ParticipantActorChannelBinding* ParticipantActorChannelMap::FindByChannel(
    std::uint32_t channel) const {
    if (channel > std::numeric_limits<std::uint16_t>::max()) return nullptr;
    const std::optional<std::size_t> slot =
        FindSlot(static_cast<std::uint16_t>(channel));
    if (!slot || slots_[*slot] != SlotState::Active) return nullptr;

    for (const auto& entry : bindings_) {
        if (entry.second.priChannel == channel ||
            entry.second.pawnChannel == channel) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::optional<ParticipantId>
ParticipantActorChannelMap::ResolveOpenLivingPawn(
    std::uint32_t channel) const {
    const ParticipantActorChannelBinding* binding = FindByChannel(channel);
    if (!binding || binding->pawnChannel != channel ||
        binding->pawnState != ParticipantActorOpenState::Open ||
        binding->dead) {
        return std::nullopt;
    }
    return binding->participant;
}

bool ParticipantActorChannelMap::Retire(const ParticipantId& participant) {
    const auto found = bindings_.find(participant);
    if (found == bindings_.end()) return false;

    const std::optional<std::size_t> slot = FindSlot(found->second.priChannel);
    if (slot) slots_[*slot] = SlotState::Retired;
    bindings_.erase(found);
    return true;
}

std::size_t ParticipantActorChannelMap::RetireAll() {
    const std::size_t retired = bindings_.size();
    for (const auto& entry : bindings_) {
        const std::optional<std::size_t> slot = FindSlot(entry.second.priChannel);
        if (slot) slots_[*slot] = SlotState::Retired;
    }
    bindings_.clear();
    return retired;
}

void ParticipantActorChannelMap::Clear() {
    bindings_.clear();
    slots_.fill(SlotState::NeverUsed);
}

bool ParticipantActorChannelMap::MarkPriOpen(
    const ParticipantId& participant) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding ||
        binding->priState != ParticipantActorOpenState::Unopened) {
        return false;
    }
    binding->priState = ParticipantActorOpenState::Open;
    return true;
}

bool ParticipantActorChannelMap::MarkPawnOpen(
    const ParticipantId& participant, std::uint32_t generation) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding || generation == 0 || generation != binding->pawnGeneration ||
        binding->pawnState != ParticipantActorOpenState::Unopened) {
        return false;
    }
    binding->pawnState = ParticipantActorOpenState::Open;
    binding->dead = false;
    return true;
}

bool ParticipantActorChannelMap::MarkPawnClosing(
    const ParticipantId& participant) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding || binding->pawnState != ParticipantActorOpenState::Open) {
        return false;
    }
    binding->pawnState = ParticipantActorOpenState::Closing;
    return true;
}

bool ParticipantActorChannelMap::AcknowledgePawnClose(
    std::uint32_t pawnChannel) {
    const ParticipantActorChannelBinding* found = FindByChannel(pawnChannel);
    if (!found || found->pawnChannel != pawnChannel) return false;

    ParticipantActorChannelBinding* binding = Find(found->participant);
    if (!binding ||
        binding->pawnState != ParticipantActorOpenState::Closing) {
        return false;
    }
    binding->pawnState = ParticipantActorOpenState::Closed;
    return true;
}

bool ParticipantActorChannelMap::MarkChannelClosed(std::uint32_t channel) {
    if (channel > std::numeric_limits<std::uint16_t>::max()) return false;
    const ParticipantActorChannelBinding* found = FindByChannel(channel);
    if (!found) return false;

    ParticipantActorChannelBinding* binding = Find(found->participant);
    if (!binding) return false;
    if (binding->priChannel == channel) {
        binding->priState = ParticipantActorOpenState::Closed;
    } else if (binding->pawnChannel == channel) {
        // A peer-originated close/rejection is not an ACK for our outbound
        // close and cannot make this channel safe to reuse. Quarantine it in
        // Closing; only AcknowledgePawnClose may advance to Closed.
        binding->pawnState = ParticipantActorOpenState::Closing;
    } else {
        return false;
    }
    return true;
}

bool ParticipantActorChannelMap::SetDead(
    const ParticipantId& participant, bool dead) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding) return false;
    binding->dead = dead;
    return true;
}

std::optional<std::uint32_t>
ParticipantActorChannelMap::BeginPawnIncarnation(
    const ParticipantId& participant) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding || binding->pawnState != ParticipantActorOpenState::Closed) {
        return std::nullopt;
    }

    ++binding->pawnGeneration;
    if (binding->pawnGeneration == 0) binding->pawnGeneration = 1;
    binding->pawnState = ParticipantActorOpenState::Unopened;
    binding->dead = false;
    return binding->pawnGeneration;
}

std::uint32_t ParticipantActorChannelMap::AdvanceReliable(
    std::uint32_t current) {
    if (current >= kReliableSequenceLimit) current %= kReliableSequenceLimit;
    return (current + 1u) % kReliableSequenceLimit;
}

std::optional<std::uint32_t>
ParticipantActorChannelMap::NextPriReliableSequence(
    const ParticipantId& participant) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding) return std::nullopt;
    binding->priOutReliable = AdvanceReliable(binding->priOutReliable);
    return binding->priOutReliable;
}

std::optional<std::uint32_t>
ParticipantActorChannelMap::NextPawnReliableSequence(
    const ParticipantId& participant) {
    ParticipantActorChannelBinding* binding = Find(participant);
    if (!binding) return std::nullopt;
    binding->pawnOutReliable = AdvanceReliable(binding->pawnOutReliable);
    return binding->pawnOutReliable;
}

std::size_t ParticipantActorChannelMap::RetiredCount() const {
    std::size_t count = 0;
    for (const SlotState state : slots_) {
        if (state == SlotState::Retired) ++count;
    }
    return count;
}
