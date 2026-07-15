// Exact, bounded decoders for the retail deployment-selection RPCs.
//
// Both functions are ROPlayerController calls, so their handles use UE3's
// SerializeInt(handle, 531).  Each non-bool parameter starts with the RPC
// presence bit emitted by InternalProcessRemoteFunction.  The ready status is
// an enum-backed byte: UByteProperty serializes it as a fixed two-bit field,
// not as a raw byte or a ranged SerializeInt.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "Game/ParticipantRoster.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/PacketCodec.h"

namespace DeploymentRepl {

constexpr uint32_t kRoPlayerControllerMaxHandle = 531;
constexpr uint32_t kServerSetSpawnSelectHandle = 261;
constexpr uint32_t kServerSetReadyToSpawnHandle = 434;
constexpr uint32_t kServerSetSpawnVolumeViewTargetHandle = 370;
constexpr uint32_t kServerSetThirdPersonSpectateHandle = 180;
constexpr uint32_t kServerStopVoiceChatHandle = 275;
constexpr uint32_t kServerAcknowledgePossessionHandle = 44;
constexpr uint32_t kServerEnableFocusHandle = 284;
constexpr uint32_t kServerSetSpectatorLocationHandle = 89;
constexpr uint32_t kChangeVivoxChannelsStateHandle = 152;
constexpr size_t kChangeVivoxChannelsStateRecordBits = 33;
// One local PRI plus every connection-local remote PRI is the largest useful
// batch.  Keep hostile concatenations bounded even though this RPC is a no-op.
constexpr size_t kMaximumVivoxChannelStateRecords = 129;
constexpr int kReadyStatusBits = 2;
constexpr uint8_t kMaxValidReadyStatus = 2;
// Installed VNSK-Compound's captured spawn-volume camera. This is corpus
// evidence, not a protocol invariant: map/team spawn cameras use other static
// PackageMap refs and the decoder accepts any bounded nonzero static ref.
constexpr uint32_t kInstalledCompoundSpawnVolumeCameraRef = 289102;

// Capture/source-bounded ROPlayerReplicationInfo / ROPawn field contracts used
// when a deployed participant is visible to a non-owning retail connection.
// The pawn archetypes and individual fields below are capture-backed.  The
// deliberately reduced initial tail has not yet been accepted by a live retail
// client and must remain an explicit rendering-compatibility uncertainty.
constexpr uint32_t kRoPlayerReplicationInfoMaxHandle = 98;
constexpr uint32_t kRoPawnMaxHandle = 168;
constexpr uint32_t kRemotePriClassRef = 86701;
// Server team 1 is US/South; server team 2 is NVA/North. The latter is the
// capture-dominant remote pawn archetype rather than an inferred class export.
constexpr uint32_t kRemoteSouthPawnArchetypeRef = 286151;
constexpr uint32_t kRemoteNorthPawnArchetypeRef = 286147;
// Class-family refs alone are not a safe visual-open contract.  The capture
// corpus has not yet pinned a complete non-owning pawn tail to each
// map/team/role combination (including Role/RemoteRole and attachment graph).
// Keep live pawn opens fail-closed until that registry exists; the bounded
// encoders below remain available for offline capture comparison and tests.
constexpr bool kRemotePawnVisualTemplatesGrounded = false;
constexpr uint8_t kRemotePawnWalkingPhysics = 1;
constexpr uint32_t kRemotePawnMovementIntervalMs = 100;
constexpr uint32_t kRemotePawnCloseDelayMs = 4700;
constexpr std::size_t kMaximumRetailPlayerNameBytes = 64;

struct RetailParticipantCombatState {
    ParticipantId participant;
    int health = 0;
    int kills = 0;
    int deaths = 0;
    int score = 0;
    bool dead = false;
};

struct RetailParticipantInitialState {
    RetailParticipantCombatState combat;
    uint8_t serverTeamId = 0;
    std::string playerName;
    Vector3 positionUu{};
    // Local server eligibility only; never serialized into the PRI tail.
    bool pawnPresent = false;
};

struct RetailRemotePawnSnapshot {
    ParticipantId participant;
    Vector3 positionUu{};
    Vector3 velocityUuPerSecond{};
    uint16_t pitch = 0;
    uint16_t yaw = 0;
    uint16_t roll = 0;
    int health = 0;
};

enum class DecodeError : uint8_t {
    None,
    InvalidBuffer,
    Truncated,
    UnsupportedHandle,
    InvalidReadyStatus,
    TrailingBits,
    UnsupportedSequence,
};

struct ServerSetSpawnSelect {
    // A false presence bit means the UnrealScript default byte value (zero).
    bool presentOnWire = false;
    uint8_t encodedSelection = 0;
    size_t consumedBits = 0;
};

struct ServerSetReadyToSpawn {
    // A false presence bit means the UnrealScript default enum value (Ready=0).
    bool presentOnWire = false;
    uint8_t status = 0;
    size_t consumedBits = 0;
};

struct SpawnSelectDecodeResult {
    DecodeError error = DecodeError::InvalidBuffer;
    ServerSetSpawnSelect rpc{};

