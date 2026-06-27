// tests/HandshakeIntegrationTests.cpp
//
// End-to-end integration test of the control-channel handshake over the REAL UE3
// wire codec, with no sockets. It mirrors exactly what ConnectionManager does on
// the receive path:
//
//   client: ControlChannel::Build* -> ONE reliable control bunch per message
//           -> PacketCodec::Encode (phase-appropriate MaxPacket) -> wire datagram
//   server: PacketCodec::Decode (MaxPacket gated on IsControlHandshakeComplete)
//           -> ack -> ControlReassembler (order/dedup) -> HandshakeState
//   server responses: captured directly from HandshakeState's rawSend callback
//           (the raw message payloads the state machine emits).
//
// The real RS2 client sends each control message as a SINGLE reliable bunch (at
// the established-phase MaxPacket of 2048 a single bunch holds up to 16384 data
// bits), and the MaxPacket grows from the tiny StatelessConnect-handshake value
// to the NMT value once the handshake completes - so this test does the same.

#include "TestFramework.h"

#include "Network/HandshakeState.h"
#include "Network/ControlChannel.h"
#include "Network/PacketCodec.h"
#include "Network/ControlReassembler.h"
#include "Network/NetMessages.h"

#include <vector>
#include <cstdint>

namespace {

// A faithful client: each control message is sent as ONE reliable control-channel
// (chIndex 0) bunch with a consecutive ChSequence, encoded into its own datagram
// at the caller-supplied MaxPacket (small during the StatelessConnect handshake,
// 2048 once in the NMT phase) - exactly as the real client frames them.
struct WireClient {
    uint32_t seq = 1;
    uint32_t packetId = 0;
    std::vector<std::vector<uint8_t>> sent;

    void Send(const std::vector<uint8_t>& payload, uint32_t maxPacketBytes) {
        PacketCodec::Bunch b;
        b.bControl   = (seq == 1);   // first control bunch opens the channel
        b.bOpen      = (seq == 1);
        b.bReliable  = true;
        b.chIndex    = static_cast<uint32_t>(kControlChannelIndex);
        b.chType     = PacketCodec::kControlChannelType;
        b.chSequence = seq++;
        b.payload    = payload;
        b.payloadBits = static_cast<uint32_t>(payload.size() * 8);

        PacketCodec::Packet pkt;
        pkt.packetId = packetId++;
        pkt.bunches.push_back(b);
        pkt.ok = true;
        sent.push_back(PacketCodec::Encode(pkt, maxPacketBytes));
    }

    void Clear() { sent.clear(); }
};

// The server side: HandshakeState fed via a ControlReassembler, decoding inbound
// datagrams with the same phase-based MaxPacket selection as ConnectionManager.
struct WireServer {
    std::vector<std::vector<uint8_t>> emitted;  // raw message payloads the state machine sent
    HandshakeState* handshake = nullptr;
    PacketCodec::ControlReassembler* reasm = nullptr;

    void Deliver(const std::vector<std::vector<uint8_t>>& datagrams) {
        for (const std::vector<uint8_t>& dg : datagrams) {
            // Mirror ConnectionManager::ParseIncomingControl: the client (C2S) frames
            // at MaxPacket 2048 from the first packet - there is no small-bound phase.
            const uint32_t maxPacketBytes = PacketCodec::kNmtMaxPacketBytes;
            PacketCodec::Packet pkt = PacketCodec::Decode(dg.data(), dg.size(), maxPacketBytes);
            ASSERT_TRUE(pkt.ok) << "server failed to decode a client datagram";
            for (const PacketCodec::Bunch& b : pkt.bunches) {
                reasm->OnBunch(b);
            }
        }
    }
};

ControlChannel::HelloMessage MakeRealHello() {
    ControlChannel::HelloMessage h;
    h.bIsLittleEndian = 1;
    h.minVersion = kMinNetVersion;
    h.version = kEngineVersion;   // >= kMinNetVersion => accepted by OnHello
    h.steamId = 0x0110000112345678ull;
    h.leechSessionId = "eos-session-0123456789abcdef";
    h.token = "auth-ticket-blob";
    return h;
}

// Drive the pre-NMT StatelessConnect handshake (client 0x1d -> server 0x1e ->
// client 0x1f -> server 0x20). The handshake NMT byte (0x1d/0x1f) is the WHOLE
// message payload - there is no 0x00 family prefix - and it is framed at the
// normal MaxPacket (no small-bound phase). Leaves the server in the NMT phase
// (AwaitingHello) and clears the captured emissions/datagrams.
void CompleteStatelessHandshake(WireClient& client, WireServer& server) {
    client.Send(std::vector<uint8_t>{ControlChannel::Handshake::kStart},   // 0x1d
                PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);
    client.Clear();

    client.Send(std::vector<uint8_t>{ControlChannel::Handshake::kResponse}, // 0x1f
                PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);
    client.Clear();

    server.emitted.clear();  // discard the 0x1e / 0x20 handshake responses
}

} // namespace

