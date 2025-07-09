#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "Network/ClientConnection.h"
#include "Security/Authentication.h"
#include "Security/BanManager.h"
#include "Security/EnhancedEACAntiCheat.h"
#include "Network/NetworkBlocker.h"

class SecurityManager {
public:
    SecurityManager(std::shared_ptr<SecurityConfig> config);
    ~SecurityManager();

    // Initialize all security subsystems
    bool Initialize();

    // Shutdown and cleanup
    void Shutdown();

    // Handle a new client connection
    void OnClientConnect(std::shared_ptr<ClientConnection> conn);

    // Handle client disconnect
    void OnClientDisconnect(uint32_t clientId);

    // Validate incoming packet (returns false to drop)
    bool ValidatePacket(uint32_t clientId, const std::vector<uint8_t>& rawData);

    // Periodic update (called once per tick)
    void Update();

    // Administrative actions
    void BanClient(const std::string& steamId,
                   BanType type,
                   std::chrono::seconds duration = std::chrono::seconds(0),
                   const std::string& reason = "");
    bool UnbanClient(const std::string& steamId);

    // Query ban status
    bool IsBanned(const std::string& steamId) const;

private:
    std::shared_ptr<SecurityConfig>        m_config;
    Authentication                         m_auth;
    BanManager                             m_banManager;
    EnhancedEACAntiCheat                   m_eac;
    NetworkBlocker                         m_blocker;

    // Map clientId → connection
    std::unordered_map<uint32_t, std::shared_ptr<ClientConnection>> m_connections;
    mutable std::mutex                     m_mutex;

    // Internal helpers
    void HandleEACReport(const EnhancedEACReport& report);
    void DisconnectClient(uint32_t clientId, const std::string& reason);
};