    bool valid() const noexcept { return error == DecodeError::None; }
};

struct ReadyToSpawnDecodeResult {
    DecodeError error = DecodeError::InvalidBuffer;
    ServerSetReadyToSpawn rpc{};

    bool valid() const noexcept { return error == DecodeError::None; }
};

enum class ReadyToSpawnBunchPattern : uint8_t {
    Single,
    ReadyForceOnlyStopVoiceChat,
    ReadyForceOnlyAcknowledgePossession,
    NotReadySpectatorLocation,
};

struct ReadyToSpawnBunch {
    std::array<ServerSetReadyToSpawn, 2> transitions{};
    uint8_t transitionCount = 0;
    ReadyToSpawnBunchPattern pattern = ReadyToSpawnBunchPattern::Single;
    std::optional<uint32_t> acknowledgedPawnChannel;
    std::optional<Vector3> spectatorLocation;
};

struct ReadyToSpawnBunchDecodeResult {
    DecodeError error = DecodeError::InvalidBuffer;
    ReadyToSpawnBunch bunch{};

    bool valid() const noexcept { return error == DecodeError::None; }
};

enum class SpawnVolumeDeploymentActionType : uint8_t {
    SpawnSelect,
    ReadyToSpawn,
};

struct SpawnVolumeDeploymentAction {
    SpawnVolumeDeploymentActionType type =
        SpawnVolumeDeploymentActionType::SpawnSelect;
    ServerSetSpawnSelect spawnSelect{};
    ServerSetReadyToSpawn readyToSpawn{};
};

enum class SpawnVolumeViewTargetBunchPattern : uint8_t {
    ForceOnlyStopVoiceChat,
    SkirmishAutoSelect,
    SkirmishAutoSelectClearSpectatorLocation,
};

struct SpawnVolumeViewTargetBunch {
    uint32_t cameraTargetRef = 0;
    std::array<SpawnVolumeDeploymentAction, 4> actions{};
    uint8_t actionCount = 0;
    SpawnVolumeViewTargetBunchPattern pattern =
        SpawnVolumeViewTargetBunchPattern::ForceOnlyStopVoiceChat;
};

struct SpawnVolumeViewTargetBunchDecodeResult {
    DecodeError error = DecodeError::InvalidBuffer;
    SpawnVolumeViewTargetBunch bunch{};

    bool valid() const noexcept { return error == DecodeError::None; }
};

struct ChangeVivoxChannelsState {
    uint32_t otherPlayerPriChannel = 0;
    uint32_t localPlayerControllerChannel = 0;
};

struct ChangeVivoxChannelsStateBunch {
    std::array<ChangeVivoxChannelsState,
               kMaximumVivoxChannelStateRecords> records{};
    size_t recordCount = 0;
};

struct ChangeVivoxChannelsStateBunchDecodeResult {
    DecodeError error = DecodeError::InvalidBuffer;
    ChangeVivoxChannelsStateBunch bunch{};

