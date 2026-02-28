// src/Input/InputValidator.cpp
#include "Input/InputValidator.h"
#include "Utils/Logger.h"
#include <algorithm>

const std::regex InputValidator::kChatPattern(R"(^[A-Za-z0-9 ,.!?;:'\"@#\$%\^&\*\(\)\-_\+=\[\]\{\}\\\/\|<>]{1,256}$)");
const std::regex InputValidator::kCommandPattern(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
const std::regex InputValidator::kAlnumPattern(R"(^[A-Za-z0-9_]+$)");

bool InputValidator::IsValidChatMessage(const std::string& msg) {
    Logger::Trace("[InputValidator::IsValidChatMessage] Entry - msg='%s' (length=%zu)", msg.c_str(), msg.size());
    bool result = std::regex_match(msg, kChatPattern);
    if (result) {
        Logger::Debug("[InputValidator::IsValidChatMessage] Message passed chat pattern validation");
    } else {
        Logger::Debug("[InputValidator::IsValidChatMessage] Message FAILED chat pattern validation - msg='%s'", msg.c_str());
    }
    Logger::Trace("[InputValidator::IsValidChatMessage] Exit - returning %s", result ? "true" : "false");
    return result;
}

std::string InputValidator::SanitizeChatMessage(const std::string& msg) {
    Logger::Trace("[InputValidator::SanitizeChatMessage] Entry - msg='%s' (length=%zu)", msg.c_str(), msg.size());
    std::string out;
    out.reserve(msg.size());
    int removedCount = 0;
    for (char c : msg) {
        if (std::regex_match(std::string(1, c), std::regex(R"([^\x00-\x1F\x7F])"))) {
            out.push_back(c);
        } else {
            removedCount++;
            Logger::Debug("[InputValidator::SanitizeChatMessage] Removed control character 0x%02X at position %zu", (unsigned char)c, out.size());
        }
    }
    if (removedCount > 0) {
        Logger::Debug("[InputValidator::SanitizeChatMessage] Total control characters removed: %d", removedCount);
    }
    if (out.size() > 256) {
        Logger::Debug("[InputValidator::SanitizeChatMessage] Truncating message from %zu to 256 characters", out.size());
        out.resize(256);
    }
    Logger::Info("[InputValidator::SanitizeChatMessage] Sanitized message: original_length=%zu, sanitized_length=%zu, chars_removed=%d", msg.size(), out.size(), removedCount);
    Logger::Trace("[InputValidator::SanitizeChatMessage] Exit - returning sanitized string of length %zu", out.size());
    return out;
}

bool InputValidator::IsValidCommand(const std::string& cmd, const std::vector<std::string>& args) {
    Logger::Trace("[InputValidator::IsValidCommand] Entry - cmd='%s', args_count=%zu", cmd.c_str(), args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        Logger::Trace("[InputValidator::IsValidCommand] arg[%zu]='%s'", i, args[i].c_str());
    }
    if (!std::regex_match(cmd, kCommandPattern)) {
        Logger::Debug("[InputValidator::IsValidCommand] Command name '%s' failed kCommandPattern validation", cmd.c_str());
        Logger::Trace("[InputValidator::IsValidCommand] Exit - returning false (invalid command name)");
        return false;
    }
    Logger::Debug("[InputValidator::IsValidCommand] Command name '%s' passed kCommandPattern validation", cmd.c_str());
    for (size_t i = 0; i < args.size(); ++i) {
        auto& a = args[i];
        if (a.empty()) {
            Logger::Debug("[InputValidator::IsValidCommand] Argument %zu is empty - rejecting command", i);
            Logger::Trace("[InputValidator::IsValidCommand] Exit - returning false (empty argument at index %zu)", i);
            return false;
        }
        if (!std::regex_match(a, kAlnumPattern)) {
            Logger::Debug("[InputValidator::IsValidCommand] Argument %zu ('%s') failed kAlnumPattern validation", i, a.c_str());
            Logger::Trace("[InputValidator::IsValidCommand] Exit - returning false (invalid argument at index %zu)", i);
            return false;
        }
        Logger::Debug("[InputValidator::IsValidCommand] Argument %zu ('%s') passed kAlnumPattern validation", i, a.c_str());
    }
    Logger::Info("[InputValidator::IsValidCommand] Command '%s' with %zu args validated successfully", cmd.c_str(), args.size());
    Logger::Trace("[InputValidator::IsValidCommand] Exit - returning true");
    return true;
}

bool InputValidator::IsValidPosition(float x, float y, float z) {
    Logger::Trace("[InputValidator::IsValidPosition] Entry - x=%.4f, y=%.4f, z=%.4f", x, y, z);
    Logger::Debug("[InputValidator::IsValidPosition] Checking bounds: x in [%.1f, %.1f], y in [%.1f, %.1f], z in [%.1f, %.1f]",
                  kWorldMinX, kWorldMaxX, kWorldMinY, kWorldMaxY, kWorldMinZ, kWorldMaxZ);
    bool result = x >= kWorldMinX && x <= kWorldMaxX &&
           y >= kWorldMinY && y <= kWorldMaxY &&
           z >= kWorldMinZ && z <= kWorldMaxZ;
    if (result) {
        Logger::Debug("[InputValidator::IsValidPosition] Position (%.4f, %.4f, %.4f) is within world bounds", x, y, z);
    } else {
        Logger::Debug("[InputValidator::IsValidPosition] Position (%.4f, %.4f, %.4f) is OUT OF world bounds", x, y, z);
        if (x < kWorldMinX || x > kWorldMaxX)
            Logger::Debug("[InputValidator::IsValidPosition] X=%.4f out of range [%.1f, %.1f]", x, kWorldMinX, kWorldMaxX);
        if (y < kWorldMinY || y > kWorldMaxY)
            Logger::Debug("[InputValidator::IsValidPosition] Y=%.4f out of range [%.1f, %.1f]", y, kWorldMinY, kWorldMaxY);
        if (z < kWorldMinZ || z > kWorldMaxZ)
            Logger::Debug("[InputValidator::IsValidPosition] Z=%.4f out of range [%.1f, %.1f]", z, kWorldMinZ, kWorldMaxZ);
    }
    Logger::Trace("[InputValidator::IsValidPosition] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool InputValidator::IsValidOrientation(float pitch, float yaw, float roll) {
    Logger::Trace("[InputValidator::IsValidOrientation] Entry - pitch=%.4f, yaw=%.4f, roll=%.4f", pitch, yaw, roll);
    bool result = pitch >= -180.0f && pitch <= 180.0f &&
           yaw   >= -180.0f && yaw   <= 180.0f &&
           roll  >= -180.0f && roll  <= 180.0f;
    if (result) {
        Logger::Debug("[InputValidator::IsValidOrientation] Orientation (pitch=%.4f, yaw=%.4f, roll=%.4f) is within [-180, 180] range", pitch, yaw, roll);
    } else {
        Logger::Debug("[InputValidator::IsValidOrientation] Orientation INVALID: pitch=%.4f, yaw=%.4f, roll=%.4f (allowed range: [-180, 180])", pitch, yaw, roll);
        if (pitch < -180.0f || pitch > 180.0f)
            Logger::Debug("[InputValidator::IsValidOrientation] Pitch=%.4f is out of range [-180, 180]", pitch);
        if (yaw < -180.0f || yaw > 180.0f)
            Logger::Debug("[InputValidator::IsValidOrientation] Yaw=%.4f is out of range [-180, 180]", yaw);
        if (roll < -180.0f || roll > 180.0f)
            Logger::Debug("[InputValidator::IsValidOrientation] Roll=%.4f is out of range [-180, 180]", roll);
    }
    Logger::Trace("[InputValidator::IsValidOrientation] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool InputValidator::IsValidAction(const std::string& actionTag) {
    Logger::Trace("[InputValidator::IsValidAction] Entry - actionTag='%s'", actionTag.c_str());
    // Allow only known action tags (alphanumeric + underscore)
    bool result = std::regex_match(actionTag, kAlnumPattern);
    if (result) {
        Logger::Debug("[InputValidator::IsValidAction] Action tag '%s' passed kAlnumPattern validation", actionTag.c_str());
    } else {
        Logger::Debug("[InputValidator::IsValidAction] Action tag '%s' FAILED kAlnumPattern validation", actionTag.c_str());
    }
    Logger::Trace("[InputValidator::IsValidAction] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool InputValidator::IsValidString(const std::string& s, size_t maxLength, const std::regex& allowedPattern) {
    Logger::Trace("[InputValidator::IsValidString] Entry - s='%s' (length=%zu), maxLength=%zu", s.c_str(), s.size(), maxLength);
    if (s.empty()) {
        Logger::Debug("[InputValidator::IsValidString] String is empty - validation failed");
        Logger::Trace("[InputValidator::IsValidString] Exit - returning false (empty string)");
        return false;
    }
    if (s.size() > maxLength) {
        Logger::Debug("[InputValidator::IsValidString] String length %zu exceeds maxLength %zu - validation failed", s.size(), maxLength);
        Logger::Trace("[InputValidator::IsValidString] Exit - returning false (too long)");
        return false;
    }
    bool result = std::regex_match(s, allowedPattern);
    if (result) {
        Logger::Debug("[InputValidator::IsValidString] String '%s' passed pattern validation (length=%zu, maxLength=%zu)", s.c_str(), s.size(), maxLength);
    } else {
        Logger::Debug("[InputValidator::IsValidString] String '%s' FAILED pattern validation", s.c_str());
    }
    Logger::Trace("[InputValidator::IsValidString] Exit - returning %s", result ? "true" : "false");
    return result;
}
