// tests/PacketCodecFuzzTests.cpp
//
// FUZZ / hostile-input tests for the UE3 packet+bunch FRAMING codec
// (src/Network/PacketCodec.{h,cpp}).
//
// GOAL: prove PacketCodec::Decode / Encode survive arbitrary, malformed and
// adversarially-mutated byte buffers WITHOUT:
//   * crashing (the test process surviving the whole corpus is the proof),
//   * reading/writing out of bounds (BitReader's sticky-overflow guard does the
//     real work; here we assert the *structural* consequences: every decoded
//     bunch's payload buffer actually holds >= payloadBits bits, every bounded
//     field is within its SerializeInt range, and the entry count is bounded by
//     the readable bit budget so the parse loop can never spin forever),
//   * hanging (a hard watchdog thread aborts the process with the offending
//     input bytes if any single Decode/Encode call exceeds a wall-clock budget),
//   * unbounded allocation (entry count is asserted <= terminatorBit, i.e. the
//     loop consumes >=1 bit per produced ack/bunch).
// And that VALID input still round-trips (Encode -> Decode -> Encode is exact).
//
// IMPORTANT SEMANTIC NOTE (documented, intentional): PacketCodec::Decode is
// UE3-LENIENT. It returns Packet::ok == true for ANY datagram whose 14-bit
// PacketId can be read (i.e. non-empty AND last byte non-zero). It only returns
// ok == false for: empty input, or a last byte == 0x00 (no terminator bit), or a
// PacketId that itself overflows. So "garbage" does NOT generally yield
// ok == false - that is by design (the engine tolerates trailing/benign bits via
// AtEnd()). These tests therefore assert the SAFETY invariants above on every
// decode, and assert ok == false ONLY for the inputs the contract actually
// rejects (empty / zero-terminator). If you want a stricter reject policy, that
// is a production-code decision, not a decoder-safety bug.
//
// Framework: RS2V native test framework (tests/TestFramework.h), one executable
// per test file with its own main() (matches tests/CMakeLists.txt rs2v_add_test
// convention).
//
// Build + run (from repo root, reusing the existing VS2022 test build dir):
//   cmake --build build-tests --target PacketCodecFuzzTests --config Debug
//   build-tests/tests/Debug/PacketCodecFuzzTests.exe
// (or:  ctest --test-dir build-tests -C Debug -R PacketCodecFuzzTests -V )

#include "TestFramework.h"

#include "Network/PacketCodec.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using PacketCodec::Decode;
using PacketCodec::Encode;
using PacketCodec::Packet;
using PacketCodec::Bunch;

namespace {

// SerializeInt bounds the codec uses (mirror of PacketCodec / NetMessages).
constexpr uint32_t kMaxPacketIdV    = 16384;  // kMaxPacketId
constexpr uint32_t kMaxChannelsV    = 1023;   // kMaxChannels
constexpr uint32_t kMaxChSequenceV  = PacketCodec::kMaxChSequence; // 1024
constexpr uint32_t kChTypeMaxV      = PacketCodec::kChTypeMax;      // 8

// The set of MaxPacket phase bounds we decode/encode against. Covers the
// StatelessConnect handshake (8), the server's S2C send size (1500) and the
// established C2S/NMT size (2048) - the brief's "both S2C and C2S MaxPacket
// bounds" plus the handshake default.
const uint32_t kPhaseBounds[] = {
    PacketCodec::kHandshakeMaxPacketBytes,   // 8
    PacketCodec::kServerSendMaxPacketBytes,  // 1500 (S2C)
    PacketCodec::kNmtMaxPacketBytes,         // 2048 (C2S/NMT)
};

// ---------------------------------------------------------------------------
// Hard watchdog: catches a genuine hang (infinite loop / unbounded read) that a
// post-hoc elapsed-time check could never observe because the call never
// returns. Call WatchdogArm(label, bytes) immediately BEFORE a Decode/Encode and
// WatchdogDisarm() immediately after. A background thread aborts the process
// (printing the armed input) if the gap exceeds kCallBudget.
// ---------------------------------------------------------------------------
constexpr auto kCallBudget = std::chrono::seconds(5);

struct Watchdog {
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool armed = false;
    std::chrono::steady_clock::time_point armedAt{};
    const char* label = "";
    std::vector<uint8_t> bytes;
    uint32_t maxBytes = 0;
    std::thread th;

