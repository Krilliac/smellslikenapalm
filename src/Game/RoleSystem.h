// src/Game/RoleSystem.h
// RS2V role/class system — defines factions, roles, loadouts, and squad structure

#pragma once

#include <string>
#include <array>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

#include "Game/ParticipantRoster.h"

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

// Match-scoped retail squad state is deliberately independent from the
// legacy Squad/CombatRole model above.  UE3 exposes a stable zero-based squad
// index and a six-slot role index; occupying the leadership slot must not
// silently replace the infantry class selected by the player.
struct RetailSquad {
    static constexpr uint8_t SLOT_COUNT = 6;

    // Retail counts both human Controllers and AI Controllers as squad owners.
    // Keep the identity tagged so Human(1) and Bot(1) cannot alias while the
    // fixed six-slot wire indices remain unchanged.
    std::array<ParticipantId, SLOT_COUNT> slotOwnerIds{};
    ParticipantId leaderId{};
    bool locked = false;

    size_t Occupancy() const {
        size_t count = 0;
        for (const ParticipantId& ownerId : slotOwnerIds) {
            if (ownerId.IsValid()) ++count;
        }
        return count;
    }
    bool IsFull() const { return Occupancy() == slotOwnerIds.size(); }
};

struct RetailSquadAssignment {
    static constexpr uint8_t UNASSIGNED_INDEX = 255;

    uint32_t teamId = 0;
    uint8_t squadIndex = UNASSIGNED_INDEX;
    uint8_t roleIndex = UNASSIGNED_INDEX;
    uint32_t generation = 0;

    bool IsValid() const {
        return teamId != 0 && squadIndex != UNASSIGNED_INDEX &&
               roleIndex != UNASSIGNED_INDEX && generation != 0;
    }
};

class GameServer;
class Player;
class RoleSystemLifecycleTestHarness;

class RoleSystem {
public:
    static constexpr uint8_t RETAIL_TEAM_COUNT = 2;
    static constexpr uint8_t RETAIL_SQUAD_COUNT = 10;

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
    // Idempotent gameplay-lifecycle cleanup used for both disconnect and map
    // travel. Removes every role, commander, and squad reference owned by the
    // player while preserving valid remaining squad membership.
    void RemovePlayer(uint32_t playerId);
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

    // Match-scoped UE3 squad allocation. Repeating this call for an already
    // assigned player is idempotent. New players join the fullest non-full
    // squad (lowest index breaks ties) and occupy its first free role slot.
    RetailSquadAssignment AutoAssignRetailSquad(uint32_t playerId,
                                                 uint32_t teamId);
    RetailSquadAssignment AutoAssignRetailSquad(
        const ParticipantId& participant, uint32_t teamId);
    // Transactionally joins one explicit retail squad on the authoritative
    // team. The target's first free role slot is reserved before the source is
    // released, so an invalid or full target leaves the current assignment
    // untouched. Rejoining the same squad is idempotent, including when full.
    RetailSquadAssignment JoinRetailSquad(uint32_t playerId,
                                          uint32_t authoritativeTeamId,
                                          uint8_t requestedSquadIndex);
    // Explicit retail leave used by the native ServerLeaveSquad path. The
    // return value reports whether any forward or reverse membership existed;
    // CombatRole and deployment ownership remain unchanged.
    bool LeaveRetailSquad(uint32_t playerId);
    // Frees only retail squad ownership, leaving CombatRole unchanged. The
    // fixed roster is reconciled so stale duplicates/orphans cannot leak slots.
    bool ReleaseRetailSquadAssignment(uint32_t playerId);
    bool ReleaseRetailSquadAssignment(const ParticipantId& participant);
    std::optional<RetailSquadAssignment> GetRetailSquadAssignment(
        uint32_t playerId) const;
    std::optional<RetailSquadAssignment> GetRetailSquadAssignment(
        const ParticipantId& participant) const;
    const RetailSquad* GetRetailSquad(uint32_t teamId,
                                     uint8_t squadIndex) const;
    uint32_t GetRetailSquadLeader(uint32_t teamId,
                                  uint8_t squadIndex) const;
    ParticipantId GetRetailSquadLeaderParticipant(
        uint32_t teamId, uint8_t squadIndex) const;
    bool IsRetailSquadLeader(uint32_t playerId) const;
    bool IsRetailSquadLeader(const ParticipantId& participant) const;

