// Server/telemetry/TelemetryManager.cpp
// Implementation of comprehensive telemetry system for the RS2V server

#include "TelemetryManager.h"
#include "MetricsReporter.h"
#include "NetworkMetricsPlatform.h"
#include "Utils/Logger.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

// Platform-specific includes for system metrics
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <iphlpapi.h>
    #include <psapi.h>
    #include <pdh.h>
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "pdh.lib")
    #pragma comment(lib, "psapi.lib")
#elif defined(__linux__)
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/sysinfo.h>
    #include <ifaddrs.h>
    #include <net/if.h>
#elif defined(__APPLE__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
    #include <mach/mach.h>
    #include <mach/processor_info.h>
    #include <mach/mach_host.h>
#endif

namespace Telemetry {

namespace {

template <typename Function>
class ScopeExit {
public:
    explicit ScopeExit(Function function) : m_function(std::move(function)) {}
    ~ScopeExit() { RunNow(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    void RunNow() {
        if (!m_active) return;
        m_active = false;
        m_function();
    }

private:
    Function m_function;
    bool m_active = true;
};

template <typename Function>
ScopeExit<Function> MakeScopeExit(Function function) {
    return ScopeExit<Function>(std::move(function));
}

enum class ReporterCallbackPhase {
    None,
    Initialize,
    Report,
    Shutdown,
};

thread_local TelemetryManager* g_callbackManager = nullptr;
thread_local ReporterCallbackPhase g_callbackPhase = ReporterCallbackPhase::None;

class ReporterCallbackScope {
public:
    ReporterCallbackScope(TelemetryManager* manager, ReporterCallbackPhase phase)
        : m_previousManager(g_callbackManager),
          m_previousPhase(g_callbackPhase) {
        g_callbackManager = manager;
        g_callbackPhase = phase;
    }

    ~ReporterCallbackScope() {
        g_callbackManager = m_previousManager;
        g_callbackPhase = m_previousPhase;
    }

private:
    TelemetryManager* m_previousManager;
    ReporterCallbackPhase m_previousPhase;
};

bool IsInsideReporterCallback(const TelemetryManager* manager) {
    return g_callbackManager == manager &&
           g_callbackPhase != ReporterCallbackPhase::None;
}

bool IsRestrictedLifecycleCallback(const TelemetryManager* manager) {
    return g_callbackManager == manager &&
           (g_callbackPhase == ReporterCallbackPhase::Initialize ||
            g_callbackPhase == ReporterCallbackPhase::Shutdown);
}

bool IsShutdownReporterCallback(const TelemetryManager* manager) {
    return g_callbackManager == manager &&
           g_callbackPhase == ReporterCallbackPhase::Shutdown;
}

TelemetryConfig SanitizeConfig(TelemetryConfig config) {
    if (config.samplingInterval <= std::chrono::milliseconds::zero()) {
        Logger::Warn("Telemetry sampling interval must be positive; clamping to 1ms");
        config.samplingInterval = std::chrono::milliseconds(1);
    }
    if (config.maxSamplesInMemory == 0) {
        Logger::Warn("Telemetry snapshot capacity must be positive; clamping to 1");
        config.maxSamplesInMemory = 1;
    }
    return config;
}

} // namespace

struct TelemetryManager::ReporterRecord {
    explicit ReporterRecord(std::shared_ptr<MetricsReporter> value)
        : reporter(std::move(value)) {}

    std::mutex mutex;
    std::shared_ptr<MetricsReporter> reporter;
    bool initializing = false;
    bool initialized = false;
    bool shutdownRequested = false;
    bool shutdownStarted = false;
    size_t activeReports = 0;
};

#ifdef _WIN32
struct TelemetryManager::WindowsCpuCounterState {
    ~WindowsCpuCounterState() {
        if (query != nullptr) PdhCloseQuery(query);
    }

    double Sample() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!initialized && !Initialize()) return 0.0;
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return 0.0;
        PDH_FMT_COUNTERVALUE value{};
        return PdhGetFormattedCounterValue(
                   counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS
                   ? value.doubleValue
                   : 0.0;
    }

    bool Initialize() {
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return false;
        }
        if (PdhAddEnglishCounter(
                query, "\\Processor(_Total)\\% Processor Time", 0,
                &counter) != ERROR_SUCCESS ||
            PdhCollectQueryData(query) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            counter = nullptr;
            return false;
        }
        initialized = true;
        return true;
    }

    std::mutex mutex;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool initialized = false;
};
#endif

// Static instance
TelemetryManager& TelemetryManager::Instance() {
    Logger::Trace("[TelemetryManager::Instance] Retrieving singleton instance");
    static TelemetryManager instance;
    Logger::Trace("[TelemetryManager::Instance] Returning singleton instance at address %p", (void*)&instance);
    return instance;
}

TelemetryManager::~TelemetryManager() {
    Logger::Trace("[TelemetryManager::~TelemetryManager] Destructor invoked, initiating shutdown sequence");
    Shutdown();
    Logger::Trace("[TelemetryManager::~TelemetryManager] Destructor completed");
}

bool TelemetryManager::Initialize(const TelemetryConfig& config) {
    Logger::Trace("[TelemetryManager::Initialize] Entry - config.enabled=%d, config.enableFileReporter=%d, config.enableSystemMetrics=%d, config.enableApplicationMetrics=%d, config.maxSamplesInMemory=%zu, config.samplingInterval=%lldms, config.metricsDirectory=%s",
                 config.enabled, config.enableFileReporter, config.enableSystemMetrics, config.enableApplicationMetrics,
                 config.maxSamplesInMemory, config.samplingInterval.count(), config.metricsDirectory.c_str());

    if (IsInsideReporterCallback(this)) {
        ReportError("Cannot initialize telemetry from a reporter lifecycle callback");
        return false;
    }

    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        if (m_shutdownPending.load() || m_shuttingDown.load() || m_initializing ||
            m_removingReporters || m_updatingConfig) {
            Logger::Warn("TelemetryManager cannot initialize during another lifecycle transition");
            return false;
        }
        if (m_initialized.load()) {
            Logger::Warn("TelemetryManager already initialized");
            return true;
        }
        m_initializing = true;
    }
    // A completed prior lifecycle must never strand the next generation behind
    // its sample-drain gate.  Reset it only after this Initialize transition has
    // been admitted (so no shutdown is active/pending) and before publishing the
    // new initialized state.  A shutdown that arrives during initialization will
    // subsequently latch the gate again and is still processed by the guard.
    UnblockSamples();
    auto initializingGuard = MakeScopeExit([this]() {
        {
            std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
            m_initializing = false;
        }
        ProcessPendingShutdown();
    });

    const TelemetryConfig sanitized = SanitizeConfig(config);
    Logger::Info("Initializing TelemetryManager...");

