// src/Network/URLOptions.cpp
//
// Implementation of the UE3 FURL-style connect/options parser. See
// URLOptions.h for the semantics this mirrors from Engine/GameInfo.uc.

#include "Network/URLOptions.h"

#include <cctype>

namespace {

// Case-insensitive ASCII string equality, matching UnrealScript's '=='
// behavior for option key comparisons.
bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

// UnrealScript int(string): parse a leading optional sign + decimal digits,
// ignoring trailing junk; empty/non-numeric -> 0.
int UScriptInt(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    // Skip leading whitespace (defensive; UE3 strings here are not padded).
    while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    bool negative = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        negative = (s[i] == '-');
        ++i;
    }
    long long value = 0;
    bool anyDigit = false;
    while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
        value = value * 10 + (s[i] - '0');
        anyDigit = true;
        ++i;
    }
    if (!anyDigit) {
        return 0;
    }
    if (negative) {
        value = -value;
    }
    return static_cast<int>(value);
}

} // namespace

URLOptions URLOptions::Parse(const std::string& url) {
    URLOptions out;

    // Split off the map / URL portion: everything before the first '?'.
    const size_t firstQ = url.find('?');
    if (firstQ == std::string::npos) {
        out.m_map = url;
        return out;
    }
    out.m_map = url.substr(0, firstQ);

    // Iterate the '?'-delimited option pairs, mirroring GrabOption: each option
    // runs from after a '?' up to the next '?' (or end of string).
    size_t pos = firstQ; // points at the current '?'
    while (pos != std::string::npos) {
        const size_t start = pos + 1;
        const size_t next = url.find('?', start);
        const size_t end = (next == std::string::npos) ? url.size() : next;

        // Extract the raw pair; skip empty segments (e.g. "Map??Foo").
        if (end > start) {
            const std::string pair = url.substr(start, end - start);

            // GetKeyValue: split on the first '='. No '=' -> bare flag.
            const size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                out.m_options.emplace_back(pair, std::string());
            } else {
                out.m_options.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
            }
        }

        pos = next;
    }

    return out;
}

bool URLOptions::Has(const std::string& key) const {
    for (const auto& kv : m_options) {
        if (IEquals(kv.first, key)) {
            return true;
        }
    }
    return false;
}

std::string URLOptions::Get(const std::string& key, const std::string& def) const {
    // First match wins (ParseOption returns on first match).
    for (const auto& kv : m_options) {
        if (IEquals(kv.first, key)) {
            return kv.second;
        }
    }
    return def;
}

int URLOptions::GetInt(const std::string& key, int def) const {
    // GetIntOption: return def when the key is absent, otherwise int(value).
    for (const auto& kv : m_options) {
        if (IEquals(kv.first, key)) {
            return UScriptInt(kv.second);
        }
    }
    return def;
}

bool URLOptions::GetBool(const std::string& key, bool def) const {
    for (const auto& kv : m_options) {
        if (IEquals(kv.first, key)) {
            // "1" is true (UE3 SpectatorOnly==1). A bare flag (empty value)
            // is treated as present/true.
            return kv.second.empty() || kv.second == "1";
        }
    }
    return def;
}
