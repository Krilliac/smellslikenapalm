// tests/HandshakeStateFuzzTests.cpp
//
// MALFORMED-INPUT / FUZZ tests for the per-connection control-channel handshake
// state machine (src/Network/HandshakeState). HandshakeState::HandleControlMessage
// is documented to "never throw; malformed input is logged + ignored" - these
// tests PROVE that contract against hostile input by feeding it:
//
//   * fully-random control-message payloads of every interesting length
//   * truncated / oversized bodies for EVERY handled NMT type (Hello, Netspeed,
//     Login, Join, SteamLogin, SteamAuth)
//   * out-of-range / garbage NMT type bytes (incl. > kNMTMaxCase, which the UE3
//     ReceivedBunch switch rejects) and every byte value 0x00..0xFF
//   * malformed pre-NMT StatelessConnect handshake subtypes
//   * mutation of a valid Hello->Netspeed->Login->Join stream
//
// Asserts: no crash / no over-read / no hang (per-loop WATCHDOG), the phase enum
// stays a valid value, and the machine never advances past Joined on garbage. A
// final regression check proves a valid handshake STILL completes after the fuzz.
//
// Build: cmake --build build-tests --target HandshakeStateFuzzTests --config Debug -- /m:1
// Run:   build-tests/tests/Debug/HandshakeStateFuzzTests.exe

#include "TestFramework.h"

#include "Network/HandshakeState.h"
#include "Network/ControlChannel.h"
#include "Network/NetMessages.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Watchdog (same intent as the reassembler fuzz): abort loudly on a hang so a
// possible infinite loop in the parse/dispatch path cannot wedge the suite.
// ---------------------------------------------------------------------------
template <typename F>
void RunWithWatchdog(F&& fn, std::chrono::milliseconds budget, const char* what) {
    std::atomic<bool> done{false};
    std::thread worker([&] { fn(); done.store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!done.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
                     "[WATCHDOG] '%s' exceeded %lld ms - probable hang in HandshakeState; aborting.\n",
                     what, static_cast<long long>(budget.count()));
        std::fflush(stderr);
        std::abort();
    }
    worker.join();
}

bool PhaseIsValid(HandshakePhase p) {
    switch (p) {
        case HandshakePhase::AwaitingHello:
        case HandshakePhase::ChallengeSent:
        case HandshakePhase::AwaitingLogin:
        case HandshakePhase::WelcomeSent:
        case HandshakePhase::Joined:
        case HandshakePhase::Rejected:
            return true;
    }
    return false;
}

// A HandshakeState wired with a recording raw-send (so emissions don't crash and
// can be inspected). Optionally driven through the StatelessConnect handshake so
// it is in the NMT phase before fuzzing the NMT switch.
struct Harness {
    std::vector<std::vector<uint8_t>> emitted;
    HandshakeState hs;

    explicit Harness(uint32_t id = 1)
        : hs(id, [this](const std::vector<uint8_t>& p) { emitted.push_back(p); }) {}

    // Complete 0x1d/0x1f so IsControlHandshakeComplete() and the NMT switch run.
    void EnterNmtPhase() {
        hs.HandleControlMessage(std::vector<uint8_t>{ControlChannel::Handshake::kStart});    // 0x1d
        hs.HandleControlMessage(std::vector<uint8_t>{ControlChannel::Handshake::kResponse}); // 0x1f
    }
};

std::vector<uint8_t> RandomBytes(std::mt19937& rng) {
    std::uniform_int_distribution<int> lenPick(0, 6);
    size_t len = 0;
    switch (lenPick(rng)) {
        case 0: len = 0; break;
        case 1: len = 1; break;
        case 2: len = 2; break;
        case 3: len = (rng() % 32) + 1; break;
        case 4: len = (rng() % 512) + 1; break;
        case 5: len = (rng() % 5000) + 1; break;   // oversized (past the 4096 Login URL cap)
        default: len = 8192; break;
    }
    std::vector<uint8_t> v(len);
    switch (rng() % 4) {
        case 0: for (auto& b : v) b = static_cast<uint8_t>(rng()); break;
        case 1: for (auto& b : v) b = 0xFF; break;
        case 2: for (auto& b : v) b = 0x00; break;
        default: for (size_t i = 0; i < v.size(); ++i) v[i] = (i & 1) ? 0xAA : 0x55; break;
    }
    return v;
}

} // namespace

