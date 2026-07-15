// src/Game/RoleSystem.cpp
// RS2V role/class system implementation

#include "Game/RoleSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cctype>

RoleSystem::RoleSystem(GameServer* server)
    : m_server(server)
{
}

RoleSystem::~RoleSystem() {
    Shutdown();
}

void RoleSystem::Initialize() {
    InitializeRoleDefinitions();

    // Default faction assignments
    m_teamFactions[1] = Faction::USArmy;
    m_teamFactions[2] = Faction::NVA;

    // Create default squads for each team
    for (uint32_t teamId = 1; teamId <= 2; ++teamId) {
        for (int i = 0; i < 8; ++i) {
            CreateSquad(teamId);
        }
    }

    Logger::Info("RoleSystem initialized with %zu role definitions", m_roleDefinitions.size());
}

void RoleSystem::Shutdown() {
    m_playerRoles.clear();
    m_squads.clear();
    m_playerSquadMap.clear();
    ResetRetailSquads();
    m_teamCommanders.clear();
    m_teamFactions.clear();
}

// --- Faction Management ---

void RoleSystem::SetTeamFaction(uint32_t teamId, Faction faction) {
    m_teamFactions[teamId] = faction;
    Logger::Info("Team %u assigned to faction %s", teamId, GetFactionName(faction).c_str());
}

Faction RoleSystem::GetTeamFaction(uint32_t teamId) const {
    auto it = m_teamFactions.find(teamId);
    return it != m_teamFactions.end() ? it->second : Faction::None;
}

std::string RoleSystem::GetFactionName(Faction f) const {
    switch (f) {
        case Faction::USArmy:  return "United States Army";
        case Faction::USMC:    return "United States Marine Corps";
        case Faction::AusArmy: return "Royal Australian Regiment";
        case Faction::NVA:     return "People's Army of Vietnam";
        case Faction::NLFSV:   return "National Liberation Front";
        default:               return "Unknown";
    }
}

std::string RoleSystem::GetFactionShortName(Faction f) const {
    switch (f) {
        case Faction::USArmy:  return "US";
        case Faction::USMC:    return "USMC";
        case Faction::AusArmy: return "AUS";
        case Faction::NVA:     return "NVA";
        case Faction::NLFSV:   return "VC";
        default:               return "???";
    }
}

// --- Role Management ---

bool RoleSystem::AssignRole(uint32_t playerId, CombatRole role) {
    auto* tm = m_server->GetTeamManager();
    uint32_t teamId = tm->GetPlayerTeam(playerId);
    if (teamId == 0) return false;

    // A player may ALWAYS re-select the role they already hold, even when that role is at its
    // team cap - they already occupy the slot (re-confirming loadout / re-deploying as the same
    // limited role). GetRoleCount counts the requester themselves, so without this exemption the
    // lone Sniper (cap 1) is rejected when re-selecting Sniper. Mirrors the source's
    // "ClassIndex == Roles[I].RoleInfoClass.ClassIndex" clause in
    // ROPlayerReplicationInfo.SelectRoleByClass.
    const bool alreadyMine = (m_playerRoles.count(playerId) > 0) && (GetPlayerRole(playerId) == role);
    if (!alreadyMine && !IsRoleAvailable(teamId, role)) {
        Logger::Warn("Role %s not available for player %u on team %u",
                     GetRoleName(role).c_str(), playerId, teamId);
        return false;
    }

    // Special handling for commander
    if (role == CombatRole::Commander) {
        return VolunteerAsCommander(playerId);
    }

    m_playerRoles[playerId] = role;
    Logger::Info("Player %u assigned role: %s", playerId, GetRoleName(role).c_str());
    return true;
}

void RoleSystem::RemovePlayer(uint32_t playerId) {
    ReleaseRetailSquadAssignment(playerId);

    // Do not rely solely on the reverse maps here. Disconnect cleanup is also
    // the recovery boundary for modest state drift, so remove every commander
    // entry and squad reference that still names this player.
    for (auto it = m_teamCommanders.begin(); it != m_teamCommanders.end();) {
        if (it->second == playerId) {
            it = m_teamCommanders.erase(it);
        } else {
            ++it;
        }
    }

    uint32_t mappedSquadId = 0;
    const auto mappedSquad = m_playerSquadMap.find(playerId);
    if (mappedSquad != m_playerSquadMap.end()) {
        mappedSquadId = mappedSquad->second;
        m_playerSquadMap.erase(mappedSquad);
    }

    for (auto squadIt = m_squads.begin(); squadIt != m_squads.end();) {
        Squad& squad = squadIt->second;
        const uint32_t squadId = squadIt->first;
        auto& members = squad.memberIds;
        const auto newEnd = std::remove(members.begin(), members.end(), playerId);
        const bool removedMembership = newEnd != members.end();
        members.erase(newEnd, members.end());

        const bool removedLeader =
            playerId != 0 && squad.leaderId == playerId;
        const bool affected = removedMembership || removedLeader ||
                              (mappedSquadId != 0 &&
                               mappedSquadId == squadId);
        if (!affected) {
            ++squadIt;
            continue;
        }

        if (members.empty()) {
            for (auto mapIt = m_playerSquadMap.begin();
                 mapIt != m_playerSquadMap.end();) {
                if (mapIt->second == squadId) {
                    mapIt = m_playerSquadMap.erase(mapIt);
                } else {
                    ++mapIt;
                }
            }
            squadIt = m_squads.erase(squadIt);
            continue;
        }

        const bool leaderIsMember =
            squad.leaderId != 0 &&
            std::find(members.begin(), members.end(), squad.leaderId) !=
                members.end();
        if (!leaderIsMember) {
            // memberIds preserves join order, making promotion deterministic.
            squad.leaderId = members.front();
            Logger::Info("Player %u promoted to Squad Leader of '%s'",
                         squad.leaderId, squad.name.c_str());
        }
        m_playerRoles[squad.leaderId] = CombatRole::SquadLeader;
        ++squadIt;
    }

    // Erase last so even corrupted duplicate squad membership can never
    // reintroduce the departing player's role during leader repair.
    m_playerRoles.erase(playerId);
}

