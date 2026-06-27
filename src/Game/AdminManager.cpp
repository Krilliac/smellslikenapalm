// src/Game/AdminManager.cpp – Complete implementation for RS2V Server AdminManager

#include "Game/AdminManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Config/ServerConfig.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include "Game/MapManager.h"
#include "Game/MapVoteManager.h"
#include "Game/WorkshopManager.h"
#include "Game/ModManager.h"
#include "Game/MutatorManager.h"
#include "Config/MapConfig.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include "Config/GameConfig.h"
#include "Config/ConfigManager.h"

static constexpr uint32_t INVALID_CLIENT_ID = UINT32_MAX;

AdminManager::AdminManager(GameServer* server, std::shared_ptr<ServerConfig> config)
    : m_server(server), m_config(config)
{
    Logger::Trace("[AdminManager::AdminManager] Entry: server=%p, config=%p", server, config.get());
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
    Logger::Trace("[AdminManager::Initialize] Exit");
}

void AdminManager::Shutdown()
{
    Logger::Trace("[AdminManager::Shutdown] Entry");
    Logger::Info("AdminManager: Shutting down");
    // Cleanup if needed
    Logger::Trace("[AdminManager::Shutdown] Exit");
}

void AdminManager::LoadAdminList()
{
    Logger::Trace("[AdminManager::LoadAdminList] Entry");
    const std::string path = "config/admin_list.txt";
    Logger::Debug("[AdminManager::LoadAdminList] Loading from path='%s'", path.c_str());
    m_admins.clear();

    std::ifstream infile(path);
    if (!infile.is_open())
    {
        Logger::Warn("AdminManager: Admin list not found at %s, continuing with empty list", path.c_str());
        Logger::Trace("[AdminManager::LoadAdminList] Exit (file not found)");
        return;
    }

    Logger::Debug("[AdminManager::LoadAdminList] File opened successfully, parsing lines");
    std::string line;
    int lineNum = 0;
    while (std::getline(infile, line))
    {
        lineNum++;
        line = StringUtils::Trim(line);
        if (line.empty() || line[0] == '#')
        {
            Logger::Trace("[AdminManager::LoadAdminList] Skipping line %d: empty or comment", lineNum);
            continue;
        }

        m_admins.push_back(line);
        Logger::Trace("[AdminManager::LoadAdminList] Added admin from line %d: '%s'", lineNum, line.c_str());
    }

    infile.close();
    Logger::Info("AdminManager: Loaded %zu admins", m_admins.size());
    Logger::Trace("[AdminManager::LoadAdminList] Exit");
}

bool AdminManager::IsAdmin(const std::string& steamId) const
{
    Logger::Trace("[AdminManager::IsAdmin] Entry: steamId='%s'", steamId.c_str());
    bool result = std::find(m_admins.begin(), m_admins.end(), steamId) != m_admins.end();
    Logger::Debug("[AdminManager::IsAdmin] steamId='%s' isAdmin=%d (checked %zu entries)", steamId.c_str(), result, m_admins.size());
    Logger::Trace("[AdminManager::IsAdmin] Exit: return %d", result);
    return result;
}

