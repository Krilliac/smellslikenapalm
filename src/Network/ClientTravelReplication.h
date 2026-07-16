// Pure encoder for PlayerController.ClientTravel on the retail UE3 actor channel.
//
// Source anchors (UE3 2013 source mirrored under D:/RE-Tools/UE3-src):
//   Engine/Classes/PlayerController.uc  ClientTravel(URL, TravelType,
//                                      bSeamless, MapPackageGuid)
//   Engine/Src/UnScript.cpp:2907-3010  RPC handle + parameter compression
//   Core/Src/UnProp.cpp                enum byte / bool / struct NetSerialize
//
// Framing, reliable sequence assignment and retransmission are deliberately not
// part of this module. ConnectionManager owns those session-specific concerns.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ClientTravelRepl {

using GuidBytes = std::array<uint8_t, 16>;

enum class TravelType : uint8_t {
    Absolute = 0,
    Partial = 1,
    Relative = 2
};

struct Request {
    std::string url;
    TravelType travelType = TravelType::Relative;
    bool seamless = false;
    // Raw FGuid storage: A, B, C and D as four little-endian uint32 values.
    // All zero is UE3's invalid/default Guid and is omitted by RPC compression.
    GuidBytes mapPackageGuid{};
};

struct EncodedRpc {
    std::vector<uint8_t> payload;
    uint32_t payloadBits = 0;
};

constexpr uint32_t kClientTravelHandle = 29;
constexpr uint32_t kRoPlayerControllerMaxHandle = 531;

// A deliberately stricter bound than UE3's 2048-byte inbound FString guard.
// It keeps this single actor RPC comfortably inside the emulator's sane bunch
// and packet budgets while covering every package URL used by map rotation.
constexpr size_t kMaxTravelUrlBytes = 1023;

bool IsZeroGuid(const GuidBytes& guid);

// Transactional: output is replaced only on success; error describes failures
// and is cleared on success. Invalid enum sentinels, non-ANSI/control URL bytes
// and oversized payloads fail closed.
bool Encode(const Request& request, EncodedRpc& output, std::string& error);

// Defensive seam validation for callers that retain or forward an EncodedRpc.
bool IsValid(const EncodedRpc& rpc);

} // namespace ClientTravelRepl
