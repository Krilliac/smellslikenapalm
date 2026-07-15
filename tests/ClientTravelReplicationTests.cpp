#include "TestFramework.h"

#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/ClientTravelReplication.h"
#include "Network/PacketCodec.h"
#include "Network/RetailBootstrap.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct DecodedTravel {
    uint32_t handle = 0;
    bool urlPresent = false;
    std::string url;
    bool travelTypePresent = false;
    uint8_t travelType = 0;
    bool seamless = false;
    bool guidPresent = false;
    ClientTravelRepl::GuidBytes guid{};
    bool overflowed = false;
    size_t trailingBits = 0;
};

DecodedTravel Decode(const ClientTravelRepl::EncodedRpc& rpc) {
    BitReader reader(rpc.payload.data(), rpc.payload.size(), rpc.payloadBits);
    DecodedTravel decoded;
    decoded.handle = reader.SerializeInt(
        ClientTravelRepl::kRoPlayerControllerMaxHandle);
    decoded.urlPresent = reader.ReadBit();
    if (decoded.urlPresent) decoded.url = reader.ReadString();
    decoded.travelTypePresent = reader.ReadBit();
    if (decoded.travelTypePresent) {
        decoded.travelType = static_cast<uint8_t>(reader.ReadBits(2));
    }
    decoded.seamless = reader.ReadBit();
    decoded.guidPresent = reader.ReadBit();
    if (decoded.guidPresent) {
        for (uint8_t& value : decoded.guid) value = reader.ReadByte();
    }
    decoded.overflowed = reader.IsOverflowed();
    decoded.trailingBits = reader.BitsLeft();
    return decoded;
}

ClientTravelRepl::EncodedRpc EncodeOrEmpty(
    const ClientTravelRepl::Request& request) {
    ClientTravelRepl::EncodedRpc encoded;
    std::string error;
    if (!ClientTravelRepl::Encode(request, encoded, error)) return {};
    return encoded;
}

std::string SwapAsciiCase(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        } else if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return value;
}

std::string DisplayPackageGuid(const ClientTravelRepl::GuidBytes& guid) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string displayed;
    displayed.reserve(32);
    for (size_t word = 0; word < 4; ++word) {
        for (int byte = 3; byte >= 0; --byte) {
            const uint8_t value = guid[word * 4 + static_cast<size_t>(byte)];
            displayed.push_back(hex[value >> 4u]);
            displayed.push_back(hex[value & 0x0fu]);
        }
    }
    return displayed;
}

} // namespace

TEST(ClientTravelReplication, RelativeZeroGuidMatchesIndependentBitFixture) {
    ClientTravelRepl::Request request;
    request.url = "A";
    request.travelType = ClientTravelRepl::TravelType::Relative;

    ClientTravelRepl::EncodedRpc encoded;
    std::string error;
    ASSERT_TRUE(ClientTravelRepl::Encode(request, encoded, error)) << error;

    // Independently derived from the UE3 source algorithms, not by calling
    // BitWriter in the fixture:
    //   h29/max531 = 9 bits; URL presence + FString("A") = 49 bits;
    //   TravelType presence + raw enum 10b = 3; bool false = 1;
    //   zero Guid presence = 1. Total 63 bits, LSB-first.
    const std::vector<uint8_t> expected = {
        0x1d, 0x0a, 0x00, 0x00, 0x00, 0x04, 0x01, 0x14
    };
    EXPECT_EQ(encoded.payloadBits, 63u);
    EXPECT_EQ(encoded.payload, expected);
    EXPECT_TRUE(ClientTravelRepl::IsValid(encoded));

    const DecodedTravel decoded = Decode(encoded);
    EXPECT_EQ(decoded.handle, 29u);
    EXPECT_TRUE(decoded.urlPresent);
    EXPECT_EQ(decoded.url, std::string("A"));
    EXPECT_TRUE(decoded.travelTypePresent);
    EXPECT_EQ(decoded.travelType, 2u);
    EXPECT_FALSE(decoded.seamless);
    EXPECT_FALSE(decoded.guidPresent);
    EXPECT_FALSE(decoded.overflowed);
    EXPECT_EQ(decoded.trailingBits, 0u);
}

