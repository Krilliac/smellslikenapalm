// src/Security/EACServerEmulator.cpp

#include "Security/EACServerEmulator.h"
#include "Security/EACPackets.h"
#include "Utils/Logger.h"
#include "../../telemetry/TelemetryManager.h"
#include <cstring>
#include <chrono>
#include <algorithm>
#include <random>

EACServerEmulator::EACServerEmulator()
    : m_listenPort(0)
    , m_running(false)
    , m_safeMode(false)
    , m_alwaysAccept(false)
    , m_nextClientId(1)
    , m_nextMemoryRequestId(1)
{
    Logger::Trace("[EACServerEmulator::EACServerEmulator] Constructor called, listenPort=%u, running=%s, safeMode=%s, alwaysAccept=%s",
                  m_listenPort, m_running ? "true" : "false", m_safeMode ? "true" : "false", m_alwaysAccept ? "true" : "false");
}

EACServerEmulator::~EACServerEmulator() {
    Logger::Trace("[EACServerEmulator::~EACServerEmulator] Destructor called, invoking Shutdown");
    Shutdown();
    Logger::Trace("[EACServerEmulator::~EACServerEmulator] Destructor completed");
}

void EACServerEmulator::SetSafeMode(bool safe) {
    Logger::Trace("[EACServerEmulator::SetSafeMode] Entry, safe=%s", safe ? "true" : "false");
    std::lock_guard<std::mutex> lock(m_mutex);
    bool oldValue = m_safeMode;
    m_safeMode = safe;
    Logger::Debug("[EACServerEmulator::SetSafeMode] Safe mode changed from %s to %s",
                  oldValue ? "true" : "false", safe ? "true" : "false");
    Logger::Info("[EACServerEmulator::SetSafeMode] Safe mode is now %s", safe ? "enabled" : "disabled");
    Logger::Trace("[EACServerEmulator::SetSafeMode] Exit");
}

void EACServerEmulator::SetAlwaysAccept(bool accept) {
    Logger::Trace("[EACServerEmulator::SetAlwaysAccept] Entry, accept=%s", accept ? "true" : "false");
    std::lock_guard<std::mutex> lock(m_mutex);
    bool oldValue = m_alwaysAccept;
    m_alwaysAccept = accept;
    Logger::Debug("[EACServerEmulator::SetAlwaysAccept] AlwaysAccept changed from %s to %s",
                  oldValue ? "true" : "false", accept ? "true" : "false");
    Logger::Info("[EACServerEmulator::SetAlwaysAccept] AlwaysAccept mode is now %s", accept ? "enabled" : "disabled");
    Logger::Trace("[EACServerEmulator::SetAlwaysAccept] Exit");
}

bool EACServerEmulator::Initialize(uint16_t listenPort) {
    Logger::Trace("[EACServerEmulator::Initialize] Entry, listenPort=%u", listenPort);
    m_listenPort = listenPort;
    Logger::Debug("[EACServerEmulator::Initialize] Attempting to bind UDP socket on port %u", listenPort);
    if (!m_socket.Bind(listenPort)) {
        Logger::Error("EACServerEmulator: Failed to bind UDP on port %u", listenPort);
        Logger::Error("[EACServerEmulator::Initialize] UDP socket bind failed on port %u - cannot start emulator", listenPort);
        Logger::Trace("[EACServerEmulator::Initialize] Exit, returning false");
        return false;
    }
    Logger::Debug("[EACServerEmulator::Initialize] UDP socket bound successfully on port %u", listenPort);

    m_running = true;
    Logger::Debug("[EACServerEmulator::Initialize] Starting run loop thread");
    m_thread = std::thread(&EACServerEmulator::RunLoop, this);
    Logger::Info("EACServerEmulator: Listening on port %u", listenPort);
    Logger::Info("[EACServerEmulator::Initialize] EAC Server Emulator initialized and listening on port %u", listenPort);
    Logger::Trace("[EACServerEmulator::Initialize] Exit, returning true");
    return true;
}

