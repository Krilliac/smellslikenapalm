#include "Security/NetworkBlocker.h"
#include "Utils/Logger.h"

NetworkBlocker::NetworkBlocker() {
    Logger::Trace("[NetworkBlocker::NetworkBlocker] Default constructor called");
}

NetworkBlocker::~NetworkBlocker() {
    Logger::Trace("[NetworkBlocker::~NetworkBlocker] Destructor called, %zu entries in block map", m_blockMap.size());
}

void NetworkBlocker::Block(const ClientAddress& addr) {
    Logger::Trace("[NetworkBlocker::Block] Entry (permanent), addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockMap[addr] = Entry{ true, {} };
    Logger::Info("[NetworkBlocker::Block] Permanently blocked address %s:%u, total blocks=%zu",
                 addr.ip.c_str(), addr.port, m_blockMap.size());
    Logger::Debug("[NetworkBlocker::Block] Block entry created: permanent=true for %s:%u", addr.ip.c_str(), addr.port);
    Logger::Trace("[NetworkBlocker::Block] Exit");
}

void NetworkBlocker::Block(const ClientAddress& addr, Duration duration) {
    Logger::Trace("[NetworkBlocker::Block] Entry (timed), addr=%s:%u, duration=%lld seconds",
                  addr.ip.c_str(), addr.port,
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(duration).count()));
    std::lock_guard<std::mutex> lock(m_mutex);
    auto expires = std::chrono::steady_clock::now() + duration;
    m_blockMap[addr] = Entry{ false, expires };
    Logger::Info("[NetworkBlocker::Block] Temporarily blocked address %s:%u for %lld seconds, total blocks=%zu",
                 addr.ip.c_str(), addr.port,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(duration).count()),
                 m_blockMap.size());
    Logger::Debug("[NetworkBlocker::Block] Block entry created: permanent=false, duration=%lld seconds for %s:%u",
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(duration).count()),
                  addr.ip.c_str(), addr.port);
    Logger::Trace("[NetworkBlocker::Block] Exit");
}

void NetworkBlocker::Unblock(const ClientAddress& addr) {
    Logger::Trace("[NetworkBlocker::Unblock] Entry, addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto erased = m_blockMap.erase(addr);
    if (erased > 0) {
        Logger::Info("[NetworkBlocker::Unblock] Unblocked address %s:%u, total blocks=%zu",
                     addr.ip.c_str(), addr.port, m_blockMap.size());
    } else {
        Logger::Debug("[NetworkBlocker::Unblock] Address %s:%u was not in block map, nothing to unblock",
                      addr.ip.c_str(), addr.port);
    }
    Logger::Trace("[NetworkBlocker::Unblock] Exit");
}

bool NetworkBlocker::IsBlocked(const ClientAddress& addr) {
    Logger::Trace("[NetworkBlocker::IsBlocked] Entry, addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_blockMap.find(addr);
    if (it == m_blockMap.end()) {
        Logger::Debug("[NetworkBlocker::IsBlocked] Address %s:%u not found in block map - not blocked",
                      addr.ip.c_str(), addr.port);
        Logger::Trace("[NetworkBlocker::IsBlocked] Exit, returning false");
        return false;
    }
    if (it->second.permanent) {
        Logger::Debug("[NetworkBlocker::IsBlocked] Address %s:%u is permanently blocked",
                      addr.ip.c_str(), addr.port);
        Logger::Info("[NetworkBlocker::IsBlocked] Block check: %s:%u is PERMANENTLY blocked",
                     addr.ip.c_str(), addr.port);
        Logger::Trace("[NetworkBlocker::IsBlocked] Exit, returning true (permanent)");
        return true;
    }
    // temporary: check expiry
    if (std::chrono::steady_clock::now() < it->second.expiresAt) {
        Logger::Debug("[NetworkBlocker::IsBlocked] Address %s:%u is temporarily blocked and block has not expired",
                      addr.ip.c_str(), addr.port);
        Logger::Trace("[NetworkBlocker::IsBlocked] Exit, returning true (temporary, active)");
        return true;
    }
    // expired
    Logger::Debug("[NetworkBlocker::IsBlocked] Temporary block for %s:%u has expired, removing entry",
                  addr.ip.c_str(), addr.port);
    Logger::Info("[NetworkBlocker::IsBlocked] Expired temporary block removed for %s:%u",
                 addr.ip.c_str(), addr.port);
    m_blockMap.erase(it);
    Logger::Trace("[NetworkBlocker::IsBlocked] Exit, returning false (expired)");
    return false;
}

void NetworkBlocker::Update() {
    Logger::Trace("[NetworkBlocker::Update] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    size_t removedCount = 0;
    for (auto it = m_blockMap.begin(); it != m_blockMap.end(); ) {
        if (!it->second.permanent && now >= it->second.expiresAt) {
            Logger::Debug("[NetworkBlocker::Update] Removing expired temporary block for %s:%u",
                          it->first.ip.c_str(), it->first.port);
            it = m_blockMap.erase(it);
            ++removedCount;
        } else {
            ++it;
        }
    }
    if (removedCount > 0) {
        Logger::Info("[NetworkBlocker::Update] Cleaned up %zu expired blocks, %zu remaining",
                     removedCount, m_blockMap.size());
    } else {
        Logger::Trace("[NetworkBlocker::Update] No expired blocks to clean up");
    }
    Logger::Trace("[NetworkBlocker::Update] Exit, total blocks=%zu", m_blockMap.size());
}

bool NetworkBlocker::Initialize() {
    Logger::Trace("[NetworkBlocker::Initialize] Entry");
    m_blockMap.clear();
    m_blockedDomains.clear();
    Logger::Info("[NetworkBlocker::Initialize] NetworkBlocker initialized, block map and domain list cleared");
    Logger::Debug("[NetworkBlocker::Initialize] Block map size=%zu, blocked domains size=%zu",
                  m_blockMap.size(), m_blockedDomains.size());
    Logger::Trace("[NetworkBlocker::Initialize] Exit, returning true");
    return true;
}

void NetworkBlocker::BlockDomain(const std::string& domain) {
    Logger::Trace("[NetworkBlocker::BlockDomain] Entry, domain='%s'", domain.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockedDomains.push_back(domain);
    Logger::Info("[NetworkBlocker::BlockDomain] Domain '%s' added to block list, total blocked domains=%zu",
                 domain.c_str(), m_blockedDomains.size());
    Logger::Trace("[NetworkBlocker::BlockDomain] Exit");
}
