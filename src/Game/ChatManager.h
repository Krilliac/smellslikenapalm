// src/Game/ChatManager.h – Header for ChatManager

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <chrono>

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

    void BroadcastChat(const std::string& message);

private:
    GameServer* m_server;
    void BroadcastTeam(uint32_t teamId, const std::string& message);

    std::string FormatChatMessage(const std::string& playerName, const std::string& message) const;
    std::string FormatTeamMessage(const std::string& playerName, const std::string& message) const;

    bool IsMessageAllowed(uint32_t clientId, const std::string& message);

    // Profanity filtering
    bool ContainsProfanity(const std::string& message) const;
    std::string CensorMessage(const std::string& message) const;
    std::vector<std::string> m_profanityList;

    // Rate limiting: track last message timestamps per client
    struct RateLimitEntry {
        std::vector<std::chrono::steady_clock::time_point> timestamps;
    };
    std::unordered_map<uint32_t, RateLimitEntry> m_rateLimits;
    static constexpr int RATE_LIMIT_MAX_MESSAGES = 5;       // max messages per window
    static constexpr int RATE_LIMIT_WINDOW_SECONDS = 10;    // window in seconds

    std::vector<std::string> Split(const std::string& s, char delimiter) const;
    std::string Join(const std::vector<std::string>& parts, const std::string& sep) const;
};