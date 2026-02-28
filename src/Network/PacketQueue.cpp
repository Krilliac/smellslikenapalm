// src/Network/PacketQueue.cpp

#include "Network/PacketQueue.h"
#include "Utils/Logger.h"

PacketQueue::PacketQueue() {
    Logger::Trace("[PacketQueue::PacketQueue] Entry: default constructor");
    Logger::Debug("[PacketQueue::PacketQueue] PacketQueue created, m_shutdown=false");
    Logger::Trace("[PacketQueue::PacketQueue] Exit");
}

PacketQueue::~PacketQueue() {
    Logger::Trace("[PacketQueue::~PacketQueue] Entry: destructor called");
    Logger::Debug("[PacketQueue::~PacketQueue] Destroying PacketQueue, calling Shutdown()");
    Shutdown();
    Logger::Trace("[PacketQueue::~PacketQueue] Exit");
}

void PacketQueue::Enqueue(const ReceivedPacket& pkt) {
    Logger::Trace("[PacketQueue::Enqueue] Entry");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Logger::Debug("[PacketQueue::Enqueue] Acquired mutex lock");
        if (m_shutdown) {
            Logger::Debug("[PacketQueue::Enqueue] Queue is shut down, dropping packet");
            Logger::Trace("[PacketQueue::Enqueue] Exit: shutdown, packet dropped");
            return;
        }
        m_queue.push(pkt);
        Logger::Debug("[PacketQueue::Enqueue] Packet enqueued, queue size now=%zu", m_queue.size());
    }
    m_cv.notify_one();
    Logger::Trace("[PacketQueue::Enqueue] Exit: notified one waiting thread");
}

bool PacketQueue::Dequeue(ReceivedPacket& outPkt) {
    Logger::Trace("[PacketQueue::Dequeue] Entry: waiting for packet");
    std::unique_lock<std::mutex> lock(m_mutex);
    Logger::Debug("[PacketQueue::Dequeue] Acquired mutex lock, waiting on condition variable");
    m_cv.wait(lock, [&] { return m_shutdown || !m_queue.empty(); });
    Logger::Debug("[PacketQueue::Dequeue] Condition met: m_shutdown=%s, queue empty=%s",
                  m_shutdown ? "true" : "false", m_queue.empty() ? "true" : "false");
    if (m_shutdown && m_queue.empty()) {
        Logger::Debug("[PacketQueue::Dequeue] Shutdown signaled and queue empty, returning false");
        Logger::Trace("[PacketQueue::Dequeue] Exit: returning false (shutdown)");
        return false;
    }
    outPkt = std::move(m_queue.front());
    m_queue.pop();
    Logger::Debug("[PacketQueue::Dequeue] Dequeued packet, queue size now=%zu", m_queue.size());
    Logger::Trace("[PacketQueue::Dequeue] Exit: returning true");
    return true;
}

bool PacketQueue::TryDequeue(ReceivedPacket& outPkt) {
    Logger::Trace("[PacketQueue::TryDequeue] Entry: non-blocking dequeue attempt");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[PacketQueue::TryDequeue] Acquired mutex lock, queue empty=%s, size=%zu",
                  m_queue.empty() ? "true" : "false", m_queue.size());
    if (m_queue.empty()) {
        Logger::Debug("[PacketQueue::TryDequeue] Queue is empty, returning false");
        Logger::Trace("[PacketQueue::TryDequeue] Exit: returning false (empty)");
        return false;
    }
    outPkt = std::move(m_queue.front());
    m_queue.pop();
    Logger::Debug("[PacketQueue::TryDequeue] Dequeued packet, queue size now=%zu", m_queue.size());
    Logger::Trace("[PacketQueue::TryDequeue] Exit: returning true");
    return true;
}

void PacketQueue::Shutdown() {
    Logger::Trace("[PacketQueue::Shutdown] Entry");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Logger::Debug("[PacketQueue::Shutdown] Acquired mutex lock, setting m_shutdown=true");
        m_shutdown = true;
    }
    m_cv.notify_all();
    Logger::Debug("[PacketQueue::Shutdown] Notified all waiting threads");
    Logger::Info("[PacketQueue::Shutdown] PacketQueue shutdown complete");
    Logger::Trace("[PacketQueue::Shutdown] Exit");
}

void PacketQueue::Clear() {
    Logger::Trace("[PacketQueue::Clear] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t previousSize = m_queue.size();
    Logger::Debug("[PacketQueue::Clear] Acquired mutex lock, clearing %zu queued packets", previousSize);
    while (!m_queue.empty()) m_queue.pop();
    Logger::Debug("[PacketQueue::Clear] Queue cleared: %zu -> 0 packets", previousSize);
    Logger::Trace("[PacketQueue::Clear] Exit");
}
