// src/Game/AdminManager.cpp – admin/ban data store + privileged player operations.

#include "Game/AdminManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Config/ServerConfig.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include <algorithm>
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
    Logger::Info("AdminManager: Loading admin list");
    LoadAdminList();
    // Bans are owned and loaded by the security layer (BanManager); AdminManager
    // no longer keeps a parallel ban list.
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

bool AdminManager::BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes,
                             const std::string& reason)
{
    Logger::Trace("[AdminManager::BanPlayer] Entry: adminClientId=%u, targetSteamId='%s', durationMinutes=%d, reason='%s'",
                  adminClientId, targetSteamId.c_str(), durationMinutes, reason.c_str());

    // GUARD (integer overflow / UB): durationMinutes is operator-supplied. An enormous value
    // (e.g. ~2e9, which still parses as a valid int) overflows when converted to a system_clock
    // time_point in the ban store, yielding a garbage (often past) expiry. Cap at ~100 years -
    // longer than any real ban, far below overflow. Negatives are treated as permanent below.
    constexpr int kMaxBanMinutes = 100 * 365 * 24 * 60;  // ~100 years, fits int and avoids tick overflow
    if (durationMinutes > kMaxBanMinutes) {
        Logger::Warn("[AdminManager::BanPlayer] duration %d min exceeds cap; clamping to %d (~100y)", durationMinutes, kMaxBanMinutes);
        durationMinutes = kMaxBanMinutes;
    }

    // Record the ban in the single authoritative store (security layer). This is the
    // ENFORCED store checked by the PreLogin gate at connect, not merely a one-time kick.
    bool recorded = m_server->BanSteamId(targetSteamId, durationMinutes, reason);
    if (!recorded) {
        Logger::Warn("[AdminManager::BanPlayer] Ban store unavailable; ban not recorded for '%s'", targetSteamId.c_str());
        if (auto c = m_server->GetClientConnection(adminClientId)) c->SendChatMessage("Ban store unavailable.");
        return false;
    }

    // Disconnect the target if currently connected so the ban takes effect now.
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId != INVALID_CLIENT_ID)
    {
        if (auto c = m_server->GetClientConnection(targetId)) c->MarkDisconnected();
    }

    m_server->BroadcastChatMessage("Player " + targetSteamId + " banned for " + std::to_string(durationMinutes) + " minutes by admin.");
    Logger::Info("[AdminManager::BanPlayer] Player '%s' banned for %d minutes by admin (client %u)", targetSteamId.c_str(), durationMinutes, adminClientId);
    return true;
}

bool AdminManager::Unban(const std::string& targetSteamId)
{
    Logger::Trace("[AdminManager::Unban] Entry: targetSteamId='%s'", targetSteamId.c_str());
    bool removed = m_server->UnbanSteamId(targetSteamId);
    Logger::Info("[AdminManager::Unban] Unban for '%s': %s", targetSteamId.c_str(), removed ? "removed" : "no active ban");
    return removed;
}

bool AdminManager::IsBanned(const std::string& steamId) const
{
    return m_server->IsSteamIdBanned(steamId);
}

std::vector<BanRecord> AdminManager::GetActiveBans() const
{
    return m_server->GetActiveBans();
}
