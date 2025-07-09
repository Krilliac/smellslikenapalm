// src/Network/NetworkUtils.cpp

#include "Network/NetworkUtils.h"
#include <sstream>
#include <regex>
#include <algorithm>
#include <arpa/inet.h>

namespace NetworkUtils {

    std::optional<std::string> NormalizeIPv4(const std::string& ip) {
        std::vector<std::string> parts = SplitString(ip, '.');
        if (parts.size() != 4) return std::nullopt;
        for (auto& part : parts) {
            // Reject leading zeros beyond "0"
            if (part.size() > 1 && part[0] == '0') part.erase(0, part.find_first_not_of('0'));
            if (part.empty()) part = "0";
            int v = std::stoi(part);
            if (v < 0 || v > 255) return std::nullopt;
            part = std::to_string(v);
        }
        return JoinString(parts, '.');
    }

    bool IsValidIPv4(const std::string& ip) {
        static const std::regex ipv4Pattern(R"(^((25[0-5]|2[0-4]\d|[01]?\d?\d)\.){3}(25[0-5]|2[0-4]\d|[01]?\d?\d)$)");
        return std::regex_match(ip, ipv4Pattern);
    }

    std::vector<std::string> SplitString(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::istringstream iss(s);
        std::string token;
        while (std::getline(iss, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    std::string JoinString(const std::vector<std::string>& tokens, char delimiter) {
        std::ostringstream oss;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i) oss << delimiter;
            oss << tokens[i];
        }
        return oss.str();
    }

    uint16_t HostToNetworkPort(uint16_t port) {
        return htons(port);
    }

    uint16_t NetworkToHostPort(uint16_t port) {
        return ntohs(port);
    }

    uint8_t ComputeChecksum(const uint8_t* data, size_t length) {
        uint8_t chk = 0;
        for (size_t i = 0; i < length; ++i) {
            chk ^= data[i];
        }
        return chk;
    }

}