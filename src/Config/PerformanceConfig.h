// src/Config/PerformanceConfig.h

#pragma once

#include "Config/ServerConfig.h"

class PerformanceConfig {
public:
    explicit PerformanceConfig(const ServerConfig& cfg);

    int   GetMaxCpuCores() const;
    int   GetCpuAffinityMask() const;
    bool  IsDynamicTuningEnabled() const;

    // ThreadPool
    int   GetWorkerThreadCount() const;
    int   GetMaxTaskQueueLength() const;
    int   GetSpillThreshold() const;

    // MemoryPool
    int   GetPreallocateChunks() const;
    int   GetMaxChunks() const;

    // Profiler
    bool        IsProfilingEnabled() const;
    int         GetProfilerMinRecordMs() const;
    int         GetProfilerBufferSize() const;
    int         GetProfilerFlushInterval() const;
    const std::string& GetProfilerOutputFormat() const;
    const std::string& GetProfilerOutputPath() const;

    // Telemetry
    int   GetTelemetryBatchSize() const;
    int   GetTelemetryFlushInterval() const;

private:
    int   m_maxCpuCores;
    int   m_cpuAffinityMask;
    bool  m_dynamicTuning;

    int   m_workerThreadCount;
    int   m_maxTaskQueueLength;
    int   m_spillThreshold;

    int   m_preallocateChunks;
    int   m_maxChunks;

    bool        m_profilingEnabled;
    int         m_profilerMinRecordMs;
    int         m_profilerBufferSize;
    int         m_profilerFlushInterval;
    std::string m_profilerOutputFormat;
    std::string m_profilerOutputPath;

    int   m_telemetryBatchSize;
    int   m_telemetryFlushInterval;
};