    // Perform fallible filesystem work before publishing the configuration.
    if (sanitized.enableFileReporter) {
        Logger::Debug("[TelemetryManager::Initialize] File reporter is enabled, creating metrics directory: %s", sanitized.metricsDirectory.c_str());
        try {
            std::filesystem::create_directories(sanitized.metricsDirectory);
            Logger::Info("Created telemetry directory: %s", sanitized.metricsDirectory.c_str());
        } catch (const std::exception& ex) {
            Logger::Error("[TelemetryManager::Initialize] Exception while creating metrics directory '%s': %s", sanitized.metricsDirectory.c_str(), ex.what());
            ReportError("Failed to create metrics directory: " + std::string(ex.what()));
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> configLock(m_configMutex);
        m_config = sanitized;
    }
    m_configGeneration.fetch_add(1);

    // Initialize snapshot storage
    {
        Logger::Debug("[TelemetryManager::Initialize] Initializing snapshot storage with capacity %zu", sanitized.maxSamplesInMemory);
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_snapshots.clear();
        m_snapshots.reserve(sanitized.maxSamplesInMemory);
        m_snapshotIndex = 0;
        m_snapshotCapacity = sanitized.maxSamplesInMemory;
    }

    std::vector<std::shared_ptr<ReporterRecord>> reporters;
    {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        reporters = m_reporters;
    }

    std::vector<std::shared_ptr<ReporterRecord>> failedReporters;
    for (const auto& reporter : reporters) {
        if (!InitializeReporter(reporter, sanitized.metricsDirectory)) {
            failedReporters.push_back(reporter);
        }
    }
    if (!failedReporters.empty()) {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        m_reporters.erase(
            std::remove_if(m_reporters.begin(), m_reporters.end(),
                           [&failedReporters](const auto& candidate) {
                               return std::find(failedReporters.begin(),
                                                failedReporters.end(),
                                                candidate) != failedReporters.end();
                           }),
            m_reporters.end());
    }

    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        m_startTime = std::chrono::steady_clock::now();
        m_initialized.store(true);
    }
    Logger::Info("TelemetryManager initialized successfully");
    return true;
}

void TelemetryManager::Shutdown() {
    Logger::Trace("[TelemetryManager::Shutdown] Entry");

    if (IsShutdownReporterCallback(this)) {
        // The enclosing shutdown already owns this request.
        return;
    }

    if (IsCurrentSampleOperationOwner()) {
        LatchPendingShutdown();
        return;
    }

    bool wasInitialized = false;
    bool deferShutdown = false;
    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        if (m_shuttingDown.load()) {
            return;
        }
        if (m_initializing || m_removingReporters || m_updatingConfig) {
            deferShutdown = true;
        } else {
            wasInitialized = m_initialized.exchange(false);
            m_shutdownPending.store(false);
            m_shuttingDown.store(true);
        }
    }
    if (deferShutdown) {
        LatchPendingShutdown();
        return;
    }
    auto shutdownGuard = MakeScopeExit([this]() {
        {
            std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
            m_shutdownPending.store(false);
            m_shuttingDown.store(false);
        }
        UnblockSamples();
    });

    m_running.store(false);
    m_samplingWaitCv.notify_all();
    m_sampleGateCv.notify_all();
    BlockAndDrainSamples();
    StopSampling();

    std::vector<std::shared_ptr<ReporterRecord>> reporters;
    {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        reporters.swap(m_reporters);
    }
    if (wasInitialized) {
        for (const auto& reporter : reporters) {
            RequestReporterShutdown(reporter);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_snapshots.clear();
        m_snapshotIndex = 0;
    }

    if (wasInitialized) Logger::Info("TelemetryManager shutdown complete");
}

bool TelemetryManager::AddReporter(std::unique_ptr<MetricsReporter> reporter) {
    Logger::Trace("[TelemetryManager::AddReporter] Entry - reporter=%p", (void*)reporter.get());
    if (!reporter) {
        Logger::Warn("[TelemetryManager::AddReporter] Ignoring null reporter");
        ReportError("Cannot add a null telemetry reporter");
        return false;
    }

    if (IsRestrictedLifecycleCallback(this)) {
        ReportError("Cannot add telemetry reporter from a reporter lifecycle callback");
        return false;
    }

    auto sharedReporter = std::shared_ptr<MetricsReporter>(std::move(reporter));
    auto record = std::make_shared<ReporterRecord>(std::move(sharedReporter));
    bool initializeNow = false;
    std::string outputDirectory;
    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        if (m_shutdownPending.load() || m_shuttingDown.load() || m_initializing ||
            m_removingReporters || m_updatingConfig) {
            ReportError("Cannot add telemetry reporter during a lifecycle transition");
            return false;
        }
        initializeNow = m_initialized.load();
        if (!initializeNow) {
            std::lock_guard<std::mutex> reporterLock(m_reporterMutex);
            m_reporters.push_back(record);
            return true;
        }
    }

    outputDirectory = GetConfig().metricsDirectory;
    if (!InitializeReporter(record, outputDirectory)) return false;

    bool accepted = false;
    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        bool configurationMatches = false;
        {
            std::lock_guard<std::mutex> configLock(m_configMutex);
            configurationMatches = m_config.metricsDirectory == outputDirectory;
        }
        if (m_initialized.load() && !m_shutdownPending.load() &&
            !m_shuttingDown.load() && !m_initializing &&
            !m_removingReporters && !m_updatingConfig && configurationMatches) {
            std::lock_guard<std::mutex> reporterLock(m_reporterMutex);
            m_reporters.push_back(record);
            accepted = true;
        }
    }
    if (!accepted) RequestReporterShutdown(record);
    return accepted;
}

void TelemetryManager::RemoveAllReporters() {
    Logger::Trace("[TelemetryManager::RemoveAllReporters] Entry");
    if (IsRestrictedLifecycleCallback(this)) {
        Logger::Warn("[TelemetryManager::RemoveAllReporters] Ignoring reentrant reporter lifecycle removal");
        return;
    }

    bool initialized = false;
    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        if (m_shuttingDown.load() || m_initializing || m_removingReporters ||
            m_updatingConfig) {
            return;
        }
        m_removingReporters = true;
        initialized = m_initialized.load();
    }
    auto removalGuard = MakeScopeExit([this]() {
        {
            std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
            m_removingReporters = false;
        }
        ProcessPendingShutdown();
    });

    std::vector<std::shared_ptr<ReporterRecord>> reporters;
    {
        std::lock_guard<std::mutex> reporterLock(m_reporterMutex);
        reporters.swap(m_reporters);
    }
    if (initialized) {
        for (const auto& reporter : reporters) {
            RequestReporterShutdown(reporter);
        }
    }

    Logger::Info("Removed all telemetry reporters");
}

