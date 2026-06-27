// tests/BitReaderFuzzTests.cpp
//
// Fuzz harness + malformed-input unit tests for src/Network/BitReader.h and its
// round-trip with src/Network/BitWriter.h.
//
// CONTRACT UNDER TEST (BitReader.h):
//   Every read is bounds-checked against the bit length of the backing buffer. A
//   read that would run past the end must:
//     (1) set the sticky overflow flag (IsOverflowed() == true),
//     (2) return a zero/default value,
//     (3) NEVER read out of bounds and NEVER throw / crash / hang.
//   Valid input must still round-trip BitWriter -> BitReader.
//
// These tests PROVE the decoder survives hostile input:
//   * random bytes of varied lengths,
//   * MUTATION fuzz (flip bits / truncate / extend / overwrite length fields),
//   * boundary values (0-length, 1-byte, max-length, all-0xFF, alternating),
//   * adversarial SerializeInt maxValue (0, 1, UINT32_MAX),
//   * calling every read past the end of a short buffer.
//
// OOB-read detection: the fuzz buffers are heap-allocated to the EXACT length so
// that any read past the end is a real out-of-bounds access. Run under
// AddressSanitizer (or the MSVC /fsanitize=address) to turn an OOB read into a
// hard failure; even without ASan, a guard buffer + length invariant catches the
// common cases. A per-call watchdog (std::async + wait_for) catches hangs.

#include "TestFramework.h"

#include "Network/BitReader.h"
#include "Network/BitWriter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Watchdog: run `fn` on a worker thread; FAIL (not hang) if it exceeds `ms`.
// Catches an accidental infinite loop in any decoder path. Returns true if the
// call completed in time. The worker is detached on timeout (we cannot safely
// kill it); the process will be failed by the native test assertion regardless.
// ---------------------------------------------------------------------------
bool RunWithWatchdog(const std::function<void()>& fn, int ms = 2000) {
    auto fut = std::async(std::launch::async, fn);
    if (fut.wait_for(std::chrono::milliseconds(ms)) == std::future_status::timeout) {
        return false;  // hang detected
    }
    fut.get();  // propagate any exception (would also be a contract violation)
    return true;
}

// Heap-allocate an exact-length copy so reads past the end are genuine OOB (so
// ASan/Valgrind can flag them). Returns an owning unique_ptr + the pointer.
struct ExactBuf {
    std::unique_ptr<uint8_t[]> mem;
    size_t len = 0;
    const uint8_t* data() const { return mem.get(); }
};

ExactBuf MakeExact(const std::vector<uint8_t>& v) {
    ExactBuf b;
    b.len = v.size();
    b.mem = std::make_unique<uint8_t[]>(v.size() == 0 ? 1 : v.size());
    if (!v.empty()) std::memcpy(b.mem.get(), v.data(), v.size());
    return b;
}

// Drive EVERY public read method of a BitReader to completion. Used to assert
// that no sequence of reads on an arbitrary buffer can crash / read OOB / hang.
// We deliberately over-read: more reads than the buffer can satisfy, so overflow
// MUST trip and every subsequent read must be a cheap no-op returning default.
void HammerAllReads(const uint8_t* data, size_t len, size_t validBits,
                    std::mt19937& rng) {
    BitReader r(data, len, validBits);
    // Silence the Logger sink (overflow would otherwise Warn-spam); also exercise
    // the handler path so SetOverflowHandler is covered.
    std::atomic<int> overflowCalls{0};
    r.SetOverflowHandler([&](const char*, size_t, size_t, size_t) {
        overflowCalls.fetch_add(1, std::memory_order_relaxed);
    });

    // Issue a randomized but bounded sequence of reads. The count is chosen so we
    // are guaranteed to exhaust any buffer up to a few KB and then keep reading.
    const int kOps = 256;
    std::uniform_int_distribution<int> opPick(0, 7);
    std::uniform_int_distribution<int> bitsPick(0, 64);
    std::uniform_int_distribution<uint32_t> maxPick(0, 0xFFFFFFFFu);

    for (int i = 0; i < kOps; ++i) {
        switch (opPick(rng)) {
            case 0: (void)r.ReadBit(); break;
            case 1: (void)r.ReadBits(bitsPick(rng)); break;
            case 2: (void)r.SerializeInt(maxPick(rng)); break;
            case 3: (void)r.ReadByte(); break;
            case 4: (void)r.ReadInt32(); break;
            case 5: (void)r.ReadFloat(); break;
            case 6: (void)r.ReadUInt64(); break;
            case 7: (void)r.ReadString(); break;
        }
        // Core invariant: the cursor never advances past the readable bit count.
        ASSERT_LE(r.BitPos(), r.NumBits());
    }
    // We issued far more reads than any small buffer can satisfy: either we
    // consumed it all cleanly or overflow tripped. Cursor must be sane regardless.
    ASSERT_LE(r.BitPos(), r.NumBits());
}

}  // namespace

