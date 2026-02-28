// src/Security/SteamAuth.cpp
#include "Security/SteamAuth.h"
#include "Utils/Logger.h"
#include <random>
#include <sstream>

SteamAuth::SteamAuth(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
{
    Logger::Trace("[SteamAuth::SteamAuth] Constructor called, config is %s",
                  m_config ? "non-null" : "null");
}

SteamAuth::~SteamAuth() {
    Logger::Trace("[SteamAuth::~SteamAuth] Destructor called, %zu active sessions being destroyed",
                  m_sessions.size());
}

bool SteamAuth::RequestAuthTicket(std::shared_ptr<ClientConnection> conn) {
    Logger::Trace("[SteamAuth::RequestAuthTicket] Entry, conn=%p", static_cast<void*>(conn.get()));
    uint32_t clientId = conn->GetClientId();
    Logger::Debug("[SteamAuth::RequestAuthTicket] Client ID resolved to %u", clientId);
    CleanupExpired();

    std::string nonce = GenerateNonce();
    Logger::Debug("[SteamAuth::RequestAuthTicket] Generated nonce for client %u (length=%zu)", clientId, nonce.size());
    SteamSession session;
    session.authenticated = false;
    session.timestamp = std::chrono::steady_clock::now();
    m_sessions[clientId] = session;
    Logger::Debug("[SteamAuth::RequestAuthTicket] Created new Steam session for client %u, total sessions=%zu",
                  clientId, m_sessions.size());

    SendChallengePacket(conn, nonce);
    Logger::Info("SteamAuth: requested ticket from client %u", clientId);
    Logger::Info("[SteamAuth::RequestAuthTicket] Steam auth ticket request sent to client %u", clientId);
    Logger::Trace("[SteamAuth::RequestAuthTicket] Exit, returning true");
    return true;
}

SteamAuthResult SteamAuth::ValidateAuthTicket(std::shared_ptr<ClientConnection> conn,
                                              const std::vector<uint8_t>& ticketData) {
    Logger::Trace("[SteamAuth::ValidateAuthTicket] Entry, conn=%p, ticketData size=%zu",
                  static_cast<void*>(conn.get()), ticketData.size());
    uint32_t clientId = conn->GetClientId();
    Logger::Debug("[SteamAuth::ValidateAuthTicket] Validating Steam auth ticket for client %u (ticket size=%zu bytes)",
                  clientId, ticketData.size());
    CleanupExpired();

    auto it = m_sessions.find(clientId);
    if (it == m_sessions.end()) {
        Logger::Warn("SteamAuth: no pending session for client %u", clientId);
        Logger::Debug("[SteamAuth::ValidateAuthTicket] No pending Steam session found for client %u in session map (map size=%zu)",
                      clientId, m_sessions.size());
        Logger::Trace("[SteamAuth::ValidateAuthTicket] Exit, returning SteamAuthResult::Error");
        return SteamAuthResult::Error;
    }
    Logger::Debug("[SteamAuth::ValidateAuthTicket] Found pending session for client %u, proceeding with backend validation",
                  clientId);

    std::string steamId;
    SteamAuthResult result = BackendValidate(ticketData, steamId);
    Logger::Debug("[SteamAuth::ValidateAuthTicket] BackendValidate returned %d for client %u, steamId='%s'",
                  int(result), clientId, steamId.c_str());
    if (result == SteamAuthResult::Success) {
        it->second.authenticated = true;
        it->second.steamId = steamId;
        Logger::Info("SteamAuth: client %u authenticated as SteamID %s", clientId, steamId.c_str());
        Logger::Info("[SteamAuth::ValidateAuthTicket] Steam authentication PASSED for client %u - SteamID '%s' verified",
                     clientId, steamId.c_str());
    } else {
        Logger::Warn("SteamAuth: client %u failed Steam ticket validation (%d)", clientId, int(result));
        Logger::Debug("[SteamAuth::ValidateAuthTicket] Steam authentication FAILED for client %u with result=%d, erasing session",
                      clientId, int(result));
        m_sessions.erase(it);
    }
    Logger::Trace("[SteamAuth::ValidateAuthTicket] Exit, returning SteamAuthResult=%d for client %u", int(result), clientId);
    return result;
}

void SteamAuth::RevokeSession(uint32_t clientId) {
    Logger::Trace("[SteamAuth::RevokeSession] Entry, clientId=%u", clientId);
    auto it = m_sessions.find(clientId);
    if (it != m_sessions.end()) {
        Logger::Debug("[SteamAuth::RevokeSession] Found session for client %u (authenticated=%s, steamId='%s'), removing it",
                      clientId, it->second.authenticated ? "true" : "false", it->second.steamId.c_str());
    } else {
        Logger::Debug("[SteamAuth::RevokeSession] No session found for client %u, nothing to revoke", clientId);
    }
    m_sessions.erase(clientId);
    Logger::Info("SteamAuth: revoked session for client %u", clientId);
    Logger::Trace("[SteamAuth::RevokeSession] Exit, remaining sessions=%zu", m_sessions.size());
}

bool SteamAuth::IsAuthenticated(uint32_t clientId) const {
    Logger::Trace("[SteamAuth::IsAuthenticated] Entry, clientId=%u", clientId);
    auto it = m_sessions.find(clientId);
    bool result = it != m_sessions.end() && it->second.authenticated;
    Logger::Debug("[SteamAuth::IsAuthenticated] Client %u authentication status: %s (session %s)",
                  clientId, result ? "authenticated" : "not authenticated",
                  it != m_sessions.end() ? "found" : "not found");
    if (result) {
        Logger::Debug("[SteamAuth::IsAuthenticated] Client %u is authenticated as SteamID '%s'",
                      clientId, it->second.steamId.c_str());
    }
    Logger::Trace("[SteamAuth::IsAuthenticated] Exit, returning %s", result ? "true" : "false");
    return result;
}

void SteamAuth::CleanupExpired() {
    Logger::Trace("[SteamAuth::CleanupExpired] Entry, session count=%zu", m_sessions.size());
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(30); // default ticket timeout
    Logger::Debug("[SteamAuth::CleanupExpired] Using timeout of %lld seconds for session expiry",
                  static_cast<long long>(timeout.count()));
    size_t removedCount = 0;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (!it->second.authenticated &&
            now - it->second.timestamp > timeout)
        {
            Logger::Info("SteamAuth: session for client %u expired", it->first);
            Logger::Debug("[SteamAuth::CleanupExpired] Removing expired unauthenticated session for client %u",
                          it->first);
            it = m_sessions.erase(it);
            ++removedCount;
        } else {
            ++it;
        }
    }
    if (removedCount > 0) {
        Logger::Debug("[SteamAuth::CleanupExpired] Removed %zu expired sessions, %zu remaining",
                      removedCount, m_sessions.size());
    }
    Logger::Trace("[SteamAuth::CleanupExpired] Exit, remaining sessions=%zu", m_sessions.size());
}