// After the StatelessConnect handshake, a single-bunch Hello reassembles
// server-side, OnHello runs, the handshake advances to ChallengeSent, and the
// server emits an NMT_Challenge carrying the recorded nonce.
TEST(HandshakeIntegration, RealHelloAdvancesToChallenge) {
    WireServer server;
    HandshakeState handshake(
        /*clientId*/ 1u,
        /*rawSend*/ [&](const std::vector<uint8_t>& payload) { server.emitted.push_back(payload); });
    PacketCodec::ControlReassembler reasm(
        [&](const std::vector<uint8_t>& msg) { handshake.HandleControlMessage(msg); });
    server.handshake = &handshake;
    server.reasm = &reasm;

    WireClient client;
    CompleteStatelessHandshake(client, server);
    ASSERT_TRUE(handshake.IsControlHandshakeComplete());
    ASSERT_EQ(handshake.Phase(), HandshakePhase::AwaitingHello);

    client.Send(ControlChannel::BuildHello(MakeRealHello()), PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);

    // Server advanced and recorded a (non-zero) nonce.
    EXPECT_EQ(handshake.Phase(), HandshakePhase::ChallengeSent);
    EXPECT_NE(handshake.Challenge(), 0u);

    // The server emitted a Challenge carrying that nonce.
    ASSERT_FALSE(server.emitted.empty());
    const std::vector<uint8_t>& resp = server.emitted.back();
    NMT type{};
    ASSERT_TRUE(ControlChannel::PeekType(resp.data(), resp.size(), type));
    EXPECT_EQ(type, NMT::Challenge);

    ControlChannel::ChallengeMessage chal;
    ASSERT_TRUE(ControlChannel::ParseChallenge(resp.data(), resp.size(), chal));
    EXPECT_EQ(chal.nonce, handshake.Challenge());
}

// Drive the full NMT happy path (Hello -> Netspeed -> Login -> Join) after the
// StatelessConnect handshake, and verify the server reaches Joined and fires the
// Game-facing events with the identity captured from Hello.
TEST(HandshakeIntegration, FullHandshakeReachesJoined) {
    WireServer server;
    bool loggedIn = false;
    bool joined = false;
    uint64_t loggedInSteamId = 0;

    HandshakeState handshake(
        /*clientId*/ 7u,
        /*rawSend*/ [&](const std::vector<uint8_t>& payload) { server.emitted.push_back(payload); },
        /*onLoggedIn*/ [&](const ClientLoggedInEvent& ev) { loggedIn = true; loggedInSteamId = ev.steamId; },
        /*onJoined*/   [&](const ClientJoinedEvent&) { joined = true; });
    PacketCodec::ControlReassembler reasm(
        [&](const std::vector<uint8_t>& msg) { handshake.HandleControlMessage(msg); });
    server.handshake = &handshake;
    server.reasm = &reasm;

    // One client => one continuous reliable ChSequence stream across the
    // StatelessConnect handshake AND all four NMT messages.
    WireClient client;
    CompleteStatelessHandshake(client, server);
    ASSERT_TRUE(handshake.IsControlHandshakeComplete());

    client.Send(ControlChannel::BuildHello(MakeRealHello()), PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);
    client.Clear();
    ASSERT_EQ(handshake.Phase(), HandshakePhase::ChallengeSent);

    ControlChannel::NetspeedMessage ns; ns.netspeed = kNetspeedInternet;
    client.Send(ControlChannel::BuildNetspeed(ns), PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);
    client.Clear();
    EXPECT_EQ(handshake.Netspeed(), kNetspeedInternet);

    ControlChannel::LoginMessage login;
    login.response = "challenge-response";
    login.url = "VNTE-CuChi?Name=Recon?Team=0";
    login.steamId = 0x0110000112345678ull;
    client.Send(ControlChannel::BuildLogin(login), PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);
    client.Clear();
    ASSERT_EQ(handshake.Phase(), HandshakePhase::WelcomeSent);
    EXPECT_TRUE(loggedIn);
    EXPECT_EQ(loggedInSteamId, 0x0110000112345678ull);

    client.Send(ControlChannel::BuildJoin(ControlChannel::JoinMessage{}), PacketCodec::kNmtMaxPacketBytes);
    server.Deliver(client.sent);

    EXPECT_EQ(handshake.Phase(), HandshakePhase::Joined);
    EXPECT_TRUE(handshake.IsJoined());
    EXPECT_TRUE(joined);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
