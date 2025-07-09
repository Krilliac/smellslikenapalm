// src/Security/Authentication.h
#pragma once

#include <string>
#include <memory>
#include "Network/ClientConnection.h"
#include "Config/SecurityConfig.h"

enum class AuthResult {
    Success,
    InvalidCredentials,
    AccountBanned,
    ServiceUnavailable,
    Error
};

class Authentication {
public:
    Authentication(std::shared_ptr<SecurityConfig> config);
    ~Authentication();

    // Perform initial handshake: send challenge to client
    bool SendChallenge(std::shared_ptr<ClientConnection> conn);

    // Validate a client’s response to the challenge
    AuthResult ValidateResponse(std::shared_ptr<ClientConnection> conn,
                                const std::string& responseToken);

    // Revoke a client’s session (e.g., on ban or timeout)
    void RevokeSession(uint32_t clientId);

    // Check if client is authenticated
    bool IsAuthenticated(uint32_t clientId) const;

private:
    struct SessionInfo {
        std::string challenge;
        bool        authenticated;
        std::chrono::steady_clock::time_point timestamp;
    };

    std::shared_ptr<SecurityConfig>         m_config;
    std::unordered_map<uint32_t, SessionInfo> m_sessions;

    // Generate a random challenge token
    std::string GenerateChallenge();

    // Perform backend validation (e.g., against Steam, JWT, or internal DB)
    AuthResult BackendValidate(uint32_t clientId,
                               const std::string& responseToken);

    // Clean up stale sessions
    void CleanupExpired();
};