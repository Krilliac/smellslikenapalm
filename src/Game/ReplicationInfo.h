// src/Game/ReplicationInfo.h
//
// Lightweight server-side mirrors of the UE3 replicated info actors that the
// RS2 client expects right after it joins:
//
//   * GameReplicationInfo (GRI) — Engine/GameReplicationInfo.uc +
//     ROGame/ROGameReplicationInfo.uc. A SINGLE instance per server holding
//     server-wide state (server name, game class, elapsed time, team scores,
//     round state). The client reads this to populate the scoreboard header.
//
//   * PlayerReplicationInfo (PRI) — Engine/PlayerReplicationInfo.uc +
//     ROGame/ROPlayerReplicationInfo.uc. One per player; carries the
//     identity/score the other clients need to display that player.
//
// These are intentionally NOT the engine's full property sets — only the
// initial-replication fields Stream B needs to make a connected client into a
// visible, scoreboard-listed player. They are registered as "actors" with the
// existing Protocol/ReplicationManager (RegisterActor + QueuePropertyUpdate)
// so they replicate on join; this file does not re-implement replication.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Mirrors Engine/PlayerReplicationInfo.uc / ROPlayerReplicationInfo.uc fields
// that are populated during Login/PostLogin.
struct PlayerReplicationInfo {
    // Engine PlayerReplicationInfo
    int32_t     playerId    = -1;       // PlayerID — unique per-match small int
    std::string playerName;             // PlayerName
    int32_t     team        = 255;      // Team index (255 == none/unassigned)
    int32_t     score       = 0;        // Score
    int32_t     deaths      = 0;        // Deaths
    bool        bBot        = false;    // bBot — always false for a real client
    bool        bIsSpectator = false;   // bIsSpectator / bOnlySpectator
    uint64_t    uniqueId    = 0;        // UniqueId (UniqueNetId) — Steam id

    // Server-side bookkeeping (not replicated as-is): the network/connection id
    // this PRI belongs to. Lets us correlate PRI <-> connection/Player.
    uint32_t    clientId    = 0;

    // The replication actor id under which this PRI is registered with the
    // ReplicationManager (0 == not yet registered).
    uint32_t    actorId     = 0;
};

// Mirrors Engine/GameReplicationInfo.uc / ROGameReplicationInfo.uc fields used
// for the initial scoreboard/header replication.
struct GameReplicationInfo {
    std::string serverName;             // ServerName
    std::string gameClass;              // GameClass (e.g. "ROGame.ROGameInfoTerritories")
    float       elapsedTime  = 0.0f;    // ElapsedTime
    float       remainingTime = 0.0f;   // RemainingTime
    int32_t     teamScore[2] = {0, 0};  // Teams[i].Score (US / NVA)
    int32_t     roundNumber  = 0;       // CurrentRound-ish
    bool        bMatchHasBegun = false; // bMatchHasBegun
    bool        bMatchIsOver   = false; // bMatchIsOver

    // Replication actor id under which the (single) GRI is registered.
    uint32_t    actorId      = 0;
};