    void Start() {
        th = std::thread([this] {
            std::unique_lock<std::mutex> lk(m);
            while (!stop) {
                if (armed) {
                    if (cv.wait_until(lk, armedAt + kCallBudget,
                                      [this] { return stop || !armed; })) {
                        continue; // disarmed or stopping in time
                    }
                    // Timed out while still armed -> a hung call.
                    std::fprintf(stderr,
                        "\n[WATCHDOG] PacketCodec call '%s' exceeded %llds "
                        "(HANG). maxPacketBytes=%u, %zu input bytes:\n",
                        label, (long long)std::chrono::duration_cast<
                            std::chrono::seconds>(kCallBudget).count(),
                        maxBytes, bytes.size());
                    for (size_t i = 0; i < bytes.size(); ++i) {
                        std::fprintf(stderr, "%02x ", bytes[i]);
                        if ((i & 31) == 31) std::fprintf(stderr, "\n");
                    }
                    std::fprintf(stderr, "\n[WATCHDOG] aborting.\n");
                    std::fflush(stderr);
                    std::abort();
                } else {
                    cv.wait(lk, [this] { return stop || armed; });
                }
            }
        });
    }
    void Stop() {
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }
    void Arm(const char* lbl, const uint8_t* d, size_t n, uint32_t mpb) {
        std::lock_guard<std::mutex> lk(m);
        armed = true;
        armedAt = std::chrono::steady_clock::now();
        label = lbl;
        bytes.assign(d, d + n);
        maxBytes = mpb;
        cv.notify_all();
    }
    void Disarm() {
        std::lock_guard<std::mutex> lk(m);
        armed = false;
        cv.notify_all();
    }
};

Watchdog g_wd;

// Decode under the watchdog.
Packet GuardedDecode(const uint8_t* d, size_t n, uint32_t mpb) {
    g_wd.Arm("Decode", d, n, mpb);
    Packet p = Decode(d, n, mpb);
    g_wd.Disarm();
    return p;
}

std::vector<uint8_t> GuardedEncode(const Packet& p, uint32_t mpb) {
    // Cheap snapshot of the packet shape for the watchdog dump.
    g_wd.Arm("Encode", nullptr, 0, mpb);
    std::vector<uint8_t> out = Encode(p, mpb);
    g_wd.Disarm();
    return out;
}

// ---------------------------------------------------------------------------
// The core SAFETY invariant: a decoded packet must be internally consistent and
// bounded. This is what "no OOB / no unbounded alloc / no infinite loop" reduces
// to structurally once BitReader has guaranteed no literal OOB read happened.
// ---------------------------------------------------------------------------
void AssertDecodeInvariants(const Packet& p, size_t numBytes,
                            uint32_t maxPacketBytes) {
    if (!p.ok) {
        // A rejected packet must carry nothing.
        EXPECT_TRUE(p.acks.empty());
        EXPECT_TRUE(p.bunches.empty());
        return;
    }
    const uint32_t bunchDataBitsMax = maxPacketBytes * 8;
    // Total readable bits is at most numBytes*8. The parse loop consumes at
    // least one bit (the IsAck flag) per produced entry, so the number of
    // entries can never exceed the bit budget -> the loop is bounded and so is
    // allocation. (terminatorBit < numBytes*8.)
    const size_t bitBudget = numBytes * 8;
    EXPECT_LE(p.acks.size() + p.bunches.size(), bitBudget)
        << "entry count exceeds bit budget => possible unbounded loop";

    // PacketId is a 14-bit bounded int.
    EXPECT_LT(p.packetId, kMaxPacketIdV);

    for (const uint32_t ack : p.acks) {
        EXPECT_LT(ack, kMaxPacketIdV);
    }

    for (const Bunch& b : p.bunches) {
        // Every bounded field stayed within its SerializeInt range.
        EXPECT_LE(b.chIndex, kMaxChannelsV);
        if (b.bReliable) {
            EXPECT_LT(b.chSequence, kMaxChSequenceV);
        }
        if (b.bReliable || b.bOpen) {
            EXPECT_LT(b.chType, kChTypeMaxV);
        }
        // payloadBits is bounded by the phase's SerializeInt cap.
        EXPECT_LE(b.payloadBits, bunchDataBitsMax)
            << "payloadBits exceeds MaxPacket bound";
        // CRITICAL OOB invariant: the materialised payload buffer must actually
        // hold at least payloadBits bits (Decode built it from a BitWriter, so
        // it is exactly ceil(payloadBits/8) bytes). If payload.size()*8 were <
        // payloadBits, a consumer copying payloadBits bits would read past the
        // buffer.
        EXPECT_GE(b.payload.size() * 8u, b.payloadBits)
            << "payload buffer shorter than payloadBits => OOB risk";
        // And not absurdly larger than necessary (tight ceil).
        EXPECT_LT(b.payload.size() * 8u, b.payloadBits + 8u);
        // A bunch can never claim more data bits than fit in the datagram.
        EXPECT_LE(b.payloadBits, bitBudget);
    }
}

// Mask the unused high bits of the final payload byte to zero so that a
// generated payload round-trips byte-exactly (Decode reconstructs the payload
// from a zero-padded BitWriter, so trailing pad bits are always 0).
std::vector<uint8_t> MakePayload(std::mt19937& rng, uint32_t payloadBits) {
    const size_t nbytes = (payloadBits + 7) / 8;
    std::vector<uint8_t> p(nbytes);
    for (auto& byte : p) byte = static_cast<uint8_t>(rng() & 0xFF);
    if (nbytes > 0) {
        const uint32_t usedInLast = payloadBits - (uint32_t)(nbytes - 1) * 8u;
        if (usedInLast < 8) {
            const uint8_t mask = static_cast<uint8_t>((1u << usedInLast) - 1u);
            p[nbytes - 1] &= mask;
        }
    }
    return p;
}

// Build a STRUCTURALLY VALID random packet for the given phase bound. Such a
// packet must survive Encode -> Decode -> Encode with field/byte equality.
Packet MakeValidPacket(std::mt19937& rng, uint32_t maxPacketBytes) {
    const uint32_t bunchDataBitsMax = maxPacketBytes * 8;
    Packet pkt;
    pkt.packetId = rng() % kMaxPacketIdV;

    const int nAcks = rng() % 4;
    for (int i = 0; i < nAcks; ++i) {
        pkt.acks.push_back(rng() % kMaxPacketIdV);
    }

    const int nBunches = 1 + (rng() % 4);
    for (int i = 0; i < nBunches; ++i) {
        Bunch b;
        b.bControl = (rng() & 1) != 0;
        if (b.bControl) {
            b.bOpen = (rng() & 1) != 0;
            b.bClose = (rng() & 1) != 0;
        }
        b.bReliable = (rng() & 1) != 0;
        // SerializeInt(max) represents [0, max): chIndex is 0..kMaxChannelsV-1.
        b.chIndex = rng() % kMaxChannelsV;
        if (b.bReliable) b.chSequence = rng() % kMaxChSequenceV;
        if (b.bReliable || b.bOpen) b.chType = rng() % kChTypeMaxV;
        // Keep payloads small but vary across [0, 256) bits, capped by phase.
        uint32_t cap = bunchDataBitsMax > 0 ? (bunchDataBitsMax - 1) : 0;
        if (cap > 256) cap = 256;
        b.payloadBits = cap ? (rng() % (cap + 1)) : 0;
        b.payload = MakePayload(rng, b.payloadBits);
        pkt.bunches.push_back(std::move(b));
    }
    return pkt;
}

void ExpectFieldsEqual(const Packet& a, const Packet& b) {
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(a.packetId, b.packetId);
    EXPECT_EQ(a.acks, b.acks);
    ASSERT_EQ(a.bunches.size(), b.bunches.size());
    for (size_t i = 0; i < a.bunches.size(); ++i) {
        const Bunch& x = a.bunches[i];
        const Bunch& y = b.bunches[i];
        EXPECT_EQ(x.bControl, y.bControl) << "bunch " << i;
        if (x.bControl) {
            EXPECT_EQ(x.bOpen, y.bOpen) << "bunch " << i;
            EXPECT_EQ(x.bClose, y.bClose) << "bunch " << i;
        }
        EXPECT_EQ(x.bReliable, y.bReliable) << "bunch " << i;
        EXPECT_EQ(x.chIndex, y.chIndex) << "bunch " << i;
        if (x.bReliable) { EXPECT_EQ(x.chSequence, y.chSequence) << "bunch " << i; }
        if (x.bReliable || x.bOpen) { EXPECT_EQ(x.chType, y.chType) << "bunch " << i; }
        EXPECT_EQ(x.payloadBits, y.payloadBits) << "bunch " << i;
        EXPECT_EQ(x.payload, y.payload) << "bunch " << i;
    }
}

// A fixed deterministic seed so any failure is reproducible.
constexpr uint32_t kSeed = 0xC0DEFACEu;

class PacketCodecFuzz : public ::rs2v::Test {
protected:
    static void SetUpTestSuite() { g_wd.Start(); }
    static void TearDownTestSuite() { g_wd.Stop(); }
};

} // namespace