bool TelemetryManager::InitializeReporter(
    const std::shared_ptr<ReporterRecord>& record,
    const std::string& outputDirectory) {
    if (!record) return false;

    std::shared_ptr<MetricsReporter> reporter;
    {
        std::lock_guard<std::mutex> lock(record->mutex);
        if (!record->reporter || record->initializing || record->initialized ||
            record->shutdownRequested) {
            return record->initialized;
        }
        record->initializing = true;
        reporter = record->reporter;
    }

    bool initialized = false;
    bool failureReported = false;
    try {
        ReporterCallbackScope callbackScope(this, ReporterCallbackPhase::Initialize);
        initialized = reporter->Initialize(outputDirectory);
    } catch (const std::exception& ex) {
        ReportError("Failed to initialize telemetry reporter: " + std::string(ex.what()));
        failureReported = true;
    } catch (...) {
        ReportError("Failed to initialize telemetry reporter: unknown exception");
        failureReported = true;
    }

    {
        std::lock_guard<std::mutex> lock(record->mutex);
        record->initializing = false;
        record->initialized = initialized;
        if (!initialized) record->reporter.reset();
    }
    if (!initialized && !failureReported) {
        ReportError("Telemetry reporter initialization returned false");
    }
    return initialized;
}

bool TelemetryManager::BeginReporterReport(
    const std::shared_ptr<ReporterRecord>& record,
    std::shared_ptr<MetricsReporter>& reporter) {
    if (!record) return false;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (!record->initialized || record->shutdownRequested ||
        record->shutdownStarted || !record->reporter) {
        return false;
    }
    ++record->activeReports;
    reporter = record->reporter;
    return true;
}

void TelemetryManager::EndReporterReport(
    const std::shared_ptr<ReporterRecord>& record) {
    bool shutdown = false;
    {
        std::lock_guard<std::mutex> lock(record->mutex);
        if (record->activeReports > 0) --record->activeReports;
        shutdown = record->activeReports == 0 && record->shutdownRequested &&
                   !record->shutdownStarted;
    }
    if (shutdown) InvokeReporterShutdown(record);
}

void TelemetryManager::RequestReporterShutdown(
    const std::shared_ptr<ReporterRecord>& record) {
    if (!record) return;
    bool shutdown = false;
    {
        std::lock_guard<std::mutex> lock(record->mutex);
        record->shutdownRequested = true;
        if (!record->initialized) {
            record->reporter.reset();
            return;
        }
        shutdown = record->activeReports == 0 && !record->shutdownStarted;
    }
    if (shutdown) InvokeReporterShutdown(record);
}

void TelemetryManager::InvokeReporterShutdown(
    const std::shared_ptr<ReporterRecord>& record) {
    std::shared_ptr<MetricsReporter> reporter;
    {
        std::lock_guard<std::mutex> lock(record->mutex);
        if (!record->initialized || record->shutdownStarted ||
            record->activeReports != 0) {
            return;
        }
        record->shutdownStarted = true;
        reporter = record->reporter;
    }

    try {
        ReporterCallbackScope callbackScope(this, ReporterCallbackPhase::Shutdown);
        reporter->Shutdown();
    } catch (const std::exception& ex) {
        ReportError("Telemetry reporter shutdown failed: " + std::string(ex.what()));
    } catch (...) {
        ReportError("Telemetry reporter shutdown failed: unknown exception");
    }

    {
        std::lock_guard<std::mutex> lock(record->mutex);
        record->initialized = false;
        record->reporter.reset();
    }
}

bool TelemetryManager::BeginSampleOperation() {
    std::unique_lock<std::mutex> lock(m_sampleGateMutex);
    if (m_sampleOperationActive &&
        m_sampleOperationOwner == std::this_thread::get_id()) {
        return false;
    }
    m_sampleGateCv.wait(lock, [this]() {
        return !m_sampleOperationActive &&
               (!m_samplesBlocked || !m_initialized.load());
    });
    if (m_samplesBlocked) return false;
    m_sampleOperationActive = true;
    m_sampleOperationOwner = std::this_thread::get_id();
    return true;
}

void TelemetryManager::EndSampleOperation() {
    {
        std::lock_guard<std::mutex> lock(m_sampleGateMutex);
        m_sampleOperationActive = false;
        m_sampleOperationOwner = std::thread::id{};
    }
    m_sampleGateCv.notify_all();
}

bool TelemetryManager::IsCurrentSampleOperationOwner() const {
    std::lock_guard<std::mutex> lock(m_sampleGateMutex);
    return m_sampleOperationActive &&
           m_sampleOperationOwner == std::this_thread::get_id();
}

void TelemetryManager::BlockAndDrainSamples() {
    std::unique_lock<std::mutex> lock(m_sampleGateMutex);
    m_samplesBlocked = true;
    m_sampleGateCv.notify_all();
    if (m_sampleOperationActive &&
        m_sampleOperationOwner == std::this_thread::get_id()) {
        return;
    }
    m_sampleGateCv.wait(lock, [this]() { return !m_sampleOperationActive; });
}

void TelemetryManager::UnblockSamples() {
    {
        std::lock_guard<std::mutex> lock(m_sampleGateMutex);
        m_samplesBlocked = false;
    }
    m_sampleGateCv.notify_all();
}

void TelemetryManager::LatchPendingShutdown() {
    m_shutdownPending.store(true);
    m_running.store(false);
    {
        std::lock_guard<std::mutex> lock(m_sampleGateMutex);
        m_samplesBlocked = true;
    }
    m_samplingWaitCv.notify_all();
    m_sampleGateCv.notify_all();
}

void TelemetryManager::ProcessPendingShutdown() {
    if (m_shutdownPending.load() && !IsInsideReporterCallback(this)) {
        Shutdown();
    }
}

void TelemetryManager::StartSampling() {
    Logger::Trace("[TelemetryManager::StartSampling] Entry");
    if (IsInsideReporterCallback(this)) {
        ReportError("Cannot start telemetry sampling from a reporter callback");
        return;
    }

    std::lock_guard<std::mutex> threadLock(m_samplingThreadMutex);
    const TelemetryConfig config = GetConfig();
    if (m_shutdownPending.load() || !m_initialized.load() || !config.enabled) {
        Logger::Info("Telemetry disabled, not starting sampling");
        return;
    }

    if (m_running.load()) {
        Logger::Warn("Telemetry sampling already running");
        return;
    }

    if (m_samplingThread.joinable()) {
        if (m_samplingThread.get_id() == std::this_thread::get_id()) {
            Logger::Error("Telemetry sampling cannot be restarted from its own sampling thread");
            return;
        }
        m_samplingThread.join();
    }

    Logger::Info("Starting telemetry sampling (interval: %lldms)",
                 config.samplingInterval.count());
    m_running.store(true);
    try {
        m_samplingThread = std::thread(&TelemetryManager::SamplingLoop, this);
    } catch (const std::exception& ex) {
        m_running.store(false);
        ReportError("Failed to start telemetry sampling thread: " + std::string(ex.what()));
    } catch (...) {
        m_running.store(false);
        ReportError("Failed to start telemetry sampling thread: unknown exception");
    }
}

