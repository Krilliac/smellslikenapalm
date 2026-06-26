// src/Game/ChatManager.cpp – Complete implementation for RS2V Server ChatManager

#include "Game/ChatManager.h"
#include "Utils/Logger.h"
#include "Network/ClientConnection.h"
#include "Game/GameServer.h"
#include <algorithm>
#include <sstream>
#include "Game/AdminManager.h"

ChatManager::ChatManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[ChatManager::ChatManager] Entry: server=%p", server);
    Logger::Info("ChatManager initialized");
    Logger::Trace("[ChatManager::ChatManager] Exit");
}

ChatManager::~ChatManager()
{
    Logger::Trace("[ChatManager::~ChatManager] Entry");
    Logger::Trace("[ChatManager::~ChatManager] Exit");
}

void ChatManager::Initialize()
{
    Logger::Trace("[ChatManager::Initialize] Entry");

    // Initialize default profanity word list for slur filtering
    m_profanityList = {
        "nigger", "nigga", "faggot", "kike", "spic", "chink",
        "wetback", "coon", "gook", "tranny"
    };

    Logger::Info("ChatManager: Ready (profanity filter: %zu words, rate limit: %d/%ds)",
                 m_profanityList.size(), RATE_LIMIT_MAX_MESSAGES, RATE_LIMIT_WINDOW_SECONDS);
    Logger::Trace("[ChatManager::Initialize] Exit");
}

void ChatManager::Shutdown()
{
    Logger::Trace("[ChatManager::Shutdown] Entry");
    Logger::Info("ChatManager: Shutting down");
    Logger::Trace("[ChatManager::Shutdown] Exit");
}

void ChatManager::HandlePlayerChat(uint32_t clientId, const std::string& message)
{
    Logger::Trace("[ChatManager::HandlePlayerChat] Entry: clientId=%u, message='%s'", clientId, message.c_str());
    auto conn = m_server->GetClientConnection(clientId);
    if (!conn) {
        Logger::Warn("ChatManager: Invalid client ID %u", clientId);
        Logger::Trace("[ChatManager::HandlePlayerChat] Exit (invalid client)");
        return;
    }

    if (!IsMessageAllowed(clientId, message)) {
        Logger::Debug("[ChatManager::HandlePlayerChat] Message rejected by filter for client %u", clientId);
        Logger::Trace("[ChatManager::HandlePlayerChat] Exit (filtered)");
        return;
    }

    std::string cleanMessage = CensorMessage(message);
    std::string playerName = std::to_string(conn->GetClientId());
    Logger::Debug("[ChatManager::HandlePlayerChat] Player '%s' (client %u) sending public chat", playerName.c_str(), clientId);
    std::string formatted = FormatChatMessage(playerName, cleanMessage);
    Logger::Debug("[ChatManager::HandlePlayerChat] Formatted message: '%s'", formatted.c_str());
    BroadcastChat(formatted);
    Logger::Trace("[ChatManager::HandlePlayerChat] Exit");
}

void ChatManager::HandleTeamChat(uint32_t clientId, const std::string& message)
{
    Logger::Trace("[ChatManager::HandleTeamChat] Entry: clientId=%u, message='%s'", clientId, message.c_str());
    auto conn = m_server->GetClientConnection(clientId);
    if (!conn) {
        Logger::Warn("ChatManager: Invalid client ID %u", clientId);
        Logger::Trace("[ChatManager::HandleTeamChat] Exit (invalid client)");
        return;
    }

    if (!IsMessageAllowed(clientId, message)) {
        Logger::Debug("[ChatManager::HandleTeamChat] Message rejected by filter for client %u", clientId);
        Logger::Trace("[ChatManager::HandleTeamChat] Exit (filtered)");
        return;
    }

    std::string cleanMessage = CensorMessage(message);
    uint32_t teamId = conn->GetClientId() % 2;
    std::string playerName = std::to_string(conn->GetClientId());
    Logger::Debug("[ChatManager::HandleTeamChat] Player '%s' (client %u) sending team chat to team %u", playerName.c_str(), clientId, teamId);
    std::string formatted = FormatTeamMessage(playerName, cleanMessage);
    Logger::Debug("[ChatManager::HandleTeamChat] Formatted message: '%s'", formatted.c_str());
    BroadcastTeam(teamId, formatted);
    Logger::Trace("[ChatManager::HandleTeamChat] Exit");
}

