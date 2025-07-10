// src/main.cpp

#include <iostream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <chrono>

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

// Scripting systems
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptHost.h"

// Global server and subsystems for signal handling
static std::unique_ptr<GameServer>         g_server;
static std::unique_ptr<EACServerEmulator>  g_eacServer;
static std::unique_ptr<ScriptManager>      g_scriptMgr;

// Signal handler for graceful shutdown
void SignalHandler(int signal)
{
    Logger::Info("Received signal %d, initiating graceful shutdown...", signal);
    if (g_server)
        g_server->RequestShutdown();
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

// Initialize logging
bool InitializeLogging(const std::string& configPath)
{
    try {
        auto cfgMgr = std::make_shared<ConfigManager>();
        if (!cfgMgr->Initialize())
            std::cerr << "Warning: ConfigManager init failed; using defaults\n";

        if (!cfgMgr->LoadConfiguration(configPath))
            std::cerr << "Warning: Could not load config file; using defaults\n";

        ServerConfig cfg(cfgMgr);
        std::string logfile  = cfg.GetLogDirectory() + "/" + cfg.GetLogFileName();
        std::string loglevel = cfg.GetLogLevel();

        Logger::Initialize(logfile);
        if      (loglevel == "TRACE") Logger::SetLevel(LogLevel::Trace);
        else if (loglevel == "DEBUG") Logger::SetLevel(LogLevel::Debug);
        else if (loglevel == "INFO")  Logger::SetLevel(LogLevel::Info);
        else if (loglevel == "WARN")  Logger::SetLevel(LogLevel::Warn);
        else if (loglevel == "ERROR") Logger::SetLevel(LogLevel::Error);
        else                           Logger::SetLevel(LogLevel::Info);

        Logger::Info("RS2V Server starting up...");
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to initialize logging: " << e.what() << "\n";
        return false;
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

// Command‐line parsing
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

    // Load configurations
    auto cfgMgr = std::make_shared<ConfigManager>();
    if (!cfgMgr->Initialize() || !cfgMgr->LoadConfiguration(args.configFile)) {
        Logger::Error("Failed to load configuration");
        return EXIT_FAILURE;
    }
    ServerConfig serverCfg(cfgMgr);

    // Override port if specified
    if (args.port) {
        cfgMgr->SetInt("Network.port", args.port);
    }

    // Initialize network
    if (!SocketFactory::Initialize()) {
        Logger::Error("Networking platform init failed");
        return EXIT_FAILURE;
    }

    // Start GameServer
    g_server = std::make_unique<GameServer>(serverCfg);
    if (!g_server->Initialize()) {
        Logger::Error("GameServer initialization failed");
        SocketFactory::Shutdown();
        return EXIT_FAILURE;
    }

    // Start EAC emulator
    g_eacServer = std::make_unique<EACServerEmulator>();
    if (!g_eacServer->Initialize(serverCfg.GetEACListenPort())) {
        Logger::Error("EAC emulator failed to start");
    }

    // Initialize scripting
    g_scriptMgr = std::make_unique<ScriptManager>(serverCfg);
    if (!g_scriptMgr->Initialize()) {
        Logger::Warn("C# scripting init failed; continuing without scripts");
    } else {
        // Wire ScriptHost facade
        if (!ScriptHost::Initialize(g_scriptMgr.get(), cfgMgr, g_server.get(), g_eacServer.get())) {
            Logger::Error("ScriptHost initialization failed");
        }
    }

    Logger::Info("Server initialized successfully");
    Logger::Info("Listening on port: %d", serverCfg.GetPort());

    // GameClock for fixed timestep updates
    GameClock gameClock;
    gameClock.SetTickRate(serverCfg.GetTickRate());
    gameClock.RegisterTickCallback([&](GameClock::Duration delta) {
        // Update server logic
        g_server->Update(delta);

        // Process EAC and scripting events
        g_eacServer->ProcessRequests();
        ProcessScheduledCallbacks();
    });

    Logger::Info("Entering main loop at %d ticks/sec", serverCfg.GetTickRate());
    gameClock.RunLoop();

    Logger::Info("Main loop exited, shutting down...");

    // Shutdown sequence
    ScriptHost::Shutdown();
    if (g_scriptMgr) {
        g_scriptMgr->Shutdown();
        g_scriptMgr.reset();
    }

    if (g_eacServer) {
        g_eacServer->Shutdown();
        g_eacServer.reset();
    }

    if (g_server) {
        g_server->Shutdown();
        g_server.reset();
    }

    SocketFactory::Shutdown();
    Logger::Info("RS2V Server shutdown complete");
    Logger::Shutdown();
    return EXIT_SUCCESS;
}