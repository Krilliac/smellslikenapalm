// src/Game/AdminManager.cpp – admin/ban data store + privileged player operations.

#include "Game/AdminManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Config/ServerConfig.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

static constexpr uint32_t INVALID_CLIENT_ID = UINT32_MAX;

AdminManager::AdminManager(GameServer* server, std::shared_ptr<ServerConfig> config)
    : m_server(server), m_config(config)
{
    Logger::Trace("[AdminManager::AdminManager] Entry: server=%p, config=%p",
                  static_cast<const void*>(server), static_cast<const void*>(config.get()));
    Logger::Info("AdminManager initialized");
    Logger::Trace("[AdminManager::AdminManager] Exit");
}

AdminManager::~AdminManager()
{
    Logger::Trace("[AdminManager::~AdminManager] Entry");
    Logger::Trace("[AdminManager::~AdminManager] Exit");
}

void AdminManager::Initialize()
{
    Logger::Trace("[AdminManager::Initialize] Entry");
    Logger::Info("AdminManager: Loading admin and ban lists");
    LoadAdminList();
    // Persisted bans must be loaded at startup or IsBanned() silently lets
    // previously-banned SteamIDs reconnect across restarts.
    LoadBanList();
    Logger::Trace("[AdminManager::Initialize] Exit");
}

void AdminManager::Shutdown()
{
    Logger::Trace("[AdminManager::Shutdown] Entry");
    Logger::Info("AdminManager: Shutting down");
    Logger::Trace("[AdminManager::Shutdown] Exit");
}

void AdminManager::LoadAdminList()
{
    Logger::Trace("[AdminManager::LoadAdminList] Entry");
    const std::string path = "config/admin_list.txt";
    Logger::Debug("[AdminManager::LoadAdminList] Loading from path='%s'", path.c_str());
    m_admins.clear();
    m_adminLevels.clear();

    std::ifstream infile(path);
    if (!infile.is_open())
    {
        Logger::Warn("AdminManager: Admin list not found at %s, continuing with empty list", path.c_str());
        Logger::Trace("[AdminManager::LoadAdminList] Exit (file not found)");
        return;
    }

    std::string line;
    int lineNum = 0;
    while (std::getline(infile, line))
    {
        lineNum++;
        line = StringUtils::Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        // Format: "<SteamID> [level] [comment...]". A bare SteamID (legacy
        // format) defaults to Admin (level 3) — matching the old behaviour where
        // any listed SteamID was a full admin.
        auto tokens = StringUtils::Split(line, ' ');
        if (tokens.empty()) continue;
        const std::string steamId = StringUtils::Trim(tokens[0]);
        if (steamId.empty()) continue;

        int level = 3; // default Admin
        if (tokens.size() >= 2) {
            if (auto parsed = StringUtils::ToInt(StringUtils::Trim(tokens[1]))) {
                level = *parsed;
                if (level < 0) level = 0;
                if (level > 5) level = 5;
            }
        }

        m_admins.push_back(steamId);
        m_adminLevels[steamId] = level;
        Logger::Trace("[AdminManager::LoadAdminList] Added '%s' level %d (line %d)", steamId.c_str(), level, lineNum);
    }

    infile.close();
    Logger::Info("AdminManager: Loaded %zu admins", m_admins.size());
    Logger::Trace("[AdminManager::LoadAdminList] Exit");
}

bool AdminManager::IsAdmin(const std::string& steamId) const
{
    bool result = std::find(m_admins.begin(), m_admins.end(), steamId) != m_admins.end();
    Logger::Debug("[AdminManager::IsAdmin] steamId='%s' isAdmin=%d", steamId.c_str(), result);
    return result;
}

int AdminManager::GetPermissionLevel(const std::string& steamId) const
{
    auto it = m_adminLevels.find(steamId);
    int level = (it != m_adminLevels.end()) ? it->second : 0;
    Logger::Debug("[AdminManager::GetPermissionLevel] steamId='%s' level=%d", steamId.c_str(), level);
    return level;
}

std::vector<std::pair<std::string, int>> AdminManager::GetAdminList() const
{
    std::vector<std::pair<std::string, int>> out;
    out.reserve(m_adminLevels.size());
    for (const auto& [id, level] : m_adminLevels) out.emplace_back(id, level);
    return out;
}

