// src/Config/PerformanceConfig.cpp

#include "Config/PerformanceConfig.h"

PerformanceConfig::PerformanceConfig(const ServerConfig& cfg)
  : m_maxCpuCores(cfg.GetMaxCpuCores())
  , m_cpuAffinityMask(cfg.GetCpuAffinityMask())
  , m_dynamicTuning(cfg.IsDynamicTuningEnabled())
  , m_workerThreadCount(cfg.GetWorkerThreadCount())
  , m_maxTaskQueueLength(cfg.GetMaxTaskQueueLength())
  , m_spillThreshold(cfg.GetSpillThreshold())
  , m_preallocateChunks(cfg.GetPreallocateChunks())
  , m_maxChunks(cfg.GetMaxChunks())
  , m_profilingEnabled(cfg.IsProfilingEnabled())
  , m_profilerMinRecordMs(cfg.GetProfilerMinRecordMs())
  , m_profilerBufferSize(cfg.GetProfilerBufferSize())
  , m_profilerFlushInterval(cfg.GetProfilerFlushInterval())
  , m_profilerOutputFormat(cfg.GetProfilerOutputFormat())
  , m_profilerOutputPath(cfg.GetProfilerOutputPath())
  , m_telemetryBatchSize(cfg.GetTelemetryBatchSize())
  , m_telemetryFlushInterval(cfg.GetTelemetryFlushInterval())
{}

int PerformanceConfig::GetMaxCpuCores() const {
    return m_maxCpuCores;
}

int PerformanceConfig::GetCpuAffinityMask() const {
    return m_cpuAffinityMask;
}

bool PerformanceConfig::IsDynamicTuningEnabled() const {
    return m_dynamicTuning;
}

int PerformanceConfig::GetWorkerThreadCount() const {
    return m_workerThreadCount;
}

int PerformanceConfig::GetMaxTaskQueueLength() const {
    return m_maxTaskQueueLength;
}

int PerformanceConfig::GetSpillThreshold() const {
    return m_spillThreshold;
}

int PerformanceConfig::GetPreallocateChunks() const {
    return m_preallocateChunks;
}

int PerformanceConfig::GetMaxChunks() const {
    return m_maxChunks;
}

bool PerformanceConfig::IsProfilingEnabled() const {
    return m_profilingEnabled;
}

int PerformanceConfig::GetProfilerMinRecordMs() const {
    return m_profilerMinRecordMs;
}

int PerformanceConfig::GetProfilerBufferSize() const {
    return m_profilerBufferSize;
}

int PerformanceConfig::GetProfilerFlushInterval() const {
    return m_profilerFlushInterval;
}

const std::string& PerformanceConfig::GetProfilerOutputFormat() const {
    return m_profilerOutputFormat;
}

const std::string& PerformanceConfig::GetProfilerOutputPath() const {
    return m_profilerOutputPath;
}

int PerformanceConfig::GetTelemetryBatchSize() const {
    return m_telemetryBatchSize;
}

int PerformanceConfig::GetTelemetryFlushInterval() const {
    return m_telemetryFlushInterval;
}