CombatRole RoleSystem::GetPlayerRole(uint32_t playerId) const {
    auto it = m_playerRoles.find(playerId);
    return it != m_playerRoles.end() ? it->second : CombatRole::Rifleman;
}

std::string RoleSystem::GetRoleName(CombatRole role) const {
    switch (role) {
        case CombatRole::Commander:         return "Commander";
        case CombatRole::SquadLeader:       return "Squad Leader";
        case CombatRole::Rifleman:          return "Rifleman";
        case CombatRole::AutomaticRifleman: return "Automatic Rifleman";
        case CombatRole::MachineGunner:     return "Machine Gunner";
        case CombatRole::Grenadier:         return "Grenadier";
        case CombatRole::Marksman:          return "Marksman";
        case CombatRole::Sniper:            return "Sniper";
        case CombatRole::Pointman:          return "Pointman";
        case CombatRole::Sapper:            return "Sapper";
        case CombatRole::RPGGunner:         return "RPG Gunner";
        case CombatRole::CombatEngineer:    return "Combat Engineer";
        case CombatRole::Radioman:          return "Radioman";
        case CombatRole::HelicopterPilot:   return "Helicopter Pilot";
        case CombatRole::HelicopterGunner:  return "Helicopter Door Gunner";
        default:                            return "Unknown";
    }
}

bool RoleSystem::IsRoleAvailable(uint32_t teamId, CombatRole role) const {
    Faction faction = GetTeamFaction(teamId);
    const auto* def = FindRoleDef(role, faction);
    if (!def) return false;
    if (def->maxPerTeam < 0) return true;  // unlimited
    return GetRoleCount(teamId, role) < def->maxPerTeam;
}

int RoleSystem::GetRoleCount(uint32_t teamId, CombatRole role) const {
    auto* tm = m_server->GetTeamManager();
    auto players = tm->GetTeamPlayers(teamId);
    int count = 0;
    for (uint32_t pid : players) {
        auto it = m_playerRoles.find(pid);
        if (it != m_playerRoles.end() && it->second == role) count++;
    }
    return count;
}

int RoleSystem::GetRoleLimit(uint32_t teamId, CombatRole role) const {
    Faction faction = GetTeamFaction(teamId);
    const auto* def = FindRoleDef(role, faction);
    return def ? def->maxPerTeam : 0;
}

std::vector<CombatRole> RoleSystem::GetAvailableRoles(uint32_t teamId, Faction faction) const {
    std::vector<CombatRole> roles;
    for (const auto& def : m_roleDefinitions) {
        if (def.faction == faction && IsRoleAvailable(teamId, def.role)) {
            roles.push_back(def.role);
        }
    }
    return roles;
}

RoleLoadout RoleSystem::GetRoleLoadout(CombatRole role, Faction faction) const {
    const auto* def = FindRoleDef(role, faction);
    return def ? def->loadout : RoleLoadout{};
}

// --- Squad Management ---

uint32_t RoleSystem::CreateSquad(uint32_t teamId, const std::string& name) {
    Squad squad;
    squad.squadId = m_nextSquadId++;
    squad.teamId = teamId;
    squad.name = name.empty() ? GenerateSquadName(teamId) : name;
    m_squads[squad.squadId] = squad;
    Logger::Info("Squad '%s' created (id=%u) for team %u", squad.name.c_str(), squad.squadId, teamId);
    return squad.squadId;
}

bool RoleSystem::JoinSquad(uint32_t playerId, uint32_t squadId) {
    auto it = m_squads.find(squadId);
    if (it == m_squads.end()) return false;
    if (it->second.IsFull()) return false;

    LeaveSquad(playerId);
    it->second.memberIds.push_back(playerId);
    m_playerSquadMap[playerId] = squadId;

    // First member becomes squad leader if none exists
    if (!it->second.HasLeader()) {
        it->second.leaderId = playerId;
        m_playerRoles[playerId] = CombatRole::SquadLeader;
        Logger::Info("Player %u is now Squad Leader of '%s'", playerId, it->second.name.c_str());
    }

    Logger::Info("Player %u joined squad '%s'", playerId, it->second.name.c_str());
    return true;
}

