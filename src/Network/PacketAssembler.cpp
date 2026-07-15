// src/Network/PacketAssembler.cpp
//
// Implementation of the outbound control-channel packet assembler. See
// PacketAssembler.h for the contract and spec citations
// (docs/RS2V_ControlChannel_WireSpec_7258.md §2/§3/§5).

#include "Network/PacketAssembler.h"

#include "Network/NetMessages.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace PacketCodec {

PacketAssembler::PacketAssembler() {
    const auto seeded = m_controlReliable.Seed(0u);
    assert(seeded.has_value());
    (void)seeded;
}

void PacketAssembler::QueueAck(uint32_t packetId) {
    m_pendingAcks.push_back(packetId);
}

void PacketAssembler::AllocatePacketIdentity(Packet& packet) {
    assert(HasPacketIdCapacity());
    packet.outboundPacketSerial = m_nextPacketSerial;
    packet.packetId = static_cast<uint32_t>(
        m_nextPacketSerial % static_cast<int64_t>(kMaxPacketId));
    ++m_nextPacketSerial;
}

PacketAssembler::PacketEncodeResult PacketAssembler::EncodePacketIfFits(
    const Packet& packet, uint32_t maxPacketBytes) const {
    if (maxPacketBytes == 0u ||
        maxPacketBytes > std::numeric_limits<uint32_t>::max() / 8u) {
        return std::unexpected(OutboundPacketBuildError::PacketTooLarge);
    }

    const uint32_t maxBunchDataBits = maxPacketBytes * 8u;
    for (const Bunch& bunch : packet.bunches) {
        if (bunch.payloadBits >= maxBunchDataBits ||
            static_cast<uint64_t>(bunch.payload.size()) * 8u <
                static_cast<uint64_t>(bunch.payloadBits)) {
            return std::unexpected(
                OutboundPacketBuildError::PacketTooLarge);
        }
    }

    std::vector<uint8_t> wireBytes = Encode(packet, maxPacketBytes);
    if (wireBytes.size() > maxPacketBytes) {
        return std::unexpected(OutboundPacketBuildError::PacketTooLarge);
    }
    return wireBytes;
}

PacketAssembler::PacketFitResult PacketAssembler::FitPendingAckPrefix(
    Packet packet, uint32_t maxPacketBytes) const {
    packet.acks.clear();
    auto wireBytes = EncodePacketIfFits(packet, maxPacketBytes);
    if (!wireBytes) {
        return std::unexpected(wireBytes.error());
    }

    size_t acceptedAcks = 0u;
    size_t lower = 0u;
    size_t upper = m_pendingAcks.size();
    while (lower < upper) {
        const size_t candidate = lower + (upper - lower + 1u) / 2u;
        packet.acks.assign(m_pendingAcks.begin(),
                           m_pendingAcks.begin() + candidate);
        auto candidateWire =
            EncodePacketIfFits(packet, maxPacketBytes);
        if (candidateWire) {
            lower = candidate;
            acceptedAcks = candidate;
            wireBytes = std::move(candidateWire);
        } else {
            upper = candidate - 1u;
        }
    }
    packet.acks.assign(m_pendingAcks.begin(),
                       m_pendingAcks.begin() + acceptedAcks);
    return FittedPacket{
        std::move(packet), std::move(*wireBytes), acceptedAcks};
}

Packet PacketAssembler::CommitFittedPacket(FittedPacket&& fitted) {
    assert(fitted.packet.packetId == NextPacketId());
    assert(fitted.packet.outboundPacketSerial == m_nextPacketSerial);
    assert(fitted.acceptedAckCount <= m_pendingAcks.size());
    assert(std::equal(fitted.packet.acks.begin(), fitted.packet.acks.end(),
                      m_pendingAcks.begin()));

    AllocatePacketIdentity(fitted.packet);
    m_pendingAcks.erase(
        m_pendingAcks.begin(),
        m_pendingAcks.begin() + fitted.acceptedAckCount);
    if (fitted.packet.bunches.empty()) {
        m_ackUnwrapReferenceSerial = std::max(
            m_ackUnwrapReferenceSerial,
            fitted.packet.outboundPacketSerial);
    }
    return std::move(fitted.packet);
}

Packet PacketAssembler::MakeControlMessagePacket(
    const std::vector<Bunch>& leadingBunches,
    const std::vector<uint8_t>& messagePayload,
    uint32_t controlSequence) const {
    Bunch bunch;
    bunch.bReliable = true;
    bunch.chIndex = kControlChannelIndex;
    bunch.chType = kControlChannelType;
    bunch.chSequence = controlSequence;
    bunch.payloadBits = static_cast<uint32_t>(messagePayload.size() * 8u);
    bunch.payload = messagePayload;

    Packet packet;
    packet.packetId = NextPacketId();
    packet.outboundPacketSerial = m_nextPacketSerial;
    packet.ok = true;
    packet.bunches = leadingBunches;
    packet.bunches.push_back(std::move(bunch));
    return packet;
}

