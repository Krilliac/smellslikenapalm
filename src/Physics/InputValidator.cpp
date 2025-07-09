// src/Input/InputValidator.cpp
#include "Input/InputValidator.h"
#include <algorithm>

const std::regex InputValidator::kChatPattern(R"(^[A-Za-z0-9 ,.!?;:'\"@#\$%\^&\*\(\)\-_\+=\[\]\{\}\\\/\|<>]{1,256}$)");
const std::regex InputValidator::kCommandPattern(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
const std::regex InputValidator::kAlnumPattern(R"(^[A-Za-z0-9_]+$)");

bool InputValidator::IsValidChatMessage(const std::string& msg) {
    return std::regex_match(msg, kChatPattern);
}

std::string InputValidator::SanitizeChatMessage(const std::string& msg) {
    std::string out;
    out.reserve(msg.size());
    for (char c : msg) {
        if (std::regex_match(std::string(1, c), std::regex(R"([^\x00-\x1F\x7F])"))) {
            out.push_back(c);
        }
    }
    if (out.size() > 256) out.resize(256);
    return out;
}

bool InputValidator::IsValidCommand(const std::string& cmd, const std::vector<std::string>& args) {
    if (!std::regex_match(cmd, kCommandPattern)) return false;
    for (auto& a : args) {
        if (a.empty() || !std::regex_match(a, kAlnumPattern)) return false;
    }
    return true;
}

bool InputValidator::IsValidPosition(float x, float y, float z) {
    return x >= kWorldMinX && x <= kWorldMaxX &&
           y >= kWorldMinY && y <= kWorldMaxY &&
           z >= kWorldMinZ && z <= kWorldMaxZ;
}

bool InputValidator::IsValidOrientation(float pitch, float yaw, float roll) {
    return pitch >= -180.0f && pitch <= 180.0f &&
           yaw   >= -180.0f && yaw   <= 180.0f &&
           roll  >= -180.0f && roll  <= 180.0f;
}

bool InputValidator::IsValidAction(const std::string& actionTag) {
    // Allow only known action tags (alphanumeric + underscore)
    return std::regex_match(actionTag, kAlnumPattern);
}

bool InputValidator::IsValidString(const std::string& s, size_t maxLength, const std::regex& allowedPattern) {
    if (s.empty() || s.size() > maxLength) return false;
    return std::regex_match(s, allowedPattern);
}