bool RoleSystem::LeaveSquad(uint32_t playerId) {
    auto sit = m_playerSquadMap.find(playerId);
    if (sit == m_playerSquadMap.end()) return false;

    // Look the squad up rather than using operator[]: a stale player->squad mapping
    // (squad already disbanded) must NOT silently default-construct a phantom squad
    // here. If the invariant "mapped squad exists" is broken, drop the dangling
    // mapping and bail instead of fabricating an empty squad.
    auto squadIt = m_squads.find(sit->second);
    if (squadIt == m_squads.end()) {
        Logger::Warn("RoleSystem: player %u mapped to non-existent squad %u — clearing stale mapping",
                     playerId, sit->second);
        m_playerSquadMap.erase(sit);
        return false;
    }
    auto& squad = squadIt->second;
    auto& members = squad.memberIds;
    members.erase(std::remove(members.begin(), members.end(), playerId), members.end());

    // If leader left, promote next member
    if (squad.leaderId == playerId) {
        squad.leaderId = members.empty() ? 0 : members.front();
        if (squad.leaderId != 0) {
            m_playerRoles[squad.leaderId] = CombatRole::SquadLeader;
            Logger::Info("Player %u promoted to Squad Leader of '%s'", squad.leaderId, squad.name.c_str());
        }
    }

    m_playerSquadMap.erase(sit);
    return true;
}

bool RoleSystem::PromoteToSquadLeader(uint32_t playerId) {
    auto sit = m_playerSquadMap.find(playerId);
    if (sit == m_playerSquadMap.end()) return false;

    auto& squad = m_squads[sit->second];
    if (squad.leaderId != 0 && squad.leaderId != playerId) {
        m_playerRoles[squad.leaderId] = CombatRole::Rifleman;
    }
    squad.leaderId = playerId;
    m_playerRoles[playerId] = CombatRole::SquadLeader;
    return true;
}

uint32_t RoleSystem::GetPlayerSquad(uint32_t playerId) const {
    auto it = m_playerSquadMap.find(playerId);
    return it != m_playerSquadMap.end() ? it->second : 0;
}

const Squad* RoleSystem::GetSquad(uint32_t squadId) const {
    auto it = m_squads.find(squadId);
    return it != m_squads.end() ? &it->second : nullptr;
}

std::vector<const Squad*> RoleSystem::GetTeamSquads(uint32_t teamId) const {
    std::vector<const Squad*> result;
    for (const auto& [id, sq] : m_squads) {
        if (sq.teamId == teamId) result.push_back(&sq);
    }
    return result;
}

void RoleSystem::DisbandSquad(uint32_t squadId) {
    auto it = m_squads.find(squadId);
    if (it == m_squads.end()) return;
    for (uint32_t pid : it->second.memberIds) {
        m_playerSquadMap.erase(pid);
    }
    m_squads.erase(it);
}

