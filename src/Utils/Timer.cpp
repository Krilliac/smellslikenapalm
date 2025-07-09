#include "Utils/Timer.h"
#include <thread>

Timer::Timer()
  : m_running(false)
{}

Timer::~Timer() {
    Cancel();
}

void Timer::StartOnce(Duration interval, Callback cb) {
    Cancel();
    m_running = true;
    m_thread = std::thread([this, interval, cb]() {
        std::this_thread::sleep_for(interval);
        if (m_running) cb();
        m_running = false;
    });
}

void Timer::StartRepeating(Duration interval, Callback cb) {
    Cancel();
    m_running = true;
    m_thread = std::thread([this, interval, cb]() {
        while (m_running) {
            std::this_thread::sleep_for(interval);
            if (!m_running) break;
            cb();
        }
    });
}

void Timer::Cancel() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool Timer::IsRunning() const {
    return m_running;
}