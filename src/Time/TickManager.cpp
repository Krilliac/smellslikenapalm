// src/Time/TickManager.cpp
#include "Time/TickManager.h"
#include <thread>

TickManager::TickManager()
    : m_ticksPerSecond(60),
      m_tickInterval(Duration(1000 / 60)),
      m_lastTime(Clock::now()),
      m_accumulatedDelta(0)
{}

TickManager::~TickManager() = default;

void TickManager::SetTickRate(uint32_t ticksPerSecond) {
    if (ticksPerSecond == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ticksPerSecond = ticksPerSecond;
    m_tickInterval = Duration(1000 / ticksPerSecond);
}

void TickManager::RegisterCallback(TickCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.push_back(std::move(cb));
}

bool TickManager::Update() {
    auto now = Clock::now();
    Duration frameDelta = std::chrono::duration_cast<Duration>(now - m_lastTime);
    m_lastTime = now;

    bool ticked = false;
    m_accumulatedDelta += frameDelta;

    // Process as many fixed ticks as fit into accumulated delta
    while (m_accumulatedDelta >= m_tickInterval) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& cb : m_callbacks) {
                cb(m_tickInterval);
            }
        }
        m_accumulatedDelta -= m_tickInterval;
        ticked = true;
    }

    return ticked;
}

TickManager::Duration TickManager::GetAccumulatedDelta() const {
    return m_accumulatedDelta;
}

void TickManager::ResetAccumulatedDelta() {
    m_accumulatedDelta = Duration(0);
}