void EACServerEmulator::RunLoop() {
    Logger::Trace("[EACServerEmulator::RunLoop] Entry - run loop thread started");
    Logger::Info("[EACServerEmulator::RunLoop] EAC Server Emulator run loop started");
    while (m_running) {
        std::string ip;
        uint16_t port = 0;
        std::vector<uint8_t> buf(1500);
        int len = m_socket.ReceiveFrom(ip, port, buf.data(), (int)buf.size());
        if (len > 0) {
            buf.resize(len);
            Logger::Trace("[EACServerEmulator::RunLoop] Received %d bytes from %s:%u", len, ip.c_str(), port);
            HandlePacket(ip, port, buf);
        }

        // Cleanup stale connections periodically
        static auto lastCleanup = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastCleanup > std::chrono::seconds(30)) {
            Logger::Debug("[EACServerEmulator::RunLoop] Triggering periodic stale connection cleanup");
            CleanupStaleConnections();
            lastCleanup = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Logger::Info("[EACServerEmulator::RunLoop] Run loop exiting (m_running=false)");
    Logger::Trace("[EACServerEmulator::RunLoop] Exit");
}

void EACServerEmulator::HandlePacket(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data) {
    Logger::Trace("[EACServerEmulator::HandlePacket] Entry, ip='%s', port=%u, data size=%zu",
                  ip.c_str(), port, data.size());
    if (data.empty()) {
        Logger::Debug("[EACServerEmulator::HandlePacket] Empty packet received from %s:%u, ignoring", ip.c_str(), port);
        Logger::Trace("[EACServerEmulator::HandlePacket] Exit (empty packet)");
        return;
    }

    uint8_t type = data[0];
    Logger::Debug("[EACServerEmulator::HandlePacket] Packet type=0x%02X from %s:%u, size=%zu bytes",
                  type, ip.c_str(), port, data.size());

    // Handle memory reply packets first
    if (type == static_cast<uint8_t>(EACMessageType::MemoryReply)) {
        Logger::Debug("[EACServerEmulator::HandlePacket] Received MemoryReply packet from %s:%u", ip.c_str(), port);
        HandleMemoryReply(0, data.data(), data.size()); // ClientId lookup needed
        Logger::Trace("[EACServerEmulator::HandlePacket] Exit (MemoryReply handled)");
        return;
    }

    // Handle debug draw commands
    if (type == static_cast<uint8_t>(EACMessageType::DebugDrawCommand)) {
        // Debug draw packets are handled by client, no server processing needed
        Logger::Debug("Debug draw command received from %s:%u", ip.c_str(), port);
        Logger::Debug("[EACServerEmulator::HandlePacket] Debug draw command - no server processing needed");
        Logger::Trace("[EACServerEmulator::HandlePacket] Exit (DebugDrawCommand)");
        return;
    }

    // Handle traditional EAC packets
    Logger::Debug("[EACServerEmulator::HandlePacket] Looking up or creating session for %s:%u", ip.c_str(), port);
    ClientSession* sess = FindOrCreateSession(ip, port);
    if (!sess) {
        Logger::Error("[EACServerEmulator::HandlePacket] Failed to find or create session for %s:%u", ip.c_str(), port);
        Logger::Trace("[EACServerEmulator::HandlePacket] Exit (no session)");
        return;
    }
    Logger::Debug("[EACServerEmulator::HandlePacket] Session found/created for %s:%u, clientId=%u",
                  ip.c_str(), port, sess->clientId);

    switch (type) {
        case 0x01: {
            Logger::Debug("Challenge request from %s:%u", ip.c_str(), port);
            Logger::Info("[EACServerEmulator::HandlePacket] Processing challenge request from %s:%u (clientId=%u)",
                         ip.c_str(), port, sess->clientId);
            SendChallenge(*sess);
            break;
        }
        case 0x02: {
            if (data.size() >= 9) {
                uint64_t clientNonce = 0;
                memcpy(&clientNonce, data.data() + 1, 8);
                Logger::Debug("Handshake request from %s:%u with nonce %llu", ip.c_str(), port, clientNonce);
                Logger::Info("[EACServerEmulator::HandlePacket] Processing handshake request from %s:%u (clientId=%u, nonce=%llu)",
                             ip.c_str(), port, sess->clientId, static_cast<unsigned long long>(clientNonce));
                HandleHandshake(*sess, clientNonce);
            } else {
                Logger::Warn("[EACServerEmulator::HandlePacket] Handshake packet too short from %s:%u (size=%zu, need>=9)",
                             ip.c_str(), port, data.size());
            }
            break;
        }
        default:
            Logger::Warn("EACServerEmulator: Unknown packet type 0x%02X from %s:%u", type, ip.c_str(), port);
            Logger::Debug("[EACServerEmulator::HandlePacket] Unrecognized packet type 0x%02X, no handler available", type);
    }
    Logger::Trace("[EACServerEmulator::HandlePacket] Exit");
}

void EACServerEmulator::HandleMemoryReply(uint32_t clientId, const uint8_t* data, size_t dataSize) {
    Logger::Trace("[EACServerEmulator::HandleMemoryReply] Entry, clientId=%u, dataSize=%zu", clientId, dataSize);
    if (dataSize < 1 + sizeof(uint32_t) + sizeof(MemoryReplyPacket)) {
        Logger::Error("[EACServerEmulator::HandleMemoryReply] Packet too small: %zu bytes (need >= %zu)",
                      dataSize, 1 + sizeof(uint32_t) + sizeof(MemoryReplyPacket));
        Logger::Trace("[EACServerEmulator::HandleMemoryReply] Exit (packet too small)");
        return;
    }

    // Extract request ID
    uint32_t reqId;
    std::memcpy(&reqId, data + 1, sizeof(reqId));
    Logger::Debug("[EACServerEmulator::HandleMemoryReply] Extracted request ID=%u", reqId);

    // Extract reply packet
    MemoryReplyPacket reply{};
    std::memcpy(&reply, data + 1 + sizeof(reqId), sizeof(reply));
    Logger::Debug("[EACServerEmulator::HandleMemoryReply] Reply packet: op=%d, address=0x%llx, length=%u",
                  static_cast<int>(reply.op), static_cast<unsigned long long>(reply.address), reply.length);

    // Extract payload data
    const uint8_t* payload = data + 1 + sizeof(reqId) + sizeof(reply);
    size_t payloadSize = dataSize - (1 + sizeof(reqId) + sizeof(reply));
    Logger::Debug("[EACServerEmulator::HandleMemoryReply] Payload size=%zu bytes", payloadSize);

    // GUARD (out-of-bounds read): reply.length is fully wire-controlled (up to 4 GB) but the
    // payload that actually follows the header is only payloadSize bytes. A consumer that trusts
    // reply.length as the readable size (RequestMemoryRead forwards it verbatim to its callback)
    // would read past the buffer. Clamp reply.length to the bytes we actually received before
    // handing the reply to any callback.
    if (reply.length > payloadSize) {
        Logger::Warn("[EACServerEmulator::HandleMemoryReply] reply.length=%u exceeds received payload=%zu for request %u; clamping",
                     reply.length, payloadSize, reqId);
        reply.length = static_cast<uint32_t>(payloadSize);
    }

    // Find and execute callback
    std::function<void(const MemoryReplyPacket&, const uint8_t*)> callback;
    {
        std::lock_guard<std::mutex> lock(m_memoryRequestMutex);
        auto it = m_pendingMemoryRequests.find(reqId);
        if (it != m_pendingMemoryRequests.end()) {
            callback = it->second;
            m_pendingMemoryRequests.erase(it);
            Logger::Debug("[EACServerEmulator::HandleMemoryReply] Found pending callback for request %u, removed from pending map (remaining=%zu)",
                          reqId, m_pendingMemoryRequests.size());
        } else {
            Logger::Debug("[EACServerEmulator::HandleMemoryReply] No pending callback found for request %u in map (map size=%zu)",
                          reqId, m_pendingMemoryRequests.size());
        }
    }

    if (callback) {
        Logger::Debug("Executing memory reply callback for request %u, op %d", reqId, static_cast<int>(reply.op));
        Logger::Info("[EACServerEmulator::HandleMemoryReply] Executing memory reply callback for request %u", reqId);
        callback(reply, payload);
        Logger::Trace("[EACServerEmulator::HandleMemoryReply] Callback executed successfully for request %u", reqId);
    } else {
        Logger::Warn("No callback found for memory request %u", reqId);
    }
    Logger::Trace("[EACServerEmulator::HandleMemoryReply] Exit");
}

void EACServerEmulator::SendChallenge(const ClientSession& sess) {
    Logger::Trace("[EACServerEmulator::SendChallenge] Entry, clientId=%u, ip='%s', port=%u",
                  sess.clientId, sess.ip.c_str(), sess.port);
    uint32_t challenge = GenerateChallenge();
    Logger::Debug("[EACServerEmulator::SendChallenge] Generated challenge value=%u for client %u", challenge, sess.clientId);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const_cast<ClientSession&>(sess).lastChallenge = challenge;
        const_cast<ClientSession&>(sess).lastActivity = std::chrono::steady_clock::now();
        Logger::Debug("[EACServerEmulator::SendChallenge] Updated session: lastChallenge=%u, lastActivity updated for client %u",
                      challenge, sess.clientId);
    }

    std::vector<uint8_t> resp(5);
    resp[0] = 0x81; // challenge response
    memcpy(resp.data() + 1, &challenge, 4);
    Logger::Debug("[EACServerEmulator::SendChallenge] Sending challenge response packet (5 bytes, type=0x81) to %s:%u",
                  sess.ip.c_str(), sess.port);
    m_socket.SendTo(sess.ip, sess.port, resp.data(), (uint32_t)resp.size());
    Logger::Debug("EACServerEmulator: Sent challenge %u to %s:%u", challenge, sess.ip.c_str(), sess.port);
    Logger::Info("[EACServerEmulator::SendChallenge] Challenge sent to client %u at %s:%u",
                 sess.clientId, sess.ip.c_str(), sess.port);
    Logger::Trace("[EACServerEmulator::SendChallenge] Exit");
}

