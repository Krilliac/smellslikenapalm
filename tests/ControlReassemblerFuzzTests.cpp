// tests/ControlReassemblerFuzzTests.cpp
//
// MALFORMED-INPUT / FUZZ tests for the inbound control-channel reassembler
// (src/Network/ControlReassembler). These complement ControlReassemblerTests.cpp
// (which proves the happy-path ordering/dedup/skip-gap contract) by proving the
// reassembler SURVIVES HOSTILE input:
//
//   * no crash / no out-of-bounds read on any bunch shape
//   * no UNBOUNDED buffering (the pending map stays bounded regardless of how
//     many out-of-order / scattered / huge-ChSequence bunches an attacker floods)
//   * the documented 256 KiB / 128-bunch pending caps are never exceeded
//   * a bunch whose declared payloadBits lies about its payload size is rejected
//     and never causes an over-read
//   * valid input still round-trips after a storm of hostile input (fresh state)
//
// Fuzz strategy per the task brief: (a) random bytes of varied lengths, (b)
// mutation of a valid bunch (flip flags / truncate / extend / overwrite the
// payloadBits "length field"), (c) boundary values (0-length, 1-byte, max-length,
// all-0xFF, alternating, huge/zero ChSequence). A per-loop WATCHDOG aborts the
// process if any single OnBunch fails to return promptly (catches an infinite
// loop / hang in Drain).
//
// Build:  cmake --build build-tests --target ControlReassemblerFuzzTests --config Debug -- /m:1
// Run:    build-tests/tests/Debug/ControlReassemblerFuzzTests.exe

#include "TestFramework.h"

#include "Network/ControlReassembler.h"
#include "Network/PacketCodec.h"
#include "Network/NetMessages.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using PacketCodec::Bunch;
using PacketCodec::ControlReassembler;

namespace {

// The reassembler's own documented DoS caps (mirror of the constants in
// ControlReassembler.cpp). These are the invariants the fuzz asserts.
constexpr size_t kDocPendingCountCap = 128;          // kMaxPending
constexpr size_t kDocPendingByteCap  = 256 * 1024;   // kMaxPendingBytes
constexpr uint32_t kDocMaxSeqAhead   = 64;           // kMaxSeqAhead

// ---------------------------------------------------------------------------
// Watchdog: run `fn` on a worker thread; if it does not finish within `budget`,
// a single OnBunch (or the whole loop) is hung -> abort the test binary loudly
// with a non-zero exit so CI flags it (a real hang would otherwise wedge forever).
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
                     "[WATCHDOG] '%s' exceeded %lld ms - probable infinite loop/hang in "
                     "ControlReassembler; aborting.\n",
                     what, static_cast<long long>(budget.count()));
        std::fflush(stderr);
        std::abort();  // turn a hang into a hard, visible test failure
    }
    worker.join();
}

// Build a control bunch with caller-chosen (possibly hostile) fields.
Bunch MakeBunch(uint32_t seq, std::vector<uint8_t> payload, uint32_t payloadBits,
                bool reliable = true,
                uint32_t chIndex = static_cast<uint32_t>(kControlChannelIndex)) {
    Bunch b;
    b.bReliable = reliable;
    b.chIndex = chIndex;
    b.chType = PacketCodec::kControlChannelType;
    b.chSequence = seq;
    b.payload = std::move(payload);
    b.payloadBits = payloadBits;
    return b;
}

// A well-formed control bunch (payloadBits matches payload size).
Bunch MakeValidBunch(uint32_t seq, const std::vector<uint8_t>& payload) {
    return MakeBunch(seq, payload, static_cast<uint32_t>(payload.size() * 8));
}