// ===========================================================================
// Random payloads fed to a FRESH machine that is already in the NMT phase. Every
// call must return; the phase must stay valid and never crash.
// ===========================================================================
TEST(HandshakeStateFuzz, RandomNmtPayloads) {
    std::mt19937 rng(0xA11CE5u);

    RunWithWatchdog([&] {
        for (int iter = 0; iter < 200000; ++iter) {
            Harness h(static_cast<uint32_t>(iter));
            h.EnterNmtPhase();
            ASSERT_TRUE(h.hs.IsControlHandshakeComplete());

            std::vector<uint8_t> msg = RandomBytes(rng);
            h.hs.HandleControlMessage(msg);
            ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "invalid phase at iter " << iter;
        }
    }, std::chrono::seconds(30), "RandomNmtPayloads");
}

// ===========================================================================
// Random payloads fed to a machine STILL in the pre-NMT StatelessConnect phase
// (routes through HandleHandshakeMessage). Must also survive cleanly.
// ===========================================================================
TEST(HandshakeStateFuzz, RandomPreHandshakePayloads) {
    std::mt19937 rng(0xBEEF77u);

    RunWithWatchdog([&] {
        for (int iter = 0; iter < 200000; ++iter) {
            Harness h(static_cast<uint32_t>(iter));
            std::vector<uint8_t> msg = RandomBytes(rng);
            h.hs.HandleControlMessage(msg);
            ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "invalid phase at iter " << iter;
        }
    }, std::chrono::seconds(30), "RandomPreHandshakePayloads");
}

// ===========================================================================
// Every leading type byte 0x00..0xFF, each with a truncated body and an oversized
// body. Covers out-of-range bytes (> kNMTMaxCase = 0x25) and every handled NMT.
// Nothing may crash; phase stays valid.
// ===========================================================================
TEST(HandshakeStateFuzz, EveryTypeByteTruncatedAndOversized) {
    RunWithWatchdog([&] {
        for (int t = 0; t <= 0xFF; ++t) {
            const uint8_t type = static_cast<uint8_t>(t);

            // Truncated: type byte alone (handlers expecting a body must reject safely).
            {
                Harness h;
                h.EnterNmtPhase();
                h.hs.HandleControlMessage(std::vector<uint8_t>{type});
                ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "truncated type 0x" << std::hex << t;
            }
            // Type + a short bogus body.
            {
                Harness h;
                h.EnterNmtPhase();
                h.hs.HandleControlMessage(std::vector<uint8_t>{type, 0x00, 0xFF, 0x7F});
                ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "short body type 0x" << std::hex << t;
            }
            // Type + an oversized all-0xFF body (FString length-prefix bait: an FString
            // parse must be bounded by the buffer, not by the declared length).
            {
                Harness h;
                h.EnterNmtPhase();
                std::vector<uint8_t> msg(2048, 0xFF);
                msg[0] = type;
                h.hs.HandleControlMessage(msg);
                ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "oversized type 0x" << std::hex << t;
            }
        }
    }, std::chrono::seconds(20), "EveryTypeByteTruncatedAndOversized");
}

// ===========================================================================
// Out-of-range NMT type bytes (> kNMTMaxCase) must be IGNORED, never dispatched.
// The machine must not advance out of AwaitingHello on them.
// ===========================================================================
TEST(HandshakeStateFuzz, OutOfRangeTypeBytesIgnored) {
    for (int t = kNMTMaxCase + 1; t <= 0xFF; ++t) {
        Harness h;
        h.EnterNmtPhase();
        ASSERT_EQ(h.hs.Phase(), HandshakePhase::AwaitingHello);
        // A long body so any (wrongly-reached) handler would have data to misparse.
        std::vector<uint8_t> msg(64, 0xCD);
        msg[0] = static_cast<uint8_t>(t);
        h.hs.HandleControlMessage(msg);
        EXPECT_EQ(h.hs.Phase(), HandshakePhase::AwaitingHello)
            << "out-of-range type 0x" << std::hex << t << " must not advance the machine";
    }
}

