#include "Security/SecurityManager.h"
#include "Utils/Logger.h"
#include <chrono>

SecurityManager::SecurityManager(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
    , m_auth(m_config)
    , m_banManager(m_config)
    , m_eac(m_config)
{
    Logger::Trace("[SecurityManager::SecurityManager] Constructor called, config is %s",
                  m_config ? "non-null" : "null");
}

SecurityManager::~SecurityManager() {
    Logger::Trace("[SecurityManager::~SecurityManager] Destructor called");
    Shutdown();
    Logger::Trace("[SecurityManager::~SecurityManager] Destructor completed");
}

bool SecurityManager::Initialize() {
    Logger::Trace("[SecurityManager::Initialize] Entry");
    Logger::Info("[SecurityManager::Initialize] Beginning SecurityManager initialization");
    // Authentication has no separate Initialize; it's ready after construction
    Logger::Debug("[SecurityManager::Initialize] Authentication module ready (no separate init required)");

    Logger::Debug("[SecurityManager::Initialize] Loading ban list...");
    if (!m_banManager.LoadBans()) {
        Logger::Warn("SecurityManager: could not load bans (proceeding)");
        Logger::Debug("[SecurityManager::Initialize] Ban list load failed, continuing without pre-existing bans");
    } else {
        Logger::Debug("[SecurityManager::Initialize] Ban list loaded successfully");
    }

    Logger::Debug("[SecurityManager::Initialize] Initializing EAC AntiCheat subsystem...");
    if (!m_eac.Initialize()) {
        Logger::Error("SecurityManager: EAC AntiCheat init failed");
        Logger::Error("[SecurityManager::Initialize] Failed to initialize EnhancedEACAntiCheat - aborting SecurityManager init");
        Logger::Trace("[SecurityManager::Initialize] Exit, returning false");
        return false;
    }
    Logger::Debug("[SecurityManager::Initialize] EAC AntiCheat subsystem initialized successfully");

    m_eac.SetReportCallback([this](const EnhancedEACReport& rpt){
        HandleEACReport(rpt);
    });
    Logger::Debug("[SecurityManager::Initialize] EAC report callback registered");

    Logger::Info("SecurityManager initialized");
    Logger::Info("[SecurityManager::Initialize] All security subsystems initialized successfully");
    Logger::Trace("[SecurityManager::Initialize] Exit, returning true");
    return true;
}

void SecurityManager::Shutdown() {
    Logger::Trace("[SecurityManager::Shutdown] Entry");
    Logger::Info("[SecurityManager::Shutdown] Beginning SecurityManager shutdown");

    Logger::Debug("[SecurityManager::Shutdown] Shutting down EAC AntiCheat subsystem...");
    m_eac.Shutdown();
    Logger::Debug("[SecurityManager::Shutdown] EAC AntiCheat shutdown complete");

    Logger::Debug("[SecurityManager::Shutdown] Saving ban list...");
    m_banManager.SaveBans();
    Logger::Debug("[SecurityManager::Shutdown] Ban list saved");

    Logger::Debug("[SecurityManager::Shutdown] Resetting authentication module...");
    m_auth = Authentication(nullptr);
    Logger::Debug("[SecurityManager::Shutdown] Authentication module reset");

    Logger::Info("SecurityManager shutdown complete");
    Logger::Trace("[SecurityManager::Shutdown] Exit");
}

void SecurityManager::OnClientConnect(std::shared_ptr<ClientConnection> conn) {
    Logger::Trace("[SecurityManager::OnClientConnect] Entry, conn=%p", static_cast<void*>(conn.get()));
    uint32_t id = conn->GetClientId();
    Logger::Debug("[SecurityManager::OnClientConnect] Client ID resolved to %u", id);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connections[id] = conn;
    Logger::Debug("[SecurityManager::OnClientConnect] Client %u added to connections map, total connections=%zu",
                  id, m_connections.size());

    // Check ban
    const std::string steamId = conn->GetSteamID();
    Logger::Debug("[SecurityManager::OnClientConnect] Checking ban status for SteamID '%s' (client %u)",
                  steamId.c_str(), id);
    if (m_banManager.IsBanned(steamId)) {
        Logger::Warn("SecurityManager: connection from banned SteamID %s", steamId.c_str());
        Logger::Info("[SecurityManager::OnClientConnect] Rejecting connection from banned SteamID '%s' (client %u)",
                     steamId.c_str(), id);
        DisconnectClient(id, "Banned");
        Logger::Trace("[SecurityManager::OnClientConnect] Exit (banned client disconnected)");
        return;
    }
    Logger::Debug("[SecurityManager::OnClientConnect] SteamID '%s' is not banned, proceeding with authentication",
                  steamId.c_str());

    // Begin authentication handshake
    Logger::Info("[SecurityManager::OnClientConnect] Initiating authentication handshake for client %u (SteamID '%s')",
                 id, steamId.c_str());
    m_auth.SendChallenge(conn);
    Logger::Debug("[SecurityManager::OnClientConnect] Authentication challenge sent to client %u", id);

    // Schedule EAC validation (after auth)
    Logger::Debug("[SecurityManager::OnClientConnect] Scheduling EAC validation for client %u, IP='%s'",
                  id, conn->GetIP().c_str());
    m_eac.ValidateClient(conn, /*processHandle=*/0, conn->GetIP());
    Logger::Info("[SecurityManager::OnClientConnect] Client %u connection processing complete - auth and EAC validation initiated",
                 id);
    Logger::Trace("[SecurityManager::OnClientConnect] Exit");
}

void SecurityManager::OnClientDisconnect(uint32_t clientId) {
    Logger::Trace("[SecurityManager::OnClientDisconnect] Entry, clientId=%u", clientId);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto erased = m_connections.erase(clientId);
    if (erased > 0) {
        Logger::Info("[SecurityManager::OnClientDisconnect] Client %u disconnected, removed from connections (remaining=%zu)",
                     clientId, m_connections.size());
    } else {
        Logger::Debug("[SecurityManager::OnClientDisconnect] Client %u was not in connections map", clientId);
    }
    Logger::Trace("[SecurityManager::OnClientDisconnect] Exit");
}

bool SecurityManager::ValidatePacket(uint32_t clientId, const std::vector<uint8_t>& rawData) {
    Logger::Trace("[SecurityManager::ValidatePacket] Entry, clientId=%u, rawData size=%zu bytes",
                  clientId, rawData.size());
    // Drop if client address blocked
    auto it = m_connections.find(clientId);
    if (it == m_connections.end()) {
        Logger::Debug("[SecurityManager::ValidatePacket] Client %u not found in connections map, rejecting packet",
                      clientId);
        Logger::Trace("[SecurityManager::ValidatePacket] Exit, returning false (unknown client)");
        return false;
    }
    ClientAddress addr{ it->second->GetIP(), it->second->GetPort() };
    Logger::Debug("[SecurityManager::ValidatePacket] Checking if address %s:%u is blocked for client %u",
                  addr.ip.c_str(), addr.port, clientId);
    if (m_blocker.IsBlocked(addr)) {
        Logger::Warn("SecurityManager: packet from blocked %s:%u dropped", addr.ip.c_str(), addr.port);
        Logger::Info("[SecurityManager::ValidatePacket] Packet from blocked address %s:%u (client %u) was dropped (%zu bytes)",
                     addr.ip.c_str(), addr.port, clientId, rawData.size());
        Logger::Trace("[SecurityManager::ValidatePacket] Exit, returning false (blocked)");
        return false;
    }
    Logger::Debug("[SecurityManager::ValidatePacket] Packet from client %u (%s:%u) passed validation (%zu bytes)",
                  clientId, addr.ip.c_str(), addr.port, rawData.size());
    Logger::Trace("[SecurityManager::ValidatePacket] Exit, returning true");
    return true;
}

void SecurityManager::Update() {
    Logger::Trace("[SecurityManager::Update] Entry");
    Logger::Debug("[SecurityManager::Update] Updating EAC AntiCheat subsystem");
    m_eac.Update();
    Logger::Debug("[SecurityManager::Update] Updating NetworkBlocker");
    m_blocker.Update();
    Logger::Debug("[SecurityManager::Update] Cleaning up expired bans");
    m_banManager.CleanupExpired();
    // Additional periodic security checks can go here
    Logger::Trace("[SecurityManager::Update] Exit");
}

void SecurityManager::BanClient(const std::string& steamId,
                                BanType type,
                                std::chrono::seconds duration,
                                const std::string& reason)
{
    Logger::Trace("[SecurityManager::BanClient] Entry, steamId='%s', type=%s, duration=%lld seconds, reason='%s'",
                  steamId.c_str(),
                  type == BanType::Permanent ? "Permanent" : "Temporary",
                  static_cast<long long>(duration.count()),
                  reason.c_str());
    Logger::Info("[SecurityManager::BanClient] Banning SteamID '%s' (%s, duration=%lld seconds, reason='%s')",
                 steamId.c_str(),
                 type == BanType::Permanent ? "permanent" : "temporary",
                 static_cast<long long>(duration.count()),
                 reason.c_str());
    m_banManager.AddBan(steamId, type, duration, reason);
    Logger::Debug("[SecurityManager::BanClient] Ban added, now checking for online clients with SteamID '%s'", steamId.c_str());
    // Disconnect any online matching client
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& kv : m_connections) {
        if (kv.second->GetSteamID() == steamId) {
            Logger::Info("[SecurityManager::BanClient] Found online client %u with SteamID '%s', disconnecting",
                         kv.first, steamId.c_str());
            DisconnectClient(kv.first, "Banned: " + reason);
            break;
        }
    }
    Logger::Trace("[SecurityManager::BanClient] Exit");
}