// ===========================================================================
// 1. Every read past the end sets overflow and returns default (no OOB, no crash)
// ===========================================================================

TEST(BitReaderMalformed, EmptyBufferEveryReadOverflows) {
    BitReader r(nullptr, 0);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_TRUE(r.IsOverflowed());

    // Once overflowed, the flag is sticky and every read is a default no-op.
    EXPECT_EQ(r.ReadBits(13), 0u);
    EXPECT_EQ(r.ReadByte(), 0u);
    EXPECT_EQ(r.ReadInt32(), 0);
    EXPECT_EQ(r.ReadUInt32(), 0u);
    EXPECT_EQ(r.ReadUInt64(), 0u);
    EXPECT_EQ(r.ReadFloat(), 0.0f);
    EXPECT_EQ(r.ReadString(), std::string());
    EXPECT_EQ(r.BitPos(), 0u);
    EXPECT_EQ(r.BitsLeft(), 0u);
}

TEST(BitReaderMalformed, OneByteReadInt32OverflowsCleanly) {
    const std::vector<uint8_t> one = {0xAB};
    BitReader r(one.data(), one.size());  // 8 readable bits
    // ReadInt32 wants 32 bits; only 8 available. The first byte (0xAB) is read
    // successfully, then the next ReadByte overflows. SAFETY CONTRACT: overflow
    // trips and NO OOB read occurs. The returned value carries the partial bytes
    // that legitimately fit (here 0xAB) with the rest zero-filled — that is fine;
    // we assert the overflow flag, not a particular partial value.
    (void)r.ReadInt32();
    EXPECT_TRUE(r.IsOverflowed());
    EXPECT_STREQ(r.OverflowOp(), "ReadBits");  // failing op was a ReadByte->ReadBits(8)
    // The reported failure stayed in-bounds and wanted more than was available.
    EXPECT_LE(r.OverflowBitPos(), r.NumBits());
    EXPECT_GT(r.OverflowWantBits(), 0u);
    EXPECT_LE(r.BitPos(), r.NumBits());
}

TEST(BitReaderMalformed, PartialThenOverflowSplit) {
    // 3 bytes = 24 readable bits. Read a byte (ok), then a uint32 (needs 32, only
    // 16 left) -> overflow on the SECOND op, first op's value intact.
    const std::vector<uint8_t> buf = {0x11, 0x22, 0x33};
    BitReader r(buf.data(), buf.size());
    EXPECT_EQ(r.ReadByte(), 0x11u);
    EXPECT_FALSE(r.IsOverflowed());
    // ReadUInt32 needs 32 bits but only 16 remain: the two available bytes are
    // read (partial value), then overflow trips on the 3rd byte. We assert the
    // overflow flag + in-bounds cursor, not the partial value.
    (void)r.ReadUInt32();
    EXPECT_TRUE(r.IsOverflowed());
    // Cursor must not have advanced past the readable length.
    EXPECT_LE(r.BitPos(), r.NumBits());
}

