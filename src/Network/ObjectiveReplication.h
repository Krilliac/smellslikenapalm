// Retail ROGameReplicationInfo objective-array replication helpers.
//
// The client already owns the cooked ROObjective actors from the loaded map.
// ObjectiveRepIndices[slot] maps those actors into the HUD, while handles
// 174..178 carry topology/progress/status. Those byte-array elements are exactly
// [SerializeInt(handle,184)][slot byte][value byte] = 24 bits; ObjCappers h124
// is instead a 47-bit four-byte struct element.

#pragma once

#include "Game/TeamMapping.h"
#include "Network/BitWriter.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ObjectiveRepl {

constexpr uint32_t kGriMaxHandle = 184;
constexpr uint32_t kTimeLimit = 25;
constexpr uint32_t kRemainingMinute = 27;
constexpr uint32_t kElapsedTime = 28;
constexpr uint32_t kRemainingTime = 29;
constexpr uint32_t kMatchIsOver = 30;
constexpr uint32_t kMatchHasBegun = 31;
constexpr uint32_t kStopCountDown = 32;
constexpr uint32_t kAllSpawnWindows = 37;
constexpr uint32_t kSpawnWindowCloseTime = 38;
constexpr uint32_t kPlayedRoundsCount = 39;
constexpr uint32_t kRoundTeamScoreLimit = 40;
constexpr uint32_t kRoundLimit = 41;
constexpr uint32_t kSuPointsHeld = 46;
constexpr uint32_t kSuCurrentScore = 47;
constexpr uint32_t kSuTargetScore = 48;
constexpr uint32_t kNextLockDownTime = 67;
constexpr uint32_t kDisableObjectiveOverview = 100;
constexpr uint32_t kBalanceTeams = 109;
constexpr uint32_t kAlliesAreAttacking = 114;
constexpr uint32_t kSuddenDeath = 116;
constexpr uint32_t kOverTime = 117;
constexpr uint32_t kObjCappers = 124;
constexpr uint32_t kOvertimeAdvantage = 126;
constexpr uint32_t kPlayersAliveCount = 129;
constexpr uint32_t kDefendingTeam = 143;
constexpr uint32_t kMaxTeamDifference = 181;
constexpr uint32_t kMaxPlayers = 182;
constexpr uint32_t kConnectedToBase = 174;
constexpr uint32_t kSatchelProgress = 175;
constexpr uint32_t kCapProgress = 176;
constexpr uint32_t kForceRatio = 177;
constexpr uint32_t kStatus = 178;
constexpr uint32_t kRepIndices = 179;

constexpr uint32_t kPcMaxHandle = 531;
constexpr uint32_t kPcObjectiveName = 323;
constexpr uint32_t kPcObjectiveIndex = 331;

struct ArrayElement {
    uint32_t handle = 0;
    uint8_t slot = 0;
    uint8_t value = 0;
};

inline void WriteArrayElement(BitWriter& w, uint32_t handle,
                              uint8_t slot, uint8_t value) {
    w.SerializeInt(handle, kGriMaxHandle);
    w.WriteByte(slot);
    w.WriteByte(value);
}

// UE3 static int arrays serialize the field handle, an 8-bit static-array
// index, then the normal 32-bit UIntProperty payload. This is distinct from
// the 24-bit byte-array triple used by the objective arrays above.
inline void WriteIntArrayElement(BitWriter& w, uint32_t handle,
                                 uint8_t slot, int32_t value) {
    w.SerializeInt(handle, kGriMaxHandle);
    w.WriteByte(slot);
    w.WriteInt32(value);
}

// ObjCappers is a 16-element array of four-byte structs, not a byte-array
// property. Capture wire: h124, outer slot, then the two team counts and two
// unused bytes. SerializeInt(124,184) makes each element exactly 47 bits.
inline void WriteCappersElement(BitWriter& w, uint8_t slot,
                                uint8_t team0, uint8_t team1) {
    w.SerializeInt(kObjCappers, kGriMaxHandle);
    w.WriteByte(slot);
    w.WriteByte(team0);
    w.WriteByte(team1);
    w.WriteByte(0);
    w.WriteByte(0);
}

inline void WriteObjectiveIndex(BitWriter& w, uint8_t slot) {
    w.SerializeInt(kPcObjectiveIndex, kPcMaxHandle);
    w.WriteByte(slot);
}

inline void WriteObjectiveName(BitWriter& w, const std::string& name) {
    w.SerializeInt(kPcObjectiveName, kPcMaxHandle);
    w.WriteString(name);
}

inline std::vector<uint8_t> EncodeArrayElements(
    const std::vector<ArrayElement>& elements, uint32_t& outBits) {
    BitWriter w;
    for (const ArrayElement& e : elements) {
        WriteArrayElement(w, e.handle, e.slot, e.value);
    }
    outBits = static_cast<uint32_t>(w.NumBits());
    return w.GetBytes();
}

inline uint8_t RetailOwner(uint32_t serverTeam) {
    // Server: 0 neutral, 1/2 playable. Retail status: 0 north/axis,
    // 1 south/allies, 2 neutral.
    return TeamMapping::ServerToRetail(serverTeam);
}

inline uint8_t QuantizeProgress(float progress) {
    return static_cast<uint8_t>(std::clamp(progress, 0.0f, 1.0f) * 255.0f);
}

inline uint8_t QuantizeForceRatio(uint32_t team0Cappers,
                                  uint32_t team1Cappers) {
    const uint32_t total = team0Cappers + team1Cappers;
    if (total == 0) return 0;
    return static_cast<uint8_t>(
        (static_cast<float>(team0Cappers) / static_cast<float>(total)) * 254.0f);
}

inline uint8_t PackStatus(uint8_t owner, bool capping, bool active,
                          uint8_t cappingTeam, bool enabled, bool satchel) {
    uint8_t status = owner & 0x03u;
    if (capping) status |= 0x80u;
    if (active) status |= 0x40u;
    if (cappingTeam == 1) status |= 0x20u;
    if (enabled) status |= 0x10u;
    if (satchel) status |= 0x08u;
    return status;
}

} // namespace ObjectiveRepl
