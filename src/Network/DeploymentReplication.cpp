#include "Network/DeploymentReplication.h"

#include <cmath>

#include "Network/ActorReplication.h"

namespace DeploymentRepl {
namespace {

bool IsValidBuffer(const uint8_t* payload, size_t payloadBytes,
                   size_t payloadBits) noexcept {
    if (!payload || payloadBits == 0) return false;
    const size_t requiredBytes =
        payloadBits / 8u + static_cast<size_t>((payloadBits % 8u) != 0);
    return payloadBytes >= requiredBytes;
}

bool HasValidPlayerName(const std::string& name) noexcept {
    return !name.empty() && name.size() <= kMaximumRetailPlayerNameBytes &&
           name.find('\0') == std::string::npos;
}

bool IsCompressibleVector(const Vector3& value) noexcept {
    auto validComponent = [](float component) {
        if (!std::isfinite(component)) return false;
        const double rounded =
            std::floor(static_cast<double>(component) + 0.5);
        return rounded >= -1048576.0 && rounded <= 1048575.0;
    };
    return validComponent(value.x) && validComponent(value.y) &&
           validComponent(value.z);
}

bool IsValidActorChannel(uint32_t channel) noexcept {
    return channel >= 2 && channel < ActorRepl::kDynamicChannelMax;
}

bool IsValidReliableSequence(uint32_t sequence) noexcept {
    return sequence < ParticipantActorChannelMap::kReliableSequenceLimit;
}

} // namespace

bool DecodeOneServerSetSpawnSelect(BitReader& reader,
                                   ServerSetSpawnSelect& output,
                                   DecodeError& error) {
    BitReader trial = reader;
    ServerSetSpawnSelect decoded;
    const size_t startBit = trial.BitPos();

    const uint32_t handle =
        trial.SerializeInt(kRoPlayerControllerMaxHandle);
    if (trial.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (handle != kServerSetSpawnSelectHandle) {
        error = DecodeError::UnsupportedHandle;
        return false;
    }

    decoded.presentOnWire = trial.ReadBit();
    if (decoded.presentOnWire) {
        decoded.encodedSelection = trial.ReadByte();
    }
    if (trial.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }

    decoded.consumedBits = trial.BitPos() - startBit;
    reader = trial;
    output = decoded;
    error = DecodeError::None;
    return true;
}

bool DecodeOneServerSetReadyToSpawn(BitReader& reader,
                                    ServerSetReadyToSpawn& output,
                                    DecodeError& error) {
    BitReader trial = reader;
    ServerSetReadyToSpawn decoded;
    const size_t startBit = trial.BitPos();

    const uint32_t handle =
        trial.SerializeInt(kRoPlayerControllerMaxHandle);
    if (trial.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (handle != kServerSetReadyToSpawnHandle) {
        error = DecodeError::UnsupportedHandle;
        return false;
    }

    decoded.presentOnWire = trial.ReadBit();
    if (decoded.presentOnWire) {
        // UByteProperty::NetSerializeItem uses SerializeBits for enum-backed
        // bytes.  ESpawnReadyStatus has three real values plus _MAX, yielding a
        // fixed ceil(log2(NumEnums - 1)) == 2 bits on the wire.
        decoded.status =
            static_cast<uint8_t>(trial.ReadBits(kReadyStatusBits));
    }
    if (trial.IsOverflowed()) {
        error = DecodeError::Truncated;
        return false;
    }
    if (decoded.status > kMaxValidReadyStatus) {
        error = DecodeError::InvalidReadyStatus;
        return false;
    }

    decoded.consumedBits = trial.BitPos() - startBit;
    reader = trial;
    output = decoded;
    error = DecodeError::None;
    return true;
}

SpawnSelectDecodeResult DecodeServerSetSpawnSelect(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    SpawnSelectDecodeResult result;
    if (!IsValidBuffer(payload, payloadBytes, payloadBits)) return result;

    BitReader reader(payload, payloadBytes, payloadBits);
    DecodeError error = DecodeError::None;
    if (!DecodeOneServerSetSpawnSelect(reader, result.rpc, error)) {
        result.error = error;
        return result;
    }
    if (reader.BitsLeft() != 0) {
        result.error = DecodeError::TrailingBits;
        return result;
    }
    result.error = DecodeError::None;
    return result;
}

ReadyToSpawnDecodeResult DecodeServerSetReadyToSpawn(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    ReadyToSpawnDecodeResult result;
    if (!IsValidBuffer(payload, payloadBytes, payloadBits)) return result;

    BitReader reader(payload, payloadBytes, payloadBits);
    DecodeError error = DecodeError::None;
    if (!DecodeOneServerSetReadyToSpawn(reader, result.rpc, error)) {
        result.error = error;
        return result;
    }
    if (reader.BitsLeft() != 0) {
        result.error = DecodeError::TrailingBits;
        return result;
    }
    result.error = DecodeError::None;
    return result;
}

ReadyToSpawnBunchDecodeResult DecodeServerSetReadyToSpawnBunch(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    ReadyToSpawnBunchDecodeResult result;
    if (!IsValidBuffer(payload, payloadBytes, payloadBits)) return result;

    BitReader reader(payload, payloadBytes, payloadBits);
    DecodeError error = DecodeError::None;
    ServerSetReadyToSpawn first;
    if (!DecodeOneServerSetReadyToSpawn(reader, first, error)) {
        result.error = error;
        return result;
    }
    result.bunch.transitions[0] = first;
    result.bunch.transitionCount = 1;
    if (reader.BitsLeft() == 0) {
        result.error = DecodeError::None;
        return result;
    }

    auto readExpectedHandle = [&](uint32_t expected) {
        const uint32_t actual =
            reader.SerializeInt(kRoPlayerControllerMaxHandle);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return false;
        }
        if (actual != expected) {
            result.error = DecodeError::UnsupportedHandle;
            return false;
        }
        return true;
    };
    auto requireEnd = [&]() {
        if (reader.BitsLeft() != 0) {
            result.error = DecodeError::TrailingBits;
            return false;
        }
        result.error = DecodeError::None;
        return true;
    };

    if (first.status == 0u) {
        // Exact retail prefixes share Ready(default) +
        // ServerSetThirdPersonSpectate() + ForceOnly(explicit).
        if (!readExpectedHandle(kServerSetThirdPersonSpectateHandle)) {
            return result;
        }
        ServerSetReadyToSpawn second;
        if (!DecodeOneServerSetReadyToSpawn(reader, second, error)) {
            result.error = error;
            return result;
        }
        if (!second.presentOnWire || second.status != 1u) {
            result.error = DecodeError::InvalidReadyStatus;
            return result;
        }
        result.bunch.transitions[1] = second;
        result.bunch.transitionCount = 2;

        const uint32_t companion =
            reader.SerializeInt(kRoPlayerControllerMaxHandle);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (companion == kServerStopVoiceChatHandle) {
            const bool force = reader.ReadBit();
            if (reader.IsOverflowed()) {
                result.error = DecodeError::Truncated;
                return result;
            }
            if (force) {
                result.error = DecodeError::UnsupportedSequence;
                return result;
            }
            result.bunch.pattern =
                ReadyToSpawnBunchPattern::ReadyForceOnlyStopVoiceChat;
            (void)requireEnd();
            return result;
        }
        if (companion != kServerAcknowledgePossessionHandle) {
            result.error = DecodeError::UnsupportedHandle;
            return result;
        }

        const bool pawnPresent = reader.ReadBit();
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (!pawnPresent) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        const ActorRepl::NetGUIDRef pawn = ActorRepl::ReadNetGUID(reader);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (!pawn.isDynamic || pawn.index == 0) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        if (!readExpectedHandle(kServerEnableFocusHandle)) return result;
        const bool enableFocus = reader.ReadBit();
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (enableFocus) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        result.bunch.pattern = ReadyToSpawnBunchPattern::
            ReadyForceOnlyAcknowledgePossession;
        result.bunch.acknowledgedPawnChannel = pawn.index;
        (void)requireEnd();
        return result;
    }

    if (first.status == 2u) {
        if (!readExpectedHandle(kServerSetSpectatorLocationHandle)) {
            return result;
        }
        const bool locationPresent = reader.ReadBit();
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (!locationPresent) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        Vector3 location;
        ActorRepl::ReadCompressedVector(
            reader, location.x, location.y, location.z);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (!std::isfinite(location.x) || !std::isfinite(location.y) ||
            !std::isfinite(location.z)) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        result.bunch.pattern =
            ReadyToSpawnBunchPattern::NotReadySpectatorLocation;
        result.bunch.spectatorLocation = location;
        (void)requireEnd();
        return result;
    }

    result.error = DecodeError::UnsupportedSequence;
    return result;
}

SpawnVolumeViewTargetBunchDecodeResult DecodeSpawnVolumeViewTargetBunch(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    SpawnVolumeViewTargetBunchDecodeResult result;
    if (!IsValidBuffer(payload, payloadBytes, payloadBits)) return result;

    BitReader reader(payload, payloadBytes, payloadBits);
    auto readExpectedHandle = [&](uint32_t expected) {
        const uint32_t actual =
            reader.SerializeInt(kRoPlayerControllerMaxHandle);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return false;
        }
        if (actual != expected) {
            result.error = DecodeError::UnsupportedHandle;
            return false;
        }
        return true;
    };
    auto requireEnd = [&]() {
        if (reader.BitsLeft() != 0) {
            result.error = DecodeError::TrailingBits;
            return false;
        }
        result.error = DecodeError::None;
        return true;
    };
    auto appendSpawnSelect = [&](const ServerSetSpawnSelect& rpc) {
        if (result.bunch.actionCount >= result.bunch.actions.size()) {
            result.error = DecodeError::UnsupportedSequence;
            return false;
        }
        SpawnVolumeDeploymentAction& action =
            result.bunch.actions[result.bunch.actionCount++];
        action.type = SpawnVolumeDeploymentActionType::SpawnSelect;
        action.spawnSelect = rpc;
        return true;
    };
    auto appendReady = [&](const ServerSetReadyToSpawn& rpc) {
        if (result.bunch.actionCount >= result.bunch.actions.size()) {
            result.error = DecodeError::UnsupportedSequence;
            return false;
        }
        SpawnVolumeDeploymentAction& action =
            result.bunch.actions[result.bunch.actionCount++];
        action.type = SpawnVolumeDeploymentActionType::ReadyToSpawn;
        action.readyToSpawn = rpc;
        return true;
    };

    if (!readExpectedHandle(kServerSetSpawnVolumeViewTargetHandle)) {
        return result;
    }
    // InternalProcessRemoteFunction prefixes the non-bool CameraActor
    // parameter with a presence bit. The live Compound scene supplies a static
    // map actor, not a dynamic channel or None.
    if (!reader.ReadBit()) {
        result.error = reader.IsOverflowed() ? DecodeError::Truncated
                                             : DecodeError::UnsupportedSequence;
        return result;
    }
    const ActorRepl::NetGUIDRef camera = ActorRepl::ReadNetGUID(reader);
    if (reader.IsOverflowed()) {
        result.error = DecodeError::Truncated;
        return result;
    }
    if (camera.isDynamic || camera.index == 0u) {
        result.error = DecodeError::UnsupportedSequence;
        return result;
    }
    result.bunch.cameraTargetRef = camera.index;

    // Peek transactionally so the real decoder still consumes the complete
    // companion RPC from its handle boundary.
    BitReader companion = reader;
    const uint32_t firstCompanion =
        companion.SerializeInt(kRoPlayerControllerMaxHandle);
    if (companion.IsOverflowed()) {
        result.error = DecodeError::Truncated;
        return result;
    }

    DecodeError nestedError = DecodeError::None;
    if (firstCompanion == kServerSetReadyToSpawnHandle) {
        // Exact 64-bit live bunch:
        // h370(camera) + h434 ForceOnly(explicit) + h275(false).
        ServerSetReadyToSpawn ready;
        if (!DecodeOneServerSetReadyToSpawn(reader, ready, nestedError)) {
            result.error = nestedError;
            return result;
        }
        if (!ready.presentOnWire || ready.status != 1u) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        if (!appendReady(ready)) return result;
        if (!readExpectedHandle(kServerStopVoiceChatHandle)) return result;
        const bool forceStopVoice = reader.ReadBit();
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (forceStopVoice) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        result.bunch.pattern =
            SpawnVolumeViewTargetBunchPattern::ForceOnlyStopVoiceChat;
        (void)requireEnd();
        return result;
    }

    if (firstCompanion != kServerSetSpawnSelectHandle) {
        result.error = DecodeError::UnsupportedHandle;
        return result;
    }

    // Live Skirmish auto-select bunch. ResetSpawnSelection and PostFirstRender
    // each emit the same h261(normal slot), Ready(default), h180 tuple. The
    // selected TeamInfo-array slot may be any normal 128..137 value, but both
    // callbacks must agree on that value. The base form ends at 116 bits; a
    // separately observed 126-bit form appends h89 with an absent/default
    // spectator-location Vector.
    std::optional<uint8_t> repeatedSelection;
    for (uint8_t repetition = 0; repetition < 2u; ++repetition) {
        ServerSetSpawnSelect spawn;
        if (!DecodeOneServerSetSpawnSelect(reader, spawn, nestedError)) {
            result.error = nestedError;
            return result;
        }
        if (!spawn.presentOnWire || spawn.encodedSelection < 128u ||
            spawn.encodedSelection > 137u) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        if (!repeatedSelection.has_value()) {
            repeatedSelection = spawn.encodedSelection;
        } else if (spawn.encodedSelection != *repeatedSelection) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        if (!appendSpawnSelect(spawn)) return result;

        ServerSetReadyToSpawn ready;
        if (!DecodeOneServerSetReadyToSpawn(reader, ready, nestedError)) {
            result.error = nestedError;
            return result;
        }
        if (ready.presentOnWire || ready.status != 0u) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        if (!appendReady(ready)) return result;
        if (!readExpectedHandle(kServerSetThirdPersonSpectateHandle)) {
            return result;
        }
    }
    if (reader.BitsLeft() == 0u) {
        result.bunch.pattern =
            SpawnVolumeViewTargetBunchPattern::SkirmishAutoSelect;
        (void)requireEnd();
        return result;
    }

    // A handle plus its Vector-presence bit is exactly ten bits. Do not let
    // arbitrary padding become a speculative suffix.
    if (reader.BitsLeft() != 10u) {
        result.error = DecodeError::TrailingBits;
        return result;
    }
    if (!readExpectedHandle(kServerSetSpectatorLocationHandle)) return result;
    const bool spectatorLocationPresent = reader.ReadBit();
    if (reader.IsOverflowed()) {
        result.error = DecodeError::Truncated;
        return result;
    }
    if (spectatorLocationPresent) {
        result.error = DecodeError::UnsupportedSequence;
        return result;
    }
    result.bunch.pattern = SpawnVolumeViewTargetBunchPattern::
        SkirmishAutoSelectClearSpectatorLocation;
    (void)requireEnd();
    return result;
}

ChangeVivoxChannelsStateBunchDecodeResult
DecodeChangeVivoxChannelsStateBunch(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    ChangeVivoxChannelsStateBunchDecodeResult result;
    if (!IsValidBuffer(payload, payloadBytes, payloadBits)) return result;
    if (payloadBits < kChangeVivoxChannelsStateRecordBits) {
        result.error = DecodeError::Truncated;
        return result;
    }
    if (payloadBits % kChangeVivoxChannelsStateRecordBits != 0u) {
        result.error = DecodeError::TrailingBits;
        return result;
    }

    const size_t recordCount =
        payloadBits / kChangeVivoxChannelsStateRecordBits;
    if (recordCount == 0u ||
        recordCount > kMaximumVivoxChannelStateRecords) {
        result.error = DecodeError::UnsupportedSequence;
        return result;
    }

    BitReader reader(payload, payloadBytes, payloadBits);
    for (size_t i = 0; i < recordCount; ++i) {
        const uint32_t handle =
            reader.SerializeInt(kRoPlayerControllerMaxHandle);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (handle != kChangeVivoxChannelsStateHandle) {
            result.error = DecodeError::UnsupportedHandle;
            return result;
        }

        // InternalProcessRemoteFunction prefixes both object parameters with a
        // presence bit.  The grounded retail record always supplies both.
        if (!reader.ReadBit()) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        const ActorRepl::NetGUIDRef otherPri =
            ActorRepl::ReadNetGUID(reader);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (!otherPri.isDynamic || otherPri.index == 0u) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }

        if (!reader.ReadBit()) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }
        const ActorRepl::NetGUIDRef localPc =
            ActorRepl::ReadNetGUID(reader);
        if (reader.IsOverflowed()) {
            result.error = DecodeError::Truncated;
            return result;
        }
        if (!localPc.isDynamic || localPc.index == 0u) {
            result.error = DecodeError::UnsupportedSequence;
            return result;
        }