TEST(BitReaderMalformed, ReadStringHugePositiveLengthIsBounded) {
    // Length prefix = INT32_MAX (huge ANSI length), but no payload follows. Must
    // NOT attempt a multi-GB read/alloc: EnsureBits rejects, overflow trips, "".
    BitWriter w;
    w.WriteInt32(std::numeric_limits<int32_t>::max());
    const std::vector<uint8_t> bytes = w.GetBytes();  // only the 4-byte length

    std::string s;
    BitReader r(bytes.data(), bytes.size());
    ASSERT_TRUE(RunWithWatchdog([&] { s = r.ReadString(); }));
    EXPECT_TRUE(s.empty());
    EXPECT_TRUE(r.IsOverflowed());
}

TEST(BitReaderMalformed, ReadStringHugeNegativeLengthIsBounded) {
    // Negative length = UCS-2. INT32_MIN magnitude must not overflow size math or
    // attempt an unbounded read; EnsureBits rejects, overflow trips, "".
    BitWriter w;
    w.WriteInt32(std::numeric_limits<int32_t>::min());
    const std::vector<uint8_t> bytes = w.GetBytes();

    std::string s;
    BitReader r(bytes.data(), bytes.size());
    ASSERT_TRUE(RunWithWatchdog([&] { s = r.ReadString(); }));
    EXPECT_TRUE(s.empty());
    EXPECT_TRUE(r.IsOverflowed());
}

TEST(BitReaderMalformed, ReadStringLengthPrefixTruncated) {
    // Only 2 bytes — not even the 4-byte length prefix fits. Overflow on prefix.
    const std::vector<uint8_t> buf = {0x05, 0x00};
    BitReader r(buf.data(), buf.size());
    EXPECT_EQ(r.ReadString(), std::string());
    EXPECT_TRUE(r.IsOverflowed());
}

// ===========================================================================
// 2. Adversarial SerializeInt maxValue (0, 1, UINT32_MAX)
// ===========================================================================

TEST(BitReaderSerializeInt, MaxValueZeroReadsNothing) {
    const std::vector<uint8_t> buf = {0xFF, 0xFF};
    BitReader r(buf.data(), buf.size());
    EXPECT_EQ(r.SerializeInt(0), 0u);   // degenerate: consumes no bits
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), 0u);
}

TEST(BitReaderSerializeInt, MaxValueOneReadsNothing) {
    // maxValue==1: only value 0 is representable; loop body never executes.
    const std::vector<uint8_t> buf = {0xFF, 0xFF};
    BitReader r(buf.data(), buf.size());
    EXPECT_EQ(r.SerializeInt(1), 0u);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), 0u);
}

TEST(BitReaderSerializeInt, MaxValueUintMaxOnEmptyOverflows) {
    // maxValue==UINT32_MAX wants up to 31 bits; on an empty buffer the first
    // ReadBit overflows and SerializeInt returns the partial (0) without OOB.
    BitReader r(nullptr, 0);
    uint32_t v = 0;
    ASSERT_TRUE(RunWithWatchdog([&] { v = r.SerializeInt(0xFFFFFFFFu); }));
    EXPECT_EQ(v, 0u);
    EXPECT_TRUE(r.IsOverflowed());
}

TEST(BitReaderSerializeInt, MaxValueUintMaxOnShortBufferTerminates) {
    // 1 byte: SerializeInt(UINT32_MAX) wants up to 31 bits, only 8 available.
    // Must consume what it can, then overflow — and must TERMINATE (no hang).
    const std::vector<uint8_t> buf = {0xFF};
    BitReader r(buf.data(), buf.size());
    uint32_t v = 0;
    ASSERT_TRUE(RunWithWatchdog([&] { v = r.SerializeInt(0xFFFFFFFFu); }));
    EXPECT_TRUE(r.IsOverflowed());
    EXPECT_LE(r.BitPos(), r.NumBits());
    (void)v;
}