PacketAssembler::ControlMessageMutationResult
PacketAssembler::ValidateControlMessagePacketWithLeadingBunches(
    const std::vector<Bunch>& leadingBunches,
    const std::vector<uint8_t>& messagePayload,
    uint32_t maxPacketBytes) const {
    if (maxPacketBytes == 0u ||
        maxPacketBytes > std::numeric_limits<uint32_t>::max() / 8u) {
        return std::unexpected(ControlMessageBuildError::PayloadTooLarge);
    }
    const uint32_t maxBunchDataBits = maxPacketBytes * 8u;
    if (messagePayload.size() >
        static_cast<size_t>(maxBunchDataBits - 1u) / 8u) {
        return std::unexpected(ControlMessageBuildError::PayloadTooLarge);
    }

    Packet packet = MakeControlMessagePacket(
        leadingBunches, messagePayload, 0u);
    const auto encoded = EncodePacketIfFits(packet, maxPacketBytes);
    if (!encoded) {
        return std::unexpected(ControlMessageBuildError::PayloadTooLarge);
    }
    return {};
}

PacketAssembler::ControlMessagePrepareResult
PacketAssembler::PrepareControlMessagePackets(
    const std::vector<uint8_t>& messagePayload,
    uint32_t maxPacketBytes) {
    return PrepareControlMessagePacketWithLeadingBunches(
        {}, messagePayload, maxPacketBytes);
}

PacketAssembler::ControlMessageMutationResult
PacketAssembler::ValidateControlMessagePacket(
    const std::vector<uint8_t>& messagePayload,
    uint32_t maxPacketBytes) const {
    return ValidateControlMessagePacketWithLeadingBunches(
        {}, messagePayload, maxPacketBytes);
}

PacketAssembler::ControlMessagePrepareResult
PacketAssembler::PrepareControlMessagePacketWithLeadingBunches(
    const std::vector<Bunch>& leadingBunches,
    const std::vector<uint8_t>& messagePayload,
    uint32_t maxPacketBytes) {
    // Full ACK-free framing validation deliberately precedes both capacity
    // checks. An impossible payload must fail permanently instead of entering a
    // backpressure queue merely because the reliable window is also full.
    const auto valid = ValidateControlMessagePacketWithLeadingBunches(
        leadingBunches, messagePayload, maxPacketBytes);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    if (!HasPacketIdCapacity()) {
        return std::unexpected(
            ControlMessageBuildError::PacketIdWindowFull);
    }

    auto reservation = m_controlReliable.ReserveBatch(1u);
    if (!reservation) {
        if (reservation.error() ==
                OutboundReliableSequenceError::OutstandingLimit ||
            reservation.error() ==
                OutboundReliableSequenceError::UnpublishedReservation) {
            return std::unexpected(
                ControlMessageBuildError::ReliableWindowFull);
        }
        return std::unexpected(
            ControlMessageBuildError::ReliableStateFailure);
    }

    try {
        // The control channel (ChIndex 0) is opened by the CLIENT. The SERVER only
        // sends on that already-open channel, so every server ch0 bunch is O0.
        Packet pkt = MakeControlMessagePacket(
            leadingBunches, messagePayload, reservation->front());

        auto fitted =
            FitPendingAckPrefix(std::move(pkt), maxPacketBytes);
        if (!fitted) {
            const auto cancelled =
                m_controlReliable.CancelBatch(*reservation);
            return std::unexpected(
                cancelled.has_value()
                    ? ControlMessageBuildError::PayloadTooLarge
                    : ControlMessageBuildError::ReliableStateFailure);
        }

        std::vector<Packet> packets;
        packets.reserve(1u);
        packets.push_back(std::move(fitted->packet));
        return PreparedControlMessage(
            std::move(*reservation), std::move(packets),
            std::move(fitted->wireBytes));
    } catch (...) {
        (void)m_controlReliable.CancelBatch(*reservation);
        throw;
    }
}

PacketAssembler::ControlMessageMutationResult
PacketAssembler::CommitPreparedControlMessage(
    PreparedControlMessage& prepared) {
    if (!prepared.m_active || prepared.m_packets.size() != 1u ||
        prepared.m_packets.front().packetId != NextPacketId() ||
        prepared.m_packets.front().outboundPacketSerial !=
            m_nextPacketSerial ||
        !HasPacketIdCapacity()) {
        return std::unexpected(
            ControlMessageBuildError::ReliableStateFailure);
    }

    const std::vector<uint32_t>& preparedAcks =
        prepared.m_packets.front().acks;
    if (preparedAcks.size() > m_pendingAcks.size() ||
        !std::equal(preparedAcks.begin(), preparedAcks.end(),
                    m_pendingAcks.begin())) {
        return std::unexpected(
            ControlMessageBuildError::ReliableStateFailure);
    }

    const auto committed =
        m_controlReliable.CommitBatch(prepared.m_reservation);
    if (!committed) {
        return std::unexpected(
            ControlMessageBuildError::ReliableStateFailure);
    }

    ++m_nextPacketSerial;
    m_pendingAcks.erase(
        m_pendingAcks.begin(),
        m_pendingAcks.begin() + preparedAcks.size());
    m_channelOpened = true;
    prepared.m_active = false;
    return {};
}

