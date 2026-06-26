// src/Protocol/ReplicationManager.cpp
#include "Protocol/ReplicationManager.h"
#include "Utils/Logger.h"
#include "Protocol/MessageEncoder.h"

ReplicationManager::ReplicationManager(ProtocolHandler& protocol)
    : m_protocol(protocol)
{
    Logger::Trace("[ReplicationManager::ReplicationManager] entry — constructing with ProtocolHandler reference");
    Logger::Info("ReplicationManager initialized");
    Logger::Debug("[ReplicationManager::ReplicationManager] initial state: compressionAlgo=NONE, compressionLevel=0, actorStates empty, propertyQueue empty");
    Logger::Trace("[ReplicationManager::ReplicationManager] exit — construction complete");
}

ReplicationManager::~ReplicationManager() {
    Logger::Trace("[ReplicationManager::~ReplicationManager] entry — destroying ReplicationManager");
    Logger::Debug("[ReplicationManager::~ReplicationManager] final state: %zu actor states, %zu dirty flags, %zu queued properties",
                  m_actorStates.size(), m_actorDirtyFlags.size(), m_propertyQueue.size());
    Logger::Trace("[ReplicationManager::~ReplicationManager] exit — destruction complete");
}

void ReplicationManager::RegisterActor(uint32_t actorId) {
    Logger::Trace("[ReplicationManager::RegisterActor] entry — actorId=%u", actorId);
    m_actorStates[actorId] = ActorState{ actorId, {}, {}, {}, 0, 0, {} };
    m_actorDirtyFlags[actorId] = 0;
    Logger::Debug("[ReplicationManager::RegisterActor] registered actorId=%u with zeroed state and dirty flags=0", actorId);
    Logger::Info("[ReplicationManager::RegisterActor] actor %u registered — total registered actors: %zu",
                 actorId, m_actorStates.size());
    Logger::Trace("[ReplicationManager::RegisterActor] exit — actorId=%u registered successfully", actorId);
}

void ReplicationManager::UnregisterActor(uint32_t actorId) {
    Logger::Trace("[ReplicationManager::UnregisterActor] entry — actorId=%u", actorId);
    size_t erasedStates = m_actorStates.erase(actorId);
    size_t erasedFlags = m_actorDirtyFlags.erase(actorId);
    if (erasedStates > 0) {
        Logger::Info("[ReplicationManager::UnregisterActor] actor %u unregistered — remaining actors: %zu",
                     actorId, m_actorStates.size());
    } else {
        Logger::Warn("[ReplicationManager::UnregisterActor] actor %u was not found in actor states (erasedStates=%zu, erasedFlags=%zu)",
                     actorId, erasedStates, erasedFlags);
    }
    Logger::Debug("[ReplicationManager::UnregisterActor] erased %zu state entries and %zu dirty flag entries for actorId=%u",
                  erasedStates, erasedFlags, actorId);
    Logger::Trace("[ReplicationManager::UnregisterActor] exit — actorId=%u removal complete", actorId);
}

void ReplicationManager::MarkActorDirty(uint32_t actorId, uint32_t flags) {
    Logger::Trace("[ReplicationManager::MarkActorDirty] entry — actorId=%u, flags=0x%08X", actorId, flags);
    if (m_actorDirtyFlags.count(actorId)) {
        uint32_t oldFlags = m_actorDirtyFlags[actorId];
        m_actorDirtyFlags[actorId] |= flags;
        Logger::Debug("[ReplicationManager::MarkActorDirty] actorId=%u dirty flags updated: 0x%08X | 0x%08X = 0x%08X",
                      actorId, oldFlags, flags, m_actorDirtyFlags[actorId]);
    } else {
        Logger::Warn("[ReplicationManager::MarkActorDirty] actorId=%u not found in dirty flags map — ignoring flags=0x%08X",
                     actorId, flags);
    }
    Logger::Trace("[ReplicationManager::MarkActorDirty] exit — actorId=%u", actorId);
}