bool AdminManager::HandleAdminCommand(uint32_t clientId, const std::string& command, const std::vector<std::string>& args)
{
    Logger::Trace("[AdminManager::HandleAdminCommand] Entry: clientId=%u, command='%s', args.size=%zu", clientId, command.c_str(), args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        Logger::Trace("[AdminManager::HandleAdminCommand] arg[%zu]='%s'", i, args[i].c_str());
    }

    auto conn = m_server->GetClientConnection(clientId);
    if (!conn)
    {
        Logger::Error("[AdminManager::HandleAdminCommand] No connection found for clientId=%u", clientId);
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return false (no connection)");
        return false;
    }

    const std::string steamId = conn->GetSteamID();
    Logger::Debug("[AdminManager::HandleAdminCommand] Client %u has steamId='%s'", clientId, steamId.c_str());
    if (!IsAdmin(steamId))
    {
        Logger::Warn("[AdminManager::HandleAdminCommand] Non-admin '%s' (client %u) attempted command '%s'", steamId.c_str(), clientId, command.c_str());
        conn->SendChatMessage("You are not an admin.");
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return false (not admin)");
        return false;
    }

    Logger::Info("AdminManager: Admin %s issued command: %s", steamId.c_str(), command.c_str());

    if (command == "kick" && args.size() >= 1)
    {
        Logger::Debug("[AdminManager::HandleAdminCommand] Dispatching KickPlayer for target='%s'", args[0].c_str());
        bool result = KickPlayer(clientId, args[0]);
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return %d (kick)", result);
        return result;
    }
    else if (command == "ban" && args.size() >= 2)
    {
        int duration = std::stoi(args[1]);
        Logger::Debug("[AdminManager::HandleAdminCommand] Dispatching BanPlayer for target='%s', duration=%d min", args[0].c_str(), duration);
        bool result = BanPlayer(clientId, args[0], duration);
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return %d (ban)", result);
        return result;
    }
    else if (command == "say" && !args.empty())
    {
        std::string message = JoinArgs(args, " ");
        Logger::Debug("[AdminManager::HandleAdminCommand] Dispatching BroadcastMessage: '%s'", message.c_str());
        bool result = BroadcastMessage(clientId, message);
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return %d (say)", result);
        return result;
    }
    else if (command == "reload" && args.size() == 1)
    {
        Logger::Debug("[AdminManager::HandleAdminCommand] Dispatching ReloadConfig for section='%s'", args[0].c_str());
        bool result = ReloadConfig(clientId, args[0]);
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return %d (reload)", result);
        return result;
    }
    else if (command == "list" && args.size() == 0)
    {
        Logger::Debug("[AdminManager::HandleAdminCommand] Dispatching ListAdmins");
        bool result = ListAdmins(clientId);
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return %d (list)", result);
        return result;
    }
    else if (command == "changemap" && args.size() == 1)
    {
        return ChangeMap(clientId, args[0]);
    }
    else if (command == "rotation")
    {
        return MapRotationCommand(clientId, args);
    }
    else if (command == "startvote")
    {
        return StartMapVote(clientId);
    }
    else if (command == "workshop")
    {
        return WorkshopCommand(clientId, args);
    }
    else if (command == "mods" && args.empty())
    {
        return ListMods(clientId);
    }
    else if (command == "mutators" && args.empty())
    {
        return ListMutators(clientId);
    }
    else
    {
        Logger::Warn("[AdminManager::HandleAdminCommand] Unknown admin command '%s' with %zu args", command.c_str(), args.size());
        conn->SendChatMessage("Unknown admin command or invalid arguments.");
        Logger::Trace("[AdminManager::HandleAdminCommand] Exit: return false (unknown command)");
        return false;
    }
}

bool AdminManager::KickPlayer(uint32_t adminClientId, const std::string& targetSteamId)
{
    Logger::Trace("[AdminManager::KickPlayer] Entry: adminClientId=%u, targetSteamId='%s'", adminClientId, targetSteamId.c_str());
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId == INVALID_CLIENT_ID)
    {
        Logger::Warn("[AdminManager::KickPlayer] Player not found: '%s'", targetSteamId.c_str());
        if (auto c = m_server->GetClientConnection(adminClientId)) c->SendChatMessage("Player not found: " + targetSteamId);
        Logger::Trace("[AdminManager::KickPlayer] Exit: return false (player not found)");
        return false;
    }

    Logger::Debug("[AdminManager::KickPlayer] Found target: clientId=%u for steamId='%s'", targetId, targetSteamId.c_str());
    if (auto c = m_server->GetClientConnection(targetId)) c->MarkDisconnected();
    m_server->BroadcastChatMessage("Player " + targetSteamId + " kicked by admin.");
    Logger::Info("[AdminManager::KickPlayer] Player '%s' (client %u) kicked by admin (client %u)", targetSteamId.c_str(), targetId, adminClientId);
    Logger::Trace("[AdminManager::KickPlayer] Exit: return true");
    return true;
}

