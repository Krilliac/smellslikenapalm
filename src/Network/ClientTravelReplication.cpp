#include "Network/ClientTravelReplication.h"

#include "Network/BitWriter.h"
#include "Network/PacketCodec.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ClientTravelRepl {

namespace {

// PacketCodec serializes BunchDataBits with SerializeInt(MaxPacket * 8), whose
// upper bound is exclusive. A 12,000-bit payload cannot be represented when
// the established server-send MaxPacket is 1,500 bytes.
constexpr uint32_t kPacketPayloadBitsExclusive =
    PacketCodec::kServerSendMaxPacketBytes * 8u;

} // namespace

bool IsZeroGuid(const GuidBytes& guid) {
    return std::all_of(guid.begin(), guid.end(),
                       [](uint8_t value) { return value == 0; });
}

bool IsValid(const EncodedRpc& rpc) {
    if (rpc.payloadBits == 0 ||
        rpc.payloadBits >= kPacketPayloadBitsExclusive ||
        rpc.payload.size() != (static_cast<size_t>(rpc.payloadBits) + 7u) / 8u) {
        return false;
    }

    const uint32_t tailBits = rpc.payloadBits & 7u;
    if (tailBits != 0) {
        const uint8_t usedMask = static_cast<uint8_t>((1u << tailBits) - 1u);
        if ((rpc.payload.back() & static_cast<uint8_t>(~usedMask)) != 0) {
            return false;
        }
    }
    return true;
}

bool Encode(const Request& request, EncodedRpc& output, std::string& error) {
    if (request.url.empty()) {
        error = "ClientTravel URL is empty";
        return false;
    }
    if (request.url.size() > kMaxTravelUrlBytes) {
        error = "ClientTravel URL exceeds the bounded ANSI limit";
        return false;
    }
    for (unsigned char value : request.url) {
        // BitWriter::WriteString emits ANSI. Restrict this API to the encoding
        // for which it is wire-identical to UE3 FString serialization rather
        // than silently treating UTF-8 bytes as ANSI code units.
        if (value < 0x20u || value > 0x7eu) {
            error = "ClientTravel URL must contain printable 7-bit ANSI bytes";
            return false;
        }
    }

    const uint8_t rawTravelType = static_cast<uint8_t>(request.travelType);
    if (rawTravelType >= 3u) {
        error = "ClientTravel TravelType is the invalid ETravelType sentinel";
        return false;
    }

    BitWriter writer;
    writer.SerializeInt(kClientTravelHandle, kRoPlayerControllerMaxHandle);

    // UnScript.cpp writes a presence bit for every non-bool RPC parameter.
    // URL is required non-empty here, so it is always present.
    writer.WriteBit(true);
    writer.WriteString(request.url);

    // UByteProperty::NetSerializeItem serializes an enum using
    // ceil(log2(NumEnums-1)) raw bits. ETravelType has three values plus MAX,
    // hence exactly two bits when the non-default byte is present. This is NOT
    // FArchive::SerializeInt(value, 3), whose width is value-dependent.
    if (rawTravelType == static_cast<uint8_t>(TravelType::Absolute)) {
        writer.WriteBit(false);
    } else {
        writer.WriteBit(true);
        writer.WriteBits(rawTravelType, 2);
    }

    // UBoolProperty is the exception: one bare value bit, no presence bit.
    writer.WriteBit(request.seamless);

    // Guid is a non-bool struct parameter. Its zero default is omitted; a
    // present Guid is A/B/C/D, each raw int32 LE, exactly these 16 bytes.
    const bool hasGuid = !IsZeroGuid(request.mapPackageGuid);
    writer.WriteBit(hasGuid);
    if (hasGuid) {
        writer.WriteRawBytes(request.mapPackageGuid.data(),
                             request.mapPackageGuid.size());
    }

    if (writer.HadInvariantViolation() ||
        writer.NumBits() == 0 ||
        writer.NumBits() >= kPacketPayloadBitsExclusive ||
        writer.NumBits() > std::numeric_limits<uint32_t>::max()) {
        error = "ClientTravel payload violates the bounded bunch invariant";
        return false;
    }

    EncodedRpc encoded;
    encoded.payload = writer.GetBytes();
    encoded.payloadBits = static_cast<uint32_t>(writer.NumBits());
    if (!IsValid(encoded)) {
        error = "ClientTravel encoder produced an invalid bit buffer";
        return false;
    }

    output = std::move(encoded);
    error.clear();
    return true;
}

} // namespace ClientTravelRepl