void TelemetryManager::StopSampling() {
    Logger::Trace("[TelemetryManager::StopSampling] Entry");
    if (IsInsideReporterCallback(this)) {
        // Never join a sampler from a callback: a forced sample can own the
        // sample lease while the background sampler waits for it.
        m_running.store(false);
        m_samplingWaitCv.notify_all();
        return;
    }

    std::lock_guard<std::mutex> threadLock(m_samplingThreadMutex);
    const bool wasRunning = m_running.exchange(false);

    // Wake a sampling thread immediately even when the configured interval is
    // minutes or hours. A plain sleep_for made shutdown block for the entire
    // remaining interval.
    m_samplingWaitCv.notify_all();

    if (wasRunning) Logger::Info("Stopping telemetry sampling...");

    if (m_samplingThread.joinable()) {
        if (m_samplingThread.get_id() == std::this_thread::get_id()) {
            Logger::Warn("Telemetry sampler requested its own stop; join deferred to its owner thread");
            return;
        }
        Logger::Debug("[TelemetryManager::StopSampling] Waiting for sampling thread to join");
        m_samplingThread.join();
        Logger::Debug("[TelemetryManager::StopSampling] Sampling thread joined successfully");
    } else {
        Logger::Debug("[TelemetryManager::StopSampling] Sampling was not running and no thread needs joining");
    }

    if (wasRunning) Logger::Info("Telemetry sampling stopped");
}

void TelemetryManager::ForceSample() {
    Logger::Trace("[TelemetryManager::ForceSample] Entry");
    if (IsInsideReporterCallback(this)) {
        ReportError("Ignoring reentrant telemetry sample from a reporter callback");
        return;
    }
    if (!BeginSampleOperation()) {
        ReportError("Ignoring reentrant telemetry sample");
        return;
    }
    auto sampleGuard = MakeScopeExit([this]() {
        EndSampleOperation();
        ProcessPendingShutdown();
    });

    const TelemetryConfig config = GetConfig();
    if (!m_initialized.load() || !config.enabled) {
        Logger::Debug("[TelemetryManager::ForceSample] Telemetry disabled, skipping forced sample");
        return;
    }

    try {
        Logger::Debug("[TelemetryManager::ForceSample] Collecting metrics snapshot");
        MetricsSnapshot snapshot = CollectSnapshot(config);
        Logger::Trace("[TelemetryManager::ForceSample] Snapshot collected successfully");

        // Store snapshot
        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            const size_t capacity = m_snapshotCapacity;
            if (m_snapshots.size() < capacity) {
                m_snapshots.push_back(snapshot);
                Logger::Trace("[TelemetryManager::ForceSample] Snapshot appended to storage (size now %zu/%zu)", m_snapshots.size(), capacity);
            } else {
                m_snapshots[m_snapshotIndex] = snapshot;
                Logger::Trace("[TelemetryManager::ForceSample] Snapshot stored at circular buffer index %zu (buffer full, overwriting)", m_snapshotIndex);
                m_snapshotIndex = (m_snapshotIndex + 1) % capacity;
                Logger::Trace("[TelemetryManager::ForceSample] Circular buffer index advanced to %zu", m_snapshotIndex);
            }
        }

        std::vector<std::shared_ptr<ReporterRecord>> reporters;
        {
            std::lock_guard<std::mutex> lock(m_reporterMutex);
            reporters = m_reporters;
        }
        Logger::Debug("[TelemetryManager::ForceSample] Reporting snapshot to %zu reporters", reporters.size());
        for (const auto& record : reporters) {
            std::shared_ptr<MetricsReporter> reporter;
            if (!BeginReporterReport(record, reporter)) continue;
            auto reportGuard = MakeScopeExit([this, record]() {
                EndReporterReport(record);
            });
            try {
                ReporterCallbackScope callbackScope(this, ReporterCallbackPhase::Report);
                reporter->Report(snapshot);
            } catch (const std::exception& ex) {
                ReportError("Reporter failed: " + std::string(ex.what()));
            } catch (...) {
                ReportError("Reporter failed: unknown exception");
            }
            reportGuard.RunNow();
            if (m_shutdownPending.load() || m_shuttingDown.load()) break;
        }

        auto totalNow = m_totalSamples.fetch_add(1, std::memory_order_relaxed) + 1;
        Logger::Debug("[TelemetryManager::ForceSample] Total samples collected: %llu", (unsigned long long)totalNow);
        // One info line per second drowns actionable server/network events.
        // Keep an initial health marker and a minute cadence; detailed samples
        // remain available at trace level and through the reporters.
        if (totalNow == 1 || totalNow % 60 == 0) {
            Logger::Info("[TelemetryManager::ForceSample] Metrics snapshot #%llu collected and reported successfully", (unsigned long long)totalNow);
        } else {
            Logger::Trace("[TelemetryManager::ForceSample] Metrics snapshot #%llu collected and reported successfully", (unsigned long long)totalNow);
        }

    } catch (const std::exception& ex) {
        Logger::Error("[TelemetryManager::ForceSample] Exception during snapshot collection: %s", ex.what());
        ReportError("Failed to collect metrics snapshot: " + std::string(ex.what()));
    } catch (...) {
        Logger::Error("[TelemetryManager::ForceSample] Unknown exception during snapshot collection");
        ReportError("Failed to collect metrics snapshot: unknown exception");
    }
    Logger::Trace("[TelemetryManager::ForceSample] Exit");
}

void TelemetryManager::SamplingLoop() {
    Logger::Info("Telemetry sampling loop started");

    uint64_t iterationCount = 0;
    while (m_running.load()) {
        auto loopStart = std::chrono::steady_clock::now();
        iterationCount++;
        Logger::Trace("[TelemetryManager::SamplingLoop] Iteration %llu starting", (unsigned long long)iterationCount);

        try {
            ForceSample();
        } catch (const std::exception& ex) {
            Logger::Error("[TelemetryManager::SamplingLoop] Exception during iteration %llu: %s", (unsigned long long)iterationCount, ex.what());
            ReportError("Sampling loop error: " + std::string(ex.what()));
        } catch (...) {
            // A non-std exception must not escape this thread function into
            // std::terminate. Record it and keep sampling.
            Logger::Error("[TelemetryManager::SamplingLoop] Non-std exception during iteration %llu", (unsigned long long)iterationCount);
            ReportError("Sampling loop error: non-std exception");
        }

        // Calculate sleep time to maintain consistent interval
        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart);
        const uint64_t configGeneration = m_configGeneration.load();
        const auto interval = GetConfig().samplingInterval;
        auto sleepTime = interval - elapsed;

        if (sleepTime > std::chrono::milliseconds(0)) {
            Logger::Trace("[TelemetryManager::SamplingLoop] Iteration %llu took %lldms, sleeping for %lldms", (unsigned long long)iterationCount, elapsed.count(), sleepTime.count());
            std::unique_lock<std::mutex> waitLock(m_samplingWaitMutex);
            m_samplingWaitCv.wait_for(waitLock, sleepTime, [this, configGeneration]() {
                return !m_running.load() ||
                       m_configGeneration.load() != configGeneration;
            });
        } else if (elapsed > interval * 2) {
            // Warn if sampling is taking too long
            Logger::Warn("Telemetry sampling took %lldms (interval: %lldms)",
                        elapsed.count(), interval.count());
            Logger::Debug("[TelemetryManager::SamplingLoop] Iteration %llu exceeded 2x interval: elapsed=%lldms, interval=%lldms", (unsigned long long)iterationCount, elapsed.count(), interval.count());
        } else {
            Logger::Debug("[TelemetryManager::SamplingLoop] Iteration %llu took %lldms, no sleep needed (exceeded interval)", (unsigned long long)iterationCount, elapsed.count());
        }
    }

    Logger::Info("Telemetry sampling loop stopped");
    Logger::Debug("[TelemetryManager::SamplingLoop] Completed %llu iterations total", (unsigned long long)iterationCount);
    Logger::Trace("[TelemetryManager::SamplingLoop] Exit");
}

