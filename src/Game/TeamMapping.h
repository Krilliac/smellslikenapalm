// Shared mapping between retail RO/UE3 team indices and the emulator's
// TeamManager ids. RS2 inherits the Axis/Allies ordering used by ROObjective:
//   retail 0 = Axis/North Vietnam, retail 1 = Allies/United States.
// The emulator historically chose the opposite numeric ids internally:
//   server 1 = US Army, server 2 = NVA.
// Keep every boundary conversion here; a naive +1 silently swaps factions.

#pragma once

#include <cstdint>

namespace TeamMapping {

constexpr uint8_t kRetailNva = 0;
constexpr uint8_t kRetailUs = 1;
constexpr uint8_t kRetailNeutral = 2;

constexpr uint32_t kServerUs = 1;
constexpr uint32_t kServerNva = 2;

constexpr bool IsPlayableServerTeam(uint32_t serverTeamId) noexcept {
    return serverTeamId == kServerUs || serverTeamId == kServerNva;
}

// Cooked Territory BaseSpawn rows describe roles, not permanent faction
// ownership: authored server team 1 is the attacker lane and authored server
// team 2 is the defender lane. Resolve that role against the current round.
// Zero is a fail-closed result for malformed or aliased roles.
constexpr uint32_t ResolveTerritoryRoleTeam(
    uint32_t authoredTeamId, uint32_t attackingTeamId,
    uint32_t defendingTeamId) noexcept {
    if (!IsPlayableServerTeam(authoredTeamId) ||
        !IsPlayableServerTeam(attackingTeamId) ||
        !IsPlayableServerTeam(defendingTeamId) ||
        attackingTeamId == defendingTeamId) {
        return 0;
    }
    return authoredTeamId == kServerUs ? attackingTeamId : defendingTeamId;
}

inline uint32_t RetailToServer(uint8_t retailTeamIndex) {
    return retailTeamIndex == kRetailUs ? kServerUs : kServerNva;
}

inline uint8_t ServerToRetail(uint32_t serverTeamId) {
    if (serverTeamId == kServerUs) return kRetailUs;
    if (serverTeamId == kServerNva) return kRetailNva;
    return kRetailNeutral;
}

} // namespace TeamMapping
