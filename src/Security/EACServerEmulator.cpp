// src/Security/EACServerEmulator.cpp

#include "Security/EACServerEmulator.h"
#include "Security/EACPackets.h"
#include "Utils/Logger.h"
#include <cstring>
#include <chrono>
#include <algorithm>

EACServerEmulator::EACServerEmulator()
    : m_listenPort(0)
    , m_running(false)
    , m_safeMode(false)
    , m_alwaysAccept(false)
    , m_nextClientId(1)
    , m_nextMemoryRequestId(1)
{
}

EACServerEmulator::~EACServerEmulator() {
    Shutdown();
}

void EACServerEmulator::SetSafeMode(bool safe) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_safeMode = safe;
}

void EACServerEmulator::SetAlwaysAccept(bool accept) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_alwaysAccept = accept;
}

bool EACServerEmulator::Initialize(uint16_t listenPort) {
    m_listenPort = listenPort;
    if (!m_socket.Bind(listenPort)) {
        Logger::Error("EACServerEmulator: Failed to bind UDP on port %u", listenPort);
        return false;
    }

    m_running = true;
    m_thread = std::thread(&EACServerEmulator::RunLoop, this);
    Logger::Info("EACServerEmulator: Listening on port %u", listenPort);
    return true;
}

void EACServerEmulator::RunLoop() {
    while (m_running) {
        std::string ip;
        uint16_t port = 0;
        std::vector<uint8_t> buf(1500);
        int len = m_socket.ReceiveFrom(ip, port, buf.data(), (int)buf.size());
        if (len > 0) {
            buf.resize(len);
            HandlePacket(ip, port, buf);
        }
        
        // Cleanup stale connections periodically
        static auto lastCleanup = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastCleanup > std::chrono::seconds(30)) {
            CleanupStaleConnections();
            lastCleanup = now;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void EACServerEmulator::HandlePacket(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data) {
    if (data.empty()) return;

    uint8_t type = data[0];
    
    // Handle memory reply packets first
    if (type == static_cast<uint8_t>(EACMessageType::MemoryReply)) {
        HandleMemoryReply(0, data.data(), data.size()); // ClientId lookup needed
        return;
    }
    
    // Handle debug draw commands
    if (type == static_cast<uint8_t>(EACMessageType::DebugDrawCommand)) {
        // Debug draw packets are handled by client, no server processing needed
        Logger::Debug("Debug draw command received from %s:%u", ip.c_str(), port);
        return;
    }

    // Handle traditional EAC packets
    ClientSession* sess = FindOrCreateSession(ip, port);
    if (!sess) return;

    switch (type) {
        case 0x01: {
            Logger::Debug("Challenge request from %s:%u", ip.c_str(), port);
            SendChallenge(*sess);
            break;
        }
        case 0x02: {
            if (data.size() >= 9) {
                uint64_t clientNonce = 0;
                memcpy(&clientNonce, data.data() + 1, 8);
                Logger::Debug("Handshake request from %s:%u with nonce %llu", ip.c_str(), port, clientNonce);
                HandleHandshake(*sess, clientNonce);
            }
            break;
        }
        default:
            Logger::Warn("EACServerEmulator: Unknown packet type 0x%02X from %s:%u", type, ip.c_str(), port);
    }
}

void EACServerEmulator::HandleMemoryReply(uint32_t clientId, const uint8_t* data, size_t dataSize) {
    if (dataSize < 1 + sizeof(uint32_t) + sizeof(MemoryReplyPacket)) return;

    // Extract request ID
    uint32_t reqId;
    std::memcpy(&reqId, data + 1, sizeof(reqId));
    
    // Extract reply packet
    MemoryReplyPacket reply{};
    std::memcpy(&reply, data + 1 + sizeof(reqId), sizeof(reply));
    
    // Extract payload data
    const uint8_t* payload = data + 1 + sizeof(reqId) + sizeof(reply);
    size_t payloadSize = dataSize - (1 + sizeof(reqId) + sizeof(reply));
    
    // Find and execute callback
    std::function<void(const MemoryReplyPacket&, const uint8_t*)> callback;
    {
        std::lock_guard<std::mutex> lock(m_memoryRequestMutex);
        auto it = m_pendingMemoryRequests.find(reqId);
        if (it != m_pendingMemoryRequests.end()) {
            callback = it->second;
            m_pendingMemoryRequests.erase(it);
        }
    }
    
    if (callback) {
        Logger::Debug("Executing memory reply callback for request %u, op %d", reqId, static_cast<int>(reply.op));
        callback(reply, payload);
    } else {
        Logger::Warn("No callback found for memory request %u", reqId);
    }
}

void EACServerEmulator::SendChallenge(const ClientSession& sess) {
    uint32_t challenge = GenerateChallenge();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const_cast<ClientSession&>(sess).lastChallenge = challenge;
        const_cast<ClientSession&>(sess).lastActivity = std::chrono::steady_clock::now();
    }

    std::vector<uint8_t> resp(5);
    resp[0] = 0x81; // challenge response
    memcpy(resp.data() + 1, &challenge, 4);
    m_socket.SendTo(sess.ip, sess.port, resp.data(), (uint32_t)resp.size());
    Logger::Debug("EACServerEmulator: Sent challenge %u to %s:%u", challenge, sess.ip.c_str(), sess.port);
}

void EACServerEmulator::HandleHandshake(const ClientSession& sess, uint64_t clientNonce) {
    bool accept = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_alwaysAccept) {
            accept = true;
        } else {
            // Basic check: nonce echoed back correctly
            accept = (clientNonce == sess.lastChallenge);
        }
        
        // Update session state
        const_cast<ClientSession&>(sess).authenticated = accept;
        const_cast<ClientSession&>(sess).lastActivity = std::chrono::steady_clock::now();
    }

    SendHandshakeResponse(sess, accept);
}