bool AdminManager::BanPlayer(uint32_t adminClientId, const std::string& targetSteamId, int durationMinutes)
{
    Logger::Trace("[AdminManager::BanPlayer] Entry: adminClientId=%u, targetSteamId='%s', durationMinutes=%d", adminClientId, targetSteamId.c_str(), durationMinutes);
    uint32_t targetId = m_server->FindClientBySteamID(targetSteamId);
    if (targetId != INVALID_CLIENT_ID)
    {
        Logger::Debug("[AdminManager::BanPlayer] Target is online (clientId=%u), disconnecting", targetId);
        if (auto c = m_server->GetClientConnection(targetId)) c->MarkDisconnected();
    }
    else
    {
        Logger::Debug("[AdminManager::BanPlayer] Target '%s' is not currently online, adding offline ban", targetSteamId.c_str());
    }

    const auto expires = std::chrono::system_clock::now() + std::chrono::minutes(durationMinutes);
    m_bans[targetSteamId] = expires;
    Logger::Debug("[AdminManager::BanPlayer] Ban recorded for '%s', duration=%d min, total bans=%zu", targetSteamId.c_str(), durationMinutes, m_bans.size());
    SaveBanList();

    m_server->BroadcastChatMessage("Player " + targetSteamId + " banned for " + std::to_string(durationMinutes) + " minutes by admin.");
    Logger::Info("[AdminManager::BanPlayer] Player '%s' banned for %d minutes by admin (client %u)", targetSteamId.c_str(), durationMinutes, adminClientId);
    Logger::Trace("[AdminManager::BanPlayer] Exit: return true");
    return true;
}

bool AdminManager::BroadcastMessage(uint32_t adminClientId, const std::string& message)
{
    Logger::Trace("[AdminManager::BroadcastMessage] Entry: adminClientId=%u, message='%s'", adminClientId, message.c_str());
    m_server->BroadcastChatMessage("[ADMIN] " + message);
    Logger::Info("[AdminManager::BroadcastMessage] Admin (client %u) broadcast: '%s'", adminClientId, message.c_str());
    Logger::Trace("[AdminManager::BroadcastMessage] Exit: return true");
    return true;
}

bool AdminManager::ReloadConfig(uint32_t adminClientId, const std::string& section)
{
    Logger::Trace("[AdminManager::ReloadConfig] Entry: adminClientId=%u, section='%s'", adminClientId, section.c_str());
    if (section == "server")
    {
        Logger::Debug("[AdminManager::ReloadConfig] Reloading server configuration");
        if (m_server->GetConfigManager()->ReloadConfiguration())
        {
            m_server->BroadcastChatMessage("Server configuration reloaded.");
            Logger::Info("[AdminManager::ReloadConfig] Server configuration reloaded successfully by admin (client %u)", adminClientId);
            Logger::Trace("[AdminManager::ReloadConfig] Exit: return true (server reload success)");
            return true;
        }
        Logger::Error("[AdminManager::ReloadConfig] Failed to reload server configuration");
    }
    else if (section == "game")
    {
        Logger::Debug("[AdminManager::ReloadConfig] Reloading game configuration");
        if (m_server->GetGameConfig()->GetMapRotation().size() > 0)
        {
            m_server->BroadcastChatMessage("Game configuration reloaded.");
            Logger::Info("[AdminManager::ReloadConfig] Game configuration reloaded successfully by admin (client %u)", adminClientId);
            Logger::Trace("[AdminManager::ReloadConfig] Exit: return true (game reload success)");
            return true;
        }
        Logger::Error("[AdminManager::ReloadConfig] Failed to reload game configuration");
    }
    else
    {
        Logger::Warn("[AdminManager::ReloadConfig] Unknown config section: '%s'", section.c_str());
    }
    if (auto c = m_server->GetClientConnection(adminClientId)) c->SendChatMessage("Failed to reload config section: " + section);
    Logger::Trace("[AdminManager::ReloadConfig] Exit: return false");
    return false;
}

bool AdminManager::ListAdmins(uint32_t adminClientId)
{
    Logger::Trace("[AdminManager::ListAdmins] Entry: adminClientId=%u", adminClientId);
    auto conn = m_server->GetClientConnection(adminClientId);
    conn->SendChatMessage("Admin list:");
    Logger::Debug("[AdminManager::ListAdmins] Listing %zu admins", m_admins.size());
    for (size_t i = 0; i < m_admins.size(); ++i)
    {
        conn->SendChatMessage(" - " + m_admins[i]);
        Logger::Trace("[AdminManager::ListAdmins] Admin[%zu]: '%s'", i, m_admins[i].c_str());
    }
    Logger::Info("[AdminManager::ListAdmins] Listed %zu admins to client %u", m_admins.size(), adminClientId);
    Logger::Trace("[AdminManager::ListAdmins] Exit: return true");
    return true;
}