bool SecurityManager::UnbanClient(const std::string& steamId) {
    Logger::Trace("[SecurityManager::UnbanClient] Entry, steamId='%s'", steamId.c_str());
    Logger::Info("[SecurityManager::UnbanClient] Attempting to unban SteamID '%s'", steamId.c_str());
    bool result = m_banManager.RemoveBan(steamId);
    if (result) {
        Logger::Info("[SecurityManager::UnbanClient] Successfully unbanned SteamID '%s'", steamId.c_str());
    } else {
        Logger::Debug("[SecurityManager::UnbanClient] SteamID '%s' was not found in ban list", steamId.c_str());
    }
    Logger::Trace("[SecurityManager::UnbanClient] Exit, returning %s", result ? "true" : "false");
    return result;
}

bool SecurityManager::IsBanned(const std::string& steamId) const {
    Logger::Trace("[SecurityManager::IsBanned] Entry, steamId='%s'", steamId.c_str());
    bool result = m_banManager.IsBanned(steamId);
    Logger::Debug("[SecurityManager::IsBanned] Ban check for SteamID '%s': %s",
                  steamId.c_str(), result ? "BANNED" : "not banned");
    Logger::Trace("[SecurityManager::IsBanned] Exit, returning %s", result ? "true" : "false");
    return result;
}

