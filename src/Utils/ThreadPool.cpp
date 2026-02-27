// src/Utils/ThreadPool.cpp
#include "Utils/ThreadPool.h"

ThreadPool::ThreadPool(size_t numThreads) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back(&ThreadPool::Worker, this);
    }
}

ThreadPool::~ThreadPool() {
    Shutdown();
}

void ThreadPool::Shutdown() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condVar_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::Worker() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condVar_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

// Note: Enqueue is a template method defined in the header.
// Moving implementation to the header to avoid template instantiation issues.