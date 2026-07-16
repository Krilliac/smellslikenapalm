// Capture/source-bounded decoder for the retail ROWeapon hit-report RPC.
//
// This deliberately implements only ROWeapon.ServerHandleClientHitsOne (h56).
// The fixed-array/projectile/melee variants have different schemas and must not
// be guessed.  See data/re/netfields/netfields_u_ROWeapon.txt and ROWeapon.uc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Game/ParticipantRoster.h"
#include "Math/Vector3.h"
#include "Network/ActorReplication.h"
#include "Network/BitReader.h"

namespace WeaponCombatRepl {

constexpr uint32_t kRoWeaponMaxHandle = 99;
constexpr uint32_t kServerHandleClientHitsOne = 56;
constexpr uint32_t kMaxNetworkedHardcodedName = 1250;
constexpr size_t kMaxRpcBits = 16384;
constexpr size_t kMaxNameCharacters = 1023;
constexpr float kSmallVectorScale = 1024.0f;

// FName as serialized by UPackageMap::SerializeName.  A hardcoded name carries
// only its EName index; a non-hardcoded name carries FString + instance number.
struct NetName {
    bool hardcoded = false;
    uint32_t hardcodedIndex = 0;
    std::string text;
    int32_t number = 0;
};

struct TraceHitInfo {
    ActorRepl::NetGUIDRef material{};
    ActorRepl::NetGUIDRef physicalMaterial{};
    int32_t item = 0;
    int32_t levelIndex = 0;
    NetName boneName{};
    ActorRepl::NetGUIDRef hitComponent{};
};

struct ImpactInfo {
    ActorRepl::NetGUIDRef hitActor{};

    // Set only when hitActor was non-null and the connection-local resolver
    // identified it as a replicated combat participant.  A known world actor is
    // valid and intentionally has no participant id.
    std::optional<ParticipantId> participantId;

    Vector3 hitLocation{};
    Vector3 hitNormal{};

    // ROWeapon multiplies RayDir by 1024 before the RPC and divides it after
    // receipt.  This field is the divided, gameplay-space direction.
    Vector3 rayDirection{};
    Vector3 startTrace{};
    TraceHitInfo hitInfo{};
    bool exitImpact = false;
};

struct ServerHandleClientHitsOne {
    ImpactInfo impact{};
    uint8_t firedMode = 0;
    Vector3 firstHitLocation{};
    Vector3 startTrace{};
    size_t consumedBits = 0;
};

// Resolution is deliberately connection-local: UE3 dynamic object references
// are actor-channel indices, not stable global player ids.
struct ActorResolution {
    bool known = false;
    std::optional<ParticipantId> participantId;
};

using ActorResolver =
    std::function<ActorResolution(const ActorRepl::NetGUIDRef& reference)>;

enum class DecodeError : uint8_t {
    None,
    InvalidBuffer,
    Oversized,
    Truncated,
    UnsupportedHandle,
    MissingRequiredParameter,
    InvalidName,
    NonFiniteVector,
    UnknownActorReference,
    TrailingBits,
};

struct DecodeResult {
    DecodeError error = DecodeError::InvalidBuffer;
    size_t consumedBits = 0;
    ServerHandleClientHitsOne rpc{};

    bool valid() const { return error == DecodeError::None; }
};

// Decode exactly one h56 call from the current reader position.  The operation
// is transactional: reader and output are unchanged on failure.
bool DecodeOne(BitReader& reader, const ActorResolver& resolveActor,
               ServerHandleClientHitsOne& output, DecodeError& error);

// Decode a complete payload containing exactly one h56 call.  Any trailing
// bits are rejected rather than interpreted as an unproven second schema.
DecodeResult DecodeServerHandleClientHitsOne(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits,
    const ActorResolver& resolveActor);

// ---- Capture-exact M61 grenade actor replication -------------------------
//
// Retail packetlog rs2_realserver_capture.pcapng contains 42 opens for
// M61GrenadeProjectile. Unlike Engine.Projectile's default, ROStickGrenadeProjectile
// overrides bNetTemporary=false, so M61 uses a persistent actor channel:
// reliable open -> unreliable movement/fuse deltas -> unreliable tear-off ->
// reliable empty close. These builders intentionally expose only the evidenced
// schema; other projectile classes need their own cooked class/field tables.

constexpr uint32_t kM61ProjectileClassRef = 60245;
constexpr uint32_t kM61ProjectileMaxHandle = 26;
constexpr uint32_t kM61VelocityHandle = 3;
constexpr uint32_t kM61InstigatorHandle = 4;
constexpr uint32_t kM61RotationHandle = 12;
constexpr uint32_t kM61LocationHandle = 13;
constexpr uint32_t kM61CollideActorsHandle = 18;
constexpr uint32_t kM61TearOffHandle = 20;
constexpr uint32_t kM61FuseLengthHandle = 25;
constexpr float kM61MaxReplicatedFuseSeconds = 60.0f;
constexpr float kM61MinDetonationFuseSeconds = -1.0f;

struct M61VisualSnapshot {
    Vector3 positionUu{};
    Vector3 velocityUuPerSecond{};
    uint16_t pitch = 0;
    uint16_t yaw = 0;
    uint16_t roll = 0;
    float fuseSeconds = 0.0f;
    // Omit when the connection has no proven actor-channel reference for the
    // thrower. Dynamic zero is UE3 None; static zero is never valid None.
    std::optional<ActorRepl::NetGUIDRef> instigator;
};

enum class M61VisualError : uint8_t {
    None,
    InvalidChannel,
    InvalidSequence,
    InvalidState,
    NonFiniteValue,
    VectorOutOfRange,
    InvalidFuse,
    InvalidInstigator,
    InvalidChannelRange,
    DuplicateProjectile,
    UnknownProjectile,
    ChannelExhausted,
};

struct M61VisualResult {
    M61VisualError error = M61VisualError::InvalidState;
    PacketCodec::Bunch bunch{};

