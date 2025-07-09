// src/Game/AdminManager.cpp – Complete implementation for RS2V Server AdminManager

#include "Game/AdminManager.h"
#include "Utils/Logger.h"
#include "Config/ServerConfig.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include <algorithm>
#include <chrono>

AdminManager::AdminManager(GameServer* server, std::shared_ptr<ServerConfig> config)
    : m_server(server), m_config(config)
{
    Logger::Info("AdminManager initialized");
}

AdminManager::~AdminManager() = default;

void AdminManager::Initialize()
{
    Logger::Info("AdminManager: Loading admin list");
    LoadAdminList();
}

void AdminManager::Shutdown()
{
    Logger::Info("AdminManager: Shutting down");
    // Cleanup if needed
}

bool AdminManager::LoadAdminList()
{
    const std::string path = "config/admin_list.txt";
    m_admins.clear();

    std::ifstream infile(path);
    if (!infile.is_open())
    {
        Logger::Warn("AdminManager: Admin list not found at %s, continuing with empty list", path.c_str());
        return false;
    }

    std::string line;
    while (std::getline(infile, line))
    {
        line = Utils::StringUtils::Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        m_admins.push_back(line);
    }

    infile.close();
    Logger::Info("AdminManager: Loaded %zu admins", m_admins.size());
    return true;
}

bool AdminManager::IsAdmin(const std::string& steamId) const
{
    return std::find(m_admins.begin(), m_admins.end(), steamId) != m_admins.end();
}

bool AdminManager::HandleAdminCommand(uint32_t clientId, const std::string& command, const std::vector<std::string>& args)
{
    auto conn = m_server->GetClientConnection(clientId);
    if (!conn)
        return false;

    const std::string steamId = conn->GetSteamID();
    if (!IsAdmin(steamId))
    {
        conn->SendChatMessage("You are not an admin.");
        return false;
    }

    Logger::Info("AdminManager: Admin %s issued command: %s", steamId.c_str(), command.c_str());

    if (command == "kick" && args.size() >= 1)
    {
        return KickPlayer(clientId, args[0]);
    }
    else if (command == "ban" && args.size() >= 2)
    {
        return BanPlayer(clientId, args[0], std::stoi(args[1]));
    }
    else if (command == "say" && !args.empty())
    {
        return BroadcastMessage(clientId, JoinArgs(args, " "));
    }
    else if (command == "reload" && args.size() == 1)
    {
        return ReloadConfig(clientId, args[0]);
    }
    else if (command == "list" && args.size() == 0)
    {
        return ListAdmins(clientId);
    }
    else
    {
        conn->SendChatMessage("Unknown admin command or invalid arguments.");
        return false;
    }
}

bool AdminManager::KickPlayer(uint32_t adminClientId, const std::string& targetSteamId)
{
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId == INVALID_CLIENT_ID)
    {
        m_server->SendPrivateMessage(adminClientId, "Player not found: " + targetSteamId);
        return false;
    }

    m_server->DisconnectClient(targetId, "Kicked by admin.");
    m_server->BroadcastChatMessage("Player " + targetSteamId + " kicked by admin.");
    return true;
}

bool AdminManager::BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes)
{
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId != INVALID_CLIENT_ID)
    {
        m_server->DisconnectClient(targetId, "Banned by admin.");
    }

    const auto expires = std::chrono::system_clock::now() + std::chrono::minutes(durationMinutes);
    m_bans[targetSteamId] = expires;
    SaveBanList();

    m_server->BroadcastChatMessage("Player " + targetSteamId + " banned for " + std::to_string(durationMinutes) + " minutes by admin.");
    return true;
}

bool AdminManager::BroadcastMessage(uint32_t adminClientId, const std::string& message)
{
    m_server->BroadcastChatMessage("[ADMIN] " + message);
    return true;
}

bool AdminManager::ReloadConfig(uint32_t adminClientId, const std::string& section)
{
    if (section == "server")
    {
        if (m_config->Reload())
        {
            m_server->BroadcastChatMessage("Server configuration reloaded.");
            return true;
        }
    }
    else if (section == "game")
    {
        if (m_server->GetGameConfig()->ReloadGameConfiguration())
        {
            m_server->BroadcastChatMessage("Game configuration reloaded.");
            return true;
        }
    }
    m_server->SendPrivateMessage(adminClientId, "Failed to reload config section: " + section);
    return false;
}

bool AdminManager::ListAdmins(uint32_t adminClientId)
{
    auto conn = m_server->GetClientConnection(adminClientId);
    conn->SendChatMessage("Admin list:");
    for (const auto& id : m_admins)
    {
        conn->SendChatMessage(" - " + id);
    }
    return true;
}

void AdminManager::SaveBanList() const
{
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

std::string AdminManager::JoinArgs(const std::vector<std::string>& args, const std::string& sep) const
{
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i) oss << sep;
        oss << args[i];
    }
    return oss.str();
}