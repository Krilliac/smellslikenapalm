// src/Time/TickManager.h
#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <vector>
#include <mutex>

class TickManager {
public:
    using Clock         = std::chrono::steady_clock;
    using Duration      = std::chrono::milliseconds;
    using TickCallback  = std::function<void(Duration delta)>;

    TickManager();
    ~TickManager();

    // Set target tick rate (ticks per second)
    void SetTickRate(uint32_t ticksPerSecond);

    // Register a callback invoked each tick with delta time
    void RegisterCallback(TickCallback cb);

    // Advance the TickManager: should be called each frame or in a loop
    // Returns true if one or more ticks were processed
    bool Update();

    // Retrieve the elapsed time since last Update (sum of processed ticks)
    Duration GetAccumulatedDelta() const;

    // Reset accumulated delta (e.g., after handing off to game loop)
    void ResetAccumulatedDelta();

private:
    uint32_t            m_ticksPerSecond;
    Duration            m_tickInterval;
    Clock::time_point   m_lastTime;

    // Accumulates delta for multiple ticks
    Duration            m_accumulatedDelta;

    std::vector<TickCallback>    m_callbacks;
    mutable std::mutex           m_mutex;
};