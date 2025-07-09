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
#include "Time/TickManager.h"

// Game systems
#include "Game/GameServer.h"
#include "Game/GameManager.h"
#include "Game/SessionManager.h"
#include "Game/PlayerManager.h"

// Network systems
#include "Network/NetworkManager.h"
#include "Network/ConnectionManager.h"
#include "Protocol/ProtocolHandler.h"

// Security systems
#include "Security/SecurityManager.h"
#include "Security/Authentication.h"
#include "Security/BanManager.h"
#include "Admin/AdminManager.h"

// Scripting systems
#include "Scripting/ScriptManager.h"

// Global server instance for signal handling
static std::unique_ptr<GameServer> g_server = nullptr;
static std::unique_ptr<ScriptManager> g_scriptMgr = nullptr;

// Signal handler for graceful shutdown
void SignalHandler(int signal) {
    Logger::Info("Received signal %d, initiating graceful shutdown...", signal);
    if (g_server) {
        g_server->RequestShutdown();
    }
}

// Setup signal handlers for graceful shutdown
void SetupSignalHandlers() {
    std::signal(SIGINT, SignalHandler);   // Ctrl+C
    std::signal(SIGTERM, SignalHandler);  // Termination request
#ifndef _WIN32
    std::signal(SIGHUP, SignalHandler);   // Terminal hangup (Unix)
    std::signal(SIGQUIT, SignalHandler);  // Quit signal (Unix)
#endif
}

// Initialize logging system
bool InitializeLogging(const std::string& configPath) {
    try {
        auto cfgMgr = std::make_shared<ConfigManager>();
        if (!cfgMgr->Initialize()) {
            std::cerr << "Warning: Could not initialize ConfigManager, using defaults" << std::endl;
        }
        ServerConfig config(cfgMgr);
        if (!cfgMgr->LoadConfiguration(configPath)) {
            std::cerr << "Warning: Could not load config file, using defaults" << std::endl;
        }

        std::string logFile = config.GetLogDirectory() + "/" + config.GetLogFileName();
        std::string logLevel = config.GetLogLevel();

        Logger::Initialize(logFile);
        if      (logLevel == "TRACE") Logger::SetLevel(LogLevel::Trace);
        else if (logLevel == "DEBUG") Logger::SetLevel(LogLevel::Debug);
        else if (logLevel == "INFO")  Logger::SetLevel(LogLevel::Info);
        else if (logLevel == "WARN")  Logger::SetLevel(LogLevel::Warn);
        else if (logLevel == "ERROR") Logger::SetLevel(LogLevel::Error);
        else                           Logger::SetLevel(LogLevel::Info);

        Logger::Info("RS2V Server starting up...");
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize logging: " << e.what() << std::endl;
        return false;
    }
}

// Print usage information
void PrintUsage(const char* programName) {
    std::cout << "RS2V Custom Server v1.0.0\n\n";
    std::cout << "Usage: " << programName << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c, --config <file>    Specify configuration file (default: config/server.ini)\n";
    std::cout << "  -p, --port <port>      Override server port (default: 7777)\n";
    std::cout << "  -h, --help             Show this help message\n";
    std::cout << "  -v, --version          Show version information\n\n";
}

// Parse command line arguments
struct CommandLineArgs {
    std::string configFile = "config/server.ini";
    uint16_t    port       = 7777;
    bool        showHelp   = false;
    bool        showVersion= false;
};

CommandLineArgs ParseArguments(int argc, char* argv[]) {
    CommandLineArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            args.showHelp = true;
        } else if (arg == "-v" || arg == "--version") {
            args.showVersion = true;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            args.configFile = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            args.showHelp = true;
        }
    }
    return args;
}

// Main server initialization and execution
int main(int argc, char* argv[]) {
    // Parse command line arguments
    CommandLineArgs args = ParseArguments(argc, argv);
    if (args.showHelp) {
        PrintUsage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (args.showVersion) {
        std::cout << "RS2V Custom Server v1.0.0\n";
        std::cout << "Built with Protocol Version " << ProtocolVersion::ToString() << std::endl;
        return EXIT_SUCCESS;
    }

    // Initialize logging
    if (!InitializeLogging(args.configFile)) {
        return EXIT_FAILURE;
    }

    try {
        Logger::Info("========================================");
        Logger::Info("RS2V Custom Server v1.0.0 Starting");
        Logger::Info("Protocol Version: %s", ProtocolVersion::ToString().c_str());
        Logger::Info("Configuration: %s", args.configFile.c_str());
        Logger::Info("========================================");

        // Setup graceful shutdown handlers
        SetupSignalHandlers();

        // Load unified configuration
        auto cfgMgr = std::make_shared<ConfigManager>();
        if (!cfgMgr->Initialize() || !cfgMgr->LoadConfiguration(args.configFile)) {
            Logger::Error("Failed to load configuration from %s", args.configFile.c_str());
            return EXIT_FAILURE;
        }
        ServerConfig serverConfig(cfgMgr);

        // Override port if specified via command line
        if (args.port != serverConfig.GetPort()) {
            cfgMgr->SetInt("Network.port", args.port);
        }

        // Load security configuration
        auto securityConfig = std::make_shared<SecurityConfig>(serverConfig);
        std::string secFile = serverConfig.GetString("Security.custom_auth_tokens_file", "config/auth_tokens.txt");
        securityConfig->Initialize(cfgMgr, secFile);

        // Initialize network platform
        if (!SocketFactory::Initialize()) {
            Logger::Error("Failed to initialize networking platform");
            return EXIT_FAILURE;
        }

        // Initialize and start game server
        g_server = std::make_unique<GameServer>(serverConfig, *securityConfig);
        if (!g_server->Initialize()) {
            Logger::Error("Failed to initialize game server");
            SocketFactory::Shutdown();
            return EXIT_FAILURE;
        }

        // Initialize C# scripting
        g_scriptMgr = std::make_unique<ScriptManager>(serverConfig);
        if (!g_scriptMgr->Initialize()) {
            Logger::Warn("C# scripting failed to initialize; continuing without scripts");
        }

        Logger::Info("Server initialized successfully");
        Logger::Info("Server Name: %s", serverConfig.GetServerName().c_str());
        Logger::Info("Max Players: %d", serverConfig.GetMaxPlayers());
        Logger::Info("Listening on port: %d", serverConfig.GetPort());

        // Configure game clock for fixed-timestep updates
        GameClock gameClock;
        gameClock.SetTickRate(serverConfig.GetTickRate());
        gameClock.RegisterTickCallback([&](GameClock::Duration delta) {
            g_server->Update(delta);
            // Dispatch per-tick script callback
            if (g_scriptMgr) {
                g_scriptMgr->ExecuteScriptMethod("__Global__", "OnTick", { std::to_string(delta.count()) });
            }
        });

        Logger::Info("Entering main loop at %d ticks/sec", serverConfig.GetTickRate());
        gameClock.RunLoop();

        Logger::Info("Main loop exited, shutting down...");

    } catch (const std::exception& e) {
        Logger::Fatal("Unhandled exception: %s", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        Logger::Fatal("Unknown exception occurred");
        return EXIT_FAILURE;
    }

    // Shutdown scripting
    if (g_scriptMgr) {
        g_scriptMgr->Shutdown();
        g_scriptMgr.reset();
    }

    // Shutdown server
    if (g_server) {
        g_server->Shutdown();
        g_server.reset();
    }

    SocketFactory::Shutdown();
    Logger::Info("RS2V Server shutdown complete");
    Logger::Shutdown();

    return EXIT_SUCCESS;
}