void ChatManager::HandleGlobalChat(uint32_t clientId, const std::string& message)
{
    Logger::Trace("[ChatManager::HandleGlobalChat] Entry: clientId=%u, message='%s'", clientId, message.c_str());
    Logger::Debug("[ChatManager::HandleGlobalChat] Routing to HandlePlayerChat as alias");
    // Alias for public chat
    HandlePlayerChat(clientId, message);
    Logger::Trace("[ChatManager::HandleGlobalChat] Exit");
}

void ChatManager::BroadcastChat(const std::string& message)
{
    Logger::Trace("[ChatManager::BroadcastChat] Entry: message='%s'", message.c_str());
    Logger::Info("Broadcasting chat: %s", message.c_str());
    auto connections = m_server->GetAllConnections();
    Logger::Debug("[ChatManager::BroadcastChat] Sending to %zu connections", connections.size());
    for (auto& conn : connections) {
        if (!conn) {
            Logger::Warn("[ChatManager::BroadcastChat] Skipping null connection in broadcast list");
            continue;
        }
        conn->SendChatMessage(message);
    }
    Logger::Trace("[ChatManager::BroadcastChat] Exit");
}

void ChatManager::BroadcastTeam(uint32_t teamId, const std::string& message)
{
    Logger::Trace("[ChatManager::BroadcastTeam] Entry: teamId=%u, message='%s'", teamId, message.c_str());
    Logger::Info("Broadcasting team chat to team %u: %s", teamId, message.c_str());
    int sentCount = 0;
    for (auto& conn : m_server->GetAllConnections()) {
        if (!conn) {
            Logger::Warn("[ChatManager::BroadcastTeam] Skipping null connection in broadcast list");
            continue;
        }
        if (conn->GetClientId() % 2 == teamId) {
            conn->SendChatMessage(message);
            sentCount++;
        }
    }
    Logger::Debug("[ChatManager::BroadcastTeam] Sent to %d players on team %u", sentCount, teamId);
    Logger::Trace("[ChatManager::BroadcastTeam] Exit");
}

std::string ChatManager::FormatChatMessage(const std::string& playerName, const std::string& message) const
{
    Logger::Trace("[ChatManager::FormatChatMessage] Entry: playerName='%s', message='%s'", playerName.c_str(), message.c_str());
    std::string result = "<" + playerName + "> " + message;
    Logger::Trace("[ChatManager::FormatChatMessage] Exit: result='%s'", result.c_str());
    return result;
}

std::string ChatManager::FormatTeamMessage(const std::string& playerName, const std::string& message) const
{
    Logger::Trace("[ChatManager::FormatTeamMessage] Entry: playerName='%s', message='%s'", playerName.c_str(), message.c_str());
    std::string result = "[TEAM] <" + playerName + "> " + message;
    Logger::Trace("[ChatManager::FormatTeamMessage] Exit: result='%s'", result.c_str());
    return result;
}

bool ChatManager::IsMessageAllowed(uint32_t clientId, const std::string& message)
{
    Logger::Trace("[ChatManager::IsMessageAllowed] Entry: clientId=%u, message.size=%zu", clientId, message.size());

    // Reject empty messages
    if (message.empty()) {
        Logger::Debug("[ChatManager::IsMessageAllowed] Message rejected: empty");
        return false;
    }

    // Reject messages that are too long
    if (message.size() > 256) {
        Logger::Debug("[ChatManager::IsMessageAllowed] Message rejected: too long (%zu > 256)", message.size());
        return false;
    }

    // Rate limiting: check if the client has sent too many messages in the time window
    auto now = std::chrono::steady_clock::now();
    auto& entry = m_rateLimits[clientId];

    // Remove timestamps outside the window
    auto windowStart = now - std::chrono::seconds(RATE_LIMIT_WINDOW_SECONDS);
    entry.timestamps.erase(
        std::remove_if(entry.timestamps.begin(), entry.timestamps.end(),
                        [&windowStart](const auto& ts) { return ts < windowStart; }),
        entry.timestamps.end());

    if (static_cast<int>(entry.timestamps.size()) >= RATE_LIMIT_MAX_MESSAGES) {
        Logger::Debug("[ChatManager::IsMessageAllowed] Message rejected: rate limited (client %u, %zu msgs in %ds)",
                      clientId, entry.timestamps.size(), RATE_LIMIT_WINDOW_SECONDS);
        return false;
    }

    // Record this message timestamp
    entry.timestamps.push_back(now);

    // Profanity check (message is still allowed but will be censored at send time)
    if (ContainsProfanity(message)) {
        Logger::Debug("[ChatManager::IsMessageAllowed] Message contains profanity, will be censored (client %u)", clientId);
        // Still allowed, but will be censored in the format step
    }

    Logger::Debug("[ChatManager::IsMessageAllowed] Message allowed (client=%u, size=%zu, rate=%zu/%d)",
                  clientId, message.size(), entry.timestamps.size(), RATE_LIMIT_MAX_MESSAGES);
    return true;
}