void SecurityManager::HandleEACReport(const EnhancedEACReport& report) {
    Logger::Trace("[SecurityManager::HandleEACReport] Entry, clientId=%u, result=%d, details='%s'",
                  report.clientId, int(report.result), report.details.c_str());
    Logger::Info("SecurityManager: EAC report for client %u: %s",
                 report.clientId, report.details.c_str());

    if (report.result != EnhancedEACResult::Clean) {
        Logger::Warn("[SecurityManager::HandleEACReport] EAC detected issue for client %u: result=%d, details='%s'",
                     report.clientId, int(report.result), report.details.c_str());
        Logger::Info("[SecurityManager::HandleEACReport] Taking enforcement action against client %u for cheat detection",
                     report.clientId);
        // Ban or block on cheat detection
        auto it = m_connections.find(report.clientId);
        if (it != m_connections.end()) {
            ClientAddress addr{ it->second->GetIP(), it->second->GetPort() };
            Logger::Debug("[SecurityManager::HandleEACReport] Blocking address %s:%u for 60 seconds due to cheat detection",
                          addr.ip.c_str(), addr.port);
            m_blocker.Block(addr, std::chrono::seconds(60));
            Logger::Debug("[SecurityManager::HandleEACReport] Disconnecting client %u for cheat detection", report.clientId);
            DisconnectClient(report.clientId, "Cheat detected");
        } else {
            Logger::Debug("[SecurityManager::HandleEACReport] Client %u not found in connections map, cannot enforce block",
                          report.clientId);
        }
    } else {
        Logger::Debug("[SecurityManager::HandleEACReport] EAC report clean for client %u, no action needed",
                      report.clientId);
    }
    Logger::Trace("[SecurityManager::HandleEACReport] Exit");
}

void SecurityManager::DisconnectClient(uint32_t clientId, const std::string& reason) {
    Logger::Trace("[SecurityManager::DisconnectClient] Entry, clientId=%u, reason='%s'", clientId, reason.c_str());
    auto it = m_connections.find(clientId);
    if (it != m_connections.end()) {
        Logger::Info("SecurityManager: disconnecting client %u (%s)", clientId, reason.c_str());
        Logger::Debug("[SecurityManager::DisconnectClient] Marking client %u as disconnected (IP='%s', SteamID='%s')",
                      clientId, it->second->GetIP().c_str(), it->second->GetSteamID().c_str());
        it->second->MarkDisconnected();
        m_connections.erase(it);
        Logger::Debug("[SecurityManager::DisconnectClient] Client %u removed from connections map, remaining=%zu",
                      clientId, m_connections.size());
    } else {
        Logger::Debug("[SecurityManager::DisconnectClient] Client %u not found in connections map, nothing to disconnect",
                      clientId);
    }
    Logger::Trace("[SecurityManager::DisconnectClient] Exit");
}
