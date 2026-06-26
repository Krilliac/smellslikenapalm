// src/Network/URLOptions.cpp
//
// Implementation of the UE3 FURL-style connect/options parser. See
// URLOptions.h for the semantics this mirrors from Engine/GameInfo.uc.

#include "Network/URLOptions.h"
#include "Utils/Logger.h"

#include <cctype>
#include <climits>

namespace {

// Defensive bounds for the attacker-controlled connect string. These are far
// larger than any legitimate UE3 FURL (map path + a handful of Key=Value login
// options is well under 1 KiB and a dozen options), so valid URLs parse
// byte-identically; they exist only to cap memory/work on hostile input.
constexpr size_t kMaxURLLength = 8192;   // reject absurdly long connect strings
constexpr size_t kMaxOptions   = 256;    // cap '?'-delimited option count

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
    // Saturate accumulation: a long digit run (attacker-supplied) would overflow
    // long long, which is signed-overflow UB. Stop growing well past the int
    // range; in-range values (the only ones a valid URL carries) are unaffected.
    constexpr long long kAccumCap = 100000000000LL; // 1e11, > INT_MAX, safe from ll overflow
    while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (value < kAccumCap) {
            value = value * 10 + (s[i] - '0');
        }
        anyDigit = true;
        ++i;
    }
    if (!anyDigit) {
        return 0;
    }
    if (negative) {
        value = -value;
    }
    // Clamp out-of-range results instead of relying on implementation-defined
    // narrowing. Values that fit in int are returned exactly (no behavior change).
    if (value > INT_MAX) {
        value = INT_MAX;
    } else if (value < INT_MIN) {
        value = INT_MIN;
    }
    return static_cast<int>(value);
}

} // namespace

URLOptions URLOptions::Parse(const std::string& url) {
    URLOptions out;

    // Reject hostile, oversized connect strings before doing any per-character
    // work. A valid FURL is well under this bound, so this never trips on a
    // legitimate join; an over-length string yields an empty (no map, no
    // options) result, which downstream login handling already treats as
    // "no usable options".
    if (url.size() > kMaxURLLength) {
        Logger::Warn("[URLOptions::Parse] Rejecting oversized connect string (%zu bytes > %zu cap)",
                     url.size(), kMaxURLLength);
        return out;
    }

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
            // Cap the number of retained options. A flood of '?' separators
            // (injected by an attacker) cannot grow the vector without bound;
            // a valid login carries only a handful of options, so this is never
            // reached on legitimate input. First-match-wins lookup semantics are
            // preserved because we keep the earliest options.
            if (out.m_options.size() >= kMaxOptions) {
                Logger::Warn("[URLOptions::Parse] Option count hit cap (%zu); ignoring remaining options",
                             kMaxOptions);
                break;
            }
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