void EACServerEmulator::HandleHandshake(const ClientSession& sess, uint64_t clientNonce) {
    Logger::Trace("[EACServerEmulator::HandleHandshake] Entry, clientId=%u, ip='%s', port=%u, clientNonce=%llu",
                  sess.clientId, sess.ip.c_str(), sess.port, static_cast<unsigned long long>(clientNonce));
    bool accept = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_alwaysAccept) {
            accept = true;
            Logger::Debug("[EACServerEmulator::HandleHandshake] AlwaysAccept mode enabled, accepting handshake for client %u",
                          sess.clientId);
        } else {
            // Basic check: nonce echoed back correctly
            accept = (clientNonce == sess.lastChallenge);
            Logger::Debug("[EACServerEmulator::HandleHandshake] Nonce check for client %u: clientNonce=%llu, lastChallenge=%u, match=%s",
                          sess.clientId, static_cast<unsigned long long>(clientNonce), sess.lastChallenge,
                          accept ? "true" : "false");
        }

        // Update session state
        const_cast<ClientSession&>(sess).authenticated = accept;
        const_cast<ClientSession&>(sess).lastActivity = std::chrono::steady_clock::now();
        Logger::Debug("[EACServerEmulator::HandleHandshake] Session state updated for client %u: authenticated=%s",
                      sess.clientId, accept ? "true" : "false");
    }

    Logger::Info("[EACServerEmulator::HandleHandshake] Handshake result for client %u: %s",
                 sess.clientId, accept ? "ACCEPTED" : "REJECTED");
    if (!accept) {
        TELEMETRY_INCREMENT_SECURITY_VIOLATION();
    }
    SendHandshakeResponse(sess, accept);
    Logger::Trace("[EACServerEmulator::HandleHandshake] Exit");
}

