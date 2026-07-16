// src/Network/PacketAssembler.h
//
// Per-connection OUTBOUND packet assembler for the UE3 (RS2 EngineVersion 7258)
// control channel. This is the SEND-side counterpart to PacketCodec::Decode: it
// takes raw control-channel MESSAGE PAYLOADS (a <BYTE NMT><fields> byte buffer,
// exactly what ControlChannel::Build* produces) and turns each into one complete
// PacketCodec::Packet plus its bounded encoded wire image.
//
// It owns the per-connection outbound SEQUENCING state:
//   * nextPacketSerial    — monotonic internal PacketId stamped on each packet;
//                           only its low 14 bits wrap on wire (spec §2).
//   * ackUnwrapReferenceSerial - newest peer-ACKed or locally retired identity.
//   * highestPeerAckPacketSerial - highest identity actually ACKed by the peer.
//   * nextControlChSequence — per-channel modulo-1024 ChSequence stamped on each
//                           reliable control bunch (starts at 1; spec §3/§5).
//   * channelOpened       — whether this server send path has used the client's
//                           already-open control channel.
//   * pendingAcks         — inbound PacketIds we have received and must ack.
//
// Message boundary (spec §3): UControlChannel processes the messages present in
// each received bunch; it does not concatenate arbitrary bit fragments across
// bunches. One logical payload therefore becomes exactly one reliable ch0 bunch.
// Oversized payloads are rejected so callers can split at a real NMT/record
// boundary rather than manufacture an invalid partial message.
//
// The control sequencer also enforces UE3's 127-record ordinary issuance window,
// including ACKed tombstones retained behind an older gap.
//
// Wire format authority: docs/RS2V_ControlChannel_WireSpec_7258.md.

#pragma once

#include <cstdint>
#include <expected>
#include <vector>

#include "Network/OutboundReliableSequencer.h"
#include "Network/PacketCodec.h"

namespace PacketCodec {

enum class ControlMessageBuildError : uint8_t {
    PayloadTooLarge,
    ReliableWindowFull,
    PacketIdWindowFull,
    ReliableStateFailure,
};

enum class OutboundPacketBuildError : uint8_t {
    PacketIdWindowFull,
    PacketTooLarge,
};

enum class OutboundAckError : uint8_t {
    InvalidWireValue,
    BeforeSession,
    FuturePacket,
    AmbiguousHalfRange,
};

class PacketAssembler {
public:
    class PreparedControlMessage {
    public:
        PreparedControlMessage(const PreparedControlMessage&) = delete;
        PreparedControlMessage& operator=(const PreparedControlMessage&) = delete;
        PreparedControlMessage(PreparedControlMessage&&) noexcept = default;
        PreparedControlMessage& operator=(PreparedControlMessage&&) noexcept = default;

        [[nodiscard]] const std::vector<Packet>& Packets() const noexcept {
            return m_packets;
        }
        [[nodiscard]] const std::vector<uint8_t>& WireBytes() const noexcept {
            return m_wireBytes;
        }
        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

    private:
        friend class PacketAssembler;
        PreparedControlMessage(
            OutboundReliableSequencer::Reservation&& reservation,
            std::vector<Packet>&& packets,
            std::vector<uint8_t>&& wireBytes) noexcept
            : m_reservation(std::move(reservation)),
              m_packets(std::move(packets)),
              m_wireBytes(std::move(wireBytes)) {}

        OutboundReliableSequencer::Reservation m_reservation;
        std::vector<Packet> m_packets;
        std::vector<uint8_t> m_wireBytes;
        bool m_active = true;
    };

    using ControlMessageBuildResult =
        std::expected<std::vector<Packet>, ControlMessageBuildError>;
    using ControlMessagePrepareResult =
        std::expected<PreparedControlMessage, ControlMessageBuildError>;
    using ControlMessageMutationResult =
        std::expected<void, ControlMessageBuildError>;
    using PacketBuildResult =
        std::expected<Packet, OutboundPacketBuildError>;
    using OutboundAckResult =
        std::expected<int64_t, OutboundAckError>;

    PacketAssembler();

    // Record an inbound PacketId that must be acknowledged on a future outbound
    // packet. Drained into the produced Packet(s) by the next Build* call (or by
    // BuildAckOnlyPacket).
    void QueueAck(uint32_t packetId);