void EACServerEmulator::SendHandshakeResponse(const ClientSession& sess, bool accepted) {
    std::vector<uint8_t> resp(2);
    resp[0] = 0x82; // handshake response
    resp[1] = accepted ? 1 : 0;
    m_socket.SendTo(sess.ip, sess.port, resp.data(), (uint32_t)resp.size());
    Logger::Info("EACServerEmulator: Handshake %s for %s:%u",
                 accepted ? "accepted" : "rejected",
                 sess.ip.c_str(), sess.port);
}

// Memory operation implementations
bool EACServerEmulator::SendMemoryRequest(uint32_t clientId, const MemoryRequestPacket& request,
                                         std::function<void(const MemoryReplyPacket&, const uint8_t*)> callback) {
    std::lock_guard<std::mutex> lock(m_memoryRequestMutex);

    auto it = m_sessions.find(clientId);
    if (it == m_sessions.end()) {
        Logger::Warn("SendMemoryRequest: client %u not found", clientId);
        return false;
    }

    if (!it->second.authenticated) {
        Logger::Warn("SendMemoryRequest: client %u not authenticated", clientId);
        return false;
    }

    // Assign a unique request ID
    uint32_t reqId = m_nextMemoryRequestId++;
    m_pendingMemoryRequests[reqId] = callback;

    // Build packet: [type][reqId][request]
    std::vector<uint8_t> buf(1 + sizeof(uint32_t) + sizeof(MemoryRequestPacket));
    buf[0] = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    std::memcpy(buf.data() + 1, &reqId, sizeof(reqId));
    std::memcpy(buf.data() + 1 + sizeof(reqId), &request, sizeof(request));

    const auto& sess = it->second;
    m_socket.SendTo(sess.ip, sess.port, buf.data(), buf.size());
    
    Logger::Debug("Sent memory request %u (op: %d, addr: 0x%llx, len: %u) to client %u", 
                  reqId, static_cast<int>(request.op), request.address, request.length, clientId);
    return true;
}

bool EACServerEmulator::RequestMemoryRead(uint32_t clientId, uint64_t address, uint32_t length,
                                         std::function<void(const uint8_t*, uint32_t)> callback) {
    MemoryRequestPacket pkt{};
    pkt.type = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    pkt.op = MemOp::Read;
    pkt.address = address;
    pkt.length = length;
    pkt.allocProtect = 0;

    // Wrap user callback
    return SendMemoryRequest(clientId, pkt, [callback](const MemoryReplyPacket& reply, const uint8_t* data) {
        if (reply.op == MemOp::Read && callback) {
            callback(data, reply.length);
        }
    });
}

bool EACServerEmulator::RequestMemoryWrite(uint32_t clientId, uint64_t address,
                                          const uint8_t* data, uint32_t length,
                                          std::function<void(bool)> callback) {
    if (length > sizeof(MemoryRequestPacket::data)) {
        Logger::Error("RequestMemoryWrite: length %u exceeds max %zu", length, sizeof(MemoryRequestPacket::data));
        return false;
    }

    MemoryRequestPacket pkt{};
    pkt.type = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    pkt.op = MemOp::Write;
    pkt.address = address;
    pkt.length = length;
    std::memcpy(pkt.data, data, length);

    return SendMemoryRequest(clientId, pkt, [callback](const MemoryReplyPacket& reply, const uint8_t*) {
        if (reply.op == MemOp::Write && callback) {
            callback(true);
        }
    });
}