        result.bunch.records[i] = {
            otherPri.index, localPc.index};
        ++result.bunch.recordCount;
    }

    if (reader.BitsLeft() != 0u) {
        result.error = DecodeError::TrailingBits;
        return result;
    }
    result.error = DecodeError::None;
    return result;
}

void WriteOwnerNextRespawnTime(BitWriter& writer, int32_t nextRespawnTime) {
    ActorRepl::WritePropInt(writer, kNextRespawnTimeHandle,
                            kRoPlayerControllerMaxHandle, nextRespawnTime);
}

bool IsValidRetailParticipantCombatState(
    const RetailParticipantCombatState& state) noexcept {
    return ParticipantActorChannelMap::EncodeWirePlayerId(state.participant)
               .has_value() &&
           state.health >= 0 && state.health <= 100 && state.kills >= 0 &&
           state.deaths >= 0 && state.score >= 0;
}

bool IsValidRetailParticipantInitialState(
    const RetailParticipantInitialState& state) noexcept {
    return IsValidRetailParticipantCombatState(state.combat) &&
           ParticipantRoster::IsPlayableTeam(state.serverTeamId) &&
           HasValidPlayerName(state.playerName) &&
           IsCompressibleVector(state.positionUu);
}

bool IsValidRetailRemotePawnSnapshot(
    const RetailRemotePawnSnapshot& snapshot) noexcept {
    return ParticipantActorChannelMap::EncodeWirePlayerId(snapshot.participant)
               .has_value() &&
           snapshot.health >= 0 && snapshot.health <= 100 &&
           IsCompressibleVector(snapshot.positionUu) &&
           IsCompressibleVector(snapshot.velocityUuPerSecond);
}

