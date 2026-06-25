// src/Network/ControlReassembler.h
//
// Per-connection INBOUND control-channel reassembler - the receive-side mirror of
// PacketAssembler. UE3 fragments a control-channel message across many small
// reliable bunches (<=63 data bits each during the handshake; see
// docs/RS2V_ControlChannel_WireSpec_7258.md §3) which arrive in ChSequence order
// (possibly out of order / duplicated across retransmitted packets). This class
// buffers reliable control bunches, orders them by ChSequence, concatenates their
// payload bits, and peels off each complete <BYTE NMT><fields> message - which it
// hands to a callback (e.g. HandshakeState::HandleControlMessage).
//
// UE3 has no per-message length marker; message length is implicit in the NMT's
// fields, so ControlChannel::ConsumeMessage is used to delimit messages on the
// continuous bit stream.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "Network/PacketCodec.h"

namespace PacketCodec {

class ControlReassembler {
public:
    // Delivered a complete control-channel message PAYLOAD (a byte-aligned
    // <BYTE NMT><fields> buffer, exactly what ControlChannel::Parse* consumes).
    using MessageFn = std::function<void(const std::vector<uint8_t>& messagePayload)>;

    explicit ControlReassembler(MessageFn onMessage);

    // Feed one decoded bunch. Non-control bunches (chIndex != 0) and unreliable
    // bunches are ignored. Reliable control bunches are buffered by ChSequence
    // (ChSequence values already consumed are ignored as duplicates), delivered
    // in order; every complete message that becomes available is dispatched via
    // the callback (in order).
    void OnBunch(const Bunch& bunch);

    // Test/diagnostic accessors.
    uint32_t NextSequence() const { return m_nextSeq; }
    size_t PendingBunchCount() const { return m_pending.size(); }

private:
    void Drain();   // deliver in-order ready bunches (per-bunch) to the callback

    MessageFn m_onMessage;
    std::map<uint32_t, Bunch> m_pending;  // chSequence -> bunch awaiting in-order drain
    uint32_t m_nextSeq = 1;               // next ChSequence to consume (starts 1)
};

} // namespace PacketCodec
