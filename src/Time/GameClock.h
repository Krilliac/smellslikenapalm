// src/Time/GameClock.h
#pragma once

#include <chrono>
#include <functional>
#include <vector>
#include <mutex>

class GameClock {
public:
    using Duration      = std::chrono::milliseconds;
    using TimePoint     = std::chrono::steady_clock::time_point;
    using TickCallback  = std::function<void(Duration delta)>;

    GameClock();
    ~GameClock();

    // Start the clock loop on this thread (blocks)
    void RunLoop();

    // Signal to stop the loop
    void Stop();

    // Set target tick rate (ticks per second)
    void SetTickRate(uint32_t ticksPerSecond);

    // Register a callback invoked each tick with delta time
    void RegisterTickCallback(TickCallback cb);

    // Retrieve current time since start
    Duration GetElapsed() const;

    // Retrieve delta of last tick
    Duration GetLastDelta() const;

private:
    std::atomic<bool>           m_running{false};
    uint32_t                    m_ticksPerSecond{60};
    Duration                    m_tickInterval{Duration(1000 / 60)};
    TimePoint                   m_startTime;
    TimePoint                   m_lastTick;
    Duration                    m_lastDelta{0};
    std::vector<TickCallback>   m_callbacks;
    std::mutex                  m_mutex;
};