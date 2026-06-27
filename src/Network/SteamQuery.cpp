// src/Network/SteamQuery.cpp
#include "Network/SteamQuery.h"
#include "Network/SocketFactory.h"
#include "Utils/Logger.h"
#include "Network/PlatformSocket.h"
#include <cstring>
#include <chrono>

SteamQuery::SteamQuery(const std::string& masterServerIp, uint16_t masterServerPort)
    : m_masterIp(masterServerIp), m_masterPort(masterServerPort)
{
    Logger::Trace("[SteamQuery::SteamQuery] Entry: masterServerIp='%s', masterServerPort=%u",
                  masterServerIp.c_str(), masterServerPort);
    Logger::Info("SteamQuery initialized (master=%s:%u)", m_masterIp.c_str(), m_masterPort);
    Logger::Debug("[SteamQuery::SteamQuery] m_masterIp='%s', m_masterPort=%u", m_masterIp.c_str(), m_masterPort);
    Logger::Trace("[SteamQuery::SteamQuery] Exit");
}

SteamQuery::~SteamQuery() {
    Logger::Trace("[SteamQuery::~SteamQuery] Entry: destructor called");
    Logger::Debug("[SteamQuery::~SteamQuery] Destroying SteamQuery for master %s:%u", m_masterIp.c_str(), m_masterPort);
    Logger::Trace("[SteamQuery::~SteamQuery] Exit");
}

std::vector<std::pair<std::string,uint16_t>> SteamQuery::QueryMaster() {
    Logger::Trace("[SteamQuery::QueryMaster] Entry: master=%s:%u", m_masterIp.c_str(), m_masterPort);
    std::vector<std::pair<std::string,uint16_t>> servers;
    // Build A2S_SERVERQUERY_GETCHALLENGE then A2S_SERVERQUERY_GETLIST...
    // For brevity, omitted full master-server protocol.
    // Typically involves query "\x31" then region byte and "\0" termination, then loop parsing IP:port entries.
    Logger::Debug("[SteamQuery::QueryMaster] Master server query protocol not fully implemented");
    Logger::Info("[SteamQuery::QueryMaster] Queried master %s:%u, got %zu servers",
                 m_masterIp.c_str(), m_masterPort, servers.size());
    Logger::Trace("[SteamQuery::QueryMaster] Exit: returning %zu servers", servers.size());
    return servers;
}

bool SteamQuery::QueryServer(const std::string& ip, uint16_t port, SteamServerInfo& outInfo) {
    Logger::Trace("[SteamQuery::QueryServer] Entry: ip='%s', port=%u", ip.c_str(), port);
    Logger::Debug("[SteamQuery::QueryServer] Building info request for %s:%u", ip.c_str(), port);
    auto req = BuildInfoRequest();
    Logger::Debug("[SteamQuery::QueryServer] Info request built, size=%zu bytes", req.size());
    std::vector<uint8_t> resp(512);
    Logger::Debug("[SteamQuery::QueryServer] Sending query to %s:%u and waiting for response", ip.c_str(), port);
    if (!SendAndReceive(req, resp, ip, port)) {
        Logger::Warn("SteamQuery: no response from %s:%u", ip.c_str(), port);
        Logger::Debug("[SteamQuery::QueryServer] SendAndReceive failed for %s:%u", ip.c_str(), port);
        Logger::Trace("[SteamQuery::QueryServer] Exit: returning false (no response)");
        return false;
    }
    Logger::Debug("[SteamQuery::QueryServer] Received response: %zu bytes from %s:%u", resp.size(), ip.c_str(), port);
    bool parsed = ParseInfoResponse(resp, outInfo);
    if (parsed) {
        Logger::Info("[SteamQuery::QueryServer] Server %s:%u: name='%s', map='%s', players=%u/%u",
                     ip.c_str(), port, outInfo.serverName.c_str(), outInfo.mapName.c_str(),
                     outInfo.currentPlayers, outInfo.maxPlayers);
    } else {
        Logger::Error("[SteamQuery::QueryServer] Failed to parse info response from %s:%u", ip.c_str(), port);
    }
    Logger::Trace("[SteamQuery::QueryServer] Exit: returning %s", parsed ? "true" : "false");
    return parsed;
}