void EACServerEmulator::SendHandshakeResponse(const ClientSession& sess, bool accepted) {
    Logger::Trace("[EACServerEmulator::SendHandshakeResponse] Entry, clientId=%u, ip='%s', port=%u, accepted=%s",
                  sess.clientId, sess.ip.c_str(), sess.port, accepted ? "true" : "false");
    std::vector<uint8_t> resp(2);
    resp[0] = 0x82; // handshake response
    resp[1] = accepted ? 1 : 0;
    Logger::Debug("[EACServerEmulator::SendHandshakeResponse] Sending handshake response packet (2 bytes, type=0x82, accepted=%d) to %s:%u",
                  accepted ? 1 : 0, sess.ip.c_str(), sess.port);
    m_socket.SendTo(sess.ip, sess.port, resp.data(), (uint32_t)resp.size());
    Logger::Info("EACServerEmulator: Handshake %s for %s:%u",
                 accepted ? "accepted" : "rejected",
                 sess.ip.c_str(), sess.port);
    Logger::Trace("[EACServerEmulator::SendHandshakeResponse] Exit");
}

// Memory operation implementations
bool EACServerEmulator::SendMemoryRequest(uint32_t clientId, const MemoryRequestPacket& request,
                                         std::function<void(const MemoryReplyPacket&, const uint8_t*)> callback) {
    Logger::Trace("[EACServerEmulator::SendMemoryRequest] Entry, clientId=%u, op=%d, address=0x%llx, length=%u",
                  clientId, static_cast<int>(request.op), static_cast<unsigned long long>(request.address), request.length);
    std::lock_guard<std::mutex> lock(m_memoryRequestMutex);

    auto it = m_sessions.find(clientId);
    if (it == m_sessions.end()) {
        Logger::Warn("SendMemoryRequest: client %u not found", clientId);
        Logger::Debug("[EACServerEmulator::SendMemoryRequest] Client %u not found in sessions map (map size=%zu)",
                      clientId, m_sessions.size());
        Logger::Trace("[EACServerEmulator::SendMemoryRequest] Exit, returning false (client not found)");
        return false;
    }

    if (!it->second.authenticated) {
        Logger::Warn("SendMemoryRequest: client %u not authenticated", clientId);
        Logger::Debug("[EACServerEmulator::SendMemoryRequest] Client %u exists but is not authenticated, rejecting memory request",
                      clientId);
        Logger::Trace("[EACServerEmulator::SendMemoryRequest] Exit, returning false (not authenticated)");
        return false;
    }
    Logger::Debug("[EACServerEmulator::SendMemoryRequest] Client %u is authenticated, proceeding with memory request", clientId);

    // Assign a unique request ID
    uint32_t reqId = m_nextMemoryRequestId++;
    m_pendingMemoryRequests[reqId] = callback;
    Logger::Debug("[EACServerEmulator::SendMemoryRequest] Assigned request ID=%u, total pending requests=%zu",
                  reqId, m_pendingMemoryRequests.size());

    // Build packet: [type][reqId][request]
    std::vector<uint8_t> buf(1 + sizeof(uint32_t) + sizeof(MemoryRequestPacket));
    buf[0] = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    std::memcpy(buf.data() + 1, &reqId, sizeof(reqId));
    std::memcpy(buf.data() + 1 + sizeof(reqId), &request, sizeof(request));
    Logger::Debug("[EACServerEmulator::SendMemoryRequest] Built memory request packet: %zu bytes", buf.size());

    const auto& sess = it->second;
    m_socket.SendTo(sess.ip, sess.port, buf.data(), buf.size());

    Logger::Debug("Sent memory request %u (op: %d, addr: 0x%llx, len: %u) to client %u",
                  reqId, static_cast<int>(request.op), request.address, request.length, clientId);
    Logger::Info("[EACServerEmulator::SendMemoryRequest] Memory request %u sent to client %u at %s:%u",
                 reqId, clientId, sess.ip.c_str(), sess.port);
    Logger::Trace("[EACServerEmulator::SendMemoryRequest] Exit, returning true");
    return true;
}

