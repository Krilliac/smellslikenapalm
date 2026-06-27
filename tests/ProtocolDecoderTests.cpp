// tests/ProtocolDecoderTests.cpp
//
// Self-contained tests for the self-reverse-engineering subsystem:
//   * NetFieldTable / NetFieldRegistry  (handle-table parsing)
//   * BunchPropertyDecoder              (bit-packed property decode vs codec spec)
//
// This file is fully self-contained: it defines its own main() + CHECK macros
// and links only rs2v_core, so it builds and runs with no network and no test
// framework at all. Wire-up: -DBUILD_RE_SELFTEST=ON (see root CMakeLists.txt).
//
// (The rest of the suite now uses the RS2V native test framework in
// tests/TestFramework.h, which is also dependency-free and offline — the
// previous GoogleTest-via-FetchContent setup has been removed.)

#include "Protocol/ReverseEngineering/NetFieldTable.h"
#include "Protocol/ReverseEngineering/BunchPropertyDecoder.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include "Network/BitWriter.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do {                                                        \
    ++g_checks;                                                                 \
    if (!(cond)) {                                                              \
        ++g_failures;                                                          \
        std::printf("  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                           \
} while (0)

#define CHECK_EQ(a, b) do {                                                     \
    ++g_checks;                                                                 \
    auto _va = (a); auto _vb = (b);                                             \
    if (!(_va == _vb)) {                                                        \
        ++g_failures;                                                          \
        std::ostringstream _os; _os << _va << " != " << _vb;                    \
        std::printf("  FAIL [%s:%d]: %s == %s  (%s)\n",                         \
                    __FILE__, __LINE__, #a, #b, _os.str().c_str());             \
    }                                                                           \
} while (0)

#include <sstream>

// Locate the repo's data dir whether run from repo root or build tree.
static std::string NetfieldsDir() {
    const char* candidates[] = {
        "data/re/netfields",
        "../data/re/netfields",
        "../../data/re/netfields",
    };
    for (const char* c : candidates) {
        std::string p = std::string(c) + "/netfields_u_ROTeamInfo.txt";
        if (FILE* f = std::fopen(p.c_str(), "r")) { std::fclose(f); return c; }
    }
    return "data/re/netfields";
}

// ---------------------------------------------------------------------------

static void TestClassifyType() {
    std::printf("TestClassifyType\n");
    using NV = NetValueType;
    CHECK(NetFieldTable::ClassifyType("float", NetPropKind::Prop) == NV::Float);
    CHECK(NetFieldTable::ClassifyType("bool", NetPropKind::Prop) == NV::Bool);
    CHECK(NetFieldTable::ClassifyType("int", NetPropKind::Prop) == NV::Int);
    CHECK(NetFieldTable::ClassifyType("byte", NetPropKind::Prop) == NV::Byte);
    CHECK(NetFieldTable::ClassifyType("byte(enum ENetRole)", NetPropKind::Prop) == NV::EnumByte);
    CHECK(NetFieldTable::ClassifyType("struct Vector", NetPropKind::Prop) == NV::StructVector);
    CHECK(NetFieldTable::ClassifyType("struct Rotator", NetPropKind::Prop) == NV::StructRotator);
    CHECK(NetFieldTable::ClassifyType("obj<Pawn>", NetPropKind::Prop) == NV::Object);
    CHECK(NetFieldTable::ClassifyType("array<int>", NetPropKind::Prop) == NV::DynArray);
    CHECK(NetFieldTable::ClassifyType("(rpc)", NetPropKind::Func) == NV::Unknown);
    CHECK(NetFieldTable::ClassifyType("", NetPropKind::Prop) == NV::Unknown);
}

static void TestTableParse(const std::string& dir) {
    std::printf("TestTableParse\n");
    NetFieldTable t;
    bool ok = t.LoadFromFile(dir + "/netfields_u_ROTeamInfo.txt", "ROTeamInfo");
    CHECK(ok);
    CHECK_EQ(t.ClassName(), std::string("ROTeamInfo"));
    CHECK_EQ(t.MaxIndex(), 78u);
    CHECK(t.HasValueTypes());

    const NetField* role = t.GetField(8);
    CHECK(role != nullptr);
    if (role) {
        CHECK_EQ(role->name, std::string("Role"));
        CHECK(role->valueType == NetValueType::EnumByte);
        CHECK(role->kind == NetPropKind::Prop);
    }
    const NetField* draw = t.GetField(11);
    CHECK(draw && draw->name == "DrawScale" && draw->valueType == NetValueType::Float);
    const NetField* loc = t.GetField(13);
    CHECK(loc && loc->name == "Location" && loc->valueType == NetValueType::StructVector);
    const NetField* owner = t.GetField(14);
    CHECK(owner && owner->name == "bNetOwner" && owner->valueType == NetValueType::Bool);
    const NetField* fn = t.GetField(0);
    CHECK(fn && fn->kind == NetPropKind::Func);  // DemoRecordSound (rpc)

    // A handle past the class range must not resolve.
    CHECK(t.GetField(9999) == nullptr);
}

static void TestLeanTableParse(const std::string& dir) {
    std::printf("TestLeanTableParse\n");
    NetFieldTable t;
    bool ok = t.LoadFromFile(dir + "/netfields_u_ROPawn.txt", "ROPawn");
    CHECK(ok);
    CHECK_EQ(t.ClassName(), std::string("ROPawn"));
    // Lean file: names resolve, but no value types and maxIndex derived from data.
    CHECK(!t.HasValueTypes());
    CHECK(t.MaxIndex() > 100u);
    const NetField* v = t.GetField(3);
    CHECK(v && v->name == "Velocity");
    CHECK(v && v->valueType == NetValueType::Unknown);
}

static void TestRegistry(const std::string& dir) {
    std::printf("TestRegistry\n");
    NetFieldRegistry reg;
    size_t n = reg.LoadDirectory(dir);
    CHECK(n >= 3);
    CHECK(reg.GetClass("ROTeamInfo") != nullptr);
    CHECK(reg.GetClass("ROPlayerController") != nullptr);
    CHECK(reg.GetClass("DoesNotExist") == nullptr);
}

// Build a property stream with the real wire codec, then decode it and check we
// recover the exact handles/names/values we wrote.
static void TestDecodeRoundTrip(const std::string& dir) {
    std::printf("TestDecodeRoundTrip\n");
    NetFieldTable t;
    CHECK(t.LoadFromFile(dir + "/netfields_u_ROTeamInfo.txt", "ROTeamInfo"));
    const uint32_t M = t.MaxIndex();  // 78

    BitWriter bw;
    // handle 14 bNetOwner (bool) = true
    bw.SerializeInt(14, M); bw.WriteBit(true);
    // handle 11 DrawScale (float) = 2.5
    bw.SerializeInt(11, M); bw.WriteFloat(2.5f);
    // handle 13 Location (struct Vector) = (10, -3, 0) via SerializeCompressed
    bw.SerializeInt(13, M);
    {
        // Bits budget for max|comp|=10 -> appCeilLogTwo(1+10)=4, clamp[1,20]=4, -1=3
        uint32_t bits = 3;
        bw.SerializeInt(bits, 20);
        uint32_t maxv = 1u << (bits + 2);          // 32
        int32_t bias = static_cast<int32_t>(1u << (bits + 1)); // 16
        bw.SerializeInt(static_cast<uint32_t>(10 + bias), maxv);
        bw.SerializeInt(static_cast<uint32_t>(-3 + bias), maxv);
        bw.SerializeInt(static_cast<uint32_t>(0 + bias), maxv);
    }

    auto bytes = bw.GetBytes();
    BunchPropertyDecoder dec(1024);
    BunchDecodeResult res = dec.Decode(t, bytes.data(), bytes.size(), bw.NumBits());

    CHECK_EQ(res.className, std::string("ROTeamInfo"));
    CHECK(res.status == BunchDecodeStatus::CleanEnd);
    CHECK_EQ(res.properties.size(), 3u);
    if (res.properties.size() == 3) {
        CHECK_EQ(res.properties[0].name, std::string("bNetOwner"));
        CHECK_EQ(res.properties[0].valueSummary, std::string("true"));
        CHECK(res.properties[0].valueDecoded);

        CHECK_EQ(res.properties[1].name, std::string("DrawScale"));
        CHECK_EQ(res.properties[1].valueSummary, std::string("2.5"));

        CHECK_EQ(res.properties[2].name, std::string("Location"));
        CHECK_EQ(res.properties[2].valueSummary, std::string("(10,-3,0)"));
    }
    // The whole stream should have been consumed.
    CHECK_EQ(res.bitsConsumed, bw.NumBits());
}

// An enum-byte value has an unknown bit width, so the decoder must record the
// handle but stop cleanly (no desync) rather than guess.
static void TestDecodeStopsOnEnum(const std::string& dir) {
    std::printf("TestDecodeStopsOnEnum\n");
    NetFieldTable t;
    CHECK(t.LoadFromFile(dir + "/netfields_u_ROTeamInfo.txt", "ROTeamInfo"));
    const uint32_t M = t.MaxIndex();

    BitWriter bw;
    bw.SerializeInt(14, M); bw.WriteBit(true);     // bNetOwner = true  (decodable)
    bw.SerializeInt(8, M);  bw.WriteBits(1, 2);    // Role = enum (undecodable width)

    auto bytes = bw.GetBytes();
    BunchPropertyDecoder dec;
    auto res = dec.Decode(t, bytes.data(), bytes.size(), bw.NumBits());

    CHECK(res.status == BunchDecodeStatus::StoppedUnresolved);
    CHECK_EQ(res.properties.size(), 2u);
    if (res.properties.size() == 2) {
        CHECK(res.properties[0].valueDecoded);             // bool decoded
        CHECK_EQ(res.properties[1].name, std::string("Role"));
        CHECK(!res.properties[1].valueDecoded);            // enum stopped
    }
}

static ProtocolDecoderConfig MakeTestConfig(bool async) {
    ProtocolDecoderConfig cfg;
    cfg.enabled = true;
    cfg.asyncAnalysis = async;
    cfg.logRawPackets = false;
    cfg.exportJsonDefinitions = false;
    cfg.persistState = false;
    cfg.decodeBunchProperties = false;       // keep payload-analysis path isolated
    cfg.minSamplesForConfidence = 8;
    cfg.outputDirectory = "build_re2/test_re_out";
    return cfg;
}

// A payload that exercises field detection: float + uint32-ish + a couple bytes.
static std::vector<uint8_t> MakeSamplePayload(uint8_t seed) {
    std::vector<uint8_t> p;
    BitWriter bw;
    bw.WriteFloat(100.0f + seed);   // a plausible float
    bw.WriteUInt32(0x00001000u + seed); // an id-ish value
    bw.WriteByte(seed & 1);         // boolean-ish
    auto bytes = bw.GetBytes();
    return bytes;
}

// Sync mode: deterministic — feed samples and assert a structure with fields is
// inferred from the modal payload size.
static void TestDecoderSyncLayout() {
    std::printf("TestDecoderSyncLayout\n");
    ProtocolDecoder dec;
    dec.Initialize(MakeTestConfig(/*async=*/false));
    dec.OnClientConnected(1, "127.0.0.1");
    for (int i = 0; i < 12; ++i) {
        auto p = MakeSamplePayload(static_cast<uint8_t>(i));
        dec.OnPacketReceived(1, p, "PLAYER_MOVE");
    }
    auto st = dec.GetStructure("PLAYER_MOVE");
    CHECK(st.has_value());
    if (st) {
        CHECK(st->totalSamples == 12);
        CHECK(st->layoutFinalized);
        CHECK(st->fields.size() >= 2);   // at least the float + the id
    }
    auto stats = dec.GetStatistics();
    CHECK(stats.totalPacketsAnalyzed == 12);
    dec.Shutdown();
}

// Async mode: the worker thread must drain the queue. Poll until processed.
static void TestDecoderAsyncDrain() {
    std::printf("TestDecoderAsyncDrain\n");
    ProtocolDecoder dec;
    dec.Initialize(MakeTestConfig(/*async=*/true));
    dec.OnClientConnected(2, "127.0.0.1");
    const int N = 20;
    for (int i = 0; i < N; ++i) {
        auto p = MakeSamplePayload(static_cast<uint8_t>(i));
        dec.OnPacketReceived(2, p, "ASYNC_TAG");
    }
    // Wait (bounded) for the worker to catch up.
    bool drained = false;
    for (int tries = 0; tries < 200; ++tries) {
        if (dec.GetStatistics().totalPacketsAnalyzed >= static_cast<uint64_t>(N)) {
            drained = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(drained);
    auto st = dec.GetStructure("ASYNC_TAG");
    CHECK(st.has_value());
    dec.Shutdown();   // joins the worker cleanly
}

// Bit-exact replica of the real capture's ch21 TeamInfo OPEN bunch
// (docs/re/open_bunch_structure.md §4.2): classRef(90245) + Location(0,0,0) +
// {handle 23 TeamIndex int = 2}. Verifies the SerializeNewActor header parse
// (class identity + headerBits=43) and that the property block decodes from the
// correct bit offset.
static void TestOpenBunchHeaderParse(const std::string& dir) {
    std::printf("TestOpenBunchHeaderParse\n");
    NetFieldTable t;
    CHECK(t.LoadFromFile(dir + "/netfields_u_ROTeamInfo.txt", "ROTeamInfo"));
    const uint32_t M = t.MaxIndex();

    BitWriter bw;
    // [class ref] selector=0 (static) + SerializeInt(index, 1<<31)
    bw.WriteBit(false);
    bw.SerializeInt(90245u, 0x80000000u);
    // [Location] compressed (0,0,0): Bits=0 -> SerializeInt(0,20) + 3x SerializeInt(Bias,Max)
    bw.SerializeInt(0u, 20u);
    bw.SerializeInt(2u, 4u);   // 0 + Bias(2)
    bw.SerializeInt(2u, 4u);
    bw.SerializeInt(2u, 4u);
    // [property block] handle 23 TeamIndex (int) = 2
    bw.SerializeInt(23u, M);
    bw.WriteInt32(2);

    auto bytes = bw.GetBytes();

    BunchPropertyDecoder dec(1024);
    auto resolver = [](uint32_t idx) -> std::string {
        return idx == 90245u ? std::string("ROTeamInfo") : std::string();
    };
    auto hdr = dec.ParseOpenHeader(bytes.data(), bytes.size(), bw.NumBits(), resolver);
    CHECK(hdr.ok);
    CHECK_EQ(hdr.classIndex, 90245u);
    CHECK_EQ(hdr.className, std::string("ROTeamInfo"));
    CHECK_EQ(hdr.headerBits, 43u);   // 1 selector + 31 classIdx + 11 location

    // Decode the property block starting after the header.
    auto res = dec.Decode(t, bytes.data(), bytes.size(), bw.NumBits(), hdr.headerBits);
    CHECK(res.properties.size() >= 1);
    if (!res.properties.empty()) {
        CHECK_EQ(res.properties[0].name, std::string("TeamIndex"));
        CHECK_EQ(res.properties[0].valueSummary, std::string("2"));
        CHECK(res.properties[0].valueDecoded);
    }

    // A dynamic-selector (bit=1) header must be rejected as "not a class ref".
    BitWriter dyn;
    dyn.WriteBit(true);
    dyn.SerializeInt(5u, 1024u);
    auto db = dyn.GetBytes();
    auto bad = dec.ParseOpenHeader(db.data(), db.size(), dyn.NumBits(), resolver);
    CHECK(!bad.ok);
}

// PRI value decode: the now-typed ROPlayerReplicationInfo table should decode a
// scalar block (float/FString/UniqueNetId QWORD) and a static-array element
// (byte[3] with its 8-bit element index), mirroring the capture's ch13 PRI.
static void TestPRIValueDecode(const std::string& dir) {
    std::printf("TestPRIValueDecode\n");
    NetFieldTable t;
    CHECK(t.LoadFromFile(dir + "/netfields_u_ROPlayerReplicationInfo.txt", "ROPlayerReplicationInfo"));
    CHECK(t.HasValueTypes());            // enrichment took effect
    CHECK_EQ(t.MaxIndex(), 98u);

    // Static-array field carries arrayDim.
    const NetField* ra = t.GetField(70);
    CHECK(ra && ra->name == "RecentAchievements");
    CHECK(ra && ra->IsStaticArray() && ra->arrayDim == 3u);
    const NetField* uid = t.GetField(23);
    CHECK(uid && uid->valueType == NetValueType::StructUniqueNetId);

    const uint32_t M = t.MaxIndex();
    BitWriter bw;
    bw.SerializeInt(40u, M); bw.WriteFloat(164.0f);                  // Score
    bw.SerializeInt(37u, M); bw.WriteString("DodgR");               // PlayerName
    bw.SerializeInt(23u, M); bw.WriteUInt64(0x011000011835f45fULL); // UniqueId
    bw.SerializeInt(70u, M); bw.WriteByte(1); bw.WriteByte(42);     // RecentAchievements[1]=42

    auto bytes = bw.GetBytes();
    BunchPropertyDecoder dec(1024);
    auto res = dec.Decode(t, bytes.data(), bytes.size(), bw.NumBits());

    CHECK_EQ(res.properties.size(), 4u);
    if (res.properties.size() == 4) {
        CHECK_EQ(res.properties[0].name, std::string("Score"));
        CHECK_EQ(res.properties[0].valueSummary, std::string("164"));
        CHECK_EQ(res.properties[1].name, std::string("PlayerName"));
        CHECK_EQ(res.properties[1].valueSummary, std::string("\"DodgR\""));
        CHECK_EQ(res.properties[2].name, std::string("UniqueId"));
        CHECK(res.properties[2].valueSummary.rfind("uid:", 0) == 0);
        CHECK_EQ(res.properties[3].name, std::string("RecentAchievements[1]"));
        CHECK_EQ(res.properties[3].valueSummary, std::string("42"));
        CHECK(res.properties[3].valueDecoded);
    }
    CHECK_EQ(res.bitsConsumed, bw.NumBits());
}

// Regression (Codex review P2): persistence seeds totalSamples across runs, so
// layout inference must gate on the CURRENT-RUN buffered sample count, not the
// cumulative total — otherwise the first packet after a restart locks the layout
// from one (possibly atypical) sample. Simulated via the real persist/reload path.
static void TestPersistenceLayoutGate() {
    std::printf("TestPersistenceLayoutGate\n");
    ProtocolDecoderConfig cfg = MakeTestConfig(/*async=*/false);
    cfg.persistState = true;
    cfg.minSamplesForConfidence = 8;
    cfg.outputDirectory = "build/re_persist_test";

    // Run 1: feed 8 packets so the tag finalizes, then shut down (writes state).
    {
        ProtocolDecoder a;
        a.Initialize(cfg);
        a.OnClientConnected(1, "127.0.0.1");
        for (int i = 0; i < 8; ++i) {
            auto p = MakeSamplePayload(static_cast<uint8_t>(i));
            a.OnPacketReceived(1, p, "SEED");
        }
        CHECK(a.GetStructure("SEED").value().layoutFinalized);
        a.Shutdown();   // persists SEED with a high totalSamples
    }

    // Run 2: a fresh decoder loads the seeded totalSamples. The first packet must
    // NOT finalize the layout (only one current-run sample buffered).
    {
        ProtocolDecoder b;
        b.Initialize(cfg);
        CHECK(b.GetStructure("SEED").has_value());      // merged from disk
        b.OnClientConnected(2, "127.0.0.1");
        auto p0 = MakeSamplePayload(0);
        b.OnPacketReceived(2, p0, "SEED");
        auto s1 = b.GetStructure("SEED");
        CHECK(s1.has_value());
        CHECK(!s1->layoutFinalized);    // the bug would make this true after 1 pkt
        // After enough current-run samples it should finalize normally.
        for (int i = 1; i < 8; ++i) {
            auto p = MakeSamplePayload(static_cast<uint8_t>(i));
            b.OnPacketReceived(2, p, "SEED");
        }
        CHECK(b.GetStructure("SEED").value().layoutFinalized);
        b.Shutdown();
    }
}

int main() {
    std::printf("=== ProtocolDecoder / NetFieldTable self-tests ===\n");
    const std::string dir = NetfieldsDir();
    std::printf("Using netfields dir: %s\n", dir.c_str());

    TestClassifyType();
    TestTableParse(dir);
    TestLeanTableParse(dir);
    TestRegistry(dir);
    TestDecodeRoundTrip(dir);
    TestDecodeStopsOnEnum(dir);
    TestOpenBunchHeaderParse(dir);
    TestPRIValueDecode(dir);
    TestDecoderSyncLayout();
    TestDecoderAsyncDrain();
    TestPersistenceLayoutGate();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
