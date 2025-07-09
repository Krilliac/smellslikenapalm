// src/Security/Authentication.cpp
#include "Security/Authentication.h"
#include "Utils/Logger.h"
#include <random>
#include <chrono>

Authentication::Authentication(std::shared_ptr<SecurityConfig> config)
    : m_config(config)
{}

Authentication::~Authentication() = default;

bool Authentication::SendChallenge(std::shared_ptr<ClientConnection> conn) {
    uint32_t clientId = conn->GetClientId();
    CleanupExpired();

    SessionInfo session;
    session.challenge = GenerateChallenge();
    session.authenticated = false;
    session.timestamp = std::chrono::steady_clock::now();
    m_sessions[clientId] = session;

    Packet pkt("AUTH_CHALLENGE");
    pkt.WriteUInt(clientId);
    pkt.WriteString(session.challenge);
    bool ok = conn->SendPacket(pkt);
    Logger::Info("Sent auth challenge to client %u", clientId);
    return ok;
}

AuthResult Authentication::ValidateResponse(std::shared_ptr<ClientConnection> conn,
                                            const std::string& responseToken)
{
    uint32_t clientId = conn->GetClientId();
    CleanupExpired();

    auto it = m_sessions.find(clientId);
    if (it == m_sessions.end()) {
        Logger::Warn("Auth response from unknown session %u", clientId);
        return AuthResult::Error;
    }
    // Verify backend
    AuthResult result = BackendValidate(clientId, responseToken);
    if (result == AuthResult::Success) {
        it->second.authenticated = true;
        Logger::Info("Client %u authenticated successfully", clientId);
    } else {
        Logger::Warn("Client %u authentication failed (%d)", clientId, int(result));
        m_sessions.erase(it);
    }
    return result;
}

void Authentication::RevokeSession(uint32_t clientId) {
    auto it = m_sessions.find(clientId);
    if (it != m_sessions.end()) {
        m_sessions.erase(it);
        Logger::Info("Revoked authentication session for client %u", clientId);
    }
}

bool Authentication::IsAuthenticated(uint32_t clientId) const {
    auto it = m_sessions.find(clientId);
    return it != m_sessions.end() && it->second.authenticated;
}

std::string Authentication::GenerateChallenge() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    uint64_t token = dist(rng);
    std::ostringstream oss;
    oss << std::hex << token;
    return oss.str();
}

AuthResult Authentication::BackendValidate(uint32_t clientId,
                                           const std::string& responseToken)
{
    // Example: validate against configured auth service
    if (!m_config->IsServiceAvailable()) {
        return AuthResult::ServiceUnavailable;
    }
    if (!m_config->ValidateToken(clientId, responseToken)) {
        if (m_config->IsBanned(clientId)) {
            return AuthResult::AccountBanned;
        }
        return AuthResult::InvalidCredentials;
    }
    return AuthResult::Success;
}

void Authentication::CleanupExpired() {
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(m_config->GetInt("Auth.ChallengeTimeout", 30));
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (!it->second.authenticated &&
            now - it->second.timestamp > timeout)
        {
            Logger::Info("Auth session for client %u expired", it->first);
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}