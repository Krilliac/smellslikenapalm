// src/Config/NetworkConfig.h

#pragma once

#include <string>
#include "Config/ServerConfig.h"

class NetworkConfig {
public:
    explicit NetworkConfig(const ServerConfig& cfg);

    int         GetPort() const;
    const std::string& GetBindAddress() const;
    int         GetMaxPacketSize() const;
    int         GetClientIdleTimeoutMs() const;
    int         GetHeartbeatIntervalMs() const;
    bool        IsDualStack() const;
    bool        IsReliableTransport() const;

private:
    int         m_port;
    std::string m_bindAddress;
    int         m_maxPacketSize;
    int         m_clientIdleTimeoutMs;
    int         m_heartbeatIntervalMs;
    bool        m_dualStack;
    bool        m_reliableTransport;
};