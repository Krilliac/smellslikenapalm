// src/main.cpp

#include <iostream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <atomic>

// Core systems
#include "Config/ConfigManager.h"
#include "Config/ServerConfig.h"
#include "Config/SecurityConfig.h"
#include "Utils/Logger.h"
#include "Utils/FileUtils.h"
#include "Time/GameClock.h"

// Game systems
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"

// Network systems
#include "Network/SocketFactory.h"

// Security / EAC emulator
#include "Security/EACServerEmulator.h"

// Global server and subsystems for signal handling
static std::unique_ptr<GameServer>         g_server;
static std::unique_ptr<EACServerEmulator>  g_eacServer;
static GameClock*                          g_gameClock = nullptr;
static std::atomic<bool>                   g_shutdownRequested{false};

// Signal handler for graceful shutdown
void SignalHandler(int signal)
{
    Logger::Info("Received signal %d, initiating graceful shutdown...", signal);
    g_shutdownRequested = true;
    if (g_gameClock)
        g_gameClock->Stop();
}

// Setup signal handlers
void SetupSignalHandlers()
{
    std::signal(SIGINT,  SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
    std::signal(SIGHUP,  SignalHandler);
    std::signal(SIGQUIT, SignalHandler);
#endif
}

// Initialize logging from a config file path
bool InitializeLogging(const std::string& configPath)
{
    try {
        auto cfgMgr = std::make_shared<ConfigManager>();
        // Don't fail hard on init — we may not have a config dir yet
        cfgMgr->Initialize();
        cfgMgr->LoadConfiguration(configPath);

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

        Logger::Info("RS2V Server starting up...");
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to initialize logging: " << e.what() << "\n";
        // Fall back to console logging
        Logger::Initialize("");
        Logger::SetLevel(LogLevel::Info);
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
                 "  -h, --help             Show help\n"
                 "  -v, --version          Show version\n\n";
}

// Command-line parsing
struct CmdArgs {
    std::string configFile = "config/server.ini";
    uint16_t    port       = 0;
    bool        help       = false;
    bool        version    = false;
};

CmdArgs ParseArgs(int argc, char* argv[])
{
    CmdArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")     a.help = true;
        else if (arg == "-v" || arg == "--version") a.version = true;
        else if ((arg == "-c" || arg == "--config") && i+1<argc) a.configFile = argv[++i];
        else if ((arg == "-p" || arg == "--port")   && i+1<argc) a.port = (uint16_t)std::stoi(argv[++i]);
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            a.help = true;
        }
    }
    return a;
}

int main(int argc, char* argv[])
{
    auto args = ParseArgs(argc, argv);
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

    Logger::Info("========================================");
    Logger::Info("RS2V Custom Server v1.0.0 Starting");
    Logger::Info("Configuration: %s", args.configFile.c_str());
    Logger::Info("========================================");

    SetupSignalHandlers();

    // Initialize network platform (WSAStartup on Windows, no-op on POSIX)
    if (!SocketFactory::Initialize()) {
        Logger::Error("Networking platform init failed");
        return EXIT_FAILURE;
    }

    // Start GameServer — it internally loads configs and creates subsystems
    g_server = std::make_unique<GameServer>();
    if (!g_server->Initialize()) {
        Logger::Error("GameServer initialization failed");
        SocketFactory::Shutdown();
        return EXIT_FAILURE;
    }

    auto serverCfg = g_server->GetServerConfig();

    // Override port if specified on command line
    if (args.port && g_server->GetConfigManager()) {
        g_server->GetConfigManager()->SetInt("Network.port", args.port);
    }

    // Start EAC emulator on configured port (default 7957)
    uint16_t eacPort = (uint16_t)g_server->GetConfigManager()->GetInt("EAC.listen_port", 7957);
    g_eacServer = std::make_unique<EACServerEmulator>();
    if (!g_eacServer->Initialize(eacPort)) {
        Logger::Warn("EAC emulator failed to start on port %u; continuing without EAC", eacPort);
    }

    Logger::Info("Server initialized successfully");
    Logger::Info("Listening on port: %d", serverCfg ? serverCfg->GetPort() : 7777);

    // GameClock for fixed timestep updates
    GameClock gameClock;
    g_gameClock = &gameClock;
    int tickRate = serverCfg ? serverCfg->GetTickRate() : 60;
    gameClock.SetTickRate(tickRate);

    gameClock.RegisterTickCallback([&](GameClock::Duration /*delta*/) {
        if (g_shutdownRequested) {
            gameClock.Stop();
            return;
        }

        // Run one tick of the game server
        g_server->Run();

        // Process EAC requests (no-op if threaded mode active)
        if (g_eacServer) {
            g_eacServer->ProcessRequests();
        }
    });

    Logger::Info("Entering main loop at %d ticks/sec", tickRate);
    gameClock.RunLoop();

    Logger::Info("Main loop exited, shutting down...");

    // Shutdown sequence
    if (g_eacServer) {
        g_eacServer->Shutdown();
        g_eacServer.reset();
    }

    if (g_server) {
        g_server->Shutdown();
        g_server.reset();
    }

    SocketFactory::Shutdown();
    g_gameClock = nullptr;
    Logger::Info("RS2V Server shutdown complete");
    Logger::Shutdown();
    return EXIT_SUCCESS;
}
