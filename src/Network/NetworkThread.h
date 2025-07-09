// src/Network/NetworkThread.h

#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include "Network/ConnectionManager.h"

class NetworkThread {
public:
    using TickCallback = std::function<void()>;

    NetworkThread(ConnectionManager* connMgr, TickCallback tickCb);
    ~NetworkThread();

    // Start the network thread loop
    void Start();

    // Signal the thread to stop and wait for join
    void Stop();

    // Set desired tick rate (times per second)
    void SetTickRate(uint32_t ticksPerSecond);

private:
    void RunLoop();

    ConnectionManager*  m_connMgr;
    TickCallback        m_tickCallback;
    std::thread         m_thread;
    std::atomic<bool>   m_running{false};
    uint32_t            m_ticksPerSecond{60};
    std::chrono::milliseconds m_tickInterval{16};  // ~60 Hz
};