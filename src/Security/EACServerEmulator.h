// src/Security/EACServerEmulator.h

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <functional>
#include "Network/UDPSocket.h"
#include "Network/Packet.h"
#include "EACPackets.h"

class EACServerEmulator {
public:
    EACServerEmulator();
    ~EACServerEmulator();

    // Configure emulator behavior
    void SetSafeMode(bool safe);                    // respond with "all clients safe"
    void SetAlwaysAccept(bool accept);              // automatically accept all handshakes

    // Initialize listening on the EAC port (default 7957)
    bool Initialize(uint16_t listenPort = 7957);

    // Process any pending client requests (should be called each tick)
    void ProcessRequests();

    // Shut down the emulator and stop listening
    void Shutdown();

    // Memory operation requests
    bool RequestMemoryRead(uint32_t clientId, uint64_t address, uint32_t length, 
                          std::function<void(const uint8_t*, uint32_t)> callback);
    
    bool RequestMemoryWrite(uint32_t clientId, uint64_t address, 
                           const uint8_t* data, uint32_t length,
                           std::function<void(bool)> callback = nullptr);
    
    bool RequestMemoryAlloc(uint32_t clientId, uint32_t length, uint64_t protection,
                           std::function<void(uint64_t)> callback);
    
    // Broadcast memory read to all connected clients
    void BroadcastMemoryRead(uint64_t address, uint32_t length,
                            std::function<void(uint32_t, const uint8_t*, uint32_t)> callback);

    // Debug drawing support
    void BroadcastDebugDraw(const DebugDrawPacket& packet);

private:
    UDPSocket m_socket;
    uint16_t m_listenPort;
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
    bool m_safeMode;
    bool m_alwaysAccept;

    // Client session management
    struct ClientSession {
        std::string ip;
        uint16_t port;
        uint32_t lastChallenge;
        uint64_t handshakeNonce;
        uint32_t clientId;                          // Unique client identifier
        bool authenticated;                         // Authentication state
        std::chrono::steady_clock::time_point lastActivity;
    };

    std::map<uint32_t, ClientSession> m_sessions;   // Changed to map for easier lookup
    uint32_t m_nextClientId;                        // For generating unique client IDs

    // Memory operation tracking
    std::map<uint32_t, std::function<void(const MemoryReplyPacket&, const uint8_t*)>> m_pendingMemoryRequests;
    uint32_t m_nextMemoryRequestId;
    std::mutex m_memoryRequestMutex;

    // Internal handlers
    void RunLoop();
    void HandlePacket(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data);
    void HandleMemoryReply(uint32_t clientId, const uint8_t* data, size_t dataSize);
    
    // EAC protocol handlers
    void SendChallenge(const ClientSession& sess);
    void HandleHandshake(const ClientSession& sess, uint64_t clientNonce);
    void SendHandshakeResponse(const ClientSession& sess, bool accepted);

    // Memory operation helpers
    bool SendMemoryRequest(uint32_t clientId, const MemoryRequestPacket& request, 
                          std::function<void(const MemoryReplyPacket&, const uint8_t*)> callback);

    // Utility functions
    ClientSession* FindOrCreateSession(const std::string& ip, uint16_t port);
    ClientSession* FindSessionByClientId(uint32_t clientId);
    uint32_t GenerateChallenge();
    uint64_t GenerateNonce();
    uint32_t GenerateClientId();
    
    // Session cleanup
    void CleanupStaleConnections();
};