#include "Utils/StringUtils.h"
#include "Utils/Logger.h"

namespace StringUtils {

std::string Trim(const std::string& s) {
    Logger::Trace("[StringUtils::Trim] Entry: s='%s' (length=%zu)", s.c_str(), s.size());
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        Logger::Debug("[StringUtils::Trim] String is entirely whitespace, returning empty");
        Logger::Trace("[StringUtils::Trim] Exit: returning ''");
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string result = s.substr(start, end - start + 1);
    Logger::Trace("[StringUtils::Trim] Exit: returning '%s'", result.c_str());
    return result;
}

std::vector<std::string> Split(const std::string& s, char delim, bool keepEmpty) {
    Logger::Trace("[StringUtils::Split] Entry: s='%s', delim='%c', keepEmpty=%s",
                  s.c_str(), delim, keepEmpty ? "true" : "false");
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(s);
    while (std::getline(iss, token, delim)) {
        if (token.empty() && !keepEmpty) {
            Logger::Debug("[StringUtils::Split] Skipping empty token (keepEmpty=false)");
            continue;
        }
        tokens.push_back(token);
    }
    Logger::Debug("[StringUtils::Split] Produced %zu tokens", tokens.size());
    Logger::Trace("[StringUtils::Split] Exit: returning %zu tokens", tokens.size());
    return tokens;
}

std::string Join(const std::vector<std::string>& parts, const std::string& delim) {
    Logger::Trace("[StringUtils::Join] Entry: parts.size()=%zu, delim='%s'", parts.size(), delim.c_str());
    if (parts.empty()) {
        Logger::Debug("[StringUtils::Join] Parts vector is empty, returning empty string");
        Logger::Trace("[StringUtils::Join] Exit: returning ''");
        return "";
    }
    std::ostringstream oss;
    auto it = parts.begin();
    oss << *it++;
    for (; it != parts.end(); ++it) {
        oss << delim << *it;
    }
    std::string result = oss.str();
    Logger::Trace("[StringUtils::Join] Exit: returning joined string (length=%zu)", result.size());
    return result;
}

std::string ToLower(const std::string& s) {
    Logger::Trace("[StringUtils::ToLower] Entry: s='%s'", s.c_str());
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    Logger::Trace("[StringUtils::ToLower] Exit: returning '%s'", out.c_str());
    return out;
}

std::string ToUpper(const std::string& s) {
    Logger::Trace("[StringUtils::ToUpper] Entry: s='%s'", s.c_str());
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    Logger::Trace("[StringUtils::ToUpper] Exit: returning '%s'", out.c_str());
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    Logger::Trace("[StringUtils::EqualsIgnoreCase] Entry: a='%s', b='%s'", a.c_str(), b.c_str());
    bool result = ToLower(a) == ToLower(b);
    Logger::Debug("[StringUtils::EqualsIgnoreCase] Comparison result: %s", result ? "true" : "false");
    Logger::Trace("[StringUtils::EqualsIgnoreCase] Exit: returning %s", result ? "true" : "false");
    return result;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    Logger::Trace("[StringUtils::StartsWith] Entry: s='%s', prefix='%s'", s.c_str(), prefix.c_str());
    bool result = s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    Logger::Debug("[StringUtils::StartsWith] Result: %s", result ? "true" : "false");
    Logger::Trace("[StringUtils::StartsWith] Exit: returning %s", result ? "true" : "false");
    return result;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    Logger::Trace("[StringUtils::EndsWith] Entry: s='%s', suffix='%s'", s.c_str(), suffix.c_str());
    bool result = s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    Logger::Debug("[StringUtils::EndsWith] Result: %s", result ? "true" : "false");
    Logger::Trace("[StringUtils::EndsWith] Exit: returning %s", result ? "true" : "false");
    return result;
}

std::string ReplaceAll(const std::string& s,
                       const std::string& from,
                       const std::string& to)
{
    Logger::Trace("[StringUtils::ReplaceAll] Entry: s.length()=%zu, from='%s', to='%s'",
                  s.size(), from.c_str(), to.c_str());
    if (from.empty()) {
        Logger::Debug("[StringUtils::ReplaceAll] 'from' string is empty, returning original");
        Logger::Trace("[StringUtils::ReplaceAll] Exit: returning original string");
        return s;
    }
    std::string result = s;
    size_t pos = 0;
    size_t replacements = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
        replacements++;
    }
    Logger::Debug("[StringUtils::ReplaceAll] Performed %zu replacements", replacements);
    Logger::Trace("[StringUtils::ReplaceAll] Exit: returning result (length=%zu)", result.size());
    return result;
}

