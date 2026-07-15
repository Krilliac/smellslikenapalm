// Retail ROTeamInfo spawn-selection replication.
//
// AvailableSpawnLocations is h59, a ten-element static object array. Each
// element is [SerializeInt(59,78)][uint8 slot][SerializeObject]. Resort's
// entries are static map actors already loaded by the client; an empty element
// is UE3 None (dynamic selector + channel zero), never static object zero.

#pragma once

#include "Network/ActorReplication.h"
#include "Network/BitWriter.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace SpawnRepl {

constexpr uint32_t kTeamInfoMaxHandle = 78;
constexpr uint32_t kAvailableSpawnLocations = 59;
constexpr uint32_t kReinforcementsRemaining = 62;
constexpr uint8_t kAvailableSpawnLocationCount = 10;
// Retail closes ROUISceneSpawnSelect immediately when the selected TeamInfo
// reports zero reinforcements. The emulator's initial==0 convention means an
// unlimited pool, so publish a positive display sentinel rather than the
// client's destructive zero default.
constexpr int32_t kUnlimitedReinforcementsDisplay = 9999;

inline int32_t ResolveWireReinforcementCount(
    uint32_t current, uint32_t initial) noexcept {
    if (initial == 0) return kUnlimitedReinforcementsDisplay;
    constexpr uint32_t kMaxWire =
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    return static_cast<int32_t>(current > kMaxWire ? kMaxWire : current);
}

inline void WriteReinforcementsRemaining(BitWriter& writer, int32_t count) {
    ActorRepl::WritePropInt(writer, kReinforcementsRemaining,
                            kTeamInfoMaxHandle, count);
}

// Map fixtures store canonical PackageMap refs. Rebase a real map actor for
// the connection's frozen artifact layout while preserving UE3 None. Use a
// widened sum so malformed offsets cannot wrap into a plausible static ref.
inline std::optional<uint32_t> RebaseStaticObjectRef(
    uint32_t canonicalRef, uint32_t artifactOffset) noexcept {
    if (canonicalRef == 0) return 0u;
    const uint64_t rebased = static_cast<uint64_t>(canonicalRef) +
                             static_cast<uint64_t>(artifactOffset);
    if (rebased >= ActorRepl::kStaticObjectMax) return std::nullopt;
    return static_cast<uint32_t>(rebased);
}

// staticObjectRef == 0 clears the array element with the canonical None object
// reference. Invalid slots/refs fail before modifying the writer.
inline bool WriteAvailableSpawnLocation(BitWriter& writer, uint8_t slot,
                                        uint32_t staticObjectRef) {
    if (slot >= kAvailableSpawnLocationCount ||
        staticObjectRef >= ActorRepl::kStaticObjectMax) {
        return false;
    }

    writer.SerializeInt(kAvailableSpawnLocations, kTeamInfoMaxHandle);
    writer.WriteByte(slot); // UE3 static-array indices are serialized as uint8.
    ActorRepl::WriteNetGUID(
        writer,
        staticObjectRef == 0
            ? ActorRepl::NetGUIDRef{/*isDynamic=*/true, 0}
            : ActorRepl::NetGUIDRef{/*isDynamic=*/false, staticObjectRef});
    return true;
}

} // namespace SpawnRepl