bool ChatManager::ContainsProfanity(const std::string& message) const
{
    // Convert message to lowercase for case-insensitive matching
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& word : m_profanityList) {
        if (lower.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string ChatManager::CensorMessage(const std::string& message) const
{
    std::string result = message;
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& word : m_profanityList) {
        size_t pos = 0;
        while ((pos = lower.find(word, pos)) != std::string::npos) {
            std::string replacement(word.size(), '*');
            result.replace(pos, word.size(), replacement);
            lower.replace(pos, word.size(), replacement);
            pos += word.size();
        }
    }
    return result;
}

void ChatManager::ProcessChatCommand(uint32_t clientId, const std::string& message)
{
    Logger::Trace("[ChatManager::ProcessChatCommand] Entry: clientId=%u, message='%s'", clientId, message.c_str());
    if (message.empty() || message[0] != '/') {
        Logger::Debug("[ChatManager::ProcessChatCommand] Not a command, routing to HandlePlayerChat");
        HandlePlayerChat(clientId, message);
        Logger::Trace("[ChatManager::ProcessChatCommand] Exit (not a command)");
        return;
    }

    // Reject oversized command strings (attacker-controlled RPC input) to bound parsing work
    if (message.size() > 256) {
        Logger::Warn("[ChatManager::ProcessChatCommand] Command rejected: too long (%zu > 256) from client %u", message.size(), clientId);
        Logger::Trace("[ChatManager::ProcessChatCommand] Exit (too long)");
        return;
    }

    // Parse command
    auto parts = Split(message.substr(1), ' ');
    std::string cmd = parts.empty() ? "" : parts[0];
    parts.erase(parts.begin());
    Logger::Debug("[ChatManager::ProcessChatCommand] Parsed command='%s' with %zu args", cmd.c_str(), parts.size());

    // Handle built-in commands
    if (cmd == "me" && !parts.empty()) {
        std::string action = Join(parts, " ");
        Logger::Debug("[ChatManager::ProcessChatCommand] Processing /me action='%s'", action.c_str());
        auto conn = m_server->GetClientConnection(clientId);
        if (!conn) {
            Logger::Warn("[ChatManager::ProcessChatCommand] /me from invalid client %u — ignoring", clientId);
            Logger::Trace("[ChatManager::ProcessChatCommand] Exit (invalid client)");
            return;
        }
        BroadcastChat("* " + std::to_string(conn->GetClientId()) + " " + action);
    }
    else if (cmd == "team" && !parts.empty()) {
        std::string teamMsg = Join(parts, " ");
        Logger::Debug("[ChatManager::ProcessChatCommand] Processing /team message='%s'", teamMsg.c_str());
        HandleTeamChat(clientId, teamMsg);
    }
    else {
        Logger::Debug("[ChatManager::ProcessChatCommand] Unknown built-in command '%s', passing to AdminManager", cmd.c_str());
        // Unknown or pass to admin/command manager
        AdminManager* admin = m_server->GetAdminManager();
        if (!admin) {
            Logger::Warn("[ChatManager::ProcessChatCommand] AdminManager unavailable — dropping command '%s' from client %u", cmd.c_str(), clientId);
            Logger::Trace("[ChatManager::ProcessChatCommand] Exit (no admin manager)");
            return;
        }
        admin->HandleAdminCommand(clientId, cmd, parts);
    }
    Logger::Trace("[ChatManager::ProcessChatCommand] Exit");
}

std::vector<std::string> ChatManager::Split(const std::string& s, char delimiter) const
{
    Logger::Trace("[ChatManager::Split] Entry: s='%s', delimiter='%c'", s.c_str(), delimiter);
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    Logger::Trace("[ChatManager::Split] Exit: %zu tokens", tokens.size());
    return tokens;
}

std::string ChatManager::Join(const std::vector<std::string>& parts, const std::string& sep) const
{
    Logger::Trace("[ChatManager::Join] Entry: parts.size=%zu, sep='%s'", parts.size(), sep.c_str());
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) oss << sep;
        oss << parts[i];
    }
    std::string result = oss.str();
    Logger::Trace("[ChatManager::Join] Exit: result='%s'", result.c_str());
    return result;
}
