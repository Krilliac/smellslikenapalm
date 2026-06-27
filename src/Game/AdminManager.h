// src/Game/AdminManager.h – admin/ban data store and privileged player operations
//
// AdminManager owns the persistent ADMIN-LIST authorization data (per-SteamID
// permission levels) and the privileged player operations that mutate connection
// state (kick/ban/unban). It is NOT the command parser — parsing, permission
// gating and dispatch live in CommandManager. It is also NOT the ban store: the
// single authoritative ban list lives in the security layer (BanManager, behind
// the login bridge); AdminManager's ban operations delegate to GameServer, which
// forwards to that one store. This avoids the previous shadow ban list that
// drifted from — and was clobbered by — the security layer's own list.

#pragma once

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <memory>

#include "Game/BanRecord.h"

class GameServer;
class ServerConfig;

class AdminManager
{
public:
    AdminManager(GameServer* server, std::shared_ptr<ServerConfig> config);
    ~AdminManager();

    void Initialize();
    void Shutdown();

    // --- Authorization queries ---
    // IsAdmin: listed at any non-zero level. GetPermissionLevel: the numeric
    // level from admin_list.txt (0 = unlisted/plain player).
    bool IsAdmin(const std::string& steamId) const;
    int  GetPermissionLevel(const std::string& steamId) const;
    bool IsBanned(const std::string& steamId) const;          // delegates to ban store

    // Snapshots for the `admins` / `banlist` commands.
    std::vector<std::pair<std::string, int>> GetAdminList() const;
    std::vector<BanRecord> GetActiveBans() const;             // delegates to ban store

    void LoadAdminList();

    // --- Privileged player operations (called by CommandManager / AntiCheat) ---
    // adminClientId is the issuing in-game client, or INVALID/0 for
    // console/remote/system callers (connection lookups are null-safe). Ban/Unban
    // record into the single authoritative ban store via GameServer.
    bool KickPlayer(uint32_t adminClientId, const std::string& targetSteamId);
    bool BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes,
                   const std::string& reason = "");
    bool Unban(const std::string& targetSteamId);

private:
    GameServer* m_server;
    std::shared_ptr<ServerConfig> m_config;

    // Flat list of all listed SteamIDs (any level) for fast IsAdmin checks, plus
    // the SteamID -> level map for GetPermissionLevel.
    std::vector<std::string> m_admins;
    std::map<std::string, int> m_adminLevels;
};