void ReplicationManager::QueuePropertyUpdate(const PropertyState& state) {
    Logger::Trace("[ReplicationManager::QueuePropertyUpdate] entry — objectId=%u, flags=0x%08X",
                  state.objectId, state.flags);
    m_propertyQueue.push_back(state);
    Logger::Debug("[ReplicationManager::QueuePropertyUpdate] queued property update for objectId=%u — queue size now: %zu",
                  state.objectId, m_propertyQueue.size());
    Logger::Trace("[ReplicationManager::QueuePropertyUpdate] exit — property queued successfully");
}

void ReplicationManager::SetCompression(CompressionAlgorithm algo, int level) {
    Logger::Trace("[ReplicationManager::SetCompression] entry — algo=%d, level=%d",
                  static_cast<int>(algo), level);
    CompressionAlgorithm oldAlgo = m_compressionAlgo;
    int oldLevel = m_compressionLevel;
    m_compressionAlgo = algo;
    m_compressionLevel = level;
    Logger::Debug("[ReplicationManager::SetCompression] compression changed from algo=%d/level=%d to algo=%d/level=%d",
                  static_cast<int>(oldAlgo), oldLevel, static_cast<int>(algo), level);
    Logger::Info("[ReplicationManager::SetCompression] compression configured: algorithm=%d, level=%d",
                 static_cast<int>(algo), level);
    Logger::Trace("[ReplicationManager::SetCompression] exit — compression settings updated");
}

void ReplicationManager::Tick(float /*deltaTime*/) {
    Logger::Trace("[ReplicationManager::Tick] entry — processing replication tick");
    Logger::Debug("[ReplicationManager::Tick] beginning tick — %zu actor states, %zu dirty flags, %zu queued properties",
                  m_actorStates.size(), m_actorDirtyFlags.size(), m_propertyQueue.size());
    BuildAndSendActorReplication();
    BuildAndSendPropertyReplication();
    Logger::Debug("[ReplicationManager::Tick] tick complete — property queue size after: %zu", m_propertyQueue.size());
    Logger::Trace("[ReplicationManager::Tick] exit — replication tick finished");
}

void ReplicationManager::BuildAndSendActorReplication() {
    Logger::Trace("[ReplicationManager::BuildAndSendActorReplication] entry — scanning %zu actor dirty flags",
                  m_actorDirtyFlags.size());
    std::vector<ActorState> toSend;
    for (auto& kv : m_actorDirtyFlags) {
        uint32_t actorId = kv.first;
        uint32_t flags   = kv.second;
        if (flags == 0) {
            Logger::Trace("[ReplicationManager::BuildAndSendActorReplication] actorId=%u has no dirty flags — skipping", actorId);
            continue;
        }

        Logger::Debug("[ReplicationManager::BuildAndSendActorReplication] actorId=%u is dirty with flags=0x%08X — adding to send list",
                      actorId, flags);
        // Only replicate actors that have a registered state. Using operator[] here
        // would silently insert a default-constructed ActorState for a stale/unknown
        // dirty-flag entry, replicating a bogus actor. Skip and clear instead.
        auto stIt = m_actorStates.find(actorId);
        if (stIt == m_actorStates.end()) {
            Logger::Warn("[ReplicationManager::BuildAndSendActorReplication] actorId=%u has dirty flags but no registered state — skipping",
                         actorId);
            m_actorDirtyFlags[actorId] = 0;
            continue;
        }
        ActorState& st = stIt->second;
        st.stateFlags = flags;
        // Assume external game logic has updated st.position, orientation, etc.
        toSend.push_back(st);
        m_actorDirtyFlags[actorId] = 0;
        Logger::Trace("[ReplicationManager::BuildAndSendActorReplication] actorId=%u dirty flags cleared to 0", actorId);
    }
    if (toSend.empty()) {
        Logger::Trace("[ReplicationManager::BuildAndSendActorReplication] exit — no dirty actors to replicate");
        return;
    }

    Logger::Debug("[ReplicationManager::BuildAndSendActorReplication] building replication packet for %zu dirty actors",
                  toSend.size());
    Packet pkt = ActorReplication::BuildReplicationPacket(toSend);
    // compress payload if needed
    std::vector<uint8_t> raw = pkt.Serialize();
    Logger::Debug("[ReplicationManager::BuildAndSendActorReplication] serialized packet: %zu raw bytes", raw.size());
    std::vector<uint8_t> comp;
    if (m_compressionAlgo != CompressionAlgorithm::NONE
        && CompressionHandler::Compress(raw, comp, m_compressionAlgo, m_compressionLevel))
    {
        Logger::Debug("[ReplicationManager::BuildAndSendActorReplication] compressed %zu bytes to %zu bytes (algo=%d, level=%d)",
                      raw.size(), comp.size(), static_cast<int>(m_compressionAlgo), m_compressionLevel);
        pkt = Packet("COMPRESSION", comp);
    } else if (m_compressionAlgo != CompressionAlgorithm::NONE) {
        Logger::Warn("[ReplicationManager::BuildAndSendActorReplication] compression failed for %zu bytes — sending uncompressed", raw.size());
    } else {
        Logger::Debug("[ReplicationManager::BuildAndSendActorReplication] compression disabled — sending %zu raw bytes", raw.size());
    }
    m_protocol.Handle(0, pkt, {});  // clientId=0 means broadcast
    Logger::Info("[ReplicationManager::BuildAndSendActorReplication] sent actor replication for %zu actors (broadcast)",
                 toSend.size());
    Logger::Trace("[ReplicationManager::BuildAndSendActorReplication] exit — actor replication sent");
}

