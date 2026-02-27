// src/Protocol/PacketTypes.cpp
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"
#include <string>

// Map PacketType enum to string tags and back.
// Used by MessageEncoder/Decoder and PacketHandler registry.
static const char* PacketTypeTags[] = {
    "INVALID",
    "HEARTBEAT",
    "CHAT_MESSAGE",
    "PLAYER_SPAWN",
    "PLAYER_MOVE",
    "PLAYER_ACTION",
    "HEALTH_UPDATE",
    "TEAM_UPDATE",
    "SPAWN_ENTITY",
    "DESPAWN_ENTITY",
    "ACTOR_REPLICATION",
    "OBJECTIVE_UPDATE",
    "SCORE_UPDATE",
    "SESSION_STATE",
    "CHAT_HISTORY",
    "ADMIN_COMMAND",
    "SERVER_NOTIFICATION",
    "MAP_CHANGE",
    "CONFIG_SYNC",
    "COMPRESSION",
    "RPC_CALL",
    "RPC_RESPONSE",
    // RS2V Game System packet types
    "ROLE_SELECT",
    "ROLE_UPDATE",
    "SPAWN_REQUEST",
    "SPAWN_LOCATIONS",
    "COMMANDER_ABILITY",
    "COMMANDER_ABILITY_ACK",
    "FIRE_SUPPORT_UPDATE",
    "SQUAD_ACTION",
    "SQUAD_UPDATE",
    "TICKET_UPDATE",
    "WEAPON_FIRE",
    "DAMAGE_EVENT",
    "KILL_EVENT",
    "VEHICLE_ACTION",
    "VEHICLE_STATE",
    "HELICOPTER_STATE",
    "GAMEMODE_STATE",
    "CAPTURE_PROGRESS",
    "LOADOUT_UPDATE",
    "SUPPRESSION_EVENT",
    "CUSTOM_START"
};

static_assert(sizeof(PacketTypeTags)/sizeof(*PacketTypeTags) == static_cast<size_t>(PacketType::PT_MAX),
              "PacketTypeTags size must match PacketType::PT_MAX");

const char* ToString(PacketType pt) {
    auto idx = static_cast<size_t>(pt);
    if (idx >= static_cast<size_t>(PacketType::PT_MAX)) {
        return PacketTypeTags[0];
    }
    return PacketTypeTags[idx];
}

PacketType FromString(const std::string& tag) {
    for (size_t i = 1; i < static_cast<size_t>(PacketType::PT_MAX); ++i) {
        if (tag == PacketTypeTags[i]) {
            return static_cast<PacketType>(i);
        }
    }
    return PacketType::PT_INVALID;
}