// Generate a random byte vector of a fuzz-interesting length / content.
std::vector<uint8_t> RandomPayload(std::mt19937& rng) {
    std::uniform_int_distribution<int> lenPick(0, 6);
    size_t len = 0;
    switch (lenPick(rng)) {
        case 0: len = 0; break;                          // zero-length
        case 1: len = 1; break;                          // 1-byte
        case 2: len = 2; break;
        case 3: len = (rng() % 64) + 1; break;           // small
        case 4: len = (rng() % 2048) + 1; break;         // up to one NMT bunch
        case 5: len = (rng() % 8192) + 1; break;         // oversized
        default: len = 16384; break;                     // max-ish
    }
    std::vector<uint8_t> v(len);
    std::uniform_int_distribution<int> pat(0, 3);
    switch (pat(rng)) {
        case 0: for (auto& b : v) b = static_cast<uint8_t>(rng()); break;  // random
        case 1: for (auto& b : v) b = 0xFF; break;                         // all-0xFF
        case 2: for (auto& b : v) b = 0x00; break;                         // all-zero
        default:                                                           // alternating
            for (size_t i = 0; i < v.size(); ++i) v[i] = (i & 1) ? 0xAA : 0x55;
            break;
    }
    return v;
}

} // namespace

// ===========================================================================
// (a) Random-bytes fuzz: thousands of fully-random bunches. Assert the pending
//     map is ALWAYS bounded (no unbounded buffering) and nothing crashes.
// ===========================================================================
TEST(ControlReassemblerFuzz, RandomBunchesStayBounded) {
    std::mt19937 rng(0xC0FFEEu);

    RunWithWatchdog([&] {
        size_t deliveries = 0;
        size_t maxPending = 0;
        ControlReassembler re([&](const std::vector<uint8_t>&) { ++deliveries; });

        for (int iter = 0; iter < 200000; ++iter) {
            // Random ChSequence across the whole interesting range: tiny, near the
            // current counter, far-future, zero, and the uint32 top (wrap bait).
            uint32_t seq;
            switch (rng() % 6) {
                case 0: seq = 0; break;
                case 1: seq = rng() % 8; break;
                case 2: seq = re.NextSequence() + (rng() % 200); break;  // straddles the 64-ahead cap
                case 3: seq = rng() % 1024; break;                       // UE3 channel-seq range
                case 4: seq = 0xFFFFFFFFu - (rng() % 4); break;          // near uint32 max
                default: seq = rng(); break;                             // anywhere
            }
            std::vector<uint8_t> payload = RandomPayload(rng);
            // payloadBits: usually honest, sometimes a LIE (too big / too small / max).
            uint32_t bits;
            switch (rng() % 5) {
                case 0: bits = static_cast<uint32_t>(payload.size() * 8); break;  // honest
                case 1: bits = static_cast<uint32_t>(payload.size() * 8) + 7; break; // slightly over
                case 2: bits = 0xFFFFFFFFu; break;                                // absurd
                case 3: bits = 0; break;                                          // claims empty
                default: bits = static_cast<uint32_t>(rng()); break;             // random
            }
            bool reliable = (rng() & 1) != 0;
            uint32_t chIndex = (rng() % 4 == 0) ? (rng() % 1024)
                                                : static_cast<uint32_t>(kControlChannelIndex);

            re.OnBunch(MakeBunch(seq, std::move(payload), bits, reliable, chIndex));

            // INVARIANTS that must hold after EVERY hostile bunch:
            maxPending = std::max(maxPending, re.PendingBunchCount());
            ASSERT_LE(re.PendingBunchCount(), kDocPendingCountCap)
                << "pending bunch count exceeded the documented cap at iter " << iter;
        }
        // No unbounded buffering: the skip-gap + caps keep pending tiny no matter
        // how the attacker scatters sequences. (Empirically << kDocPendingCountCap.)
        EXPECT_LE(maxPending, kDocPendingCountCap);
        std::fprintf(stderr, "[fuzz] random: deliveries=%zu maxPending=%zu\n",
                     deliveries, maxPending);
    }, std::chrono::seconds(30), "RandomBunchesStayBounded");
}