MetricsSnapshot TelemetryManager::CollectSnapshot(const TelemetryConfig& config) {
    Logger::Trace("[TelemetryManager::CollectSnapshot] Entry");
    MetricsSnapshot snapshot;
    snapshot.timestamp = std::chrono::system_clock::now();
    Logger::Trace("[TelemetryManager::CollectSnapshot] Timestamp set for snapshot");

    try {
        if (config.enableSystemMetrics) {
            Logger::Debug("[TelemetryManager::CollectSnapshot] System metrics collection enabled, collecting system metrics");
            CollectSystemMetrics(snapshot);
            Logger::Debug("[TelemetryManager::CollectSnapshot] System metrics collected successfully");
        } else {
            Logger::Debug("[TelemetryManager::CollectSnapshot] System metrics collection is disabled, skipping");
        }

        if (config.enableApplicationMetrics) {
            Logger::Debug("[TelemetryManager::CollectSnapshot] Application metrics collection enabled, collecting application metrics");
            CollectApplicationMetrics(snapshot);
            Logger::Debug("[TelemetryManager::CollectSnapshot] Application metrics collected successfully");
        } else {
            Logger::Debug("[TelemetryManager::CollectSnapshot] Application metrics collection is disabled, skipping");
        }

    } catch (const std::exception& ex) {
        Logger::Error("[TelemetryManager::CollectSnapshot] Exception during metrics collection: %s", ex.what());
        ReportError("Error collecting metrics: " + std::string(ex.what()));
    }

    Logger::Trace("[TelemetryManager::CollectSnapshot] Exit - returning snapshot");
    return snapshot;
}

void TelemetryManager::CollectSystemMetrics(MetricsSnapshot& snapshot) {
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Entry");

    // CPU Usage
    snapshot.cpuUsagePercent = GetCPUUsage();
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] CPU usage: %.2f%%", snapshot.cpuUsagePercent);

    // Memory Usage
    auto [memUsed, memTotal] = GetMemoryUsage();
    snapshot.memoryUsedBytes = memUsed;
    snapshot.memoryTotalBytes = memTotal;
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Memory: used=%llu bytes, total=%llu bytes", (unsigned long long)memUsed, (unsigned long long)memTotal);

    // Network Stats
    auto [netSent, netRecv] = GetNetworkStats();
    snapshot.networkBytesSent = netSent;
    snapshot.networkBytesReceived = netRecv;
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Network: sent=%llu bytes, received=%llu bytes", (unsigned long long)netSent, (unsigned long long)netRecv);

    // Disk Stats
    auto [diskRead, diskWrite] = GetDiskStats();
    snapshot.diskReadBytes = diskRead;
    snapshot.diskWriteBytes = diskWrite;
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Disk: read=%llu bytes, write=%llu bytes", (unsigned long long)diskRead, (unsigned long long)diskWrite);

    Logger::Debug("[TelemetryManager::CollectSystemMetrics] All system metrics collected: cpu=%.2f%%, memUsed=%llu, memTotal=%llu, netSent=%llu, netRecv=%llu, diskRead=%llu, diskWrite=%llu",
                 snapshot.cpuUsagePercent, (unsigned long long)memUsed, (unsigned long long)memTotal,
                 (unsigned long long)netSent, (unsigned long long)netRecv,
                 (unsigned long long)diskRead, (unsigned long long)diskWrite);
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Exit");
}

void TelemetryManager::CollectApplicationMetrics(MetricsSnapshot& snapshot) {
    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Entry");

    // Copy atomic values to snapshot
    snapshot.activeConnections = m_customMetrics.activeConnections.load(std::memory_order_relaxed);
    snapshot.authenticatedPlayers = m_customMetrics.authenticatedPlayers.load(std::memory_order_relaxed);
    snapshot.totalPacketsProcessed = m_customMetrics.totalPacketsProcessed.load(std::memory_order_relaxed);
    snapshot.totalPacketsDropped = m_customMetrics.totalPacketsDropped.load(std::memory_order_relaxed);
    snapshot.currentTick = m_customMetrics.currentTick.load(std::memory_order_relaxed);
    snapshot.averageLatencyMs = m_customMetrics.averageLatencyMs.load(std::memory_order_relaxed);
    snapshot.packetLossRate = m_customMetrics.packetLossRate.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Network metrics: activeConnections=%llu, authenticatedPlayers=%llu, totalPacketsProcessed=%llu, totalPacketsDropped=%llu, currentTick=%llu, avgLatency=%.2fms, packetLossRate=%.4f",
                 (unsigned long long)snapshot.activeConnections, (unsigned long long)snapshot.authenticatedPlayers,
                 (unsigned long long)snapshot.totalPacketsProcessed, (unsigned long long)snapshot.totalPacketsDropped,
                 (unsigned long long)snapshot.currentTick, snapshot.averageLatencyMs, snapshot.packetLossRate);

    snapshot.activeMatches = m_customMetrics.activeMatches.load(std::memory_order_relaxed);
    snapshot.totalKills = m_customMetrics.totalKills.load(std::memory_order_relaxed);
    snapshot.totalDeaths = m_customMetrics.totalDeaths.load(std::memory_order_relaxed);
    snapshot.objectivesCaptured = m_customMetrics.objectivesCaptured.load(std::memory_order_relaxed);
    snapshot.chatMessagesSent = m_customMetrics.chatMessagesSent.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Gameplay metrics: activeMatches=%llu, totalKills=%llu, totalDeaths=%llu, objectivesCaptured=%llu, chatMessages=%llu",
                 (unsigned long long)snapshot.activeMatches, (unsigned long long)snapshot.totalKills, (unsigned long long)snapshot.totalDeaths,
                 (unsigned long long)snapshot.objectivesCaptured, (unsigned long long)snapshot.chatMessagesSent);

    snapshot.frameTimeMs = m_customMetrics.frameTimeMs.load(std::memory_order_relaxed);
    snapshot.physicsTimeMs = m_customMetrics.physicsTimeMs.load(std::memory_order_relaxed);
    snapshot.networkTimeMs = m_customMetrics.networkTimeMs.load(std::memory_order_relaxed);
    snapshot.gameLogicTimeMs = m_customMetrics.gameLogicTimeMs.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Performance metrics: frameTime=%.2fms, physicsTime=%.2fms, networkTime=%.2fms, gameLogicTime=%.2fms",
                 snapshot.frameTimeMs, snapshot.physicsTimeMs, snapshot.networkTimeMs, snapshot.gameLogicTimeMs);

    snapshot.securityViolations = m_customMetrics.securityViolations.load(std::memory_order_relaxed);
    snapshot.malformedPackets = m_customMetrics.malformedPackets.load(std::memory_order_relaxed);
    snapshot.speedHackDetections = m_customMetrics.speedHackDetections.load(std::memory_order_relaxed);
    snapshot.kickedPlayers = m_customMetrics.kickedPlayers.load(std::memory_order_relaxed);
    snapshot.bannedPlayers = m_customMetrics.bannedPlayers.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Security metrics: violations=%llu, malformedPackets=%llu, speedHackDetections=%llu, kicked=%llu, banned=%llu",
                 (unsigned long long)snapshot.securityViolations, (unsigned long long)snapshot.malformedPackets,
                 (unsigned long long)snapshot.speedHackDetections, (unsigned long long)snapshot.kickedPlayers,
                 (unsigned long long)snapshot.bannedPlayers);

    Logger::Debug("[TelemetryManager::CollectApplicationMetrics] All application metrics collected successfully");
    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Exit");
}

