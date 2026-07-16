// Focused telemetry lifecycle and bounded-storage safety coverage.

#include "TestFramework.h"

#include "TelemetryManager.h"
#include "MetricsReporter.h"
#include "NetworkMetricsPlatform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class ScopedTelemetryDirectory {
public:
    explicit ScopedTelemetryDirectory(const char* label) {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path = std::filesystem::temp_directory_path() /
               (std::string("rs2v_telemetry_") + label + "_" +
                std::to_string(nonce));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path);
    }

    ~ScopedTelemetryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

struct ReporterLifecycleState {
    std::atomic<int> initializeCalls{0};
    std::atomic<int> reportCalls{0};
    std::atomic<int> shutdownCalls{0};
    bool initializeResult = true;
    bool throwOnInitialize = false;
    bool throwOnReport = false;
    bool throwOnShutdown = false;
    std::function<void()> onInitialize;
    std::function<void()> onReport;
    std::function<void()> onShutdown;
};

class LifecycleProbeReporter final : public Telemetry::MetricsReporter {
public:
    explicit LifecycleProbeReporter(std::shared_ptr<ReporterLifecycleState> state)
        : m_state(std::move(state)) {}

    bool Initialize(const std::string&) override {
        m_state->initializeCalls.fetch_add(1);
        if (m_state->onInitialize) m_state->onInitialize();
        if (m_state->throwOnInitialize) {
            throw std::runtime_error("probe initialization failure");
        }
        return m_state->initializeResult;
    }

    void Shutdown() override {
        m_state->shutdownCalls.fetch_add(1);
        if (m_state->onShutdown) m_state->onShutdown();
        if (m_state->throwOnShutdown) {
            throw std::runtime_error("probe shutdown failure");
        }
    }
    void Report(const Telemetry::MetricsSnapshot&) override {
        m_state->reportCalls.fetch_add(1);
        if (m_state->onReport) m_state->onReport();
        if (m_state->throwOnReport) {
            throw std::runtime_error("probe report failure");
        }
    }
    std::string GetReporterType() const override { return "LifecycleProbe"; }

private:
    std::shared_ptr<ReporterLifecycleState> m_state;
};

class TelemetryManagerTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        auto& telemetry = Telemetry::TelemetryManager::Instance();
        telemetry.Shutdown();
        telemetry.RemoveAllReporters();
        telemetry.ClearErrors();
    }

    void TearDown() override {
        Telemetry::TelemetryManager::Instance().Shutdown();
    }

    static Telemetry::TelemetryConfig QuietConfig() {
        Telemetry::TelemetryConfig config;
        config.enabled = true;
        config.enableFileReporter = false;
        config.enablePrometheusReporter = false;
        config.enableSystemMetrics = false;
        config.enableApplicationMetrics = true;
        config.enablePerformanceMetrics = false;
        config.enableSecurityMetrics = false;
        return config;
    }
};

TEST_F(TelemetryManagerTest, InvalidBoundsAreClampedAndSamplingRemainsBounded) {
    auto config = QuietConfig();
    config.samplingInterval = std::chrono::milliseconds(0);
    config.maxSamplesInMemory = 0;

    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(config));
    EXPECT_EQ(telemetry.GetConfig().samplingInterval,
              std::chrono::milliseconds(1));
    EXPECT_EQ(telemetry.GetConfig().maxSamplesInMemory, 1u);

    telemetry.ForceSample();
    telemetry.ForceSample();
    EXPECT_EQ(telemetry.GetRecentSnapshots(8).size(), 1u);
}

TEST_F(TelemetryManagerTest, StopSamplingWakesAThreadWithLongInterval) {
    auto config = QuietConfig();
    config.samplingInterval = std::chrono::hours(1);

    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(config));
    telemetry.StartSampling();

    const auto started = std::chrono::steady_clock::now();
    telemetry.StopSampling();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_FALSE(telemetry.IsRunning());
    EXPECT_TRUE(elapsed < std::chrono::seconds(1));
}

TEST_F(TelemetryManagerTest, NullReporterIsIgnoredSafely) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    EXPECT_FALSE(telemetry.AddReporter(std::unique_ptr<Telemetry::MetricsReporter>{}));
    EXPECT_FALSE(telemetry.GetLastErrors().empty());
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));
    telemetry.ForceSample();
    EXPECT_EQ(telemetry.GetRecentSnapshots(1).size(), 1u);
}

