// src/Config/PerformanceConfig.cpp

#include "Config/PerformanceConfig.h"
#include "Utils/Logger.h"

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
{
    Logger::Trace("[PerformanceConfig::PerformanceConfig] Entry - constructing from ServerConfig");
    Logger::Info("[PerformanceConfig::PerformanceConfig] PerformanceConfig initialized: cores=%d, affinity=%d, dynTune=%s, workers=%d, queueLen=%d, spill=%d, prealloc=%d, maxChunks=%d, profiling=%s",
                 m_maxCpuCores, m_cpuAffinityMask, m_dynamicTuning ? "true" : "false",
                 m_workerThreadCount, m_maxTaskQueueLength, m_spillThreshold,
                 m_preallocateChunks, m_maxChunks, m_profilingEnabled ? "true" : "false");
    Logger::Trace("[PerformanceConfig::PerformanceConfig] Exit");
}

int PerformanceConfig::GetMaxCpuCores() const {
    Logger::Trace("[PerformanceConfig::GetMaxCpuCores] Entry");
    Logger::Trace("[PerformanceConfig::GetMaxCpuCores] Exit - returning %d", m_maxCpuCores);
    return m_maxCpuCores;
}

int PerformanceConfig::GetCpuAffinityMask() const {
    Logger::Trace("[PerformanceConfig::GetCpuAffinityMask] Entry");
    Logger::Trace("[PerformanceConfig::GetCpuAffinityMask] Exit - returning %d", m_cpuAffinityMask);
    return m_cpuAffinityMask;
}

bool PerformanceConfig::IsDynamicTuningEnabled() const {
    Logger::Trace("[PerformanceConfig::IsDynamicTuningEnabled] Entry");
    Logger::Trace("[PerformanceConfig::IsDynamicTuningEnabled] Exit - returning %s", m_dynamicTuning ? "true" : "false");
    return m_dynamicTuning;
}

int PerformanceConfig::GetWorkerThreadCount() const {
    Logger::Trace("[PerformanceConfig::GetWorkerThreadCount] Entry");
    Logger::Trace("[PerformanceConfig::GetWorkerThreadCount] Exit - returning %d", m_workerThreadCount);
    return m_workerThreadCount;
}

int PerformanceConfig::GetMaxTaskQueueLength() const {
    Logger::Trace("[PerformanceConfig::GetMaxTaskQueueLength] Entry");
    Logger::Trace("[PerformanceConfig::GetMaxTaskQueueLength] Exit - returning %d", m_maxTaskQueueLength);
    return m_maxTaskQueueLength;
}

int PerformanceConfig::GetSpillThreshold() const {
    Logger::Trace("[PerformanceConfig::GetSpillThreshold] Entry");
    Logger::Trace("[PerformanceConfig::GetSpillThreshold] Exit - returning %d", m_spillThreshold);
    return m_spillThreshold;
}

int PerformanceConfig::GetPreallocateChunks() const {
    Logger::Trace("[PerformanceConfig::GetPreallocateChunks] Entry");
    Logger::Trace("[PerformanceConfig::GetPreallocateChunks] Exit - returning %d", m_preallocateChunks);
    return m_preallocateChunks;
}

int PerformanceConfig::GetMaxChunks() const {
    Logger::Trace("[PerformanceConfig::GetMaxChunks] Entry");
    Logger::Trace("[PerformanceConfig::GetMaxChunks] Exit - returning %d", m_maxChunks);
    return m_maxChunks;
}

bool PerformanceConfig::IsProfilingEnabled() const {
    Logger::Trace("[PerformanceConfig::IsProfilingEnabled] Entry");
    Logger::Trace("[PerformanceConfig::IsProfilingEnabled] Exit - returning %s", m_profilingEnabled ? "true" : "false");
    return m_profilingEnabled;
}

int PerformanceConfig::GetProfilerMinRecordMs() const {
    Logger::Trace("[PerformanceConfig::GetProfilerMinRecordMs] Entry");
    Logger::Trace("[PerformanceConfig::GetProfilerMinRecordMs] Exit - returning %d", m_profilerMinRecordMs);
    return m_profilerMinRecordMs;
}

int PerformanceConfig::GetProfilerBufferSize() const {
    Logger::Trace("[PerformanceConfig::GetProfilerBufferSize] Entry");
    Logger::Trace("[PerformanceConfig::GetProfilerBufferSize] Exit - returning %d", m_profilerBufferSize);
    return m_profilerBufferSize;
}

int PerformanceConfig::GetProfilerFlushInterval() const {
    Logger::Trace("[PerformanceConfig::GetProfilerFlushInterval] Entry");
    Logger::Trace("[PerformanceConfig::GetProfilerFlushInterval] Exit - returning %d", m_profilerFlushInterval);
    return m_profilerFlushInterval;
}

const std::string& PerformanceConfig::GetProfilerOutputFormat() const {
    Logger::Trace("[PerformanceConfig::GetProfilerOutputFormat] Entry");
    Logger::Trace("[PerformanceConfig::GetProfilerOutputFormat] Exit - returning '%s'", m_profilerOutputFormat.c_str());
    return m_profilerOutputFormat;
}

const std::string& PerformanceConfig::GetProfilerOutputPath() const {
    Logger::Trace("[PerformanceConfig::GetProfilerOutputPath] Entry");
    Logger::Trace("[PerformanceConfig::GetProfilerOutputPath] Exit - returning '%s'", m_profilerOutputPath.c_str());
    return m_profilerOutputPath;
}

int PerformanceConfig::GetTelemetryBatchSize() const {
    Logger::Trace("[PerformanceConfig::GetTelemetryBatchSize] Entry");
    Logger::Trace("[PerformanceConfig::GetTelemetryBatchSize] Exit - returning %d", m_telemetryBatchSize);
    return m_telemetryBatchSize;
}

int PerformanceConfig::GetTelemetryFlushInterval() const {
    Logger::Trace("[PerformanceConfig::GetTelemetryFlushInterval] Entry");
    Logger::Trace("[PerformanceConfig::GetTelemetryFlushInterval] Exit - returning %d", m_telemetryFlushInterval);
    return m_telemetryFlushInterval;
}