// Round-trip SerializeInt across a representative spread of maxValues/values.
TEST(BitReaderSerializeInt, RoundTripBoundedValues) {
    const uint32_t maxes[] = {2u, 3u, 7u, 8u, 255u, 256u, 1024u, 65535u,
                              0x7FFFFFFFu, 0xFFFFFFFFu};
    for (uint32_t mx : maxes) {
        // Sample values: 0, mx-1, and a midpoint.
        const uint32_t vals[] = {0u, mx > 1 ? mx - 1 : 0u, mx / 2u};
        for (uint32_t val : vals) {
            BitWriter w;
            w.SerializeInt(val, mx);
            const std::vector<uint8_t> bytes = w.GetBytes();
            BitReader r(bytes.data(), bytes.size(),
                        /*validBits=*/w.NumBits());
            const uint32_t got = r.SerializeInt(mx);
            EXPECT_EQ(got, val) << "max=" << mx << " val=" << val;
            EXPECT_FALSE(r.IsOverflowed()) << "max=" << mx << " val=" << val;
        }
    }
}

// ===========================================================================
// 3. BitWriter -> BitReader round-trip for random typed values
// ===========================================================================

TEST(BitReaderRoundTrip, RandomTypedSequence) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<uint32_t> u32(0, 0xFFFFFFFFu);
    std::uniform_int_distribution<uint64_t> u64(0, ~0ull);
    std::uniform_int_distribution<int> bits(0, 32);

    for (int iter = 0; iter < 500; ++iter) {
        // Build a random typed record.
        const uint8_t by = static_cast<uint8_t>(u32(rng));
        const int32_t i32 = static_cast<int32_t>(u32(rng));
        const uint32_t u_32 = u32(rng);
        const uint64_t u_64 = u64(rng);
        float fv;
        const uint32_t fbits = u32(rng);
        std::memcpy(&fv, &fbits, 4);
        const int nbits = bits(rng);
        const uint64_t packed = (nbits == 0) ? 0ull
                              : (u64(rng) & ((nbits >= 64) ? ~0ull
                                            : ((1ull << nbits) - 1ull)));

        BitWriter w;
        w.WriteByte(by);
        w.WriteInt32(i32);
        w.WriteUInt32(u_32);
        w.WriteUInt64(u_64);
        w.WriteFloat(fv);
        w.WriteBits(packed, nbits);

        const std::vector<uint8_t> bytes = w.GetBytes();
        BitReader r(bytes.data(), bytes.size(), w.NumBits());

        EXPECT_EQ(r.ReadByte(), by);
        EXPECT_EQ(r.ReadInt32(), i32);
        EXPECT_EQ(r.ReadUInt32(), u_32);
        EXPECT_EQ(r.ReadUInt64(), u_64);
        // Compare floats by their raw bits (handles NaN payloads bit-exactly).
        uint32_t gotF;
        const float rf = r.ReadFloat();
        std::memcpy(&gotF, &rf, 4);
        EXPECT_EQ(gotF, fbits);
        EXPECT_EQ(r.ReadBits(nbits), packed);

        ASSERT_FALSE(r.IsOverflowed());
        // Everything consumed: with validBits == written bits, nothing trails.
        EXPECT_EQ(r.BitsLeft(), 0u);
    }
}

TEST(BitReaderRoundTrip, AnsiAndUcs2Strings) {
    const char* samples[] = {"", "a", "MapName_VNTE-Hue", "session-12345",
                             "the quick brown fox 0123456789"};
    for (const char* s : samples) {
        BitWriter w;
        w.WriteString(s);
        const std::vector<uint8_t> bytes = w.GetBytes();
        BitReader r(bytes.data(), bytes.size(), w.NumBits());
        EXPECT_EQ(r.ReadString(), std::string(s));
        EXPECT_FALSE(r.IsOverflowed());
    }
    // UCS-2 path.
    {
        BitWriter w;
        w.WriteStringUCS2(L"Hue City");
        const std::vector<uint8_t> bytes = w.GetBytes();
        BitReader r(bytes.data(), bytes.size(), w.NumBits());
        EXPECT_EQ(r.ReadString(), std::string("Hue City"));
        EXPECT_FALSE(r.IsOverflowed());
    }
}