// ===========================================================================
// (b) Mutation fuzz: start from a valid two-message stream and mutate one field
//     (flags / seq / truncate / extend / overwrite payloadBits) each iteration.
//     Assert no crash and bounded pending.
// ===========================================================================
TEST(ControlReassemblerFuzz, MutatedValidStreamSurvives) {
    std::mt19937 rng(0xBADC0DEu);
    const std::vector<uint8_t> base = {NMTByte(NMT::Netspeed), 0x80, 0x1A, 0x06, 0x00};

    RunWithWatchdog([&] {
        for (int iter = 0; iter < 100000; ++iter) {
            ControlReassembler re([&](const std::vector<uint8_t>&) {});
            // Deliver a clean seq 1 first so the stream is "live".
            re.OnBunch(MakeValidBunch(1, base));

            // Build a mutated seq-2 bunch.
            std::vector<uint8_t> p = base;
            uint32_t seq = 2;
            uint32_t bits = static_cast<uint32_t>(p.size() * 8);
            bool reliable = true;
            uint32_t chIndex = static_cast<uint32_t>(kControlChannelIndex);

            switch (rng() % 8) {
                case 0: if (!p.empty()) p[rng() % p.size()] ^= (1u << (rng() % 8)); break; // bitflip
                case 1: if (!p.empty()) p.resize(rng() % p.size()); break;                  // truncate
                case 2: p.resize(p.size() + (rng() % 4096), static_cast<uint8_t>(rng())); break; // extend
                case 3: bits = static_cast<uint32_t>(rng()); break;                         // bad length field
                case 4: seq = rng(); break;                                                  // bad seq
                case 5: reliable = false; break;                                             // wrong flag
                case 6: chIndex = rng() % 1024; break;                                       // wrong channel
                default: bits += (rng() % 64); break;                                        // length skew
            }
            re.OnBunch(MakeBunch(seq, std::move(p), bits, reliable, chIndex));
            ASSERT_LE(re.PendingBunchCount(), kDocPendingCountCap) << "iter " << iter;
        }
    }, std::chrono::seconds(30), "MutatedValidStreamSurvives");
}

// ===========================================================================
// A bunch that LIES about its size (payloadBits >> payload.size()*8) must be
// rejected and must NOT over-read: nothing is delivered, nothing is buffered.
// ===========================================================================
TEST(ControlReassemblerFuzz, RejectsPayloadBitsOverrun) {
    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });

    // 4 real bytes (32 bits) but claims 4096 bits -> over-read bait at seq 1.
    re.OnBunch(MakeBunch(1, std::vector<uint8_t>{0x04, 0x80, 0x1A, 0x06}, 4096));
    EXPECT_TRUE(got.empty()) << "lying bunch must be dropped, not delivered";
    EXPECT_EQ(re.PendingBunchCount(), 0u) << "lying bunch must not be buffered";
    EXPECT_EQ(re.NextSequence(), 1u) << "stream must not advance on a rejected bunch";

    // The honest version of the same seq is still accepted afterwards.
    re.OnBunch(MakeBunch(1, std::vector<uint8_t>{0x04, 0x80, 0x1A, 0x06}, 32));
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], (std::vector<uint8_t>{0x04, 0x80, 0x1A, 0x06}));
}

// ===========================================================================
// Huge / far-future ChSequence flood: an attacker spraying sequences far ahead
// of m_nextSeq (and near the uint32 top, to bait wrap) must never buffer them.
// Proves the kMaxSeqAhead window + caps bound memory.
// ===========================================================================
TEST(ControlReassemblerFuzz, HugeSequenceFloodNeverBuffers) {
    ControlReassembler re([&](const std::vector<uint8_t>&) {});

    // Sequences beyond m_nextSeq(=1)+64 must be ignored, including uint32 max
    // (which would WRAP a naive m_nextSeq+kMaxSeqAhead comparison).
    for (uint32_t i = 0; i < 5000; ++i) {
        uint32_t seq = 1 + kDocMaxSeqAhead + 1 + i;        // just past the window and up
        re.OnBunch(MakeValidBunch(seq, {0xAA, 0xBB}));
        re.OnBunch(MakeValidBunch(0xFFFFFFFFu - (i % 7), {0xCC}));  // near-top, wrap bait
        ASSERT_EQ(re.PendingBunchCount(), 0u) << "far-future seq must not buffer (i=" << i << ")";
    }
    EXPECT_EQ(re.NextSequence(), 1u);
}