// ===========================================================================
// FString length-prefix overflow bait for the body-bearing NMTs (Hello/Login/
// Netspeed). A 4-byte little-endian count claiming a huge string with no backing
// bytes must be rejected by the bounded BitReader, never over-read or hang.
// ===========================================================================
TEST(HandshakeStateFuzz, FStringLengthOverflowRejected) {
    const uint8_t bodyTypes[] = {
        NMTByte(NMT::Hello), NMTByte(NMT::Login), NMTByte(NMT::Netspeed),
        NMTByte(NMT::SteamLogin), NMTByte(NMT::SteamAuth),
    };
    // Claimed FString lengths, including absurd / negative-as-unsigned values.
    const uint32_t claims[] = {0x7FFFFFFFu, 0xFFFFFFFFu, 0x40000000u, 0x00100000u, 0x80000000u};

    RunWithWatchdog([&] {
        for (uint8_t type : bodyTypes) {
            for (uint32_t claim : claims) {
                Harness h;
                h.EnterNmtPhase();
                // type byte, then a 4-byte LE length, then only a couple backing bytes.
                std::vector<uint8_t> msg = {
                    type,
                    static_cast<uint8_t>(claim & 0xFF),
                    static_cast<uint8_t>((claim >> 8) & 0xFF),
                    static_cast<uint8_t>((claim >> 16) & 0xFF),
                    static_cast<uint8_t>((claim >> 24) & 0xFF),
                    0x41, 0x42, 0x00,
                };
                h.hs.HandleControlMessage(msg);
                ASSERT_TRUE(PhaseIsValid(h.hs.Phase()));
                // Must NOT have completed login off a malformed Login/Steam body.
                ASSERT_NE(h.hs.Phase(), HandshakePhase::Joined);
            }
        }
    }, std::chrono::seconds(10), "FStringLengthOverflowRejected");
}

// ===========================================================================
// Malformed pre-NMT StatelessConnect handshake subtypes: unknown subtypes, the
// server-only subtypes (0x1e/0x20), empty payloads. Must never enter the NMT
// phase off a bad subtype and never crash.
// ===========================================================================
TEST(HandshakeStateFuzz, MalformedStatelessHandshakeSubtypes) {
    for (int s = 0; s <= 0xFF; ++s) {
        Harness h;
        h.hs.HandleControlMessage(std::vector<uint8_t>{static_cast<uint8_t>(s)});
        ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "subtype 0x" << std::hex << s;
        const bool isStart = (s == ControlChannel::Handshake::kStart);
        const bool isResponse = (s == ControlChannel::Handshake::kResponse);
        if (!isStart && !isResponse) {
            // Only kStart (emits challenge, stays pre-NMT) and kResponse (completes)
            // are meaningful; everything else must leave the handshake incomplete.
            EXPECT_FALSE(h.hs.IsControlHandshakeComplete())
                << "subtype 0x" << std::hex << s << " must not complete the handshake";
        }
    }
    // kResponse alone completes the StatelessConnect handshake (blind-accept).
    {
        Harness h;
        h.hs.HandleControlMessage(std::vector<uint8_t>{ControlChannel::Handshake::kResponse});
        EXPECT_TRUE(h.hs.IsControlHandshakeComplete());
    }
}