// ===========================================================================
// (a) RANDOM bytes of varied lengths, across all phase bounds.
// ===========================================================================
TEST_F(PacketCodecFuzz, RandomBytesNeverCrashOrOOB) {
    std::mt19937 rng(kSeed);
    const size_t kIters = 60000;
    for (size_t it = 0; it < kIters; ++it) {
        // Lengths from 0 to ~300 bytes, weighted toward small (where framing
        // edge cases live) but reaching past several bunches.
        size_t n;
        switch (rng() % 4) {
            case 0:  n = rng() % 4;   break;   // 0..3 (degenerate)
            case 1:  n = rng() % 16;  break;   // tiny
            case 2:  n = rng() % 64;  break;   // small
            default: n = rng() % 300; break;   // larger
        }
        std::vector<uint8_t> buf(n);
        for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFF);

        const uint32_t mpb = kPhaseBounds[rng() % 3];
        Packet p = GuardedDecode(buf.data(), buf.size(), mpb);
        AssertDecodeInvariants(p, buf.size(), mpb);
    }
}

// ===========================================================================
// (b) MUTATION fuzz: start from a VALID encoded packet, then flip bits /
//     truncate / extend / overwrite bytes, and decode the corruption.
// ===========================================================================
TEST_F(PacketCodecFuzz, MutatedValidPacketsNeverCrashOrOOB) {
    std::mt19937 rng(kSeed ^ 0x1111u);
    const size_t kIters = 40000;
    for (size_t it = 0; it < kIters; ++it) {
        const uint32_t mpb = kPhaseBounds[rng() % 3];
        Packet seed = MakeValidPacket(rng, mpb);
        std::vector<uint8_t> wire = GuardedEncode(seed, mpb);
        if (wire.empty()) continue;

        // Apply 1..5 random mutations.
        const int nMut = 1 + (rng() % 5);
        for (int m = 0; m < nMut; ++m) {
            switch (rng() % 5) {
                case 0: { // flip a random bit
                    if (wire.empty()) break;
                    size_t bit = rng() % (wire.size() * 8);
                    wire[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
                    break;
                }
                case 1: { // overwrite a random byte
                    if (wire.empty()) break;
                    wire[rng() % wire.size()] = static_cast<uint8_t>(rng() & 0xFF);
                    break;
                }
                case 2: { // truncate
                    if (wire.size() > 1) wire.resize(rng() % wire.size());
                    break;
                }
                case 3: { // extend with junk
                    size_t add = 1 + (rng() % 8);
                    for (size_t k = 0; k < add; ++k)
                        wire.push_back(static_cast<uint8_t>(rng() & 0xFF));
                    break;
                }
                case 4: { // zero the last byte (kills the terminator)
                    if (!wire.empty()) wire.back() = 0x00;
                    break;
                }
            }
        }

        // Decode under a possibly-different phase bound too.
        const uint32_t decMpb = kPhaseBounds[rng() % 3];
        Packet p = GuardedDecode(wire.data(), wire.size(), decMpb);
        AssertDecodeInvariants(p, wire.size(), decMpb);
    }
}

// ===========================================================================
// (c) BOUNDARY values: 0-length, 1-byte, all-0x00, all-0xFF, alternating,
//     single high-bit, max-ish lengths.
// ===========================================================================
TEST_F(PacketCodecFuzz, BoundaryBuffers) {
    auto run = [&](const std::vector<uint8_t>& buf) {
        for (uint32_t mpb : kPhaseBounds) {
            Packet p = GuardedDecode(buf.data(), buf.size(), mpb);
            AssertDecodeInvariants(p, buf.size(), mpb);
        }
    };

    run({});                                  // 0-length -> must be ok=false
    EXPECT_FALSE(Decode(nullptr, 0, 8).ok);

    for (int v = 0; v < 256; ++v) {
        run({static_cast<uint8_t>(v)});       // every possible single byte
    }

    for (size_t len : {1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 63u, 64u, 255u, 256u,
                       1024u, 4096u}) {
        run(std::vector<uint8_t>(len, 0x00)); // all zero -> ok=false (no term)
        run(std::vector<uint8_t>(len, 0xFF)); // all ones
        std::vector<uint8_t> alt(len);
        for (size_t i = 0; i < len; ++i) alt[i] = (i & 1) ? 0xAA : 0x55;
        run(alt);                             // alternating
        std::vector<uint8_t> hi(len, 0x00);
        hi.back() = 0x80;                     // only the terminator-ish high bit
        run(hi);
    }

    // Explicit contract checks for the inputs Decode DOES reject.
    EXPECT_FALSE(Decode(std::vector<uint8_t>(8, 0x00).data(), 8, 8).ok);
    EXPECT_FALSE(Decode(std::vector<uint8_t>(1, 0x00).data(), 1, 8).ok);
}

// ===========================================================================
// (d) VALID round-trip: Encode -> Decode -> Encode is field- and byte-exact,
//     across every phase bound. This proves the hardening didn't break the
//     happy path.
// ===========================================================================
TEST_F(PacketCodecFuzz, ValidPacketsRoundTrip) {
    std::mt19937 rng(kSeed ^ 0x2222u);
    const size_t kIters = 20000;
    for (size_t it = 0; it < kIters; ++it) {
        const uint32_t mpb = kPhaseBounds[rng() % 3];
        Packet seed = MakeValidPacket(rng, mpb);

        std::vector<uint8_t> wire = GuardedEncode(seed, mpb);
        ASSERT_FALSE(wire.empty());

        Packet decoded = GuardedDecode(wire.data(), wire.size(), mpb);
        ASSERT_TRUE(decoded.ok) << "iter " << it << " mpb " << mpb;
        AssertDecodeInvariants(decoded, wire.size(), mpb);
        ExpectFieldsEqual(seed, decoded);

        // Re-encoding the decoded packet reproduces the same wire bytes.
        std::vector<uint8_t> reencoded = GuardedEncode(decoded, mpb);
        EXPECT_EQ(reencoded, wire) << "iter " << it << " mpb " << mpb;
    }
}

// ===========================================================================
// (e) PER-BUNCH PAYLOAD decode fuzz: drive the bunch-payload path directly.
//   (e1) random payloadBits + random payload bytes encode->decode without
//        crashing and the payload bit count is preserved (when consistent).
//   (e2) INCONSISTENT bunch (payloadBits >> actual payload bytes, e.g. claims
//        thousands of bits with an empty buffer) must not crash Encode or the
//        subsequent Decode (Encode's BitReader clamps; Decode's BitReader
//        guards).
// ===========================================================================
TEST_F(PacketCodecFuzz, PerBunchPayloadFuzz) {
    std::mt19937 rng(kSeed ^ 0x3333u);
    const size_t kIters = 30000;
    for (size_t it = 0; it < kIters; ++it) {
        const uint32_t mpb = kPhaseBounds[rng() % 3];
        const uint32_t bunchDataBitsMax = mpb * 8;

        Packet pkt;
        pkt.packetId = rng() % kMaxPacketIdV;

        Bunch b;
        b.bControl = (rng() & 1) != 0;
        if (b.bControl) { b.bOpen = (rng() & 1); b.bClose = (rng() & 1); }
        b.bReliable = (rng() & 1) != 0;
        b.chIndex = rng() % kMaxChannelsV; // [0, kMaxChannelsV)
        if (b.bReliable) b.chSequence = rng() % kMaxChSequenceV;
        if (b.bReliable || b.bOpen) b.chType = rng() % kChTypeMaxV;

        const bool consistent = (rng() & 1) != 0;
        if (consistent) {
            uint32_t cap = bunchDataBitsMax ? (bunchDataBitsMax - 1) : 0;
            if (cap > 512) cap = 512;
            b.payloadBits = cap ? (rng() % (cap + 1)) : 0;
            b.payload = MakePayload(rng, b.payloadBits);
        } else {
            // Deliberately inconsistent: claim a huge bit count with a short
            // (often empty) buffer. Exercises the clamp/guard paths.
            b.payloadBits = rng() % (bunchDataBitsMax + 16); // may EXCEED the bound
            const size_t haveBytes = rng() % 4;              // 0..3 bytes only
            b.payload.assign(haveBytes, static_cast<uint8_t>(rng() & 0xFF));
        }
        pkt.bunches.push_back(b);

        // Encode must not crash even on the inconsistent bunch.
        std::vector<uint8_t> wire = GuardedEncode(pkt, mpb);
        // Decode the result must not crash / OOB.
        Packet decoded = GuardedDecode(wire.data(), wire.size(), mpb);
        AssertDecodeInvariants(decoded, wire.size(), mpb);

        // For the consistent case with payloadBits within bound, the round-trip
        // must preserve the bunch payload exactly.
        if (consistent && b.payloadBits < bunchDataBitsMax) {
            ASSERT_TRUE(decoded.ok);
            ASSERT_EQ(decoded.bunches.size(), 1u);
            EXPECT_EQ(decoded.bunches[0].payloadBits, b.payloadBits);
            EXPECT_EQ(decoded.bunches[0].payload, b.payload);
        }
    }
}

// ===========================================================================
// (f) Decode is IDEMPOTENT under re-decode of its own canonical output: decode
//     random bytes, re-encode whatever came out, decode again -> identical
//     structure. Catches any non-determinism / state bleed in the parser.
// ===========================================================================
TEST_F(PacketCodecFuzz, DecodeReencodeDecodeStable) {
    std::mt19937 rng(kSeed ^ 0x4444u);
    const size_t kIters = 20000;
    for (size_t it = 0; it < kIters; ++it) {
        const uint32_t mpb = kPhaseBounds[rng() % 3];
        size_t n = rng() % 200;
        std::vector<uint8_t> buf(n);
        for (auto& x : buf) x = static_cast<uint8_t>(rng() & 0xFF);

        Packet p1 = GuardedDecode(buf.data(), buf.size(), mpb);
        if (!p1.ok) continue;
        // Only round-trip when the decode is internally encodable (every bunch's
        // payload buffer holds its bits - guaranteed by Decode's construction).
        std::vector<uint8_t> w = GuardedEncode(p1, mpb);
        Packet p2 = GuardedDecode(w.data(), w.size(), mpb);
        ASSERT_TRUE(p2.ok);
        std::vector<uint8_t> w2 = GuardedEncode(p2, mpb);
        EXPECT_EQ(w, w2) << "Encode not idempotent at iter " << it;
        ExpectFieldsEqual(p1, p2);
    }
}

RS2V_TEST_MAIN()
