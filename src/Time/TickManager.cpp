// src/Time/TickManager.cpp
#include "Time/TickManager.h"
#include "Utils/Logger.h"
#include <thread>

TickManager::TickManager()
    : m_ticksPerSecond(60),
      m_tickInterval(Duration(1000 / 60)),
      m_lastTime(Clock::now()),
      m_accumulatedDelta(0)
{
    Logger::Trace("[TickManager::TickManager] Constructor entered — default ticksPerSecond=60, tickInterval=%lld ms",
                  static_cast<long long>(m_tickInterval.count()));
    Logger::Info("[TickManager::TickManager] TickManager instance created with default tick rate of 60 ticks/sec");
}

TickManager::~TickManager() = default;

void TickManager::SetTickRate(uint32_t ticksPerSecond) {
    Logger::Trace("[TickManager::SetTickRate] Entry — ticksPerSecond=%u", ticksPerSecond);
    if (ticksPerSecond == 0) {
        Logger::Warn("[TickManager::SetTickRate] Tick rate of 0 requested, ignoring to avoid division by zero");
        Logger::Trace("[TickManager::SetTickRate] Exit — returning early due to zero tick rate");
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ticksPerSecond = ticksPerSecond;
    m_tickInterval = Duration(1000 / ticksPerSecond);
    Logger::Info("[TickManager::SetTickRate] Tick rate updated to %u ticks/sec, tick interval set to %lld ms",
                 ticksPerSecond, static_cast<long long>(m_tickInterval.count()));
    Logger::Trace("[TickManager::SetTickRate] Exit — tick rate configured successfully");
}

void TickManager::RegisterCallback(TickCallback cb) {
    Logger::Trace("[TickManager::RegisterCallback] Entry — registering new tick callback");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.push_back(std::move(cb));
    Logger::Debug("[TickManager::RegisterCallback] Callback added, total registered callbacks: %zu", m_callbacks.size());
    Logger::Trace("[TickManager::RegisterCallback] Exit — callback registered successfully");
}

bool TickManager::Update() {
    Logger::Trace("[TickManager::Update] Entry");
    auto now = Clock::now();
    Duration frameDelta = std::chrono::duration_cast<Duration>(now - m_lastTime);
    m_lastTime = now;
    Logger::Debug("[TickManager::Update] Frame delta computed: %lld ms", static_cast<long long>(frameDelta.count()));

    bool ticked = false;
    m_accumulatedDelta += frameDelta;
    Logger::Debug("[TickManager::Update] Accumulated delta after adding frame delta: %lld ms, tick interval: %lld ms",
                  static_cast<long long>(m_accumulatedDelta.count()),
                  static_cast<long long>(m_tickInterval.count()));

    // Process as many fixed ticks as fit into accumulated delta
    int tickCount = 0;
    while (m_accumulatedDelta >= m_tickInterval) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Logger::Debug("[TickManager::Update] Processing fixed tick #%d — invoking %zu callbacks with interval=%lld ms",
                          tickCount + 1, m_callbacks.size(), static_cast<long long>(m_tickInterval.count()));
            for (auto& cb : m_callbacks) {
                cb(m_tickInterval);
            }
        }
        m_accumulatedDelta -= m_tickInterval;
        ticked = true;
        tickCount++;
        Logger::Trace("[TickManager::Update] Tick #%d complete, remaining accumulated delta: %lld ms",
                      tickCount, static_cast<long long>(m_accumulatedDelta.count()));
    }

    if (!ticked) {
        Logger::Trace("[TickManager::Update] No ticks processed this frame — accumulated delta %lld ms below interval %lld ms",
                      static_cast<long long>(m_accumulatedDelta.count()),
                      static_cast<long long>(m_tickInterval.count()));
    } else {
        Logger::Debug("[TickManager::Update] Processed %d tick(s) this frame", tickCount);
    }

    Logger::Trace("[TickManager::Update] Exit — returning ticked=%s", ticked ? "true" : "false");
    return ticked;
}

TickManager::Duration TickManager::GetAccumulatedDelta() const {
    Logger::Trace("[TickManager::GetAccumulatedDelta] Entry");
    Logger::Trace("[TickManager::GetAccumulatedDelta] Exit — accumulatedDelta=%lld ms",
                  static_cast<long long>(m_accumulatedDelta.count()));
    return m_accumulatedDelta;
}

void TickManager::ResetAccumulatedDelta() {
    Logger::Trace("[TickManager::ResetAccumulatedDelta] Entry — current accumulatedDelta=%lld ms",
                  static_cast<long long>(m_accumulatedDelta.count()));
    m_accumulatedDelta = Duration(0);
    Logger::Info("[TickManager::ResetAccumulatedDelta] Accumulated delta reset to 0");
    Logger::Trace("[TickManager::ResetAccumulatedDelta] Exit");
}
