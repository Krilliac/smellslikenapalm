// src/Network/ControlReassembler.cpp
// See ControlReassembler.h.

#include "Network/ControlReassembler.h"

#include "Network/NetMessages.h"
#include "Utils/Logger.h"

#include <algorithm>

namespace PacketCodec {

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
    if (bunch.chSequence > m_nextSeq + kMaxSeqAhead) {
        return;
    }
    if (m_pending.size() >= kMaxPending && m_pending.find(bunch.chSequence) == m_pending.end()) {
        return;
    }
    // dedup: keep the first copy of a given sequence (ignore later differing copies)
    m_pending.emplace(bunch.chSequence, bunch);
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
        if (m_onMessage && b.payloadBits > 0) {
            const size_t nbytes = std::min(static_cast<size_t>((b.payloadBits + 7) / 8), b.payload.size());
            m_onMessage(std::vector<uint8_t>(b.payload.begin(), b.payload.begin() + nbytes));
        }
        m_pending.erase(it);
        ++m_nextSeq;
    }
}

} // namespace PacketCodec
