// src/Security/Authentication.cpp
#include "Security/Authentication.h"
#include "Utils/Logger.h"
#include <random>
#include <chrono>
#include <sstream>

Authentication::Authentication(std::shared_ptr<SecurityConfig> config)
    : m_config(config)
{
    Logger::Trace("[Authentication::Authentication] Constructor called, config is %s",
                  config ? "non-null" : "null");
}

Authentication::~Authentication() {
    Logger::Trace("[Authentication::~Authentication] Destructor called, %zu active sessions being destroyed",
                  m_sessions.size());
}

bool Authentication::SendChallenge(std::shared_ptr<ClientConnection> conn) {
    Logger::Trace("[Authentication::SendChallenge] Entry, conn=%p", static_cast<void*>(conn.get()));
    uint32_t clientId = conn->GetClientId();
    Logger::Debug("[Authentication::SendChallenge] Client ID resolved to %u", clientId);
    CleanupExpired();

    SessionInfo session;
    session.challenge = GenerateChallenge();
    session.authenticated = false;
    session.timestamp = std::chrono::steady_clock::now();
    m_sessions[clientId] = session;
    Logger::Debug("[Authentication::SendChallenge] Created new session for client %u with challenge token (length=%zu)",
                  clientId, session.challenge.size());

    Packet pkt("AUTH_CHALLENGE");
    pkt.WriteUInt(clientId);
    pkt.WriteString(session.challenge);
    Logger::Debug("[Authentication::SendChallenge] Built AUTH_CHALLENGE packet for client %u", clientId);
    bool ok = conn->SendPacket(pkt);
    Logger::Info("Sent auth challenge to client %u", clientId);
    if (ok) {
        Logger::Debug("[Authentication::SendChallenge] AUTH_CHALLENGE packet sent successfully to client %u", clientId);
    } else {
        Logger::Error("[Authentication::SendChallenge] Failed to send AUTH_CHALLENGE packet to client %u", clientId);
    }
    Logger::Trace("[Authentication::SendChallenge] Exit, returning %s", ok ? "true" : "false");
    return ok;
}

AuthResult Authentication::ValidateResponse(std::shared_ptr<ClientConnection> conn,
                                            const std::string& responseToken)
{
    Logger::Trace("[Authentication::ValidateResponse] Entry, conn=%p, responseToken length=%zu",
                  static_cast<void*>(conn.get()), responseToken.size());
    uint32_t clientId = conn->GetClientId();
    Logger::Debug("[Authentication::ValidateResponse] Validating response for client %u", clientId);
    CleanupExpired();

    auto it = m_sessions.find(clientId);
    if (it == m_sessions.end()) {
        Logger::Warn("Auth response from unknown session %u", clientId);
        Logger::Debug("[Authentication::ValidateResponse] No pending session found for client %u in session map (map size=%zu)",
                      clientId, m_sessions.size());
        Logger::Trace("[Authentication::ValidateResponse] Exit, returning AuthResult::Error");
        return AuthResult::Error;
    }
    Logger::Debug("[Authentication::ValidateResponse] Found pending session for client %u, proceeding with backend validation",
                  clientId);
    // Verify backend
    AuthResult result = BackendValidate(clientId, responseToken);
    Logger::Debug("[Authentication::ValidateResponse] BackendValidate returned %d for client %u", int(result), clientId);
    if (result == AuthResult::Success) {
        it->second.authenticated = true;
        Logger::Info("Client %u authenticated successfully", clientId);
        Logger::Info("[Authentication::ValidateResponse] Authentication check PASSED for client %u - session marked as authenticated",
                     clientId);
    } else {
        Logger::Warn("Client %u authentication failed (%d)", clientId, int(result));
        Logger::Debug("[Authentication::ValidateResponse] Authentication FAILED for client %u with result=%d, erasing session",
                      clientId, int(result));
        m_sessions.erase(it);
    }
    Logger::Trace("[Authentication::ValidateResponse] Exit, returning AuthResult=%d for client %u", int(result), clientId);
    return result;
}

