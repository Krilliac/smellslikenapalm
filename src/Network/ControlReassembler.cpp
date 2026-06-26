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
    // Already-consumed sequence numbers are duplicate retransmits - ignore.
    if (bunch.chSequence < m_nextSeq) {
        return;
    }
    // Bound out-of-order buffering: a garbled/malicious client could send bunches
    // with arbitrary far-future ChSequence (0..1023). Reject sequences too far
    // ahead of what we expect, and cap the pending map, so reassembly can never
    // grow without bound while waiting for a gap that will never arrive.
    constexpr uint32_t kMaxSeqAhead = 64;
    constexpr size_t kMaxPending = 128;
    // Total buffered payload cap. NMT-phase bunches can be ~kNmtMaxPacketBytes
    // each; without a byte cap an attacker could pin kMaxPending oversized bunches
    // in memory. 256 KiB is far above any legitimate handshake/NMT reassembly need
    // (control messages are tiny and almost always drain per-bunch immediately).
    constexpr size_t kMaxPendingBytes = 256 * 1024;
    // Use uint64_t for the seq-ahead comparison so m_nextSeq + kMaxSeqAhead can
    // never wrap a uint32_t near the counter's top.
    if (static_cast<uint64_t>(bunch.chSequence) >
        static_cast<uint64_t>(m_nextSeq) + kMaxSeqAhead) {
        return;
    }
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
    if (m_pending.size() >= kMaxPending && !alreadyBuffered) {
        Logger::Warn("[ControlReassembler] pending bunch cap (%zu) hit; dropping seq=%u",
                     kMaxPending, bunch.chSequence);
        return;
    }
    const size_t addBytes = BunchByteSize(bunch);
    if (!alreadyBuffered && m_pendingBytes + addBytes > kMaxPendingBytes) {
        Logger::Warn("[ControlReassembler] pending byte cap (%zu) hit (have=%zu add=%zu); dropping seq=%u",
                     kMaxPendingBytes, m_pendingBytes, addBytes, bunch.chSequence);
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
    // Deliver each in-order reliable control bunch's payload as ONE complete
    // message. RS2 control messages in the handshake + early NMT phase
    // (HandshakeStart/Challenge/Response/Complete, Hello, Netspeed, ...) each fit
    // in a single bunch, so per-bunch delivery is correct here. (A genuinely
    // multi-bunch message - e.g. the large Login auth blob - would need
    // cross-bunch re-accumulation; that is deferred until the NMT phase needs it.)
    //
    // IMPORTANT: UE3's reliable ChSequence is effectively a connection-global
    // counter shared across channels - so the control channel's (chIndex 0) own
    // reliable bunches arrive with GAPS in their sequence numbers (e.g. 3,5,6,7,...
    // where 4 was a bunch on another channel, or was lost and not retransmitted in
    // this stream). A strictly-contiguous "deliver only m_nextSeq" loop would
    // deadlock forever on the first such gap, blocking EVERY control message after
    // it (observed live: login completed at seq 3, then seqs 5,6,7,11,12... piled
    // up unread because seq 4 never arrived). So we skip a gap once enough later
    // bunches have accumulated: the missing sequence is not coming.
    constexpr size_t kSkipGapThreshold = 4; // buffered bunches before we skip a gap
    for (;;) {
        auto it = m_pending.find(m_nextSeq);
        if (it == m_pending.end()) {
            // m_nextSeq is missing. If enough later bunches have piled up, the gap
            // won't fill - skip ahead to the lowest buffered sequence.
            if (m_pending.size() >= kSkipGapThreshold) {
                uint32_t lowest = m_pending.begin()->first;
                if (lowest > m_nextSeq) {
                    Logger::Debug("[ControlReassembler] skipping gap: m_nextSeq %u -> %u (%zu buffered)",
                                  m_nextSeq, lowest, m_pending.size());
                    m_nextSeq = lowest;
                    continue;
                }
            }
            break;
        }
        const Bunch& b = it->second;
        const size_t nbytes = BunchByteSize(b);  // size_t math; cannot overflow or over-read
        if (m_onMessage && b.payloadBits > 0 && nbytes > 0) {
            m_onMessage(std::vector<uint8_t>(b.payload.begin(), b.payload.begin() + nbytes));
        }
        // Keep the buffered-byte accounting in lockstep with the map.
        m_pendingBytes -= std::min(m_pendingBytes, BunchByteSize(b));
        m_pending.erase(it);
        ++m_nextSeq;
    }
}

} // namespace PacketCodec