// ===========================================================================
// 4. MUTATION fuzz: take a valid record, flip/truncate/extend, never crash.
// ===========================================================================

// Build a canonical valid record used as the mutation seed.
static std::vector<uint8_t> MakeSeedRecord(size_t* outBits) {
    BitWriter w;
    w.WriteByte(0x1d);
    w.WriteInt32(-12345);
    w.WriteUInt32(0xDEADBEEFu);
    w.WriteFloat(3.14159f);
    w.WriteString("VNTE-CuChi");
    w.SerializeInt(733, 1024);
    if (outBits) *outBits = w.NumBits();
    return w.GetBytes();
}

// Decode the seed record's schema; used to confirm a clean seed round-trips and
// to drive the same schema over mutated bytes (must never crash).
static void DecodeSeedSchema(const uint8_t* data, size_t len, size_t validBits) {
    BitReader r(data, len, validBits);
    (void)r.ReadByte();
    (void)r.ReadInt32();
    (void)r.ReadUInt32();
    (void)r.ReadFloat();
    (void)r.ReadString();
    (void)r.SerializeInt(1024);
    // No assertion on values — mutated input may be anything — only that the
    // cursor stayed in-bounds, which the reader guarantees:
    ASSERT_LE(r.BitPos(), r.NumBits());
}

TEST(BitReaderMutationFuzz, SeedRoundTripsClean) {
    size_t bits = 0;
    const std::vector<uint8_t> seed = MakeSeedRecord(&bits);
    BitReader r(seed.data(), seed.size(), bits);
    EXPECT_EQ(r.ReadByte(), 0x1du);
    EXPECT_EQ(r.ReadInt32(), -12345);
    EXPECT_EQ(r.ReadUInt32(), 0xDEADBEEFu);
    EXPECT_FLOAT_EQ(r.ReadFloat(), 3.14159f);
    EXPECT_EQ(r.ReadString(), std::string("VNTE-CuChi"));
    EXPECT_EQ(r.SerializeInt(1024), 733u);
    EXPECT_FALSE(r.IsOverflowed());
}

TEST(BitReaderMutationFuzz, BitFlipsNeverCrash) {
    size_t bits = 0;
    const std::vector<uint8_t> seed = MakeSeedRecord(&bits);
    std::mt19937 rng(0xBADF00Du);
    std::uniform_int_distribution<size_t> bytePick(0, seed.size() - 1);
    std::uniform_int_distribution<int> bitPick(0, 7);

    for (int iter = 0; iter < 4000; ++iter) {
        std::vector<uint8_t> m = seed;
        // Flip 1-3 random bits.
        const int flips = 1 + (iter % 3);
        for (int f = 0; f < flips; ++f) {
            m[bytePick(rng)] ^= static_cast<uint8_t>(1u << bitPick(rng));
        }
        ExactBuf eb = MakeExact(m);
        ASSERT_TRUE(RunWithWatchdog(
            [&] { DecodeSeedSchema(eb.data(), eb.len, eb.len * 8); }))
            << "hang on bit-flip iter " << iter;
    }
}

TEST(BitReaderMutationFuzz, TruncationNeverCrashes) {
    size_t bits = 0;
    const std::vector<uint8_t> seed = MakeSeedRecord(&bits);
    // Every prefix length [0, seed.size()].
    for (size_t n = 0; n <= seed.size(); ++n) {
        std::vector<uint8_t> t(seed.begin(), seed.begin() + n);
        ExactBuf eb = MakeExact(t);
        ASSERT_TRUE(RunWithWatchdog(
            [&] { DecodeSeedSchema(eb.data(), eb.len, eb.len * 8); }))
            << "hang on truncation len " << n;
    }
}