// ===========================================================================
// Adversarial out-of-order / scattered / oversized storm aimed squarely at the
// pending caps: large payloads at scattered in-window sequences with a permanent
// gap at m_nextSeq. Proves buffering stays bounded (no unbounded allocation) and
// the byte/count caps hold under sustained pressure.
// ===========================================================================
TEST(ControlReassemblerFuzz, ScatteredOversizedStormStaysCapped) {
    std::mt19937 rng(0x5EED1234u);
    size_t maxPending = 0;

    RunWithWatchdog([&] {
        ControlReassembler re([&](const std::vector<uint8_t>&) {});
        // Never deliver seq 1: hold a permanent gap so buffering is exercised.
        // Spray large (8 KiB) bunches at scattered sequences inside the 64-window.
        std::vector<uint8_t> big(8192, 0xEE);
        for (int iter = 0; iter < 50000; ++iter) {
            uint32_t base = re.NextSequence();
            // Scatter within [base+1, base+kMaxSeqAhead]; skip base to keep a gap.
            uint32_t seq = base + 1 + (rng() % kDocMaxSeqAhead);
            re.OnBunch(MakeValidBunch(seq, big));
            maxPending = std::max(maxPending, re.PendingBunchCount());
            ASSERT_LE(re.PendingBunchCount(), kDocPendingCountCap) << "iter " << iter;
            // Byte ceiling: count*8KiB must stay under the 256 KiB cap + one bunch
            // of slack. (Proves the pending bytes never blow past the documented cap.)
            ASSERT_LE(re.PendingBunchCount() * big.size(), kDocPendingByteCap + big.size())
                << "buffered bytes exceeded the documented 256 KiB cap at iter " << iter;
        }
    }, std::chrono::seconds(30), "ScatteredOversizedStormStaysCapped");

    std::fprintf(stderr, "[fuzz] scattered-oversized: maxPending=%zu (cap=%zu)\n",
                 maxPending, kDocPendingCountCap);
}

// ===========================================================================
// Boundary bunches: 0-length, 1-byte, all-0xFF max-length, alternating, plus a
// zero-payloadBits bunch at the live sequence (must advance the stream without
// delivering an empty message). None may crash.
// ===========================================================================
TEST(ControlReassemblerFuzz, BoundaryBunches) {
    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });

    // seq 1: zero-length payload + zero payloadBits => Drain skips delivery but
    // the stream must still advance to 2 (no stall, no empty callback).
    re.OnBunch(MakeBunch(1, {}, 0));
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(re.NextSequence(), 2u);

    // seq 2: 1-byte real message.
    re.OnBunch(MakeValidBunch(2, {0x09}));  // NMT_Join (empty-body), 1 byte
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], (std::vector<uint8_t>{0x09}));

    // seq 3: a max-length all-0xFF bunch (16384 bytes). Must deliver intact.
    std::vector<uint8_t> maxBunch(16384, 0xFF);
    re.OnBunch(MakeValidBunch(3, maxBunch));
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1].size(), 16384u);

    // seq 4: alternating pattern.
    std::vector<uint8_t> alt(100);
    for (size_t i = 0; i < alt.size(); ++i) alt[i] = (i & 1) ? 0xAA : 0x55;
    re.OnBunch(MakeValidBunch(4, alt));
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[2], alt);
}

// ===========================================================================
// Regression: valid input STILL round-trips on a fresh reassembler after all the
// hostile fuzzing above (proves the fuzz did not rely on / corrupt shared state).
// ===========================================================================
TEST(ControlReassemblerFuzz, ValidStreamStillRoundTripsAfterFuzz) {
    const std::vector<uint8_t> m1 = {NMTByte(NMT::Hello), 0x01};
    const std::vector<uint8_t> m2 = {NMTByte(NMT::Netspeed), 0x80, 0x1A, 0x06, 0x00};
    const std::vector<uint8_t> m3 = {NMTByte(NMT::Join)};

    std::vector<std::vector<uint8_t>> got;
    ControlReassembler re([&](const std::vector<uint8_t>& m) { got.push_back(m); });
    re.OnBunch(MakeValidBunch(3, m3));   // out of order
    re.OnBunch(MakeValidBunch(1, m1));
    re.OnBunch(MakeValidBunch(1, m1));   // duplicate
    re.OnBunch(MakeValidBunch(2, m2));

    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], m1);
    EXPECT_EQ(got[1], m2);
    EXPECT_EQ(got[2], m3);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
