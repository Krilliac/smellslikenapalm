// src/Game/AdminManager.h – Header for AdminManager

#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>

class GameServer;
class ServerConfig;
namespace Utils { class StringUtils; }

class AdminManager
{
public:
    AdminManager(GameServer* server, std::shared_ptr<ServerConfig> config);
    ~AdminManager();

    void Initialize();
    void Shutdown();

    bool HandleAdminCommand(uint32_t clientId, const std::string& command, const std::vector<std::string>& args);

    bool IsAdmin(const std::string& steamId) const;
    bool IsBanned(const std::string& steamId) const;

    void LoadAdminList();
    void LoadBanList();
    void SaveBanList() const;

private:
    GameServer* m_server;
    std::shared_ptr<ServerConfig> m_config;
    std::vector<std::string> m_admins;
    std::map<std::string, std::chrono::system_clock::time_point> m_bans;

    bool KickPlayer(uint32_t adminClientId, const std::string& targetSteamId);
    bool BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes);
    bool BroadcastMessage(uint32_t adminClientId, const std::string& message);
    bool ReloadConfig(uint32_t adminClientId, const std::string& section);
    bool ListAdmins(uint32_t adminClientId);

    std::string JoinArgs(const std::vector<std::string>& args, const std::string& sep) const;
};