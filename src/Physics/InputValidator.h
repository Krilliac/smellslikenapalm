// src/Physics/InputValidator.h
#pragma once

#include <string>
#include <vector>
#include <regex>

class InputValidator {
public:
    // Chat/message validation
    static bool IsValidChatMessage(const std::string& msg);
    static std::string SanitizeChatMessage(const std::string& msg);

    // Command validation (e.g., “/kick steamId”)
    static bool IsValidCommand(const std::string& cmd, const std::vector<std::string>& args);

    // Movement/position validation
    static bool IsValidPosition(float x, float y, float z);
    static bool IsValidOrientation(float pitch, float yaw, float roll);

    // Action validation (e.g., weapon fire rates, reload)
    static bool IsValidAction(const std::string& actionTag);

    // Generic string validator (length + allowed chars)
    static bool IsValidString(const std::string& s, size_t maxLength,
                              const std::regex& allowedPattern);

private:
    // Patterns
    static const std::regex kChatPattern;       // allowed chat chars
    static const std::regex kCommandPattern;    // command name
    static const std::regex kAlnumPattern;      // alphanumeric + underscores

    // Bounds (use the wider/safer world bounds; matches large RS2V maps)
    static constexpr float kWorldMinX = -100000.0f;
    static constexpr float kWorldMaxX =  100000.0f;
    static constexpr float kWorldMinY = -100000.0f;
    static constexpr float kWorldMaxY =  100000.0f;
    static constexpr float kWorldMinZ = -100000.0f;
    static constexpr float kWorldMaxZ =  100000.0f;
};