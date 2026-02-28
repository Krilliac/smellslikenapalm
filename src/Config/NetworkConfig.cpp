// src/Config/NetworkConfig.cpp

#include "Config/NetworkConfig.h"
#include "Utils/Logger.h"

NetworkConfig::NetworkConfig(const ServerConfig& cfg)
  : m_port(cfg.GetPort())
  , m_bindAddress(cfg.GetBindAddress())
  , m_maxPacketSize(cfg.GetMaxPacketSize())
  , m_clientIdleTimeoutMs(cfg.GetClientIdleTimeout() * 1000)
  , m_heartbeatIntervalMs(cfg.GetHeartbeatInterval() * 1000)
  , m_dualStack(cfg.IsDualStack())
  , m_reliableTransport(cfg.IsReliableTransport())
{
    Logger::Trace("[NetworkConfig::NetworkConfig] Entry - constructing from ServerConfig");
    Logger::Info("[NetworkConfig::NetworkConfig] NetworkConfig initialized: port=%d, bind='%s', maxPacket=%d, idleTimeout=%dms, heartbeat=%dms, dualStack=%s, reliable=%s",
                 m_port, m_bindAddress.c_str(), m_maxPacketSize, m_clientIdleTimeoutMs, m_heartbeatIntervalMs,
                 m_dualStack ? "true" : "false", m_reliableTransport ? "true" : "false");
    Logger::Trace("[NetworkConfig::NetworkConfig] Exit");
}

int NetworkConfig::GetPort() const {
    Logger::Trace("[NetworkConfig::GetPort] Entry");
    Logger::Trace("[NetworkConfig::GetPort] Exit - returning %d", m_port);
    return m_port;
}

const std::string& NetworkConfig::GetBindAddress() const {
    Logger::Trace("[NetworkConfig::GetBindAddress] Entry");
    Logger::Trace("[NetworkConfig::GetBindAddress] Exit - returning '%s'", m_bindAddress.c_str());
    return m_bindAddress;
}

int NetworkConfig::GetMaxPacketSize() const {
    Logger::Trace("[NetworkConfig::GetMaxPacketSize] Entry");
    Logger::Trace("[NetworkConfig::GetMaxPacketSize] Exit - returning %d", m_maxPacketSize);
    return m_maxPacketSize;
}

int NetworkConfig::GetClientIdleTimeoutMs() const {
    Logger::Trace("[NetworkConfig::GetClientIdleTimeoutMs] Entry");
    Logger::Trace("[NetworkConfig::GetClientIdleTimeoutMs] Exit - returning %d", m_clientIdleTimeoutMs);
    return m_clientIdleTimeoutMs;
}

int NetworkConfig::GetHeartbeatIntervalMs() const {
    Logger::Trace("[NetworkConfig::GetHeartbeatIntervalMs] Entry");
    Logger::Trace("[NetworkConfig::GetHeartbeatIntervalMs] Exit - returning %d", m_heartbeatIntervalMs);
    return m_heartbeatIntervalMs;
}

bool NetworkConfig::IsDualStack() const {
    Logger::Trace("[NetworkConfig::IsDualStack] Entry");
    Logger::Trace("[NetworkConfig::IsDualStack] Exit - returning %s", m_dualStack ? "true" : "false");
    return m_dualStack;
}

bool NetworkConfig::IsReliableTransport() const {
    Logger::Trace("[NetworkConfig::IsReliableTransport] Entry");
    Logger::Trace("[NetworkConfig::IsReliableTransport] Exit - returning %s", m_reliableTransport ? "true" : "false");
    return m_reliableTransport;
}