std::optional<int> ToInt(const std::string& s) {
    Logger::Trace("[StringUtils::ToInt] Entry: s='%s'", s.c_str());
    try {
        size_t idx = 0;
        int v = std::stoi(s, &idx);
        if (idx == s.size()) {
            Logger::Debug("[StringUtils::ToInt] Successfully parsed integer: %d", v);
            Logger::Trace("[StringUtils::ToInt] Exit: returning %d", v);
            return v;
        }
        Logger::Debug("[StringUtils::ToInt] Partial parse: consumed %zu of %zu chars", idx, s.size());
        Logger::Trace("[StringUtils::ToInt] Exit: returning nullopt (partial parse)");
        return std::nullopt;
    } catch (const std::exception& ex) {
        Logger::Error("[StringUtils::ToInt] Exception parsing '%s': %s", s.c_str(), ex.what());
        Logger::Trace("[StringUtils::ToInt] Exit: returning nullopt (exception)");
        return std::nullopt;
    }
}

std::optional<double> ToDouble(const std::string& s) {
    Logger::Trace("[StringUtils::ToDouble] Entry: s='%s'", s.c_str());
    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        if (idx == s.size()) {
            Logger::Debug("[StringUtils::ToDouble] Successfully parsed double: %.6f", v);
            Logger::Trace("[StringUtils::ToDouble] Exit: returning %.6f", v);
            return v;
        }
        Logger::Debug("[StringUtils::ToDouble] Partial parse: consumed %zu of %zu chars", idx, s.size());
        Logger::Trace("[StringUtils::ToDouble] Exit: returning nullopt (partial parse)");
        return std::nullopt;
    } catch (const std::exception& ex) {
        Logger::Error("[StringUtils::ToDouble] Exception parsing '%s': %s", s.c_str(), ex.what());
        Logger::Trace("[StringUtils::ToDouble] Exit: returning nullopt (exception)");
        return std::nullopt;
    }
}

void TrimInPlace(std::vector<std::string>& parts) {
    Logger::Trace("[StringUtils::TrimInPlace] Entry: parts.size()=%zu", parts.size());
    for (auto& p : parts) {
        p = Trim(p);
    }
    Logger::Debug("[StringUtils::TrimInPlace] Trimmed %zu parts in place", parts.size());
    Logger::Trace("[StringUtils::TrimInPlace] Exit");
}

std::string RemoveWhitespace(const std::string& s) {
    Logger::Trace("[StringUtils::RemoveWhitespace] Entry: s.length()=%zu", s.size());
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(c);
        }
    }
    Logger::Debug("[StringUtils::RemoveWhitespace] Removed %zu whitespace characters", s.size() - out.size());
    Logger::Trace("[StringUtils::RemoveWhitespace] Exit: returning string (length=%zu)", out.size());
    return out;
}

bool ToBool(const std::string& s, bool defaultValue) {
    Logger::Trace("[StringUtils::ToBool] Entry: s='%s', defaultValue=%s", s.c_str(), defaultValue ? "true" : "false");
    std::string lower = ToLower(Trim(s));
    if (lower == "true" || lower == "yes" || lower == "1" || lower == "on") {
        Logger::Debug("[StringUtils::ToBool] Parsed as true (value='%s')", lower.c_str());
        Logger::Trace("[StringUtils::ToBool] Exit: returning true");
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "0" || lower == "off") {
        Logger::Debug("[StringUtils::ToBool] Parsed as false (value='%s')", lower.c_str());
        Logger::Trace("[StringUtils::ToBool] Exit: returning false");
        return false;
    }
    Logger::Debug("[StringUtils::ToBool] Unrecognized value '%s', using default=%s", lower.c_str(), defaultValue ? "true" : "false");
    Logger::Trace("[StringUtils::ToBool] Exit: returning %s (default)", defaultValue ? "true" : "false");
    return defaultValue;
}

} // namespace StringUtils
