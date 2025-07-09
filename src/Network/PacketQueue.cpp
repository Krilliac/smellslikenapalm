// src/Network/PacketQueue.cpp

#include "Network/PacketQueue.h"

PacketQueue::PacketQueue() = default;
PacketQueue::~PacketQueue() {
    Shutdown();
}

void PacketQueue::Enqueue(const ReceivedPacket& pkt) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return;
        m_queue.push(pkt);
    }
    m_cv.notify_one();
}

bool PacketQueue::Dequeue(ReceivedPacket& outPkt) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&] { return m_shutdown || !m_queue.empty(); });
    if (m_shutdown && m_queue.empty()) return false;
    outPkt = std::move(m_queue.front());
    m_queue.pop();
    return true;
}

bool PacketQueue::TryDequeue(ReceivedPacket& outPkt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) return false;
    outPkt = std::move(m_queue.front());
    m_queue.pop();
    return true;
}

void PacketQueue::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
}

void PacketQueue::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) m_queue.pop();
}