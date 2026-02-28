// src/Network/NetworkUtils.cpp

#include "Network/NetworkUtils.h"
#include "Utils/Logger.h"
#include <sstream>
#include <regex>
#include <algorithm>
#include <arpa/inet.h>

namespace NetworkUtils {

    std::optional<std::string> NormalizeIPv4(const std::string& ip) {
        Logger::Trace("[NetworkUtils::NormalizeIPv4] Entry: ip='%s'", ip.c_str());
        std::vector<std::string> parts = SplitString(ip, '.');
        Logger::Debug("[NetworkUtils::NormalizeIPv4] Split IP into %zu parts", parts.size());
        if (parts.size() != 4) {
            Logger::Debug("[NetworkUtils::NormalizeIPv4] Invalid part count %zu (expected 4), returning nullopt", parts.size());
            Logger::Trace("[NetworkUtils::NormalizeIPv4] Exit: returning nullopt");
            return std::nullopt;
        }
        for (size_t idx = 0; idx < parts.size(); ++idx) {
            auto& part = parts[idx];
            Logger::Trace("[NetworkUtils::NormalizeIPv4] Processing part[%zu]='%s'", idx, part.c_str());
            // Reject leading zeros beyond "0"
            if (part.size() > 1 && part[0] == '0') {
                Logger::Debug("[NetworkUtils::NormalizeIPv4] Part[%zu] has leading zeros, stripping: '%s'", idx, part.c_str());
                part.erase(0, part.find_first_not_of('0'));
            }
            if (part.empty()) {
                part = "0";
                Logger::Debug("[NetworkUtils::NormalizeIPv4] Part[%zu] was empty after stripping, set to '0'", idx);
            }
            int v = std::stoi(part);
            Logger::Trace("[NetworkUtils::NormalizeIPv4] Part[%zu] parsed value=%d", idx, v);
            if (v < 0 || v > 255) {
                Logger::Debug("[NetworkUtils::NormalizeIPv4] Part[%zu] value %d out of range [0,255], returning nullopt", idx, v);
                Logger::Trace("[NetworkUtils::NormalizeIPv4] Exit: returning nullopt (out of range)");
                return std::nullopt;
            }
            part = std::to_string(v);
            Logger::Trace("[NetworkUtils::NormalizeIPv4] Part[%zu] normalized to '%s'", idx, part.c_str());
        }
        auto result = JoinString(parts, '.');
        Logger::Debug("[NetworkUtils::NormalizeIPv4] Normalized result: '%s'", result.c_str());
        Logger::Trace("[NetworkUtils::NormalizeIPv4] Exit: returning '%s'", result.c_str());
        return result;
    }

    bool IsValidIPv4(const std::string& ip) {
        Logger::Trace("[NetworkUtils::IsValidIPv4] Entry: ip='%s'", ip.c_str());
        static const std::regex ipv4Pattern(R"(^((25[0-5]|2[0-4]\d|[01]?\d?\d)\.){3}(25[0-5]|2[0-4]\d|[01]?\d?\d)$)");
        bool valid = std::regex_match(ip, ipv4Pattern);
        Logger::Debug("[NetworkUtils::IsValidIPv4] Regex match result for '%s': %s", ip.c_str(), valid ? "true" : "false");
        Logger::Trace("[NetworkUtils::IsValidIPv4] Exit: returning %s", valid ? "true" : "false");
        return valid;
    }

    std::vector<std::string> SplitString(const std::string& s, char delimiter) {
        Logger::Trace("[NetworkUtils::SplitString] Entry: s='%s', delimiter='%c'", s.c_str(), delimiter);
        std::vector<std::string> tokens;
        std::istringstream iss(s);
        std::string token;
        while (std::getline(iss, token, delimiter)) {
            tokens.push_back(token);
            Logger::Trace("[NetworkUtils::SplitString] Extracted token[%zu]='%s'", tokens.size() - 1, token.c_str());
        }
        Logger::Debug("[NetworkUtils::SplitString] Split '%s' into %zu tokens", s.c_str(), tokens.size());
        Logger::Trace("[NetworkUtils::SplitString] Exit: returning %zu tokens", tokens.size());
        return tokens;
    }

    std::string JoinString(const std::vector<std::string>& tokens, char delimiter) {
        Logger::Trace("[NetworkUtils::JoinString] Entry: %zu tokens, delimiter='%c'", tokens.size(), delimiter);
        std::ostringstream oss;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i) oss << delimiter;
            oss << tokens[i];
        }
        std::string result = oss.str();
        Logger::Debug("[NetworkUtils::JoinString] Joined %zu tokens into '%s'", tokens.size(), result.c_str());
        Logger::Trace("[NetworkUtils::JoinString] Exit: returning '%s'", result.c_str());
        return result;
    }

    uint16_t HostToNetworkPort(uint16_t port) {
        Logger::Trace("[NetworkUtils::HostToNetworkPort] Entry: port=%u", port);
        uint16_t result = htons(port);
        Logger::Debug("[NetworkUtils::HostToNetworkPort] Converted host port %u to network order %u", port, result);
        Logger::Trace("[NetworkUtils::HostToNetworkPort] Exit: returning %u", result);
        return result;
    }

    uint16_t NetworkToHostPort(uint16_t port) {
        Logger::Trace("[NetworkUtils::NetworkToHostPort] Entry: port=%u", port);
        uint16_t result = ntohs(port);
        Logger::Debug("[NetworkUtils::NetworkToHostPort] Converted network port %u to host order %u", port, result);
        Logger::Trace("[NetworkUtils::NetworkToHostPort] Exit: returning %u", result);
        return result;
    }

    uint8_t ComputeChecksum(const uint8_t* data, size_t length) {
        Logger::Trace("[NetworkUtils::ComputeChecksum] Entry: data=%p, length=%zu", (const void*)data, length);
        uint8_t chk = 0;
        for (size_t i = 0; i < length; ++i) {
            chk ^= data[i];
        }
        Logger::Debug("[NetworkUtils::ComputeChecksum] Computed XOR checksum over %zu bytes: 0x%02X", length, chk);
        Logger::Trace("[NetworkUtils::ComputeChecksum] Exit: returning 0x%02X", chk);
        return chk;
    }

}
