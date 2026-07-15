// src/Network/OutboundReliableSequencer.h
//
// Channel-agnostic allocation for UE3 outbound reliable ChSequence values.
// Each instance owns one channel's modulo-1024 sequence space. Reserved values
// remain unavailable until ACK release or transactional cancellation of the
// latest unpublished batch.
//
// This class is not internally synchronized. The connection owner must
// serialize seed/adopt/reserve/cancel/commit/release operations for a channel.

#pragma once

#include "Network/PacketCodec.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace PacketCodec {

enum class OutboundReliableSequenceError : uint8_t {
    Uninitialized,
    AlreadyInitialized,
    InvalidSequence,
    InvalidBatchSize,
    OutstandingLimit,
    SequenceInFlight,
    NonContiguousAdoption,
    NonContiguousBatch,
    NotLatestReservation,
    NotInFlight,
    UnpublishedReservation,
};

class OutboundReliableSequencer {
public:
    // Modular ordering is unambiguous only strictly inside half a cycle.
    static constexpr size_t kMaximumOutstanding =
        (static_cast<size_t>(kMaxChSequence) / 2u) - 1u;

    OutboundReliableSequencer() noexcept;
    OutboundReliableSequencer(const OutboundReliableSequencer&) = delete;
    OutboundReliableSequencer& operator=(const OutboundReliableSequencer&) = delete;
    OutboundReliableSequencer(OutboundReliableSequencer&&) = delete;
    OutboundReliableSequencer& operator=(OutboundReliableSequencer&&) = delete;

    // Opaque reservation token. Sequence values are read-only to callers and
    // the monotonic id prevents an ancient same-shaped batch from cancelling a
    // new reservation after the modulo cursor eventually makes a full lap.
    class Reservation {
    public:
        using const_iterator = std::vector<uint32_t>::const_iterator;

        Reservation() = default;
        Reservation(const Reservation&) = default;
        Reservation(Reservation&&) noexcept = default;
        Reservation& operator=(const Reservation&) = default;
        Reservation& operator=(Reservation&&) noexcept = default;
        [[nodiscard]] bool empty() const noexcept { return m_sequences.empty(); }
        [[nodiscard]] size_t size() const noexcept { return m_sequences.size(); }
        [[nodiscard]] uint32_t front() const { return m_sequences.front(); }
        [[nodiscard]] const uint32_t& operator[](size_t index) const {
            return m_sequences[index];
        }
        [[nodiscard]] const_iterator begin() const noexcept {
            return m_sequences.begin();
        }
        [[nodiscard]] const_iterator end() const noexcept {
            return m_sequences.end();
        }
        [[nodiscard]] const std::vector<uint32_t>& SequenceValues() const noexcept {
            return m_sequences;
        }

    private:
        friend class OutboundReliableSequencer;
        Reservation(uint64_t ownerId, uint64_t id,
                    std::vector<uint32_t>&& sequences) noexcept
            : m_ownerId(ownerId), m_id(id),
              m_sequences(std::move(sequences)) {}

        uint64_t m_ownerId = 0;
        uint64_t m_id = 0;
        std::vector<uint32_t> m_sequences;
    };
    using ReservationResult =
        std::expected<Reservation, OutboundReliableSequenceError>;
    using MutationResult =
        std::expected<void, OutboundReliableSequenceError>;

    // Initialize from an acknowledged (or otherwise no-longer-in-flight)
    // last-issued value. The first reservation is the following modulo value.
    [[nodiscard]] MutationResult Seed(uint32_t lastIssuedSequence);

    // Adopt an externally assigned reliable sequence. On an uninitialized
    // instance this establishes the cursor and marks the adopted sequence as
    // in flight. Later adoptions must be exactly contiguous with the cursor
    // and remain inside the half-cycle issuance window.
    [[nodiscard]] MutationResult Adopt(uint32_t sequence);

    // Reserve count contiguous values and mark all of them in flight. At most
    // one unpublished reservation may exist. Failure is transactional: neither
    // the cursor nor the in-flight/window state is changed.
    [[nodiscard]] ReservationResult ReserveBatch(size_t count);

    // Apply ReserveBatch's complete validation without allocating a token or
    // mutating the cursor/window. This is used to preflight a synchronous
    // authority callback that may itself consume a known number of sequences
    // before the caller performs its real reservation.
    [[nodiscard]] MutationResult CanReserveBatch(size_t count) const;

    // Roll back the latest successful ReserveBatch call before its values are
    // published. Only the exact, still-in-flight batch at the current cursor
    // can be cancelled; failure leaves all allocator state unchanged.
    [[nodiscard]] MutationResult
    CancelBatch(const Reservation& reservation);

    // Mark the latest reservation as published after it is durably queued for
    // retransmission. Sequence state is unchanged, but rollback eligibility is
    // cleared so a transmitted reliable batch can never rewind the cursor.
    [[nodiscard]] MutationResult
    CommitBatch(const Reservation& reservation);

    // Release one in-flight value after its reliable bunch is acknowledged.
    // Release does not rewind the cursor; unpublished batches must use
    // CancelBatch so the next reservation cannot create a reliable gap.
    [[nodiscard]] MutationResult Release(uint32_t sequence);

    // Return to the initial, unseeded state. Connection teardown/replacement is
    // expected to clear the entire allocator rather than preserve old ACK state.
    void Clear() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] std::optional<uint32_t> NextSequence() const noexcept;
    [[nodiscard]] bool IsInFlight(uint32_t sequence) const noexcept;
    [[nodiscard]] size_t OutstandingCount() const noexcept {
        return m_outstandingCount;
    }
    // Includes out-of-order ACKed successors that cannot be reclaimed until
    // every older issued sequence is acknowledged. This span, rather than the
    // raw in-flight count, is the modulo-ordering safety boundary.
    [[nodiscard]] size_t IssuanceWindowSize() const noexcept {
        return m_issuanceWindow.size();
    }
    [[nodiscard]] size_t AvailableCapacity() const noexcept {
        return kMaximumOutstanding - m_issuanceWindow.size();
    }

private:
    struct BatchRecord {
        uint64_t id = 0;
        uint32_t firstSequence = 0;
        size_t count = 0;
    };
    enum class WindowState : uint8_t {
        Reserved,
        Published,
        Acknowledged,
    };
    struct WindowEntry {
        uint32_t sequence = 0;
        WindowState state = WindowState::Reserved;
    };

    static constexpr bool IsValidSequence(uint32_t sequence) noexcept {
        return sequence < kMaxChSequence;
    }
    static constexpr uint32_t Following(uint32_t sequence) noexcept {
        return (sequence + 1u) % kMaxChSequence;
    }
    [[nodiscard]] MutationResult
    ValidateLatestBatch(const Reservation& reservation) const;
    [[nodiscard]] uint64_t NextReservationId() noexcept;

    const uint64_t m_ownerId;
    bool m_initialized = false;
    uint32_t m_nextSequence = 0;
    size_t m_outstandingCount = 0;
    std::bitset<kMaxChSequence> m_inFlight;
    // Contiguous issuance order from the oldest not-yet-contiguously-ACKed
    // sequence through the most recently issued sequence. An entry remains in
    // this deque after an out-of-order ACK so a single old gap cannot let the
    // cursor advance more than half a modulo cycle.
    std::deque<WindowEntry> m_issuanceWindow;
    std::optional<BatchRecord> m_latestReservation;
    uint64_t m_nextReservationId = 1u;
};

static_assert(OutboundReliableSequencer::kMaximumOutstanding == 511u);
static_assert(kMaxChSequence == 1024u);

} // namespace PacketCodec
