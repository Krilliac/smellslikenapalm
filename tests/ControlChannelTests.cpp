// tests/ControlChannelTests.cpp
//
// Round-trip unit tests for the UE3 / RS2 control-channel MESSAGE-BODY codec
// (src/Network/ControlChannel.{h,cpp}). These tests are the executable contract
// for spec §4 of RS2V_ControlChannel_WireSpec_7258.md: the authoritative
// on-wire body field order/types per NMT.
//
// For each message we assert:
//   * the leading message-type byte equals the NMT value, and
//   * Build* -> Parse* round-trips every field exactly (Build a struct,
//     Parse the produced bytes, fields compare equal).
// We also exercise the `expectType` parameter and overflow/short-buffer safety.

#include "TestFramework.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Network/ControlChannel.h"
#include "Network/NetMessages.h"

using namespace ControlChannel;

namespace {

// The leading on-wire byte of a built payload must be the NMT type byte. Because
// BitWriter packs LSB-first and a byte-aligned write reproduces the source byte
// verbatim, payload[0] is exactly the NMT byte.
void ExpectLeadingNMT(const std::vector<uint8_t>& payload, NMT expected) {
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(payload[0], NMTByte(expected));
}

} // namespace

// ---------------------------------------------------------------------------
// Hello (0x00, C->S): int32 MinVer, int32 Ver, QWORD SteamId, FString, FString
// (spec §4 note: matches the existing HelloMessage layout, incl. leading
//  bIsLittleEndian byte). Verify it still round-trips.
// ---------------------------------------------------------------------------
TEST(ControlChannelHello, RoundTrip) {
    HelloMessage in;
    in.bIsLittleEndian = 1;
    in.minVersion = kMinNetVersion;
    in.version = kEngineVersion;
    in.steamId = 0x1122334455667788ull;
    in.leechSessionId = "leech-session-abc";
    in.token = "token-xyz";

    std::vector<uint8_t> bytes = BuildHello(in);
    ExpectLeadingNMT(bytes, NMT::Hello);

    HelloMessage out;
    ASSERT_TRUE(ParseHello(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.bIsLittleEndian, in.bIsLittleEndian);
    EXPECT_EQ(out.minVersion, in.minVersion);
    EXPECT_EQ(out.version, in.version);
    EXPECT_EQ(out.steamId, in.steamId);
    EXPECT_EQ(out.leechSessionId, in.leechSessionId);
    EXPECT_EQ(out.token, in.token);
}

// ---------------------------------------------------------------------------
// Welcome (0x01, S->C): FString, FString, QWORD   (NOT three FStrings)
// ---------------------------------------------------------------------------
TEST(ControlChannelWelcome, RoundTrip) {
    WelcomeMessage in;
    in.levelName = "VNTE-CuChi";
    in.gameName = "ROGame.ROGameInfoTerritories";
    in.flags = 0xCAFEF00DDEADBEEFull;

    std::vector<uint8_t> bytes = BuildWelcome(in);
    ExpectLeadingNMT(bytes, NMT::Welcome);

    WelcomeMessage out;
    ASSERT_TRUE(ParseWelcome(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.levelName, in.levelName);
    EXPECT_EQ(out.gameName, in.gameName);
    EXPECT_EQ(out.flags, in.flags);
}

// ---------------------------------------------------------------------------
// Upgrade (0x02, S->C): int32, int32
// ---------------------------------------------------------------------------
TEST(ControlChannelUpgrade, RoundTrip) {
    UpgradeMessage in;
    in.remoteMinVer = kMinNetVersion;
    in.remoteVer = kEngineVersion;

    std::vector<uint8_t> bytes = BuildUpgrade(in);
    ExpectLeadingNMT(bytes, NMT::Upgrade);

    UpgradeMessage out;
    ASSERT_TRUE(ParseUpgrade(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.remoteMinVer, in.remoteMinVer);
    EXPECT_EQ(out.remoteVer, in.remoteVer);
}

// ---------------------------------------------------------------------------
// Challenge (0x03, S->C): a single 32-bit nonce/cookie. (The RS2 on-wire
// NMT_Challenge body is one DWORD - a 40-bit bunch NMT + DWORD - NOT the
// {int32, FString} layout earlier assumed, which bloated it across 4 bunches
// and the client ignored.)
// ---------------------------------------------------------------------------
TEST(ControlChannelChallenge, RoundTrip) {
    ChallengeMessage in;
    in.nonce = 0xDEADBEEFu;

    std::vector<uint8_t> bytes = BuildChallenge(in);
    ExpectLeadingNMT(bytes, NMT::Challenge);
    // NMT byte + a single DWORD = exactly 5 bytes on the wire.
    EXPECT_EQ(bytes.size(), 5u);

    ChallengeMessage out;
    ASSERT_TRUE(ParseChallenge(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.nonce, in.nonce);
}

// ---------------------------------------------------------------------------
// Netspeed (0x04, C->S): single int32
// ---------------------------------------------------------------------------
TEST(ControlChannelNetspeed, RoundTrip) {
    NetspeedMessage in;
    in.netspeed = kNetspeedInternet;

    std::vector<uint8_t> bytes = BuildNetspeed(in);
    ExpectLeadingNMT(bytes, NMT::Netspeed);

    NetspeedMessage out;
    ASSERT_TRUE(ParseNetspeed(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.netspeed, in.netspeed);
}

// ---------------------------------------------------------------------------
// Login (0x05, C->S): FString, FString, QWORD
// ---------------------------------------------------------------------------
TEST(ControlChannelLogin, RoundTrip) {
    LoginMessage in;
    in.response = "response-hash";
    in.url = "VNTE-CuChi?Name=Charlie?Team=1";
    in.steamId = 0x0102030405060708ull;

    std::vector<uint8_t> bytes = BuildLogin(in);
    ExpectLeadingNMT(bytes, NMT::Login);

    LoginMessage out;
    ASSERT_TRUE(ParseLogin(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.response, in.response);
    EXPECT_EQ(out.url, in.url);
    EXPECT_EQ(out.steamId, in.steamId);
}

// ---------------------------------------------------------------------------
// Failure (0x06, S->C): single FString
// ---------------------------------------------------------------------------
TEST(ControlChannelFailure, RoundTrip) {
    FailureMessage in;
    in.errorKey = NetFailureKeys::SteamClientRequired;

    std::vector<uint8_t> bytes = BuildFailure(in);
    ExpectLeadingNMT(bytes, NMT::Failure);

    FailureMessage out;
    ASSERT_TRUE(ParseFailure(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.errorKey, in.errorKey);
}

// ---------------------------------------------------------------------------
// Join (empty body) round-trips and carries only its type byte.
// ---------------------------------------------------------------------------
TEST(ControlChannelJoin, RoundTrip) {
    JoinMessage in;
    std::vector<uint8_t> bytes = BuildJoin(in);
    ExpectLeadingNMT(bytes, NMT::Join);

    JoinMessage out;
    EXPECT_TRUE(ParseJoin(bytes.data(), bytes.size(), out));
}

// ---------------------------------------------------------------------------
// expectType behavior: parsing a payload whose leading byte is the WRONG NMT
// must fail when expectType=true.
// ---------------------------------------------------------------------------
TEST(ControlChannelExpectType, WrongTypeByteRejected) {
    // Build a Welcome but try to Parse it as a Challenge.
    WelcomeMessage w;
    w.levelName = "Map";
    w.gameName = "Game";
    w.flags = 7;
    std::vector<uint8_t> bytes = BuildWelcome(w);

    ChallengeMessage out;
    EXPECT_FALSE(ParseChallenge(bytes.data(), bytes.size(), out, /*expectType=*/true));
}

// When expectType=false, the caller is responsible for the type byte. Parsing
// the BODY (skipping the leading byte) must succeed.
TEST(ControlChannelExpectType, SkipTypeByteParsesBody) {
    ChallengeMessage in;
    in.nonce = 0x12345678u;
    std::vector<uint8_t> bytes = BuildChallenge(in);
    ASSERT_GE(bytes.size(), 1u);

    // Hand the parser the body WITHOUT the leading type byte.
    ChallengeMessage out;
    ASSERT_TRUE(ParseChallenge(bytes.data() + 1, bytes.size() - 1, out, /*expectType=*/false));
    EXPECT_EQ(out.nonce, in.nonce);
}

// ---------------------------------------------------------------------------
// Short / truncated buffers must fail cleanly (overflow flag -> false), never
// read out of bounds.
// ---------------------------------------------------------------------------
TEST(ControlChannelOverflow, TruncatedLoginFails) {
    LoginMessage in;
    in.response = "r";
    in.url = "u";
    in.steamId = 0xDEADBEEFull;
    std::vector<uint8_t> bytes = BuildLogin(in);

    // Lop off the trailing QWORD region so the SteamId read overflows.
    ASSERT_GT(bytes.size(), 4u);
    LoginMessage out;
    EXPECT_FALSE(ParseLogin(bytes.data(), bytes.size() - 4, out));
}

TEST(ControlChannelOverflow, EmptyBufferFails) {
    HelloMessage out;
    EXPECT_FALSE(ParseHello(nullptr, 0, out));

    NMT t;
    EXPECT_FALSE(PeekType(nullptr, 0, t));
}

// ---------------------------------------------------------------------------
// PeekType returns the leading NMT byte without consuming/parsing the body.
// ---------------------------------------------------------------------------
TEST(ControlChannelPeek, ReturnsLeadingType) {
    FailureMessage f;
    f.errorKey = "x";
    std::vector<uint8_t> bytes = BuildFailure(f);

    NMT t;
    ASSERT_TRUE(PeekType(bytes.data(), bytes.size(), t));
    EXPECT_EQ(t, NMT::Failure);
}

RS2V_TEST_MAIN()
