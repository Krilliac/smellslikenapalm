#include "TestFramework.h"

#include "Network/RetailBootstrap.h"
#include "Utils/CryptoUtils.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path FindDataFile(std::string_view fileName) {
    const std::filesystem::path sourceFile(__FILE__);
    const std::filesystem::path name(fileName);
    const std::array<std::filesystem::path, 5> candidates = {
        sourceFile.parent_path().parent_path() / "data" / name,
        std::filesystem::current_path() / "data" / name,
        std::filesystem::current_path().parent_path() / "data" / name,
        std::filesystem::current_path().parent_path().parent_path() / "data" / name,
        std::filesystem::current_path().parent_path().parent_path().parent_path() /
            "data" / name
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::vector<uint8_t> ReadDataFile(std::string_view fileName) {
    const std::filesystem::path path = FindDataFile(fileName);
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> ReadCanonicalBootstrap() {
    return ReadDataFile("replication_bootstrap.bin");
}

std::vector<uint8_t> ReadInstalledBootstrap() {
    return ReadDataFile("replication_bootstrap_installed.bin");
}

std::vector<uint8_t> ReadActorBootstrap() {
    return ReadDataFile("actor_bootstrap.bin");
}

size_t CountChangedRecords(const RetailBootstrap::Document& left,
                           const RetailBootstrap::Document& right) {
    if (left.records.size() != right.records.size()) {
        return std::numeric_limits<size_t>::max();
    }
    size_t changed = 0;
    for (size_t i = 0; i < left.records.size(); ++i) {
        if (left.records[i] != right.records[i]) ++changed;
    }
    return changed;
}

std::optional<uint32_t> ReadStaticObjectIndexAt(
    const std::vector<uint8_t>& bytes, size_t selectorBit) {
    if (selectorBit > bytes.size() * 8u ||
        bytes.size() * 8u - selectorBit < 32u) {
        return std::nullopt;
    }
    auto bitAt = [&](size_t bit) {
        return (bytes[bit / 8u] >> (bit % 8u)) & 1u;
    };
    if (bitAt(selectorBit) != 0u) return std::nullopt;

    uint32_t index = 0;
    for (size_t bit = 0; bit < 31u; ++bit) {
        index |= static_cast<uint32_t>(bitAt(selectorBit + 1u + bit)) << bit;
    }
    return index;
}

} // namespace

TEST(RetailBootstrap, ResortRoundTripIsByteIdenticalToCanonicalCapture) {
    const std::vector<uint8_t> original = ReadCanonicalBootstrap();
    ASSERT_FALSE(original.empty()) << "canonical data/replication_bootstrap.bin not found";
    EXPECT_EQ(original.size(), 34199u);

    RetailBootstrap::Document canonical;
    std::string error;
    ASSERT_TRUE(RetailBootstrap::Parse(original, canonical, error)) << error;
    EXPECT_EQ(canonical.records.size(), 31u);

    RetailBootstrap::Document rebuilt;
    ASSERT_TRUE(RetailBootstrap::BuildVariant(
        canonical, RetailBootstrap::CanonicalProfile(), rebuilt, error)) << error;
    EXPECT_EQ(CountChangedRecords(canonical, rebuilt), 0u);
    EXPECT_EQ(RetailBootstrap::Serialize(rebuilt), original);
}

TEST(RetailBootstrap, ArtifactSelectionRequiresExactInstalledOptIn) {
    std::string error = "stale";

    const auto absent = RetailBootstrap::ResolveArtifactSelection({}, error);
    ASSERT_TRUE(absent.has_value());
    EXPECT_EQ(absent->variant, std::string_view("canonical"));
    EXPECT_EQ(absent->path,
              std::string_view("data/replication_bootstrap.bin"));
    EXPECT_EQ(absent->mapObjectRefOffset, 0u);
    EXPECT_TRUE(error.empty());

    error = "stale";
    const auto installed =
        RetailBootstrap::ResolveArtifactSelection("installed", error);
    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(installed->variant, std::string_view("installed"));
    EXPECT_EQ(installed->path,
              std::string_view("data/replication_bootstrap_installed.bin"));
    EXPECT_EQ(installed->mapObjectRefOffset, 5u);
    EXPECT_TRUE(error.empty());

    for (const std::string_view unsupported : {
             std::string_view("canonical"), std::string_view("Installed"),
             std::string_view(" installed"), std::string_view("installed "),
             std::string_view("unknown")}) {
        error.clear();
        EXPECT_FALSE(
            RetailBootstrap::ResolveArtifactSelection(unsupported, error).has_value())
            << unsupported;
        EXPECT_FALSE(error.empty()) << unsupported;
    }
}

TEST(RetailBootstrap, ReplicationArtifactsArePinnedToTheirSelectedLayouts) {
    const std::vector<uint8_t> canonical = ReadCanonicalBootstrap();
    const std::vector<uint8_t> installed = ReadInstalledBootstrap();
    ASSERT_EQ(canonical.size(), 34199u);
    ASSERT_EQ(installed.size(), 34199u);
    EXPECT_EQ(CryptoUtils::SHA256Hex(canonical),
              std::string("a8ea6dcabc9f550136f6ea0bd2dd5c61c3694839bb7b2e9de82b4b1dad5e53d6"));
    EXPECT_EQ(CryptoUtils::SHA256Hex(installed),
              std::string("519d7594002182059cfdbb0c25b5ecb330fb83b392878c2e62ba3c29dada596f"));

    std::string error = "stale";
    EXPECT_TRUE(RetailBootstrap::ValidateReplicationArtifact(
        canonical, "canonical", error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(RetailBootstrap::ValidateReplicationArtifact(
        installed, "installed", error)) << error;
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(RetailBootstrap::ValidateReplicationArtifact(
        canonical, "installed", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(RetailBootstrap::ValidateReplicationArtifact(
        installed, "canonical", error));
    EXPECT_FALSE(error.empty());

    std::vector<uint8_t> corrupted = installed;
    corrupted.back() ^= 0x01u;
    EXPECT_FALSE(RetailBootstrap::ValidateReplicationArtifact(
        corrupted, "installed", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(RetailBootstrap::ValidateReplicationArtifact(
        installed, "Installed", error));
    EXPECT_FALSE(error.empty());
}

TEST(RetailBootstrap, ArtifactSelectionCarriesExactRoGameLayoutAndSafetyGates) {
    std::string error;
    const auto canonical = RetailBootstrap::ResolveArtifactSelection({}, error);
    const auto installed =
        RetailBootstrap::ResolveArtifactSelection("installed", error);
    ASSERT_TRUE(canonical.has_value());
    ASSERT_TRUE(installed.has_value());

    EXPECT_EQ(canonical->roGame.actualObjectBase, 39478u);
    EXPECT_EQ(canonical->roGame.playerControllerClassRef, 57520u);
    EXPECT_EQ(canonical->roGame.playerReplicationInfoClassRef, 86701u);
    EXPECT_EQ(canonical->roGame.gameReplicationInfoClassRef, 70887u);
    EXPECT_EQ(canonical->roGame.teamInfoClassRef, 90245u);
    EXPECT_TRUE(canonical->roGame.roleRegistryGrounded);
    EXPECT_TRUE(canonical->roGame.capturedWorldReplayGrounded);

    EXPECT_EQ(installed->roGame.actualObjectBase, 39478u);
    EXPECT_EQ(installed->roGame.playerControllerClassRef, 57522u);
    EXPECT_EQ(installed->roGame.playerReplicationInfoClassRef, 86704u);
    EXPECT_EQ(installed->roGame.gameReplicationInfoClassRef, 70889u);
    EXPECT_EQ(installed->roGame.teamInfoClassRef, 90248u);
    EXPECT_FALSE(installed->roGame.roleRegistryGrounded);
    EXPECT_FALSE(installed->roGame.capturedWorldReplayGrounded);

    struct GameClassCase {
        const char* path;
        uint32_t canonicalRef;
        uint32_t installedRef;
    };
    constexpr std::array<GameClassCase, 3> cases = {{
        {"ROGame.ROGameInfoTerritories", 69601u, 69603u},
        {"ROGame.ROGameInfoSkirmish", 70363u, 70365u},
        {"ROGame.ROGameInfoSupremacy", 70442u, 70444u},
    }};
    for (const GameClassCase& testCase : cases) {
        EXPECT_EQ(RetailBootstrap::ResolveGameClassRef(
                      *canonical, testCase.path),
                  std::optional<uint32_t>(testCase.canonicalRef));
        EXPECT_EQ(RetailBootstrap::ResolveGameClassRef(
                      *installed, testCase.path),
                  std::optional<uint32_t>(testCase.installedRef));
    }
    EXPECT_FALSE(RetailBootstrap::ResolveGameClassRef(
        *installed, "ROGame.UnknownGameInfo").has_value());

    // The role decoder's historical grounding token is deliberately distinct
    // from the actual package ObjectBase and must not drift with this layout.
    EXPECT_EQ(RetailBootstrap::kCapturedRoGameObjectBase, 39479u);
    EXPECT_NE(canonical->roGame.actualObjectBase,
              RetailBootstrap::kCapturedRoGameObjectBase);
}

TEST(RetailBootstrap, ArtifactSelectionPinsOwningPawnPackageMapLayout) {
    std::string error = "stale";
    const auto canonical = RetailBootstrap::ResolveArtifactSelection({}, error);
    ASSERT_TRUE(canonical.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(canonical->variant, std::string_view("canonical"));
    EXPECT_EQ(canonical->owningPawn.roGameContentObjectRefOffset, 0u);
    EXPECT_EQ(canonical->owningPawn.inventoryManagerArchetypeRef, 82735u);

    error = "stale";
    const auto installed =
        RetailBootstrap::ResolveArtifactSelection("installed", error);
    ASSERT_TRUE(installed.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(installed->variant, std::string_view("installed"));
    EXPECT_EQ(installed->owningPawn.roGameContentObjectRefOffset, 5u);
    EXPECT_EQ(installed->owningPawn.inventoryManagerArchetypeRef, 82737u);

    EXPECT_EQ(RetailBootstrap::ResolveRoGameContentWireRef(
                  *canonical, 286151u),
              std::optional<uint32_t>(286151u));
    EXPECT_EQ(RetailBootstrap::ResolveRoGameContentWireRef(
                  *installed, 286151u),
              std::optional<uint32_t>(286156u));

    // Every independently decoded ROGameContent reference emitted by the
    // owning pawn graph must move with the same package boundary. This covers
    // both faction actor opens and the h167/h147 attachment refs.
    constexpr std::array<uint32_t, 18> owningGraphRefs{{
        286151u, 286374u, 286391u, 286464u, 286109u, 286389u,
        286147u, 286271u, 286804u, 286758u,
        286936u, 286946u, 287063u, 286944u, 286126u,
        286845u, 287188u, 287147u,
    }};
    for (const uint32_t canonicalRef : owningGraphRefs) {
        EXPECT_EQ(RetailBootstrap::ResolveRoGameContentWireRef(
                      *canonical, canonicalRef),
                  std::optional<uint32_t>(canonicalRef));
        EXPECT_EQ(RetailBootstrap::ResolveRoGameContentWireRef(
                      *installed, canonicalRef),
                  std::optional<uint32_t>(canonicalRef + 5u));
    }

    constexpr uint32_t maxStaticRef = 0x7fffffffu;
    EXPECT_EQ(RetailBootstrap::ResolveRoGameContentWireRef(
                  *canonical, maxStaticRef),
              std::optional<uint32_t>(maxStaticRef));
    EXPECT_EQ(RetailBootstrap::ResolveRoGameContentWireRef(
                  *installed, maxStaticRef - 5u),
              std::optional<uint32_t>(maxStaticRef));
    EXPECT_FALSE(RetailBootstrap::ResolveRoGameContentWireRef(
        *canonical, 0u).has_value());
    EXPECT_FALSE(RetailBootstrap::ResolveRoGameContentWireRef(
        *installed, 0u).has_value());
    EXPECT_FALSE(RetailBootstrap::ResolveRoGameContentWireRef(
        *installed, maxStaticRef - 4u).has_value());
    EXPECT_FALSE(RetailBootstrap::ResolveRoGameContentWireRef(
        *canonical, maxStaticRef + 1u).has_value());
}

TEST(RetailBootstrap, ActorArtifactFramingRejectsEveryPartialStream) {
    const std::vector<uint8_t> canonical = ReadActorBootstrap();
    ASSERT_FALSE(canonical.empty());
    std::string error = "stale";
    EXPECT_TRUE(RetailBootstrap::ValidateActorArtifactFraming(
        canonical, error)) << error;
    EXPECT_TRUE(error.empty());

    std::vector<uint8_t> trailing = canonical;
    trailing.push_back(0xff);
    EXPECT_FALSE(RetailBootstrap::ValidateActorArtifactFraming(
        trailing, error));
    EXPECT_FALSE(error.empty());

    std::vector<uint8_t> truncated = canonical;
    truncated.pop_back();
    EXPECT_FALSE(RetailBootstrap::ValidateActorArtifactFraming(
        truncated, error));
    EXPECT_FALSE(error.empty());

    // One complete descriptor claiming a two-byte payload but carrying one.
    const std::vector<uint8_t> shortPayload = {
        0x02, 0x00, 0x02, 0x05, 0x01, 0x00,
        0x10, 0x00, 0x00, 0x00, 0xaa};
    EXPECT_FALSE(RetailBootstrap::ValidateActorArtifactFraming(
        shortPayload, error));
    EXPECT_FALSE(error.empty());

    EXPECT_FALSE(RetailBootstrap::ValidateActorArtifactFraming({}, error));
    EXPECT_FALSE(error.empty());
}

TEST(RetailBootstrap, InstalledActorArtifactHasOnlyGroundedReferenceChanges) {
    const std::vector<uint8_t> canonical = ReadActorBootstrap();
    ASSERT_FALSE(canonical.empty()) << "canonical data/actor_bootstrap.bin not found";
    ASSERT_EQ(canonical.size(), 14537u);
    EXPECT_EQ(CryptoUtils::SHA256Hex(canonical),
              std::string("af0ca5564045d74b265087066d848d42d5c0dd0cf2936ea9738842430522b3e0"));

    std::string error = "stale";
    std::vector<uint8_t> installed;
    ASSERT_TRUE(RetailBootstrap::BuildActorArtifactVariant(
        canonical, "installed", installed, error)) << error;
    EXPECT_TRUE(error.empty());
    ASSERT_EQ(installed.size(), canonical.size());
    EXPECT_EQ(CryptoUtils::SHA256Hex(installed),
              std::string("6df1e41b196315deb796b20c64c7397c55861feed16bcb636220239964851418"));

    struct ExpectedChange {
        size_t offset;
        uint8_t before;
        uint8_t after;
    };
    constexpr std::array<ExpectedChange, 19> expectedChanges = {{
        {10,   0x60, 0x64},
        {118,  0x82, 0x92},
        {2514, 0x0a, 0x10},
        {2904, 0x5a, 0x60},
        {5268, 0xce, 0xd2},
        {5281, 0x44, 0xc4},
        {5461, 0x00, 0xa0},
        {5467, 0xe0, 0x30},
        {5468, 0x86, 0x87},
        {5473, 0x78, 0xa0},
        {5479, 0xc4, 0xd8},
        {5485, 0xe4, 0xee},
        {5491, 0x6d, 0x72},
        {5496, 0x80, 0x00},
        {5497, 0x4b, 0x4e},
        {5502, 0x00, 0x40},
        {5503, 0x25, 0x26},
        {5979, 0x0a, 0x10},
        {8362, 0x0a, 0x10},
    }};
    size_t changed = 0;
    for (size_t offset = 0; offset < canonical.size(); ++offset) {
        if (canonical[offset] != installed[offset]) ++changed;
    }
    EXPECT_EQ(changed, expectedChanges.size());
    for (const ExpectedChange& change : expectedChanges) {
        EXPECT_EQ(canonical[change.offset], change.before) << change.offset;
        EXPECT_EQ(installed[change.offset], change.after) << change.offset;
    }

    struct StaticRefCase {
        size_t selectorBit;
        uint32_t canonicalRef;
        uint32_t installedRef;
    };
    // Absolute bit offsets include actor-artifact descriptors. This semantic
    // cohort catches a byte patch that happens to produce the pinned diff while
    // corrupting a neighbouring unaligned object reference.
    constexpr std::array<StaticRefCase, 16> staticRefs = {{
        {80u,    57520u, 57522u},  // ch2 Default__ROPlayerController archetype
        {946u,   76592u, 76594u},  // ch2 ClientSetHUD -> ROHUD UClass
        {20112u, 90245u, 90248u},  // ch21 Default__ROTeamInfo archetype
        {23232u, 86701u, 86704u},  // ch26 Default__ROPlayerReplicationInfo archetype
        {42144u, 70887u, 70889u},  // ch54 Default__ROGameReplicationInfo archetype
        {42253u, 69601u, 69603u},  // ch54 GameClass
        {43692u, 301168u, 301173u}, // MapBoundaries[0]
        {43739u, 301166u, 301171u}, // MapBoundaries[1]
        {43786u, 301167u, 301172u}, // MapBoundaries[2]
        {43833u, 301169u, 301174u}, // MapBoundaries[3]
        {43880u, 301170u, 301175u}, // MapBoundaries[4]
        {43927u, 301165u, 301170u}, // MapBoundaries[5]
        {43974u, 301207u, 301212u}, // AxisSpawnProtection[0]
        {44021u, 301204u, 301209u}, // AlliesSpawnProtection[0]
        {47832u, 90245u, 90248u},  // ch56 Default__ROTeamInfo archetype
        {66896u, 90245u, 90248u},  // ch76 Default__ROTeamInfo archetype
    }};
    for (const StaticRefCase& ref : staticRefs) {
        EXPECT_EQ(ReadStaticObjectIndexAt(canonical, ref.selectorBit),
                  std::optional<uint32_t>(ref.canonicalRef))
            << ref.selectorBit;
        EXPECT_EQ(ReadStaticObjectIndexAt(installed, ref.selectorBit),
                  std::optional<uint32_t>(ref.installedRef))
            << ref.selectorBit;
    }

    std::vector<uint8_t> unchanged;
    ASSERT_TRUE(RetailBootstrap::BuildActorArtifactVariant(
        canonical, "canonical", unchanged, error)) << error;
    EXPECT_EQ(unchanged, canonical);
}

TEST(RetailBootstrap, ActorArtifactFailsClosedOnWrongInputOrVariant) {
    const std::vector<uint8_t> canonical = ReadActorBootstrap();
    ASSERT_EQ(canonical.size(), 14537u);

    std::string error;
    std::vector<uint8_t> output = {0xff};
    EXPECT_FALSE(RetailBootstrap::BuildActorArtifactVariant(
        canonical, "Installed", output, error));
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(error.empty());

    std::vector<uint8_t> corrupted = canonical;
    corrupted[10] ^= 0x01;
    output = {0xff};
    error.clear();
    EXPECT_FALSE(RetailBootstrap::BuildActorArtifactVariant(
        corrupted, "installed", output, error));
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(error.empty());
    output = {0xff};
    error.clear();
    EXPECT_FALSE(RetailBootstrap::BuildActorArtifactVariant(
        corrupted, "canonical", output, error));
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(error.empty());

    std::vector<uint8_t> truncated(canonical.begin(), canonical.end() - 1);
    output = {0xff};
    error.clear();
    EXPECT_FALSE(RetailBootstrap::BuildActorArtifactVariant(
        truncated, "installed", output, error));
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(error.empty());
    output = {0xff};
    error.clear();
    EXPECT_FALSE(RetailBootstrap::BuildActorArtifactVariant(
        truncated, "canonical", output, error));
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(error.empty());
}

TEST(RetailBootstrap, RepresentativeProfilesShareOneModeClassResolver) {
    const auto resort = RetailBootstrap::ResolveProfile("VNTE-Resort", "Territories");
    EXPECT_EQ(resort.mapUrl, std::string("VNTE-Resort"));
    EXPECT_EQ(resort.gameClassPath, std::string("ROGame.ROGameInfoTerritories"));
    EXPECT_EQ(resort.gameClassIndex, 69601u);
    EXPECT_EQ(resort.roGameObjectBase,
              RetailBootstrap::kCapturedRoGameObjectBase);
    EXPECT_FALSE(resort.experimental);
    EXPECT_FALSE(resort.usedFallback);

    const auto cuchi = RetailBootstrap::ResolveProfile("vnte-cuchi", "Territories");
    EXPECT_EQ(cuchi.mapUrl, std::string("VNTE-CuChi"));
    EXPECT_EQ(cuchi.gameClassIndex, 69601u);
    EXPECT_EQ(cuchi.roGameObjectBase,
              RetailBootstrap::kCapturedRoGameObjectBase);
    EXPECT_TRUE(cuchi.experimental);

    const auto hue = RetailBootstrap::ResolveProfile("VNSU-HueCity", "Supremacy");
    EXPECT_EQ(hue.gameClassPath, std::string("ROGame.ROGameInfoSupremacy"));
    EXPECT_EQ(hue.gameClassIndex, 70442u);
    EXPECT_EQ(hue.roGameObjectBase, 0u);

    const auto compound = RetailBootstrap::ResolveProfile("VNSK-Compound", "Skirmish");
    EXPECT_EQ(compound.gameClassPath, std::string("ROGame.ROGameInfoSkirmish"));
    EXPECT_EQ(compound.gameClassIndex, 70363u);
    EXPECT_EQ(compound.roGameObjectBase, 0u);

    // Effective mode is authoritative when explicitly resolved by GameServer.
    const auto hueTerritories =
        RetailBootstrap::ResolveProfile("VNSU-HueCity", "Territories");
    EXPECT_EQ(hueTerritories.mapUrl, std::string("VNSU-HueCity"));
    EXPECT_EQ(hueTerritories.gameClassPath,
              std::string("ROGame.ROGameInfoTerritories"));
    EXPECT_EQ(hueTerritories.gameClassIndex, 69601u);

    const auto fallback = RetailBootstrap::ResolveProfile("VNTE-Unknown", "Supremacy");
    EXPECT_TRUE(fallback.usedFallback);
    EXPECT_EQ(fallback.mapUrl, std::string("VNTE-Resort"));
    EXPECT_EQ(fallback.gameClassIndex, 69601u);
}

TEST(RetailBootstrap, ExactProfilesNeverExposeLegacyResortFallback) {
    struct Case {
        const char* map;
        const char* mode;
        const char* canonicalMap;
    };
    constexpr std::array<Case, 4> exactCases = {{
        {"vnte-resort", "Territories", "VNTE-Resort"},
        {"VNTE-CUCHI", "Territories", "VNTE-CuChi"},
        {"vnsu-huecity", "Supremacy", "VNSU-HueCity"},
        {"VNSK-Compound", "Skirmish", "VNSK-Compound"},
    }};
    constexpr std::array<const char*, 32> guidOnlyMaps = {{
        "VNSK-Firebase", "VNSK-JungleCamp", "VNSK-Riverbed", "VNSK-Temple",
        "VNSU-AnLaoValley", "VNSU-OperationForrest", "VNSU-QuangTri",
        "VNSU-SongBe", "VNTE-ASau", "VNTE-AnLaoValley", "VNTE-ApacheSnow",
        "VNTE-BorderWatch", "VNTE-CampaignStart", "VNTE-Compound",
        "VNTE-CuaViet", "VNTE-DaNangAirBase", "VNTE-DemilitarizedZone",
        "VNTE-DongHa", "VNTE-Firebase", "VNTE-FirebaseGeorgina",
        "VNTE-Highway14", "VNTE-Hill937", "VNTE-HueCity", "VNTE-KheSanh",
        "VNTE-LongTan", "VNTE-Mekong", "VNTE-NinhPhu",
        "VNTE-OperationForrest", "VNTE-QuangTri", "VNTE-RungSac",
        "VNTE-Saigon", "VNTE-SongBe",
    }};

    for (const Case& testCase : exactCases) {
        EXPECT_TRUE(RetailBootstrap::HasExactProfile(testCase.map));
        const std::optional<RetailBootstrap::Profile> profile =
            RetailBootstrap::ResolveExactProfile(testCase.map, testCase.mode);
        ASSERT_TRUE(profile.has_value()) << testCase.map;
        EXPECT_FALSE(profile->usedFallback) << testCase.map;
        EXPECT_EQ(profile->mapUrl, std::string(testCase.canonicalMap));

        const std::optional<RetailBootstrap::GuidBytes> guid =
            RetailBootstrap::ResolveMapPackageGuid(testCase.map);
        ASSERT_TRUE(guid.has_value()) << testCase.map;
        EXPECT_EQ(profile->mapPackageGuid, *guid) << testCase.map;
    }

    // A source-grounded package GUID alone must never make a map eligible for
    // bootstrap-driven rotation. Only captured PackageMap/Welcome profiles can
    // enter ResolveExactProfile.
    for (const char* map : guidOnlyMaps) {
        EXPECT_FALSE(RetailBootstrap::HasExactProfile(map)) << map;
        EXPECT_FALSE(
            RetailBootstrap::ResolveExactProfile(map, "Territories").has_value())
            << map;
        const auto guid = RetailBootstrap::ResolveMapPackageGuid(map);
        ASSERT_TRUE(guid.has_value()) << map;
    }

    EXPECT_FALSE(RetailBootstrap::HasExactProfile("VNTE-Unknown"));
    EXPECT_FALSE(RetailBootstrap::ResolveExactProfile(
        "VNTE-Unknown", "Territories").has_value());
    EXPECT_FALSE(RetailBootstrap::ResolveMapPackageGuid("VNTE-Unknown").has_value());
    EXPECT_FALSE(RetailBootstrap::HasExactProfile(""));

    // Startup compatibility remains explicit and isolated to ResolveProfile.
    const RetailBootstrap::Profile legacy =
        RetailBootstrap::ResolveProfile("VNTE-RungSac", "Territories");
    EXPECT_TRUE(legacy.usedFallback);
    EXPECT_EQ(legacy.mapUrl, std::string("VNTE-Resort"));
}

TEST(RetailBootstrap, ExperimentalVariantsEditOnlyDecodedPackageAndWelcome) {
    const std::vector<uint8_t> original = ReadCanonicalBootstrap();
    ASSERT_FALSE(original.empty()) << "canonical data/replication_bootstrap.bin not found";

    RetailBootstrap::Document canonical;
    std::string error;
    ASSERT_TRUE(RetailBootstrap::Parse(original, canonical, error)) << error;

    RetailBootstrap::PackageRecord sourcePackage;
    RetailBootstrap::WelcomeRecord sourceWelcome;
    ASSERT_TRUE(RetailBootstrap::FindPackage(
        canonical, "VNTE-Resort", sourcePackage, error)) << error;
    ASSERT_TRUE(RetailBootstrap::FindWelcome(
        canonical, "VNTE-Resort", "ROGame.ROGameInfoTerritories",
        sourceWelcome, error)) << error;

    struct Case {
        const char* map;
        const char* mode;
        size_t serializedBytes;
        RetailBootstrap::GuidBytes guid;
    };
    const std::array<Case, 3> cases = {{
        {"VNTE-CuChi", "Territories", 34197u,
         {0xe5, 0x45, 0xe1, 0x1b, 0xa9, 0x54, 0x7b, 0x45,
          0x82, 0x32, 0x96, 0x41, 0x2b, 0x01, 0x2e, 0xd6}},
        {"VNSU-HueCity", "Supremacy", 34199u,
         {0x58, 0x1a, 0xb9, 0xd3, 0x3c, 0x88, 0xd6, 0x47,
          0x9f, 0xbf, 0x8c, 0xd7, 0x4a, 0x35, 0xfb, 0xde}},
        {"VNSK-Compound", "Skirmish", 34200u,
         {0xb3, 0xb0, 0xf3, 0xbc, 0xbf, 0x0f, 0xfc, 0x4f,
          0x81, 0x90, 0x0a, 0xf3, 0xda, 0x6d, 0xbd, 0x28}}
    }};

    for (const Case& testCase : cases) {
        const RetailBootstrap::Profile profile =
            RetailBootstrap::ResolveProfile(testCase.map, testCase.mode);
        RetailBootstrap::Document variant;
        ASSERT_TRUE(RetailBootstrap::BuildVariant(canonical, profile, variant, error))
            << testCase.map << ": " << error;

        EXPECT_EQ(variant.records.size(), canonical.records.size());
        EXPECT_EQ(CountChangedRecords(canonical, variant), 2u) << testCase.map;
        EXPECT_EQ(variant.records[0], canonical.records[0]) << testCase.map;
        EXPECT_EQ(variant.records[1], canonical.records[1]) << testCase.map;
        EXPECT_EQ(RetailBootstrap::Serialize(variant).size(), testCase.serializedBytes)
            << testCase.map;

        RetailBootstrap::PackageRecord targetPackage;
        ASSERT_TRUE(RetailBootstrap::FindPackage(
            variant, profile.mapUrl, targetPackage, error)) << error;
        EXPECT_EQ(targetPackage.guid, testCase.guid) << testCase.map;
        EXPECT_EQ(targetPackage.extension, std::string("roe"));
        EXPECT_EQ(targetPackage.packageFlags, 0x20024001u);
        EXPECT_EQ(targetPackage.generation, 2);

        // Everything after the package-name FString is copied verbatim.
        const auto& sourcePackagePayload = canonical.records[sourcePackage.outerRecord];
        const auto& targetPackagePayload = variant.records[targetPackage.outerRecord];
        const std::vector<uint8_t> sourcePackagePrefix(
            sourcePackagePayload.begin(),
            sourcePackagePayload.begin() + static_cast<std::ptrdiff_t>(sourcePackage.begin));
        const std::vector<uint8_t> targetPackagePrefix(
            targetPackagePayload.begin(),
            targetPackagePayload.begin() + static_cast<std::ptrdiff_t>(targetPackage.begin));
        EXPECT_EQ(targetPackagePrefix, sourcePackagePrefix) << testCase.map;
        const std::vector<uint8_t> sourcePackageTail(
            sourcePackagePayload.begin() + static_cast<std::ptrdiff_t>(sourcePackage.nameFieldEnd),
            sourcePackagePayload.begin() + static_cast<std::ptrdiff_t>(sourcePackage.end));
        const std::vector<uint8_t> targetPackageTail(
            targetPackagePayload.begin() + static_cast<std::ptrdiff_t>(targetPackage.nameFieldEnd),
            targetPackagePayload.begin() + static_cast<std::ptrdiff_t>(targetPackage.end));
        EXPECT_EQ(targetPackageTail, sourcePackageTail) << testCase.map;
        const std::vector<uint8_t> sourceFollowingPackages(
            sourcePackagePayload.begin() + static_cast<std::ptrdiff_t>(sourcePackage.end),
            sourcePackagePayload.end());
        const std::vector<uint8_t> targetFollowingPackages(
            targetPackagePayload.begin() + static_cast<std::ptrdiff_t>(targetPackage.end),
            targetPackagePayload.end());
        EXPECT_EQ(targetFollowingPackages, sourceFollowingPackages) << testCase.map;

        RetailBootstrap::WelcomeRecord targetWelcome;
        ASSERT_TRUE(RetailBootstrap::FindWelcome(
            variant, profile.mapUrl, profile.gameClassPath, targetWelcome, error)) << error;
        const auto& sourceWelcomePayload = canonical.records[sourceWelcome.outerRecord];
        const auto& targetWelcomePayload = variant.records[targetWelcome.outerRecord];
        const std::vector<uint8_t> sourceWelcomePrefix(
            sourceWelcomePayload.begin(),
            sourceWelcomePayload.begin() + static_cast<std::ptrdiff_t>(sourceWelcome.begin));
        const std::vector<uint8_t> targetWelcomePrefix(
            targetWelcomePayload.begin(),
            targetWelcomePayload.begin() + static_cast<std::ptrdiff_t>(targetWelcome.begin));
        EXPECT_EQ(targetWelcomePrefix, sourceWelcomePrefix) << testCase.map;
        const std::vector<uint8_t> sourceOpaqueTail(
            sourceWelcomePayload.begin() +
                static_cast<std::ptrdiff_t>(sourceWelcome.gameFieldEnd),
            sourceWelcomePayload.end());
        const std::vector<uint8_t> targetOpaqueTail(
            targetWelcomePayload.begin() +
                static_cast<std::ptrdiff_t>(targetWelcome.gameFieldEnd),
            targetWelcomePayload.end());
        EXPECT_EQ(targetOpaqueTail, sourceOpaqueTail) << testCase.map;
    }
}

TEST(RetailBootstrap, MalformedOuterRecordIsRejectedWithoutPartialDocument) {
    const std::vector<uint8_t> truncated = {0x08, 0x00, 0x00, 0x00, 0x11, 0x01};
    RetailBootstrap::Document document;
    std::string error;
    EXPECT_FALSE(RetailBootstrap::Parse(truncated, document, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(document.records.empty());
}

RS2V_TEST_MAIN()
