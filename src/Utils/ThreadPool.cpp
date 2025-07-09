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

template<typename F, typename... Args>
auto ThreadPool::Enqueue(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result_t<F, Args...>>
{
    using ReturnType = typename std::invoke_result_t<F, Args...>;
    auto packagedTask = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    std::future<ReturnType> result = packagedTask->get_future();
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("ThreadPool is stopped; cannot enqueue new tasks");
        }
        tasks_.emplace([packagedTask]() { (*packagedTask)(); });
    }
    condVar_.notify_one();
    return result;
}

// Explicit template instantiation (if needed) to avoid linker errors:
template std::future<void> ThreadPool::Enqueue<void>(void(*)());