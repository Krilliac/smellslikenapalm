// src/Game/SecureGameServer.h – Header for SecureGameServer

#pragma once

#include "Game/GameServer.h"
#include "Security/NetworkBlocker.h"
#include "Security/TelemetryBlocker.h"
#include "Security/EACServerEmulator.h"
#include "Security/EnhancedEACAntiCheat.h"

class SecureGameServer : public GameServer {
public:
    SecureGameServer();
    ~SecureGameServer() override;

    bool Initialize() override;
    void Shutdown() override;

protected:
    void ProcessNetworkMessages() override;

private:
    void PreventEACInitialization();
    void BlockEACTraffic();
    void DisableTelemetry();

    std::unique_ptr<NetworkBlocker>     m_networkBlocker;
    std::unique_ptr<TelemetryBlocker>   m_telemetryBlocker;
    std::unique_ptr<EACServerEmulator>   m_eacEmulator;
    std::unique_ptr<EnhancedEACAntiCheat> m_enhancedAntiCheat;
};