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
    Logger::Trace("[ToString(PacketType)] entry — pt=%d", static_cast<int>(pt));
    auto idx = static_cast<size_t>(pt);
    if (idx >= static_cast<size_t>(PacketType::PT_MAX)) {
        Logger::Warn("[ToString(PacketType)] index %zu out of range (max=%zu) — returning 'INVALID'",
                     idx, static_cast<size_t>(PacketType::PT_MAX));
        Logger::Trace("[ToString(PacketType)] exit — returning '%s' (out of range)", PacketTypeTags[0]);
        return PacketTypeTags[0];
    }
    Logger::Debug("[ToString(PacketType)] resolved PacketType %d (index %zu) to tag '%s'",
                  static_cast<int>(pt), idx, PacketTypeTags[idx]);
    Logger::Trace("[ToString(PacketType)] exit — returning '%s'", PacketTypeTags[idx]);
    return PacketTypeTags[idx];
}

PacketType FromString(const std::string& tag) {
    Logger::Trace("[FromString(PacketType)] entry — tag='%s'", tag.c_str());
    for (size_t i = 1; i < static_cast<size_t>(PacketType::PT_MAX); ++i) {
        if (tag == PacketTypeTags[i]) {
            Logger::Debug("[FromString(PacketType)] matched tag '%s' at index %zu to PacketType=%d",
                          tag.c_str(), i, static_cast<int>(static_cast<PacketType>(i)));
            Logger::Trace("[FromString(PacketType)] exit — returning PacketType %d", static_cast<int>(static_cast<PacketType>(i)));
            return static_cast<PacketType>(i);
        }
    }
    Logger::Warn("[FromString(PacketType)] no match found for tag '%s' after searching %zu entries — returning PT_INVALID",
                 tag.c_str(), static_cast<size_t>(PacketType::PT_MAX) - 1);
    Logger::Trace("[FromString(PacketType)] exit — returning PT_INVALID");
    return PacketType::PT_INVALID;
}
