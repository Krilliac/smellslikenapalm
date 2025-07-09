// src/Security/SteamAuth.cpp
#include "Security/SteamAuth.h"
#include "Utils/Logger.h"
#include <random>
#include <sstream>

SteamAuth::SteamAuth(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
{}

SteamAuth::~SteamAuth() = default;

bool SteamAuth::RequestAuthTicket(std::shared_ptr<ClientConnection> conn) {
    uint32_t clientId = conn->GetClientId();
    CleanupExpired();

    std::string nonce = GenerateNonce();
    SteamSession session;
    session.authenticated = false;
    session.timestamp = std::chrono::steady_clock::now();
    m_sessions[clientId] = session;

    SendChallengePacket(conn, nonce);
    Logger::Info("SteamAuth: requested ticket from client %u", clientId);
    return true;
}

SteamAuthResult SteamAuth::ValidateAuthTicket(std::shared_ptr<ClientConnection> conn,
                                              const std::vector<uint8_t>& ticketData) {
    uint32_t clientId = conn->GetClientId();
    CleanupExpired();

    auto it = m_sessions.find(clientId);
    if (it == m_sessions.end()) {
        Logger::Warn("SteamAuth: no pending session for client %u", clientId);
        return SteamAuthResult::Error;
    }

    std::string steamId;
    SteamAuthResult result = BackendValidate(ticketData, steamId);
    if (result == SteamAuthResult::Success) {
        it->second.authenticated = true;
        it->second.steamId = steamId;
        Logger::Info("SteamAuth: client %u authenticated as SteamID %s", clientId, steamId.c_str());
    } else {
        m_sessions.erase(it);
        Logger::Warn("SteamAuth: client %u failed Steam ticket validation (%d)", clientId, int(result));
    }
    return result;
}

void SteamAuth::RevokeSession(uint32_t clientId) {
    m_sessions.erase(clientId);
    Logger::Info("SteamAuth: revoked session for client %u", clientId);
}

bool SteamAuth::IsAuthenticated(uint32_t clientId) const {
    auto it = m_sessions.find(clientId);
    return it != m_sessions.end() && it->second.authenticated;
}

void SteamAuth::CleanupExpired() {
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(m_config->GetInt("SteamAuth.TicketTimeoutSec", 30));
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (!it->second.authenticated &&
            now - it->second.timestamp > timeout)
        {
            Logger::Info("SteamAuth: session for client %u expired", it->first);
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

// Stub: call into Steamworks API or external service
SteamAuthResult SteamAuth::BackendValidate(const std::vector<uint8_t>& ticket,
                                           std::string& outSteamId) {
    // Example code:
    // if (!SteamAPI_Init()) return ServiceUnavailable;
    // SteamAuthTicketResponse_t resp = SteamUser()->BeginAuthSession(ticket.data(), ticket.size(), steamID);
    // switch(resp.m_eAuthSessionResponse) { ... }
    // on success: outSteamId = steamID.ConvertToUint64String();
    // return Success or appropriate error.

    // Placeholder implementation:
    if (ticket.empty()) return SteamAuthResult::InvalidTicket;
    outSteamId = "STEAM_0:1:12345678";
    return SteamAuthResult::Success;
}

void SteamAuth::SendChallengePacket(std::shared_ptr<ClientConnection> conn, const std::string& nonce) {
    Packet pkt("STEAM_AUTH_REQUEST");
    pkt.WriteString(nonce);
    conn->SendPacket(pkt);
}

std::string SteamAuth::GenerateNonce() {
    static std::mt19937_64 rng(std::random_device{}());
    uint64_t v = rng();
    std::ostringstream oss;
    oss << std::hex << v;
    return oss.str();
}