std::optional<uint32_t> RemotePawnArchetypeForServerTeam(
    uint8_t serverTeamId) noexcept {
    if (serverTeamId == 1) return kRemoteSouthPawnArchetypeRef;
    if (serverTeamId == 2) return kRemoteNorthPawnArchetypeRef;
    return std::nullopt;
}

bool WriteRemotePriInitial(
    BitWriter& writer, const RetailParticipantInitialState& state) {
    if (!IsValidRetailParticipantInitialState(state)) return false;
    const std::optional<std::int32_t> wirePlayerId =
        ParticipantActorChannelMap::EncodeWirePlayerId(
            state.combat.participant);
    if (!wirePlayerId) return false;

    // Ascending net-field handles keep this bounded block deterministic.
    ActorRepl::WritePropInt(writer, 24, kRoPlayerReplicationInfoMaxHandle,
                            state.combat.kills);
    ActorRepl::WritePropBool(writer, 28, kRoPlayerReplicationInfoMaxHandle,
                             state.combat.participant.IsBot());
    ActorRepl::WritePropBool(writer, 31, kRoPlayerReplicationInfoMaxHandle,
                             false); // bWaitingPlayer
    ActorRepl::WritePropBool(writer, 32, kRoPlayerReplicationInfoMaxHandle,
                             false); // bOnlySpectator
    ActorRepl::WritePropBool(writer, 33, kRoPlayerReplicationInfoMaxHandle,
                             false); // bIsSpectator
    ActorRepl::WritePropInt(writer, 36, kRoPlayerReplicationInfoMaxHandle,
                            *wirePlayerId);
    ActorRepl::WritePropString(writer, 37, kRoPlayerReplicationInfoMaxHandle,
                               state.playerName);
    ActorRepl::WritePropInt(writer, 39, kRoPlayerReplicationInfoMaxHandle,
                            state.combat.deaths);
    ActorRepl::WritePropFloat(
        writer, 40, kRoPlayerReplicationInfoMaxHandle,
        static_cast<float>(state.combat.score));
    ActorRepl::WritePropBool(writer, 61, kRoPlayerReplicationInfoMaxHandle,
                             state.combat.dead);
    return true;
}

