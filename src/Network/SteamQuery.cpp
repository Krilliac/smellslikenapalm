// src/Network/SteamQuery.cpp
#include "Network/SteamQuery.h"
#include "Network/SocketFactory.h"
#include "Utils/Logger.h"
#include <cstring>
#include <chrono>

SteamQuery::SteamQuery(const std::string& masterServerIp, uint16_t masterServerPort)
    : m_masterIp(masterServerIp), m_masterPort(masterServerPort)
{
    Logger::Info("SteamQuery initialized (master=%s:%u)", m_masterIp.c_str(), m_masterPort);
}

SteamQuery::~SteamQuery() = default;

std::vector<std::pair<std::string,uint16_t>> SteamQuery::QueryMaster() {
    std::vector<std::pair<std::string,uint16_t>> servers;
    // Build A2S_SERVERQUERY_GETCHALLENGE then A2S_SERVERQUERY_GETLIST...
    // For brevity, omitted full master-server protocol.  
    // Typically involves query “\x31” then region byte and “\0” termination, then loop parsing IP:port entries.
    return servers;
}

bool SteamQuery::QueryServer(const std::string& ip, uint16_t port, SteamServerInfo& outInfo) {
    auto req = BuildInfoRequest();
    std::vector<uint8_t> resp(512);
    if (!SendAndReceive(req, resp, ip, port)) {
        Logger::Warn("SteamQuery: no response from %s:%u", ip.c_str(), port);
        return false;
    }
    return ParseInfoResponse(resp, outInfo);
}

std::vector<uint8_t> SteamQuery::BuildInfoRequest() {
    // Format: 0xFF 0xFF 0xFF 0xFF 'T' "Source Engine Query" 0x00
    const char* payload = "\xFF\xFF\xFF\xFFTSource Engine Query\x00";
    size_t len = std::strlen(payload) + 4;
    return std::vector<uint8_t>(payload, payload + len);
}

bool SteamQuery::ParseInfoResponse(const std::vector<uint8_t>& buf, SteamServerInfo& info) {
    if (buf.size() < 5 || buf[4] != 'I') return false;
    size_t offset = 5;
    // Protocol: byte Protocol; string Name; string Map; string Folder; string Game; short ID; byte Players; byte max; byte bots; ...
    // We extract Name, Map, Game, Players, Max, and ping separate.
    auto readString = [&](std::string& out) {
        out.clear();
        while (offset < buf.size() && buf[offset] != '\0') {
            out.push_back(buf[offset++]);
        }
        offset++;
    };
    // Skip protocol byte
    // offset already at first char of name
    readString(info.serverName);
    readString(info.mapName);
    // skip Folder
    std::string dummy;
    readString(dummy);
    readString(info.gameMode);
    offset += 2; // game ID
    if (offset + 2 > buf.size()) return false;
    info.currentPlayers = buf[offset++];
    info.maxPlayers     = buf[offset++];
    // Ping must be measured client-side via round-trip; here placeholder 0
    info.pingMs = 0;
    return true;
}

bool SteamQuery::SendAndReceive(const std::vector<uint8_t>& req,
                                std::vector<uint8_t>& resp,
                                const std::string& ip,
                                uint16_t port,
                                uint32_t timeoutMs)
{
    SocketConfig cfg;
    cfg.recvTimeout = std::chrono::milliseconds(timeoutMs);
    cfg.sendTimeout = std::chrono::milliseconds(timeoutMs);
    cfg.nonBlocking = false;
    auto sock = SocketFactory::CreateUdpSocket(0, cfg);
    if (sock < 0) return false;

    // Send request
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);

    sendto(sock, req.data(), (int)req.size(), 0, (sockaddr*)&dest, sizeof(dest));

    // Receive
    socklen_t addrLen = sizeof(dest);
    int len = recvfrom(sock, resp.data(), (int)resp.size(), 0, (sockaddr*)&dest, &addrLen);
    close(sock);
    if (len <= 0) return false;
    resp.resize(len);
    return true;
}