bool EACServerEmulator::RequestMemoryAlloc(uint32_t clientId, uint32_t length, uint64_t protection,
                                          std::function<void(uint64_t)> callback) {
    MemoryRequestPacket pkt{};
    pkt.type = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    pkt.op = MemOp::Alloc;
    pkt.address = 0;
    pkt.length = length;
    pkt.allocProtect = protection;

    return SendMemoryRequest(clientId, pkt, [callback](const MemoryReplyPacket& reply, const uint8_t*) {
        if (reply.op == MemOp::Alloc && callback) {
            callback(reply.address);
        }
    });
}

void EACServerEmulator::BroadcastMemoryRead(uint64_t address, uint32_t length,
                                           std::function<void(uint32_t, const uint8_t*, uint32_t)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [clientId, session] : m_sessions) {
        if (session.authenticated) {
            RequestMemoryRead(clientId, address, length, [clientId, callback](const uint8_t* data, uint32_t len) {
                if (callback) {
                    callback(clientId, data, len);
                }
            });
        }
    }
}

void EACServerEmulator::BroadcastDebugDraw(const DebugDrawPacket& packet) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [clientId, session] : m_sessions) {
        if (session.authenticated) {
            m_socket.SendTo(session.ip, session.port, &packet, sizeof(packet));
        }
    }
}

void EACServerEmulator::ProcessRequests() {
    // This method is called from the main server loop if running in single-threaded mode
    // When using the threaded mode (which is default), this does nothing
    if (!m_running) {
        // Process one iteration manually
        std::string ip;
        uint16_t port = 0;
        std::vector<uint8_t> buf(1500);
        int len = m_socket.ReceiveFrom(ip, port, buf.data(), (int)buf.size());
        if (len > 0) {
            buf.resize(len);
            HandlePacket(ip, port, buf);
        }
    }
}

EACServerEmulator::ClientSession* EACServerEmulator::FindOrCreateSession(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // First try to find existing session
    for (auto& [clientId, session] : m_sessions) {
        if (session.ip == ip && session.port == port) {
            session.lastActivity = std::chrono::steady_clock::now();
            return &session;
        }
    }
    
    // Create new session
    uint32_t newClientId = GenerateClientId();
    ClientSession newSession;
    newSession.ip = ip;
    newSession.port = port;
    newSession.clientId = newClientId;
    newSession.lastChallenge = 0;
    newSession.handshakeNonce = GenerateNonce();
    newSession.authenticated = false;
    newSession.lastActivity = std::chrono::steady_clock::now();
    
    m_sessions[newClientId] = newSession;
    Logger::Debug("Created new session for %s:%u with clientId %u", ip.c_str(), port, newClientId);
    
    return &m_sessions[newClientId];
}

EACServerEmulator::ClientSession* EACServerEmulator::FindSessionByClientId(uint32_t clientId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(clientId);
    return (it != m_sessions.end()) ? &it->second : nullptr;
}

uint32_t EACServerEmulator::GenerateChallenge() {
    static std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return rng();
}

uint64_t EACServerEmulator::GenerateNonce() {
    static std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return rng();
}

uint32_t EACServerEmulator::GenerateClientId() {
    return m_nextClientId++;
}

void EACServerEmulator::CleanupStaleConnections() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (now - it->second.lastActivity > std::chrono::minutes(5)) {
            Logger::Debug("Removing stale session %u (%s:%u)", it->first, it->second.ip.c_str(), it->second.port);
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
    
    // Also cleanup stale memory requests
    std::lock_guard<std::mutex> memLock(m_memoryRequestMutex);
    // Note: In a production system, you'd want to track request timestamps and timeout old ones
    if (m_pendingMemoryRequests.size() > 1000) {
        Logger::Warn("Large number of pending memory requests: %zu", m_pendingMemoryRequests.size());
    }
}

void EACServerEmulator::Shutdown() {
    if (!m_running) return;
    
    Logger::Info("EACServerEmulator: Shutting down...");
    m_running = false;
    
    if (m_thread.joinable()) {
        m_thread.join();
    }
    
    m_socket.Close();
    
    // Cleanup pending requests
    {
        std::lock_guard<std::mutex> lock(m_memoryRequestMutex);
        m_pendingMemoryRequests.clear();
    }
    
    // Cleanup sessions
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.clear();
    }
    
    Logger::Info("EACServerEmulator: Shutdown complete");
}