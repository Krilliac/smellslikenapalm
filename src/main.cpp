// src/main.cpp

#include <iostream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <vector>

// Core systems
#include "Config/ConfigManager.h"
#include "Config/ServerConfig.h"
#include "Config/SecurityConfig.h"
#include "Config/StartupOptions.h"
#include "Utils/Logger.h"
#include "Utils/CrashHandler.h"
#include "Utils/FileUtils.h"
#include "Time/GameClock.h"

// Game systems
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"

// Network systems
#include "Network/SocketFactory.h"

// Telemetry system
#include "../telemetry/TelemetryManager.h"
#include "../telemetry/MetricsReporter.h"

// Global server and subsystems for signal handling
static std::unique_ptr<GameServer>         g_server;
static GameClock*                          g_gameClock = nullptr;
static std::atomic<bool>                   g_shutdownRequested{false};

// Signal handler for graceful shutdown
void SignalHandler(int signal)
{
    Logger::Info("[main::SignalHandler] Received signal %d, initiating graceful shutdown...", signal);
    Logger::Debug("[main::SignalHandler] Setting g_shutdownRequested=true");
    g_shutdownRequested = true;
    if (g_gameClock) {
        Logger::Debug("[main::SignalHandler] Stopping GameClock via g_gameClock->Stop()");
        g_gameClock->Stop();
    } else {
        Logger::Warn("[main::SignalHandler] g_gameClock is null, cannot stop game clock from signal handler");
    }

    // Graceful telemetry shutdown
    Logger::Debug("[main::SignalHandler] Initiating telemetry shutdown from signal handler");
    Telemetry::TelemetryManager::Instance().Shutdown();

    Logger::Info("[main::SignalHandler] Signal handler completed for signal %d", signal);
}

// Setup signal handlers
void SetupSignalHandlers()
{
    Logger::Trace("[main::SetupSignalHandlers] Entering SetupSignalHandlers()");
    Logger::Debug("[main::SetupSignalHandlers] Registering SIGINT handler");
    std::signal(SIGINT,  SignalHandler);
    Logger::Debug("[main::SetupSignalHandlers] Registering SIGTERM handler");
    std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
    Logger::Debug("[main::SetupSignalHandlers] Registering SIGHUP handler (POSIX)");
    std::signal(SIGHUP,  SignalHandler);
    Logger::Debug("[main::SetupSignalHandlers] Registering SIGQUIT handler (POSIX)");
    std::signal(SIGQUIT, SignalHandler);
#else
    Logger::Debug("[main::SetupSignalHandlers] Skipping POSIX-only signals (SIGHUP, SIGQUIT) on Windows");
#endif
    Logger::Info("[main::SetupSignalHandlers] All signal handlers registered successfully");
}

// Initialize logging from a config file path
bool InitializeLogging(const std::string& configPath)
{
    try {
        auto cfgMgr = std::make_shared<ConfigManager>();
        // Don't fail hard on init — we may not have a config dir yet
        cfgMgr->Initialize(configPath);

        ServerConfig cfg(cfgMgr);
        std::string logfile  = cfg.GetLogDirectory() + "/" + cfg.GetLogFileName();
        std::string loglevel = cfg.GetLogLevel();

        Logger::Initialize(logfile);
        if      (loglevel == "TRACE" || loglevel == "trace") Logger::SetLevel(LogLevel::Trace);
        else if (loglevel == "DEBUG" || loglevel == "debug") Logger::SetLevel(LogLevel::Debug);
        else if (loglevel == "INFO"  || loglevel == "info")  Logger::SetLevel(LogLevel::Info);
        else if (loglevel == "WARN"  || loglevel == "warn")  Logger::SetLevel(LogLevel::Warn);
        else if (loglevel == "ERROR" || loglevel == "error") Logger::SetLevel(LogLevel::Error);
        else                                                  Logger::SetLevel(LogLevel::Info);

        Logger::Info("[main::InitializeLogging] RS2V Server logging initialized successfully");
        Logger::Info("[main::InitializeLogging] Log file: '%s', Log level: '%s'", logfile.c_str(), loglevel.c_str());
        Logger::Debug("[main::InitializeLogging] Config path used for logging init: '%s'", configPath.c_str());
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to initialize logging: " << e.what() << "\n";
        // Fall back to console logging
        Logger::Initialize("");
        Logger::SetLevel(LogLevel::Info);
        Logger::Warn("[main::InitializeLogging] Fell back to console-only logging due to exception: %s", e.what());
        return true;
    }
}

