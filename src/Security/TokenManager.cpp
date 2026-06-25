// src/Security/TokenManager.cpp

#include "Security/TokenManager.h"
#include "Utils/CryptoUtils.h"
#include "Utils/Logger.h"

#include <vector>

namespace {
// 32 random bytes -> URL-safe-ish base64 token. CryptoUtils::GenerateRandomBytes
// uses OpenSSL's CSPRNG when available and a documented non-crypto fallback
// otherwise (logged loudly by CryptoUtils itself).
constexpr size_t kTokenBytes = 32;
}

TokenManager::TokenManager(std::chrono::seconds tokenLifetime)
    : m_lifetime(tokenLifetime) {
    Logger::Trace("[TokenManager::TokenManager] Entry: lifetime=%llds",
                  static_cast<long long>(tokenLifetime.count()));
    Logger::Info("[TokenManager] Initialized (token lifetime=%llds, 0=never expires)",
                 static_cast<long long>(tokenLifetime.count()));
    Logger::Trace("[TokenManager::TokenManager] Exit");
}

TokenManager::~TokenManager() {
    Logger::Trace("[TokenManager::~TokenManager] Entry");
    Logger::Trace("[TokenManager::~TokenManager] Exit");
}

bool TokenManager::IsExpired(const Entry& e) const {
    if (!e.hasExpiry) return false;
    return std::chrono::steady_clock::now() > e.expiry;
}

std::string TokenManager::GenerateToken(const std::string& user) {
    Logger::Trace("[TokenManager::GenerateToken] Entry: user='%s'", user.c_str());

    std::string token;
    // Defend against the astronomically-unlikely collision so we never alias two
    // users onto one token.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        do {
            std::vector<uint8_t> raw = CryptoUtils::GenerateRandomBytes(kTokenBytes);
            token = CryptoUtils::Base64Encode(raw);
        } while (token.empty() || m_tokens.find(token) != m_tokens.end());

        Entry e;
        e.user = user;
        e.hasExpiry = (m_lifetime.count() > 0);
        if (e.hasExpiry) {
            e.expiry = std::chrono::steady_clock::now() + m_lifetime;
        }
        m_tokens.emplace(token, std::move(e));
    }

    Logger::Debug("[TokenManager::GenerateToken] Issued token for user='%s' (len=%zu)",
                  user.c_str(), token.size());
    Logger::Trace("[TokenManager::GenerateToken] Exit");
    return token;
}

bool TokenManager::ValidateToken(const std::string& user, const std::string& token) {
    Logger::Trace("[TokenManager::ValidateToken] Entry: user='%s'", user.c_str());

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tokens.find(token);
    if (it == m_tokens.end()) {
        Logger::Debug("[TokenManager::ValidateToken] Token not found");
        return false;
    }
    if (IsExpired(it->second)) {
        Logger::Debug("[TokenManager::ValidateToken] Token expired, purging");
        m_tokens.erase(it);
        return false;
    }
    if (it->second.user != user) {
        Logger::Warn("[TokenManager::ValidateToken] Token presented by wrong user "
                     "(issued for '%s', presented by '%s')",
                     it->second.user.c_str(), user.c_str());
        return false;
    }
    Logger::Trace("[TokenManager::ValidateToken] Exit: valid");
    return true;
}

void TokenManager::RevokeToken(const std::string& token) {
    Logger::Trace("[TokenManager::RevokeToken] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tokens.erase(token);
    Logger::Trace("[TokenManager::RevokeToken] Exit");
}

void TokenManager::RevokeUser(const std::string& user) {
    Logger::Trace("[TokenManager::RevokeUser] Entry: user='%s'", user.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->second.user == user) it = m_tokens.erase(it);
        else ++it;
    }
    Logger::Trace("[TokenManager::RevokeUser] Exit");
}

size_t TokenManager::ActiveTokenCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tokens.size();
}