bool WriteRemotePriTeam(BitWriter& writer, uint32_t teamInfoChannel) {
    if (teamInfoChannel == 0 ||
        teamInfoChannel >= ActorRepl::kDynamicChannelMax) {
        return false;
    }
    ActorRepl::WritePropObject(
        writer, 35, kRoPlayerReplicationInfoMaxHandle,
        ActorRepl::NetGUIDRef{/*isDynamic=*/true, teamInfoChannel});
    return true;
}

bool WriteRemotePriCombat(
    BitWriter& writer, const RetailParticipantCombatState& state,
    bool includeDead) {
    if (!IsValidRetailParticipantCombatState(state)) return false;
    ActorRepl::WritePropInt(writer, 24, kRoPlayerReplicationInfoMaxHandle,
                            state.kills);
    ActorRepl::WritePropInt(writer, 39, kRoPlayerReplicationInfoMaxHandle,
                            state.deaths);
    ActorRepl::WritePropFloat(writer, 40, kRoPlayerReplicationInfoMaxHandle,
                              static_cast<float>(state.score));
    if (includeDead) {
        ActorRepl::WritePropBool(writer, 61, kRoPlayerReplicationInfoMaxHandle,
                                 state.dead);
    }
    return true;
}

bool WriteRemotePawnHealth(
    BitWriter& writer, const RetailParticipantCombatState& state) {
    if (!IsValidRetailParticipantCombatState(state)) return false;
    ActorRepl::WritePropInt(writer, 33, kRoPawnMaxHandle, state.health);
    return true;
}