std::vector<uint8_t> SteamQuery::BuildInfoRequest() {
    Logger::Trace("[SteamQuery::BuildInfoRequest] Entry");
    // Format: 0xFF 0xFF 0xFF 0xFF 'T' "Source Engine Query" 0x00
    const char* payload = "\xFF\xFF\xFF\xFFTSource Engine Query\x00";
    size_t len = std::strlen(payload) + 4;
    Logger::Debug("[SteamQuery::BuildInfoRequest] Building A2S_INFO request, payload length=%zu", len);
    auto result = std::vector<uint8_t>(payload, payload + len);
    Logger::Debug("[SteamQuery::BuildInfoRequest] Request built: %zu bytes", result.size());
    Logger::Trace("[SteamQuery::BuildInfoRequest] Exit: returning %zu bytes", result.size());
    return result;
}

bool SteamQuery::ParseInfoResponse(const std::vector<uint8_t>& buf, SteamServerInfo& info) {
    Logger::Trace("[SteamQuery::ParseInfoResponse] Entry: buf size=%zu", buf.size());
    if (buf.size() < 5) {
        Logger::Error("[SteamQuery::ParseInfoResponse] Buffer too small: %zu bytes (minimum 5)", buf.size());
        Logger::Trace("[SteamQuery::ParseInfoResponse] Exit: returning false (buffer too small)");
        return false;
    }
    if (buf[4] != 'I') {
        Logger::Error("[SteamQuery::ParseInfoResponse] Invalid response header byte: 0x%02X (expected 'I' = 0x49)", buf[4]);
        Logger::Trace("[SteamQuery::ParseInfoResponse] Exit: returning false (invalid header)");
        return false;
    }
    Logger::Debug("[SteamQuery::ParseInfoResponse] Valid A2S_INFO response header detected");
    size_t offset = 5;
    // Protocol: byte Protocol; string Name; string Map; string Folder; string Game; short ID; byte Players; byte max; byte bots; ...
    // We extract Name, Map, Game, Players, Max, and ping separate.
    auto readString = [&](std::string& out) {
        out.clear();
        while (offset < buf.size() && buf[offset] != '\0') {
            out.push_back(buf[offset++]);
        }
        // Only step over the NUL terminator if one is actually present. A response
        // from a malicious/MITM'd server may omit it; advancing unconditionally would
        // push offset past buf.size() and could underflow later "remaining" maths.
        if (offset < buf.size()) {
            ++offset;
        }
    };
    // Skip protocol byte
    // offset already at first char of name
    readString(info.serverName);
    Logger::Debug("[SteamQuery::ParseInfoResponse] Parsed serverName='%s', offset=%zu", info.serverName.c_str(), offset);
    readString(info.mapName);
    Logger::Debug("[SteamQuery::ParseInfoResponse] Parsed mapName='%s', offset=%zu", info.mapName.c_str(), offset);
    // skip Folder
    std::string dummy;
    readString(dummy);
    Logger::Debug("[SteamQuery::ParseInfoResponse] Skipped folder='%s', offset=%zu", dummy.c_str(), offset);
    readString(info.gameMode);
    Logger::Debug("[SteamQuery::ParseInfoResponse] Parsed gameMode='%s', offset=%zu", info.gameMode.c_str(), offset);
    offset += 2; // game ID
    Logger::Debug("[SteamQuery::ParseInfoResponse] Skipped game ID (2 bytes), offset=%zu", offset);
    if (offset + 2 > buf.size()) {
        Logger::Error("[SteamQuery::ParseInfoResponse] Buffer overrun at offset %zu (buf size=%zu), cannot read player counts",
                      offset, buf.size());
        Logger::Trace("[SteamQuery::ParseInfoResponse] Exit: returning false (buffer overrun)");
        return false;
    }
    info.currentPlayers = buf[offset++];
    info.maxPlayers     = buf[offset++];
    Logger::Debug("[SteamQuery::ParseInfoResponse] Parsed currentPlayers=%u, maxPlayers=%u", info.currentPlayers, info.maxPlayers);
    // Ping must be measured client-side via round-trip; here placeholder 0
    info.pingMs = 0;
    Logger::Debug("[SteamQuery::ParseInfoResponse] Ping set to %u ms (placeholder)", info.pingMs);
    Logger::Info("[SteamQuery::ParseInfoResponse] Successfully parsed: server='%s', map='%s', game='%s', players=%u/%u",
                 info.serverName.c_str(), info.mapName.c_str(), info.gameMode.c_str(),
                 info.currentPlayers, info.maxPlayers);
    Logger::Trace("[SteamQuery::ParseInfoResponse] Exit: returning true");
    return true;
}

