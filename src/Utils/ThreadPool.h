// src/Utils/ThreadPool.h
#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>

class ThreadPool {
public:
    // Construct a thread pool with the given number of worker threads.
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();

    // Enqueue a task returning a future for its result.
    template<typename F, typename... Args>
    auto Enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>;

    // Stop the pool and join all threads.
    void Shutdown();

private:
    // Worker loop
    void Worker();

    std::vector<std::thread>               workers_;
    std::queue<std::function<void()>>      tasks_;
    std::mutex                             mutex_;
    std::condition_variable                condVar_;
    bool                                   stopping_{false};
};

// Template implementation must be in the header
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