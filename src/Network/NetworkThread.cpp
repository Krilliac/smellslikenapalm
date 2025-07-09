// src/Network/NetworkThread.cpp

#include "Network/NetworkThread.h"
#include "Utils/Logger.h"

NetworkThread::NetworkThread(ConnectionManager* connMgr, TickCallback tickCb)
    : m_connMgr(connMgr)
    , m_tickCallback(std::move(tickCb))
{
    Logger::Info("NetworkThread constructed");
}

NetworkThread::~NetworkThread() {
    Stop();
}

void NetworkThread::Start() {
    if (m_running) return;
    Logger::Info("Starting NetworkThread at %u ticks/sec", m_ticksPerSecond);
    m_running = true;
    m_thread = std::thread(&NetworkThread::RunLoop, this);
}

void NetworkThread::Stop() {
    if (!m_running) return;
    Logger::Info("Stopping NetworkThread");
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void NetworkThread::SetTickRate(uint32_t ticksPerSecond) {
    m_ticksPerSecond = ticksPerSecond;
    if (ticksPerSecond > 0) {
        m_tickInterval = std::chrono::milliseconds(1000 / ticksPerSecond);
    }
    Logger::Info("NetworkThread tick rate set to %u ticks/sec", m_ticksPerSecond);
}

void NetworkThread::RunLoop() {
    auto nextTick = std::chrono::steady_clock::now() + m_tickInterval;
    while (m_running) {
        // 1. Poll and dispatch network I/O
        m_connMgr->PumpNetwork();

        // 2. Invoke game-server tick callback (processing of packets)
        if (m_tickCallback) {
            m_tickCallback();
        }

        // 3. Sleep until next tick
        std::this_thread::sleep_until(nextTick);
        nextTick += m_tickInterval;
    }
    Logger::Info("NetworkThread loop exited");
}