bool EACServerEmulator::RequestMemoryRead(uint32_t clientId, uint64_t address, uint32_t length,
                                         std::function<void(const uint8_t*, uint32_t)> callback) {
    Logger::Trace("[EACServerEmulator::RequestMemoryRead] Entry, clientId=%u, address=0x%llx, length=%u",
                  clientId, static_cast<unsigned long long>(address), length);
    MemoryRequestPacket pkt{};
    pkt.type = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    pkt.op = MemOp::Read;
    pkt.address = address;
    pkt.length = length;
    pkt.allocProtect = 0;
    Logger::Debug("[EACServerEmulator::RequestMemoryRead] Built MemoryRequestPacket: op=Read, address=0x%llx, length=%u",
                  static_cast<unsigned long long>(address), length);

    // Wrap user callback
    bool result = SendMemoryRequest(clientId, pkt, [callback, clientId](const MemoryReplyPacket& reply, const uint8_t* data) {
        Logger::Trace("[EACServerEmulator::RequestMemoryRead::callback] Memory read reply for client %u: op=%d, length=%u",
                      clientId, static_cast<int>(reply.op), reply.length);
        if (reply.op == MemOp::Read && callback) {
            Logger::Debug("[EACServerEmulator::RequestMemoryRead::callback] Invoking user callback with %u bytes of data for client %u",
                          reply.length, clientId);
            callback(data, reply.length);
        } else {
            Logger::Debug("[EACServerEmulator::RequestMemoryRead::callback] Reply op mismatch or null callback for client %u (op=%d)",
                          clientId, static_cast<int>(reply.op));
        }
    });
    Logger::Trace("[EACServerEmulator::RequestMemoryRead] Exit, returning %s", result ? "true" : "false");
    return result;
}

bool EACServerEmulator::RequestMemoryWrite(uint32_t clientId, uint64_t address,
                                          const uint8_t* data, uint32_t length,
                                          std::function<void(bool)> callback) {
    Logger::Trace("[EACServerEmulator::RequestMemoryWrite] Entry, clientId=%u, address=0x%llx, length=%u",
                  clientId, static_cast<unsigned long long>(address), length);
    if (length > sizeof(MemoryRequestPacket::data)) {
        Logger::Error("RequestMemoryWrite: length %u exceeds max %zu", length, sizeof(MemoryRequestPacket::data));
        Logger::Error("[EACServerEmulator::RequestMemoryWrite] Write length %u exceeds maximum buffer size %zu for client %u",
                      length, sizeof(MemoryRequestPacket::data), clientId);
        Logger::Trace("[EACServerEmulator::RequestMemoryWrite] Exit, returning false (length exceeded)");
        return false;
    }

    MemoryRequestPacket pkt{};
    pkt.type = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    pkt.op = MemOp::Write;
    pkt.address = address;
    pkt.length = length;
    std::memcpy(pkt.data, data, length);
    Logger::Debug("[EACServerEmulator::RequestMemoryWrite] Built MemoryRequestPacket: op=Write, address=0x%llx, length=%u",
                  static_cast<unsigned long long>(address), length);

    bool result = SendMemoryRequest(clientId, pkt, [callback, clientId](const MemoryReplyPacket& reply, const uint8_t*) {
        Logger::Trace("[EACServerEmulator::RequestMemoryWrite::callback] Memory write reply for client %u: op=%d",
                      clientId, static_cast<int>(reply.op));
        if (reply.op == MemOp::Write && callback) {
            Logger::Debug("[EACServerEmulator::RequestMemoryWrite::callback] Write confirmed for client %u, invoking user callback",
                          clientId);
            callback(true);
        } else {
            Logger::Debug("[EACServerEmulator::RequestMemoryWrite::callback] Reply op mismatch or null callback for client %u",
                          clientId);
        }
    });
    Logger::Trace("[EACServerEmulator::RequestMemoryWrite] Exit, returning %s", result ? "true" : "false");
    return result;
}