    // Retail ROMapInfo.GetNumSquads behavior. Invalid/non-positive capacities
    // use the shipped 64-player default instead of accidentally constraining a
    // match to the smallest roster.
    static uint8_t ResolveRetailSquadCount(std::string_view modeName,
                                           int maxPlayers);
    // Applies map/mode capacity and starts a fresh squad generation. The fixed
    // ten-element backing arrays remain addressable for replication, while
    // assignment APIs reject indices outside the active prefix.
    uint32_t ConfigureRetailSquads(std::string_view modeName, int maxPlayers);
    uint8_t GetActiveRetailSquadCount() const {
        return m_activeRetailSquadCount;
    }
    // Squad locking affects new automatic and explicit joins. Existing members
    // remain valid and may repeat an idempotent join request.
    bool SetRetailSquadLocked(uint32_t teamId, uint8_t squadIndex,
                              bool locked);
    bool IsRetailSquadLocked(uint32_t teamId, uint8_t squadIndex) const;

    // Starts a fresh map/match generation and invalidates every previous
    // retail assignment. The configured active count is retained; all locks
    // are cleared. Generation zero is reserved for invalid results.
    uint32_t ResetRetailSquads();
    uint32_t GetRetailSquadGeneration() const {
        return m_retailSquadGeneration;
    }

    // Commander management
    bool VolunteerAsCommander(uint32_t playerId);
    bool ResignAsCommander(uint32_t playerId);
    uint32_t GetTeamCommander(uint32_t teamId) const;
    bool HasCommander(uint32_t teamId) const;

    // Radioman
    bool IsRadiomanNearCommander(uint32_t teamId, float maxDistance = 15.0f) const;
    uint32_t GetNearestRadioman(uint32_t commanderId) const;

private:
    friend class RoleSystemLifecycleTestHarness;

    GameServer* m_server;

    // Faction assignments per team
    std::map<uint32_t, Faction> m_teamFactions;

    // Player role assignments
    std::unordered_map<uint32_t, CombatRole> m_playerRoles;

    // Squad system
    std::map<uint32_t, Squad> m_squads;
    std::unordered_map<uint32_t, uint32_t> m_playerSquadMap;  // playerId -> squadId
    uint32_t m_nextSquadId = 1;

    // Fixed team/squad arrays keep the retail indices stable for a 64-player
    // match. Non-leader holes are retained; only a vacated leader slot zero
    // promotes the owner from the lowest remaining occupied role slot.
    using RetailSquadTeam = std::array<RetailSquad, RETAIL_SQUAD_COUNT>;
    std::array<RetailSquadTeam, RETAIL_TEAM_COUNT> m_retailSquads{};
    std::map<ParticipantId, RetailSquadAssignment>
        m_retailSquadAssignments;
    uint32_t m_retailSquadGeneration = 1;
    uint8_t m_activeRetailSquadCount = RETAIL_SQUAD_COUNT;

    // Commander assignments per team
    std::map<uint32_t, uint32_t> m_teamCommanders;  // teamId -> playerId

    // Role definitions
    std::vector<RoleDefinition> m_roleDefinitions;
    void InitializeRoleDefinitions();
    const RoleDefinition* FindRoleDef(CombatRole role, Faction faction) const;
    std::string GenerateSquadName(uint32_t teamId) const;
    RetailSquadAssignment FindRetailSquadSlot(uint32_t teamId) const;
    void ReconcileRetailSquads(
        const ParticipantId& removingParticipant = ParticipantId{});
    void RepairRetailSquad(uint32_t teamId, uint8_t squadIndex);
};