    // Prepare one control-channel message payload (raw <BYTE NMT><fields> bytes)
    // as exactly one encoded packet carrying exactly one reliable ch0 bunch. The
    // client owns/opens ch0, so server bunches always have bControl=false,
    // bOpen=false. The largest prefix of queued ACKs that still fits is included.
    //
    // An empty payload still produces exactly one packet carrying a single
    // zero-bit bunch (so an ack can still go out). An oversized complete packet or
    // a full 127-record reliable window returns an error without consuming
    // PacketId, ChSequence, channel state, or queued ACKs.
    //
    // Preparation reserves but does not publish ChSequence or drain ACK/PacketId
    // state. The caller must first establish durable retransmission ownership and
    // then CommitPreparedControlMessage. CancelPreparedControlMessage restores the
    // reservation after any staging failure. This is the production send API.
    ControlMessagePrepareResult PrepareControlMessagePackets(
        const std::vector<uint8_t>& messagePayload,
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes);

    // Prepare one atomic packet whose caller-owned bunches appear first in
    // exact order, followed by one newly reserved reliable ch0 message. This
    // supports capture-faithful bootstrap entry packets such as a ch2 OPEN
    // immediately followed by NMT 0x24 on ch0, under one PacketId.
    //
    // The leading bunches are copied without changing any of their fields.
    // Full framing and the largest fitting FIFO ACK prefix are validated before
    // any PacketId, ACK, or reliable-sequence state is published. Commit/cancel
    // use the same transaction contract as PrepareControlMessagePackets.
    ControlMessagePrepareResult PrepareControlMessagePacketWithLeadingBunches(
        const std::vector<Bunch>& leadingBunches,
        const std::vector<uint8_t>& messagePayload,
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes);

    // Validate the ACK-free, fully framed control packet without reserving a
    // reliable sequence or consuming PacketId/ACK state. Connection-level
    // backpressure uses this before retaining a payload for a later retry.
    [[nodiscard]] ControlMessageMutationResult ValidateControlMessagePacket(
        const std::vector<uint8_t>& messagePayload,
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes) const;

    [[nodiscard]] ControlMessageMutationResult
    CommitPreparedControlMessage(PreparedControlMessage& prepared);
    [[nodiscard]] ControlMessageMutationResult
    CancelPreparedControlMessage(PreparedControlMessage& prepared);

    // Convenience for structure-level callers: prepare and immediately publish
    // the message. Production delivery code that needs a retransmission ledger
    // must use the explicit prepare/commit transaction above.
    ControlMessageBuildResult BuildControlMessagePackets(
        const std::vector<uint8_t>& messagePayload,
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes);

    // Release one sent ch0 sequence when any packet attempt carrying it is ACKed.
    // Out-of-order ACKs remain issuance-window tombstones until the oldest gap
    // clears, matching UE3 UChannel::ReceivedAcks/IsNetReady behavior.
    [[nodiscard]] OutboundReliableSequencer::MutationResult
    AcknowledgeControlSequence(uint32_t sequence);

    // Expand one 14-bit wire ACK into UE3's monotonic internal PacketId space
    // relative to the latest peer-ACKed or locally retired bunch-less packet.
    // Future, pre-session, and exact half-range ambiguous values are rejected
    // without advancing either the unwrap reference or peer ACK high-water.
    [[nodiscard]] OutboundAckResult ResolveOutboundAck(uint32_t wirePacketId);

    // Flush any pending acks in a bunch-less packet (PacketId + acks only). Useful
    // when we must acknowledge received packets without sending data. Always
    // produces a packet (with an incremented PacketId) even if no acks are queued.
    // Bunch-less identities retire locally because no reliable ledger waits on
    // their peer ACK; this prevents keepalives from exhausting the half-window.
    PacketBuildResult BuildAckOnlyPacket(
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes);

    // Wrap ONE fully-specified bunch in its own packet (next PacketId, largest
    // fitting queued-ACK prefix). Unlike BuildControlMessagePackets this does
    // not touch the control-channel sequence; the caller owns every bunch field.
    // Full framing is validated before PacketId/ACK state is consumed.
    PacketBuildResult BuildRawBunchPacket(
        const Bunch& bunch,
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes);

