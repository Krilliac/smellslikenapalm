// src/Game/RoleSystem.h
// RS2V role/class system — defines factions, roles, loadouts, and squad structure

#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <memory>

// Factions in RS2V
enum class Faction : uint8_t {
    None = 0,
    USArmy,         // United States Army
    USMC,           // United States Marine Corps
    AusArmy,        // Australian Army
    NVA,            // North Vietnamese Army (PAVN)
    NLFSV,          // National Liberation Front (Viet Cong)
    FactionCount
};

// Combat roles available in RS2V
enum class CombatRole : uint8_t {
    None = 0,
    Commander,          // 1 per team — calls in fire support
    SquadLeader,        // 1 per squad — marks targets, squad spawn point
    Rifleman,           // Basic infantry
    AutomaticRifleman,  // BAR/RPD — suppressive fire
    MachineGunner,      // M60/DShK — heavy suppressive fire
    Grenadier,          // M79/RPG — explosives
    Marksman,           // Scoped rifles
    Sniper,             // Bolt-action scoped rifles
    Pointman,           // Shotgun/SMG — close quarters
    Sapper,             // C4/mines/traps — VC/NVA role
    RPGGunner,          // RPG-7 — anti-vehicle/anti-air
    CombatEngineer,     // Flamethrower/mines — US role
    Radioman,           // Carries radio for commander
    HelicopterPilot,    // US only — flies transport/attack choppers
    HelicopterGunner,   // US only — door gunner
    RoleCount
};

// Per-role loadout: which weapons and equipment a role carries
struct RoleLoadout {
    std::string primaryWeapon;
    std::string secondaryWeapon;
    std::vector<std::string> equipment;    // grenades, bandages, traps, etc.
    int primaryAmmo = 0;                   // magazines
    int secondaryAmmo = 0;
    int grenades = 0;
    int smokeGrenades = 0;
};

// Definition of a specific role within a faction
struct RoleDefinition {
    CombatRole role = CombatRole::None;
    Faction faction = Faction::None;
    std::string displayName;
    std::string description;
    int maxPerTeam = -1;        // -1 = unlimited
    int maxPerSquad = -1;       // -1 = unlimited
    int levelRequired = 0;      // minimum player level
    RoleLoadout loadout;
};

// Squad structure
struct Squad {
    uint32_t squadId = 0;
    std::string name;                   // "Alpha", "Bravo", etc.
    uint32_t teamId = 0;
    uint32_t leaderId = 0;              // player ID of squad leader
    std::vector<uint32_t> memberIds;
    static constexpr int MAX_SQUAD_SIZE = 6;

    bool IsFull() const { return memberIds.size() >= MAX_SQUAD_SIZE; }
    bool HasLeader() const { return leaderId != 0; }
};

class GameServer;
class Player;

class RoleSystem {
public:
    explicit RoleSystem(GameServer* server);
    ~RoleSystem();

    void Initialize();
    void Shutdown();

    // Faction management
    void SetTeamFaction(uint32_t teamId, Faction faction);
    Faction GetTeamFaction(uint32_t teamId) const;
    std::string GetFactionName(Faction f) const;
    std::string GetFactionShortName(Faction f) const;

    // Role management
    bool AssignRole(uint32_t playerId, CombatRole role);
    CombatRole GetPlayerRole(uint32_t playerId) const;
    std::string GetRoleName(CombatRole role) const;
    bool IsRoleAvailable(uint32_t teamId, CombatRole role) const;
    int GetRoleCount(uint32_t teamId, CombatRole role) const;
    int GetRoleLimit(uint32_t teamId, CombatRole role) const;
    std::vector<CombatRole> GetAvailableRoles(uint32_t teamId, Faction faction) const;
    RoleLoadout GetRoleLoadout(CombatRole role, Faction faction) const;

    // Squad management
    uint32_t CreateSquad(uint32_t teamId, const std::string& name = "");
    bool JoinSquad(uint32_t playerId, uint32_t squadId);
    bool LeaveSquad(uint32_t playerId);
    bool PromoteToSquadLeader(uint32_t playerId);
    uint32_t GetPlayerSquad(uint32_t playerId) const;
    const Squad* GetSquad(uint32_t squadId) const;
    std::vector<const Squad*> GetTeamSquads(uint32_t teamId) const;
    void DisbandSquad(uint32_t squadId);

    // Commander management
    bool VolunteerAsCommander(uint32_t playerId);
    bool ResignAsCommander(uint32_t playerId);
    uint32_t GetTeamCommander(uint32_t teamId) const;
    bool HasCommander(uint32_t teamId) const;

    // Radioman
    bool IsRadiomanNearCommander(uint32_t teamId, float maxDistance = 15.0f) const;
    uint32_t GetNearestRadioman(uint32_t commanderId) const;

private:
    GameServer* m_server;

    // Faction assignments per team
    std::map<uint32_t, Faction> m_teamFactions;

    // Player role assignments
    std::unordered_map<uint32_t, CombatRole> m_playerRoles;

    // Squad system
    std::map<uint32_t, Squad> m_squads;
    std::unordered_map<uint32_t, uint32_t> m_playerSquadMap;  // playerId -> squadId
    uint32_t m_nextSquadId = 1;

    // Commander assignments per team
    std::map<uint32_t, uint32_t> m_teamCommanders;  // teamId -> playerId

    // Role definitions
    std::vector<RoleDefinition> m_roleDefinitions;
    void InitializeRoleDefinitions();
    const RoleDefinition* FindRoleDef(CombatRole role, Faction faction) const;
    std::string GenerateSquadName(uint32_t teamId) const;
};
