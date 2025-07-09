// src/Config/NetworkConfig.cpp - Complete implementation for RS2V Server network configuration
#include "Config/NetworkConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <filesystem>
#include <algorithm>

NetworkConfig::NetworkConfig() {
    Logger::Info("NetworkConfig initialized");
}

NetworkConfig::~NetworkConfig() = default;

bool NetworkConfig::Initialize(std::shared_ptr<ConfigManager> configManager) {
    Logger::Info("Initializing NetworkConfig...");
    m_configManager = configManager;
    if (!m_configManager) {
        Logger::Error("ConfigManager is null");
        return false;
    }
    return Load();
}

bool NetworkConfig::Load() {
    Logger::Debug("Loading network configuration...");
    // Read basic network settings
    m_listenAddress = m_configManager->GetString("Network.BindAddress", "0.0.0.0");
    m_gamePort      = m_configManager->GetInt   ("Network.GamePort",    7777);
    m_queryPort     = m_configManager->GetInt   ("Network.QueryPort",   27015);
    m_maxPacketSize = m_configManager->GetInt   ("Network.MaxPacketSize", 1200);
    m_timeoutSec    = m_configManager->GetInt   ("Network.TimeoutSeconds", 30);
    m_compression   = m_configManager->GetBool  ("Network.CompressionEnabled", true);
    m_dedicated     = m_configManager->GetBool  ("Network.DedicatedServer", true);

    // Validate ports do not conflict
    if (m_gamePort == m_queryPort) {
        Logger::Error("NetworkConfig: GamePort and QueryPort must differ (%d)", m_gamePort);
        return false;
    }

    // Validate address format
    if (!IsValidIPAddress(m_listenAddress)) {
        Logger::Error("NetworkConfig: Invalid BindAddress: %s", m_listenAddress.c_str());
        return false;
    }

    // Enforce sensible bounds
    m_maxPacketSize = std::clamp(m_maxPacketSize, 64, 65536);
    m_timeoutSec    = std::clamp(m_timeoutSec, 5, 300);

    Logger::Info(
        "NetworkConfig loaded: BindAddress=%s, GamePort=%d, QueryPort=%d, MaxPacketSize=%d, Timeout=%ds, Compression=%s, Dedicated=%s",
        m_listenAddress.c_str(),
        m_gamePort,
        m_queryPort,
        m_maxPacketSize,
        m_timeoutSec,
        m_compression ? "enabled" : "disabled",
        m_dedicated   ? "yes"     : "no"
    );

    return true;
}

bool NetworkConfig::Save(const std::string& configFile) const {
    Logger::Info("Saving network configuration to %s", configFile.c_str());

    std::filesystem::path path(configFile);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream file(configFile, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("NetworkConfig: Failed to open file for writing: %s", configFile.c_str());
        return false;
    }

    file << "# RS2V Network Configuration\n\n";
    file << "[Network]\n";
    file << "BindAddress="          << m_listenAddress   << "\n";
    file << "GamePort="             << m_gamePort        << "\n";
    file << "QueryPort="            << m_queryPort       << "\n";
    file << "MaxPacketSize="        << m_maxPacketSize   << "\n";
    file << "TimeoutSeconds="       << m_timeoutSec      << "\n";
    file << "CompressionEnabled="   << (m_compression ? "true" : "false") << "\n";
    file << "DedicatedServer="      << (m_dedicated   ? "true" : "false") << "\n";

    file.close();
    Logger::Info("Network configuration saved");
    return true;
}

std::string NetworkConfig::GetBindAddress() const {
    return m_listenAddress;
}

int NetworkConfig::GetGamePort() const {
    return m_gamePort;
}

int NetworkConfig::GetQueryPort() const {
    return m_queryPort;
}

int NetworkConfig::GetMaxPacketSize() const {
    return m_maxPacketSize;
}

int NetworkConfig::GetTimeoutSeconds() const {
    return m_timeoutSec;
}

bool NetworkConfig::IsCompressionEnabled() const {
    return m_compression;
}

bool NetworkConfig::IsDedicatedServer() const {
    return m_dedicated;
}

bool NetworkConfig::Reload() {
    Logger::Info("Reloading network configuration...");
    return Load();
}

bool NetworkConfig::IsValidIPAddress(const std::string& ip) const {
    // Simple IPv4 validation
    const std::regex ipRegex(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");
    return std::regex_match(ip, ipRegex) || ip == "0.0.0.0" || ip == "localhost";
}

void NetworkConfig::LogSummary() const {
    Logger::Info("=== NetworkConfig Summary ===");
    Logger::Info("BindAddress:        %s", m_listenAddress.c_str());
    Logger::Info("GamePort:           %d", m_gamePort);
    Logger::Info("QueryPort:          %d", m_queryPort);
    Logger::Info("MaxPacketSize:      %d", m_maxPacketSize);
    Logger::Info("TimeoutSeconds:     %d", m_timeoutSec);
    Logger::Info("CompressionEnabled: %s", m_compression ? "yes" : "no");
    Logger::Info("DedicatedServer:    %s", m_dedicated   ? "yes" : "no");
    Logger::Info("=============================");
}