void Authentication::RevokeSession(uint32_t clientId) {
    Logger::Trace("[Authentication::RevokeSession] Entry, clientId=%u", clientId);
    auto it = m_sessions.find(clientId);
    if (it != m_sessions.end()) {
        Logger::Debug("[Authentication::RevokeSession] Found session for client %u (authenticated=%s), removing it",
                      clientId, it->second.authenticated ? "true" : "false");
        m_sessions.erase(it);
        Logger::Info("Revoked authentication session for client %u", clientId);
    } else {
        Logger::Debug("[Authentication::RevokeSession] No session found for client %u, nothing to revoke", clientId);
    }
    Logger::Trace("[Authentication::RevokeSession] Exit, remaining sessions=%zu", m_sessions.size());
}

bool Authentication::IsAuthenticated(uint32_t clientId) const {
    Logger::Trace("[Authentication::IsAuthenticated] Entry, clientId=%u", clientId);
    auto it = m_sessions.find(clientId);
    bool result = it != m_sessions.end() && it->second.authenticated;
    Logger::Debug("[Authentication::IsAuthenticated] Client %u authentication status: %s (session %s)",
                  clientId, result ? "authenticated" : "not authenticated",
                  it != m_sessions.end() ? "found" : "not found");
    Logger::Trace("[Authentication::IsAuthenticated] Exit, returning %s", result ? "true" : "false");
    return result;
}

std::string Authentication::GenerateChallenge() {
    Logger::Trace("[Authentication::GenerateChallenge] Entry");
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    uint64_t token = dist(rng);
    std::ostringstream oss;
    oss << std::hex << token;
    std::string challenge = oss.str();
    Logger::Debug("[Authentication::GenerateChallenge] Generated challenge token of length %zu", challenge.size());
    Logger::Trace("[Authentication::GenerateChallenge] Exit, returning challenge (length=%zu)", challenge.size());
    return challenge;
}

AuthResult Authentication::BackendValidate(uint32_t clientId,
                                           const std::string& responseToken)
{
    Logger::Trace("[Authentication::BackendValidate] Entry, clientId=%u, responseToken length=%zu",
                  clientId, responseToken.size());
    // Simplified validation: check if auth is enabled
    if (!m_config->IsAntiCheatEnabled()) {
        // If anti-cheat/auth is disabled, allow all
        Logger::Debug("[Authentication::BackendValidate] Anti-cheat/auth is disabled in config, auto-accepting client %u",
                      clientId);
        Logger::Info("[Authentication::BackendValidate] Authentication bypassed for client %u - anti-cheat disabled",
                     clientId);
        Logger::Trace("[Authentication::BackendValidate] Exit, returning AuthResult::Success (auth disabled)");
        return AuthResult::Success;
    }
    Logger::Debug("[Authentication::BackendValidate] Anti-cheat/auth is enabled, performing token validation for client %u",
                  clientId);
    // Accept any non-empty response token
    if (responseToken.empty()) {
        Logger::Warn("Client %u sent empty response token", clientId);
        Logger::Error("[Authentication::BackendValidate] Client %u provided empty response token - validation failed",
                      clientId);
        Logger::Trace("[Authentication::BackendValidate] Exit, returning AuthResult::InvalidCredentials");
        return AuthResult::InvalidCredentials;
    }
    Logger::Debug("[Authentication::BackendValidate] Client %u provided non-empty token (length=%zu), validation passed",
                  clientId, responseToken.size());
    Logger::Trace("[Authentication::BackendValidate] Exit, returning AuthResult::Success");
    return AuthResult::Success;
}

void Authentication::CleanupExpired() {
    Logger::Trace("[Authentication::CleanupExpired] Entry, session count=%zu", m_sessions.size());
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(30); // default challenge timeout
    Logger::Debug("[Authentication::CleanupExpired] Using timeout of %lld seconds for session expiry",
                  static_cast<long long>(timeout.count()));
    size_t removedCount = 0;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (!it->second.authenticated &&
            now - it->second.timestamp > timeout)
        {
            Logger::Info("Auth session for client %u expired", it->first);
            Logger::Debug("[Authentication::CleanupExpired] Removing expired unauthenticated session for client %u",
                          it->first);
            it = m_sessions.erase(it);
            ++removedCount;
        } else {
            ++it;
        }
    }
    if (removedCount > 0) {
        Logger::Debug("[Authentication::CleanupExpired] Removed %zu expired sessions, %zu remaining",
                      removedCount, m_sessions.size());
    }
    Logger::Trace("[Authentication::CleanupExpired] Exit, remaining sessions=%zu", m_sessions.size());
}
