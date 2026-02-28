// src/Utils/PerformanceProfiler.cpp
#include "Utils/PerformanceProfiler.h"
#include "Utils/Logger.h"
#include <stdexcept>

void PerformanceProfiler::Begin(const std::string& sectionName) {
    Logger::Trace("[PerformanceProfiler::Begin] Entry: sectionName='%s'", sectionName.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[PerformanceProfiler::Begin] Mutex acquired, looking up section '%s' in entries map (size=%zu)", sectionName.c_str(), m_entries.size());
    auto& e = m_entries[sectionName];
    if (!e.running) {
        e.name       = sectionName;
        e.startTime  = Clock::now();
        e.running    = true;
        Logger::Debug("[PerformanceProfiler::Begin] Section '%s' started, startTime recorded, current callCount=%zu", sectionName.c_str(), e.callCount);
        Logger::Trace("[PerformanceProfiler::Begin] Exit: section '%s' now running", sectionName.c_str());
    } else {
        // Nested Begin without End: ignore or throw
        Logger::Error("[PerformanceProfiler::Begin] Section '%s' is already running (callCount=%zu), throwing runtime_error", sectionName.c_str(), e.callCount);
        throw std::runtime_error("PerformanceProfiler::Begin called while already running for section: " + sectionName);
    }
}

void PerformanceProfiler::End(const std::string& sectionName) {
    Logger::Trace("[PerformanceProfiler::End] Entry: sectionName='%s'", sectionName.c_str());
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[PerformanceProfiler::End] Mutex acquired, looking up section '%s'", sectionName.c_str());
    auto it = m_entries.find(sectionName);
    if (it == m_entries.end() || !it->second.running) {
        Logger::Error("[PerformanceProfiler::End] Section '%s' not found or not running (found=%s, running=%s), throwing runtime_error",
                      sectionName.c_str(),
                      it != m_entries.end() ? "true" : "false",
                      (it != m_entries.end() && it->second.running) ? "true" : "false");
        throw std::runtime_error("PerformanceProfiler::End called without matching Begin for section: " + sectionName);
    }
    Entry& e = it->second;
    Duration delta = now - e.startTime;
    e.totalTime += delta;
    e.callCount += 1;
    e.running = false;
    long long deltaMicros = std::chrono::duration_cast<std::chrono::microseconds>(delta).count();
    long long totalMicros = std::chrono::duration_cast<std::chrono::microseconds>(e.totalTime).count();
    Logger::Debug("[PerformanceProfiler::End] Section '%s' ended: delta=%lld us, totalTime=%lld us, callCount=%zu",
                  sectionName.c_str(), deltaMicros, totalMicros, e.callCount);
    Logger::Trace("[PerformanceProfiler::End] Exit: section '%s' stopped successfully", sectionName.c_str());
}

void PerformanceProfiler::Reset() {
    Logger::Trace("[PerformanceProfiler::Reset] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t entryCount = m_entries.size();
    Logger::Debug("[PerformanceProfiler::Reset] Clearing %zu profiling entries", entryCount);
    m_entries.clear();
    Logger::Info("[PerformanceProfiler::Reset] All profiling entries cleared (was %zu entries)", entryCount);
    Logger::Trace("[PerformanceProfiler::Reset] Exit: entries map now empty");
}

std::vector<PerformanceProfiler::Entry> PerformanceProfiler::GetEntries() const {
    Logger::Trace("[PerformanceProfiler::GetEntries] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Entry> list;
    list.reserve(m_entries.size());
    Logger::Debug("[PerformanceProfiler::GetEntries] Collecting %zu entries from map", m_entries.size());
    for (const auto& kv : m_entries) {
        list.push_back(kv.second);
        Logger::Trace("[PerformanceProfiler::GetEntries] Collected entry: name='%s', callCount=%zu, running=%s",
                      kv.second.name.c_str(), kv.second.callCount, kv.second.running ? "true" : "false");
    }
    Logger::Debug("[PerformanceProfiler::GetEntries] Returning %zu profiling entries", list.size());
    Logger::Trace("[PerformanceProfiler::GetEntries] Exit: returning %zu entries", list.size());
    return list;
}

void PerformanceProfiler::Report(const std::function<void(const Entry&)>& callback) const {
    Logger::Trace("[PerformanceProfiler::Report] Entry: callback=%s", callback ? "valid" : "null");
    if (!callback) {
        Logger::Warn("[PerformanceProfiler::Report] Callback is null, nothing to report");
        Logger::Trace("[PerformanceProfiler::Report] Exit: no-op (null callback)");
        return;
    }
    auto entries = GetEntries();
    Logger::Debug("[PerformanceProfiler::Report] Reporting %zu profiling entries via callback", entries.size());
    for (const auto& e : entries) {
        Logger::Trace("[PerformanceProfiler::Report] Invoking callback for entry '%s' (callCount=%zu)", e.name.c_str(), e.callCount);
        callback(e);
    }
    Logger::Info("[PerformanceProfiler::Report] Performance report complete, %zu entries reported", entries.size());
    Logger::Trace("[PerformanceProfiler::Report] Exit: all entries reported");
}