bool WriteRemotePawnInitial(
    BitWriter& writer, const RetailRemotePawnSnapshot& snapshot,
    uint32_t priChannel) {
    if (!IsValidRetailRemotePawnSnapshot(snapshot) || priChannel == 0 ||
        priChannel >= ActorRepl::kDynamicChannelMax) {
        return false;
    }

    ActorRepl::WritePropByte(writer, 10, kRoPawnMaxHandle,
                             kRemotePawnWalkingPhysics, 4);
    ActorRepl::WritePropObject(
        writer, 32, kRoPawnMaxHandle,
        ActorRepl::NetGUIDRef{/*isDynamic=*/true, priChannel});
    ActorRepl::WritePropInt(writer, 33, kRoPawnMaxHandle, snapshot.health);
    return true;
}

bool WriteRemotePawnMovement(
    BitWriter& writer, const RetailRemotePawnSnapshot& snapshot) {
    if (!IsValidRetailRemotePawnSnapshot(snapshot)) return false;

    writer.SerializeInt(3, kRoPawnMaxHandle);
    ActorRepl::WriteCompressedVector(
        writer, snapshot.velocityUuPerSecond.x,
        snapshot.velocityUuPerSecond.y, snapshot.velocityUuPerSecond.z);
    ActorRepl::WritePropByte(writer, 10, kRoPawnMaxHandle,
                             kRemotePawnWalkingPhysics, 4);
    writer.SerializeInt(12, kRoPawnMaxHandle);
    ActorRepl::WriteCompressedRotator(
        writer, snapshot.pitch, snapshot.yaw, snapshot.roll);
    writer.SerializeInt(13, kRoPawnMaxHandle);
    ActorRepl::WriteCompressedVector(
        writer, snapshot.positionUu.x, snapshot.positionUu.y,
        snapshot.positionUu.z);
    return true;
}