TEST(FileMetricsReporterTest,
     RotationNeverPrunesCurrentOpenFileOrRetainsFutureDatedFile) {
    ScopedTelemetryDirectory directory("rotation_current");
    const std::filesystem::path futureFile =
        directory.path / "metrics_99991231_235959_999.json";
    {
        std::ofstream seed(futureFile);
        ASSERT_TRUE(seed.is_open());
        seed << "stale";
    }

    Telemetry::FileReporterConfig config;
    config.maxFiles = 1;
    config.enableCompression = false;
    config.enableRotation = true;
    config.flushImmediately = true;
    Telemetry::FileMetricsReporter reporter(config);
    ASSERT_TRUE(reporter.Initialize(directory.path.string()));

    // The current file's real timestamp sorts before the deliberately future-
    // dated seed.  Retention must nevertheless preserve the open file rather
    // than trusting lexical order alone.  This also covers Windows' native
    // backslash scan paths versus generated paths without platform branches.
    reporter.ForceRotation();
    const std::vector<std::string> files = reporter.GetGeneratedFiles();
    ASSERT_EQ(files.size(), static_cast<size_t>(1));
    const std::filesystem::path retained(files.front());
    EXPECT_NE(retained.filename(), futureFile.filename());
    EXPECT_TRUE(std::filesystem::exists(retained));
    EXPECT_FALSE(std::filesystem::exists(futureFile));
    EXPECT_EQ(files.front(),
              std::filesystem::absolute(retained)
                  .lexically_normal()
                  .make_preferred()
                  .string());

    const size_t sizeBeforeReport = reporter.GetCurrentFileSize();
    reporter.Report(Telemetry::MetricsSnapshot{});
    EXPECT_GT(reporter.GetCurrentFileSize(), sizeBeforeReport);
    EXPECT_TRUE(std::filesystem::exists(retained));
    reporter.Shutdown();
}

TEST_F(TelemetryManagerTest, ReporterExceptionsAreContainedAndRecorded) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    auto throwingState = std::make_shared<ReporterLifecycleState>();
    throwingState->throwOnReport = true;
    throwingState->throwOnShutdown = true;
    auto healthyState = std::make_shared<ReporterLifecycleState>();
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(throwingState)));
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(healthyState)));

    telemetry.ForceSample();
    EXPECT_EQ(throwingState->reportCalls.load(), 1);
    EXPECT_EQ(healthyState->reportCalls.load(), 1);

    telemetry.Shutdown();
    EXPECT_EQ(throwingState->shutdownCalls.load(), 1);
    EXPECT_EQ(healthyState->shutdownCalls.load(), 1);

    const auto errors = telemetry.GetLastErrors();
    EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error) {
        return error.find("probe report failure") != std::string::npos;
    }));
    EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error) {
        return error.find("probe shutdown failure") != std::string::npos;
    }));
}

TEST(NetworkInterfaceCounterAggregationTest, ExcludesLoopbackAndFilterInterfaces) {
    Telemetry::Detail::NetworkInterfaceCounterTotals totals;
    Telemetry::Detail::AccumulateNetworkInterfaceCounters(
        totals, {false, false, 100, 200});
    Telemetry::Detail::AccumulateNetworkInterfaceCounters(
        totals, {true, false, 1000, 2000});
    Telemetry::Detail::AccumulateNetworkInterfaceCounters(
        totals, {false, true, 4000, 8000});
    Telemetry::Detail::AccumulateNetworkInterfaceCounters(
        totals, {false, false, 50, 75});

    EXPECT_EQ(totals.bytesSent, 150u);
    EXPECT_EQ(totals.bytesReceived, 275u);
}

TEST(NetworkInterfaceCounterAggregationTest, SaturatesInsteadOfWrapping) {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    Telemetry::Detail::NetworkInterfaceCounterTotals totals{maximum - 2, maximum - 1};
    Telemetry::Detail::AccumulateNetworkInterfaceCounters(
        totals, {false, false, 8, 8});

    EXPECT_EQ(totals.bytesSent, maximum);
    EXPECT_EQ(totals.bytesReceived, maximum);
}

TEST_F(TelemetryManagerTest, ManagerOwnsImmediateReporterLifecycleExactlyOnce) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    auto state = std::make_shared<ReporterLifecycleState>();
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));
    EXPECT_EQ(state->initializeCalls.load(), 1);

    telemetry.ForceSample();
    EXPECT_EQ(state->reportCalls.load(), 1);

    telemetry.Shutdown();
    EXPECT_EQ(state->initializeCalls.load(), 1);
    EXPECT_EQ(state->shutdownCalls.load(), 1);
    telemetry.Shutdown();
    EXPECT_EQ(state->shutdownCalls.load(), 1);
}

TEST_F(TelemetryManagerTest, DeferredReporterInitializesOnceWhenManagerStarts) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    auto state = std::make_shared<ReporterLifecycleState>();

    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));
    EXPECT_EQ(state->initializeCalls.load(), 0);

    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));
    EXPECT_EQ(state->initializeCalls.load(), 1);
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));
    EXPECT_EQ(state->initializeCalls.load(), 1);

    telemetry.Shutdown();
    EXPECT_EQ(state->shutdownCalls.load(), 1);
}

