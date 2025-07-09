// src/Game/ChatManager.cpp – Complete implementation for RS2V Server ChatManager

#include "Game/ChatManager.h"
#include "Utils/Logger.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include <algorithm>

ChatManager::ChatManager(GameServer* server)
    : m_server(server)
{
    Logger::Info("ChatManager initialized");
}

ChatManager::~ChatManager() = default;

void ChatManager::Initialize()
{
    Logger::Info("ChatManager: Ready to handle chat messages");
}

void ChatManager::Shutdown()
{
    Logger::Info("ChatManager: Shutting down");
}

void ChatManager::HandlePlayerChat(uint32_t clientId, const std::string& message)
{
    auto conn = m_server->GetClientConnection(clientId);
    if (!conn) {
        Logger::Warn("ChatManager: Invalid client ID %u", clientId);
        return;
    }

    std::string playerName = conn->GetPlayerName();
    std::string formatted = FormatChatMessage(playerName, message);
    BroadcastChat(formatted);
}

void ChatManager::HandleTeamChat(uint32_t clientId, const std::string& message)
{
    auto conn = m_server->GetClientConnection(clientId);
    if (!conn) {
        Logger::Warn("ChatManager: Invalid client ID %u", clientId);
        return;
    }

    uint32_t teamId = conn->GetTeamId();
    std::string playerName = conn->GetPlayerName();
    std::string formatted = FormatTeamMessage(playerName, message);
    BroadcastTeam(teamId, formatted);
}

void ChatManager::HandleGlobalChat(uint32_t clientId, const std::string& message)
{
    // Alias for public chat
    HandlePlayerChat(clientId, message);
}

void ChatManager::BroadcastChat(const std::string& message)
{
    Logger::Info("Broadcasting chat: %s", message.c_str());
    for (auto& conn : m_server->GetAllConnections()) {
        conn->SendChatMessage(message);
    }
}

void ChatManager::BroadcastTeam(uint32_t teamId, const std::string& message)
{
    Logger::Info("Broadcasting team chat to team %u: %s", teamId, message.c_str());
    for (auto& conn : m_server->GetAllConnections()) {
        if (conn->GetTeamId() == teamId) {
            conn->SendChatMessage(message);
        }
    }
}

std::string ChatManager::FormatChatMessage(const std::string& playerName, const std::string& message) const
{
    return "<" + playerName + "> " + message;
}

std::string ChatManager::FormatTeamMessage(const std::string& playerName, const std::string& message) const
{
    return "[TEAM] <" + playerName + "> " + message;
}

bool ChatManager::IsMessageAllowed(const std::string& message) const
{
    // Basic spam/filter check; reject empty or too long
    if (message.empty() || message.size() > 256) {
        return false;
    }
    // TODO: Add profanity filtering, rate limiting
    return true;
}

void ChatManager::ProcessChatCommand(uint32_t clientId, const std::string& message)
{
    if (message.empty() || message[0] != '/') {
        HandlePlayerChat(clientId, message);
        return;
    }

    // Parse command
    auto parts = Split(message.substr(1), ' ');
    std::string cmd = parts.empty() ? "" : parts[0];
    parts.erase(parts.begin());

    // Handle built-in commands
    if (cmd == "me" && !parts.empty()) {
        std::string action = Join(parts, " ");
        BroadcastChat("* " + m_server->GetClientConnection(clientId)->GetPlayerName() + " " + action);
    }
    else if (cmd == "team" && !parts.empty()) {
        std::string teamMsg = Join(parts, " ");
        HandleTeamChat(clientId, teamMsg);
    }
    else {
        // Unknown or pass to admin/command manager
        m_server->GetAdminManager()->HandleAdminCommand(clientId, cmd, parts);
    }
}

std::vector<std::string> ChatManager::Split(const std::string& s, char delimiter) const
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string ChatManager::Join(const std::vector<std::string>& parts, const std::string& sep) const
{
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) oss << sep;
        oss << parts[i];
    }
    return oss.str();
}