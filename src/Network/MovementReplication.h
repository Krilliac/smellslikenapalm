// Bounded decoder for the retail ROPlayerController movement RPC stream.
// A bunch may contain several calls; DecodeOne consumes exactly one evidenced
// schema so a class-aware outer dispatcher can safely continue at the next RPC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

#include "Math/Vector3.h"
#include "Network/ActorReplication.h"
#include "Network/BitReader.h"

namespace MovementRepl {

constexpr uint32_t kDualServerMove = 63;
constexpr uint32_t kOldServerMove = 64;
constexpr uint32_t kServerMove = 65;
constexpr uint32_t kMantleServerMove = 297;
constexpr uint32_t kRMServerMove = 298;
constexpr uint32_t kCoverServerMove = 299;
constexpr uint32_t kResetTeamSwapDelay = 496;
constexpr uint32_t kRoPcMaxHandle = 531;

enum class Kind : uint8_t {
    Dual,
    Old,
    Normal,
    Mantle,
    RootMotion,
    Cover,
    ResetTeamSwapDelay,
};

struct Rpc {
    Kind kind = Kind::Normal;
    uint32_t handle = 0;
    bool hasTimestamp = false;
    float timestamp = 0.0f;
    bool hasAcceleration = false;
    Vector3 acceleration{};
    bool hasClientLocation = false;
    Vector3 clientLocation{};
    bool hasMoveFlags = false;
    uint8_t moveFlags = 0;
    bool hasClientRoll = false;
    uint8_t clientRoll = 0;
    bool hasView = false;
    int32_t view = 0;
    bool isInCover = false;
    bool hasCoverInfo = false;
    int32_t coverInfo = 0;
    bool hasFreeAimRot = false;
    int32_t freeAimRot = 0;
    size_t consumedBits = 0;
};

struct DecodeResult {
    bool valid = false;
    bool hasClientLocation = false;
    bool hasMoveFlags = false;
    uint8_t latestMoveFlags = 0;
    Kind latestKind = Kind::Normal;
    uint32_t latestHandle = 0;
    uint32_t rpcCount = 0;
    size_t consumedBits = 0;
    Vector3 latestClientLocation{};
};

namespace detail {

inline bool ReadOptionalFloat(BitReader& r, float& value) {
    const bool present = r.ReadBit();
    value = present ? r.ReadFloat() : 0.0f;
    return present;
}

inline bool ReadOptionalByte(BitReader& r, uint8_t& value) {
    const bool present = r.ReadBit();
    value = present ? r.ReadByte() : 0;
    return present;
}

inline bool ReadOptionalInt32(BitReader& r, int32_t& value) {
    const bool present = r.ReadBit();
    value = present ? r.ReadInt32() : 0;
    return present;
}

inline bool ReadOptionalVector(BitReader& r, Vector3& value) {
    const bool present = r.ReadBit();
    value = {};
    if (present) {
        ActorRepl::ReadCompressedVector(r, value.x, value.y, value.z);
    }
    return present;
}

} // namespace detail

inline bool IsKnownHandle(uint32_t handle) {
    switch (handle) {
        case kDualServerMove:
        case kOldServerMove:
        case kServerMove:
        case kMantleServerMove:
        case kRMServerMove:
        case kCoverServerMove:
        case kResetTeamSwapDelay:
            return true;
        default:
            return false;
    }
}

// Transactional: on unknown or malformed input, `r` is not advanced.
inline bool DecodeOne(BitReader& r, Rpc& out) {
    BitReader trial = r;
    Rpc decoded;
    const size_t start = trial.BitPos();
    decoded.handle = trial.SerializeInt(kRoPcMaxHandle);
    if (trial.IsOverflowed() || !IsKnownHandle(decoded.handle)) return false;

    switch (decoded.handle) {
        case kDualServerMove: {
            decoded.kind = Kind::Dual;
            float oldTimestamp = 0.0f;
            Vector3 oldAcceleration{};
            uint8_t oldFlags = 0;
            int32_t oldView = 0;
            int32_t oldFreeAim = 0;
            decoded.hasTimestamp = detail::ReadOptionalFloat(trial, oldTimestamp);
            decoded.hasAcceleration = detail::ReadOptionalVector(trial, oldAcceleration);
            (void)detail::ReadOptionalByte(trial, oldFlags);
            (void)detail::ReadOptionalInt32(trial, oldView);
            decoded.hasTimestamp = detail::ReadOptionalFloat(trial, decoded.timestamp);
            decoded.hasAcceleration = detail::ReadOptionalVector(trial, decoded.acceleration);
            decoded.hasClientLocation = detail::ReadOptionalVector(trial, decoded.clientLocation);
            decoded.hasMoveFlags = detail::ReadOptionalByte(trial, decoded.moveFlags);
            decoded.hasClientRoll = detail::ReadOptionalByte(trial, decoded.clientRoll);
            decoded.hasView = detail::ReadOptionalInt32(trial, decoded.view);
            (void)detail::ReadOptionalInt32(trial, oldFreeAim);
            decoded.hasFreeAimRot = detail::ReadOptionalInt32(trial, decoded.freeAimRot);
            break;
        }
        case kOldServerMove: {
            decoded.kind = Kind::Old;
            uint8_t accelX = 0, accelY = 0, accelZ = 0;
            decoded.hasTimestamp = detail::ReadOptionalFloat(trial, decoded.timestamp);
            (void)detail::ReadOptionalByte(trial, accelX);
            (void)detail::ReadOptionalByte(trial, accelY);
            (void)detail::ReadOptionalByte(trial, accelZ);
            decoded.hasMoveFlags = detail::ReadOptionalByte(trial, decoded.moveFlags);
            break;
        }
        case kServerMove:
            decoded.kind = Kind::Normal;
            decoded.hasTimestamp = detail::ReadOptionalFloat(trial, decoded.timestamp);
            decoded.hasAcceleration = detail::ReadOptionalVector(trial, decoded.acceleration);
            decoded.hasClientLocation = detail::ReadOptionalVector(trial, decoded.clientLocation);
            decoded.hasMoveFlags = detail::ReadOptionalByte(trial, decoded.moveFlags);
            decoded.hasClientRoll = detail::ReadOptionalByte(trial, decoded.clientRoll);
            decoded.hasView = detail::ReadOptionalInt32(trial, decoded.view);
            decoded.hasFreeAimRot = detail::ReadOptionalInt32(trial, decoded.freeAimRot);
            break;
        case kMantleServerMove:
        case kRMServerMove:
        case kCoverServerMove:
            decoded.kind = decoded.handle == kMantleServerMove ? Kind::Mantle :
                           decoded.handle == kRMServerMove ? Kind::RootMotion : Kind::Cover;
            decoded.hasTimestamp = detail::ReadOptionalFloat(trial, decoded.timestamp);
            decoded.hasAcceleration = detail::ReadOptionalVector(trial, decoded.acceleration);
            decoded.hasClientLocation = detail::ReadOptionalVector(trial, decoded.clientLocation);
            decoded.hasMoveFlags = detail::ReadOptionalByte(trial, decoded.moveFlags);
            decoded.hasClientRoll = detail::ReadOptionalByte(trial, decoded.clientRoll);
            decoded.hasView = detail::ReadOptionalInt32(trial, decoded.view);
            if (decoded.handle == kCoverServerMove) {
                decoded.hasCoverInfo = detail::ReadOptionalInt32(trial, decoded.coverInfo);
            } else {
                decoded.isInCover = trial.ReadBit(); // bool parameters are bare bits
            }
            if (decoded.handle != kMantleServerMove) {
                decoded.hasFreeAimRot = detail::ReadOptionalInt32(trial, decoded.freeAimRot);
            }
            break;
        case kResetTeamSwapDelay:
            decoded.kind = Kind::ResetTeamSwapDelay;
            break;
        default:
            return false;
    }

    if (trial.IsOverflowed() ||
        (decoded.hasTimestamp && !std::isfinite(decoded.timestamp))) {
        return false;
    }
    decoded.consumedBits = trial.BitPos() - start;
    r = trial;
    out = decoded;
    return true;
}

inline DecodeResult DecodeRoPlayerControllerMoves(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    DecodeResult result;
    if (!payload || payloadBits == 0 ||
        payloadBytes > std::numeric_limits<size_t>::max() / 8u ||
        payloadBits > payloadBytes * 8u) return result;

    BitReader r(payload, payloadBytes, payloadBits);
    while (r.BitsLeft() > 0 && !r.IsOverflowed()) {
        Rpc rpc;
        if (!DecodeOne(r, rpc)) return DecodeResult{};
        if (rpc.kind == Kind::ResetTeamSwapDelay) continue;
        ++result.rpcCount;
        result.latestKind = rpc.kind;
        result.latestHandle = rpc.handle;
        if (rpc.hasClientLocation) {
            result.hasClientLocation = true;
            result.latestClientLocation = rpc.clientLocation;
        }
        if (rpc.hasMoveFlags) {
            result.hasMoveFlags = true;
            result.latestMoveFlags = rpc.moveFlags;
        }
    }
    result.consumedBits = r.BitPos();
    result.valid = !r.IsOverflowed() && r.BitsLeft() == 0 && result.rpcCount > 0;
    return result;
}

} // namespace MovementRepl
