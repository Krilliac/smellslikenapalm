#include "Network/WeaponCombatReplication.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace WeaponCombatRepl {
namespace {

bool IsFinite(const Vector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool ReadVector(BitReader& reader, Vector3& value, DecodeError& error) {
    ActorRepl::ReadCompressedVector(reader, value.x, value.y, value.z);
    if (reader.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (!IsFinite(value)) {
        error = DecodeError::NonFiniteVector;
        return false;
    }
    return true;
}

bool ReadObject(BitReader& reader, ActorRepl::NetGUIDRef& value,
                DecodeError& error) {
    value = ActorRepl::ReadNetGUID(reader);
    if (reader.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    return true;
}

bool ReadName(BitReader& reader, NetName& value, DecodeError& error) {
    value = {};
    value.hardcoded = reader.ReadBit();
    if (value.hardcoded) {
        value.hardcodedIndex =
            reader.SerializeInt(kMaxNetworkedHardcodedName + 1u);
    } else {
        value.text = reader.ReadString();
        value.number = reader.ReadInt32();
    }
    if (reader.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if ((!value.hardcoded &&
         (value.text.size() > kMaxNameCharacters || value.number < 0))) {
        error = DecodeError::InvalidName;
        return false;
    }
    return true;
}

bool ReadTraceHitInfo(BitReader& reader, TraceHitInfo& value,
                      DecodeError& error) {
    if (!ReadObject(reader, value.material, error) ||
        !ReadObject(reader, value.physicalMaterial, error)) {
        return false;
    }
    value.item = reader.ReadInt32();
    value.levelIndex = reader.ReadInt32();
    if (reader.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    return ReadName(reader, value.boneName, error) &&
           ReadObject(reader, value.hitComponent, error);
}

bool ReadImpactInfo(BitReader& reader, const ActorResolver& resolveActor,
                    ImpactInfo& value, DecodeError& error) {
    if (!ReadObject(reader, value.hitActor, error)) return false;

    // Dynamic channel zero is UE3's explicit None reference.  Every other actor
    // reference must be known to the connection-local package/channel map.
    const bool isNone = value.hitActor.isDynamic && value.hitActor.index == 0;
    if (!isNone) {
        if (!resolveActor) {
            error = DecodeError::UnknownActorReference;
            return false;
        }
        const ActorResolution resolution = resolveActor(value.hitActor);
        if (!resolution.known) {
            error = DecodeError::UnknownActorReference;
            return false;
        }
        value.participantId = resolution.participantId;
    }

    Vector3 encodedRayDirection{};
    if (!ReadVector(reader, value.hitLocation, error) ||
        !ReadVector(reader, value.hitNormal, error) ||
        !ReadVector(reader, encodedRayDirection, error) ||
        !ReadVector(reader, value.startTrace, error) ||
        !ReadTraceHitInfo(reader, value.hitInfo, error)) {
        if (reader.IsOverflowed()) error = DecodeError::Truncated;
        return false;
    }
    value.rayDirection = encodedRayDirection / kSmallVectorScale;
    if (!IsFinite(value.rayDirection)) {
        error = DecodeError::NonFiniteVector;
        return false;
    }
    value.exitImpact = reader.ReadBit();
    if (reader.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    return true;
}

bool ReadRequiredPresence(BitReader& reader, DecodeError& error) {
    const bool present = reader.ReadBit();
    if (reader.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (!present) {
        error = DecodeError::MissingRequiredParameter;
        return false;
    }
    return true;
}

} // namespace

bool DecodeOne(BitReader& reader, const ActorResolver& resolveActor,
               ServerHandleClientHitsOne& output, DecodeError& error) {
    if (reader.BitsLeft() > kMaxRpcBits) {
        error = DecodeError::Oversized;
        return false;
    }
    BitReader trial = reader;
    ServerHandleClientHitsOne decoded;
    error = DecodeError::None;
    const size_t startBit = trial.BitPos();

    const uint32_t handle = trial.SerializeInt(kRoWeaponMaxHandle);
    if (trial.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (handle != kServerHandleClientHitsOne) {
        error = DecodeError::UnsupportedHandle;
        return false;
    }

    // UE3 puts a one-bit Send marker before every non-bool RPC parameter.
    if (!ReadRequiredPresence(trial, error) ||
        !ReadImpactInfo(trial, resolveActor, decoded.impact, error) ||
        !ReadRequiredPresence(trial, error)) {
        return false;
    }
    decoded.firedMode = trial.ReadByte();
    if (trial.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (!ReadRequiredPresence(trial, error) ||
        !ReadVector(trial, decoded.firstHitLocation, error) ||
        !ReadRequiredPresence(trial, error) ||
        !ReadVector(trial, decoded.startTrace, error)) {
        return false;
    }

    decoded.consumedBits = trial.BitPos() - startBit;
    reader = trial;
    output = decoded;
    return true;
}

DecodeResult DecodeServerHandleClientHitsOne(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits,
    const ActorResolver& resolveActor) {
    DecodeResult result;
    if (!payload || payloadBits == 0) {
        result.error = DecodeError::InvalidBuffer;
        return result;
    }
    if (payloadBits > kMaxRpcBits) {
        result.error = DecodeError::Oversized;
        return result;
    }
    if (payloadBytes < (payloadBits + 7u) / 8u) {
        result.error = DecodeError::InvalidBuffer;
        return result;
    }

    BitReader reader(payload, payloadBytes, payloadBits);
    DecodeError error = DecodeError::None;
    if (!DecodeOne(reader, resolveActor, result.rpc, error)) {
        result.error = error;
        return result;
    }
    result.consumedBits = reader.BitPos();
    if (reader.BitsLeft() != 0) {
        result.error = DecodeError::TrailingBits;
        return result;
    }
    result.error = DecodeError::None;
    return result;
}

namespace {

constexpr uint32_t kFirstActorChannel = 2;
constexpr double kCompressedVectorMinimum = -1048576.0;
constexpr double kCompressedVectorMaximum = 1048575.0;

M61VisualResult M61Failure(M61VisualError error) {
    M61VisualResult result;
    result.error = error;
    return result;
}

bool IsValidM61Channel(uint32_t channelIndex) {
    return channelIndex >= kFirstActorChannel &&
           channelIndex < ActorRepl::kDynamicChannelMax;
}

bool IsValidReliableSequence(uint32_t sequence) {
    return sequence < ActorRepl::kDynamicChannelMax;
}

bool IsCompressedComponent(float value) {
    if (!std::isfinite(value)) return false;
    const double rounded = std::floor(static_cast<double>(value) + 0.5);
    return rounded >= kCompressedVectorMinimum &&
           rounded <= kCompressedVectorMaximum;
}

bool IsCompressedVector(const Vector3& value) {
    return IsCompressedComponent(value.x) &&
           IsCompressedComponent(value.y) &&
           IsCompressedComponent(value.z);
}

M61VisualError ValidateM61Snapshot(const M61VisualSnapshot& snapshot) {
    if (!IsFinite(snapshot.positionUu) ||
        !IsFinite(snapshot.velocityUuPerSecond) ||
        !std::isfinite(snapshot.fuseSeconds)) {
        return M61VisualError::NonFiniteValue;
    }
    if (!IsCompressedVector(snapshot.positionUu) ||
        !IsCompressedVector(snapshot.velocityUuPerSecond)) {
        return M61VisualError::VectorOutOfRange;
    }
    if (snapshot.fuseSeconds < 0.0f ||
        snapshot.fuseSeconds > kM61MaxReplicatedFuseSeconds) {
        return M61VisualError::InvalidFuse;
    }
    if (snapshot.instigator) {
        const ActorRepl::NetGUIDRef& ref = *snapshot.instigator;
        const uint32_t bound = ref.isDynamic
            ? ActorRepl::kDynamicChannelMax
            : ActorRepl::kStaticObjectMax;
        if (ref.index >= bound || (!ref.isDynamic && ref.index == 0)) {
            return M61VisualError::InvalidInstigator;
        }
    }
    return M61VisualError::None;
}

M61VisualError ValidateM61Fuse(float fuseSeconds, bool detonation) {
    if (!std::isfinite(fuseSeconds)) {
        return M61VisualError::NonFiniteValue;
    }
    if (detonation) {
        return fuseSeconds >= kM61MinDetonationFuseSeconds &&
                       fuseSeconds <= 0.0f
                   ? M61VisualError::None
                   : M61VisualError::InvalidFuse;
    }
    return fuseSeconds >= 0.0f &&
                   fuseSeconds <= kM61MaxReplicatedFuseSeconds
               ? M61VisualError::None
               : M61VisualError::InvalidFuse;
}

PacketCodec::Bunch MakeM61UnreliableBunch(uint32_t channelIndex,
                                          const BitWriter& writer) {
    PacketCodec::Bunch bunch;
    // For a non-open unreliable bunch UE3 does not put ChType on the wire.
    // Leave chType/sequence at zero instead of claiming CHTYPE_Actor in memory.
    bunch.bControl = false;
    bunch.bOpen = false;
    bunch.bClose = false;
    bunch.bReliable = false;
    bunch.chIndex = channelIndex;
    bunch.chType = 0;
    bunch.chSequence = 0;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

void WriteM61Rotation(BitWriter& writer, const M61VisualSnapshot& snapshot) {
    writer.SerializeInt(kM61RotationHandle, kM61ProjectileMaxHandle);
    ActorRepl::WriteCompressedRotator(
        writer, snapshot.pitch, snapshot.yaw, snapshot.roll);
}

void WriteM61Velocity(BitWriter& writer, const M61VisualSnapshot& snapshot) {
    writer.SerializeInt(kM61VelocityHandle, kM61ProjectileMaxHandle);
    ActorRepl::WriteCompressedVector(
        writer, snapshot.velocityUuPerSecond.x,
        snapshot.velocityUuPerSecond.y,
        snapshot.velocityUuPerSecond.z);
}

} // namespace

M61VisualResult EncodeM61Open(uint32_t channelIndex,
                              uint32_t reliableSequence,
                              const M61VisualSnapshot& snapshot) {
    if (!IsValidM61Channel(channelIndex)) {
        return M61Failure(M61VisualError::InvalidChannel);
    }
    if (!IsValidReliableSequence(reliableSequence)) {
        return M61Failure(M61VisualError::InvalidSequence);
    }
    if (const M61VisualError error = ValidateM61Snapshot(snapshot);
        error != M61VisualError::None) {
        return M61Failure(error);
    }

    ActorRepl::ActorOpenHeader header;
    header.classRef = {false, kM61ProjectileClassRef};
    header.locX = snapshot.positionUu.x;
    header.locY = snapshot.positionUu.y;
    header.locZ = snapshot.positionUu.z;
    // M61 does not set bNetInitialRotation. Rotation is a replicated Actor
    // property after the compressed initial-location header.
    header.hasRotation = false;

    M61VisualResult result;
    result.bunch = ActorRepl::MakeOpeningActorBunch(
        channelIndex, reliableSequence, header,
        [&snapshot](BitWriter& writer) {
            // This is retail capture order, not numeric handle order. UE3 walks
            // the owning replication blocks, whose property handles are sparse.
            if (snapshot.instigator) {
                ActorRepl::WritePropObject(
                    writer, kM61InstigatorHandle,
                    kM61ProjectileMaxHandle, *snapshot.instigator);
            }
            WriteM61Rotation(writer, snapshot);
            WriteM61Velocity(writer, snapshot);
            ActorRepl::WritePropFloat(
                writer, kM61FuseLengthHandle,
                kM61ProjectileMaxHandle, snapshot.fuseSeconds);
        });
    result.error = M61VisualError::None;
    return result;
}

M61VisualResult EncodeM61Update(uint32_t channelIndex,
                                const M61VisualSnapshot& snapshot) {
    if (!IsValidM61Channel(channelIndex)) {
        return M61Failure(M61VisualError::InvalidChannel);
    }
    if (const M61VisualError error = ValidateM61Snapshot(snapshot);
        error != M61VisualError::None) {
        return M61Failure(error);
    }

    BitWriter writer;
    writer.SerializeInt(kM61LocationHandle, kM61ProjectileMaxHandle);
    ActorRepl::WriteCompressedVector(
        writer, snapshot.positionUu.x, snapshot.positionUu.y,
        snapshot.positionUu.z);
    WriteM61Rotation(writer, snapshot);
    WriteM61Velocity(writer, snapshot);
    ActorRepl::WritePropFloat(
        writer, kM61FuseLengthHandle,
        kM61ProjectileMaxHandle, snapshot.fuseSeconds);

    M61VisualResult result;
    result.error = M61VisualError::None;
    result.bunch = MakeM61UnreliableBunch(channelIndex, writer);
    return result;
}

M61VisualResult EncodeM61FuseUpdate(uint32_t channelIndex,
                                    float fuseSeconds) {
    if (!IsValidM61Channel(channelIndex)) {
        return M61Failure(M61VisualError::InvalidChannel);
    }
    if (const M61VisualError error = ValidateM61Fuse(fuseSeconds, false);
        error != M61VisualError::None) {
        return M61Failure(error);
    }

    BitWriter writer;
    ActorRepl::WritePropFloat(
        writer, kM61FuseLengthHandle,
        kM61ProjectileMaxHandle, fuseSeconds);
    M61VisualResult result;
    result.error = M61VisualError::None;
    result.bunch = MakeM61UnreliableBunch(channelIndex, writer);
    return result;
}

M61VisualResult EncodeM61Detonation(uint32_t channelIndex,
                                    float fuseSeconds) {
    if (!IsValidM61Channel(channelIndex)) {
        return M61Failure(M61VisualError::InvalidChannel);
    }
    if (const M61VisualError error = ValidateM61Fuse(fuseSeconds, true);
        error != M61VisualError::None) {
        return M61Failure(error);
    }

    BitWriter writer;
    ActorRepl::WritePropBool(
        writer, kM61CollideActorsHandle,
        kM61ProjectileMaxHandle, false);
    ActorRepl::WritePropBool(
        writer, kM61TearOffHandle,
        kM61ProjectileMaxHandle, true);
    ActorRepl::WritePropFloat(
        writer, kM61FuseLengthHandle,
        kM61ProjectileMaxHandle, fuseSeconds);

    M61VisualResult result;
    result.error = M61VisualError::None;
    result.bunch = MakeM61UnreliableBunch(channelIndex, writer);
    return result;
}

M61VisualResult EncodeM61Close(uint32_t channelIndex,
                               uint32_t reliableSequence) {
    if (!IsValidM61Channel(channelIndex)) {
        return M61Failure(M61VisualError::InvalidChannel);
    }
    if (!IsValidReliableSequence(reliableSequence)) {
        return M61Failure(M61VisualError::InvalidSequence);
    }

    M61VisualResult result;
    result.error = M61VisualError::None;
    result.bunch.bControl = true;
    result.bunch.bOpen = false;
    result.bunch.bClose = true;
    result.bunch.bReliable = true;
    result.bunch.chIndex = channelIndex;
    result.bunch.chType = 2;
    result.bunch.chSequence = reliableSequence;
    result.bunch.payloadBits = 0;
    return result;
}

M61VisualResult M61VisualLifecycle::Spawn(
    uint32_t reliableSequence, const M61VisualSnapshot& snapshot) {
    if (m_state != M61VisualState::Dormant) {
        return M61Failure(M61VisualError::InvalidState);
    }
    M61VisualResult result =
        EncodeM61Open(m_channelIndex, reliableSequence, snapshot);
    if (result.valid()) m_state = M61VisualState::Active;
    return result;
}

M61VisualResult M61VisualLifecycle::Update(
    const M61VisualSnapshot& snapshot) {
    if (m_state != M61VisualState::Active) {
        return M61Failure(M61VisualError::InvalidState);
    }
    return EncodeM61Update(m_channelIndex, snapshot);
}

M61VisualResult M61VisualLifecycle::UpdateFuse(float fuseSeconds) {
    if (m_state != M61VisualState::Active) {
        return M61Failure(M61VisualError::InvalidState);
    }
    return EncodeM61FuseUpdate(m_channelIndex, fuseSeconds);
}

M61VisualResult M61VisualLifecycle::Detonate(float fuseSeconds) {
    if (m_state != M61VisualState::Active) {
        return M61Failure(M61VisualError::InvalidState);
    }
    M61VisualResult result =
        EncodeM61Detonation(m_channelIndex, fuseSeconds);
    if (result.valid()) m_state = M61VisualState::Detonated;
    return result;
}

M61VisualResult M61VisualLifecycle::Close(uint32_t reliableSequence) {
    if (m_state != M61VisualState::Active &&
        m_state != M61VisualState::Detonated) {
        return M61Failure(M61VisualError::InvalidState);
    }
    M61VisualResult result =
        EncodeM61Close(m_channelIndex, reliableSequence);
    if (result.valid()) m_state = M61VisualState::Closed;
    return result;
}

M61VisualChannelPool::M61VisualChannelPool(uint32_t firstChannel,
                                           uint32_t lastChannel)
    : m_firstChannel(firstChannel),
      m_lastChannel(lastChannel),
      m_nextChannel(firstChannel) {
    m_valid = firstChannel >= kFirstActorChannel &&
              firstChannel <= lastChannel &&
              lastChannel < ActorRepl::kDynamicChannelMax;
}

std::optional<uint32_t> M61VisualChannelPool::ChannelFor(
    uint64_t projectileKey) const {
    const auto found = m_active.find(projectileKey);
    if (found == m_active.end()) return std::nullopt;
    return found->second.channel;
}

std::optional<uint64_t> M61VisualChannelPool::ProjectileForChannel(
    uint32_t channel) const {
    const auto found = std::find_if(
        m_active.begin(), m_active.end(),
        [channel](const auto& entry) {
            return entry.second.channel == channel;
        });
    return found == m_active.end()
        ? std::nullopt
        : std::optional<uint64_t>(found->first);
}

bool M61VisualChannelPool::ChannelInUse(uint32_t channel) const {
    return std::any_of(
        m_active.begin(), m_active.end(),
        [channel](const auto& entry) {
            return entry.second.channel == channel;
        });
}

std::optional<uint32_t> M61VisualChannelPool::FindFreeChannel(
    const ChannelAvailable& channelAvailable) const {
    if (!m_valid) return std::nullopt;
    const uint64_t count =
        static_cast<uint64_t>(m_lastChannel) - m_firstChannel + 1u;
    uint32_t candidate = m_nextChannel;
    for (uint64_t scanned = 0; scanned < count; ++scanned) {
        if (!ChannelInUse(candidate) &&
            (!channelAvailable || channelAvailable(candidate))) {
            return candidate;
        }
        candidate = candidate == m_lastChannel ? m_firstChannel
                                                : candidate + 1u;
    }
    return std::nullopt;
}

uint32_t M61VisualChannelPool::NextReliableSequence(uint32_t channel) const {
    const auto found = m_reliableSequence.find(channel);
    const uint32_t current = found == m_reliableSequence.end()
        ? 0u
        : found->second;
    return (current + 1u) % ActorRepl::kDynamicChannelMax;
}

void M61VisualChannelPool::CommitReliableSequence(uint32_t channel,
                                                  uint32_t sequence) {
    m_reliableSequence[channel] = sequence;
}

void M61VisualChannelPool::AdvanceAllocationCursor(uint32_t channel) {
    m_nextChannel = channel == m_lastChannel ? m_firstChannel : channel + 1u;
}

M61VisualBatchResult M61VisualChannelPool::Spawn(
    uint64_t projectileKey, const M61VisualSnapshot& snapshot,
    const ChannelAvailable& channelAvailable) {
    M61VisualBatchResult result;
    if (!m_valid) {
        result.error = M61VisualError::InvalidChannelRange;
        return result;
    }
    if (projectileKey == 0) {
        result.error = M61VisualError::UnknownProjectile;
        return result;
    }
    if (m_active.find(projectileKey) != m_active.end()) {
        result.error = M61VisualError::DuplicateProjectile;
        return result;
    }
    const std::optional<uint32_t> channel =
        FindFreeChannel(channelAvailable);
    if (!channel) {
        result.error = M61VisualError::ChannelExhausted;
        return result;
    }
    const uint32_t sequence = NextReliableSequence(*channel);
    M61VisualResult encoded = EncodeM61Open(*channel, sequence, snapshot);
    if (!encoded.valid()) {
        result.error = encoded.error;
        return result;
    }

    m_active.emplace(projectileKey, ActiveVisual{*channel, false, false});
    CommitReliableSequence(*channel, sequence);
    AdvanceAllocationCursor(*channel);
    result.error = M61VisualError::None;
    result.bunches.push_back(std::move(encoded.bunch));
    return result;
}

M61VisualBatchResult M61VisualChannelPool::Update(
    uint64_t projectileKey, const M61VisualSnapshot& snapshot) const {
    M61VisualBatchResult result;
    const auto found = m_active.find(projectileKey);
    if (found == m_active.end()) {
        result.error = M61VisualError::UnknownProjectile;
        return result;
    }
    if (found->second.closeQueued || found->second.suppressed) {
        result.error = M61VisualError::InvalidState;
        return result;
    }
    M61VisualResult encoded = EncodeM61Update(found->second.channel, snapshot);
    result.error = encoded.error;
    if (encoded.valid()) result.bunches.push_back(std::move(encoded.bunch));
    return result;
}

M61VisualBatchResult M61VisualChannelPool::UpdateFuse(
    uint64_t projectileKey, float fuseSeconds) const {
    M61VisualBatchResult result;
    const auto found = m_active.find(projectileKey);
    if (found == m_active.end()) {
        result.error = M61VisualError::UnknownProjectile;
        return result;
    }
    if (found->second.closeQueued || found->second.suppressed) {
        result.error = M61VisualError::InvalidState;
        return result;
    }
    M61VisualResult encoded =
        EncodeM61FuseUpdate(found->second.channel, fuseSeconds);
    result.error = encoded.error;
    if (encoded.valid()) result.bunches.push_back(std::move(encoded.bunch));
    return result;
}

M61VisualBatchResult M61VisualChannelPool::DetonateAndClose(
    uint64_t projectileKey, float fuseSeconds) {
    M61VisualBatchResult result;
    const auto found = m_active.find(projectileKey);
    if (found == m_active.end()) {
        result.error = M61VisualError::UnknownProjectile;
        return result;
    }
    if (found->second.closeQueued || found->second.suppressed) {
        result.error = M61VisualError::InvalidState;
        return result;
    }
    const uint32_t channel = found->second.channel;
    M61VisualResult detonation = EncodeM61Detonation(channel, fuseSeconds);
    if (!detonation.valid()) {
        result.error = detonation.error;
        return result;
    }
    const uint32_t closeSequence = NextReliableSequence(channel);
    M61VisualResult close = EncodeM61Close(channel, closeSequence);
    if (!close.valid()) {
        result.error = close.error;
        return result;
    }

    CommitReliableSequence(channel, closeSequence);
    found->second.closeQueued = true;
    result.error = M61VisualError::None;
    result.bunches.push_back(std::move(detonation.bunch));
    result.bunches.push_back(std::move(close.bunch));
    return result;
}

M61VisualBatchResult M61VisualChannelPool::Close(uint64_t projectileKey) {
    M61VisualBatchResult result;
    const auto found = m_active.find(projectileKey);
    if (found == m_active.end()) {
        result.error = M61VisualError::UnknownProjectile;
        return result;
    }
    if (found->second.closeQueued || found->second.suppressed) {
        result.error = M61VisualError::InvalidState;
        return result;
    }
    const uint32_t channel = found->second.channel;
    const uint32_t closeSequence = NextReliableSequence(channel);
    M61VisualResult close = EncodeM61Close(channel, closeSequence);
    if (!close.valid()) {
        result.error = close.error;
        return result;
    }

    CommitReliableSequence(channel, closeSequence);
    found->second.closeQueued = true;
    result.error = M61VisualError::None;
    result.bunches.push_back(std::move(close.bunch));
    return result;
}

bool M61VisualChannelPool::AcknowledgeClose(uint32_t channel) {
    const std::optional<uint64_t> projectile = ProjectileForChannel(channel);
    if (!projectile) return false;
    const auto found = m_active.find(*projectile);
    if (found == m_active.end() || !found->second.closeQueued ||
        found->second.suppressed) {
        return false;
    }
    m_active.erase(found);
    return true;
}

bool M61VisualChannelPool::IsCloseQueued(uint32_t channel) const {
    const std::optional<uint64_t> projectile = ProjectileForChannel(channel);
    if (!projectile) return false;
    const auto found = m_active.find(*projectile);
    return found != m_active.end() && found->second.closeQueued;
}

bool M61VisualChannelPool::SuppressChannel(uint32_t channel) {
    const std::optional<uint64_t> projectile = ProjectileForChannel(channel);
    if (!projectile) return false;
    auto found = m_active.find(*projectile);
    if (found == m_active.end()) return false;
    found->second.suppressed = true;
    return true;
}

bool M61VisualChannelPool::IsSuppressed(uint32_t channel) const {
    const std::optional<uint64_t> projectile = ProjectileForChannel(channel);
    if (!projectile) return false;
    const auto found = m_active.find(*projectile);
    return found != m_active.end() && found->second.suppressed;
}

void M61VisualChannelPool::Clear() {
    m_active.clear();
    m_reliableSequence.clear();
    m_nextChannel = m_firstChannel;
}

} // namespace WeaponCombatRepl
