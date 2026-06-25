// src/Network/PacketCodec.cpp
//
// Implementation of the UE3 packet/bunch framing codec. See PacketCodec.h and
// docs/RS2V_ControlChannel_WireSpec_7258.md.

#include "Network/PacketCodec.h"

namespace PacketCodec {

Packet Decode(const uint8_t* /*data*/, size_t /*numBytes*/) {
    // Not yet implemented.
    return Packet{};
}

} // namespace PacketCodec
