// src/Network/ActorReliableSequencer.h
//
// Bounded receive-side ordering for UE3 reliable actor-channel bunches. Reliable
// ChSequence values are per channel and wrap in PacketCodec::kMaxChSequence
// space. This class deliberately does not handle unreliable bunches: their wire
// order is caller-owned and they must never be delayed behind a reliable gap.

#pragma once

#include "Network/PacketCodec.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

namespace PacketCodec {

enum class ActorReliableSequenceStatus : uint8_t {
    Released,          // this bunch and zero or more buffered successors are ready
    Buffered,          // accepted inside the forward reorder window
    Duplicate,         // sequence is already buffered or was recently released
    Stale,             // sequence is behind the channel's receive cursor
    GapOverflow,       // forward gap exceeds the bounded reorder window
    CapacityExceeded,  // connection-wide pending bunch/byte cap was reached
    Unreliable,        // caller must process this bunch without this sequencer
    InvalidBunch       // channel or sequence lies outside its wire bound
};

struct ActorReliableSequenceResult {
    ActorReliableSequenceStatus status = ActorReliableSequenceStatus::InvalidBunch;
    std::vector<Bunch> released;
};

class ActorReliableSequencer {
public:
    static constexpr uint32_t kDefaultFirstSequence = 1;
    static constexpr uint32_t kDefaultReorderWindow = 64;
    // Modular ordering is unambiguous only inside half of the sequence space.
    static constexpr uint32_t kMaximumReorderWindow =
        (kMaxChSequence / 2u) - 1u;
    // These connection-wide caps complement the per-channel sequence window.
    // Without them, a peer could fill the window on many different channels and
    // retain far more memory than any single-channel bound suggests.
    static constexpr size_t kDefaultMaximumPendingBunches = 4096;
    static constexpr size_t kDefaultMaximumPendingPayloadBytes =
        4u * 1024u * 1024u;

    explicit ActorReliableSequencer(
        uint32_t reorderWindow = kDefaultReorderWindow,
        uint32_t firstSequence = kDefaultFirstSequence,
        size_t maximumPendingBunches = kDefaultMaximumPendingBunches,
        size_t maximumPendingPayloadBytes =
            kDefaultMaximumPendingPayloadBytes);

    // Accept one decoded reliable bunch. Released bunches are returned in exact
    // per-channel ChSequence order; an in-order arrival can release previously
    // buffered successors as part of the same result.
    ActorReliableSequenceResult Push(const Bunch& bunch);

    // Drop all channel cursors and buffered bunches. New channels restart at the
    // constructor's firstSequence value.
    void Clear();

    // Drop one channel's state. The one-argument form restarts it at the
    // constructor default; the explicit form is useful when adopting a channel
    // whose first reliable sequence was learned from an actor-open bunch.
    bool ResetChannel(uint32_t channelIndex);
    bool ResetChannel(uint32_t channelIndex, uint32_t nextSequence);

    // Drop buffered successors for a closed channel incarnation while
    // preserving UE3's per-index InReliable cursor and recent-delivery history.
    // Channel reuse continues the sequence space; only a new connection Clear()
    // or an explicit ResetChannel() may reseed it.
    bool DiscardPending(uint32_t channelIndex);

    uint32_t NextSequence(uint32_t channelIndex) const;
    size_t PendingBunchCount(uint32_t channelIndex) const;
    size_t PendingBunchCount() const;
    size_t PendingPayloadBytes() const { return m_pendingPayloadBytes; }
    size_t ChannelCount() const { return m_channels.size(); }
    uint32_t ReorderWindow() const { return m_reorderWindow; }
    size_t MaximumPendingBunches() const { return m_maximumPendingBunches; }
    size_t MaximumPendingPayloadBytes() const {
        return m_maximumPendingPayloadBytes;
    }

private:
    struct ChannelState {
        explicit ChannelState(uint32_t firstSequence)
            : nextSequence(firstSequence) {}

        uint32_t nextSequence;
        std::map<uint32_t, Bunch> pending;
        std::deque<uint32_t> recentlyReleased;
    };

    static uint32_t ForwardDistance(uint32_t from, uint32_t to);
    static bool WasRecentlyReleased(const ChannelState& state, uint32_t sequence);
    void RememberReleased(ChannelState& state, uint32_t sequence) const;
    void ReleaseOne(ChannelState& state, const Bunch& bunch,
                    ActorReliableSequenceResult& result) const;
    void RemovePendingAccounting(const Bunch& bunch);
    void RemoveChannelPendingAccounting(const ChannelState& state);
    static bool IsValidChannel(uint32_t channelIndex);
    static bool HasValidPayloadBounds(const Bunch& bunch);

    uint32_t m_reorderWindow;
    uint32_t m_firstSequence;
    size_t m_maximumPendingBunches;
    size_t m_maximumPendingPayloadBytes;
    size_t m_pendingBunchCount = 0;
    size_t m_pendingPayloadBytes = 0;
    std::unordered_map<uint32_t, ChannelState> m_channels;
};

} // namespace PacketCodec