TEST_F(TelemetryManagerTest, FailedImmediateReporterIsRejected) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    auto state = std::make_shared<ReporterLifecycleState>();
    state->initializeResult = false;
    EXPECT_FALSE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));

    telemetry.ForceSample();
    EXPECT_EQ(state->initializeCalls.load(), 1);
    EXPECT_EQ(state->reportCalls.load(), 0);
    telemetry.Shutdown();
    EXPECT_EQ(state->shutdownCalls.load(), 0);
}

TEST_F(TelemetryManagerTest, ThrowingDeferredReporterIsRemovedDuringInitialize) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    auto state = std::make_shared<ReporterLifecycleState>();
    state->throwOnInitialize = true;

    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));
    telemetry.ForceSample();

    EXPECT_EQ(state->initializeCalls.load(), 1);
    EXPECT_EQ(state->reportCalls.load(), 0);
    telemetry.Shutdown();
    EXPECT_EQ(state->shutdownCalls.load(), 0);
    EXPECT_FALSE(telemetry.GetLastErrors().empty());
}

TEST_F(TelemetryManagerTest, CapacityChangeResetsRingBeforeReindexing) {
    auto initial = QuietConfig();
    initial.maxSamplesInMemory = 3;

    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(initial));
    telemetry.ForceSample();
    telemetry.ForceSample();
    EXPECT_EQ(telemetry.GetRecentSnapshots(8).size(), 2u);

    auto updated = initial;
    updated.maxSamplesInMemory = 1;
    telemetry.UpdateConfig(updated);
    EXPECT_TRUE(telemetry.GetRecentSnapshots(8).empty());

    telemetry.ForceSample();
    telemetry.ForceSample();
    EXPECT_EQ(telemetry.GetRecentSnapshots(8).size(), 1u);
}

TEST_F(TelemetryManagerTest, ReportCallbackCanRemoveItselfWithoutDeadlock) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    auto state = std::make_shared<ReporterLifecycleState>();
    state->onReport = [&telemetry]() { telemetry.RemoveAllReporters(); };
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));

    telemetry.ForceSample();
    EXPECT_EQ(state->reportCalls.load(), 1);
    EXPECT_EQ(state->shutdownCalls.load(), 1);
    telemetry.ForceSample();
    EXPECT_EQ(state->reportCalls.load(), 1);
}

TEST_F(TelemetryManagerTest, ReportCallbackCanAddReporterWithoutJoiningCurrentSample) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    auto first = std::make_shared<ReporterLifecycleState>();
    auto added = std::make_shared<ReporterLifecycleState>();
    std::atomic<bool> attempted{false};
    std::atomic<bool> addResult{false};
    first->onReport = [&]() {
        if (!attempted.exchange(true)) {
            addResult.store(telemetry.AddReporter(
                std::make_unique<LifecycleProbeReporter>(added)));
        }
    };
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(first)));

    telemetry.ForceSample();
    EXPECT_TRUE(addResult.load());
    EXPECT_EQ(added->initializeCalls.load(), 1);
    EXPECT_EQ(added->reportCalls.load(), 0);

    telemetry.ForceSample();
    EXPECT_EQ(added->reportCalls.load(), 1);
    telemetry.Shutdown();
    EXPECT_EQ(first->shutdownCalls.load(), 1);
    EXPECT_EQ(added->shutdownCalls.load(), 1);
}

TEST_F(TelemetryManagerTest, ReportAndShutdownCallbacksCanReenterSafely) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    auto state = std::make_shared<ReporterLifecycleState>();
    auto rejected = std::make_shared<ReporterLifecycleState>();
    std::atomic<bool> nestedAddResult{true};
    std::atomic<bool> nestedInitializeResult{true};
    state->onReport = [&]() {
        telemetry.Shutdown();
        nestedInitializeResult.store(telemetry.Initialize(QuietConfig()));
    };
    state->onShutdown = [&]() {
        nestedAddResult.store(telemetry.AddReporter(
            std::make_unique<LifecycleProbeReporter>(rejected)));
        telemetry.RemoveAllReporters();
        telemetry.Shutdown();
    };
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));

    telemetry.ForceSample();
    EXPECT_EQ(state->reportCalls.load(), 1);
    EXPECT_EQ(state->shutdownCalls.load(), 1);
    EXPECT_FALSE(nestedAddResult.load());
    EXPECT_FALSE(nestedInitializeResult.load());
    EXPECT_EQ(rejected->initializeCalls.load(), 0);
    telemetry.ForceSample();
    EXPECT_EQ(state->reportCalls.load(), 1);
}