double TelemetryManager::GetCPUUsage() {
    Logger::Trace("[TelemetryManager::GetCPUUsage] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetCPUUsage] Platform: Linux, reading /proc/stat");
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        Logger::Warn("[TelemetryManager::GetCPUUsage] Failed to open /proc/stat");
        Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (cannot open /proc/stat)");
        return 0.0;
    }

    CPUTimes current;
    std::string cpu;
    stat >> cpu >> current.user >> current.nice >> current.system >> current.idle
         >> current.iowait >> current.irq >> current.softirq >> current.steal;
    stat.close();
    Logger::Trace("[TelemetryManager::GetCPUUsage] Read CPU times: user=%llu, nice=%llu, system=%llu, idle=%llu, iowait=%llu, irq=%llu, softirq=%llu, steal=%llu",
                 (unsigned long long)current.user, (unsigned long long)current.nice, (unsigned long long)current.system,
                 (unsigned long long)current.idle, (unsigned long long)current.iowait, (unsigned long long)current.irq,
                 (unsigned long long)current.softirq, (unsigned long long)current.steal);

    if (!m_hasPreviousCPUTimes) {
        Logger::Debug("[TelemetryManager::GetCPUUsage] No previous CPU times available, storing baseline and returning 0.0");
        m_lastCPUTimes = current;
        m_hasPreviousCPUTimes = true;
        Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (first sample baseline)");
        return 0.0;
    }

    uint64_t totalDelta = (current.user + current.nice + current.system + current.idle +
                          current.iowait + current.irq + current.softirq + current.steal) -
                         (m_lastCPUTimes.user + m_lastCPUTimes.nice + m_lastCPUTimes.system +
                          m_lastCPUTimes.idle + m_lastCPUTimes.iowait + m_lastCPUTimes.irq +
                          m_lastCPUTimes.softirq + m_lastCPUTimes.steal);

    uint64_t idleDelta = current.idle - m_lastCPUTimes.idle;
    Logger::Trace("[TelemetryManager::GetCPUUsage] Computed deltas: totalDelta=%llu, idleDelta=%llu", (unsigned long long)totalDelta, (unsigned long long)idleDelta);

    m_lastCPUTimes = current;

    if (totalDelta == 0) {
        Logger::Debug("[TelemetryManager::GetCPUUsage] Total delta is zero, no CPU activity detected");
        Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (zero delta)");
        return 0.0;
    }

    double cpuUsage = 100.0 * (totalDelta - idleDelta) / totalDelta;
    Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning %.2f%%", cpuUsage);
    return cpuUsage;

#elif defined(_WIN32)
    Logger::Trace("[TelemetryManager::GetCPUUsage] Platform: Windows, using PDH counters");
    if (!m_windowsCpuCounter) {
        m_windowsCpuCounter = std::make_unique<WindowsCpuCounterState>();
    }
    return m_windowsCpuCounter->Sample();

#else
    // macOS or other platforms - simplified implementation
    Logger::Debug("[TelemetryManager::GetCPUUsage] Platform not supported for CPU metrics, returning 0.0");
    Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (unsupported platform)");
    return 0.0;
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetMemoryUsage() {
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Platform: Linux, reading /proc/meminfo");
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        Logger::Warn("[TelemetryManager::GetMemoryUsage] Failed to open /proc/meminfo");
        Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {0, 0} (cannot open /proc/meminfo)");
        return {0, 0};
    }

    uint64_t memTotal = 0, memFree = 0, buffers = 0, cached = 0;
    std::string line;

    while (std::getline(meminfo, line)) {
        std::istringstream iss(line);
        std::string key;
        uint64_t value;
        std::string unit;

        if (iss >> key >> value >> unit) {
            if (key == "MemTotal:") memTotal = value * 1024;
            else if (key == "MemFree:") memFree = value * 1024;
            else if (key == "Buffers:") buffers = value * 1024;
            else if (key == "Cached:") cached = value * 1024;
        }
    }
    meminfo.close();

    uint64_t memUsed = memTotal - memFree - buffers - cached;
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Parsed /proc/meminfo: total=%llu, free=%llu, buffers=%llu, cached=%llu, used=%llu",
                 (unsigned long long)memTotal, (unsigned long long)memFree, (unsigned long long)buffers,
                 (unsigned long long)cached, (unsigned long long)memUsed);
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {%llu, %llu}", (unsigned long long)memUsed, (unsigned long long)memTotal);
    return {memUsed, memTotal};

#elif defined(_WIN32)
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Platform: Windows, using GlobalMemoryStatusEx");
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        uint64_t memUsed = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
        Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {%llu, %llu} (Windows)", (unsigned long long)memUsed, (unsigned long long)memStatus.ullTotalPhys);
        return {memUsed, memStatus.ullTotalPhys};
    }
    Logger::Warn("[TelemetryManager::GetMemoryUsage] GlobalMemoryStatusEx failed");
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {0, 0} (Windows API failed)");
    return {0, 0};

