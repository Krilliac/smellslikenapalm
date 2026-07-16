// src/Network/PeerControlClose.h
//
// Side-effect-free classification for UE3 channel-close bunches.  Keeping this
// policy separate from ConnectionManager makes the fail-closed rules directly
// testable without constructing sockets or a GameServer.

#pragma once

#include "Network/PacketCodec.h"

#include <cstdint>

namespace PacketCodec {

enum class PeerCloseClassification : uint8_t {
    None,
    GracefulControlClose,
    MalformedControlClose,
    OtherChannelClose,
};

// The control channel is connection-lifetime state.  Its close notify is the
// canonical empty reliable close bunch; accepting payload or contradictory
// flags would let malformed bytes reach the NMT reassembler during teardown.
inline PeerCloseClassification ClassifyPeerClose(const Bunch& bunch) noexcept {
    if (!bunch.bClose) {
        return PeerCloseClassification::None;
    }
    if (bunch.chIndex != static_cast<uint32_t>(kControlChannelIndex)) {
        return PeerCloseClassification::OtherChannelClose;
    }

    const bool canonical = bunch.bControl && !bunch.bOpen && bunch.bReliable &&
        bunch.chType == kControlChannelType && bunch.payloadBits == 0u &&
        bunch.payload.empty();
    return canonical ? PeerCloseClassification::GracefulControlClose
                     : PeerCloseClassification::MalformedControlClose;
}

// Malformed ch0 closes dominate a mixed packet: none of that datagram is fed to
// channel state.  Otherwise a valid connection close dominates actor closes and
// ordinary bunches because the peer has ended the whole UE3 connection.
inline PeerCloseClassification ClassifyPeerClose(const Packet& packet) noexcept {
    PeerCloseClassification result = PeerCloseClassification::None;
    for (const Bunch& bunch : packet.bunches) {
        const PeerCloseClassification current = ClassifyPeerClose(bunch);
        if (current == PeerCloseClassification::MalformedControlClose) {
            return current;
        }
        if (current == PeerCloseClassification::GracefulControlClose) {
            result = current;
        } else if (current == PeerCloseClassification::OtherChannelClose &&
                   result == PeerCloseClassification::None) {
            result = current;
        }
    }
    return result;
}

} // namespace PacketCodec
