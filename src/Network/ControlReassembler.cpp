// src/Network/ControlReassembler.cpp
// See ControlReassembler.h.

#include "Network/ControlReassembler.h"

#include "Network/NetMessages.h"
#include "Utils/Logger.h"

#include <algorithm>

namespace PacketCodec {

// Bytes a buffered bunch occupies. Computed in size_t (not uint32_t) so the
// "+7" round-up can never wrap, and clamped to the actual payload buffer size so
// a bogus payloadBits can never imply more bytes than we actually hold.
static size_t BunchByteSize(const Bunch& b) {
    const size_t implied = (static_cast<size_t>(b.payloadBits) + 7u) / 8u;
    return std::min(implied, b.payload.size());
}

ControlReassembler::ControlReassembler(MessageFn onMessage)
    : m_onMessage(std::move(onMessage)) {}

void ControlReassembler::OnBunch(const Bunch& bunch) {
    Logger::Trace("[ControlReassembler] OnBunch ch=%u reliable=%d open=%d ctrl=%d seq=%u type=%u bits=%u | m_nextSeq=%u pending=%zu",
                  bunch.chIndex, (int)bunch.bReliable, (int)bunch.bOpen, (int)bunch.bControl,
                  bunch.chSequence, bunch.chType, bunch.payloadBits, m_nextSeq, m_pending.size());
    // Only reliable control-channel (index 0) bunches participate in the ordered
    // message stream. Everything else is ignored here.
    if (bunch.chIndex != static_cast<uint32_t>(kControlChannelIndex) || !bunch.bReliable) {
        return;
    }
    const ChSequenceDelta sequence =
        ClassifyChSequenceFromExpected(m_nextSeq, bunch.chSequence);
    if (sequence.relation == ChSequenceRelation::Invalid ||
        sequence.relation == ChSequenceRelation::Behind) {
        return;
    }
    // Bound out-of-order buffering to UE3's RELIABLE_BUFFER window. Values in
    // the newer half of the modulo space but beyond a legitimate sender's 127
    // outstanding ordinary reliable bunches are rejected as a receive hole.
    if (sequence.relation == ChSequenceRelation::Ahead &&
        sequence.forwardDistance > kMaximumForwardDistance) {
        return;
    }
    // Cap the pending map so hostile direct callers cannot retain unbounded data
    // while the expected sequence is absent.
    // Total buffered payload cap. NMT-phase bunches can be ~kNmtMaxPacketBytes
    // each; without a byte cap an attacker could pin kMaxPending oversized bunches
    // in memory. 256 KiB is far above any legitimate handshake/NMT reassembly need
    // (control messages are tiny and almost always drain per-bunch immediately).
    // Reject a bunch whose declared payloadBits exceeds the bits actually present
    // in its payload buffer - a malformed/forged bunch. Valid bunches always have
    // payloadBits <= payload.size()*8, so this never rejects correct-path input.
    if (static_cast<uint64_t>(bunch.payloadBits) >
        static_cast<uint64_t>(bunch.payload.size()) * 8u) {
        Logger::Warn("[ControlReassembler] dropping bunch seq=%u: payloadBits=%u exceeds payload bytes=%zu",
                     bunch.chSequence, bunch.payloadBits, bunch.payload.size());
        return;
    }
    const bool alreadyBuffered = m_pending.find(bunch.chSequence) != m_pending.end();
    if (m_pending.size() >= kMaximumPendingBunches && !alreadyBuffered) {
        Logger::Warn("[ControlReassembler] pending bunch cap (%zu) hit; dropping seq=%u",
                     kMaximumPendingBunches, bunch.chSequence);
        return;
    }
    const size_t addBytes = BunchByteSize(bunch);
    if (!alreadyBuffered &&
        m_pendingBytes + addBytes > kMaximumPendingPayloadBytes) {
        Logger::Warn("[ControlReassembler] pending byte cap (%zu) hit (have=%zu add=%zu); dropping seq=%u",
                     kMaximumPendingPayloadBytes, m_pendingBytes, addBytes,
                     bunch.chSequence);
        return;
    }
    // dedup: keep the first copy of a given sequence (ignore later differing copies)
    auto ins = m_pending.emplace(bunch.chSequence, bunch);
    if (ins.second) {
        m_pendingBytes += addBytes;  // only count newly-inserted bunches
    }
    Drain();
}

void ControlReassembler::Drain() {
    // A callback may synchronously feed another decoded packet. Keep those
    // arrivals in the ordinary bounded pending map; the outer drain observes them
    // after the callback returns instead of recursively redispatching this bunch.
    if (m_dispatching) {
        return;
    }
    m_dispatching = true;

    // UE3 reconstructs the 10-bit wire value relative to InReliable[ChIndex]
    // and UChannel::ReceivedRawBunch releases only exactly InReliable+1. Ch0 is
    // not connection-global: an apparent gap is a delayed/lost ch0 reliable (or
    // an upstream decode bug), never evidence that another channel consumed it.
    try {
        for (;;) {
            auto it = m_pending.find(m_nextSeq);
            if (it == m_pending.end()) {
                break;
            }
            const Bunch& b = it->second;
            const size_t nbytes = BunchByteSize(b);  // cannot overflow or over-read
            if (m_onMessage && b.payloadBits > 0 && nbytes > 0) {
                // Build the callback value before mutating sequencing state. The
                // current map node remains stable across reentrant std::map inserts.
                const std::vector<uint8_t> message(
                    b.payload.begin(), b.payload.begin() + nbytes);
                m_onMessage(message);
            }
            // Commit only after the callback succeeds. A throw leaves this bunch,
            // its accounting, and the cursor intact so a retransmit can retry it.
            m_pendingBytes -= std::min(m_pendingBytes, BunchByteSize(b));
            m_pending.erase(it);
            m_nextSeq = AdvanceChSequence(m_nextSeq);
        }
    } catch (...) {
        m_dispatching = false;
        throw;
    }
    m_dispatching = false;
}

} // namespace PacketCodec
