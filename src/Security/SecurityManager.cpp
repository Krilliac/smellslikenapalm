#include "Security/SecurityManager.h"
#include "Utils/Logger.h"
#include <chrono>

SecurityManager::SecurityManager(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
    , m_auth(m_config)
    , m_banManager(m_config)
    , m_eac(m_config)
{
}

SecurityManager::~SecurityManager() {
    Shutdown();
}

bool SecurityManager::Initialize() {
    if (!m_auth.Initialize()) {
        Logger::Error("SecurityManager: Authentication init failed");
        return false;
    }
    if (!m_banManager.LoadBans()) {
        Logger::Warn("SecurityManager: could not load bans (proceeding)");
    }
    if (!m_eac.Initialize()) {
        Logger::Error("SecurityManager: EAC AntiCheat init failed");
        return false;
    }
    m_eac.SetReportCallback([this](const EnhancedEACReport& rpt){
        HandleEACReport(rpt);
    });
    Logger::Info("SecurityManager initialized");
    return true;
}

void SecurityManager::Shutdown() {
    m_eac.Shutdown();
    m_banManager.SaveBans();
    m_auth = Authentication(nullptr);
    Logger::Info("SecurityManager shutdown complete");
}

void SecurityManager::OnClientConnect(std::shared_ptr<ClientConnection> conn) {
    uint32_t id = conn->GetClientId();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connections[id] = conn;

    // Check ban
    const std::string steamId = conn->GetSteamID();
    if (m_banManager.IsBanned(steamId)) {
        Logger::Warn("SecurityManager: connection from banned SteamID %s", steamId.c_str());
        DisconnectClient(id, "Banned");
        return;
    }

    // Begin authentication handshake
    m_auth.SendChallenge(conn);

    // Schedule EAC validation (after auth)
    m_eac.ValidateClient(conn, /*processHandle=*/0, conn->GetIP());
}

void SecurityManager::OnClientDisconnect(uint32_t clientId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connections.erase(clientId);
}

bool SecurityManager::ValidatePacket(uint32_t clientId, const std::vector<uint8_t>& rawData) {
    // Drop if client address blocked
    auto it = m_connections.find(clientId);
    if (it == m_connections.end()) return false;
    ClientAddress addr{ it->second->GetIP(), it->second->GetPort() };
    if (m_blocker.IsBlocked(addr)) {
        Logger::Warn("SecurityManager: packet from blocked %s:%u dropped", addr.ip.c_str(), addr.port);
        return false;
    }
    return true;
}

void SecurityManager::Update() {
    m_eac.Update();
    m_blocker.Update();
    m_banManager.CleanupExpired();
    // Additional periodic security checks can go here
}

void SecurityManager::BanClient(const std::string& steamId,
                                BanType type,
                                std::chrono::seconds duration,
                                const std::string& reason)
{
    m_banManager.AddBan(steamId, type, duration, reason);
    // Disconnect any online matching client
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& kv : m_connections) {
        if (kv.second->GetSteamID() == steamId) {
            DisconnectClient(kv.first, "Banned: " + reason);
            break;
        }
    }
}

bool SecurityManager::UnbanClient(const std::string& steamId) {
    return m_banManager.RemoveBan(steamId);
}

bool SecurityManager::IsBanned(const std::string& steamId) const {
    return m_banManager.IsBanned(steamId);
}

void SecurityManager::HandleEACReport(const EnhancedEACReport& report) {
    Logger::Info("SecurityManager: EAC report for client %u: %s",
                 report.clientId, report.details.c_str());

    if (report.result != EnhancedEACResult::Clean) {
        // Ban or block on cheat detection
        auto it = m_connections.find(report.clientId);
        if (it != m_connections.end()) {
            ClientAddress addr{ it->second->GetIP(), it->second->GetPort() };
            m_blocker.Block(addr, std::chrono::seconds(m_config->GetInt("Security.BlockDurationSec", 60)));
            DisconnectClient(report.clientId, "Cheat detected");
        }
    }
}

void SecurityManager::DisconnectClient(uint32_t clientId, const std::string& reason) {
    auto it = m_connections.find(clientId);
    if (it != m_connections.end()) {
        Logger::Info("SecurityManager: disconnecting client %u (%s)", clientId, reason.c_str());
        it->second->MarkDisconnected();
        m_connections.erase(it);
    }
}