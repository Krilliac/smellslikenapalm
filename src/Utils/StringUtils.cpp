#include "Utils/StringUtils.h"

namespace StringUtils {

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> Split(const std::string& s, char delim, bool keepEmpty) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(s);
    while (std::getline(iss, token, delim)) {
        if (token.empty() && !keepEmpty) continue;
        tokens.push_back(token);
    }
    return tokens;
}

std::string Join(const std::vector<std::string>& parts, const std::string& delim) {
    if (parts.empty()) return "";
    std::ostringstream oss;
    auto it = parts.begin();
    oss << *it++;
    for (; it != parts.end(); ++it) {
        oss << delim << *it;
    }
    return oss.str();
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

std::string ToUpper(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return ToLower(a) == ToLower(b);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ReplaceAll(const std::string& s,
                       const std::string& from,
                       const std::string& to)
{
    if (from.empty()) return s;
    std::string result = s;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

std::optional<int> ToInt(const std::string& s) {
    try {
        size_t idx = 0;
        int v = std::stoi(s, &idx);
        return idx == s.size() ? v : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> ToDouble(const std::string& s) {
    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        return idx == s.size() ? v : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

void TrimInPlace(std::vector<std::string>& parts) {
    for (auto& p : parts) {
        p = Trim(p);
    }
}

std::string RemoveWhitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(c);
        }
    }
    return out;
}