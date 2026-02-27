// src/Input/InputValidator.h
#pragma once

#include <string>
#include <vector>
#include <regex>

class InputValidator {
public:
    static bool IsValidChatMessage(const std::string& msg);
    static std::string SanitizeChatMessage(const std::string& msg);
    static bool IsValidCommand(const std::string& cmd, const std::vector<std::string>& args);
    static bool IsValidPosition(float x, float y, float z);
    static bool IsValidOrientation(float pitch, float yaw, float roll);
    static bool IsValidAction(const std::string& actionTag);
    static bool IsValidString(const std::string& s, size_t maxLength, const std::regex& allowedPattern);

    static constexpr float kWorldMinX = -100000.0f;
    static constexpr float kWorldMaxX =  100000.0f;
    static constexpr float kWorldMinY = -100000.0f;
    static constexpr float kWorldMaxY =  100000.0f;
    static constexpr float kWorldMinZ = -100000.0f;
    static constexpr float kWorldMaxZ =  100000.0f;

private:
    static const std::regex kChatPattern;
    static const std::regex kCommandPattern;
    static const std::regex kAlnumPattern;
};
