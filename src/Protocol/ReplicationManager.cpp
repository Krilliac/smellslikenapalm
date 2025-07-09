// src/Protocol/ReplicationManager.cpp
#include "Protocol/ReplicationManager.h"
#include "Utils/Logger.h"
#include "Protocol/MessageEncoder.h"

ReplicationManager::ReplicationManager(ProtocolHandler& protocol)
    : m_protocol(protocol)
{
    Logger::Info("ReplicationManager initialized");
}

ReplicationManager::~ReplicationManager() = default;

void ReplicationManager::RegisterActor(uint32_t actorId) {
    m_actorStates[actorId] = ActorState{ actorId, {}, {}, {}, 0, 0, {} };
    m_actorDirtyFlags[actorId] = 0;
}

void ReplicationManager::UnregisterActor(uint32_t actorId) {
    m_actorStates.erase(actorId);
    m_actorDirtyFlags.erase(actorId);
}

void ReplicationManager::MarkActorDirty(uint32_t actorId, uint32_t flags) {
    if (m_actorDirtyFlags.count(actorId)) {
        m_actorDirtyFlags[actorId] |= flags;
    }
}

void ReplicationManager::QueuePropertyUpdate(const PropertyState& state) {
    m_propertyQueue.push_back(state);
}

void ReplicationManager::SetCompression(CompressionAlgorithm algo, int level) {
    m_compressionAlgo = algo;
    m_compressionLevel = level;
}

void ReplicationManager::Tick(float /*deltaTime*/) {
    BuildAndSendActorReplication();
    BuildAndSendPropertyReplication();
}

void ReplicationManager::BuildAndSendActorReplication() {
    std::vector<ActorState> toSend;
    for (auto& kv : m_actorDirtyFlags) {
        uint32_t actorId = kv.first;
        uint32_t flags   = kv.second;
        if (flags == 0) continue;

        ActorState& st = m_actorStates[actorId];
        st.stateFlags = flags;
        // Assume external game logic has updated st.position, orientation, etc.
        toSend.push_back(st);
        m_actorDirtyFlags[actorId] = 0;
    }
    if (toSend.empty()) return;

    Packet pkt = ActorReplication::BuildReplicationPacket(toSend);
    // compress payload if needed
    std::vector<uint8_t> raw = pkt.Serialize();
    std::vector<uint8_t> comp;
    if (m_compressionAlgo != CompressionAlgorithm::NONE
        && CompressionHandler::Compress(raw, comp, m_compressionAlgo, m_compressionLevel))
    {
        pkt = Packet("COMPRESSION", comp);
    }
    m_protocol.Handle(0, pkt, {});  // clientId=0 means broadcast
}

void ReplicationManager::BuildAndSendPropertyReplication() {
    if (m_propertyQueue.empty()) return;

    Packet pkt = PropertyReplication::BuildPacket(m_propertyQueue);
    std::vector<uint8_t> raw = pkt.Serialize();
    std::vector<uint8_t> comp;
    if (m_compressionAlgo != CompressionAlgorithm::NONE
        && CompressionHandler::Compress(raw, comp, m_compressionAlgo, m_compressionLevel))
    {
        pkt = Packet("COMPRESSION", comp);
    }
    m_protocol.Handle(0, pkt, {});
    m_propertyQueue.clear();
}