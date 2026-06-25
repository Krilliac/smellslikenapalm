// tests/HandshakeIntegrationTests.cpp
//
// End-to-end integration test of the control-channel handshake over the REAL UE3
// wire codec, with no sockets. It wires the same pieces ConnectionManager does:
//
//   client: ControlChannel::Build* -> PacketAssembler (fragment + sequence)
//           -> PacketCodec::Encode -> wire datagrams
//   server: PacketCodec::Decode -> ack -> ControlReassembler (order/dedup/peel)
//           -> HandshakeState::HandleControlMessage
//   server responses: HandshakeState emits message payloads -> server
//           PacketAssembler -> PacketCodec::Encode -> wire datagrams (decoded
//           back on the client side to verify the response).
//
// This exercises the whole stack against realistic, multi-bunch messages -
// crucially the real Hello body (versions, SteamId, FString session/token) parsed
// by OnHello - so it de-risks a live retail-client connect.

#include <gtest/gtest.h>

#include "Network/HandshakeState.h"
#include "Network/ControlChannel.h"
#include "Network/PacketCodec.h"
#include "Network/PacketAssembler.h"
#include "Network/ControlReassembler.h"
#include "Network/NetMessages.h"

#include <vector>
#include <cstdint>

namespace {

// A simulated peer endpoint: an outbound PacketAssembler whose produced packets
// are encoded and captured as wire datagrams.
struct Endpoint {
    PacketCodec::PacketAssembler out;
    std::vector<std::vector<uint8_t>> sent; // encoded datagrams this peer "sent"

    void Send(const std::vector<uint8_t>& messagePayload) {
        for (const PacketCodec::Packet& pkt : out.BuildControlMessagePackets(messagePayload)) {
            sent.push_back(PacketCodec::Encode(pkt));
        }
    }
};

// Decode every datagram and reassemble its control messages via `reasm`.
void Deliver(const std::vector<std::vector<uint8_t>>& datagrams,
             PacketCodec::PacketAssembler* ackTo,
             PacketCodec::ControlReassembler& reasm) {
    for (const std::vector<uint8_t>& dg : datagrams) {
        PacketCodec::Packet pkt = PacketCodec::Decode(dg.data(), dg.size());
        ASSERT_TRUE(pkt.ok) << "server/client failed to decode a peer datagram";
        if (ackTo) {
            ackTo->QueueAck(pkt.packetId);
        }
        for (const PacketCodec::Bunch& b : pkt.bunches) {
            reasm.OnBunch(b);
        }
    }
}

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

} // namespace

// A realistic, multi-bunch Hello reassembles server-side, OnHello parses its body,
// the handshake advances to ChallengeSent, and the framed Challenge response
// decodes back on the client as NMT_Challenge with the recorded nonce.
TEST(HandshakeIntegration, RealHelloAdvancesToChallenge) {
    Endpoint server;
    std::vector<std::vector<uint8_t>> serverResponses;

    HandshakeState handshake(
        /*clientId*/ 1u,
        /*rawSend*/ [&](const std::vector<uint8_t>& payload) {
            for (const PacketCodec::Packet& pkt : server.out.BuildControlMessagePackets(payload)) {
                serverResponses.push_back(PacketCodec::Encode(pkt));
            }
        });

    PacketCodec::ControlReassembler serverReasm(
        [&](const std::vector<uint8_t>& msg) { handshake.HandleControlMessage(msg); });

    // Client sends a (multi-bunch) Hello.
    Endpoint client;
    client.Send(ControlChannel::BuildHello(MakeRealHello()));
    Deliver(client.sent, &server.out, serverReasm);

    // Server advanced and recorded a nonce.
    EXPECT_EQ(handshake.Phase(), HandshakePhase::ChallengeSent);
    EXPECT_FALSE(handshake.Challenge().empty());
    ASSERT_FALSE(serverResponses.empty());

    // The server's framed response decodes/reassembles back to a Challenge.
    std::vector<std::vector<uint8_t>> clientMsgs;
    PacketCodec::ControlReassembler clientReasm(
        [&](const std::vector<uint8_t>& m) { clientMsgs.push_back(m); });
    Deliver(serverResponses, /*ackTo*/ nullptr, clientReasm);

    ASSERT_FALSE(clientMsgs.empty());
    NMT type{};
    ASSERT_TRUE(ControlChannel::PeekType(clientMsgs[0].data(), clientMsgs[0].size(), type));
    EXPECT_EQ(type, NMT::Challenge);

    ControlChannel::ChallengeMessage chal;
    ASSERT_TRUE(ControlChannel::ParseChallenge(clientMsgs[0].data(), clientMsgs[0].size(), chal));
    EXPECT_EQ(chal.challenge, handshake.Challenge());
}

// Drive the full happy-path sequence Hello -> Netspeed -> Login -> Join and verify
// the server-side state machine reaches Joined and fires the Game-facing events.
TEST(HandshakeIntegration, FullHandshakeReachesJoined) {
    Endpoint server;
    bool loggedIn = false;
    bool joined = false;
    uint64_t loggedInSteamId = 0;

    HandshakeState handshake(
        /*clientId*/ 7u,
        /*rawSend*/ [&](const std::vector<uint8_t>& payload) {
            for (const PacketCodec::Packet& pkt : server.out.BuildControlMessagePackets(payload)) {
                server.sent.push_back(PacketCodec::Encode(pkt));
            }
        },
        /*onLoggedIn*/ [&](const ClientLoggedInEvent& ev) { loggedIn = true; loggedInSteamId = ev.steamId; },
        /*onJoined*/   [&](const ClientJoinedEvent&) { joined = true; });

    PacketCodec::ControlReassembler serverReasm(
        [&](const std::vector<uint8_t>& msg) { handshake.HandleControlMessage(msg); });

    // One client endpoint => one continuous reliable ChSequence stream across all
    // four messages, exactly as a real client sends them.
    Endpoint client;

    client.Send(ControlChannel::BuildHello(MakeRealHello()));
    Deliver(client.sent, &server.out, serverReasm);
    client.sent.clear();
    ASSERT_EQ(handshake.Phase(), HandshakePhase::ChallengeSent);

    ControlChannel::NetspeedMessage ns; ns.netspeed = kNetspeedInternet;
    client.Send(ControlChannel::BuildNetspeed(ns));
    Deliver(client.sent, &server.out, serverReasm);
    client.sent.clear();
    EXPECT_EQ(handshake.Netspeed(), kNetspeedInternet);

    ControlChannel::LoginMessage login;
    login.response = "challenge-response";
    login.url = "VNTE-CuChi?Name=Recon?Team=0";
    login.steamId = 0x0110000112345678ull;
    client.Send(ControlChannel::BuildLogin(login));
    Deliver(client.sent, &server.out, serverReasm);
    client.sent.clear();
    ASSERT_EQ(handshake.Phase(), HandshakePhase::WelcomeSent);
    EXPECT_TRUE(loggedIn);
    EXPECT_EQ(loggedInSteamId, 0x0110000112345678ull);

    client.Send(ControlChannel::BuildJoin(ControlChannel::JoinMessage{}));
    Deliver(client.sent, &server.out, serverReasm);

    EXPECT_EQ(handshake.Phase(), HandshakePhase::Joined);
    EXPECT_TRUE(handshake.IsJoined());
    EXPECT_TRUE(joined);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
