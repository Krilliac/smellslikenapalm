// src/Security/EACServerEmulator.h
#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include "Network/UDPSocket.h"
#include "Network/Packet.h"

class EACServerEmulator {
public:
    EACServerEmulator();
    ~EACServerEmulator();

    // Configure emulator behavior
    void SetSafeMode(bool safe);          // respond with “all clients safe”
    void SetAlwaysAccept(bool accept);    // automatically accept all handshakes

    // Initialize listening on the EAC port (default 7957)
    bool Initialize(uint16_t listenPort = 7957);

    // Process any pending client requests (should be called each tick)
    void ProcessRequests();

    // Shut down the emulator and stop listening
    void Shutdown();

private:
    UDPSocket                         m_socket;
    uint16_t                          m_listenPort;
    std::thread                       m_thread;
    std::atomic<bool>                 m_running;
    std::mutex                        m_mutex;
    bool                              m_safeMode;
    bool                              m_alwaysAccept;

    struct ClientSession {
        std::string                   ip;
        uint16_t                      port;
        uint32_t                      lastChallenge;
        uint64_t                      handshakeNonce;
    };
    std::vector<ClientSession>        m_sessions;

    // Internal handlers
    void RunLoop();
    void HandlePacket(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data);
    void SendChallenge(const ClientSession& sess);
    void HandleHandshake(const ClientSession& sess, uint64_t clientNonce);
    void SendHandshakeResponse(const ClientSession& sess, bool accepted);

    // Helpers
    ClientSession* FindOrCreateSession(const std::string& ip, uint16_t port);
    uint32_t GenerateChallenge();
    uint64_t GenerateNonce();
};