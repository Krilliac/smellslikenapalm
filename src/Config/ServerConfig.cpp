// src/Config/ServerConfig.cpp – Complete implementation for RS2V ServerConfig

#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <filesystem>
#include <sstream>

ServerConfig::ServerConfig() {
    Logger::Info("ServerConfig initialized");
}

ServerConfig::~ServerConfig() = default;

bool ServerConfig::Initialize(const std::string& configFile) {
    Logger::Info("Initializing ServerConfig from %s", configFile.c_str());
    m_configFile = configFile;
    
    // Ensure config directory exists
    auto path = std::filesystem::path(configFile).parent_path();
    if (!path.empty() && !std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
        Logger::Info("Created config directory: %s", path.string().c_str());
    }
    
    // Load or create defaults
    if (!std::filesystem::exists(configFile)) {
        Logger::Warn("Config file not found, creating default at %s", configFile.c_str());
        SetDefaults();
        return Save();
    }
    return Load();
}

bool ServerConfig::Load() {
    Logger::Info("Loading server configuration: %s", m_configFile.c_str());
    std::ifstream file(m_configFile);
    if (!file.is_open()) {
        Logger::Error("Failed to open server config: %s", m_configFile.c_str());
        return false;
    }
    
    std::string line, section;
    size_t lineno = 0;
    while (std::getline(file, line)) {
        ++lineno;
        line = StringUtils::Trim(line);
        if (line.empty() || line[0]=='#') continue;
        if (line.front()=='[' && line.back()==']') {
            section = line.substr(1, line.size()-2);
            continue;
        }
        auto pos = line.find('=');
        if (pos==std::string::npos) {
            Logger::Warn("Invalid line %zu in %s: %s", lineno, m_configFile.c_str(), line.c_str());
            continue;
        }
        std::string key = StringUtils::Trim(line.substr(0,pos));
        std::string val = StringUtils::Trim(line.substr(pos+1));
        ApplyProperty(section, key, val);
    }
    file.close();
    LogSummary();
    return true;
}

bool ServerConfig::Save() const {
    Logger::Info("Saving server configuration to %s", m_configFile.c_str());
    std::ofstream file(m_configFile, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("Failed to open file for writing: %s", m_configFile.c_str());
        return false;
    }
    file << "# RS2V Server Configuration\n\n";
    file << "[Server]\n";
    file << "ServerName=" << serverName << "\n";
    file << "MaxPlayers=" << maxPlayers << "\n";
    file << "GamePort=" << gamePort << "\n";
    file << "QueryPort=" << queryPort << "\n";
    file << "TickRate=" << tickRate << "\n\n";
    
    file << "[Logging]\n";
    file << "LogLevel=" << logLevel << "\n";
    file << "LogFile=" << logFile << "\n";
    file << "PacketLogging=" << (packetLogging? "true":"false") << "\n\n";
    
    file << "[EAC]\n";
    file << "Mode=" << eacMode << "\n";
    file << "ProxyPort=" << eacProxyPort << "\n";
    file << "ForwardToEpic=" << (forwardToEpic? "true":"false") << "\n\n";
    
    file << "[Security]\n";
    file << "SecureMode=" << (secureMode? "true":"false") << "\n";
    file << "PrivacyMode=" << (privacyMode? "true":"false") << "\n";
    file << "AllowTelemetry=" << (allowTelemetry? "true":"false") << "\n";
    file.close();
    Logger::Info("Server configuration saved");
    return true;
}

void ServerConfig::SetDefaults() {
    Logger::Info("Applying default server configuration");
    serverName      = "RS2V Custom Server";
    maxPlayers      = 64;
    gamePort        = 7777;
    queryPort       = 27015;
    tickRate        = 60;
    logLevel        = "INFO";
    logFile         = "logs/server.log";
    packetLogging   = true;
    eacMode         = "DISABLED";
    eacProxyPort    = 7957;
    forwardToEpic   = false;
    secureMode      = true;
    privacyMode     = true;
    allowTelemetry  = false;
}

