// src/Utils/ThreadPool.cpp
#include "Utils/ThreadPool.h"
#include "Utils/Logger.h"

ThreadPool::ThreadPool(size_t numThreads) {
    Logger::Trace("[ThreadPool::ThreadPool] Entry: numThreads=%zu", numThreads);
    // Hardening: 0 workers means enqueued tasks never run and their futures block forever.
    // An absurdly large count exhausts OS thread/handle resources. Clamp to a sane range.
    static constexpr size_t kMaxWorkers = 1024;
    if (numThreads == 0) {
        Logger::Warn("[ThreadPool::ThreadPool] numThreads=0 would deadlock all tasks; clamping to 1");
        numThreads = 1;
    } else if (numThreads > kMaxWorkers) {
        Logger::Warn("[ThreadPool::ThreadPool] numThreads=%zu exceeds cap; clamping to %zu", numThreads, kMaxWorkers);
        numThreads = kMaxWorkers;
    }
    Logger::Info("[ThreadPool::ThreadPool] Creating thread pool with %zu worker threads", numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back(&ThreadPool::Worker, this);
        Logger::Debug("[ThreadPool::ThreadPool] Spawned worker thread %zu", i);
    }
    Logger::Trace("[ThreadPool::ThreadPool] Exit: all %zu workers spawned", numThreads);
}

ThreadPool::~ThreadPool() {
    Logger::Trace("[ThreadPool::~ThreadPool] Entry");
    Shutdown();
    Logger::Trace("[ThreadPool::~ThreadPool] Exit");
}

void ThreadPool::Shutdown() {
    Logger::Trace("[ThreadPool::Shutdown] Entry");
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            Logger::Debug("[ThreadPool::Shutdown] Already stopping, skipping");
            Logger::Trace("[ThreadPool::Shutdown] Exit: already stopped");
            return;
        }
        stopping_ = true;
        Logger::Debug("[ThreadPool::Shutdown] Set stopping flag to true");
    }
    condVar_.notify_all();
    Logger::Debug("[ThreadPool::Shutdown] Notified all worker threads to stop");
    size_t joinedCount = 0;
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
            joinedCount++;
        }
    }
    Logger::Info("[ThreadPool::Shutdown] Joined %zu worker threads", joinedCount);
    Logger::Trace("[ThreadPool::Shutdown] Exit");
}

void ThreadPool::Worker() {
    Logger::Trace("[ThreadPool::Worker] Entry: worker thread started");
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condVar_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty()) {
                Logger::Debug("[ThreadPool::Worker] Stopping flag set and queue empty, exiting worker");
                Logger::Trace("[ThreadPool::Worker] Exit: worker thread terminating (stop requested)");
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            Logger::Debug("[ThreadPool::Worker] Dequeued task, remaining tasks in queue: %zu", tasks_.size());
        }
        Logger::Debug("[ThreadPool::Worker] Executing task");
        task();
        Logger::Debug("[ThreadPool::Worker] Task completed");
    }
}

// Note: Enqueue is a template method defined in the header.
// Moving implementation to the header to avoid template instantiation issues.