    bool valid() const noexcept { return error == DecodeError::None; }
};

// Decode one RPC from the reader's current position.  Both overloads are
// transactional: reader and output remain unchanged on failure.  They consume
// only the decoded RPC, allowing a caller that has proven a compound schema to
// continue parsing.
bool DecodeOneServerSetSpawnSelect(BitReader& reader,
                                   ServerSetSpawnSelect& output,
                                   DecodeError& error);
bool DecodeOneServerSetReadyToSpawn(BitReader& reader,
                                    ServerSetReadyToSpawn& output,
                                    DecodeError& error);

// Decode a complete actor-bunch payload containing exactly one deployment RPC.
// These entry points reject trailing bits rather than guessing another schema.
SpawnSelectDecodeResult DecodeServerSetSpawnSelect(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits);
ReadyToSpawnDecodeResult DecodeServerSetReadyToSpawn(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits);

// Decode a complete leading-h434 actor bunch. Besides a standalone h434, this
// accepts only the exact companion-RPC sequences observed in retail packet
// captures. The entire bunch is validated before callers apply any transition.
ReadyToSpawnBunchDecodeResult DecodeServerSetReadyToSpawnBunch(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits);

// Decode the exact h370-led reliable Compound spawn-scene bunches observed from
// the retail client. The whole sequence, including camera object and
// otherwise-no-op companion RPCs, is validated before ordered h261/h434
// actions are exposed to the caller. Retail may append an exact
// h89 ServerSetSpectatorLocation(default/absent Vector) reset to auto-select.
SpawnVolumeViewTargetBunchDecodeResult DecodeSpawnVolumeViewTargetBunch(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits);

// Decode one or more exact 33-bit
// ChangeVivoxChannelsState(OtherPlayerROPRI, LocalPlayerROPC) records.  Both
// object parameters are required dynamic actor references.  Participant/channel
// ownership is connection-local and is therefore validated by the caller.
ChangeVivoxChannelsStateBunchDecodeResult
DecodeChangeVivoxChannelsStateBunch(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits);

// Transactional actor-property writers: invalid input returns false before any
// bits are appended.  Team is deliberately a separate reliable delta because
// the referenced TeamInfo actor must already be open on the viewer.
bool IsValidRetailParticipantCombatState(
    const RetailParticipantCombatState& state) noexcept;
bool IsValidRetailParticipantInitialState(
    const RetailParticipantInitialState& state) noexcept;
bool IsValidRetailRemotePawnSnapshot(
    const RetailRemotePawnSnapshot& snapshot) noexcept;
std::optional<uint32_t> RemotePawnArchetypeForServerTeam(
    uint8_t serverTeamId) noexcept;
bool WriteRemotePriInitial(BitWriter& writer,
                           const RetailParticipantInitialState& state);
bool WriteRemotePriTeam(BitWriter& writer, uint32_t teamInfoChannel);
bool WriteRemotePriCombat(BitWriter& writer,
                          const RetailParticipantCombatState& state,
                          bool includeDead = true);
bool WriteRemotePawnHealth(BitWriter& writer,
                           const RetailParticipantCombatState& state);
// Source-safe reduced initial tail only: h10 Walking, h32 viewer-local PRI,
// h33 Health.  No map/role-specific capture tail is replayed.
bool WriteRemotePawnInitial(BitWriter& writer,
                            const RetailRemotePawnSnapshot& snapshot,
                            uint32_t priChannel);
// Ordinary unreliable pawn movement properties.  Packet/channel sequencing is
// owned by the caller; these fields do not invent a timestamp or reliable ACK.
bool WriteRemotePawnMovement(BitWriter& writer,
                             const RetailRemotePawnSnapshot& snapshot);
bool WriteRemotePawnDeathCore(BitWriter& writer,
                              int negativeHealth = -1);

// Fully-framed actor bunch builders keep protocol flags and channel sequencing
// testable beside the capture/source-bounded property encoders.
std::optional<PacketCodec::Bunch> MakeRemotePawnOpeningBunch(
    uint32_t pawnChannel, uint32_t reliableSequence, uint8_t serverTeamId,
    uint32_t priChannel, const RetailRemotePawnSnapshot& snapshot);
std::optional<PacketCodec::Bunch> MakeRemotePawnMovementBunch(
    uint32_t pawnChannel, const RetailRemotePawnSnapshot& snapshot);
std::optional<PacketCodec::Bunch> MakeRemotePawnDeathBunch(
    uint32_t pawnChannel, int negativeHealth = -1);
std::optional<PacketCodec::Bunch> MakeRemotePawnCloseBunch(
    uint32_t pawnChannel, uint32_t reliableSequence);

} // namespace DeploymentRepl