void ReplicationManager::BuildAndSendPropertyReplication() {
    Logger::Trace("[ReplicationManager::BuildAndSendPropertyReplication] entry — propertyQueue.size()=%zu",
                  m_propertyQueue.size());
    if (m_propertyQueue.empty()) {
        Logger::Trace("[ReplicationManager::BuildAndSendPropertyReplication] exit — property queue is empty, nothing to send");
        return;
    }

    Logger::Debug("[ReplicationManager::BuildAndSendPropertyReplication] building property replication packet for %zu queued properties",
                  m_propertyQueue.size());
    Packet pkt = PropertyReplication::BuildPacket(m_propertyQueue);
    std::vector<uint8_t> raw = pkt.Serialize();
    Logger::Debug("[ReplicationManager::BuildAndSendPropertyReplication] serialized packet: %zu raw bytes", raw.size());
    std::vector<uint8_t> comp;
    if (m_compressionAlgo != CompressionAlgorithm::NONE
        && CompressionHandler::Compress(raw, comp, m_compressionAlgo, m_compressionLevel))
    {
        Logger::Debug("[ReplicationManager::BuildAndSendPropertyReplication] compressed %zu bytes to %zu bytes (algo=%d, level=%d)",
                      raw.size(), comp.size(), static_cast<int>(m_compressionAlgo), m_compressionLevel);
        pkt = Packet("COMPRESSION", comp);
    } else if (m_compressionAlgo != CompressionAlgorithm::NONE) {
        Logger::Warn("[ReplicationManager::BuildAndSendPropertyReplication] compression failed for %zu bytes — sending uncompressed", raw.size());
    } else {
        Logger::Debug("[ReplicationManager::BuildAndSendPropertyReplication] compression disabled — sending %zu raw bytes", raw.size());
    }
    m_protocol.Handle(0, pkt, {});
    size_t sentCount = m_propertyQueue.size();
    m_propertyQueue.clear();
    Logger::Info("[ReplicationManager::BuildAndSendPropertyReplication] sent property replication for %zu properties (broadcast), queue cleared",
                 sentCount);
    Logger::Trace("[ReplicationManager::BuildAndSendPropertyReplication] exit — property replication sent and queue cleared");
}
