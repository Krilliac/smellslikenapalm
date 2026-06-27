#include "Utils/Timer.h"
#include "Utils/Logger.h"
#include "Utils/CrashHandler.h"
#include <thread>

Timer::Timer()
  : m_running(false)
{
    Logger::Trace("[Timer::Timer] Entry");
    Logger::Trace("[Timer::Timer] Exit: initialized with m_running=false");
}

Timer::~Timer() {
    Logger::Trace("[Timer::~Timer] Entry");
    Cancel();
    Logger::Trace("[Timer::~Timer] Exit");
}

void Timer::StartOnce(Duration interval, Callback cb) {
    Logger::Trace("[Timer::StartOnce] Entry: interval=%lld ms",
                  std::chrono::duration_cast<std::chrono::milliseconds>(interval).count());
    Cancel();
    m_running = true;
    Logger::Info("[Timer::StartOnce] Starting one-shot timer with interval %lld ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(interval).count());
    m_thread = std::thread([this, interval, cb]() {
        Logger::Debug("[Timer::StartOnce] Timer thread started, sleeping for interval");
        std::this_thread::sleep_for(interval);
        if (m_running) {
            Logger::Debug("[Timer::StartOnce] Timer fired, invoking callback");
            // Guard the user callback: it runs on this timer thread, so a throw
            // would escape into std::terminate. Report non-fatally instead.
            rs2v::Guard("timer callback (once)", cb);
        } else {
            Logger::Debug("[Timer::StartOnce] Timer was cancelled during sleep, skipping callback");
        }
        m_running = false;
        Logger::Debug("[Timer::StartOnce] Timer thread exiting");
    });
    Logger::Trace("[Timer::StartOnce] Exit: timer thread launched");
}

void Timer::StartRepeating(Duration interval, Callback cb) {
    Logger::Trace("[Timer::StartRepeating] Entry: interval=%lld ms",
                  std::chrono::duration_cast<std::chrono::milliseconds>(interval).count());
    Cancel();
    m_running = true;
    Logger::Info("[Timer::StartRepeating] Starting repeating timer with interval %lld ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(interval).count());
    m_thread = std::thread([this, interval, cb]() {
        Logger::Debug("[Timer::StartRepeating] Repeating timer thread started");
        while (m_running) {
            std::this_thread::sleep_for(interval);
            if (!m_running) {
                Logger::Debug("[Timer::StartRepeating] Timer cancelled during sleep, breaking");
                break;
            }
            Logger::Debug("[Timer::StartRepeating] Timer tick, invoking callback");
            // Guard the user callback: a throw on a repeating tick must not
            // escape into std::terminate or silently kill future ticks.
            rs2v::Guard("timer callback (repeating)", cb);
        }
        Logger::Debug("[Timer::StartRepeating] Repeating timer thread exiting");
    });
    Logger::Trace("[Timer::StartRepeating] Exit: repeating timer thread launched");
}

void Timer::Cancel() {
    Logger::Trace("[Timer::Cancel] Entry");
    if (m_running) {
        Logger::Debug("[Timer::Cancel] Timer is running, setting m_running=false");
    } else {
        Logger::Debug("[Timer::Cancel] Timer is not running");
    }
    m_running = false;
    if (m_thread.joinable()) {
        Logger::Debug("[Timer::Cancel] Joining timer thread");
        m_thread.join();
        Logger::Debug("[Timer::Cancel] Timer thread joined");
    }
    Logger::Trace("[Timer::Cancel] Exit");
}

bool Timer::IsRunning() const {
    Logger::Trace("[Timer::IsRunning] Entry");
    bool running = m_running;
    Logger::Trace("[Timer::IsRunning] Exit: returning %s", running ? "true" : "false");
    return running;
}
