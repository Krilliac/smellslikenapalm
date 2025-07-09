// src/Security/SteamAuth.h
#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <chrono>
#include "Network/ClientConnection.h"
#include "Config/SecurityConfig.h"

enum class SteamAuthResult {
    Success,
    InvalidTicket,
    ExpiredTicket,
    ServiceUnavailable,
    BannedAccount,
    Error
};

struct SteamSession {
    std::string steamId;
    bool        authenticated = false;
    std::chrono::steady_clock::time_point timestamp;
};

class SteamAuth {
public:
    explicit SteamAuth(std::shared_ptr<SecurityConfig> config);
    ~SteamAuth();

    // Send a request for the client to provide a Steam auth ticket
    bool RequestAuthTicket(std::shared_ptr<ClientConnection> conn);

    // Validate the ticket provided by the client
    SteamAuthResult ValidateAuthTicket(std::shared_ptr<ClientConnection> conn,
                                       const std::vector<uint8_t>& ticketData);

    // Revoke and remove a session
    void RevokeSession(uint32_t clientId);

    // Check if client is authenticated
    bool IsAuthenticated(uint32_t clientId) const;

    // Periodic cleanup of expired sessions
    void CleanupExpired();

private:
    std::shared_ptr<SecurityConfig> m_config;
    std::unordered_map<uint32_t, SteamSession> m_sessions;

    // Backend call to Steamworks or REST API
    SteamAuthResult BackendValidate(const std::vector<uint8_t>& ticket, std::string& outSteamId);

    // Generate and send an auth challenge message
    void SendChallengePacket(std::shared_ptr<ClientConnection> conn, const std::string& nonce);

    // Generate a random nonce for challenge
    std::string GenerateNonce();
};