// src/Network/ControlReassembler.h
//
// Per-connection INBOUND control-channel bunch sequencer - the receive-side
// ordering mirror of PacketAssembler. Reliable ch0 bunches use their own
// per-channel modulo-1024 ChSequence cursor and can arrive out of order or as
// duplicate retransmits. This class applies UE3's strict per-channel ordering,
// then dispatches each accepted bunch payload to the handshake/NMT callback.
//
// The current retail input path treats one received control bunch as one callback
// payload. It does not concatenate a logical message across bunches; callers that
// need the general continuous UControlChannel stream must layer message delimiting
// above this sequencer.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "Network/PacketCodec.h"

namespace PacketCodec {

class ControlReassembler {
public:
    // UE3 RELIABLE_BUFFER is 128, leaving 127 ordinary forward successors that
    // can be outstanding behind one missing reliable bunch.
    static constexpr uint32_t kMaximumForwardDistance = kReliableBuffer - 1u;
    static constexpr size_t kMaximumPendingBunches = kReliableBuffer;
    static constexpr size_t kMaximumPendingPayloadBytes = 256u * 1024u;

    // Delivered a complete control-channel message PAYLOAD (a byte-aligned
    // <BYTE NMT><fields> buffer, exactly what ControlChannel::Parse* consumes).
    using MessageFn = std::function<void(const std::vector<uint8_t>& messagePayload)>;

    explicit ControlReassembler(MessageFn onMessage);

    // Feed one decoded bunch. Non-control bunches (chIndex != 0) and unreliable
    // bunches are ignored. Reliable control bunches are buffered by ChSequence
    // (ChSequence values already consumed are ignored as duplicates), delivered
    // in strict per-channel order; every bunch payload that becomes available is
    // dispatched via the callback exactly once and in order. Callback exceptions
    // propagate; the current bunch remains pending and is retried by a later
    // accepted/retransmitted input.
    void OnBunch(const Bunch& bunch);

    // Test/diagnostic accessors.
    uint32_t NextSequence() const { return m_nextSeq; }
    size_t PendingBunchCount() const { return m_pending.size(); }

private:
    void Drain();  // commit each in-order bunch after its guarded callback succeeds

    MessageFn m_onMessage;
    std::map<uint32_t, Bunch> m_pending;  // chSequence -> bunch awaiting in-order drain
    uint32_t m_nextSeq = 1;               // next modulo-1024 ChSequence to consume
    size_t m_pendingBytes = 0;            // total payload bytes currently buffered (DoS cap)
    bool m_dispatching = false;           // callback re-entry buffers; outer drain resumes
};

} // namespace PacketCodec