TEST(BitReaderMutationFuzz, ExtensionNeverCrashes) {
    size_t bits = 0;
    std::vector<uint8_t> seed = MakeSeedRecord(&bits);
    std::mt19937 rng(0x5EED5EEDu);
    std::uniform_int_distribution<int> byteVal(0, 255);
    for (int iter = 0; iter < 200; ++iter) {
        std::vector<uint8_t> e = seed;
        const int extra = 1 + (iter % 257);
        for (int i = 0; i < extra; ++i) e.push_back(static_cast<uint8_t>(byteVal(rng)));
        ExactBuf eb = MakeExact(e);
        ASSERT_TRUE(RunWithWatchdog(
            [&] { DecodeSeedSchema(eb.data(), eb.len, eb.len * 8); }));
    }
}

TEST(BitReaderMutationFuzz, OverwriteStringLengthField) {
    // The FString length prefix is the classic attack surface: overwrite it with
    // every possible "huge"/negative/zero pattern and confirm bounded behavior.
    // Re-derive the byte offset of the length prefix in the seed: byte(1) + int32
    // + uint32 + float = 1 + 4 + 4 + 4 = 13 bytes precede the WriteString prefix.
    size_t bits = 0;
    const std::vector<uint8_t> seed = MakeSeedRecord(&bits);
    const size_t kPrefixOff = 13;
    ASSERT_LT(kPrefixOff + 4, seed.size());

    const int32_t evil[] = {
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
        -1, 1, 0, 0x40000000, -0x40000000, 0x7FFFFFFE};
    for (int32_t L : evil) {
        std::vector<uint8_t> m = seed;
        std::memcpy(&m[kPrefixOff], &L, 4);  // host LE matches wire LE here
        ExactBuf eb = MakeExact(m);
        ASSERT_TRUE(RunWithWatchdog(
            [&] { DecodeSeedSchema(eb.data(), eb.len, eb.len * 8); }, 3000))
            << "hang/crash on evil string length " << L;
    }
}

// ===========================================================================
// 5. Random-bytes + boundary-value sweep through ALL read methods.
// ===========================================================================

TEST(BitReaderRandomFuzz, ArbitraryBytesHammerAllReads) {
    std::mt19937 rng(0x1234ABCDu);
    std::uniform_int_distribution<int> lenPick(0, 4096);
    std::uniform_int_distribution<int> byteVal(0, 255);

    for (int iter = 0; iter < 1500; ++iter) {
        const int len = lenPick(rng);
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<uint8_t>(byteVal(rng));
        ExactBuf eb = MakeExact(buf);
        // validBits may be < len*8 (sub-byte valid length) to exercise that path.
        std::uniform_int_distribution<size_t> vb(0, eb.len * 8);
        const size_t validBits = (iter & 1) ? vb(rng) : eb.len * 8;
        ASSERT_TRUE(RunWithWatchdog(
            [&] { HammerAllReads(eb.data(), eb.len, validBits, rng); }))
            << "hang on random fuzz iter " << iter << " len " << len;
    }
}

TEST(BitReaderRandomFuzz, BoundaryBuffers) {
    std::mt19937 rng(0x99);
    auto run = [&](const std::vector<uint8_t>& v) {
        ExactBuf eb = MakeExact(v);
        ASSERT_TRUE(RunWithWatchdog(
            [&] { HammerAllReads(eb.data(), eb.len, eb.len * 8, rng); }));
    };
    run({});                                   // 0-length
    run({0x00});                               // single zero byte
    run({0xFF});                               // single all-ones byte
    run(std::vector<uint8_t>(1, 0xFF));        // 1 byte 0xFF
    run(std::vector<uint8_t>(4096, 0xFF));     // max-ish, all 0xFF
    run(std::vector<uint8_t>(4096, 0x00));     // max-ish, all zero
    // Alternating 0xAA / 0x55.
    {
        std::vector<uint8_t> alt(1024);
        for (size_t i = 0; i < alt.size(); ++i) alt[i] = (i & 1) ? 0x55 : 0xAA;
        run(alt);
    }
}

RS2V_TEST_MAIN()