uint8_t RoleSystem::ResolveRetailSquadCount(std::string_view modeName,
                                            int maxPlayers) {
    // Retail defaults an absent/invalid MaxPlayers value to the normal
    // 64-player server capacity.
    if (maxPlayers <= 0) maxPlayers = 64;

    std::string normalizedMode(modeName);
    std::transform(
        normalizedMode.begin(), normalizedMode.end(), normalizedMode.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool isSkirmish =
        normalizedMode.find("skirm") != std::string::npos;

    // Source-exact ROMapInfo.GetNumSquads thresholds. Skirmish has its own
    // small-server branch through 24 players, then falls through to the normal
    // 25+ behavior.
    if (isSkirmish && maxPlayers <= 24) {
        return maxPlayers <= 12 ? 1 : 2;
    }
    if (maxPlayers <= 12) return 2;
    if (maxPlayers <= 24) return 4;
    if (maxPlayers <= 32) return 8;
    return RETAIL_SQUAD_COUNT;
}

uint32_t RoleSystem::ConfigureRetailSquads(std::string_view modeName,
                                           int maxPlayers) {
    m_activeRetailSquadCount =
        ResolveRetailSquadCount(modeName, maxPlayers);
    return ResetRetailSquads();
}

bool RoleSystem::SetRetailSquadLocked(uint32_t teamId, uint8_t squadIndex,
                                      bool locked) {
    if (teamId == 0 || teamId > RETAIL_TEAM_COUNT ||
        squadIndex >= m_activeRetailSquadCount) {
        return false;
    }
    m_retailSquads[teamId - 1][squadIndex].locked = locked;
    return true;
}

bool RoleSystem::IsRetailSquadLocked(uint32_t teamId,
                                     uint8_t squadIndex) const {
    if (teamId == 0 || teamId > RETAIL_TEAM_COUNT ||
        squadIndex >= m_activeRetailSquadCount) {
        return false;
    }
    return m_retailSquads[teamId - 1][squadIndex].locked;
}

RetailSquadAssignment RoleSystem::AutoAssignRetailSquad(uint32_t playerId,
                                                        uint32_t teamId) {
    return AutoAssignRetailSquad(ParticipantId::Human(playerId), teamId);
}

RetailSquadAssignment RoleSystem::AutoAssignRetailSquad(
    const ParticipantId& participant, uint32_t teamId) {
    RetailSquadAssignment invalid;
    invalid.generation = m_retailSquadGeneration;
    if (!participant.IsValid() || teamId == 0 ||
        teamId > RETAIL_TEAM_COUNT) {
        return invalid;
    }

    ReconcileRetailSquads();
    if (const auto current = GetRetailSquadAssignment(participant)) {
        if (current->teamId == teamId) {
            return *current;
        }
    }

    // Select target capacity while the source assignment is still intact. A
    // failed cross-team move must leave the player's current squad untouched.
    const RetailSquadAssignment selected = FindRetailSquadSlot(teamId);
    if (!selected.IsValid()) {
        return invalid;
    }

    // No other thread mutates RoleSystem between selection and placement. The
    // release reconciles all fixed rosters, but cannot consume a free slot on
    // a different team.
    const auto squadsBeforeMove = m_retailSquads;
    const auto assignmentsBeforeMove = m_retailSquadAssignments;
    ReleaseRetailSquadAssignment(participant);

    RetailSquad& squad =
        m_retailSquads[selected.teamId - 1][selected.squadIndex];
    if (squad.slotOwnerIds[selected.roleIndex].IsValid()) {
        // Restore the complete clean snapshot if an invariant violation ever
        // invalidates the preselected target during source release.
        m_retailSquads = squadsBeforeMove;
        m_retailSquadAssignments = assignmentsBeforeMove;
        return invalid;
    }
    squad.slotOwnerIds[selected.roleIndex] = participant;

    RetailSquadAssignment assignment = selected;
    m_retailSquadAssignments[participant] = assignment;
    RepairRetailSquad(assignment.teamId, assignment.squadIndex);
    return assignment;
}

RetailSquadAssignment RoleSystem::JoinRetailSquad(
    uint32_t playerId, uint32_t authoritativeTeamId,
    uint8_t requestedSquadIndex) {
    const ParticipantId participant = ParticipantId::Human(playerId);
    RetailSquadAssignment invalid;
    invalid.generation = m_retailSquadGeneration;
    if (!participant.IsValid() || authoritativeTeamId == 0 ||
        authoritativeTeamId > RETAIL_TEAM_COUNT ||
        requestedSquadIndex >= RETAIL_SQUAD_COUNT) {
        return invalid;
    }

    ReconcileRetailSquads();
    const auto current = GetRetailSquadAssignment(participant);
    if (current && current->teamId == authoritativeTeamId &&
        current->squadIndex == requestedSquadIndex) {
        return *current;
    }
    if (requestedSquadIndex >= m_activeRetailSquadCount ||
        m_retailSquads[authoritativeTeamId - 1][requestedSquadIndex].locked) {
        return invalid;
    }

    RetailSquadAssignment selected;
    selected.teamId = authoritativeTeamId;
    selected.squadIndex = requestedSquadIndex;
    selected.generation = m_retailSquadGeneration;

    // Preflight the exact target while the source assignment is still intact.
    // In particular, a full target cannot evict a player from their current
    // squad before the request is rejected.
    const RetailSquad& target =
        m_retailSquads[authoritativeTeamId - 1][requestedSquadIndex];
    for (uint8_t roleIndex = 0; roleIndex < RetailSquad::SLOT_COUNT;
         ++roleIndex) {
        if (!target.slotOwnerIds[roleIndex].IsValid()) {
            selected.roleIndex = roleIndex;
            break;
        }
    }
    if (!selected.IsValid()) {
        return invalid;
    }

    // Keep a complete clean snapshot so a future invariant change cannot turn
    // an otherwise transactional move into a partial release.
    const auto squadsBeforeMove = m_retailSquads;
    const auto assignmentsBeforeMove = m_retailSquadAssignments;
    ReleaseRetailSquadAssignment(participant);

    RetailSquad& destination =
        m_retailSquads[selected.teamId - 1][selected.squadIndex];
    if (destination.slotOwnerIds[selected.roleIndex].IsValid()) {
        m_retailSquads = squadsBeforeMove;
        m_retailSquadAssignments = assignmentsBeforeMove;
        return invalid;
    }

    destination.slotOwnerIds[selected.roleIndex] = participant;
    m_retailSquadAssignments[participant] = selected;
    RepairRetailSquad(selected.teamId, selected.squadIndex);
    return selected;
}

bool RoleSystem::LeaveRetailSquad(uint32_t playerId) {
    return ReleaseRetailSquadAssignment(playerId);
}

RetailSquadAssignment RoleSystem::FindRetailSquadSlot(uint32_t teamId) const {
    RetailSquadAssignment selected;
    selected.generation = m_retailSquadGeneration;
    if (teamId == 0 || teamId > RETAIL_TEAM_COUNT) return selected;

    const RetailSquadTeam& squads = m_retailSquads[teamId - 1];
    size_t selectedOccupancy = 0;
    for (uint8_t squadIndex = 0; squadIndex < m_activeRetailSquadCount;
         ++squadIndex) {
        const RetailSquad& squad = squads[squadIndex];
        if (squad.locked) continue;
        const size_t occupancy = squad.Occupancy();
        if (occupancy >= RetailSquad::SLOT_COUNT) continue;
        if (selected.squadIndex !=
                RetailSquadAssignment::UNASSIGNED_INDEX &&
            occupancy <= selectedOccupancy) {
            continue;
        }

        uint8_t firstHole = RetailSquadAssignment::UNASSIGNED_INDEX;
        for (uint8_t roleIndex = 0; roleIndex < RetailSquad::SLOT_COUNT;
             ++roleIndex) {
            if (!squad.slotOwnerIds[roleIndex].IsValid()) {
                firstHole = roleIndex;
                break;
            }
        }
        if (firstHole == RetailSquadAssignment::UNASSIGNED_INDEX) continue;

        selected.teamId = teamId;
        selected.squadIndex = squadIndex;
        selected.roleIndex = firstHole;
        selectedOccupancy = occupancy;
    }
    return selected;
}

std::optional<RetailSquadAssignment>
RoleSystem::GetRetailSquadAssignment(uint32_t playerId) const {
    return GetRetailSquadAssignment(ParticipantId::Human(playerId));
}

std::optional<RetailSquadAssignment>
RoleSystem::GetRetailSquadAssignment(
    const ParticipantId& participant) const {
    if (!participant.IsValid()) return std::nullopt;
    const auto it = m_retailSquadAssignments.find(participant);
    if (it == m_retailSquadAssignments.end()) return std::nullopt;

    const RetailSquadAssignment& assignment = it->second;
    if (!assignment.IsValid() ||
        assignment.generation != m_retailSquadGeneration ||
        assignment.teamId > RETAIL_TEAM_COUNT ||
        assignment.squadIndex >= m_activeRetailSquadCount ||
        assignment.roleIndex >= RetailSquad::SLOT_COUNT) {
        return std::nullopt;
    }

    const RetailSquad& squad =
        m_retailSquads[assignment.teamId - 1][assignment.squadIndex];
    if (squad.slotOwnerIds[assignment.roleIndex] != participant) {
        return std::nullopt;
    }
    return assignment;
}

const RetailSquad* RoleSystem::GetRetailSquad(uint32_t teamId,
                                              uint8_t squadIndex) const {
    if (teamId == 0 || teamId > RETAIL_TEAM_COUNT ||
        squadIndex >= RETAIL_SQUAD_COUNT) {
        return nullptr;
    }
    return &m_retailSquads[teamId - 1][squadIndex];
}

uint32_t RoleSystem::GetRetailSquadLeader(uint32_t teamId,
                                          uint8_t squadIndex) const {
    const ParticipantId leader =
        GetRetailSquadLeaderParticipant(teamId, squadIndex);
    return leader.IsHuman() ? leader.value : 0u;
}

ParticipantId RoleSystem::GetRetailSquadLeaderParticipant(
    uint32_t teamId, uint8_t squadIndex) const {
    const RetailSquad* squad = GetRetailSquad(teamId, squadIndex);
    return squad ? squad->leaderId : ParticipantId{};
}

bool RoleSystem::IsRetailSquadLeader(uint32_t playerId) const {
    return IsRetailSquadLeader(ParticipantId::Human(playerId));
}

bool RoleSystem::IsRetailSquadLeader(
    const ParticipantId& participant) const {
    const auto assignment = GetRetailSquadAssignment(participant);
    return assignment &&
           GetRetailSquadLeaderParticipant(
               assignment->teamId, assignment->squadIndex) == participant;
}

uint32_t RoleSystem::ResetRetailSquads() {
    m_retailSquads = {};
    m_retailSquadAssignments.clear();
    ++m_retailSquadGeneration;
    if (m_retailSquadGeneration == 0) {
        // Keep zero reserved for default/invalid assignment values.
        m_retailSquadGeneration = 1;
    }
    return m_retailSquadGeneration;
}

bool RoleSystem::ReleaseRetailSquadAssignment(uint32_t playerId) {
    return ReleaseRetailSquadAssignment(ParticipantId::Human(playerId));
}

bool RoleSystem::ReleaseRetailSquadAssignment(
    const ParticipantId& participant) {
    if (!participant.IsValid()) return false;
    bool found = m_retailSquadAssignments.find(participant) !=
                 m_retailSquadAssignments.end();
    for (const RetailSquadTeam& team : m_retailSquads) {
        for (const RetailSquad& squad : team) {
            if (std::find(squad.slotOwnerIds.begin(),
                          squad.slotOwnerIds.end(), participant) !=
                squad.slotOwnerIds.end()) {
                found = true;
            }
        }
    }
    ReconcileRetailSquads(participant);
    return found;
}

void RoleSystem::ReconcileRetailSquads(
    const ParticipantId& removingParticipant) {
    if (removingParticipant.IsValid()) {
        m_retailSquadAssignments.erase(removingParticipant);
    }

    // Inactive stable indices must never retain owners or locks. This also
    // makes reconciliation a recovery boundary for raw/stale state injected
    // before a capacity reduction.
    for (RetailSquadTeam& team : m_retailSquads) {
        for (uint8_t squadIndex = m_activeRetailSquadCount;
             squadIndex < RETAIL_SQUAD_COUNT; ++squadIndex) {
            team[squadIndex] = {};
        }
    }

    // Reject invalid reverse records first. Their owner slots become orphans
    // and are removed by the following grid sweep.
    for (auto it = m_retailSquadAssignments.begin();
         it != m_retailSquadAssignments.end();) {
        const RetailSquadAssignment& assignment = it->second;
        if (!it->first.IsValid() || !assignment.IsValid() ||
            assignment.generation != m_retailSquadGeneration ||
            assignment.teamId > RETAIL_TEAM_COUNT ||
            assignment.squadIndex >= m_activeRetailSquadCount ||
            assignment.roleIndex >= RetailSquad::SLOT_COUNT) {
            it = m_retailSquadAssignments.erase(it);
        } else {
            ++it;
        }
    }

    // Every retained owner slot must have a live reverse record. Also remove
    // every occurrence of the departing player, including stale duplicates.
    for (RetailSquadTeam& team : m_retailSquads) {
        for (uint8_t squadIndex = 0; squadIndex < m_activeRetailSquadCount;
             ++squadIndex) {
            RetailSquad& squad = team[squadIndex];
            for (ParticipantId& ownerId : squad.slotOwnerIds) {
                if (!ownerId.IsValid()) continue;
                if (ownerId == removingParticipant ||
                    m_retailSquadAssignments.find(ownerId) ==
                        m_retailSquadAssignments.end()) {
                    ownerId = {};
                }
            }
        }
    }

    // Canonicalize each reverse record against the fixed owner grid. A unique
    // owner occurrence repairs stale coordinates. For duplicates, preserve the
    // recorded exact slot when possible; otherwise keep the lowest coordinate.
    for (auto it = m_retailSquadAssignments.begin();
         it != m_retailSquadAssignments.end();) {
        const ParticipantId participant = it->first;
        RetailSquadAssignment& assignment = it->second;
        uint32_t occurrenceCount = 0;
        uint32_t chosenTeam = 0;
        uint8_t chosenSquad = RetailSquadAssignment::UNASSIGNED_INDEX;
        uint8_t chosenRole = RetailSquadAssignment::UNASSIGNED_INDEX;
        bool choseRecordedSlot = false;

        for (uint32_t teamId = 1; teamId <= RETAIL_TEAM_COUNT; ++teamId) {
            RetailSquadTeam& team = m_retailSquads[teamId - 1];
            for (uint8_t squadIndex = 0;
                 squadIndex < m_activeRetailSquadCount;
                 ++squadIndex) {
                RetailSquad& squad = team[squadIndex];
                for (uint8_t roleIndex = 0;
                     roleIndex < RetailSquad::SLOT_COUNT; ++roleIndex) {
                    if (squad.slotOwnerIds[roleIndex] != participant) continue;
                    ++occurrenceCount;
                    const bool recorded =
                        assignment.teamId == teamId &&
                        assignment.squadIndex == squadIndex &&
                        assignment.roleIndex == roleIndex;
                    if (chosenTeam == 0 || (recorded && !choseRecordedSlot)) {
                        chosenTeam = teamId;
                        chosenSquad = squadIndex;
                        chosenRole = roleIndex;
                        choseRecordedSlot = recorded;
                    }
                }
            }
        }

        if (occurrenceCount == 0) {
            it = m_retailSquadAssignments.erase(it);
            continue;
        }

        for (uint32_t teamId = 1; teamId <= RETAIL_TEAM_COUNT; ++teamId) {
            RetailSquadTeam& team = m_retailSquads[teamId - 1];
            for (uint8_t squadIndex = 0;
                 squadIndex < m_activeRetailSquadCount;
                 ++squadIndex) {
                RetailSquad& squad = team[squadIndex];
                for (uint8_t roleIndex = 0;
                     roleIndex < RetailSquad::SLOT_COUNT; ++roleIndex) {
                    if (squad.slotOwnerIds[roleIndex] == participant &&
                        (teamId != chosenTeam || squadIndex != chosenSquad ||
                         roleIndex != chosenRole)) {
                        squad.slotOwnerIds[roleIndex] = {};
                    }
                }
            }
        }

        assignment.teamId = chosenTeam;
        assignment.squadIndex = chosenSquad;
        assignment.roleIndex = chosenRole;
        assignment.generation = m_retailSquadGeneration;
        ++it;
    }

    for (uint32_t teamId = 1; teamId <= RETAIL_TEAM_COUNT; ++teamId) {
        for (uint8_t squadIndex = 0;
             squadIndex < m_activeRetailSquadCount;
             ++squadIndex) {
            RepairRetailSquad(teamId, squadIndex);
        }
    }
}

void RoleSystem::RepairRetailSquad(uint32_t teamId, uint8_t squadIndex) {
    RetailSquad& squad = m_retailSquads[teamId - 1][squadIndex];
    if (!squad.slotOwnerIds[0].IsValid()) {
        for (uint8_t roleIndex = 1; roleIndex < RetailSquad::SLOT_COUNT;
             ++roleIndex) {
            const ParticipantId promoted = squad.slotOwnerIds[roleIndex];
            if (!promoted.IsValid()) continue;

            squad.slotOwnerIds[0] = promoted;
            squad.slotOwnerIds[roleIndex] = {};
            auto assignment = m_retailSquadAssignments.find(promoted);
            if (assignment != m_retailSquadAssignments.end()) {
                assignment->second.teamId = teamId;
                assignment->second.squadIndex = squadIndex;
                assignment->second.roleIndex = 0;
                assignment->second.generation = m_retailSquadGeneration;
            }
            break;
        }
    }
    squad.leaderId = squad.slotOwnerIds[0];
}

// --- Commander Management ---

bool RoleSystem::VolunteerAsCommander(uint32_t playerId) {
    auto* tm = m_server->GetTeamManager();
    uint32_t teamId = tm->GetPlayerTeam(playerId);
    if (teamId == 0) return false;

    if (HasCommander(teamId)) {
        Logger::Warn("Team %u already has a commander", teamId);
        return false;
    }

    m_teamCommanders[teamId] = playerId;
    m_playerRoles[playerId] = CombatRole::Commander;
    Logger::Info("Player %u is now Commander of team %u", playerId, teamId);
    return true;
}

bool RoleSystem::ResignAsCommander(uint32_t playerId) {
    for (auto& [teamId, cmdId] : m_teamCommanders) {
        if (cmdId == playerId) {
            m_teamCommanders.erase(teamId);
            m_playerRoles[playerId] = CombatRole::Rifleman;
            Logger::Info("Player %u resigned as Commander of team %u", playerId, teamId);
            return true;
        }
    }
    return false;
}

uint32_t RoleSystem::GetTeamCommander(uint32_t teamId) const {
    auto it = m_teamCommanders.find(teamId);
    return it != m_teamCommanders.end() ? it->second : 0;
}

bool RoleSystem::HasCommander(uint32_t teamId) const {
    return GetTeamCommander(teamId) != 0;
}

bool RoleSystem::IsRadiomanNearCommander(uint32_t teamId, float maxDistance) const {
    uint32_t cmdId = GetTeamCommander(teamId);
    if (cmdId == 0) return false;
    return GetNearestRadioman(cmdId) != 0;
}

uint32_t RoleSystem::GetNearestRadioman(uint32_t commanderId) const {
    auto* pm = m_server->GetPlayerManager();
    auto cmdPlayer = pm->GetPlayer(commanderId);
    if (!cmdPlayer) return 0;

    auto* tm = m_server->GetTeamManager();
    uint32_t teamId = tm->GetPlayerTeam(commanderId);
    auto teamPlayers = tm->GetTeamPlayers(teamId);

    float bestDist = 15.0f;  // max radioman range
    uint32_t bestId = 0;

    for (uint32_t pid : teamPlayers) {
        if (GetPlayerRole(pid) != CombatRole::Radioman) continue;
        auto rPlayer = pm->GetPlayer(pid);
        if (!rPlayer || !rPlayer->IsAlive()) continue;

        float dist = cmdPlayer->GetPosition().Distance(rPlayer->GetPosition());
        if (dist < bestDist) {
            bestDist = dist;
            bestId = pid;
        }
    }
    return bestId;
}

// --- Internal ---

std::string RoleSystem::GenerateSquadName(uint32_t teamId) const {
    static const char* names[] = {
        "Alpha", "Bravo", "Charlie", "Delta", "Echo",
        "Foxtrot", "Golf", "Hotel", "India", "Juliet"
    };
    int idx = 0;
    for (const auto& [id, sq] : m_squads) {
        if (sq.teamId == teamId) idx++;
    }
    if (idx < 10) return names[idx];
    return "Squad-" + std::to_string(idx + 1);
}

const RoleDefinition* RoleSystem::FindRoleDef(CombatRole role, Faction faction) const {
    for (const auto& def : m_roleDefinitions) {
        if (def.role == role && def.faction == faction) return &def;
    }
    return nullptr;
}

void RoleSystem::InitializeRoleDefinitions() {
    auto addRole = [&](Faction f, CombatRole r, const std::string& name,
                       int maxTeam, const std::string& primary, const std::string& secondary,
                       int primAmmo, int grenades) {
        RoleDefinition def;
        def.faction = f;
        def.role = r;
        def.displayName = name;
        def.maxPerTeam = maxTeam;
        def.loadout.primaryWeapon = primary;
        def.loadout.secondaryWeapon = secondary;
        def.loadout.primaryAmmo = primAmmo;
        def.loadout.grenades = grenades;
        def.loadout.smokeGrenades = (r == CombatRole::SquadLeader) ? 2 : 0;
        def.loadout.equipment.push_back("Bandage");
        if (r == CombatRole::SquadLeader)
            def.loadout.equipment.push_back("Smoke Grenade");
        m_roleDefinitions.push_back(def);
    };

    // === US Army Roles ===
    addRole(Faction::USArmy, CombatRole::Commander,         "Commander",         1, "M16A1", "M1911", 7, 1);
    addRole(Faction::USArmy, CombatRole::SquadLeader,       "Squad Leader",     -1, "M16A1", "M1911", 7, 2);
    addRole(Faction::USArmy, CombatRole::Rifleman,          "Rifleman",         -1, "M16A1", "M1911", 7, 2);
    addRole(Faction::USArmy, CombatRole::AutomaticRifleman, "Automatic Rifleman",3, "M14 Auto", "M1911", 6, 1);
    addRole(Faction::USArmy, CombatRole::MachineGunner,     "Machine Gunner",    2, "M60", "M1911", 3, 0);
    addRole(Faction::USArmy, CombatRole::Grenadier,         "Grenadier",         3, "M16A1/M203", "M1911", 5, 1);
    addRole(Faction::USArmy, CombatRole::Marksman,          "Marksman",          2, "M14 Scoped", "M1911", 6, 1);
    addRole(Faction::USArmy, CombatRole::Sniper,            "Sniper",            1, "M40", "M1911", 5, 0);
    addRole(Faction::USArmy, CombatRole::Pointman,          "Pointman",          3, "M1897 Trenchgun", "M1911", 6, 2);
    addRole(Faction::USArmy, CombatRole::CombatEngineer,    "Combat Engineer",   2, "M16A1", "M1911", 5, 1);
    addRole(Faction::USArmy, CombatRole::Radioman,          "Radioman",          2, "M16A1", "M1911", 7, 1);
    addRole(Faction::USArmy, CombatRole::HelicopterPilot,   "Helicopter Pilot",  2, "M1911", "", 3, 0);
    addRole(Faction::USArmy, CombatRole::HelicopterGunner,  "Door Gunner",       2, "M60 Door Gun", "M1911", 4, 0);

    // === USMC Roles ===
    addRole(Faction::USMC, CombatRole::Commander,         "Commander",         1, "M16A1", "M1911", 7, 1);
    addRole(Faction::USMC, CombatRole::SquadLeader,       "Squad Leader",     -1, "M16A1", "M1911", 7, 2);
    addRole(Faction::USMC, CombatRole::Rifleman,          "Rifleman",         -1, "M16A1", "M1911", 7, 2);
    addRole(Faction::USMC, CombatRole::MachineGunner,     "Machine Gunner",    2, "M60", "M1911", 3, 0);
    addRole(Faction::USMC, CombatRole::Grenadier,         "Grenadier",         3, "M16A1/M203", "M1911", 5, 1);
    addRole(Faction::USMC, CombatRole::Marksman,          "Marksman",          2, "M14 Scoped", "M1911", 6, 1);
    addRole(Faction::USMC, CombatRole::Pointman,          "Pointman",          3, "Ithaca 37", "M1911", 6, 2);
    addRole(Faction::USMC, CombatRole::CombatEngineer,    "Combat Engineer",   2, "M16A1", "M1911", 5, 1);
    addRole(Faction::USMC, CombatRole::Radioman,          "Radioman",          2, "M16A1", "M1911", 7, 1);

    // === Australian Army Roles ===
    addRole(Faction::AusArmy, CombatRole::Commander,         "Commander",         1, "L1A1 SLR", "Browning Hi-Power", 6, 1);
    addRole(Faction::AusArmy, CombatRole::SquadLeader,       "Squad Leader",     -1, "L1A1 SLR", "Browning Hi-Power", 6, 2);
    addRole(Faction::AusArmy, CombatRole::Rifleman,          "Rifleman",         -1, "L1A1 SLR", "Browning Hi-Power", 6, 2);
    addRole(Faction::AusArmy, CombatRole::MachineGunner,     "Machine Gunner",    2, "L2A1 LMG", "Browning Hi-Power", 3, 0);
    addRole(Faction::AusArmy, CombatRole::Grenadier,         "Grenadier",         3, "M79", "Browning Hi-Power", 10, 1);
    addRole(Faction::AusArmy, CombatRole::Marksman,          "Marksman",          2, "L1A1 Scoped", "Browning Hi-Power", 6, 1);
    addRole(Faction::AusArmy, CombatRole::Radioman,          "Radioman",          2, "L1A1 SLR", "Browning Hi-Power", 6, 1);

    // === NVA/PAVN Roles ===
    addRole(Faction::NVA, CombatRole::Commander,         "Commander",         1, "AK-47", "TT-33", 6, 1);
    addRole(Faction::NVA, CombatRole::SquadLeader,       "Squad Leader",     -1, "AK-47", "TT-33", 6, 2);
    addRole(Faction::NVA, CombatRole::Rifleman,          "Rifleman",         -1, "Mosin-Nagant M91/30", "TT-33", 8, 2);
    addRole(Faction::NVA, CombatRole::AutomaticRifleman, "Automatic Rifleman",3, "RPD", "TT-33", 3, 1);
    addRole(Faction::NVA, CombatRole::MachineGunner,     "Machine Gunner",    2, "DShK", "TT-33", 2, 0);
    addRole(Faction::NVA, CombatRole::Grenadier,         "Grenadier",         2, "AK-47", "TT-33", 4, 3);
    addRole(Faction::NVA, CombatRole::Marksman,          "Marksman",          2, "SVD Dragunov", "TT-33", 5, 1);
    addRole(Faction::NVA, CombatRole::Sniper,            "Sniper",            1, "Mosin-Nagant Scoped", "TT-33", 8, 0);
    addRole(Faction::NVA, CombatRole::RPGGunner,         "RPG Gunner",        2, "RPG-7", "TT-33", 3, 0);
    addRole(Faction::NVA, CombatRole::Sapper,            "Sapper",            3, "MAT-49", "TT-33", 4, 1);
    addRole(Faction::NVA, CombatRole::Radioman,          "Radioman",          2, "AK-47", "TT-33", 6, 1);

    // === Viet Cong (NLFSV) Roles ===
    addRole(Faction::NLFSV, CombatRole::Commander,         "Commander",         1, "AK-47", "TT-33", 6, 1);
    addRole(Faction::NLFSV, CombatRole::SquadLeader,       "Squad Leader",     -1, "SKS", "TT-33", 8, 2);
    addRole(Faction::NLFSV, CombatRole::Rifleman,          "Rifleman",         -1, "Mosin-Nagant M91/30", "TT-33", 8, 2);
    addRole(Faction::NLFSV, CombatRole::Pointman,          "Scout",              3, "MAT-49", "TT-33", 4, 2);
    addRole(Faction::NLFSV, CombatRole::AutomaticRifleman, "Automatic Rifleman",3, "RPD", "TT-33", 3, 1);
    addRole(Faction::NLFSV, CombatRole::MachineGunner,     "Machine Gunner",    2, "DP-28", "TT-33", 3, 0);
    addRole(Faction::NLFSV, CombatRole::Sapper,            "Sapper",            4, "MAT-49", "TT-33", 4, 1);
    addRole(Faction::NLFSV, CombatRole::RPGGunner,         "RPG Gunner",        2, "RPG-7", "TT-33", 3, 0);
    addRole(Faction::NLFSV, CombatRole::Sniper,            "Sniper",            1, "Mosin-Nagant Scoped", "TT-33", 8, 0);
    addRole(Faction::NLFSV, CombatRole::Radioman,          "Radioman",          2, "SKS", "TT-33", 8, 1);
}