    bool valid() const { return error == M61VisualError::None; }
};

// Pure, transactional wire builders. A failed call returns an empty/default
// bunch and never emits a partially encoded payload.
M61VisualResult EncodeM61Open(uint32_t channelIndex,
                              uint32_t reliableSequence,
                              const M61VisualSnapshot& snapshot);
M61VisualResult EncodeM61Update(uint32_t channelIndex,
                                const M61VisualSnapshot& snapshot);
M61VisualResult EncodeM61FuseUpdate(uint32_t channelIndex,
                                    float fuseSeconds);
M61VisualResult EncodeM61Detonation(uint32_t channelIndex,
                                    float fuseSeconds);
M61VisualResult EncodeM61Close(uint32_t channelIndex,
                               uint32_t reliableSequence);

enum class M61VisualState : uint8_t {
    Dormant,
    Active,
    Detonated,
    Closed,
};

// Enforces the captured per-projectile lifecycle while leaving reliable
// sequence ownership with the connection/channel layer. Construct a new
// lifecycle for channel reuse; there is deliberately no cursor-reset API.
class M61VisualLifecycle {
public:
    explicit M61VisualLifecycle(uint32_t channelIndex)
        : m_channelIndex(channelIndex) {}

    uint32_t ChannelIndex() const { return m_channelIndex; }
    M61VisualState State() const { return m_state; }

    M61VisualResult Spawn(uint32_t reliableSequence,
                          const M61VisualSnapshot& snapshot);
    M61VisualResult Update(const M61VisualSnapshot& snapshot);
    M61VisualResult UpdateFuse(float fuseSeconds);
    M61VisualResult Detonate(float fuseSeconds);
    M61VisualResult Close(uint32_t reliableSequence);

private:
    uint32_t m_channelIndex = 0;
    M61VisualState m_state = M61VisualState::Dormant;
};

// Per-connection ownership for capture-exact M61 actor channels.  Actor
// channels may be reused only after their reliable close is acknowledged and
// every earlier reliable bunch on that channel has left the retransmit ledger,
// while the reliable sequence cursor survives that reuse exactly as UE3's
// In/OutReliable arrays do.  The default range deliberately sits above remote
// participant actors and below the retail 1024-channel bound.
constexpr uint32_t kM61VisualFirstChannel = 768;
constexpr uint32_t kM61VisualLastChannel =
    ActorRepl::kDynamicChannelMax - 1u;

struct M61VisualBatchResult {
    M61VisualError error = M61VisualError::InvalidState;
    std::vector<PacketCodec::Bunch> bunches;

    bool valid() const { return error == M61VisualError::None; }
};

class M61VisualChannelPool {
public:
    using ChannelAvailable = std::function<bool(uint32_t)>;

    explicit M61VisualChannelPool(
        uint32_t firstChannel = kM61VisualFirstChannel,
        uint32_t lastChannel = kM61VisualLastChannel);

    bool IsValid() const { return m_valid; }
    size_t ActiveCount() const { return m_active.size(); }
    std::optional<uint32_t> ChannelFor(uint64_t projectileKey) const;
    std::optional<uint64_t> ProjectileForChannel(uint32_t channel) const;

    M61VisualBatchResult Spawn(uint64_t projectileKey,
                               const M61VisualSnapshot& snapshot,
                               const ChannelAvailable& channelAvailable = {});
    M61VisualBatchResult Update(uint64_t projectileKey,
                                const M61VisualSnapshot& snapshot) const;
    M61VisualBatchResult UpdateFuse(uint64_t projectileKey,
                                    float fuseSeconds) const;
    // Retail tears the persistent actor off with an unreliable property delta,
    // then reliably closes the channel. Both bunches are produced
    // transactionally; the channel remains quarantined until AcknowledgeClose.
    M61VisualBatchResult DetonateAndClose(uint64_t projectileKey,
                                          float fuseSeconds);
    M61VisualBatchResult Close(uint64_t projectileKey);
    // A queued close remains owned until ConnectionManager observes its packet
    // ACK and proves no earlier reliable bunch for the channel is still pending.
    bool AcknowledgeClose(uint32_t channel);
    bool IsCloseQueued(uint32_t channel) const;
    // An inbound actor close/failure is not an ACK of our ordered close. Stop
    // emitting and quarantine the channel until this connection is destroyed.
    bool SuppressChannel(uint32_t channel);
    bool IsSuppressed(uint32_t channel) const;
    void Clear();

private:
    struct ActiveVisual {
        uint32_t channel = 0;
        bool closeQueued = false;
        bool suppressed = false;
    };

    bool m_valid = false;
    uint32_t m_firstChannel = 0;
    uint32_t m_lastChannel = 0;
    uint32_t m_nextChannel = 0;
    std::map<uint64_t, ActiveVisual> m_active;
    std::map<uint32_t, uint32_t> m_reliableSequence;

    bool ChannelInUse(uint32_t channel) const;
    std::optional<uint32_t> FindFreeChannel(
        const ChannelAvailable& channelAvailable) const;
    uint32_t NextReliableSequence(uint32_t channel) const;
    void CommitReliableSequence(uint32_t channel, uint32_t sequence);
    void AdvanceAllocationCursor(uint32_t channel);
};

} // namespace WeaponCombatRepl
