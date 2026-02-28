#include "Time/TimeSync.h"
#include "Utils/Logger.h"
#include <cstring>

TimeSync::TimeSync(TimeSyncCallback cb)
    : m_callback(std::move(cb))
{
    Logger::Trace("[TimeSync::TimeSync] Constructor entered — callback %s provided",
                  m_callback ? "was" : "was NOT");
    Logger::Info("[TimeSync::TimeSync] TimeSync instance created");
}

TimeSync::~TimeSync() {
    Logger::Trace("[TimeSync::~TimeSync] Destructor entered");
    Logger::Info("[TimeSync::~TimeSync] TimeSync instance destroyed");
}

uint32_t TimeSync::SendTimeSyncRequest(uint32_t clientId,
                                       const ClientAddress& addr,
                                       std::function<void(const ClientAddress&, const std::vector<uint8_t>&)> sender)
{
    Logger::Trace("[TimeSync::SendTimeSyncRequest] Entry — clientId=%u, addr=%s:%u",
                  clientId, addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_clients[clientId];
    uint32_t id = info.nextId++;
    info.outstanding[id] = Clock::now();
    Logger::Debug("[TimeSync::SendTimeSyncRequest] Created sync request id=%u for clientId=%u, total outstanding for this client: %zu",
                  id, clientId, info.outstanding.size());

    // Build payload: [requestId:uint32]
    std::vector<uint8_t> buf(4);
    std::memcpy(buf.data(), &id, 4);
    Logger::Debug("[TimeSync::SendTimeSyncRequest] Built payload of %zu bytes with requestId=%u, invoking sender to addr=%s:%u",
                  buf.size(), id, addr.ip.c_str(), addr.port);
    sender(addr, buf);
    Logger::Info("[TimeSync::SendTimeSyncRequest] Time sync request sent — requestId=%u, clientId=%u, addr=%s:%u",
                 id, clientId, addr.ip.c_str(), addr.port);
    Logger::Trace("[TimeSync::SendTimeSyncRequest] Exit — returning requestId=%u", id);
    return id;
}

std::vector<uint8_t> TimeSync::HandleRequest(const std::vector<uint8_t>& payload)
{
    Logger::Trace("[TimeSync::HandleRequest] Entry — payload size=%zu bytes", payload.size());
    // Extract requestId
    if (payload.size() < 4) {
        Logger::Error("[TimeSync::HandleRequest] Payload too small: expected at least 4 bytes, got %zu bytes — returning empty response",
                      payload.size());
        Logger::Trace("[TimeSync::HandleRequest] Exit — returning empty vector due to undersized payload");
        return {};
    }
    uint32_t id;
    std::memcpy(&id, payload.data(), 4);
    Logger::Debug("[TimeSync::HandleRequest] Extracted requestId=%u from payload", id);
    // Build response: [requestId:uint32][serverTime:int64]
    auto now = std::chrono::duration_cast<Millis>(Clock::now().time_since_epoch()).count();
    Logger::Debug("[TimeSync::HandleRequest] Current server time (epoch ms): %lld", static_cast<long long>(now));
    std::vector<uint8_t> resp(4 + 8);
    std::memcpy(resp.data(), &id, 4);
    WriteInt64(resp, now);
    Logger::Debug("[TimeSync::HandleRequest] Built response of %zu bytes — requestId=%u, serverTime=%lld ms",
                  resp.size(), id, static_cast<long long>(now));
    Logger::Info("[TimeSync::HandleRequest] Time sync request handled — requestId=%u, serverTime=%lld ms",
                 id, static_cast<long long>(now));
    Logger::Trace("[TimeSync::HandleRequest] Exit — returning response of %zu bytes", resp.size());
    return resp;
}

void TimeSync::HandleResponse(uint32_t clientId,
                              const ClientAddress& addr,
                              const std::vector<uint8_t>& payload)
{
    Logger::Trace("[TimeSync::HandleResponse] Entry — clientId=%u, addr=%s:%u, payload size=%zu bytes",
                  clientId, addr.ip.c_str(), addr.port, payload.size());
    // payload: [id:uint32][clientSend:int64][serverRecv:int64]
    if (payload.size() < 4 + 8 + 8) {
        Logger::Error("[TimeSync::HandleResponse] Payload too small: expected at least %d bytes, got %zu bytes — ignoring response",
                      4 + 8 + 8, payload.size());
        Logger::Trace("[TimeSync::HandleResponse] Exit — returning early due to undersized payload");
        return;
    }
    size_t off = 0;
    uint32_t id = 0;
    std::memcpy(&id, payload.data()+off, 4);
    off += 4;
    int64_t clientTime = ReadInt64(payload, off);
    int64_t serverRecv = ReadInt64(payload, off);
    Logger::Debug("[TimeSync::HandleResponse] Parsed payload — requestId=%u, clientTime=%lld ms, serverRecv=%lld ms",
                  id, static_cast<long long>(clientTime), static_cast<long long>(serverRecv));

    Clock::time_point sendTime;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_clients.find(clientId);
        if (it==m_clients.end()) {
            Logger::Warn("[TimeSync::HandleResponse] No client entry found for clientId=%u, ignoring response", clientId);
            Logger::Trace("[TimeSync::HandleResponse] Exit — early return, unknown clientId");
            return;
        }
        auto oit = it->second.outstanding.find(id);
        if (oit==it->second.outstanding.end()) {
            Logger::Warn("[TimeSync::HandleResponse] No outstanding request found with id=%u for clientId=%u (may have expired or been duplicated)",
                         id, clientId);
            Logger::Trace("[TimeSync::HandleResponse] Exit — early return, unknown requestId");
            return;
        }
        sendTime = oit->second;
        it->second.outstanding.erase(oit);
        Logger::Debug("[TimeSync::HandleResponse] Matched outstanding request id=%u for clientId=%u, erased from outstanding, remaining: %zu",
                      id, clientId, it->second.outstanding.size());
    }
    auto now = Clock::now();
    int64_t rtt = std::chrono::duration_cast<Millis>(now - sendTime).count();
    // Approximate offset = serverRecv + rtt/2 - clientTime
    int64_t offset = serverRecv + rtt/2 - clientTime;
    Logger::Debug("[TimeSync::HandleResponse] Computed RTT=%lld ms, offset=%lld ms (formula: serverRecv(%lld) + rtt/2(%lld) - clientTime(%lld))",
                  static_cast<long long>(rtt), static_cast<long long>(offset),
                  static_cast<long long>(serverRecv), static_cast<long long>(rtt/2),
                  static_cast<long long>(clientTime));

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& info = m_clients[clientId];
        info.rtt = rtt;
        info.offset = offset;
        Logger::Debug("[TimeSync::HandleResponse] Updated clientId=%u sync state — rtt=%lld ms, offset=%lld ms",
                      clientId, static_cast<long long>(rtt), static_cast<long long>(offset));
    }
    if (m_callback) {
        Logger::Debug("[TimeSync::HandleResponse] Invoking time sync callback for clientId=%u with offset=%lld ms, rtt=%lld ms",
                      clientId, static_cast<long long>(offset), static_cast<long long>(rtt));
        m_callback(clientId, offset, rtt);
    } else {
        Logger::Debug("[TimeSync::HandleResponse] No callback registered, skipping notification for clientId=%u", clientId);
    }
    Logger::Info("[TimeSync::HandleResponse] Time sync response processed — clientId=%u, requestId=%u, rtt=%lld ms, offset=%lld ms",
                 clientId, id, static_cast<long long>(rtt), static_cast<long long>(offset));
    Logger::Trace("[TimeSync::HandleResponse] Exit");
}

