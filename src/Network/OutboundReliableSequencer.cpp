// src/Network/OutboundReliableSequencer.cpp

#include "Network/OutboundReliableSequencer.h"

#include <algorithm>
#include <atomic>

namespace PacketCodec {
namespace {

std::atomic<uint64_t> g_nextSequencerOwnerId{1u};

uint64_t AllocateSequencerOwnerId() noexcept {
    for (;;) {
        const uint64_t id =
            g_nextSequencerOwnerId.fetch_add(1u, std::memory_order_relaxed);
        if (id != 0u) return id;
    }
}

} // namespace

OutboundReliableSequencer::OutboundReliableSequencer() noexcept
    : m_ownerId(AllocateSequencerOwnerId()) {}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::Seed(uint32_t lastIssuedSequence) {
    if (!IsValidSequence(lastIssuedSequence)) {
        return std::unexpected(
            OutboundReliableSequenceError::InvalidSequence);
    }
    if (m_initialized) {
        return std::unexpected(
            OutboundReliableSequenceError::AlreadyInitialized);
    }

    m_nextSequence = Following(lastIssuedSequence);
    m_latestReservation.reset();
    m_initialized = true;
    return {};
}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::Adopt(uint32_t sequence) {
    if (!IsValidSequence(sequence)) {
        return std::unexpected(
            OutboundReliableSequenceError::InvalidSequence);
    }

    if (!m_initialized) {
        m_issuanceWindow.push_back(
            WindowEntry{sequence, WindowState::Published});
        m_inFlight.set(sequence);
        m_outstandingCount = 1u;
        m_nextSequence = Following(sequence);
        m_latestReservation.reset();
        m_initialized = true;
        return {};
    }
    if (m_latestReservation.has_value()) {
        return std::unexpected(
            OutboundReliableSequenceError::UnpublishedReservation);
    }

    if (m_inFlight.test(sequence)) {
        return std::unexpected(
            OutboundReliableSequenceError::SequenceInFlight);
    }
    if (sequence != m_nextSequence) {
        return std::unexpected(
            OutboundReliableSequenceError::NonContiguousAdoption);
    }
    if (m_issuanceWindow.size() >= kMaximumOutstanding) {
        return std::unexpected(
            OutboundReliableSequenceError::OutstandingLimit);
    }

    // deque::push_back has the strong exception guarantee; publish the rest of
    // the allocator mutation only after the window entry exists.
    m_issuanceWindow.push_back(
        WindowEntry{sequence, WindowState::Published});
    m_inFlight.set(sequence);
    ++m_outstandingCount;
    m_nextSequence = Following(sequence);
    m_latestReservation.reset();
    return {};
}

OutboundReliableSequencer::ReservationResult
OutboundReliableSequencer::ReserveBatch(size_t count) {
    const MutationResult canReserve = CanReserveBatch(count);
    if (!canReserve) {
        return std::unexpected(canReserve.error());
    }

    std::vector<uint32_t> sequences;
    sequences.reserve(count);
    uint32_t sequence = m_nextSequence;
    for (size_t index = 0; index < count; ++index) {
        sequences.push_back(sequence);
        sequence = Following(sequence);
    }

    // Build the replacement issuance window before mutating live state. This
    // preserves ReserveBatch's all-or-nothing contract if allocation throws.
    auto updatedWindow = m_issuanceWindow;
    for (const uint32_t reservedSequence : sequences) {
        updatedWindow.push_back(
            WindowEntry{reservedSequence, WindowState::Reserved});
    }

    Reservation reservation(
        m_ownerId, NextReservationId(), std::move(sequences));

    // All fallible work and all validation complete before state mutation.
    m_issuanceWindow.swap(updatedWindow);
    for (const uint32_t reservedSequence : reservation) {
        m_inFlight.set(reservedSequence);
    }
    m_outstandingCount += reservation.size();
    m_nextSequence = sequence;
    m_latestReservation =
        BatchRecord{reservation.m_id, reservation.front(), reservation.size()};
    return reservation;
}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::CanReserveBatch(size_t count) const {
    if (!m_initialized) {
        return std::unexpected(
            OutboundReliableSequenceError::Uninitialized);
    }
    if (m_latestReservation.has_value()) {
        return std::unexpected(
            OutboundReliableSequenceError::UnpublishedReservation);
    }
    if (count == 0u) {
        return std::unexpected(
            OutboundReliableSequenceError::InvalidBatchSize);
    }
    // Subtraction avoids overflowing size_t for a hostile/direct huge count.
    if (count > kMaximumOutstanding - m_issuanceWindow.size()) {
        return std::unexpected(
            OutboundReliableSequenceError::OutstandingLimit);
    }

    uint32_t sequence = m_nextSequence;
    for (size_t index = 0; index < count; ++index) {
        if (m_inFlight.test(sequence)) {
            return std::unexpected(
                OutboundReliableSequenceError::SequenceInFlight);
        }
        sequence = Following(sequence);
    }
    return {};
}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::ValidateLatestBatch(
    const Reservation& reservation) const {
    if (!m_initialized) {
        return std::unexpected(
            OutboundReliableSequenceError::Uninitialized);
    }
    const std::vector<uint32_t>& sequences = reservation.m_sequences;
    if (sequences.empty() || sequences.size() > kMaximumOutstanding) {
        return std::unexpected(
            OutboundReliableSequenceError::InvalidBatchSize);
    }

    uint32_t expectedSequence = sequences.front();
    for (const uint32_t sequence : sequences) {
        if (!IsValidSequence(sequence)) {
            return std::unexpected(
                OutboundReliableSequenceError::InvalidSequence);
        }
        if (sequence != expectedSequence) {
            return std::unexpected(
                OutboundReliableSequenceError::NonContiguousBatch);
        }
        expectedSequence = Following(expectedSequence);
    }

    if (reservation.m_ownerId != m_ownerId ||
        !m_latestReservation.has_value() ||
        m_latestReservation->id != reservation.m_id ||
        m_latestReservation->firstSequence != sequences.front() ||
        m_latestReservation->count != sequences.size() ||
        m_nextSequence != expectedSequence) {
        return std::unexpected(
            OutboundReliableSequenceError::NotLatestReservation);
    }

    for (const uint32_t sequence : sequences) {
        if (!m_inFlight.test(sequence)) {
            return std::unexpected(
                OutboundReliableSequenceError::NotInFlight);
        }
    }

    if (m_issuanceWindow.size() < sequences.size()) {
        return std::unexpected(
            OutboundReliableSequenceError::NotLatestReservation);
    }
    const size_t tailOffset = m_issuanceWindow.size() - sequences.size();
    for (size_t index = 0; index < sequences.size(); ++index) {
        const WindowEntry& entry = m_issuanceWindow[tailOffset + index];
        if (entry.sequence != sequences[index]) {
            return std::unexpected(
                OutboundReliableSequenceError::NotLatestReservation);
        }
        if (entry.state != WindowState::Reserved) {
            return std::unexpected(
                OutboundReliableSequenceError::UnpublishedReservation);
        }
    }

    return {};
}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::CancelBatch(
    const Reservation& reservation) {
    const MutationResult validation = ValidateLatestBatch(reservation);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

    // Validation is complete. bitset resets and optional reset cannot throw.
    for (const uint32_t sequence : reservation) {
        m_inFlight.reset(sequence);
    }
    for (size_t index = 0; index < reservation.size(); ++index) {
        m_issuanceWindow.pop_back();
    }
    m_outstandingCount -= reservation.size();
    m_nextSequence = reservation.front();
    m_latestReservation.reset();
    return {};
}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::CommitBatch(
    const Reservation& reservation) {
    const MutationResult validation = ValidateLatestBatch(reservation);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

    const size_t tailOffset = m_issuanceWindow.size() - reservation.size();
    for (size_t index = 0; index < reservation.size(); ++index) {
        m_issuanceWindow[tailOffset + index].state = WindowState::Published;
    }
    m_latestReservation.reset();
    return {};
}

OutboundReliableSequencer::MutationResult
OutboundReliableSequencer::Release(uint32_t sequence) {
    if (!IsValidSequence(sequence)) {
        return std::unexpected(
            OutboundReliableSequenceError::InvalidSequence);
    }
    if (!m_initialized) {
        return std::unexpected(
            OutboundReliableSequenceError::Uninitialized);
    }
    if (!m_inFlight.test(sequence)) {
        return std::unexpected(
            OutboundReliableSequenceError::NotInFlight);
    }

    const auto windowEntry = std::find_if(
        m_issuanceWindow.begin(), m_issuanceWindow.end(),
        [sequence](const WindowEntry& entry) {
            return entry.sequence == sequence &&
                   entry.state != WindowState::Acknowledged;
        });
    if (windowEntry == m_issuanceWindow.end()) {
        return std::unexpected(
            OutboundReliableSequenceError::NotInFlight);
    }
    if (windowEntry->state == WindowState::Reserved) {
        return std::unexpected(
            OutboundReliableSequenceError::UnpublishedReservation);
    }

    m_inFlight.reset(sequence);
    windowEntry->state = WindowState::Acknowledged;
    --m_outstandingCount;
    while (!m_issuanceWindow.empty() &&
           m_issuanceWindow.front().state == WindowState::Acknowledged) {
        m_issuanceWindow.pop_front();
    }
    return {};
}

void OutboundReliableSequencer::Clear() noexcept {
    m_inFlight.reset();
    m_outstandingCount = 0u;
    m_nextSequence = 0u;
    m_issuanceWindow.clear();
    m_latestReservation.reset();
    m_initialized = false;
}

std::optional<uint32_t>
OutboundReliableSequencer::NextSequence() const noexcept {
    if (!m_initialized) return std::nullopt;
    return m_nextSequence;
}

bool OutboundReliableSequencer::IsInFlight(uint32_t sequence) const noexcept {
    return IsValidSequence(sequence) && m_inFlight.test(sequence);
}

uint64_t OutboundReliableSequencer::NextReservationId() noexcept {
    const uint64_t id = m_nextReservationId;
    ++m_nextReservationId;
    if (m_nextReservationId == 0u) {
        m_nextReservationId = 1u;
    }
    return id;
}

} // namespace PacketCodec
