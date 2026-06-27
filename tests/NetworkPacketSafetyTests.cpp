// tests/NetworkPacketSafetyTests.cpp
//
// SECURITY REGRESSION tests for the legacy Packet read API (src/Network/NetworkPacket.cpp).
//
// The legacy readers (ReadString/DecodeString, ReadUInt, ReadFloat, ReadBytes) consume
// wire-controlled data. Before hardening they did NOT bounds-check the read offset/length
// against the payload size (unlike the guarded ReadUInt16/32/64): an attacker CHAT_MESSAGE
// payload with an oversize 4-byte string length prefix (e.g. 0xFFFFFFFF) constructed a
// multi-GB std::string reading far past the buffer (OOB read -> crash/DoS or heap info
// disclosure), and a too-short payload over-read the length prefix itself.
//
// These tests feed malformed payloads and assert the readers fail safe (empty/0, no crash,
// no over-read), and that well-formed values still decode byte-for-byte (the fix is purely
// additive). Found by the packet-dispatch security review (workflow wf_fff418dc-46f).

#include "TestFramework.h"

#include <cstdint>
#include <vector>
#include <string>

#include "Network/Packet.h"
#include "Protocol/MessageDecoder.h"

namespace {

// Build a Packet whose read-payload (m_payload) is exactly `bytes`; the read offset
// starts at 0. WriteBytes appends to m_payload, which the legacy readers consume.
Packet WithPayload(const std::vector<uint8_t>& bytes) {
    Packet p;
    p.WriteBytes(bytes);
    return p;
}

} // namespace

// Empty payload: every legacy reader returns safely with no out-of-bounds access.
TEST(NetworkPacketSafety, EmptyPayloadReadsAreSafe) {
    Packet p = WithPayload({});
    EXPECT_TRUE(p.ReadString().empty());
    EXPECT_EQ(p.ReadUInt(), 0u);
    EXPECT_FLOAT_EQ(p.ReadFloat(), 0.0f);
    EXPECT_TRUE(p.ReadBytes(16).empty());
}

// The headline bug: an oversize string length prefix must NOT trigger a ~4GB read.
TEST(NetworkPacketSafety, OversizeStringLengthIsRejected) {
    EXPECT_TRUE(WithPayload({0xFF, 0xFF, 0xFF, 0xFF}).ReadString().empty());
    // length says 10 but only 3 string bytes follow
    EXPECT_TRUE(WithPayload({0x0A, 0x00, 0x00, 0x00, 'a', 'b', 'c'}).ReadString().empty());
}

// A length prefix that is itself truncated (fewer than 4 bytes) is rejected, not over-read.
TEST(NetworkPacketSafety, TruncatedLengthPrefixIsRejected) {
    EXPECT_TRUE(WithPayload({0x01, 0x00}).ReadString().empty());
    EXPECT_TRUE(WithPayload({0x01}).ReadString().empty());
}

// ReadBytes with a count beyond the payload returns empty instead of over-reading.
TEST(NetworkPacketSafety, ReadBytesBeyondPayloadIsRejected) {
    EXPECT_TRUE(WithPayload({1, 2, 3}).ReadBytes(100).empty());
    EXPECT_TRUE(WithPayload({}).ReadBytes(1).empty());
}

// Well-formed values still decode exactly as before — the hardening is purely additive.
TEST(NetworkPacketSafety, WellFormedValuesStillDecode) {
    EXPECT_EQ(WithPayload({0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c'}).ReadString(), "abc");
    EXPECT_EQ(WithPayload({0x00, 0x00, 0x00, 0x00}).ReadString(), "");   // valid empty string
    EXPECT_EQ(WithPayload({0x04, 0x00, 0x00, 0x00}).ReadUInt(), 4u);
    EXPECT_EQ(WithPayload({1, 2, 3, 4}).ReadBytes(4).size(), 4u);
}

// MessageDecoder::DecodeAction reads an attacker-controlled argument count and then
// loops that many times reading a string per iteration. The per-string reads are
// bounds-safe (return ""), but an unbounded loop count (e.g. 0xFFFFFFFF) would push
// billions of empty strings into the output vector, exhausting memory (DoS) before any
// read fails. The decoder must reject an argCount that cannot fit in the remaining
// payload (each arg costs >= 4 bytes on the wire) rather than attempt the loop.
TEST(NetworkPacketSafety, DecodeActionRejectsHostileArgCount) {
    uint32_t clientId = 0;
    std::string action;
    std::vector<std::string> args;

    // argCount = 0xFFFFFFFF with no argument bytes following.
    Packet huge("PLAYER_ACTION");
    huge.WriteUInt(7);              // clientId
    huge.WriteString("kick");      // action
    huge.WriteUInt(0xFFFFFFFFu);   // argCount — hostile
    EXPECT_FALSE(MessageDecoder::DecodeAction(huge, clientId, action, args));
    EXPECT_TRUE(args.empty());

    // A large-but-not-max count with only a few payload bytes left is also rejected.
    Packet many("PLAYER_ACTION");
    many.WriteUInt(7);
    many.WriteString("kick");
    many.WriteUInt(1000000u);      // claims 1e6 args; nowhere near that many bytes follow
    EXPECT_FALSE(MessageDecoder::DecodeAction(many, clientId, action, args));
    EXPECT_TRUE(args.empty());
}

RS2V_TEST_MAIN()