bool SteamQuery::SendAndReceive(const std::vector<uint8_t>& req,
                                std::vector<uint8_t>& resp,
                                const std::string& ip,
                                uint16_t port,
                                uint32_t timeoutMs)
{
    Logger::Trace("[SteamQuery::SendAndReceive] Entry: ip='%s', port=%u, timeoutMs=%u, req size=%zu",
                  ip.c_str(), port, timeoutMs, req.size());
    SocketConfig cfg;
    cfg.recvTimeout = std::chrono::milliseconds(timeoutMs);
    cfg.sendTimeout = std::chrono::milliseconds(timeoutMs);
    cfg.nonBlocking = false;
    Logger::Debug("[SteamQuery::SendAndReceive] Socket config: recvTimeout=%u ms, sendTimeout=%u ms, nonBlocking=false",
                  timeoutMs, timeoutMs);
    auto sock = SocketFactory::CreateUdpSocket(0, cfg);
    if (sock < 0) {
        Logger::Error("[SteamQuery::SendAndReceive] Failed to create UDP socket for query to %s:%u", ip.c_str(), port);
        Logger::Trace("[SteamQuery::SendAndReceive] Exit: returning false (socket creation failed)");
        return false;
    }
    Logger::Debug("[SteamQuery::SendAndReceive] Created UDP socket: fd=%d", sock);

    // Send request
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);

    Logger::Debug("[SteamQuery::SendAndReceive] Sending %zu bytes to %s:%u", req.size(), ip.c_str(), port);
    ssize_t sentBytes = sendto(sock, reinterpret_cast<const char*>(req.data()), (int)req.size(), 0, (sockaddr*)&dest, sizeof(dest));
    Logger::Debug("[SteamQuery::SendAndReceive] sendto returned %zd", sentBytes);
    if (sentBytes < 0) {
        Logger::Error("[SteamQuery::SendAndReceive] sendto failed for %s:%u", ip.c_str(), port);
    }

    // Receive
    socklen_t addrLen = sizeof(dest);
    Logger::Debug("[SteamQuery::SendAndReceive] Waiting for response from %s:%u (timeout=%u ms)", ip.c_str(), port, timeoutMs);
    int len = recvfrom(sock, reinterpret_cast<char*>(resp.data()), (int)resp.size(), 0, (sockaddr*)&dest, &addrLen);
    Logger::Debug("[SteamQuery::SendAndReceive] recvfrom returned %d", len);
    CloseSocket(sock);
    Logger::Debug("[SteamQuery::SendAndReceive] Socket fd=%d closed", sock);
    if (len <= 0) {
        Logger::Warn("[SteamQuery::SendAndReceive] No response received from %s:%u (recvfrom returned %d)",
                     ip.c_str(), port, len);
        Logger::Trace("[SteamQuery::SendAndReceive] Exit: returning false (no response)");
        return false;
    }
    resp.resize(len);
    Logger::Info("[SteamQuery::SendAndReceive] Received %d byte response from %s:%u", len, ip.c_str(), port);
    Logger::Trace("[SteamQuery::SendAndReceive] Exit: returning true");
    return true;
}
