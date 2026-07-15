// src/Time/GameClock.cpp
#include "Time/GameClock.h"
#include "Utils/Logger.h"
#include <thread>

namespace {
double ToMilliseconds(GameClock::Duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}
} // namespace

GameClock::GameClock()
    : m_startTime(Clock::now()),
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
    m_tickInterval = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(ticksPerSecond)));
    Logger::Info("[GameClock::SetTickRate] Tick rate updated to %u ticks/sec, tick interval set to %.3f ms",
                 ticksPerSecond, ToMilliseconds(m_tickInterval));
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
    auto elapsed = Clock::now() - m_startTime;
    Logger::Trace("[GameClock::GetElapsed] Exit — elapsed=%.3f ms", ToMilliseconds(elapsed));
    return elapsed;
}

GameClock::Duration GameClock::GetLastDelta() const {
    Logger::Trace("[GameClock::GetLastDelta] Entry");
    Logger::Trace("[GameClock::GetLastDelta] Exit — lastDelta=%.3f ms", ToMilliseconds(m_lastDelta));
    return m_lastDelta;
}

void GameClock::RunLoop() {
    Logger::Trace("[GameClock::RunLoop] Entry — starting main game clock loop");
    m_running = true;
    m_startTime = Clock::now();
    m_lastTick  = m_startTime;
    Logger::Info("[GameClock::RunLoop] Game clock loop started, m_running=true, start time and last tick reset");

    while (m_running) {
        auto now = Clock::now();
        auto delta = now - m_lastTick;
        if (delta < m_tickInterval) {
            auto sleepTime = m_tickInterval - delta;
            Logger::Trace("[GameClock::RunLoop] Delta %.3f ms < tick interval %.3f ms, sleeping for %.3f ms",
                          ToMilliseconds(delta),
                          ToMilliseconds(m_tickInterval),
                          ToMilliseconds(sleepTime));
            std::this_thread::sleep_for(sleepTime);
            continue;
        }

        m_lastDelta = delta;
        m_lastTick  = now;
        // Per-frame timing is intentionally trace-only. Emitting two DEBUG log
        // records every tick can itself throttle a Windows server doing
        // synchronous console/file logging.
        Logger::Trace("[GameClock::RunLoop] Tick fired — delta=%.3f ms, updating lastDelta and lastTick",
                      ToMilliseconds(delta));

        // Invoke callbacks
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Logger::Trace("[GameClock::RunLoop] Invoking %zu registered tick callbacks with delta=%.3f ms",
                          m_callbacks.size(), ToMilliseconds(m_lastDelta));
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