TEST(ClientTravelReplication, AbsoluteDefaultByteIsOmittedButBoolStillHasOneBit) {
    ClientTravelRepl::Request request;
    request.url = "A";
    request.travelType = ClientTravelRepl::TravelType::Absolute;
    request.seamless = true;

    const ClientTravelRepl::EncodedRpc encoded = EncodeOrEmpty(request);
    ASSERT_FALSE(encoded.payload.empty());
    EXPECT_EQ(encoded.payloadBits, 61u);

    const DecodedTravel decoded = Decode(encoded);
    EXPECT_FALSE(decoded.travelTypePresent);
    EXPECT_EQ(decoded.travelType, 0u);
    EXPECT_TRUE(decoded.seamless);
    EXPECT_FALSE(decoded.guidPresent);
    EXPECT_FALSE(decoded.overflowed);
    EXPECT_EQ(decoded.trailingBits, 0u);
}

TEST(ClientTravelReplication, PartialEnumUsesTwoRawBitsNotBoundedSerializeInt) {
    ClientTravelRepl::Request request;
    request.url = "A";
    request.travelType = ClientTravelRepl::TravelType::Partial;

    const ClientTravelRepl::EncodedRpc encoded = EncodeOrEmpty(request);
    ASSERT_FALSE(encoded.payload.empty());
    EXPECT_EQ(encoded.payloadBits, 63u);

    const DecodedTravel decoded = Decode(encoded);
    EXPECT_TRUE(decoded.travelTypePresent);
    EXPECT_EQ(decoded.travelType, 1u);
    EXPECT_FALSE(decoded.overflowed);
    EXPECT_EQ(decoded.trailingBits, 0u);
}

TEST(ClientTravelReplication, PresentGuidIsRawLittleEndianABCDInParameterOrder) {
    ClientTravelRepl::Request request;
    request.url = "VNTE-Hill937";
    request.travelType = ClientTravelRepl::TravelType::Relative;
    request.seamless = false;
    const auto grounded =
        RetailBootstrap::ResolveMapPackageGuid("vnte-hill937");
    ASSERT_TRUE(grounded.has_value());
    request.mapPackageGuid = *grounded;

    const ClientTravelRepl::EncodedRpc encoded = EncodeOrEmpty(request);
    ASSERT_FALSE(encoded.payload.empty());
    const DecodedTravel decoded = Decode(encoded);

    const ClientTravelRepl::GuidBytes expected = {
        0x4f, 0xec, 0x60, 0xaa, 0x08, 0x05, 0x22, 0x41,
        0x98, 0xbe, 0xcd, 0x44, 0x90, 0x9c, 0x96, 0x43
    };
    EXPECT_EQ(decoded.handle, 29u);
    EXPECT_EQ(decoded.url, std::string("VNTE-Hill937"));
    EXPECT_EQ(decoded.travelType, 2u);
    EXPECT_FALSE(decoded.seamless);
    EXPECT_TRUE(decoded.guidPresent);
    EXPECT_EQ(decoded.guid, expected);
    EXPECT_FALSE(decoded.overflowed);
    EXPECT_EQ(decoded.trailingBits, 0u);
}

TEST(ClientTravelReplication, CookedGuidResolverNeverSubstitutesFallbackMap) {
    const auto resort = RetailBootstrap::ResolveMapPackageGuid("VNTE-Resort");
    const auto cuchi = RetailBootstrap::ResolveMapPackageGuid("vnte-cuchi");
    const auto hue = RetailBootstrap::ResolveMapPackageGuid("VNSU-HUECITY");
    const auto compound = RetailBootstrap::ResolveMapPackageGuid("VNSK-Compound");
    const auto firebase = RetailBootstrap::ResolveMapPackageGuid("vnsk-firebase");
    const auto hill = RetailBootstrap::ResolveMapPackageGuid("VNTE-Hill937");
    const auto rungSac = RetailBootstrap::ResolveMapPackageGuid("vnte-rungsac");
    const auto saigon = RetailBootstrap::ResolveMapPackageGuid("VNTE-SAIGON");
    const auto songBe = RetailBootstrap::ResolveMapPackageGuid("VNTE-SongBe");
    ASSERT_TRUE(resort.has_value());
    ASSERT_TRUE(cuchi.has_value());
    ASSERT_TRUE(hue.has_value());
    ASSERT_TRUE(compound.has_value());
    ASSERT_TRUE(firebase.has_value());
    ASSERT_TRUE(hill.has_value());
    ASSERT_TRUE(rungSac.has_value());
    ASSERT_TRUE(saigon.has_value());
    ASSERT_TRUE(songBe.has_value());
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*resort));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*cuchi));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*hue));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*compound));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*firebase));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*hill));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*rungSac));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*saigon));
    EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*songBe));

    // ResolveProfile intentionally falls back to Resort for bootstrap safety;
    // ClientTravel package identity must not inherit that behavior.
    EXPECT_FALSE(
        RetailBootstrap::ResolveMapPackageGuid("VNTE-Unknown").has_value());
    EXPECT_FALSE(
        RetailBootstrap::ResolveMapPackageGuid("VNTE-Resort?game=x").has_value());
}