void ServerConfig::ApplyProperty(const std::string& section, const std::string& key, const std::string& val) {
    if (section=="Server") {
        if (key=="ServerName")      serverName    = val;
        else if (key=="MaxPlayers") maxPlayers    = std::stoi(val);
        else if (key=="GamePort")   gamePort      = std::stoi(val);
        else if (key=="QueryPort")  queryPort     = std::stoi(val);
        else if (key=="TickRate")   tickRate      = std::stoi(val);
        else Logger::Warn("Unknown Server key: %s", key.c_str());
    }
    else if (section=="Logging") {
        if (key=="LogLevel")        logLevel      = val;
        else if (key=="LogFile")    logFile       = val;
        else if (key=="PacketLogging") packetLogging = StringUtils::ToBool(val);
        else Logger::Warn("Unknown Logging key: %s", key.c_str());
    }
    else if (section=="EAC") {
        if (key=="Mode")            eacMode       = val;
        else if (key=="ProxyPort")  eacProxyPort  = std::stoi(val);
        else if (key=="ForwardToEpic") forwardToEpic = StringUtils::ToBool(val);
        else Logger::Warn("Unknown EAC key: %s", key.c_str());
    }
    else if (section=="Security") {
        if (key=="SecureMode")      secureMode    = StringUtils::ToBool(val);
        else if (key=="PrivacyMode") privacyMode   = StringUtils::ToBool(val);
        else if (key=="AllowTelemetry") allowTelemetry = StringUtils::ToBool(val);
        else Logger::Warn("Unknown Security key: %s", key.c_str());
    }
    else {
        Logger::Warn("Unknown config section: %s", section.c_str());
    }
}

void ServerConfig::LogSummary() const {
    Logger::Info("=== ServerConfig Summary ===");
    Logger::Info("ServerName:      %s", serverName.c_str());
    Logger::Info("MaxPlayers:      %d", maxPlayers);
    Logger::Info("GamePort:        %d", gamePort);
    Logger::Info("QueryPort:       %d", queryPort);
    Logger::Info("TickRate:        %d", tickRate);
    Logger::Info("LogLevel:        %s", logLevel.c_str());
    Logger::Info("LogFile:         %s", logFile.c_str());
    Logger::Info("PacketLogging:   %s", packetLogging? "yes":"no");
    Logger::Info("EAC Mode:        %s", eacMode.c_str());
    Logger::Info("EAC ProxyPort:   %d", eacProxyPort);
    Logger::Info("ForwardToEpic:   %s", forwardToEpic? "yes":"no");
    Logger::Info("SecureMode:      %s", secureMode? "yes":"no");
    Logger::Info("PrivacyMode:     %s", privacyMode? "yes":"no");
    Logger::Info("AllowTelemetry:  %s", allowTelemetry? "yes":"no");
    Logger::Info("============================");
}

// Getters
const std::string& ServerConfig::GetServerName() const     { return serverName; }
int ServerConfig::GetMaxPlayers()       const              { return maxPlayers; }
int ServerConfig::GetGamePort()         const              { return gamePort; }
int ServerConfig::GetQueryPort()        const              { return queryPort; }
int ServerConfig::GetTickRate()         const              { return tickRate; }
const std::string& ServerConfig::GetLogLevel() const       { return logLevel; }
const std::string& ServerConfig::GetLogFile() const        { return logFile; }
bool ServerConfig::IsPacketLogging()    const              { return packetLogging; }
const std::string& ServerConfig::GetEACMode() const        { return eacMode; }
int ServerConfig::GetEACProxyPort()     const              { return eacProxyPort; }
bool ServerConfig::ShouldForwardToEpic() const            { return forwardToEpic; }
bool ServerConfig::IsSecureMode()       const              { return secureMode; }
bool ServerConfig::IsPrivacyMode()      const              { return privacyMode; }
bool ServerConfig::IsTelemetryAllowed() const              { return allowTelemetry; }

bool ServerConfig::Reload() {
    Logger::Info("Reloading server configuration...");
    return Load();
}