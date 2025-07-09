// src/Protocol/ReplicationManager.h
#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include "Protocol/ActorReplication.h"
#include "Protocol/PropertyReplication.h"
#include "Network/Packet.h"
#include "Protocol/CompressionHandler.h"
#include "Protocol/ProtocolHandler.h"

class ReplicationManager {
public:
    explicit ReplicationManager(ProtocolHandler& protocol);
    ~ReplicationManager();

    // Register an actor for replication (server‐side)
    void RegisterActor(uint32_t actorId);

    // Unregister an actor
    void UnregisterActor(uint32_t actorId);

    // Mark per‐actor state dirty flags for next update
    void MarkActorDirty(uint32_t actorId, uint32_t flags);

    // Queue a property update for an object
    void QueuePropertyUpdate(const PropertyState& state);

    // Build and send replication packets each tick
    void Tick(float deltaTime);

    // Set compression algorithm for replication packets
    void SetCompression(CompressionAlgorithm algo, int level = -1);

private:
    ProtocolHandler&                                  m_protocol;
    CompressionAlgorithm                             m_compressionAlgo = CompressionAlgorithm::NONE;
    int                                              m_compressionLevel = -1;

    // Actor replication state
    std::unordered_map<uint32_t, ActorState>         m_actorStates;
    std::unordered_map<uint32_t, uint32_t>           m_actorDirtyFlags;

    // Property replication queue
    std::vector<PropertyState>                       m_propertyQueue;

    // Helpers
    void BuildAndSendActorReplication();
    void BuildAndSendPropertyReplication();
};