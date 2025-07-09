#include "Time/TimeSync.h"

TimeSync::TimeSync(TimeSyncCallback cb)
    : m_callback(std::move(cb))
{}

TimeSync::~TimeSync() = default;

uint32_t TimeSync::SendTimeSyncRequest(uint32_t clientId,
                                       const ClientAddress& addr,
                                       std::function<void(const ClientAddress&, const std::vector<uint8_t>&)> sender)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_clients[clientId];
    uint32_t id = info.nextId++;
    info.outstanding[id] = Clock::now();

    // Build payload: [requestId:uint32]
    std::vector<uint8_t> buf(4);
    std::memcpy(buf.data(), &id, 4);
    sender(addr, buf);
    return id;
}

std::vector<uint8_t> TimeSync::HandleRequest(const std::vector<uint8_t>& payload)
{
    // Extract requestId
    if (payload.size() < 4) return {};
    uint32_t id;
    std::memcpy(&id, payload.data(), 4);
    // Build response: [requestId:uint32][serverTime:int64]
    auto now = std::chrono::duration_cast<Millis>(Clock::now().time_since_epoch()).count();
    std::vector<uint8_t> resp(4 + 8);
    std::memcpy(resp.data(), &id, 4);
    WriteInt64(resp, now);
    return resp;
}

void TimeSync::HandleResponse(uint32_t clientId,
                              const ClientAddress& addr,
                              const std::vector<uint8_t>& payload)
{
    // payload: [id:uint32][clientSend:int64][serverRecv:int64]
    if (payload.size() < 4 + 8 + 8) return;
    size_t off = 0;
    uint32_t id = 0;
    std::memcpy(&id, payload.data()+off, 4);
    off += 4;
    int64_t clientTime = ReadInt64(payload, off);
    int64_t serverRecv = ReadInt64(payload, off);

    Clock::time_point sendTime;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_clients.find(clientId);
        if (it==m_clients.end()) return;
        auto oit = it->second.outstanding.find(id);
        if (oit==it->second.outstanding.end()) return;
        sendTime = oit->second;
        it->second.outstanding.erase(oit);
    }
    auto now = Clock::now();
    int64_t rtt = std::chrono::duration_cast<Millis>(now - sendTime).count();
    // Approximate offset = serverRecv + rtt/2 – clientTime
    int64_t offset = serverRecv + rtt/2 - clientTime;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& info = m_clients[clientId];
        info.rtt = rtt;
        info.offset = offset;
    }
    if (m_callback) m_callback(clientId, offset, rtt);
}

void TimeSync::Update()
{
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& kv : m_clients) {
        auto& info = kv.second;
        for (auto it = info.outstanding.begin(); it != info.outstanding.end(); ) {
            if (now - it->second > m_timeout) {
                it = info.outstanding.erase(it);
            } else {
                ++it;
            }
        }
    }
}

int64_t TimeSync::GetOffset(uint32_t clientId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(clientId);
    return it != m_clients.end() ? it->second.offset : 0;
}

int64_t TimeSync::GetRtt(uint32_t clientId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(clientId);
    return it != m_clients.end() ? it->second.rtt : -1;
}

void TimeSync::WriteInt64(std::vector<uint8_t>& buf, int64_t v)
{
    uint8_t tmp[8];
    std::memcpy(tmp, &v, 8);
    buf.insert(buf.end(), tmp, tmp+8);
}

int64_t TimeSync::ReadInt64(const std::vector<uint8_t>& buf, size_t& off)
{
    int64_t v;
    std::memcpy(&v, buf.data()+off, 8);
    off += 8;
    return v;
}