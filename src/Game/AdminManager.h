// src/Game/AdminManager.h – admin/ban data store and privileged player operations
//
// AdminManager owns the persistent authorization data (the admin list with
// per-SteamID permission levels, and the ban list) and the privileged player
// operations that mutate connection state (kick/ban/unban). It is NOT the
// command parser — command parsing, permission gating and dispatch live in
// CommandManager, which calls the operations here. Keeping the data/ops here
// and the dispatch there avoids two parallel command paths.

#pragma once

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <chrono>
#include <memory>

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
    bool IsBanned(const std::string& steamId) const;

    // Snapshots for the `admins` / `banlist` commands.
    std::vector<std::pair<std::string, int>> GetAdminList() const;
    std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> GetActiveBans() const;

    void LoadAdminList();
    void LoadBanList();
    void SaveBanList() const;

    // --- Privileged player operations (called by CommandManager / AntiCheat) ---
    // adminClientId is the issuing in-game client, or INVALID/0 for
    // console/remote/system callers (connection lookups are null-safe).
    bool KickPlayer(uint32_t adminClientId, const std::string& targetSteamId);
    bool BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes);
    bool Unban(const std::string& targetSteamId);

private:
    GameServer* m_server;
    std::shared_ptr<ServerConfig> m_config;

    // Flat list of all listed SteamIDs (any level) for fast IsAdmin checks, plus
    // the SteamID -> level map for GetPermissionLevel.
    std::vector<std::string> m_admins;
    std::map<std::string, int> m_adminLevels;

    std::map<std::string, std::chrono::system_clock::time_point> m_bans;
};