bool EACServerEmulator::RequestMemoryAlloc(uint32_t clientId, uint32_t length, uint64_t protection,
                                          std::function<void(uint64_t)> callback) {
    Logger::Trace("[EACServerEmulator::RequestMemoryAlloc] Entry, clientId=%u, length=%u, protection=0x%llx",
                  clientId, length, static_cast<unsigned long long>(protection));
    MemoryRequestPacket pkt{};
    pkt.type = static_cast<uint8_t>(EACMessageType::MemoryRequest);
    pkt.op = MemOp::Alloc;
    pkt.address = 0;
    pkt.length = length;
    pkt.allocProtect = protection;
    Logger::Debug("[EACServerEmulator::RequestMemoryAlloc] Built MemoryRequestPacket: op=Alloc, length=%u, protection=0x%llx",
                  length, static_cast<unsigned long long>(protection));

    bool result = SendMemoryRequest(clientId, pkt, [callback, clientId](const MemoryReplyPacket& reply, const uint8_t*) {
        Logger::Trace("[EACServerEmulator::RequestMemoryAlloc::callback] Memory alloc reply for client %u: op=%d, address=0x%llx",
                      clientId, static_cast<int>(reply.op), static_cast<unsigned long long>(reply.address));
        if (reply.op == MemOp::Alloc && callback) {
            Logger::Debug("[EACServerEmulator::RequestMemoryAlloc::callback] Allocation returned address 0x%llx for client %u",
                          static_cast<unsigned long long>(reply.address), clientId);
            callback(reply.address);
        } else {
            Logger::Debug("[EACServerEmulator::RequestMemoryAlloc::callback] Reply op mismatch or null callback for client %u",
                          clientId);
        }
    });
    Logger::Trace("[EACServerEmulator::RequestMemoryAlloc] Exit, returning %s", result ? "true" : "false");
    return result;
}

void EACServerEmulator::BroadcastMemoryRead(uint64_t address, uint32_t length,
                                           std::function<void(uint32_t, const uint8_t*, uint32_t)> callback) {
    Logger::Trace("[EACServerEmulator::BroadcastMemoryRead] Entry, address=0x%llx, length=%u",
                  static_cast<unsigned long long>(address), length);
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t authenticatedCount = 0;
    for (const auto& [clientId, session] : m_sessions) {
        if (session.authenticated) {
            Logger::Debug("[EACServerEmulator::BroadcastMemoryRead] Sending memory read request to authenticated client %u (%s:%u)",
                          clientId, session.ip.c_str(), session.port);
            RequestMemoryRead(clientId, address, length, [clientId, callback](const uint8_t* data, uint32_t len) {
                Logger::Trace("[EACServerEmulator::BroadcastMemoryRead::callback] Broadcast read reply from client %u: %u bytes",
                              clientId, len);
                if (callback) {
                    callback(clientId, data, len);
                }
            });
            ++authenticatedCount;
        } else {
            Logger::Trace("[EACServerEmulator::BroadcastMemoryRead] Skipping unauthenticated client %u", clientId);
        }
    }
    Logger::Info("[EACServerEmulator::BroadcastMemoryRead] Broadcast memory read sent to %zu authenticated clients (address=0x%llx, length=%u)",
                 authenticatedCount, static_cast<unsigned long long>(address), length);
    Logger::Trace("[EACServerEmulator::BroadcastMemoryRead] Exit");
}

