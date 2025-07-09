// RS2V Server/src/main.cpp

#include <iostream>
#include <memory>
#include <csignal>
#include <cstdlib>

// Core systems
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

// Global server instance for signal handling
static std::unique_ptr<GameServer> g_server = nullptr;

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
        auto config = std::make_shared<ServerConfig>();
        if (!config->LoadFromFile(configPath)) {
            std::cerr << "Warning: Could not load config file, using defaults" << std::endl;
        }

        std::string logFile = config->GetString("Server.LogFile", "rs2v_server.log");
        std::string logLevel = config->GetString("Server.LogLevel", "INFO");

        Logger::Initialize(logFile);
        
        if (logLevel == "TRACE") Logger::SetLevel(LogLevel::Trace);
        else if (logLevel == "DEBUG") Logger::SetLevel(LogLevel::Debug);
        else if (logLevel == "INFO") Logger::SetLevel(LogLevel::Info);
        else if (logLevel == "WARN") Logger::SetLevel(LogLevel::Warn);
        else if (logLevel == "ERROR") Logger::SetLevel(LogLevel::Error);
        else Logger::SetLevel(LogLevel::Info);

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
    std::cout << "  -c, --config <file>    Specify configuration file (default: config.json)\n";
    std::cout << "  -p, --port <port>      Override server port (default: 7777)\n";
    std::cout << "  -h, --help             Show this help message\n";
    std::cout << "  -v, --version          Show version information\n";
    std::cout << std::endl;
}

// Parse command line arguments
struct CommandLineArgs {
    std::string configFile = "config.json";
    uint16_t port = 7777;
    bool showHelp = false;
    bool showVersion = false;
};

CommandLineArgs ParseArguments(int argc, char* argv[]) {
    CommandLineArgs args;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            args.showHelp = true;
        }
        else if (arg == "-v" || arg == "--version") {
            args.showVersion = true;
        }
        else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            args.configFile = argv[++i];
        }
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else {
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
        return 0;
    }
    
    if (args.showVersion) {
        std::cout << "RS2V Custom Server v1.0.0\n";
        std::cout << "Built with Protocol Version " << ProtocolVersion::ToString() << std::endl;
        return 0;
    }
    
    // Initialize logging first
    if (!InitializeLogging(args.configFile)) {
        return EXIT_FAILURE;
    }
    
    try {
        Logger::Info("========================================");
        Logger::Info("RS2V Custom Server v1.0.0 Starting");
        Logger::Info("Protocol Version: %s", ProtocolVersion::ToString().c_str());
        Logger::Info("Configuration: %s", args.configFile.c_str());
        Logger::Info("========================================");
        
        // Setup signal handlers for graceful shutdown
        SetupSignalHandlers();
        
        // Load configuration
        auto serverConfig = std::make_shared<ServerConfig>();
        if (!serverConfig->LoadFromFile(args.configFile)) {
            Logger::Error("Failed to load server configuration from %s", args.configFile.c_str());
            return EXIT_FAILURE;
        }
        
        // Override port if specified via command line
        if (args.port != 7777) {
            serverConfig->SetInt("Server.Port", args.port);
        }
        
        // Load security configuration
        auto securityConfig = std::make_shared<SecurityConfig>();
        std::string securityConfigFile = serverConfig->GetString("Server.SecurityConfigFile", "security.json");
        if (!securityConfig->LoadFromFile(securityConfigFile)) {
            Logger::Warn("Failed to load security configuration, using defaults");
        }
        
        // Initialize network platform (Winsock on Windows)
        if (!SocketFactory::Initialize()) {
            Logger::Error("Failed to initialize networking platform");
            return EXIT_FAILURE;
        }
        
        // Create and initialize the game server
        g_server = std::make_unique<GameServer>(serverConfig, securityConfig);
        
        if (!g_server->Initialize()) {
            Logger::Error("Failed to initialize game server");
            SocketFactory::Shutdown();
            return EXIT_FAILURE;
        }
        
        Logger::Info("Server initialized successfully");
        Logger::Info("Server Name: %s", serverConfig->GetString("Server.Name", "RS2V Custom Server").c_str());
        Logger::Info("Max Players: %d", serverConfig->GetInt("Server.MaxPlayers", 64));
        Logger::Info("Listening on port: %d", serverConfig->GetInt("Server.Port", 7777));
        
        // Create and configure the game clock for fixed timestep updates
        GameClock gameClock;
        gameClock.SetTickRate(serverConfig->GetInt("Server.TickRate", 60));
        
        // Register server update callback
        gameClock.RegisterTickCallback([&](GameClock::Duration delta) {
            g_server->Update(delta);
        });
        
        Logger::Info("Starting main server loop at %d ticks per second", 
                     serverConfig->GetInt("Server.TickRate", 60));
        
        // Run the main server loop - this blocks until shutdown
        gameClock.RunLoop();
        
        Logger::Info("Main loop ended, shutting down server...");
        
    } catch (const std::exception& e) {
        Logger::Fatal("Unhandled exception in main: %s", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        Logger::Fatal("Unknown exception in main");
        return EXIT_FAILURE;
    }
    
    // Cleanup and shutdown
    if (g_server) {
        g_server->Shutdown();
        g_server.reset();
    }
    
    SocketFactory::Shutdown();
    Logger::Info("RS2V Server shutdown complete");
    Logger::Shutdown();
    
    return EXIT_SUCCESS;
}