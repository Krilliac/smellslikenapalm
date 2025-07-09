#pragma once

#include <chrono>
#include <functional>
#include <atomic>

class Timer {
public:
    using Clock      = std::chrono::steady_clock;
    using Duration   = std::chrono::milliseconds;
    using Callback   = std::function<void()>;

    Timer();
    ~Timer();

    // Start a one-shot timer: invokes cb after interval
    void StartOnce(Duration interval, Callback cb);

    // Start a repeating timer: invokes cb every interval until stopped
    void StartRepeating(Duration interval, Callback cb);

    // Cancel the timer (stops one-shot or repeating)
    void Cancel();

    // Check whether the timer is currently running
    bool IsRunning() const;

private:
    std::atomic<bool>        m_running;
    std::thread              m_thread;

    // Internal worker loop for repeating timers
    void Worker(Duration interval, Callback cb, bool repeat);
};