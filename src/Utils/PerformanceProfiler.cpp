// src/Utils/PerformanceProfiler.cpp
#include "Utils/PerformanceProfiler.h"
#include <stdexcept>

void PerformanceProfiler::Begin(const std::string& sectionName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& e = m_entries[sectionName];
    if (!e.running) {
        e.name       = sectionName;
        e.startTime  = Clock::now();
        e.running    = true;
    } else {
        // Nested Begin without End: ignore or throw
        throw std::runtime_error("PerformanceProfiler::Begin called while already running for section: " + sectionName);
    }
}

void PerformanceProfiler::End(const std::string& sectionName) {
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(sectionName);
    if (it == m_entries.end() || !it->second.running) {
        throw std::runtime_error("PerformanceProfiler::End called without matching Begin for section: " + sectionName);
    }
    Entry& e = it->second;
    Duration delta = now - e.startTime;
    e.totalTime += delta;
    e.callCount += 1;
    e.running = false;
}

void PerformanceProfiler::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

std::vector<PerformanceProfiler::Entry> PerformanceProfiler::GetEntries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Entry> list;
    list.reserve(m_entries.size());
    for (const auto& kv : m_entries) {
        list.push_back(kv.second);
    }
    return list;
}

void PerformanceProfiler::Report(const std::function<void(const Entry&)>& callback) const {
    auto entries = GetEntries();
    for (const auto& e : entries) {
        callback(e);
    }
}