void TimeSync::Update()
{
    Logger::Trace("[TimeSync::Update] Entry");
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    int totalExpired = 0;
    for (auto& kv : m_clients) {
        auto& info = kv.second;
        size_t outstandingBefore = info.outstanding.size();
        for (auto it = info.outstanding.begin(); it != info.outstanding.end(); ) {
            if (now - it->second > m_timeout) {
                Logger::Debug("[TimeSync::Update] Expiring outstanding sync request id=%u for clientId=%u (exceeded timeout)",
                              it->first, kv.first);
                it = info.outstanding.erase(it);
                totalExpired++;
            } else {
                ++it;
            }
        }
        if (outstandingBefore != info.outstanding.size()) {
            Logger::Debug("[TimeSync::Update] Expired %zu outstanding requests for clientId=%u, remaining: %zu",
                          outstandingBefore - info.outstanding.size(), kv.first, info.outstanding.size());
        }
    }
    Logger::Debug("[TimeSync::Update] Update complete — expired %d requests across %zu clients", totalExpired, m_clients.size());
    Logger::Trace("[TimeSync::Update] Exit");
}

int64_t TimeSync::GetOffset(uint32_t clientId) const
{
    Logger::Trace("[TimeSync::GetOffset] Entry — clientId=%u", clientId);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(clientId);
    if (it != m_clients.end()) {
        Logger::Debug("[TimeSync::GetOffset] Found client entry for clientId=%u, offset=%lld ms",
                      clientId, static_cast<long long>(it->second.offset));
        Logger::Trace("[TimeSync::GetOffset] Exit — returning offset=%lld ms", static_cast<long long>(it->second.offset));
        return it->second.offset;
    } else {
        Logger::Debug("[TimeSync::GetOffset] No client entry found for clientId=%u, returning default offset=0", clientId);
        Logger::Trace("[TimeSync::GetOffset] Exit — returning 0");
        return 0;
    }
}

int64_t TimeSync::GetRtt(uint32_t clientId) const
{
    Logger::Trace("[TimeSync::GetRtt] Entry — clientId=%u", clientId);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(clientId);
    if (it != m_clients.end()) {
        Logger::Debug("[TimeSync::GetRtt] Found client entry for clientId=%u, rtt=%lld ms",
                      clientId, static_cast<long long>(it->second.rtt));
        Logger::Trace("[TimeSync::GetRtt] Exit — returning rtt=%lld ms", static_cast<long long>(it->second.rtt));
        return it->second.rtt;
    } else {
        Logger::Debug("[TimeSync::GetRtt] No client entry found for clientId=%u, returning default rtt=-1", clientId);
        Logger::Trace("[TimeSync::GetRtt] Exit — returning -1");
        return -1;
    }
}

void TimeSync::WriteInt64(std::vector<uint8_t>& buf, int64_t v)
{
    Logger::Trace("[TimeSync::WriteInt64] Entry — value=%lld, buf size before=%zu",
                  static_cast<long long>(v), buf.size());
    uint8_t tmp[8];
    std::memcpy(tmp, &v, 8);
    buf.insert(buf.end(), tmp, tmp+8);
    Logger::Trace("[TimeSync::WriteInt64] Exit — buf size after=%zu", buf.size());
}

int64_t TimeSync::ReadInt64(const std::vector<uint8_t>& buf, size_t& off)
{
    Logger::Trace("[TimeSync::ReadInt64] Entry — buf size=%zu, offset=%zu", buf.size(), off);
    int64_t v;
    std::memcpy(&v, buf.data()+off, 8);
    off += 8;
    Logger::Trace("[TimeSync::ReadInt64] Exit — read value=%lld, new offset=%zu",
                  static_cast<long long>(v), off);
    return v;
}
