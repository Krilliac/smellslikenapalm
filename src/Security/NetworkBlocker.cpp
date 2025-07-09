#include "Network/NetworkBlocker.h"

NetworkBlocker::NetworkBlocker() = default;

NetworkBlocker::~NetworkBlocker() = default;

void NetworkBlocker::Block(const ClientAddress& addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockMap[addr] = Entry{ true, {} };
}

void NetworkBlocker::Block(const ClientAddress& addr, Duration duration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto expires = std::chrono::steady_clock::now() + duration;
    m_blockMap[addr] = Entry{ false, expires };
}

void NetworkBlocker::Unblock(const ClientAddress& addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockMap.erase(addr);
}

bool NetworkBlocker::IsBlocked(const ClientAddress& addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_blockMap.find(addr);
    if (it == m_blockMap.end()) {
        return false;
    }
    if (it->second.permanent) {
        return true;
    }
    // temporary: check expiry
    if (std::chrono::steady_clock::now() < it->second.expiresAt) {
        return true;
    }
    // expired
    m_blockMap.erase(it);
    return false;
}

void NetworkBlocker::Update() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    for (auto it = m_blockMap.begin(); it != m_blockMap.end(); ) {
        if (!it->second.permanent && now >= it->second.expiresAt) {
            it = m_blockMap.erase(it);
        } else {
            ++it;
        }
    }
}