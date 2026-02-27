// src/Protocol/PacketTypes.h
#pragma once

#include <cstdint>
#include <string>

// PacketTypes enumerates all high‐level packet tags used in RS2V networking.
// Each value corresponds to a Packet::GetTag() string.
//
// When adding a new packet type, append before PT_MAX and update PacketTypes.cpp.
enum class PacketType : uint16_t {
    PT_INVALID = 0,           // placeholder for uninitialized

    PT_HEARTBEAT,             // keep‐alive ping/pong
    PT_CHAT_MESSAGE,          // in/out chat text
    PT_PLAYER_SPAWN,          // spawn player event
    PT_PLAYER_MOVE,           // client movement update
    PT_PLAYER_ACTION,         // client action (shoot/reload/command)
    PT_HEALTH_UPDATE,         // server→client health sync
    PT_TEAM_UPDATE,           // server→client team assignment
    PT_SPAWN_ENTITY,          // server→client entity spawn
    PT_DESPAWN_ENTITY,        // server→client entity removal
    PT_ACTOR_REPLICATION,     // server→client world state delta
    PT_OBJECTIVE_UPDATE,      // server→client objective state change
    PT_SCORE_UPDATE,          // server→client score or stats
    PT_SESSION_STATE,         // server→client session/alive count
    PT_CHAT_HISTORY,          // server→client chat backlog
    PT_ADMIN_COMMAND,         // client→server admin request
    PT_SERVER_NOTIFICATION,   // server→client generic notices
    PT_MAP_CHANGE,            // server→client map rotation
    PT_CONFIG_SYNC,           // server→client config data
    PT_COMPRESSION,           // wrapper when payload compressed
    PT_RPC_CALL,              // remote‐procedure call invocation
    PT_RPC_RESPONSE,          // RPC return data

    // RS2V Game System packet types:
    PT_ROLE_SELECT,           // client→server role/class selection
    PT_ROLE_UPDATE,           // server→client role assignment confirmation
    PT_SPAWN_REQUEST,         // client→server spawn location request
    PT_SPAWN_LOCATIONS,       // server→client available spawn points
    PT_COMMANDER_ABILITY,     // client→server fire support request
    PT_COMMANDER_ABILITY_ACK, // server→client ability confirmation/denial
    PT_FIRE_SUPPORT_UPDATE,   // server→client active fire support state
    PT_SQUAD_ACTION,          // client→server squad create/join/leave
    PT_SQUAD_UPDATE,          // server→client squad state sync
    PT_TICKET_UPDATE,         // server→client reinforcement ticket count
    PT_WEAPON_FIRE,           // client→server weapon fire event
    PT_DAMAGE_EVENT,          // server→client damage notification
    PT_KILL_EVENT,            // server→client kill feed entry
    PT_VEHICLE_ACTION,        // client→server vehicle enter/exit/control
    PT_VEHICLE_STATE,         // server→client vehicle position/health sync
    PT_HELICOPTER_STATE,      // server→client helicopter flight state
    PT_GAMEMODE_STATE,        // server→client game mode phase/timer
    PT_CAPTURE_PROGRESS,      // server→client objective capture progress
    PT_LOADOUT_UPDATE,        // server→client player loadout sync
    PT_SUPPRESSION_EVENT,     // server→client suppression effect

    // Custom mod/extension packet types start here:
    PT_CUSTOM_START,

    // Sentinel
    PT_MAX
};

// Convert PacketType to string tag and back
const char* ToString(PacketType pt);
PacketType FromString(const std::string& tag);