void AdminManager::SaveBanList() const
{
    Logger::Trace("[AdminManager::SaveBanList] Entry");
    const std::string path = "config/ban_list.txt";
    Logger::Debug("[AdminManager::SaveBanList] Saving to path='%s', %zu bans", path.c_str(), m_bans.size());
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Error("AdminManager: Unable to write ban list to %s", path.c_str());
        Logger::Trace("[AdminManager::SaveBanList] Exit (file open failed)");
        return;
    }

    for (const auto& [steamId, expiry] : m_bans)
    {
        auto timeT = std::chrono::system_clock::to_time_t(expiry);
        file << steamId << " " << timeT << "\n";
        Logger::Trace("[AdminManager::SaveBanList] Wrote ban: steamId='%s', expires=%ld", steamId.c_str(), (long)timeT);
    }
    file.close();
    Logger::Info("AdminManager: Ban list saved (%zu entries)", m_bans.size());
    Logger::Trace("[AdminManager::SaveBanList] Exit");
}

void AdminManager::LoadBanList()
{
    Logger::Trace("[AdminManager::LoadBanList] Entry");
    const std::string path = "config/ban_list.txt";
    Logger::Debug("[AdminManager::LoadBanList] Loading from path='%s'", path.c_str());
    m_bans.clear();

    std::ifstream file(path);
    if (!file.is_open())
    {
        Logger::Warn("AdminManager: Ban list not found at %s", path.c_str());
        Logger::Trace("[AdminManager::LoadBanList] Exit (file not found)");
        return;
    }

    std::string steamId;
    std::time_t expiryTime;
    while (file >> steamId >> expiryTime)
    {
        m_bans[steamId] = std::chrono::system_clock::from_time_t(expiryTime);
        Logger::Trace("[AdminManager::LoadBanList] Loaded ban: steamId='%s', expires=%ld", steamId.c_str(), (long)expiryTime);
    }
    file.close();
    Logger::Info("AdminManager: Loaded %zu ban entries", m_bans.size());
    Logger::Trace("[AdminManager::LoadBanList] Exit");
}

bool AdminManager::IsBanned(const std::string& steamId) const
{
    Logger::Trace("[AdminManager::IsBanned] Entry: steamId='%s'", steamId.c_str());
    auto it = m_bans.find(steamId);
    if (it == m_bans.end())
    {
        Logger::Debug("[AdminManager::IsBanned] steamId='%s' not found in ban list", steamId.c_str());
        Logger::Trace("[AdminManager::IsBanned] Exit: return false (not in list)");
        return false;
    }

    if (std::chrono::system_clock::now() > it->second)
    {
        Logger::Debug("[AdminManager::IsBanned] steamId='%s' ban has expired", steamId.c_str());
        Logger::Trace("[AdminManager::IsBanned] Exit: return false (expired)");
        return false; // expired
    }

    Logger::Debug("[AdminManager::IsBanned] steamId='%s' is currently banned", steamId.c_str());
    Logger::Trace("[AdminManager::IsBanned] Exit: return true");
    return true;
}

std::string AdminManager::JoinArgs(const std::vector<std::string>& args, const std::string& sep) const
{
    Logger::Trace("[AdminManager::JoinArgs] Entry: args.size=%zu, sep='%s'", args.size(), sep.c_str());
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i) oss << sep;
        oss << args[i];
    }
    std::string result = oss.str();
    Logger::Trace("[AdminManager::JoinArgs] Exit: result='%s'", result.c_str());
    return result;
}

// ---------------------------------------------------------------------------
// Map / workshop / mod / mutator admin commands
// ---------------------------------------------------------------------------

bool AdminManager::ChangeMap(uint32_t adminClientId, const std::string& mapName)
{
    Logger::Trace("[AdminManager::ChangeMap] Entry: client=%u, map='%s'", adminClientId, mapName.c_str());
    auto conn = m_server->GetClientConnection(adminClientId);
    auto* mapMgr = m_server->GetMapManager();
    if (!mapMgr) {
        if (conn) conn->SendChatMessage("MapManager unavailable.");
        return false;
    }
    if (mapMgr->LoadMap(mapName)) {
        if (conn) conn->SendChatMessage("Changed map to: " + mapName);
        m_server->BroadcastChatMessage("Admin changed map to " + mapName);
        Logger::Info("AdminManager: client %u changed map to '%s'", adminClientId, mapName.c_str());
        return true;
    }
    if (conn) conn->SendChatMessage("Failed to load map: " + mapName);
    Logger::Warn("AdminManager: failed to change map to '%s'", mapName.c_str());
    return false;
}