// ===========================================================================
// Mutation fuzz over a valid Hello->Netspeed->Login->Join stream: each iteration
// corrupts ONE message (bitflip / truncate / extend / type-swap) and feeds the
// whole stream. Must never crash; phase stays valid throughout.
// ===========================================================================
TEST(HandshakeStateFuzz, MutatedValidStreamSurvives) {
    std::mt19937 rng(0xD15EA5Eu);

    ControlChannel::HelloMessage hello;
    hello.version = kEngineVersion;
    hello.minVersion = kMinNetVersion;
    hello.steamId = 0x0110000112345678ull;
    hello.leechSessionId = "eos-session";
    hello.token = "tok";

    ControlChannel::NetspeedMessage ns; ns.netspeed = kNetspeedInternet;

    ControlChannel::LoginMessage login;
    login.response = "resp";
    login.url = "VNTE-CuChi?Name=Recon?Team=0";
    login.steamId = 0x0110000112345678ull;

    const std::vector<std::vector<uint8_t>> baseStream = {
        ControlChannel::BuildHello(hello),
        ControlChannel::BuildNetspeed(ns),
        ControlChannel::BuildLogin(login),
        ControlChannel::BuildJoin(ControlChannel::JoinMessage{}),
    };

    RunWithWatchdog([&] {
        for (int iter = 0; iter < 100000; ++iter) {
            Harness h(static_cast<uint32_t>(iter));
            h.EnterNmtPhase();

            const int corruptIdx = rng() % static_cast<int>(baseStream.size());
            for (int i = 0; i < static_cast<int>(baseStream.size()); ++i) {
                std::vector<uint8_t> msg = baseStream[i];
                if (i == corruptIdx && !msg.empty()) {
                    switch (rng() % 4) {
                        case 0: msg[rng() % msg.size()] ^= (1u << (rng() % 8)); break; // bitflip
                        case 1: msg.resize(rng() % msg.size()); break;                  // truncate
                        case 2: msg.resize(msg.size() + (rng() % 256),
                                           static_cast<uint8_t>(rng())); break;          // extend
                        default: msg[0] = static_cast<uint8_t>(rng()); break;            // type swap
                    }
                }
                h.hs.HandleControlMessage(msg);
                ASSERT_TRUE(PhaseIsValid(h.hs.Phase())) << "iter " << iter << " step " << i;
            }
        }
    }, std::chrono::seconds(30), "MutatedValidStreamSurvives");
}

// ===========================================================================
// Regression: a fully VALID handshake still drives AwaitingHello -> ... -> Joined
// and fires the Game-facing events, proving the hardening/fuzz did not break the
// happy path.
// ===========================================================================
TEST(HandshakeStateFuzz, ValidHandshakeStillReachesJoined) {
    bool loggedIn = false, joined = false;
    std::vector<std::vector<uint8_t>> emitted;

    HandshakeState hs(
        42u,
        [&](const std::vector<uint8_t>& p) { emitted.push_back(p); },
        [&](const ClientLoggedInEvent&) { loggedIn = true; },
        [&](const ClientJoinedEvent&) { joined = true; });

    hs.HandleControlMessage(std::vector<uint8_t>{ControlChannel::Handshake::kStart});
    hs.HandleControlMessage(std::vector<uint8_t>{ControlChannel::Handshake::kResponse});
    ASSERT_TRUE(hs.IsControlHandshakeComplete());
    ASSERT_EQ(hs.Phase(), HandshakePhase::AwaitingHello);

    ControlChannel::HelloMessage hello;
    hello.version = kEngineVersion;
    hello.minVersion = kMinNetVersion;
    hello.steamId = 0x0110000112345678ull;
    hello.leechSessionId = "eos-session";
    hello.token = "tok";
    hs.HandleControlMessage(ControlChannel::BuildHello(hello));
    ASSERT_EQ(hs.Phase(), HandshakePhase::ChallengeSent);

    ControlChannel::NetspeedMessage ns; ns.netspeed = kNetspeedInternet;
    hs.HandleControlMessage(ControlChannel::BuildNetspeed(ns));
    EXPECT_EQ(hs.Netspeed(), kNetspeedInternet);

    ControlChannel::LoginMessage login;
    login.response = "resp";
    login.url = "VNTE-CuChi?Name=Recon?Team=0";
    login.steamId = 0x0110000112345678ull;
    hs.HandleControlMessage(ControlChannel::BuildLogin(login));
    ASSERT_EQ(hs.Phase(), HandshakePhase::WelcomeSent);
    EXPECT_TRUE(loggedIn);

    hs.HandleControlMessage(ControlChannel::BuildJoin(ControlChannel::JoinMessage{}));
    EXPECT_EQ(hs.Phase(), HandshakePhase::Joined);
    EXPECT_TRUE(joined);
}

RS2V_TEST_MAIN()