#else
    Logger::Debug("[TelemetryManager::GetMemoryUsage] Platform not supported for memory metrics, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {0, 0} (unsupported platform)");
    return {0, 0};
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetNetworkStats() {
    Logger::Trace("[TelemetryManager::GetNetworkStats] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetNetworkStats] Platform: Linux, reading /proc/net/dev");
    std::ifstream netdev("/proc/net/dev");
    if (!netdev.is_open()) {
        Logger::Warn("[TelemetryManager::GetNetworkStats] Failed to open /proc/net/dev");
        Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {0, 0} (cannot open /proc/net/dev)");
        return {0, 0};
    }

    uint64_t totalSent = 0, totalReceived = 0;
    std::string line;

    // Skip header lines
    std::getline(netdev, line);
    std::getline(netdev, line);
    Logger::Trace("[TelemetryManager::GetNetworkStats] Skipped 2 header lines from /proc/net/dev");

    int interfaceCount = 0;
    while (std::getline(netdev, line)) {
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            Logger::Trace("[TelemetryManager::GetNetworkStats] Skipping line without colon separator");
            continue;
        }

        std::string interface = line.substr(0, colonPos);
        // Trim whitespace
        interface.erase(0, interface.find_first_not_of(" \t"));
        interface.erase(interface.find_last_not_of(" \t") + 1);

        // Skip loopback interface
        if (interface == "lo") {
            Logger::Trace("[TelemetryManager::GetNetworkStats] Skipping loopback interface 'lo'");
            continue;
        }

        std::istringstream iss(line.substr(colonPos + 1));
        uint64_t recvBytes, recvPackets, recvErrs, recvDrop, recvFifo, recvFrame, recvCompressed, recvMulticast;
        uint64_t transBytes, transPackets, transErrs, transDrop, transFifo, transColls, transCarrier, transCompressed;

        if (iss >> recvBytes >> recvPackets >> recvErrs >> recvDrop >> recvFifo >> recvFrame >> recvCompressed >> recvMulticast
               >> transBytes >> transPackets >> transErrs >> transDrop >> transFifo >> transColls >> transCarrier >> transCompressed) {
            totalReceived += recvBytes;
            totalSent += transBytes;
            interfaceCount++;
            Logger::Trace("[TelemetryManager::GetNetworkStats] Interface '%s': recv=%llu bytes, sent=%llu bytes", interface.c_str(), (unsigned long long)recvBytes, (unsigned long long)transBytes);
        } else {
            Logger::Warn("[TelemetryManager::GetNetworkStats] Failed to parse stats for interface '%s'", interface.c_str());
        }
    }
    netdev.close();

    Logger::Debug("[TelemetryManager::GetNetworkStats] Parsed %d network interfaces: totalSent=%llu, totalReceived=%llu", interfaceCount, (unsigned long long)totalSent, (unsigned long long)totalReceived);
    Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {%llu, %llu}", (unsigned long long)totalSent, (unsigned long long)totalReceived);
    return {totalSent, totalReceived};

#elif defined(_WIN32)
    Logger::Trace("[TelemetryManager::GetNetworkStats] Platform: Windows, querying GetIfTable2");

    MIB_IF_TABLE2* rawTable = nullptr;
    const NETIO_STATUS status = GetIfTable2(&rawTable);
    if (status != NO_ERROR) {
        Logger::Warn("[TelemetryManager::GetNetworkStats] GetIfTable2 failed with status %lu",
                     static_cast<unsigned long>(status));
        ReportError("GetIfTable2 failed with status " + std::to_string(status));
        return {0, 0};
    }
    if (rawTable == nullptr) {
        Logger::Warn("[TelemetryManager::GetNetworkStats] GetIfTable2 succeeded without returning a table");
        ReportError("GetIfTable2 succeeded without returning a table");
        return {0, 0};
    }

    struct MibTableReleaser {
        void operator()(MIB_IF_TABLE2* table) const noexcept {
            FreeMibTable(table);
        }
    };
    const std::unique_ptr<MIB_IF_TABLE2, MibTableReleaser> table(rawTable);

    Detail::NetworkInterfaceCounterTotals totals;
    size_t includedInterfaces = 0;
    size_t excludedInterfaces = 0;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IF_ROW2& row = table->Table[index];
        const Detail::NetworkInterfaceCounterSample sample{
            row.Type == IF_TYPE_SOFTWARE_LOOPBACK,
            row.InterfaceAndOperStatusFlags.FilterInterface != FALSE,
            static_cast<uint64_t>(row.OutOctets),
            static_cast<uint64_t>(row.InOctets),
        };

        if (sample.isLoopback || sample.isFilterInterface) {
            ++excludedInterfaces;
        } else {
            ++includedInterfaces;
        }
        Detail::AccumulateNetworkInterfaceCounters(totals, sample);
    }

    // Do not filter by the interface's current OperStatus. These are cumulative
    // counters, so removing an interface when it transitions down would make the
    // machine total regress and create false negative byte rates.
    Logger::Debug("[TelemetryManager::GetNetworkStats] Windows interfaces: included=%zu, excluded=%zu, sent=%llu, received=%llu",
                  includedInterfaces, excludedInterfaces,
                  static_cast<unsigned long long>(totals.bytesSent),
                  static_cast<unsigned long long>(totals.bytesReceived));
    Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {%llu, %llu} (Windows GetIfTable2)",
                  static_cast<unsigned long long>(totals.bytesSent),
                  static_cast<unsigned long long>(totals.bytesReceived));
    return {totals.bytesSent, totals.bytesReceived};

#else
    Logger::Debug("[TelemetryManager::GetNetworkStats] Platform not supported for network metrics, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {0, 0} (unsupported platform)");
    return {0, 0};
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetDiskStats() {
    Logger::Trace("[TelemetryManager::GetDiskStats] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetDiskStats] Platform: Linux, reading /proc/diskstats");
    std::ifstream diskstats("/proc/diskstats");
    if (!diskstats.is_open()) {
        Logger::Warn("[TelemetryManager::GetDiskStats] Failed to open /proc/diskstats");
        Logger::Trace("[TelemetryManager::GetDiskStats] Exit - returning {0, 0} (cannot open /proc/diskstats)");
        return {0, 0};
    }

    uint64_t totalRead = 0, totalWrite = 0;
    std::string line;
    int deviceCount = 0;

    while (std::getline(diskstats, line)) {
        std::istringstream iss(line);
        int major, minor;
        std::string device;
        uint64_t readIOs, readMerges, readSectors, readTicks;
        uint64_t writeIOs, writeMerges, writeSectors, writeTicks;
        uint64_t inFlight, ioTicks, timeInQueue;

        if (iss >> major >> minor >> device >> readIOs >> readMerges >> readSectors >> readTicks
               >> writeIOs >> writeMerges >> writeSectors >> writeTicks >> inFlight >> ioTicks >> timeInQueue) {
            // Only count main disk devices (not partitions)
            if (device.find_first_of("0123456789") == std::string::npos) {
                totalRead += readSectors * 512;  // sectors are 512 bytes
                totalWrite += writeSectors * 512;
                deviceCount++;
                Logger::Trace("[TelemetryManager::GetDiskStats] Device '%s': readSectors=%llu (%llu bytes), writeSectors=%llu (%llu bytes)",
                             device.c_str(), (unsigned long long)readSectors, (unsigned long long)(readSectors * 512),
                             (unsigned long long)writeSectors, (unsigned long long)(writeSectors * 512));
            } else {
                Logger::Trace("[TelemetryManager::GetDiskStats] Skipping partition device '%s'", device.c_str());
            }
        }
    }
    diskstats.close();

    Logger::Debug("[TelemetryManager::GetDiskStats] Parsed %d disk devices: totalRead=%llu bytes, totalWrite=%llu bytes", deviceCount, (unsigned long long)totalRead, (unsigned long long)totalWrite);
    Logger::Trace("[TelemetryManager::GetDiskStats] Exit - returning {%llu, %llu}", (unsigned long long)totalRead, (unsigned long long)totalWrite);
    return {totalRead, totalWrite};

#else
    Logger::Debug("[TelemetryManager::GetDiskStats] Platform not supported for disk metrics, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetDiskStats] Exit - returning {0, 0} (unsupported platform)");
    return {0, 0};
#endif
}