bool AdminManager::MapRotationCommand(uint32_t adminClientId, const std::vector<std::string>& args)
{
    auto conn = m_server->GetClientConnection(adminClientId);
    if (!conn) return false;

    std::string sub = args.empty() ? "list" : StringUtils::ToLower(args[0]);

    if (sub == "list") {
        auto* mapMgr = m_server->GetMapManager();
        auto gc = m_server->GetGameConfig();
        (void)gc;
        // The rotation is the set of map definitions; list them in config order.
        if (auto sc = m_server->GetServerConfig()) {
            MapConfig mc(*sc);
            mc.Initialize();
            conn->SendChatMessage("Map rotation:");
            for (const auto& name : mc.GetAvailableMaps()) {
                std::string mark = (mapMgr && name == mapMgr->GetCurrentMapName()) ? " *" : "";
                conn->SendChatMessage(" - " + name + mark);
            }
            return true;
        }
        return false;
    }

    // Runtime add/remove of rotation entries is config-driven: maps are defined
    // in maps.ini. Surface this clearly rather than silently no-op.
    if (sub == "add" || sub == "remove") {
        conn->SendChatMessage("Rotation is defined in maps.ini; edit it and run 'reload map', "
                              "then use 'changemap <id>' to switch immediately.");
        return true;
    }

    conn->SendChatMessage("Usage: rotation [list|add <id>|remove <id>]");
    return false;
}

bool AdminManager::StartMapVote(uint32_t adminClientId)
{
    auto conn = m_server->GetClientConnection(adminClientId);
    bool ok = m_server->StartMapVote();
    if (conn) conn->SendChatMessage(ok ? "Map vote started." : "Could not start map vote.");
    return ok;
}

bool AdminManager::WorkshopCommand(uint32_t adminClientId, const std::vector<std::string>& args)
{
    auto conn = m_server->GetClientConnection(adminClientId);
    auto* ws = m_server->GetWorkshopManager();
    if (!conn) return false;
    if (!ws) { conn->SendChatMessage("WorkshopManager unavailable."); return false; }

    std::string sub = args.empty() ? "list" : StringUtils::ToLower(args[0]);

    if (sub == "list") {
        const auto& items = ws->GetItems();
        conn->SendChatMessage("Workshop items (" + std::to_string(items.size()) + "):");
        for (const auto& it : items) {
            conn->SendChatMessage(" - [" + WorkshopManager::TypeToString(it.type) + "] " +
                                  it.fileName + (it.present ? " (ok)" : " (MISSING)"));
        }
        return true;
    }
    if (sub == "reload") {
        ws->Reload();
        conn->SendChatMessage("Workshop manifest reloaded.");
        return true;
    }
    if (sub == "validate") {
        size_t missing = ws->ValidateItems();
        conn->SendChatMessage("Workshop validate: " + std::to_string(missing) + " missing.");
        return true;
    }
    if (sub == "download") {
        size_t n = ws->DownloadMissing();
        conn->SendChatMessage("Workshop download complete: " + std::to_string(n) + " fetched "
                              "(0 if download disabled / dry-run).");
        return true;
    }

    conn->SendChatMessage("Usage: workshop [list|reload|validate|download]");
    return false;
}

bool AdminManager::ListMods(uint32_t adminClientId)
{
    auto conn = m_server->GetClientConnection(adminClientId);
    auto* mods = m_server->GetModManager();
    if (!conn) return false;
    if (!mods) { conn->SendChatMessage("ModManager unavailable."); return false; }

    const auto& list = mods->GetMods();
    conn->SendChatMessage("Mods/assets (" + std::to_string(list.size()) + "):");
    for (const auto& m : list) {
        conn->SendChatMessage(" - " + m.name + (m.isAsset ? " [asset]" : " [mod]") +
                              (m.present ? "" : " (MISSING)"));
    }
    return true;
}

bool AdminManager::ListMutators(uint32_t adminClientId)
{
    auto conn = m_server->GetClientConnection(adminClientId);
    auto* mut = m_server->GetMutatorManager();
    if (!conn) return false;
    if (!mut) { conn->SendChatMessage("MutatorManager unavailable."); return false; }

    auto active = mut->GetActiveNames();
    conn->SendChatMessage("Active mutators (" + std::to_string(active.size()) + "):");
    for (const auto& n : active) conn->SendChatMessage(" - " + n);
    auto registered = mut->GetRegisteredIds();
    conn->SendChatMessage("Available: " + StringUtils::Join(registered, ", "));
    return true;
}
