// src/Game/SecureGameServer.cpp – Implementation for SecureGameServer

#include "Game/SecureGameServer.h"
#include "Utils/Logger.h"

SecureGameServer::SecureGameServer() {
    Logger::Info("SecureGameServer constructed");
}

SecureGameServer::~SecureGameServer() {
    Shutdown();
}

bool SecureGameServer::Initialize() {
    Logger::Info("Initializing SecureGameServer...");

    // First, initialize base server
    if (!GameServer::Initialize()) {
        return false;
    }

    // Initialize network blocker
    m_networkBlocker = std::make_unique<NetworkBlocker>();
    if (!m_networkBlocker->Initialize()) {
        Logger::Error("NetworkBlocker initialization failed");
        return false;
    }

    // Initialize telemetry blocker
    m_telemetryBlocker = std::make_unique<TelemetryBlocker>();
    if (!m_telemetryBlocker->Initialize()) {
        Logger::Error("TelemetryBlocker initialization failed");
        return false;
    }

    // Prevent EAC library from loading
    PreventEACInitialization();

    // Block known EAC/Epic endpoints
    BlockEACTraffic();

    // Disable all telemetry systems
    DisableTelemetry();

    // Initialize EAC server emulator to satisfy clients
    m_eacEmulator = std::make_unique<EACServerEmulator>();
    m_eacEmulator->SetSafeMode(true);
    m_eacEmulator->SetAlwaysAccept(true);
    if (!m_eacEmulator->Initialize(7957)) {
        Logger::Error("EACServerEmulator initialization failed");
        return false;
    }

    // Initialize enhanced anti-cheat over EAC protocol
    m_enhancedAntiCheat = std::make_unique<EnhancedEACAntiCheat>(
        m_serverConfig, m_gameConfig);
    if (!m_enhancedAntiCheat->Initialize(7957)) {
        Logger::Error("EnhancedEACAntiCheat initialization failed");
        return false;
    }

    Logger::Info("SecureGameServer initialized successfully");
    return true;
}

void SecureGameServer::Shutdown() {
    Logger::Info("Shutting down SecureGameServer...");
    if (m_eacEmulator) {
        m_eacEmulator->Shutdown();
        m_eacEmulator.reset();
    }
    if (m_enhancedAntiCheat) {
        m_enhancedAntiCheat.reset();
    }
    GameServer::Shutdown();
}

void SecureGameServer::ProcessNetworkMessages() {
    // First, handle EAC emulator traffic
    if (m_eacEmulator) {
        m_eacEmulator->ProcessRequests();
    }

    // Then, process normal game traffic, filtering EAC packets
    GameServer::ProcessNetworkMessages();
}

void SecureGameServer::PreventEACInitialization() {
    Logger::Info("Preventing EAC initialization...");
#ifdef _WIN32
    SetEnvironmentVariableA("DISABLE_EAC", "1");
    SetEnvironmentVariableA("EAC_DISABLED", "true");
#else
    setenv("DISABLE_EAC", "1", 1);
    setenv("EAC_DISABLED", "true", 1);
#endif
    Logger::Debug("Environment variables set to disable EAC");
}

void SecureGameServer::BlockEACTraffic() {
    Logger::Info("Blocking EAC network traffic...");
    std::vector<std::string> endpoints = {
        "prod.easyanticheat.net",
        "api.easyanticheat.net",
        "eac.epicgames.com",
        "telemetry.epicgames.com"
    };
    for (const auto& ep : endpoints) {
        m_networkBlocker->BlockDomain(ep);
        Logger::Debug("Blocked domain: %s", ep.c_str());
    }
}

void SecureGameServer::DisableTelemetry() {
    Logger::Info("Disabling telemetry...");
    m_telemetryBlocker->BlockAllTelemetry();
    m_telemetryBlocker->BlockCrashReporting();
    Logger::Debug("Telemetry and crash reporting blocked");
}