void EACServerEmulator::BroadcastDebugDraw(const DebugDrawPacket& packet) {
    Logger::Trace("[EACServerEmulator::BroadcastDebugDraw] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t sentCount = 0;
    for (const auto& [clientId, session] : m_sessions) {
        if (session.authenticated) {
            Logger::Trace("[EACServerEmulator::BroadcastDebugDraw] Sending debug draw packet to client %u (%s:%u)",
                          clientId, session.ip.c_str(), session.port);
            m_socket.SendTo(session.ip, session.port, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
            ++sentCount;
        }
    }
    Logger::Debug("[EACServerEmulator::BroadcastDebugDraw] Debug draw packet broadcast to %zu authenticated clients", sentCount);
    Logger::Trace("[EACServerEmulator::BroadcastDebugDraw] Exit");
}

void EACServerEmulator::ProcessRequests() {
    Logger::Trace("[EACServerEmulator::ProcessRequests] Entry, m_running=%s", m_running ? "true" : "false");
    // This method is called from the main server loop if running in single-threaded mode
    // When using the threaded mode (which is default), this does nothing
    if (!m_running) {
        Logger::Debug("[EACServerEmulator::ProcessRequests] Not running in threaded mode, processing one iteration manually");
        // Process one iteration manually
        std::string ip;
        uint16_t port = 0;
        std::vector<uint8_t> buf(1500);
        int len = m_socket.ReceiveFrom(ip, port, buf.data(), (int)buf.size());
        if (len > 0) {
            buf.resize(len);
            Logger::Debug("[EACServerEmulator::ProcessRequests] Received %d bytes from %s:%u in manual poll", len, ip.c_str(), port);
            HandlePacket(ip, port, buf);
        } else {
            Logger::Trace("[EACServerEmulator::ProcessRequests] No data received in manual poll");
        }
    } else {
        Logger::Trace("[EACServerEmulator::ProcessRequests] Running in threaded mode, no manual processing needed");
    }
    Logger::Trace("[EACServerEmulator::ProcessRequests] Exit");
}

EACServerEmulator::ClientSession* EACServerEmulator::FindOrCreateSession(const std::string& ip, uint16_t port) {
    Logger::Trace("[EACServerEmulator::FindOrCreateSession] Entry, ip='%s', port=%u", ip.c_str(), port);
    std::lock_guard<std::mutex> lock(m_mutex);

    // First try to find existing session
    for (auto& [clientId, session] : m_sessions) {
        if (session.ip == ip && session.port == port) {
            session.lastActivity = std::chrono::steady_clock::now();
            Logger::Debug("[EACServerEmulator::FindOrCreateSession] Found existing session for %s:%u -> clientId=%u",
                          ip.c_str(), port, clientId);
            Logger::Trace("[EACServerEmulator::FindOrCreateSession] Exit, returning existing session");
            return &session;
        }
    }

    // Sessions are keyed off the (spoofable) UDP source ip:port, so an attacker
    // can otherwise create unlimited sessions and exhaust memory before the
    // 5-minute stale cleanup runs. Cap the table: once full, evict the
    // least-recently-active session to make room for the new one.
    constexpr size_t kMaxSessions = 4096;
    if (m_sessions.size() >= kMaxSessions) {
        auto oldest = m_sessions.end();
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            if (oldest == m_sessions.end() ||
                it->second.lastActivity < oldest->second.lastActivity) {
                oldest = it;
            }
        }
        if (oldest != m_sessions.end()) {
            Logger::Warn("[EACServerEmulator::FindOrCreateSession] Session cap (%zu) reached; "
                         "evicting least-recently-active session clientId=%u (%s:%u) to admit %s:%u",
                         kMaxSessions, oldest->first, oldest->second.ip.c_str(),
                         oldest->second.port, ip.c_str(), port);
            m_sessions.erase(oldest);
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
    Logger::Info("[EACServerEmulator::FindOrCreateSession] New client session created: clientId=%u, ip='%s', port=%u, total sessions=%zu",
                 newClientId, ip.c_str(), port, m_sessions.size());
    Logger::Trace("[EACServerEmulator::FindOrCreateSession] Exit, returning new session");
    return &m_sessions[newClientId];
}

EACServerEmulator::ClientSession* EACServerEmulator::FindSessionByClientId(uint32_t clientId) {
    Logger::Trace("[EACServerEmulator::FindSessionByClientId] Entry, clientId=%u", clientId);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(clientId);
    if (it != m_sessions.end()) {
        Logger::Debug("[EACServerEmulator::FindSessionByClientId] Found session for clientId=%u (ip='%s', port=%u, authenticated=%s)",
                      clientId, it->second.ip.c_str(), it->second.port, it->second.authenticated ? "true" : "false");
        Logger::Trace("[EACServerEmulator::FindSessionByClientId] Exit, returning session pointer");
        return &it->second;
    }
    Logger::Debug("[EACServerEmulator::FindSessionByClientId] No session found for clientId=%u", clientId);
    Logger::Trace("[EACServerEmulator::FindSessionByClientId] Exit, returning nullptr");
    return nullptr;
}

uint32_t EACServerEmulator::GenerateChallenge() {
    Logger::Trace("[EACServerEmulator::GenerateChallenge] Entry");
    static std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint32_t challenge = rng();
    Logger::Debug("[EACServerEmulator::GenerateChallenge] Generated challenge value=%u", challenge);
    Logger::Trace("[EACServerEmulator::GenerateChallenge] Exit, returning %u", challenge);
    return challenge;
}

uint64_t EACServerEmulator::GenerateNonce() {
    Logger::Trace("[EACServerEmulator::GenerateNonce] Entry");
    static std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint64_t nonce = rng();
    Logger::Debug("[EACServerEmulator::GenerateNonce] Generated nonce=%llu", static_cast<unsigned long long>(nonce));
    Logger::Trace("[EACServerEmulator::GenerateNonce] Exit, returning nonce");
    return nonce;
}

uint32_t EACServerEmulator::GenerateClientId() {
    Logger::Trace("[EACServerEmulator::GenerateClientId] Entry");
    uint32_t id = m_nextClientId++;
    Logger::Debug("[EACServerEmulator::GenerateClientId] Generated clientId=%u, next will be %u", id, m_nextClientId);
    Logger::Trace("[EACServerEmulator::GenerateClientId] Exit, returning %u", id);
    return id;
}

void EACServerEmulator::CleanupStaleConnections() {
    Logger::Trace("[EACServerEmulator::CleanupStaleConnections] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    size_t removedCount = 0;

    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        auto inactiveMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.lastActivity).count();
        if (now - it->second.lastActivity > std::chrono::minutes(5)) {
            Logger::Debug("Removing stale session %u (%s:%u)", it->first, it->second.ip.c_str(), it->second.port);
            Logger::Info("[EACServerEmulator::CleanupStaleConnections] Removing stale session: clientId=%u, ip='%s', port=%u, inactive for %lld ms",
                         it->first, it->second.ip.c_str(), it->second.port, static_cast<long long>(inactiveMs));
            it = m_sessions.erase(it);
            ++removedCount;
        } else {
            Logger::Trace("[EACServerEmulator::CleanupStaleConnections] Session %u (%s:%u) is still active (inactive for %lld ms)",
                          it->first, it->second.ip.c_str(), it->second.port, static_cast<long long>(inactiveMs));
            ++it;
        }
    }

    if (removedCount > 0) {
        Logger::Info("[EACServerEmulator::CleanupStaleConnections] Removed %zu stale sessions, %zu remaining",
                     removedCount, m_sessions.size());
    }

    // Also cleanup stale memory requests
    std::lock_guard<std::mutex> memLock(m_memoryRequestMutex);
    // Note: In a production system, you'd want to track request timestamps and timeout old ones
    if (m_pendingMemoryRequests.size() > 1000) {
        Logger::Warn("Large number of pending memory requests: %zu", m_pendingMemoryRequests.size());
        Logger::Warn("[EACServerEmulator::CleanupStaleConnections] Pending memory requests exceeding threshold: %zu (>1000)",
                     m_pendingMemoryRequests.size());
    } else {
        Logger::Debug("[EACServerEmulator::CleanupStaleConnections] Pending memory requests: %zu", m_pendingMemoryRequests.size());
    }
    Logger::Trace("[EACServerEmulator::CleanupStaleConnections] Exit");
}

void EACServerEmulator::Shutdown() {
    Logger::Trace("[EACServerEmulator::Shutdown] Entry, m_running=%s", m_running ? "true" : "false");
    if (!m_running) {
        Logger::Debug("[EACServerEmulator::Shutdown] Already shut down, nothing to do");
        Logger::Trace("[EACServerEmulator::Shutdown] Exit (already shut down)");
        return;
    }

    Logger::Info("EACServerEmulator: Shutting down...");
    Logger::Info("[EACServerEmulator::Shutdown] Beginning EAC Server Emulator shutdown");
    m_running = false;
    Logger::Debug("[EACServerEmulator::Shutdown] m_running set to false, waiting for run loop thread to join");

    if (m_thread.joinable()) {
        m_thread.join();
        Logger::Debug("[EACServerEmulator::Shutdown] Run loop thread joined successfully");
    } else {
        Logger::Debug("[EACServerEmulator::Shutdown] Run loop thread was not joinable");
    }

    Logger::Debug("[EACServerEmulator::Shutdown] Closing UDP socket");
    m_socket.Close();

    // Cleanup pending requests
    {
        std::lock_guard<std::mutex> lock(m_memoryRequestMutex);
        size_t pendingCount = m_pendingMemoryRequests.size();
        m_pendingMemoryRequests.clear();
        Logger::Debug("[EACServerEmulator::Shutdown] Cleared %zu pending memory requests", pendingCount);
    }

    // Cleanup sessions
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t sessionCount = m_sessions.size();
        m_sessions.clear();
        Logger::Debug("[EACServerEmulator::Shutdown] Cleared %zu client sessions", sessionCount);
    }

    Logger::Info("EACServerEmulator: Shutdown complete");
    Logger::Info("[EACServerEmulator::Shutdown] EAC Server Emulator shutdown complete");
    Logger::Trace("[EACServerEmulator::Shutdown] Exit");
}
