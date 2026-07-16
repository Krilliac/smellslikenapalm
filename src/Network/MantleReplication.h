// Bounded RS2 mantle RPC codec.
//
// Retail asks the authority to validate traversal with unreliable
// ROPlayerController.ServerAttemptMantle(h280). A successful dynamic mantle is
// started with reliable ClientStartDynamicMantle(h344). DecodeServerAttempt is
// prefix-based because retail may append a movement RPC to the same bunch.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

#include "Math/Vector3.h"
#include "Network/ActorReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"

namespace MantleRepl {

constexpr uint32_t kRoPcMaxHandle = 531;
constexpr uint32_t kServerAttemptMantle = 280;
constexpr uint32_t kClientStartDynamicMantle = 344;
constexpr uint32_t kSpecialMoveMax = 24; // ESpecialMove values 0..SM_MAX(23)

struct MantleTestInfo {
    bool valid = false;
    Vector3 hitLocation{};
    Vector3 hitNormal{};
    Vector3 playerLocation{};
};

struct Attempt {
    bool valid = false;
    bool wantsToClimb = false;
    bool hasLastGoodInfo = false;
    MantleTestInfo lastGood{};
    size_t consumedBits = 0;
};

struct DynamicInfo {
    bool canDynamicMantle = true;
    bool mantleCrouched = false;
    Vector3 startLocation{};
    Vector3 endLocation{};
    Vector3 normal{};
    float height = 0.0f;
};

// Transactional: on an unknown/malformed prefix, `r` is not advanced.
inline Attempt DecodeServerAttempt(BitReader& r) {
    BitReader trial = r;
    Attempt out;
    const size_t start = trial.BitPos();
    if (trial.SerializeInt(kRoPcMaxHandle) != kServerAttemptMantle ||
        trial.IsOverflowed()) return Attempt{};
    // Bool RPC parameters are serialized directly; non-bool struct parameters
    // carry UE3's one-bit presence marker.
    out.wantsToClimb = trial.ReadBit();
    out.hasLastGoodInfo = trial.ReadBit();
    if (out.hasLastGoodInfo) {
        out.lastGood.valid = trial.ReadBit();
        ActorRepl::ReadCompressedVector(
            trial, out.lastGood.hitLocation.x, out.lastGood.hitLocation.y,
            out.lastGood.hitLocation.z);
        ActorRepl::ReadCompressedVector(
            trial, out.lastGood.hitNormal.x, out.lastGood.hitNormal.y,
            out.lastGood.hitNormal.z);
        ActorRepl::ReadCompressedVector(
            trial, out.lastGood.playerLocation.x, out.lastGood.playerLocation.y,
            out.lastGood.playerLocation.z);
    }
    if (trial.IsOverflowed()) return Attempt{};
    out.consumedBits = trial.BitPos() - start;
    out.valid = true;
    r = trial;
    return out;
}

inline Attempt DecodeServerAttempt(const uint8_t* payload, size_t payloadBytes,
                                   size_t payloadBits) {
    if (!payload || payloadBits == 0 ||
        payloadBytes > std::numeric_limits<size_t>::max() / 8u ||
        payloadBits > payloadBytes * 8u) {
        return Attempt{};
    }
    BitReader r(payload, payloadBytes, payloadBits);
    return DecodeServerAttempt(r);
}

inline std::vector<uint8_t> EncodeClientStartDynamicMantle(
    uint8_t specialMove, const DynamicInfo& info, uint32_t& outBits) {
    outBits = 0;
    const auto finiteVector = [](const Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    constexpr float kMaximumCompressedComponent = 1048575.0f;
    const auto encodableVector = [&](const Vector3& value) {
        return finiteVector(value) &&
               std::abs(value.x) <= kMaximumCompressedComponent &&
               std::abs(value.y) <= kMaximumCompressedComponent &&
               std::abs(value.z) <= kMaximumCompressedComponent;
    };
    // SM_None and SM_MAX are not valid starts. Protect the compressed-vector
    // encoder from non-finite/out-of-range server state before any float-to-int
    // conversion occurs.
    if (specialMove == 0 || specialMove >= kSpecialMoveMax - 1u ||
        !encodableVector(info.startLocation) ||
        !encodableVector(info.endLocation) ||
        !encodableVector(info.normal) || !std::isfinite(info.height) ||
        info.height <= 0.0f || info.height > 1000.0f) {
        return {};
    }
    BitWriter w;
    w.SerializeInt(kClientStartDynamicMantle, kRoPcMaxHandle);
    w.WriteBit(true); // MantleMove present
    w.SerializeInt(specialMove, kSpecialMoveMax);
    w.WriteBit(true); // DynamicMantleInfo present
    w.WriteBit(info.canDynamicMantle);
    w.WriteBit(info.mantleCrouched);
    ActorRepl::WriteCompressedVector(
        w, info.startLocation.x, info.startLocation.y, info.startLocation.z);
    ActorRepl::WriteCompressedVector(
        w, info.endLocation.x, info.endLocation.y, info.endLocation.z);
    ActorRepl::WriteCompressedVector(w, info.normal.x, info.normal.y, info.normal.z);
    w.WriteFloat(info.height);
    outBits = static_cast<uint32_t>(w.NumBits());
    return w.GetBytes();
}

} // namespace MantleRepl
