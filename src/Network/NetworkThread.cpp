// src/Network/NetworkThread.cpp

#include "Network/NetworkThread.h"
#include "Utils/Logger.h"

NetworkThread::NetworkThread(ConnectionManager* connMgr, TickCallback tickCb)
    : m_connMgr(connMgr)
    , m_tickCallback(std::move(tickCb))
{
    Logger::Trace("[NetworkThread::NetworkThread] Entry: connMgr=%p, tickCb=%s",
                  (void*)connMgr, tickCb ? "non-null" : "null");
    Logger::Info("NetworkThread constructed");
    Logger::Debug("[NetworkThread::NetworkThread] m_connMgr=%p, m_tickCallback=%s",
                  (void*)m_connMgr, m_tickCallback ? "set" : "not set");
    Logger::Trace("[NetworkThread::NetworkThread] Exit");
}

NetworkThread::~NetworkThread() {
    Logger::Trace("[NetworkThread::~NetworkThread] Entry: destructor called");
    Logger::Debug("[NetworkThread::~NetworkThread] m_running=%s, calling Stop()",
                  m_running ? "true" : "false");
    Stop();
    Logger::Trace("[NetworkThread::~NetworkThread] Exit");
}

void NetworkThread::Start() {
    Logger::Trace("[NetworkThread::Start] Entry: m_running=%s, m_ticksPerSecond=%u",
                  m_running ? "true" : "false", m_ticksPerSecond);
    if (m_running) {
        Logger::Debug("[NetworkThread::Start] Already running, returning early");
        Logger::Trace("[NetworkThread::Start] Exit: already running");
        return;
    }
    Logger::Info("Starting NetworkThread at %u ticks/sec", m_ticksPerSecond);
    m_running = true;
    Logger::Debug("[NetworkThread::Start] m_running set to true, spawning thread");
    m_thread = std::thread(&NetworkThread::RunLoop, this);
    Logger::Debug("[NetworkThread::Start] Thread spawned successfully");
    Logger::Trace("[NetworkThread::Start] Exit");
}

void NetworkThread::Stop() {
    Logger::Trace("[NetworkThread::Stop] Entry: m_running=%s", m_running ? "true" : "false");
    if (!m_running) {
        Logger::Debug("[NetworkThread::Stop] Not running, returning early");
        Logger::Trace("[NetworkThread::Stop] Exit: not running");
        return;
    }
    Logger::Info("Stopping NetworkThread");
    m_running = false;
    Logger::Debug("[NetworkThread::Stop] m_running set to false, checking if thread is joinable");
    if (m_thread.joinable()) {
        Logger::Debug("[NetworkThread::Stop] Thread is joinable, joining...");
        m_thread.join();
        Logger::Debug("[NetworkThread::Stop] Thread joined successfully");
    } else {
        Logger::Debug("[NetworkThread::Stop] Thread is not joinable");
    }
    Logger::Trace("[NetworkThread::Stop] Exit");
}

void NetworkThread::SetTickRate(uint32_t ticksPerSecond) {
    Logger::Trace("[NetworkThread::SetTickRate] Entry: ticksPerSecond=%u", ticksPerSecond);
    uint32_t previousTickRate = m_ticksPerSecond;
    m_ticksPerSecond = ticksPerSecond;
    if (ticksPerSecond > 0) {
        m_tickInterval = std::chrono::milliseconds(1000 / ticksPerSecond);
        Logger::Debug("[NetworkThread::SetTickRate] ticksPerSecond > 0, computed m_tickInterval=%lld ms",
                      (long long)m_tickInterval.count());
    } else {
        Logger::Warn("[NetworkThread::SetTickRate] ticksPerSecond is 0, tick interval not updated");
    }
    Logger::Info("NetworkThread tick rate set to %u ticks/sec", m_ticksPerSecond);
    Logger::Debug("[NetworkThread::SetTickRate] Tick rate changed from %u to %u",
                  previousTickRate, m_ticksPerSecond);
    Logger::Trace("[NetworkThread::SetTickRate] Exit");
}

void NetworkThread::RunLoop() {
    Logger::Trace("[NetworkThread::RunLoop] Entry: m_tickInterval=%lld ms",
                  (long long)m_tickInterval.count());
    Logger::Info("[NetworkThread::RunLoop] Network loop starting");
    auto nextTick = std::chrono::steady_clock::now() + m_tickInterval;
    Logger::Debug("[NetworkThread::RunLoop] First nextTick scheduled");
    while (m_running) {
        // 1. Poll and dispatch network I/O
        Logger::Trace("[NetworkThread::RunLoop] Tick iteration: calling PumpNetwork");
        m_connMgr->PumpNetwork();
        Logger::Trace("[NetworkThread::RunLoop] PumpNetwork completed");

        // 2. Invoke game-server tick callback (processing of packets)
        if (m_tickCallback) {
            Logger::Trace("[NetworkThread::RunLoop] Invoking tick callback");
            m_tickCallback();
            Logger::Trace("[NetworkThread::RunLoop] Tick callback completed");
        } else {
            Logger::Debug("[NetworkThread::RunLoop] No tick callback set, skipping");
        }

        // 3. Sleep until next tick
        Logger::Trace("[NetworkThread::RunLoop] Sleeping until next tick");
        std::this_thread::sleep_until(nextTick);
        nextTick += m_tickInterval;
    }
    Logger::Info("NetworkThread loop exited");
    Logger::Debug("[NetworkThread::RunLoop] m_running became false, loop terminated");
    Logger::Trace("[NetworkThread::RunLoop] Exit");
}