TEST(ClientTravelReplication, GroundedMapGuidsPreserveUe3WordByteOrder) {
    struct Case {
        const char* map;
        const char* displayedGuid;
    };
    constexpr std::array<Case, 36> cases = {{
        {"VNSK-Compound", "BCF3B0B34FFC0FBFF30A908128BD6DDA"},
        {"VNSK-Firebase", "BA1BDD124DC9ECCF3B48B79C2F86CECE"},
        {"VNSK-JungleCamp", "E1D4BDBD42FF4205B5442AAB4A1809D7"},
        {"VNSK-Riverbed", "6A0B38A340E899F6139E81B5E4938E9C"},
        {"VNSK-Temple", "6DC607F24C14644FC9B356A37A298DEC"},
        {"VNSU-AnLaoValley", "CC3D08E94F925F44F14EDEBD026269CE"},
        {"VNSU-HueCity", "D3B91A5847D6883CD78CBF9FDEFB354A"},
        {"VNSU-OperationForrest", "57514D104733FFEFF1CD988561A156C3"},
        {"VNSU-QuangTri", "F4783A4E4D57435A0C0C7FBF6EC2FDF2"},
        {"VNSU-SongBe", "524383D54078A53A63345EB8B026011D"},
        {"VNTE-ASau", "2352D59E474FE2366EF30D9F127DDF1F"},
        {"VNTE-AnLaoValley", "5EFE03B24AB53DA058A716A5A02DD87F"},
        {"VNTE-ApacheSnow", "5C28B5274E53D326288844AE891E3E3F"},
        {"VNTE-BorderWatch", "D14E4F2B47372F26B671E29192031AD6"},
        {"VNTE-CampaignStart", "5B9BD9844BA55D12190BFF9218B40803"},
        {"VNTE-Compound", "023AFB544CEA575FCEEB2CA23598D8C4"},
        {"VNTE-CuChi", "1BE145E5457B54A941963282D62E012B"},
        {"VNTE-CuaViet", "185C318F4778673FA2116895E2F63015"},
        {"VNTE-DaNangAirBase", "8CF9E1B04B2CB76FAF9046813F045E43"},
        {"VNTE-DemilitarizedZone", "ABC6DF734BB8997464908F8F7DC04E16"},
        {"VNTE-DongHa", "75C92C9C46B523E3202B8BAF3ED94353"},
        {"VNTE-Firebase", "40E07EBF4BD5295862A80C90163CD031"},
        {"VNTE-FirebaseGeorgina", "E54A57454DA6E0EDC73DA5A7781E7523"},
        {"VNTE-Highway14", "F3046C284CE6ACFDC8D43D8CDA24D72A"},
        {"VNTE-Hill937", "AA60EC4F4122050844CDBE9843969C90"},
        {"VNTE-HueCity", "D33D4DA640580F6411332483552401E3"},
        {"VNTE-KheSanh", "D942AF974D183929B301FAB0BE713B98"},
        {"VNTE-LongTan", "6AD7009B46CF1C62F89359B7CAE81086"},
        {"VNTE-Mekong", "ED8C67FE499BCBFCF0501A96EA27EAEB"},
        {"VNTE-NinhPhu", "86F86CEE442F9EED9A9DB0B1312B2A3A"},
        {"VNTE-OperationForrest", "792F7DFC4435EC3D4B568F8B577C062E"},
        {"VNTE-QuangTri", "E467282F48469D44D79A538F91A2AD70"},
        {"VNTE-Resort", "C75E786345B77AA5243259ABAF16C294"},
        {"VNTE-RungSac", "28867F2947A19DC5A5C5A0801B37FEE5"},
        {"VNTE-Saigon", "D57AA61242BB6D894AB5A58D58D45A1F"},
        {"VNTE-SongBe", "2203FB92469FD80E171F78AFCDE51D29"},
    }};

    for (const Case& testCase : cases) {
        const auto resolved =
            RetailBootstrap::ResolveMapPackageGuid(SwapAsciiCase(testCase.map));
        ASSERT_TRUE(resolved.has_value()) << testCase.map;
        EXPECT_FALSE(ClientTravelRepl::IsZeroGuid(*resolved)) << testCase.map;
        EXPECT_EQ(DisplayPackageGuid(*resolved), std::string(testCase.displayedGuid))
            << testCase.map;
    }
}