// Stub: call into Steamworks API or external service
SteamAuthResult SteamAuth::BackendValidate(const std::vector<uint8_t>& ticket,
                                           std::string& outSteamId) {
    Logger::Trace("[SteamAuth::BackendValidate] Entry, ticket size=%zu bytes", ticket.size());
    Logger::Debug("[SteamAuth::BackendValidate] Performing backend Steam ticket validation (ticket=%zu bytes)",
                  ticket.size());
    // Example code:
    // if (!SteamAPI_Init()) return ServiceUnavailable;
    // SteamAuthTicketResponse_t resp = SteamUser()->BeginAuthSession(ticket.data(), ticket.size(), steamID);
    // switch(resp.m_eAuthSessionResponse) { ... }
    // on success: outSteamId = steamID.ConvertToUint64String();
    // return Success or appropriate error.

    // Placeholder implementation:
    if (ticket.empty()) {
        Logger::Error("[SteamAuth::BackendValidate] Empty ticket data received - validation failed");
        Logger::Warn("[SteamAuth::BackendValidate] Client provided empty Steam auth ticket");
        Logger::Trace("[SteamAuth::BackendValidate] Exit, returning SteamAuthResult::InvalidTicket");
        return SteamAuthResult::InvalidTicket;
    }
    Logger::Debug("[SteamAuth::BackendValidate] Ticket data is non-empty (%zu bytes), accepting (placeholder implementation)",
                  ticket.size());
    outSteamId = "STEAM_0:1:12345678";
    Logger::Info("[SteamAuth::BackendValidate] Backend validation successful, resolved SteamID='%s'",
                 outSteamId.c_str());
    Logger::Trace("[SteamAuth::BackendValidate] Exit, returning SteamAuthResult::Success");
    return SteamAuthResult::Success;
}

void SteamAuth::SendChallengePacket(std::shared_ptr<ClientConnection> conn, const std::string& nonce) {
    Logger::Trace("[SteamAuth::SendChallengePacket] Entry, conn=%p, nonce length=%zu",
                  static_cast<void*>(conn.get()), nonce.size());
    Packet pkt("STEAM_AUTH_REQUEST");
    pkt.WriteString(nonce);
    Logger::Debug("[SteamAuth::SendChallengePacket] Built STEAM_AUTH_REQUEST packet with nonce");
    conn->SendPacket(pkt);
    Logger::Debug("[SteamAuth::SendChallengePacket] STEAM_AUTH_REQUEST packet sent to client");
    Logger::Trace("[SteamAuth::SendChallengePacket] Exit");
}

std::string SteamAuth::GenerateNonce() {
    Logger::Trace("[SteamAuth::GenerateNonce] Entry");
    static std::mt19937_64 rng(std::random_device{}());
    uint64_t v = rng();
    std::ostringstream oss;
    oss << std::hex << v;
    std::string nonce = oss.str();
    Logger::Debug("[SteamAuth::GenerateNonce] Generated nonce of length %zu", nonce.size());
    Logger::Trace("[SteamAuth::GenerateNonce] Exit, returning nonce (length=%zu)", nonce.size());
    return nonce;
}