bool WriteRemotePawnDeathCore(BitWriter& writer, int negativeHealth) {
    if (negativeHealth >= 0) return false;

    ActorRepl::WritePropBool(writer, 20, kRoPawnMaxHandle, true);
    ActorRepl::WritePropInt(writer, 33, kRoPawnMaxHandle, negativeHealth);
    ActorRepl::WritePropObject(
        writer, 32, kRoPawnMaxHandle,
        ActorRepl::NetGUIDRef{/*isDynamic=*/true, 0});
    return true;
}

std::optional<PacketCodec::Bunch> MakeRemotePawnOpeningBunch(
    uint32_t pawnChannel, uint32_t reliableSequence, uint8_t serverTeamId,
    uint32_t priChannel, const RetailRemotePawnSnapshot& snapshot) {
    const std::optional<uint32_t> archetype =
        RemotePawnArchetypeForServerTeam(serverTeamId);
    if (!IsValidActorChannel(pawnChannel) ||
        !IsValidReliableSequence(reliableSequence) || !archetype ||
        !IsValidRetailRemotePawnSnapshot(snapshot) || priChannel == 0 ||
        priChannel >= ActorRepl::kDynamicChannelMax) {
        return std::nullopt;
    }

    ActorRepl::ActorOpenHeader header;
    header.classRef = ActorRepl::NetGUIDRef{
        /*isDynamic=*/false, *archetype};
    header.locX = snapshot.positionUu.x;
    header.locY = snapshot.positionUu.y;
    header.locZ = snapshot.positionUu.z;
    // These selected ROPawn archetypes do not serialize initial rotation.
    header.hasRotation = false;
    return ActorRepl::MakeOpeningActorBunch(
        pawnChannel, reliableSequence, header,
        [&](BitWriter& writer) {
            (void)WriteRemotePawnInitial(writer, snapshot, priChannel);
        });
}