TEST(ClientTravelReplication, MalformedInputsFailWithoutMutatingOutput) {
    ClientTravelRepl::EncodedRpc output{{0xa5}, 8};
    const ClientTravelRepl::EncodedRpc sentinel = output;
    std::string error = "unchanged";

    ClientTravelRepl::Request request;
    EXPECT_FALSE(ClientTravelRepl::Encode(request, output, error));
    EXPECT_EQ(output.payload, sentinel.payload);
    EXPECT_EQ(output.payloadBits, sentinel.payloadBits);
    EXPECT_FALSE(error.empty());

    request.url = std::string(ClientTravelRepl::kMaxTravelUrlBytes + 1u, 'A');
    EXPECT_FALSE(ClientTravelRepl::Encode(request, output, error));
    EXPECT_EQ(output.payload, sentinel.payload);

    request.url = std::string("A\0B", 3);
    EXPECT_FALSE(ClientTravelRepl::Encode(request, output, error));
    EXPECT_EQ(output.payload, sentinel.payload);

    request.url.assign(1, static_cast<char>(0x80));
    EXPECT_FALSE(ClientTravelRepl::Encode(request, output, error));
    EXPECT_EQ(output.payload, sentinel.payload);

    request.url = "VNTE-Resort";
    request.travelType = static_cast<ClientTravelRepl::TravelType>(3);
    EXPECT_FALSE(ClientTravelRepl::Encode(request, output, error));
    EXPECT_EQ(output.payload, sentinel.payload);
}

TEST(ClientTravelReplication, MaximumAcceptedUrlStaysInsideBunchBound) {
    ClientTravelRepl::Request request;
    request.url.assign(ClientTravelRepl::kMaxTravelUrlBytes, 'A');

    const ClientTravelRepl::EncodedRpc encoded = EncodeOrEmpty(request);
    ASSERT_FALSE(encoded.payload.empty());
    EXPECT_TRUE(ClientTravelRepl::IsValid(encoded));
    EXPECT_LT(encoded.payloadBits,
              PacketCodec::kServerSendMaxPacketBytes * 8u);
}

TEST(ClientTravelReplication, PacketRepresentablePayloadBoundIsExclusive) {
    constexpr uint32_t limit = PacketCodec::kServerSendMaxPacketBytes * 8u;
    static_assert(limit == 12000u,
                  "retail server-send packet bound changed; re-audit framing");

    ClientTravelRepl::EncodedRpc largestRepresentable;
    largestRepresentable.payloadBits = limit - 1u;
    largestRepresentable.payload.assign(
        (largestRepresentable.payloadBits + 7u) / 8u, 0u);
    EXPECT_TRUE(ClientTravelRepl::IsValid(largestRepresentable));

    ClientTravelRepl::EncodedRpc exclusiveLimit;
    exclusiveLimit.payloadBits = limit;
    exclusiveLimit.payload.assign((limit + 7u) / 8u, 0u);
    EXPECT_FALSE(ClientTravelRepl::IsValid(exclusiveLimit));

    ClientTravelRepl::EncodedRpc aboveLimit = exclusiveLimit;
    ++aboveLimit.payloadBits;
    aboveLimit.payload.assign((aboveLimit.payloadBits + 7u) / 8u, 0u);
    EXPECT_FALSE(ClientTravelRepl::IsValid(aboveLimit));
}

TEST(ClientTravelReplication, EncodedSeamRejectsLengthAndPaddingCorruption) {
    ClientTravelRepl::Request request;
    request.url = "A";
    ClientTravelRepl::EncodedRpc encoded = EncodeOrEmpty(request);
    ASSERT_TRUE(ClientTravelRepl::IsValid(encoded));

    ClientTravelRepl::EncodedRpc wrongLength = encoded;
    wrongLength.payloadBits += 8u;
    EXPECT_FALSE(ClientTravelRepl::IsValid(wrongLength));

    ClientTravelRepl::EncodedRpc badPadding = encoded;
    badPadding.payload.back() |= 0x80u;
    EXPECT_FALSE(ClientTravelRepl::IsValid(badPadding));

    ClientTravelRepl::EncodedRpc empty;
    EXPECT_FALSE(ClientTravelRepl::IsValid(empty));
}

RS2V_TEST_MAIN()
