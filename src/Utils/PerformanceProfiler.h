// src/Utils/PerformanceProfiler.h
#pragma once

#include <string>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <functional>

class PerformanceProfiler {
public:
    using Clock      = std::chrono::high_resolution_clock;
    using TimePoint  = Clock::time_point;
    using Duration   = std::chrono::duration<double, std::milli>;  // milliseconds

    // A profiling entry for a named section
    struct Entry {
        std::string    name;
        Duration       totalTime{0};
        uint64_t       callCount{0};
        TimePoint      startTime;
        bool           running{false};
    };

    // Start timing a section (nested calls allowed)
    void Begin(const std::string& sectionName);

    // End timing the most recent Begin for this section
    void End(const std::string& sectionName);

    // Reset all collected data
    void Reset();

    // Retrieve current snapshot of all entries
    std::vector<Entry> GetEntries() const;

    // Log a report via callback (e.g., to your Logger)
    void Report(const std::function<void(const Entry&)>& callback) const;

private:
    mutable std::mutex                           m_mutex;
    std::unordered_map<std::string, Entry>       m_entries;
};