MetricsSnapshot TelemetryManager::GetLatestSnapshot() const {
    Logger::Trace("[TelemetryManager::GetLatestSnapshot] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    if (m_snapshots.empty()) {
        Logger::Debug("[TelemetryManager::GetLatestSnapshot] No snapshots available, returning default empty snapshot");
        Logger::Trace("[TelemetryManager::GetLatestSnapshot] Exit - returning default MetricsSnapshot (empty)");
        return MetricsSnapshot{};
    }

    const size_t capacity = m_snapshotCapacity;
    if (m_snapshots.size() < capacity) {
        Logger::Debug("[TelemetryManager::GetLatestSnapshot] Linear storage mode, returning last snapshot (index %zu of %zu)", m_snapshots.size() - 1, m_snapshots.size());
        Logger::Trace("[TelemetryManager::GetLatestSnapshot] Exit - returning snapshot from back of linear storage");
        return m_snapshots.back();
    } else {
        size_t latest = (m_snapshotIndex + capacity - 1) % capacity;
        Logger::Debug("[TelemetryManager::GetLatestSnapshot] Circular buffer mode, returning snapshot at index %zu (snapshotIndex=%zu, maxSamples=%zu)", latest, m_snapshotIndex, capacity);
        Logger::Trace("[TelemetryManager::GetLatestSnapshot] Exit - returning snapshot from circular buffer index %zu", latest);
        return m_snapshots[latest];
    }
}

std::vector<MetricsSnapshot> TelemetryManager::GetRecentSnapshots(size_t count) const {
    Logger::Trace("[TelemetryManager::GetRecentSnapshots] Entry - count=%zu", count);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    std::vector<MetricsSnapshot> result;

    if (m_snapshots.empty()) {
        Logger::Debug("[TelemetryManager::GetRecentSnapshots] No snapshots available, returning empty vector");
        Logger::Trace("[TelemetryManager::GetRecentSnapshots] Exit - returning empty vector");
        return result;
    }

    size_t available = std::min(count, m_snapshots.size());
    result.reserve(available);
    Logger::Debug("[TelemetryManager::GetRecentSnapshots] Requested %zu snapshots, %zu available, returning %zu", count, m_snapshots.size(), available);

    const size_t capacity = m_snapshotCapacity;
    if (m_snapshots.size() < capacity) {
        // Linear storage
        size_t start = m_snapshots.size() >= available ? m_snapshots.size() - available : 0;
        Logger::Debug("[TelemetryManager::GetRecentSnapshots] Using linear storage mode, reading from index %zu to %zu", start, m_snapshots.size() - 1);
        for (size_t i = start; i < m_snapshots.size(); ++i) {
            result.push_back(m_snapshots[i]);
        }
    } else {
        // Circular buffer
        Logger::Debug("[TelemetryManager::GetRecentSnapshots] Using circular buffer mode, snapshotIndex=%zu, maxSamples=%zu", m_snapshotIndex, capacity);
        for (size_t i = 0; i < available; ++i) {
            size_t index = (m_snapshotIndex + capacity - available + i) % capacity;
            Logger::Trace("[TelemetryManager::GetRecentSnapshots] Reading circular buffer at index %zu", index);
            result.push_back(m_snapshots[index]);
        }
    }

    Logger::Trace("[TelemetryManager::GetRecentSnapshots] Exit - returning %zu snapshots", result.size());
    return result;
}

void TelemetryManager::UpdateConfig(const TelemetryConfig& config) {
    Logger::Trace("[TelemetryManager::UpdateConfig] Entry - config.enabled=%d, config.samplingInterval=%lldms, config.enableSystemMetrics=%d, config.enableApplicationMetrics=%d",
                 config.enabled, config.samplingInterval.count(), config.enableSystemMetrics, config.enableApplicationMetrics);
    if (IsInsideReporterCallback(this)) {
        ReportError("Cannot update telemetry configuration from a reporter callback");
        return;
    }

    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        if (m_shuttingDown.load() || m_initializing || m_removingReporters ||
            m_updatingConfig) {
            ReportError("Cannot update telemetry configuration during a lifecycle transition");
            return;
        }
        m_updatingConfig = true;
    }
    auto transitionGuard = MakeScopeExit([this]() {
        {
            std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
            m_updatingConfig = false;
        }
        ProcessPendingShutdown();
    });

    if (!BeginSampleOperation()) {
        ReportError("Ignoring reentrant telemetry configuration update");
        return;
    }
    auto sampleGuard = MakeScopeExit([this]() { EndSampleOperation(); });

    const TelemetryConfig sanitized = SanitizeConfig(config);
    const TelemetryConfig previous = GetConfig();
    {
        std::lock_guard<std::mutex> configLock(m_configMutex);
        m_config = sanitized;
    }
    m_configGeneration.fetch_add(1);

    if (previous.maxSamplesInMemory != sanitized.maxSamplesInMemory) {
        std::lock_guard<std::mutex> snapshotLock(m_snapshotMutex);
        m_snapshots.clear();
        m_snapshots.reserve(sanitized.maxSamplesInMemory);
        m_snapshotIndex = 0;
        m_snapshotCapacity = sanitized.maxSamplesInMemory;
    }

    m_samplingWaitCv.notify_all();
    sampleGuard.RunNow();

    // Always issue the stop after publishing a disabled configuration. This
    // closes the race where StartSampling read the old enabled value just
    // before this update was published.
    if (!sanitized.enabled) StopSampling();

    Logger::Info("Telemetry configuration updated");
}

TelemetryConfig TelemetryManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

std::chrono::steady_clock::time_point TelemetryManager::GetStartTime() const {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    return m_startTime;
}

std::vector<std::string> TelemetryManager::GetLastErrors() const {
    Logger::Trace("[TelemetryManager::GetLastErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[TelemetryManager::GetLastErrors] Returning %zu stored errors", m_errors.size());
    Logger::Trace("[TelemetryManager::GetLastErrors] Exit - returning %zu errors", m_errors.size());
    return m_errors;
}

void TelemetryManager::ClearErrors() {
    Logger::Trace("[TelemetryManager::ClearErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[TelemetryManager::ClearErrors] Clearing %zu stored errors", m_errors.size());
    m_errors.clear();
    Logger::Info("[TelemetryManager::ClearErrors] Error history cleared");
    Logger::Trace("[TelemetryManager::ClearErrors] Exit");
}

void TelemetryManager::ReportError(const std::string& error) {
    Logger::Trace("[TelemetryManager::ReportError] Entry - error='%s'", error.c_str());
    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << error;

    m_errors.push_back(oss.str());
    Logger::Debug("[TelemetryManager::ReportError] Error recorded, total errors in history: %zu", m_errors.size());

    // Limit error history
    if (m_errors.size() > MAX_ERRORS) {
        Logger::Debug("[TelemetryManager::ReportError] Error history exceeded MAX_ERRORS (%zu), removing oldest entry", (size_t)MAX_ERRORS);
        m_errors.erase(m_errors.begin());
    }

    Logger::Error("Telemetry: %s", error.c_str());
    Logger::Trace("[TelemetryManager::ReportError] Exit");
}

} // namespace Telemetry
