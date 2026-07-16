#pragma once

#include "Math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <vector>

enum class ParticipantKind : std::uint8_t {
    Invalid = 0,
    Human = 1,
    Bot = 2,
};

// Participant IDs are explicitly tagged instead of reserving part of the
// numeric client-ID space. Human(7) and Bot(7) are therefore distinct keys.
struct ParticipantId {
    ParticipantKind kind = ParticipantKind::Invalid;
    std::uint32_t value = 0;

    static ParticipantId Human(std::uint32_t clientId) {
        return ParticipantId{ParticipantKind::Human, clientId};
    }

    static ParticipantId Bot(std::uint32_t botId) {
        return ParticipantId{ParticipantKind::Bot, botId};
    }

    bool IsValid() const {
        return (kind == ParticipantKind::Human || kind == ParticipantKind::Bot) &&
               value != 0;
    }

    bool IsHuman() const { return kind == ParticipantKind::Human && value != 0; }
    bool IsBot() const { return kind == ParticipantKind::Bot && value != 0; }

    bool operator==(const ParticipantId& other) const {
        return kind == other.kind && value == other.value;
    }

    bool operator!=(const ParticipantId& other) const { return !(*this == other); }

    bool operator<(const ParticipantId& other) const {
        if (kind != other.kind) {
            return static_cast<std::uint8_t>(kind) <
                   static_cast<std::uint8_t>(other.kind);
        }
        return value < other.value;
    }
};

struct ParticipantSnapshot {
    ParticipantId id;
    std::uint8_t teamId = 0;
    bool alive = false;
    Vector3 position;
    float captureWeight = 0.0f;
};

// A small value-oriented roster shared by headless actors and future human
// adapters. It deliberately has no dependency on Player or ClientConnection.
class ParticipantRoster {
public:
    bool Upsert(const ParticipantSnapshot& participant);
    bool Remove(const ParticipantId& id);

    void Clear();
    void ClearKind(ParticipantKind kind);

    const ParticipantSnapshot* Find(const ParticipantId& id) const;

    std::vector<ParticipantSnapshot> GetAll() const;
    std::vector<ParticipantSnapshot> GetAlive() const;
    std::vector<ParticipantSnapshot> GetTeam(std::uint8_t teamId,
                                             bool aliveOnly = false) const;

    std::size_t Size() const { return participants_.size(); }
    std::size_t CountTeam(std::uint8_t teamId, bool aliveOnly = false) const;
    bool HasLivingParticipant(const ParticipantId& id) const;

    static bool IsPlayableTeam(std::uint8_t teamId);
    static bool IsFinite(const Vector3& position);

private:
    std::map<ParticipantId, ParticipantSnapshot> participants_;
};

// One retail connection has its own dynamic actor-channel namespace.  These
// bindings therefore live per viewer (inside ConnectionManager::ControlState),
// never globally.  The upper-half range is deliberately disjoint from the
// capture/live bootstrap and the fixed owning graph (ch2/ch26/ch209..219).
//
// Slots are tombstoned when a participant leaves and are not reassigned until
// Clear() starts a new connection namespace. A continuing participant may
// reuse its pawn channel for a later incarnation, but only after the reliable
// close ACK/drain contract reaches Closed. Exhaustion fails closed instead of
// aliasing participants or actor incarnations.
enum class ParticipantActorOpenState : std::uint8_t {
    Unopened = 0,
    Open = 1,
    Closing = 2,
    Closed = 3,
};

struct ParticipantActorChannelBinding {
    ParticipantId participant;
    std::uint16_t priChannel = 0;
    std::uint16_t pawnChannel = 0;
    std::int32_t wirePlayerId = 0;
    std::uint32_t pawnGeneration = 1;
    std::uint32_t priOutReliable = 0;
    std::uint32_t pawnOutReliable = 0;
    ParticipantActorOpenState priState = ParticipantActorOpenState::Unopened;
    ParticipantActorOpenState pawnState = ParticipantActorOpenState::Unopened;
    bool dead = false;
    // Last bDead value queued/sent on this viewer's remote PRI.  This is
    // deliberately distinct from the authoritative lifecycle bit above:
    // repeating unchanged h61 makes retail answer with h152 indefinitely.
    bool priDeadWireValid = false;
    bool priDeadWireValue = false;
    // Viewer-local class/team identity already published for the continuing
    // participant.  PRI identity survives a faction switch, so h35 is updated
    // in place; a pawn's archetype does not, so team drift closes that actor
    // incarnation before the channel can be reused.
    bool priTeamWireValid = false;
    std::uint32_t priTeamInfoChannel = 0;
    bool pawnServerTeamValid = false;
    std::uint32_t pawnServerTeamId = 0;
};

class ParticipantActorChannelMap {
public:
    static constexpr std::uint16_t kFirstChannel = 512;
    static constexpr std::size_t kSlotCount = 128;
    static constexpr std::uint16_t kLastChannel =
        static_cast<std::uint16_t>(kFirstChannel + kSlotCount * 2u - 1u);
    static constexpr std::uint32_t kReliableSequenceLimit = 1024;
    static constexpr std::uint32_t kMaximumTaggedValue = 0x3FFFFFFFu;

    ParticipantActorChannelBinding* Ensure(const ParticipantId& participant);
    const ParticipantActorChannelBinding* Find(
        const ParticipantId& participant) const;
    ParticipantActorChannelBinding* Find(const ParticipantId& participant);
    const ParticipantActorChannelBinding* FindByChannel(
        std::uint32_t channel) const;
    std::optional<ParticipantId> ResolveOpenLivingPawn(
        std::uint32_t channel) const;

    // Retire without making the pair available again on this connection.
    bool Retire(const ParticipantId& participant);
    // World travel invalidates every remote actor in the old PackageMap.  Keep
    // every used pair tombstoned until the connection namespace itself resets,
    // so delayed old-world reliable traffic can never alias a new participant.
    std::size_t RetireAll();
    void Clear();

    bool MarkPriOpen(const ParticipantId& participant);
    bool MarkPawnOpen(const ParticipantId& participant,
                      std::uint32_t generation);
    bool MarkPawnClosing(const ParticipantId& participant);
    bool AcknowledgePawnClose(std::uint32_t pawnChannel);
    bool MarkChannelClosed(std::uint32_t channel);
    bool SetDead(const ParticipantId& participant, bool dead);

    // A new pawn incarnation is only legal after the previous pawn close has
    // been acknowledged and all earlier reliable bunches have drained.  The
    // caller owns that pending-reliable check before AcknowledgePawnClose().
    // PRI identity survives respawns; only the pawn generation moves.
    std::optional<std::uint32_t> BeginPawnIncarnation(
        const ParticipantId& participant);
    std::optional<std::uint32_t> NextPriReliableSequence(
        const ParticipantId& participant);
    std::optional<std::uint32_t> NextPawnReliableSequence(
        const ParticipantId& participant);

    std::size_t Size() const { return bindings_.size(); }
    std::size_t RetiredCount() const;

    static std::optional<std::int32_t> EncodeWirePlayerId(
        const ParticipantId& participant);

private:
    enum class SlotState : std::uint8_t {
        NeverUsed = 0,
        Active = 1,
        Retired = 2,
    };

    static std::uint32_t AdvanceReliable(std::uint32_t current);
    std::optional<std::size_t> FindSlot(std::uint16_t channel) const;

    std::map<ParticipantId, ParticipantActorChannelBinding> bindings_;
    std::array<SlotState, kSlotCount> slots_{};
};