    // Wrap MULTIPLE fully-specified bunches in ONE packet (next PacketId, queued
    // ACK prefix drained onto it). The complete framing is validated against
    // MaxPacket before any allocator state changes. Used to batch bootstrap
    // actor-channel opens without overflowing the client's receive buffer.
    PacketBuildResult BuildRawBunchesPacket(
        const std::vector<Bunch>& bunches,
        uint32_t maxPacketBytes = kServerSendMaxPacketBytes);

    // ---- state accessors (for tests / integration) ----
    uint32_t NextPacketId() const {
        return static_cast<uint32_t>(
            m_nextPacketSerial % static_cast<int64_t>(kMaxPacketId));
    }
    int64_t NextPacketSerial() const { return m_nextPacketSerial; }
    int64_t HighestResolvedAckSerial() const {
        return m_highestPeerAckPacketSerial;
    }
    int64_t HighestPeerAckSerial() const {
        return m_highestPeerAckPacketSerial;
    }
    int64_t AckUnwrapReferenceSerial() const {
        return m_ackUnwrapReferenceSerial;
    }
    bool HasPacketIdCapacity() const {
        return m_nextPacketSerial - m_ackUnwrapReferenceSerial <
               static_cast<int64_t>(kMaxPacketId) / 2;
    }
    uint32_t NextControlChSequence() const {
        return m_controlReliable.NextSequence().value_or(kMaxChSequence);
    }
    size_t OutstandingControlBunchCount() const {
        return m_controlReliable.OutstandingCount();
    }
    size_t ControlIssuanceWindowSize() const {
        return m_controlReliable.IssuanceWindowSize();
    }
    size_t AvailableControlCapacity() const {
        return m_controlReliable.AvailableCapacity();
    }
    bool IsChannelOpened() const { return m_channelOpened; }
    size_t PendingAckCount() const { return m_pendingAcks.size(); }

private:
    struct FittedPacket {
        Packet packet;
        std::vector<uint8_t> wireBytes;
        size_t acceptedAckCount = 0u;
    };

    using PacketFitResult =
        std::expected<FittedPacket, OutboundPacketBuildError>;
    using PacketEncodeResult =
        std::expected<std::vector<uint8_t>, OutboundPacketBuildError>;

    // Allocate the next monotonic PacketId and stamp its wrapped wire projection.
    void AllocatePacketIdentity(Packet& packet);

    // Encode and validate one complete packet. This never mutates allocator or
    // pending-ACK state.
    [[nodiscard]] PacketEncodeResult EncodePacketIfFits(
        const Packet& packet, uint32_t maxPacketBytes) const;

    // Select the largest FIFO prefix of pending ACKs that keeps the complete
    // packet within MaxPacket. The unselected suffix remains queued on commit.
    [[nodiscard]] PacketFitResult FitPendingAckPrefix(
        Packet packet, uint32_t maxPacketBytes) const;

    // Construct the ACK-free packet shape shared by preflight and preparation.
    // The caller must first enforce the control-payload size bound.
    [[nodiscard]] Packet MakeControlMessagePacket(
        const std::vector<Bunch>& leadingBunches,
        const std::vector<uint8_t>& messagePayload,
        uint32_t controlSequence) const;

    [[nodiscard]] ControlMessageMutationResult
    ValidateControlMessagePacketWithLeadingBunches(
        const std::vector<Bunch>& leadingBunches,
        const std::vector<uint8_t>& messagePayload,
        uint32_t maxPacketBytes) const;

    // Publish a fitted non-control packet. Bunch-less identities are locally
    // retired because no reliable-delivery ledger can depend on them.
    Packet CommitFittedPacket(FittedPacket&& fitted);

    int64_t m_nextPacketSerial = 0;       // monotonic; wire projection wraps at 16384
    int64_t m_ackUnwrapReferenceSerial = -1; // peer-ACK/local-retirement cursor
    int64_t m_highestPeerAckPacketSerial = -1; // observed peer ACK high-water
    OutboundReliableSequencer m_controlReliable; // seeded at 0 -> first seq 1
    bool m_channelOpened = false;         // server has sent on client-opened ch0
    std::vector<uint32_t> m_pendingAcks;  // inbound PacketIds awaiting ack
};

} // namespace PacketCodec
