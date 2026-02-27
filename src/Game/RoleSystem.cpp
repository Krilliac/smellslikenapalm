// src/Game/RoleSystem.cpp
// RS2V role/class system implementation

#include "Game/RoleSystem.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Utils/Logger.h"
#include <algorithm>

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

    if (!IsRoleAvailable(teamId, role)) {
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

    auto& squad = m_squads[sit->second];
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
    addRole(Faction::NLFSV, CombatRole::AutomaticRifleman, "Automatic Rifleman",3, "RPD", "TT-33", 3, 1);
    addRole(Faction::NLFSV, CombatRole::MachineGunner,     "Machine Gunner",    2, "DP-28", "TT-33", 3, 0);
    addRole(Faction::NLFSV, CombatRole::Sapper,            "Sapper",            4, "MAT-49", "TT-33", 4, 1);
    addRole(Faction::NLFSV, CombatRole::RPGGunner,         "RPG Gunner",        2, "RPG-7", "TT-33", 3, 0);
    addRole(Faction::NLFSV, CombatRole::Sniper,            "Sniper",            1, "Mosin-Nagant Scoped", "TT-33", 8, 0);
    addRole(Faction::NLFSV, CombatRole::Radioman,          "Radioman",          2, "SKS", "TT-33", 8, 1);
}