std::optional<PacketCodec::Bunch> MakeRemotePawnMovementBunch(
    uint32_t pawnChannel, const RetailRemotePawnSnapshot& snapshot) {
    if (!IsValidActorChannel(pawnChannel)) return std::nullopt;
    BitWriter writer;
    if (!WriteRemotePawnMovement(writer, snapshot)) return std::nullopt;

    PacketCodec::Bunch bunch;
    bunch.bReliable = false;
    bunch.chIndex = pawnChannel;
    bunch.chType = 2;
    bunch.chSequence = 0;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

std::optional<PacketCodec::Bunch> MakeRemotePawnDeathBunch(
    uint32_t pawnChannel, int negativeHealth) {
    if (!IsValidActorChannel(pawnChannel)) return std::nullopt;
    BitWriter writer;
    if (!WriteRemotePawnDeathCore(writer, negativeHealth)) {
        return std::nullopt;
    }

    PacketCodec::Bunch bunch;
    bunch.bReliable = false;
    bunch.chIndex = pawnChannel;
    bunch.chType = 2;
    bunch.chSequence = 0;
    bunch.payload = writer.GetBytes();
    bunch.payloadBits = static_cast<uint32_t>(writer.NumBits());
    return bunch;
}

std::optional<PacketCodec::Bunch> MakeRemotePawnCloseBunch(
    uint32_t pawnChannel, uint32_t reliableSequence) {
    if (!IsValidActorChannel(pawnChannel) ||
        !IsValidReliableSequence(reliableSequence)) {
        return std::nullopt;
    }

    PacketCodec::Bunch bunch;
    bunch.bControl = true;
    bunch.bClose = true;
    bunch.bReliable = true;
    bunch.chIndex = pawnChannel;
    bunch.chType = 2;
    bunch.chSequence = reliableSequence;
    bunch.payloadBits = 0;
    return bunch;
}

} // namespace DeploymentRepl
