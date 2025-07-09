// src/Network/PacketQueue.h

#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include "Network/Packet.h"

struct ReceivedPacket {
    uint32_t clientId;
    Packet   packet;
    PacketMetadata meta;
};

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    // Enqueue a received packet (thread-safe)
    void Enqueue(const ReceivedPacket& pkt);

    // Dequeue the next packet; blocks if empty until shutdown or packet arrives
    bool Dequeue(ReceivedPacket& outPkt);

    // Non‐blocking attempt to dequeue; returns false if queue empty
    bool TryDequeue(ReceivedPacket& outPkt);

    // Signal shutdown to unblock waiting threads
    void Shutdown();

    // Clear all queued packets
    void Clear();

private:
    std::queue<ReceivedPacket>     m_queue;
    std::mutex                     m_mutex;
    std::condition_variable        m_cv;
    bool                           m_shutdown{false};
};