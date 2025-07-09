// src/Time/GameClock.cpp
#include "Time/GameClock.h"
#include <thread>

GameClock::GameClock()
    : m_startTime(std::chrono::steady_clock::now()),
      m_lastTick(m_startTime)
{}

GameClock::~GameClock() {
    Stop();
}

void GameClock::SetTickRate(uint32_t ticksPerSecond) {
    if (ticksPerSecond == 0) return;
    m_ticksPerSecond = ticksPerSecond;
    m_tickInterval = Duration(1000 / ticksPerSecond);
}

void GameClock::RegisterTickCallback(TickCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.push_back(std::move(cb));
}

GameClock::Duration GameClock::GetElapsed() const {
    return std::chrono::duration_cast<Duration>(
        std::chrono::steady_clock::now() - m_startTime);
}

GameClock::Duration GameClock::GetLastDelta() const {
    return m_lastDelta;
}

void GameClock::RunLoop() {
    m_running = true;
    m_startTime = std::chrono::steady_clock::now();
    m_lastTick  = m_startTime;

    while (m_running) {
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<Duration>(now - m_lastTick);
        if (delta < m_tickInterval) {
            std::this_thread::sleep_for(m_tickInterval - delta);
            continue;
        }

        m_lastDelta = delta;
        m_lastTick  = now;

        // Invoke callbacks
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& cb : m_callbacks) {
                cb(m_lastDelta);
            }
        }
    }
}

void GameClock::Stop() {
    m_running = false;
}