PacketAssembler::ControlMessageMutationResult
PacketAssembler::CancelPreparedControlMessage(
    PreparedControlMessage& prepared) {
    if (!prepared.m_active) {
        return std::unexpected(
            ControlMessageBuildError::ReliableStateFailure);
    }
    const auto cancelled =
        m_controlReliable.CancelBatch(prepared.m_reservation);
    if (!cancelled) {
        return std::unexpected(
            ControlMessageBuildError::ReliableStateFailure);
    }
    prepared.m_active = false;
    return {};
}

PacketAssembler::ControlMessageBuildResult
PacketAssembler::BuildControlMessagePackets(
    const std::vector<uint8_t>& messagePayload,
    uint32_t maxPacketBytes) {
    auto prepared =
        PrepareControlMessagePackets(messagePayload, maxPacketBytes);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }

    const auto committed = CommitPreparedControlMessage(*prepared);
    if (!committed) {
        if (prepared->IsActive()) {
            (void)CancelPreparedControlMessage(*prepared);
        }
        return std::unexpected(committed.error());
    }
    return std::move(prepared->m_packets);
}

OutboundReliableSequencer::MutationResult
PacketAssembler::AcknowledgeControlSequence(uint32_t sequence) {
    return m_controlReliable.Release(sequence);
}

PacketAssembler::OutboundAckResult
PacketAssembler::ResolveOutboundAck(uint32_t wirePacketId) {
    constexpr int64_t modulus = static_cast<int64_t>(kMaxPacketId);
    constexpr int64_t halfRange = modulus / 2;
    if (wirePacketId >= static_cast<uint32_t>(kMaxPacketId)) {
        return std::unexpected(OutboundAckError::InvalidWireValue);
    }

    const int64_t referenceWire =
        ((m_ackUnwrapReferenceSerial % modulus) + modulus) % modulus;
    const int64_t forwardDistance =
        (static_cast<int64_t>(wirePacketId) - referenceWire + modulus) %
        modulus;
    if (forwardDistance == halfRange) {
        return std::unexpected(OutboundAckError::AmbiguousHalfRange);
    }
    const int64_t relativeDelta =
        forwardDistance > halfRange
            ? forwardDistance - modulus
            : forwardDistance;
    const int64_t candidate =
        m_ackUnwrapReferenceSerial + relativeDelta;
    if (candidate < 0) {
        return std::unexpected(OutboundAckError::BeforeSession);
    }
    if (candidate >= m_nextPacketSerial) {
        return std::unexpected(OutboundAckError::FuturePacket);
    }
    if (candidate > m_ackUnwrapReferenceSerial) {
        m_ackUnwrapReferenceSerial = candidate;
    }
    if (candidate > m_highestPeerAckPacketSerial) {
        m_highestPeerAckPacketSerial = candidate;
    }
    return candidate;
}

PacketAssembler::PacketBuildResult
PacketAssembler::BuildRawBunchPacket(
    const Bunch& bunch, uint32_t maxPacketBytes) {
    Packet packet;
    packet.packetId = NextPacketId();
    packet.outboundPacketSerial = m_nextPacketSerial;
    packet.ok = true;
    packet.bunches.push_back(bunch);
    auto fitted =
        FitPendingAckPrefix(std::move(packet), maxPacketBytes);
    if (!fitted) {
        return std::unexpected(fitted.error());
    }
    if (!HasPacketIdCapacity()) {
        return std::unexpected(OutboundPacketBuildError::PacketIdWindowFull);
    }
    return CommitFittedPacket(std::move(*fitted));
}

PacketAssembler::PacketBuildResult
PacketAssembler::BuildRawBunchesPacket(
    const std::vector<Bunch>& bunches, uint32_t maxPacketBytes) {
    Packet packet;
    packet.packetId = NextPacketId();
    packet.outboundPacketSerial = m_nextPacketSerial;
    packet.ok = true;
    packet.bunches = bunches;
    auto fitted =
        FitPendingAckPrefix(std::move(packet), maxPacketBytes);
    if (!fitted) {
        return std::unexpected(fitted.error());
    }
    if (!HasPacketIdCapacity()) {
        return std::unexpected(OutboundPacketBuildError::PacketIdWindowFull);
    }
    return CommitFittedPacket(std::move(*fitted));
}

PacketAssembler::PacketBuildResult PacketAssembler::BuildAckOnlyPacket(
    uint32_t maxPacketBytes) {
    Packet packet;
    packet.packetId = NextPacketId();
    packet.outboundPacketSerial = m_nextPacketSerial;
    packet.ok = true;
    auto fitted =
        FitPendingAckPrefix(std::move(packet), maxPacketBytes);
    if (!fitted) {
        return std::unexpected(fitted.error());
    }
    if (!HasPacketIdCapacity()) {
        return std::unexpected(OutboundPacketBuildError::PacketIdWindowFull);
    }
    return CommitFittedPacket(std::move(*fitted));
}

} // namespace PacketCodec
