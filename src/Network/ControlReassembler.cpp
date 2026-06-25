// src/Network/ControlReassembler.cpp
// See ControlReassembler.h.

#include "Network/ControlReassembler.h"

#include "Network/NetMessages.h"

#include <algorithm>

namespace PacketCodec {

ControlReassembler::ControlReassembler(MessageFn onMessage)
    : m_onMessage(std::move(onMessage)) {}

void ControlReassembler::OnBunch(const Bunch& bunch) {
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
    auto it = m_pending.find(m_nextSeq);
    while (it != m_pending.end()) {
        const Bunch& b = it->second;
        if (m_onMessage && b.payloadBits > 0) {
            const size_t nbytes = std::min(static_cast<size_t>((b.payloadBits + 7) / 8), b.payload.size());
            m_onMessage(std::vector<uint8_t>(b.payload.begin(), b.payload.begin() + nbytes));
        }
        m_pending.erase(it);
        ++m_nextSeq;
        it = m_pending.find(m_nextSeq);
    }
}

} // namespace PacketCodec
