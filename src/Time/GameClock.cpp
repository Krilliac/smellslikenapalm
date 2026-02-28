// src/Time/GameClock.cpp
#include "Time/GameClock.h"
#include "Utils/Logger.h"
#include <thread>

GameClock::GameClock()
    : m_startTime(std::chrono::steady_clock::now()),
      m_lastTick(m_startTime)
{
    Logger::Trace("[GameClock::GameClock] Constructor entered, initializing start time and last tick to current steady_clock time");
    Logger::Info("[GameClock::GameClock] GameClock instance created successfully");
}

GameClock::~GameClock() {
    Logger::Trace("[GameClock::~GameClock] Destructor entered, initiating shutdown");
    Stop();
    Logger::Info("[GameClock::~GameClock] GameClock instance destroyed");
}

void GameClock::SetTickRate(uint32_t ticksPerSecond) {
    Logger::Trace("[GameClock::SetTickRate] Entry — ticksPerSecond=%u", ticksPerSecond);
    if (ticksPerSecond == 0) {
        Logger::Warn("[GameClock::SetTickRate] Tick rate of 0 requested, ignoring to avoid division by zero");
        Logger::Trace("[GameClock::SetTickRate] Exit — returning early due to zero tick rate");
        return;
    }
    m_ticksPerSecond = ticksPerSecond;
    m_tickInterval = Duration(1000 / ticksPerSecond);
    Logger::Info("[GameClock::SetTickRate] Tick rate updated to %u ticks/sec, tick interval set to %lld ms",
                 ticksPerSecond, static_cast<long long>(m_tickInterval.count()));
    Logger::Trace("[GameClock::SetTickRate] Exit — tick rate configured successfully");
}

void GameClock::RegisterTickCallback(TickCallback cb) {
    Logger::Trace("[GameClock::RegisterTickCallback] Entry — registering new tick callback");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.push_back(std::move(cb));
    Logger::Debug("[GameClock::RegisterTickCallback] Callback added, total registered callbacks: %zu", m_callbacks.size());
    Logger::Trace("[GameClock::RegisterTickCallback] Exit — callback registered successfully");
}

GameClock::Duration GameClock::GetElapsed() const {
    Logger::Trace("[GameClock::GetElapsed] Entry");
    auto elapsed = std::chrono::duration_cast<Duration>(
        std::chrono::steady_clock::now() - m_startTime);
    Logger::Trace("[GameClock::GetElapsed] Exit — elapsed=%lld ms", static_cast<long long>(elapsed.count()));
    return elapsed;
}

GameClock::Duration GameClock::GetLastDelta() const {
    Logger::Trace("[GameClock::GetLastDelta] Entry");
    Logger::Trace("[GameClock::GetLastDelta] Exit — lastDelta=%lld ms", static_cast<long long>(m_lastDelta.count()));
    return m_lastDelta;
}

void GameClock::RunLoop() {
    Logger::Trace("[GameClock::RunLoop] Entry — starting main game clock loop");
    m_running = true;
    m_startTime = std::chrono::steady_clock::now();
    m_lastTick  = m_startTime;
    Logger::Info("[GameClock::RunLoop] Game clock loop started, m_running=true, start time and last tick reset");

    while (m_running) {
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<Duration>(now - m_lastTick);
        if (delta < m_tickInterval) {
            auto sleepTime = m_tickInterval - delta;
            Logger::Trace("[GameClock::RunLoop] Delta %lld ms < tick interval %lld ms, sleeping for %lld ms",
                          static_cast<long long>(delta.count()),
                          static_cast<long long>(m_tickInterval.count()),
                          static_cast<long long>(sleepTime.count()));
            std::this_thread::sleep_for(sleepTime);
            continue;
        }

        m_lastDelta = delta;
        m_lastTick  = now;
        Logger::Debug("[GameClock::RunLoop] Tick fired — delta=%lld ms, updating lastDelta and lastTick",
                      static_cast<long long>(delta.count()));

        // Invoke callbacks
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Logger::Debug("[GameClock::RunLoop] Invoking %zu registered tick callbacks with delta=%lld ms",
                          m_callbacks.size(), static_cast<long long>(m_lastDelta.count()));
            for (auto& cb : m_callbacks) {
                cb(m_lastDelta);
            }
            Logger::Trace("[GameClock::RunLoop] All callbacks invoked for this tick");
        }
    }

    Logger::Info("[GameClock::RunLoop] Game clock loop exited, m_running is now false");
    Logger::Trace("[GameClock::RunLoop] Exit");
}

void GameClock::Stop() {
    Logger::Trace("[GameClock::Stop] Entry — current m_running=%s", m_running ? "true" : "false");
    m_running = false;
    Logger::Info("[GameClock::Stop] Game clock stopped, m_running set to false");
    Logger::Trace("[GameClock::Stop] Exit");
}
