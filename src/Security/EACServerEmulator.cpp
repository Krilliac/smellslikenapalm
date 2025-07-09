// src/Security/EACServerEmulator.cpp
#include "Security/EACServerEmulator.h"
#include "Utils/Logger.h"
#include <chrono>
#include <random>
#include <algorithm>

EACServerEmulator::EACServerEmulator()
    : m_listenPort(0)
    , m_running(false)
    , m_safeMode(false)
    , m_alwaysAccept(false)
{}

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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void EACServerEmulator::HandlePacket(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data) {
    // Simple protocol: first byte indicates message type
    // 0x01 = challenge request, 0x02 = handshake request
    if (data.empty()) return;
    uint8_t type = data[0];
    ClientSession* sess = FindOrCreateSession(ip, port);
    if (!sess) return;

    switch (type) {
        case 0x01:
            SendChallenge(*sess);
            break;
        case 0x02: {
            if (data.size() >= 9) {
                uint64_t clientNonce = 0;
                memcpy(&clientNonce, data.data() + 1, 8);
                HandleHandshake(*sess, clientNonce);
            }
            break;
        }
        default:
            Logger::Warn("EACServerEmulator: Unknown packet type 0x%02X from %s:%u", type, ip.c_str(), port);
    }
}

void EACServerEmulator::SendChallenge(const ClientSession& sess) {
    uint32_t challenge = GenerateChallenge();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const_cast<ClientSession&>(sess).lastChallenge = challenge;
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
    }
    SendHandshakeResponse(sess, accept);
}

void EACServerEmulator::SendHandshakeResponse(const ClientSession& sess, bool accepted) {
    std::vector<uint8_t> resp(2);
    resp[0] = 0x82;            // handshake response
    resp[1] = accepted ? 1 : 0;
    m_socket.SendTo(sess.ip, sess.port, resp.data(), (uint32_t)resp.size());
    Logger::Info("EACServerEmulator: Handshake %s for %s:%u",
                 accepted ? "accepted" : "rejected",
                 sess.ip.c_str(), sess.port);
}

void EACServerEmulator::ProcessRequests() {
    // In single-threaded mode, call RunLoop body once
    RunLoop();
}

EACServerEmulator::ClientSession* EACServerEmulator::FindOrCreateSession(const std::string& ip, uint16_t port) {
    auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
        [&](const ClientSession& s){ return s.ip == ip && s.port == port; });
    if (it != m_sessions.end()) return &*it;
    m_sessions.push_back({ ip, port, 0, GenerateNonce() });
    return &m_sessions.back();
}

uint32_t EACServerEmulator::GenerateChallenge() {
    static std::mt19937 rng((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
    return rng();
}

uint64_t EACServerEmulator::GenerateNonce() {
    static std::mt19937_64 rng((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    return rng();
}

void EACServerEmulator::Shutdown() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    m_socket.Close();
    Logger::Info("EACServerEmulator: Shutdown complete");
}