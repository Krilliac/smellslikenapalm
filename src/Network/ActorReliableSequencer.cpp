// src/Network/ActorReliableSequencer.cpp

#include "Network/ActorReliableSequencer.h"

#include <algorithm>
#include <utility>

namespace PacketCodec {

ActorReliableSequencer::ActorReliableSequencer(uint32_t reorderWindow,
                                               uint32_t firstSequence,
                                               size_t maximumPendingBunches,
                                               size_t maximumPendingPayloadBytes)
    : m_reorderWindow(std::min(reorderWindow, kMaximumReorderWindow)),
      m_firstSequence(firstSequence < kMaxChSequence
                          ? firstSequence
                          : kDefaultFirstSequence),
      m_maximumPendingBunches(maximumPendingBunches),
      m_maximumPendingPayloadBytes(maximumPendingPayloadBytes) {}

bool ActorReliableSequencer::IsValidChannel(uint32_t channelIndex) {
    return channelIndex < static_cast<uint32_t>(kMaxChannels);
}

bool ActorReliableSequencer::HasValidPayloadBounds(const Bunch& bunch) {
    // Avoid payload.size()*8: that expression can overflow size_t for a
    // deliberately huge direct caller even though PacketCodec never produces
    // such a vector. Rounding the uint32_t bit count down to required bytes is
    // overflow-safe on both 32- and 64-bit targets.
    const size_t requiredBytes =
        static_cast<size_t>(bunch.payloadBits / 8u) +
        (bunch.payloadBits % 8u == 0u ? 0u : 1u);
    return requiredBytes <= bunch.payload.size();
}

bool ActorReliableSequencer::WasRecentlyReleased(const ChannelState& state,
                                                 uint32_t sequence) {
    return std::find(state.recentlyReleased.begin(), state.recentlyReleased.end(),
                     sequence) != state.recentlyReleased.end();
}

void ActorReliableSequencer::RememberReleased(ChannelState& state,
                                              uint32_t sequence) const {
    state.recentlyReleased.push_back(sequence);
    // One extra entry recognizes a duplicate of the bunch immediately behind
    // the receive cursor even when reordering is disabled.
    const size_t historyLimit = static_cast<size_t>(m_reorderWindow) + 1u;
    while (state.recentlyReleased.size() > historyLimit) {
        state.recentlyReleased.pop_front();
    }
}

void ActorReliableSequencer::ReleaseOne(
    ChannelState& state, const Bunch& bunch,
    ActorReliableSequenceResult& result) const {
    result.released.push_back(bunch);
    RememberReleased(state, bunch.chSequence);
    state.nextSequence = AdvanceChSequence(state.nextSequence);
}

void ActorReliableSequencer::RemovePendingAccounting(const Bunch& bunch) {
    // These checks make cleanup fail-safe even if a future code path violates an
    // internal accounting invariant; neither counter is ever allowed to wrap.
    if (m_pendingBunchCount > 0u) {
        --m_pendingBunchCount;
    }
    const size_t payloadBytes = bunch.payload.size();
    m_pendingPayloadBytes = payloadBytes <= m_pendingPayloadBytes
        ? m_pendingPayloadBytes - payloadBytes
        : 0u;
}

void ActorReliableSequencer::RemoveChannelPendingAccounting(
    const ChannelState& state) {
    for (const auto& [sequence, bunch] : state.pending) {
        (void)sequence;
        RemovePendingAccounting(bunch);
    }
}

ActorReliableSequenceResult ActorReliableSequencer::Push(const Bunch& bunch) {
    ActorReliableSequenceResult result;

    if (!bunch.bReliable) {
        result.status = ActorReliableSequenceStatus::Unreliable;
        return result;
    }
    if (!IsValidChannel(bunch.chIndex) || bunch.chSequence >= kMaxChSequence ||
        !HasValidPayloadBounds(bunch)) {
        result.status = ActorReliableSequenceStatus::InvalidBunch;
        return result;
    }

    auto [channelIt, inserted] =
        m_channels.try_emplace(bunch.chIndex, m_firstSequence);
    (void)inserted;
    ChannelState& state = channelIt->second;

    if (state.pending.find(bunch.chSequence) != state.pending.end() ||
        WasRecentlyReleased(state, bunch.chSequence)) {
        result.status = ActorReliableSequenceStatus::Duplicate;
        return result;
    }

    const ChSequenceDelta sequence =
        ClassifyChSequenceFromExpected(state.nextSequence, bunch.chSequence);
    if (sequence.relation == ChSequenceRelation::Equal) {
        result.status = ActorReliableSequenceStatus::Released;
        ReleaseOne(state, bunch, result);

        // An in-order bunch can close a gap and release several buffered bunches.
        for (;;) {
            auto pendingIt = state.pending.find(state.nextSequence);
            if (pendingIt == state.pending.end()) {
                break;
            }
            RemovePendingAccounting(pendingIt->second);
            Bunch ready = std::move(pendingIt->second);
            state.pending.erase(pendingIt);
            ReleaseOne(state, ready, result);
        }
        return result;
    }

    if (sequence.relation == ChSequenceRelation::Ahead &&
        sequence.forwardDistance <= m_reorderWindow) {
        const size_t payloadBytes = bunch.payload.size();
        // Subtraction after checking the addend avoids size_t addition overflow.
        // A zero-byte bunch still consumes one entry from the bunch-count cap.
        if (m_pendingBunchCount >= m_maximumPendingBunches ||
            payloadBytes > m_maximumPendingPayloadBytes ||
            m_pendingPayloadBytes >
                m_maximumPendingPayloadBytes - payloadBytes) {
            result.status = ActorReliableSequenceStatus::CapacityExceeded;
            return result;
        }
        state.pending.emplace(bunch.chSequence, bunch);
        ++m_pendingBunchCount;
        m_pendingPayloadBytes += payloadBytes;
        result.status = ActorReliableSequenceStatus::Buffered;
        return result;
    }

    // UE3 MakeRelative treats an exact half-cycle as the older side of the
    // cursor. It is still dropped either way, but matching that classification
    // keeps diagnostics and boundary behavior faithful to the engine.
    if (sequence.relation == ChSequenceRelation::Behind) {
        result.status = ActorReliableSequenceStatus::Stale;
    } else {
        result.status = ActorReliableSequenceStatus::GapOverflow;
    }
    return result;
}

void ActorReliableSequencer::Clear() {
    m_channels.clear();
    m_pendingBunchCount = 0;
    m_pendingPayloadBytes = 0;
}

bool ActorReliableSequencer::ResetChannel(uint32_t channelIndex) {
    return ResetChannel(channelIndex, m_firstSequence);
}

bool ActorReliableSequencer::ResetChannel(uint32_t channelIndex,
                                          uint32_t nextSequence) {
    if (!IsValidChannel(channelIndex) || nextSequence >= kMaxChSequence) {
        return false;
    }
    const auto channelIt = m_channels.find(channelIndex);
    if (channelIt != m_channels.end()) {
        RemoveChannelPendingAccounting(channelIt->second);
        m_channels.erase(channelIt);
    }
    m_channels.try_emplace(channelIndex, nextSequence);
    return true;
}

bool ActorReliableSequencer::DiscardPending(uint32_t channelIndex) {
    if (!IsValidChannel(channelIndex)) return false;
    const auto channelIt = m_channels.find(channelIndex);
    if (channelIt == m_channels.end()) return true;

    RemoveChannelPendingAccounting(channelIt->second);
    channelIt->second.pending.clear();
    return true;
}

size_t ActorReliableSequencer::RetirePending(uint32_t channelIndex) {
    if (!IsValidChannel(channelIndex)) return 0u;
    const auto channelIt = m_channels.find(channelIndex);
    if (channelIt == m_channels.end() || channelIt->second.pending.empty()) {
        return 0u;
    }

    ChannelState& state = channelIt->second;
    uint32_t farthestDistance = 0u;
    for (const auto& [sequence, bunch] : state.pending) {
        (void)bunch;
        farthestDistance = std::max(
            farthestDistance,
            ClassifyChSequenceFromExpected(
                state.nextSequence, sequence).forwardDistance);
    }
    const size_t retired = state.pending.size();
    RemoveChannelPendingAccounting(state);
    state.pending.clear();
    state.nextSequence =
        AdvanceChSequence(state.nextSequence, farthestDistance + 1u);
    return retired;
}

uint32_t ActorReliableSequencer::NextSequence(uint32_t channelIndex) const {
    const auto it = m_channels.find(channelIndex);
    return it == m_channels.end() ? m_firstSequence : it->second.nextSequence;
}

size_t ActorReliableSequencer::PendingBunchCount(uint32_t channelIndex) const {
    const auto it = m_channels.find(channelIndex);
    return it == m_channels.end() ? 0u : it->second.pending.size();
}

size_t ActorReliableSequencer::PendingBunchCount() const {
    return m_pendingBunchCount;
}

} // namespace PacketCodec