// Usage
void PrintUsage(const char* prog)
{
    std::cout << "RS2V Custom Server v1.0.0\n\n"
                 "Usage: " << prog << " [options]\n\n"
                 "Options:\n"
                 "  -c, --config <file>    Config file (default: config/server.ini)\n"
                 "  -p, --port <port>      Override server port\n"
                 "      --eac-port <port>  Override EAC emulator port\n"
                 "  -m, --map <name>       Override initial map without editing config\n"
                 "  -h, --help             Show help\n"
                 "  -v, --version          Show version\n\n";
}

int main(int argc, char* argv[])
{
    // Install crash diagnostics FIRST — before any other init — so even a crash
    // during early startup prints a banner + symbolized stack trace instead of
    // dying silently with a bare exit code.
    rs2v::InstallCrashHandler();

    // Pre-logging initialization: parse args first since logging depends on config
    std::vector<std::string> rawArguments;
    rawArguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
    for (int index = 1; index < argc; ++index) {
        rawArguments.emplace_back(argv[index] ? argv[index] : "");
    }
    const StartupOptions args = ParseStartupOptions(rawArguments);
    if (!args.valid) {
        std::cerr << "Invalid command line: " << args.error << "\n\n";
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (args.help) {
        PrintUsage(argv[0]);
        return 0;
    }
    if (args.version) {
        std::cout << "RS2V Custom Server v1.0.0\n";
        return 0;
    }

    if (!InitializeLogging(args.configFile))
        return EXIT_FAILURE;

    Logger::Info("================================================================");
    Logger::Info("RS2V Custom Server v1.0.0 Starting");
    Logger::Info("Configuration file: %s", args.configFile.c_str());
    Logger::Info("Command-line port override: %s", args.port ? std::to_string(args.port).c_str() : "none");
    Logger::Info("Command-line EAC port override: %s", args.eacPort ? std::to_string(args.eacPort).c_str() : "none");
    Logger::Info("Build date: %s %s", __DATE__, __TIME__);
    Logger::Info("================================================================");

    Logger::Trace("[main] Setting up signal handlers...");
    SetupSignalHandlers();
    Logger::Debug("[main] Signal handlers configured successfully");

    // Initialize network platform (WSAStartup on Windows, no-op on POSIX)
    Logger::Info("[main] Initializing networking platform via SocketFactory::Initialize()...");
    if (!SocketFactory::Initialize()) {
        Logger::Error("[main] Networking platform initialization FAILED - cannot continue");
        // Logger owns background state and must be stopped while its static
        // lifetime is still valid, even on an early startup failure.
        Logger::Shutdown();
        return EXIT_FAILURE;
    }
    Logger::Info("[main] Networking platform initialized successfully");

    // Start GameServer — it internally loads configs and creates subsystems
    Logger::Info("[main] Creating GameServer instance...");
    g_server = std::make_unique<GameServer>();
    Logger::Debug("[main] GameServer instance created at %p, calling Initialize(config, port, map, eacPort)...", (void*)g_server.get());
    if (!g_server->Initialize(
            args.configFile, args.port, args.mapName, args.eacPort)) {
        Logger::Error("[main] GameServer::Initialize() FAILED - shutting down networking and exiting");
        // Do not leave the partially initialized global GameServer for static
        // destruction after Logger has begun tearing down. Its destructor logs
        // and shuts down singleton helpers, so ordering this explicitly avoids
        // the observed Windows startup-failure exit hang.
        g_server.reset();
        SocketFactory::Shutdown();
        Logger::Shutdown();
        return EXIT_FAILURE;
    }
    Logger::Info("[main] GameServer initialized successfully");

    auto serverCfg = g_server->GetServerConfig();
    Logger::Debug("[main] Retrieved ServerConfig pointer: %p", (void*)serverCfg.get());

    // Initialize telemetry system
    Logger::Info("[main] Initializing telemetry system...");
    {
        Telemetry::TelemetryConfig telemetryCfg;
        auto cfgMgr = g_server->GetConfigManager();
        telemetryCfg.enabled = cfgMgr ? cfgMgr->GetBool("Telemetry.enabled", true) : true;
        telemetryCfg.enableFileReporter = cfgMgr ? cfgMgr->GetBool("Telemetry.file_reporter", true) : true;
        telemetryCfg.enablePrometheusReporter = cfgMgr ? cfgMgr->GetBool("Telemetry.prometheus_reporter", false) : false;
        telemetryCfg.prometheusPort = cfgMgr ? cfgMgr->GetInt("Telemetry.prometheus_port", 9090) : 9090;
        telemetryCfg.metricsDirectory = cfgMgr ? cfgMgr->GetString("Telemetry.metrics_directory", "telemetry/output") : "telemetry/output";
        int samplingMs = cfgMgr ? cfgMgr->GetInt("Telemetry.sampling_interval_ms", 1000) : 1000;
        telemetryCfg.samplingInterval = std::chrono::milliseconds(samplingMs);

        auto& telemetry = Telemetry::TelemetryManager::Instance();
        if (telemetry.Initialize(telemetryCfg)) {
            // Add file reporter
            if (telemetryCfg.enableFileReporter) {
                if (telemetry.AddReporter(
                        Telemetry::ReporterFactory::CreateFileReporter("rs2v_metrics"))) {
                    Logger::Info("[main] File metrics reporter enabled in '%s'", telemetryCfg.metricsDirectory.c_str());
                } else {
                    Logger::Warn("[main] File metrics reporter failed to initialize");
                }
            }

            // Add CSV reporter
            {
                if (telemetry.AddReporter(
                        Telemetry::ReporterFactory::CreateCSVReporter("rs2v_metrics.csv"))) {
                    Logger::Info("[main] CSV metrics reporter enabled");
                } else {
                    Logger::Warn("[main] CSV metrics reporter failed to initialize");
                }
            }

            // Add in-memory reporter
            {
                if (telemetry.AddReporter(
                        Telemetry::ReporterFactory::CreateMemoryReporter(3600))) {
                    Logger::Info("[main] In-memory metrics reporter enabled");
                } else {
                    Logger::Warn("[main] In-memory metrics reporter failed to initialize");
                }
            }

            // Add Prometheus reporter if configured
            if (telemetryCfg.enablePrometheusReporter) {
                if (telemetry.AddReporter(
                        Telemetry::ReporterFactory::CreatePrometheusReporter(
                            telemetryCfg.prometheusPort, "rs2v_server"))) {
                    Logger::Info("[main] Prometheus metrics reporter enabled on port %d", telemetryCfg.prometheusPort);
                } else {
                    Logger::Warn("[main] Prometheus metrics reporter failed to initialize on port %d", telemetryCfg.prometheusPort);
                }
            }

            // Add alert reporter with default thresholds
            {
                Telemetry::AlertReporterConfig alertCfg;
                Telemetry::AlertReporterConfig::AlertRule cpuRule;
                cpuRule.name = "high_cpu";
                cpuRule.metricPath = "cpuUsagePercent";
                cpuRule.op = Telemetry::AlertReporterConfig::AlertRule::GREATER_THAN;
                cpuRule.threshold = 90.0;
                cpuRule.cooldownPeriod = std::chrono::seconds(300);
                alertCfg.rules.push_back(cpuRule);

                Telemetry::AlertReporterConfig::AlertRule connRule;
                connRule.name = "max_connections";
                connRule.metricPath = "activeConnections";
                connRule.op = Telemetry::AlertReporterConfig::AlertRule::GREATER_THAN;
                connRule.threshold = 200.0;
                connRule.cooldownPeriod = std::chrono::seconds(60);
                alertCfg.rules.push_back(connRule);

                auto alertReporter = std::make_unique<Telemetry::AlertMetricsReporter>(alertCfg);
                if (telemetry.AddReporter(std::move(alertReporter))) {
                    Logger::Info("[main] Alert metrics reporter enabled with %zu rules", alertCfg.rules.size());
                } else {
                    Logger::Warn("[main] Alert metrics reporter failed to initialize");
                }
            }

            telemetry.StartSampling();
            Logger::Info("[main] Telemetry system initialized and sampling started (interval: %dms)", samplingMs);
        } else {
            Logger::Warn("[main] Telemetry system initialization failed, continuing without telemetry");
        }
    }

    Logger::Info("[main] Server initialization complete");
    int serverPort = serverCfg ? serverCfg->GetPort() : 7777;
    Logger::Info("[main] Listening on game port: %d", serverPort);

    // GameClock for fixed timestep updates
    Logger::Debug("[main] Creating GameClock for fixed-timestep game loop...");
    GameClock gameClock;
    g_gameClock = &gameClock;
    // GameServer validates and normalizes the configured value during
    // initialization; use that same rate for the wall-clock scheduler.
    int tickRate = g_server ? g_server->GetTickRate() : 60;
    Logger::Info("[main] Setting tick rate to %d ticks/sec", tickRate);
    gameClock.SetTickRate(tickRate);

    // Let the `tickrate` command reach the GameClock without GameServer having to
    // depend on it. CommandManager drains console/remote work on this same game
    // thread before invoking the hook.
    g_server->SetTickRateHook([&gameClock](int r) { gameClock.SetTickRate(static_cast<uint32_t>(r)); });

    Logger::Trace("[main] Registering tick callback for main game loop...");
    gameClock.RegisterTickCallback([&](GameClock::Duration delta) {
        if (g_shutdownRequested || (g_server && g_server->IsShutdownRequested())) {
            Logger::Debug("[main::TickCallback] Shutdown requested, stopping game clock");
            gameClock.Stop();
            return;
        }

        // Run one tick of the game server. Guarded so a single bad tick (a
        // recoverable exception from hostile input or a subsystem) is logged via
        // the crash handler and the loop continues, instead of escaping the
        // GameClock callback into std::terminate and killing the whole server.
        const float elapsedSeconds = std::chrono::duration<float>(delta).count();
        rs2v::Guard("game tick", [elapsedSeconds] { g_server->Run(elapsedSeconds); });

    });

    Logger::Info("[main] ========================================");
    Logger::Info("[main] Entering main game loop at %d ticks/sec", tickRate);
    Logger::Info("[main] ========================================");
    gameClock.RunLoop();

    Logger::Info("[main] Main game loop exited, beginning shutdown sequence...");

    // Shutdown telemetry system first to capture final metrics
    Logger::Debug("[main] Shutting down telemetry system...");
    {
        auto& telemetry = Telemetry::TelemetryManager::Instance();
        if (telemetry.IsRunning()) {
            telemetry.ForceSample(); // Capture final snapshot
            telemetry.Shutdown();
            Logger::Info("[main] Telemetry system shut down (total samples: %llu)",
                        (unsigned long long)telemetry.GetTotalSamplesTaken());
        } else {
            Logger::Debug("[main] Telemetry was not running");
        }
    }

    // Shutdown sequence
    Logger::Debug("[main] Shutting down GameServer...");
    if (g_server) {
        g_server->Shutdown();
        g_server.reset();
        Logger::Info("[main] GameServer shut down successfully");
    } else {
        Logger::Debug("[main] No GameServer to shut down");
    }

    Logger::Debug("[main] Shutting down networking platform...");
    SocketFactory::Shutdown();
    Logger::Info("[main] Networking platform shut down successfully");

    g_gameClock = nullptr;
    Logger::Info("[main] ========================================");
    Logger::Info("[main] RS2V Server shutdown complete");
    Logger::Info("[main] ========================================");
    Logger::Shutdown();
    return EXIT_SUCCESS;
}
