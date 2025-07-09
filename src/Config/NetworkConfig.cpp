// src/Config/NetworkConfig.cpp

#include "Config/NetworkConfig.h"

NetworkConfig::NetworkConfig(const ServerConfig& cfg)
  : m_port(cfg.GetPort())
  , m_bindAddress(cfg.GetBindAddress())
  , m_maxPacketSize(cfg.GetMaxPacketSize())
  , m_clientIdleTimeoutMs(cfg.GetClientIdleTimeout() * 1000)
  , m_heartbeatIntervalMs(cfg.GetHeartbeatInterval() * 1000)
  , m_dualStack(cfg.IsDualStack())
  , m_reliableTransport(cfg.IsReliableTransport())
{}

int NetworkConfig::GetPort() const {
    return m_port;
}

const std::string& NetworkConfig::GetBindAddress() const {
    return m_bindAddress;
}

int NetworkConfig::GetMaxPacketSize() const {
    return m_maxPacketSize;
}

int NetworkConfig::GetClientIdleTimeoutMs() const {
    return m_clientIdleTimeoutMs;
}

int NetworkConfig::GetHeartbeatIntervalMs() const {
    return m_heartbeatIntervalMs;
}

bool NetworkConfig::IsDualStack() const {
    return m_dualStack;
}

bool NetworkConfig::IsReliableTransport() const {
    return m_reliableTransport;
}
