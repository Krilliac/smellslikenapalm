// src/Game/ChatManager.h – Header for ChatManager

#pragma once

#include <string>
#include <vector>
#include <memory>

class GameServer;

class ChatManager {
public:
    explicit ChatManager(GameServer* server);
    ~ChatManager();

    void Initialize();
    void Shutdown();

    void HandlePlayerChat(uint32_t clientId, const std::string& message);
    void HandleTeamChat(uint32_t clientId, const std::string& message);
    void HandleGlobalChat(uint32_t clientId, const std::string& message);

    void ProcessChatCommand(uint32_t clientId, const std::string& message);

private:
    GameServer* m_server;

    void BroadcastChat(const std::string& message);
    void BroadcastTeam(uint32_t teamId, const std::string& message);

    std::string FormatChatMessage(const std::string& playerName, const std::string& message) const;
    std::string FormatTeamMessage(const std::string& playerName, const std::string& message) const;

    bool IsMessageAllowed(const std::string& message) const;

    std::vector<std::string> Split(const std::string& s, char delimiter) const;
    std::string Join(const std::vector<std::string>& parts, const std::string& sep) const;
};