TEST_F(TelemetryManagerTest, ExternalShutdownDrainsAnInFlightSample) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    auto state = std::make_shared<ReporterLifecycleState>();
    state->onReport = [&]() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&]() { return release; });
    };
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));

    std::thread sampler([&]() { telemetry.ForceSample(); });
    bool observedEntry = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        observedEntry = cv.wait_for(lock, std::chrono::seconds(1),
                                    [&]() { return entered; });
    }
    EXPECT_TRUE(observedEntry);
    if (!observedEntry) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        cv.notify_all();
        sampler.join();
        return;
    }
    auto shutdown = std::async(std::launch::async, [&]() { telemetry.Shutdown(); });
    EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(20)),
              std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    sampler.join();
    EXPECT_EQ(shutdown.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(state->shutdownCalls.load(), 1);
    EXPECT_TRUE(telemetry.GetRecentSnapshots(1).empty());
}

TEST_F(TelemetryManagerTest, ShutdownRequestedDuringInitializeIsNotDropped) {
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    auto state = std::make_shared<ReporterLifecycleState>();
    state->onInitialize = [&telemetry]() { telemetry.Shutdown(); };
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(state)));

    ASSERT_TRUE(telemetry.Initialize(QuietConfig()));
    EXPECT_EQ(state->initializeCalls.load(), 1);
    EXPECT_EQ(state->shutdownCalls.load(), 1);

    auto deferred = std::make_shared<ReporterLifecycleState>();
    ASSERT_TRUE(telemetry.AddReporter(
        std::make_unique<LifecycleProbeReporter>(deferred)));
    EXPECT_EQ(deferred->initializeCalls.load(), 0);
}

TEST_F(TelemetryManagerTest, IntervalUpdateRetimesSleepingSampler) {
    auto config = QuietConfig();
    config.samplingInterval = std::chrono::hours(1);
    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(config));
    const uint64_t baseline = telemetry.GetTotalSamplesTaken();
    telemetry.StartSampling();
    ASSERT_TRUE(telemetry.IsRunning());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (telemetry.GetTotalSamplesTaken() <= baseline &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const uint64_t before = telemetry.GetTotalSamplesTaken();
    ASSERT_GT(before, baseline);
    config.samplingInterval = std::chrono::milliseconds(1);
    telemetry.UpdateConfig(config);
    ASSERT_EQ(telemetry.GetConfig().samplingInterval,
              std::chrono::milliseconds(1));

    // CI and local dogfood runs can be CPU-starved by the retail client and an
    // MSVC link.  The assertion is about interrupting a one-hour wait, not
    // sub-quarter-second scheduling latency, so retain a bounded but generous
    // deadline that cannot confuse scheduler pressure with a lifecycle bug.
    const auto retimeDeadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
    while (telemetry.GetTotalSamplesTaken() <= before &&
           std::chrono::steady_clock::now() < retimeDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(telemetry.IsRunning());
    EXPECT_GT(telemetry.GetTotalSamplesTaken(), before);
    telemetry.StopSampling();
}

TEST_F(TelemetryManagerTest, ConcurrentSamplingConfigAndThreadControlStayCoherent) {
    auto config = QuietConfig();
    config.samplingInterval = std::chrono::hours(1);
    config.maxSamplesInMemory = 4;

    auto& telemetry = Telemetry::TelemetryManager::Instance();
    ASSERT_TRUE(telemetry.Initialize(config));
    std::atomic<bool> invalidConfigObserved{false};

    std::thread samplerA([&]() {
        for (int i = 0; i < 12; ++i) telemetry.ForceSample();
    });
    std::thread samplerB([&]() {
        for (int i = 0; i < 12; ++i) telemetry.ForceSample();
    });
    std::thread updater([&]() {
        for (int i = 0; i < 12; ++i) {
            auto next = config;
            next.maxSamplesInMemory = static_cast<size_t>((i % 4) + 1);
            next.samplingInterval = std::chrono::milliseconds((i % 3) + 1);
            telemetry.UpdateConfig(next);
        }
    });
    std::thread controller([&]() {
        for (int i = 0; i < 8; ++i) {
            telemetry.StartSampling();
            const auto observed = telemetry.GetConfig();
            if (observed.maxSamplesInMemory == 0 ||
                observed.samplingInterval <= std::chrono::milliseconds::zero()) {
                invalidConfigObserved.store(true);
            }
            telemetry.StopSampling();
        }
    });

    samplerA.join();
    samplerB.join();
    updater.join();
    controller.join();
    telemetry.StopSampling();

    EXPECT_FALSE(invalidConfigObserved.load());
    const auto finalConfig = telemetry.GetConfig();
    EXPECT_LE(telemetry.GetRecentSnapshots(32).size(),
              finalConfig.maxSamplesInMemory);
}

} // namespace

RS2V_TEST_MAIN()