bool AdminManager::KickPlayer(uint32_t adminClientId, const std::string& targetSteamId)
{
    Logger::Trace("[AdminManager::KickPlayer] Entry: adminClientId=%u, targetSteamId='%s'", adminClientId, targetSteamId.c_str());
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId == INVALID_CLIENT_ID)
    {
        Logger::Warn("[AdminManager::KickPlayer] Player not found: '%s'", targetSteamId.c_str());
        if (auto c = m_server->GetClientConnection(adminClientId)) c->SendChatMessage("Player not found: " + targetSteamId);
        return false;
    }

    if (auto c = m_server->GetClientConnection(targetId)) c->MarkDisconnected();
    m_server->BroadcastChatMessage("Player " + targetSteamId + " kicked by admin.");
    Logger::Info("[AdminManager::KickPlayer] Player '%s' (client %u) kicked by admin (client %u)", targetSteamId.c_str(), targetId, adminClientId);
    return true;
}

bool AdminManager::BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes)
{
    Logger::Trace("[AdminManager::BanPlayer] Entry: adminClientId=%u, targetSteamId='%s', durationMinutes=%d", adminClientId, targetSteamId.c_str(), durationMinutes);
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId != INVALID_CLIENT_ID)
    {
        if (auto c = m_server->GetClientConnection(targetId)) c->MarkDisconnected();
    }

    const auto expires = std::chrono::system_clock::now() + std::chrono::minutes(durationMinutes);
    m_bans[targetSteamId] = expires;
    SaveBanList();

    m_server->BroadcastChatMessage("Player " + targetSteamId + " banned for " + std::to_string(durationMinutes) + " minutes by admin.");
    Logger::Info("[AdminManager::BanPlayer] Player '%s' banned for %d minutes by admin (client %u)", targetSteamId.c_str(), durationMinutes, adminClientId);
    return true;
}

bool AdminManager::Unban(const std::string& targetSteamId)
{
    Logger::Trace("[AdminManager::Unban] Entry: targetSteamId='%s'", targetSteamId.c_str());
    auto it = m_bans.find(targetSteamId);
    if (it == m_bans.end())
    {
        Logger::Debug("[AdminManager::Unban] No ban entry for '%s'", targetSteamId.c_str());
        return false;
    }
    m_bans.erase(it);
    SaveBanList();
    Logger::Info("[AdminManager::Unban] Ban removed for '%s'", targetSteamId.c_str());
    return true;
}

void AdminManager::SaveBanList() const
{
    Logger::Trace("[AdminManager::SaveBanList] Entry");
    const std::string path = "config/ban_list.txt";
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Error("AdminManager: Unable to write ban list to %s", path.c_str());
        return;
    }

    for (const auto& [steamId, expiry] : m_bans)
    {
        auto timeT = std::chrono::system_clock::to_time_t(expiry);
        file << steamId << " " << timeT << "\n";
    }
    file.close();
    Logger::Info("AdminManager: Ban list saved (%zu entries)", m_bans.size());
}

void AdminManager::LoadBanList()
{
    Logger::Trace("[AdminManager::LoadBanList] Entry");
    const std::string path = "config/ban_list.txt";
    m_bans.clear();

    std::ifstream file(path);
    if (!file.is_open())
    {
        Logger::Warn("AdminManager: Ban list not found at %s", path.c_str());
        return;
    }

    std::string steamId;
    std::time_t expiryTime;
    while (file >> steamId >> expiryTime)
    {
        m_bans[steamId] = std::chrono::system_clock::from_time_t(expiryTime);
    }
    file.close();
    Logger::Info("AdminManager: Loaded %zu ban entries", m_bans.size());
}

bool AdminManager::IsBanned(const std::string& steamId) const
{
    auto it = m_bans.find(steamId);
    if (it == m_bans.end())
        return false;
    if (std::chrono::system_clock::now() > it->second)
        return false; // expired
    return true;
}

std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> AdminManager::GetActiveBans() const
{
    std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> out;
    const auto now = std::chrono::system_clock::now();
    for (const auto& [steamId, expiry] : m_bans)
    {
        if (now <= expiry) out.emplace_back(steamId, expiry);
    }
    return out;
}
