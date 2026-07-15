#include "Network/RetailBootstrap.h"

#include "Utils/CryptoUtils.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace RetailBootstrap {
namespace {

struct ActorBootstrapPatch {
    size_t offset;
    uint8_t expected;
    uint8_t replacement;
};

constexpr size_t kCanonicalActorBootstrapBytes = 14537;
constexpr std::string_view kCanonicalActorBootstrapSha256 =
    "af0ca5564045d74b265087066d848d42d5c0dd0cf2936ea9738842430522b3e0";
constexpr std::string_view kInstalledActorBootstrapSha256 =
    "6df1e41b196315deb796b20c64c7397c55861feed16bcb636220239964851418";
constexpr size_t kReplicationBootstrapBytes = 34199;
constexpr std::string_view kCanonicalReplicationBootstrapSha256 =
    "a8ea6dcabc9f550136f6ea0bd2dd5c61c3694839bb7b2e9de82b4b1dad5e53d6";
constexpr std::string_view kInstalledReplicationBootstrapSha256 =
    "519d7594002182059cfdbb0c25b5ecb330fb83b392878c2e62ba3c29dada596f";

// File offsets include each actor bunch's ten-byte descriptor. These changes
// were decoded from the installed ROGame.u and VNTE-Resort.roe export tables,
// not inferred from warning text alone. They rebase the six menu-critical actor
// opens, PC.ClientSetHUD's ROHUD class, GRI.GameClass, six map boundaries, and
// the Axis/Allies spawn-protection references. The map references move by +5
// because packages preceding VNTE-Resort contribute five additional exports.
constexpr std::array<ActorBootstrapPatch, 19> kInstalledActorBootstrapPatches = {{
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

constexpr uint32_t kTerritoriesClassIndex = 69601;
constexpr uint32_t kSkirmishClassIndex = 70363;
constexpr uint32_t kSupremacyClassIndex = 70442;

constexpr RoGameLayout kCanonicalRoGameLayout{
    39478,  // actual ObjectBase
    57520,  // Default__ROPlayerController actor archetype
    86701,  // Default__ROPlayerReplicationInfo actor archetype
    70887,  // Default__ROGameReplicationInfo actor archetype
    90245,  // Default__ROTeamInfo actor archetype
    69601,  // ROGameInfoTerritories
    70363,  // ROGameInfoSkirmish
    70442,  // ROGameInfoSupremacy
    true,   // legacy role registry is grounded for the canonical package
    true,   // canonical diagnostic capture contains all 139 records
};

constexpr RoGameLayout kInstalledRoGameLayout{
    39478,  // installed package retains the actual ObjectBase
    57522,  // Default__ROPlayerController actor archetype
    86704,  // Default__ROPlayerReplicationInfo actor archetype
    70889,  // Default__ROGameReplicationInfo actor archetype
    90248,  // Default__ROTeamInfo actor archetype
    69603,  // ROGameInfoTerritories
    70365,  // ROGameInfoSkirmish
    70444,  // ROGameInfoSupremacy
    false,  // role CDO registry has not been migrated
    false,  // only the menu-critical Resort cohort is fully rebased
};

constexpr OwningPawnPackageMapLayout kCanonicalOwningPawnLayout{
    0u,      // captured ROGameContent refs are already canonical
    82735u,  // Default__ROInventoryManager in the canonical PackageMap
};

constexpr OwningPawnPackageMapLayout kInstalledOwningPawnLayout{
    5u,      // installed ROGameContent exports are shifted by five
    82737u,  // Default__ROInventoryManager CDO in the installed PackageMap
};

constexpr uint32_t kStaticObjectRefMax = 0x80000000u;

constexpr uint8_t HexNibble(char value) {
    return value >= '0' && value <= '9'
        ? static_cast<uint8_t>(value - '0')
        : static_cast<uint8_t>((value >= 'a' && value <= 'f'
            ? value - 'a'
            : value - 'A') + 10);
}

// UE3 renders FGuid as four hexadecimal uint32s. Each uint32 is serialized
// little-endian, so reverse bytes within each word while preserving word order.
constexpr GuidBytes PackageGuid(std::string_view displayed) {
    GuidBytes guid{};
    for (size_t word = 0; word < 4; ++word) {
        for (size_t byte = 0; byte < 4; ++byte) {
            const size_t source = word * 8 + (3 - byte) * 2;
            guid[word * 4 + byte] = static_cast<uint8_t>(
                (HexNibble(displayed[source]) << 4u) |
                HexNibble(displayed[source + 1]));
        }
    }
    return guid;
}

struct MapGuidEntry {
    std::string_view mapUrl;
    GuidBytes guid;
};

// BEGIN SOURCE-GROUNDED MAP GUIDS
// Values come from read-only package-table extraction of the configured retail
// .roe files. tools/ground_cooked_map_guids.py verifies source hashes and this
// table's uniqueness/format before reporting candidates.
constexpr std::array<MapGuidEntry, 36> kMapPackageGuids = {{
    {"VNSK-Compound", PackageGuid("BCF3B0B34FFC0FBFF30A908128BD6DDA")},
    {"VNSK-Firebase", PackageGuid("BA1BDD124DC9ECCF3B48B79C2F86CECE")},
    {"VNSK-JungleCamp", PackageGuid("E1D4BDBD42FF4205B5442AAB4A1809D7")},
    {"VNSK-Riverbed", PackageGuid("6A0B38A340E899F6139E81B5E4938E9C")},
    {"VNSK-Temple", PackageGuid("6DC607F24C14644FC9B356A37A298DEC")},
    {"VNSU-AnLaoValley", PackageGuid("CC3D08E94F925F44F14EDEBD026269CE")},
    {"VNSU-HueCity", PackageGuid("D3B91A5847D6883CD78CBF9FDEFB354A")},
    {"VNSU-OperationForrest", PackageGuid("57514D104733FFEFF1CD988561A156C3")},
    {"VNSU-QuangTri", PackageGuid("F4783A4E4D57435A0C0C7FBF6EC2FDF2")},
    {"VNSU-SongBe", PackageGuid("524383D54078A53A63345EB8B026011D")},
    {"VNTE-ASau", PackageGuid("2352D59E474FE2366EF30D9F127DDF1F")},
    {"VNTE-AnLaoValley", PackageGuid("5EFE03B24AB53DA058A716A5A02DD87F")},
    {"VNTE-ApacheSnow", PackageGuid("5C28B5274E53D326288844AE891E3E3F")},
    {"VNTE-BorderWatch", PackageGuid("D14E4F2B47372F26B671E29192031AD6")},
    {"VNTE-CampaignStart", PackageGuid("5B9BD9844BA55D12190BFF9218B40803")},
    {"VNTE-Compound", PackageGuid("023AFB544CEA575FCEEB2CA23598D8C4")},
    {"VNTE-CuChi", PackageGuid("1BE145E5457B54A941963282D62E012B")},
    {"VNTE-CuaViet", PackageGuid("185C318F4778673FA2116895E2F63015")},
    {"VNTE-DaNangAirBase", PackageGuid("8CF9E1B04B2CB76FAF9046813F045E43")},
    {"VNTE-DemilitarizedZone", PackageGuid("ABC6DF734BB8997464908F8F7DC04E16")},
    {"VNTE-DongHa", PackageGuid("75C92C9C46B523E3202B8BAF3ED94353")},
    {"VNTE-Firebase", PackageGuid("40E07EBF4BD5295862A80C90163CD031")},
    {"VNTE-FirebaseGeorgina", PackageGuid("E54A57454DA6E0EDC73DA5A7781E7523")},
    {"VNTE-Highway14", PackageGuid("F3046C284CE6ACFDC8D43D8CDA24D72A")},
    {"VNTE-Hill937", PackageGuid("AA60EC4F4122050844CDBE9843969C90")},
    {"VNTE-HueCity", PackageGuid("D33D4DA640580F6411332483552401E3")},
    {"VNTE-KheSanh", PackageGuid("D942AF974D183929B301FAB0BE713B98")},
    {"VNTE-LongTan", PackageGuid("6AD7009B46CF1C62F89359B7CAE81086")},
    {"VNTE-Mekong", PackageGuid("ED8C67FE499BCBFCF0501A96EA27EAEB")},
    {"VNTE-NinhPhu", PackageGuid("86F86CEE442F9EED9A9DB0B1312B2A3A")},
    {"VNTE-OperationForrest", PackageGuid("792F7DFC4435EC3D4B568F8B577C062E")},
    {"VNTE-QuangTri", PackageGuid("E467282F48469D44D79A538F91A2AD70")},
    {"VNTE-Resort", PackageGuid("C75E786345B77AA5243259ABAF16C294")},
    {"VNTE-RungSac", PackageGuid("28867F2947A19DC5A5C5A0801B37FEE5")},
    {"VNTE-Saigon", PackageGuid("D57AA61242BB6D894AB5A58D58D45A1F")},
    {"VNTE-SongBe", PackageGuid("2203FB92469FD80E171F78AFCDE51D29")},
}};
// END SOURCE-GROUNDED MAP GUIDS

constexpr GuidBytes GuidForExactMap(std::string_view mapUrl) {
    for (const MapGuidEntry& entry : kMapPackageGuids) {
        if (entry.mapUrl == mapUrl) return entry.guid;
    }
    return {};
}

constexpr bool IsZeroGuid(const GuidBytes& guid) {
    for (uint8_t value : guid) {
        if (value != 0) return false;
    }
    return true;
}

constexpr bool EqualGuid(const GuidBytes& left, const GuidBytes& right) {
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

constexpr bool IsMapGuidTableValid() {
    for (size_t i = 0; i < kMapPackageGuids.size(); ++i) {
        if (kMapPackageGuids[i].mapUrl.empty() ||
            IsZeroGuid(kMapPackageGuids[i].guid)) {
            return false;
        }
        for (size_t j = i + 1; j < kMapPackageGuids.size(); ++j) {
            if (kMapPackageGuids[i].mapUrl == kMapPackageGuids[j].mapUrl ||
                EqualGuid(kMapPackageGuids[i].guid, kMapPackageGuids[j].guid)) {
                return false;
            }
        }
    }
    return true;
}

constexpr GuidBytes kResortGuid = GuidForExactMap("VNTE-Resort");
constexpr GuidBytes kCuChiGuid = GuidForExactMap("VNTE-CuChi");
constexpr GuidBytes kHueCityGuid = GuidForExactMap("VNSU-HueCity");
constexpr GuidBytes kCompoundGuid = GuidForExactMap("VNSK-Compound");
static_assert(IsMapGuidTableValid(), "map package GUID table must be unique and nonzero");
static_assert(!IsZeroGuid(kResortGuid), "Resort package GUID must remain grounded");
static_assert(!IsZeroGuid(kCuChiGuid), "CuChi package GUID must remain grounded");
static_assert(!IsZeroGuid(kHueCityGuid), "HueCity package GUID must remain grounded");
static_assert(!IsZeroGuid(kCompoundGuid), "Compound package GUID must remain grounded");

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
    bytes.push_back(static_cast<uint8_t>(value >> 16u));
    bytes.push_back(static_cast<uint8_t>(value >> 24u));
}

void AppendFString(std::vector<uint8_t>& bytes, std::string_view value) {
    AppendU32(bytes, static_cast<uint32_t>(value.size() + 1u));
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0);
}

bool ReadFString(const std::vector<uint8_t>& bytes,
                 size_t offset,
                 std::string& value,
                 size_t& end) {
    if (offset > bytes.size() || bytes.size() - offset < 4u) return false;
    const int32_t count = static_cast<int32_t>(ReadU32(bytes, offset));
    // Captured package/control names are ANSI FStrings. A negative count is a
    // UTF-16 FString and is intentionally rejected rather than mis-decoded.
    if (count <= 0) return false;
    const size_t length = static_cast<size_t>(count);
    if (length > bytes.size() - offset - 4u) return false;
    const size_t data = offset + 4u;
    if (bytes[data + length - 1u] != 0) return false;
    if (std::find(bytes.begin() + static_cast<std::ptrdiff_t>(data),
                  bytes.begin() + static_cast<std::ptrdiff_t>(data + length - 1u),
                  uint8_t{0}) != bytes.begin() + static_cast<std::ptrdiff_t>(data + length - 1u)) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes.data() + data), length - 1u);
    end = data + length;
    return true;
}

std::string Lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool EqualsInsensitive(std::string_view left, std::string_view right) {
    return Lower(left) == Lower(right);
}

struct ModeClass {
    const char* mode;
    const char* path;
    uint32_t index;
};

ModeClass ClassForMode(std::string_view effectiveMode, const ModeClass& fallback) {
    const std::string mode = Lower(effectiveMode);
    if (mode.find("territor") != std::string::npos) {
        return {"Territories", "ROGame.ROGameInfoTerritories", kTerritoriesClassIndex};
    }
    if (mode.find("suprem") != std::string::npos) {
        return {"Supremacy", "ROGame.ROGameInfoSupremacy", kSupremacyClassIndex};
    }
    if (mode.find("skirm") != std::string::npos) {
        return {"Skirmish", "ROGame.ROGameInfoSkirmish", kSkirmishClassIndex};
    }
    return fallback;
}

bool TryParsePackageRecord(const std::vector<uint8_t>& payload,
                           size_t offset,
                           PackageRecord& out) {
    constexpr size_t kFixedPrefix = 1u + 16u;
    constexpr size_t kFixedTail = 4u + 4u + 8u;
    if (offset > payload.size() || payload.size() - offset < kFixedPrefix) return false;
    if (payload[offset] != 0x07) return false;

    PackageRecord parsed;
    parsed.begin = offset;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset + 1u),
                parsed.guid.size(), parsed.guid.begin());

    size_t cursor = offset + kFixedPrefix;
    parsed.nameFieldBegin = cursor;
    if (!ReadFString(payload, cursor, parsed.name, cursor)) return false;
    parsed.nameFieldEnd = cursor;
    if (!ReadFString(payload, cursor, parsed.extension, cursor)) return false;
    if (payload.size() - cursor < kFixedTail) return false;
    parsed.packageFlags = ReadU32(payload, cursor);
    cursor += 4u;
    parsed.generation = static_cast<int32_t>(ReadU32(payload, cursor));
    cursor += 4u;
    std::string download;
    if (!ReadFString(payload, cursor, download, cursor) || download != "None") return false;
    if (payload.size() - cursor < 8u) return false;
    cursor += 8u;
    parsed.end = cursor;
    out = std::move(parsed);
    return true;
}

std::vector<uint8_t> BuildPackageReplacement(const std::vector<uint8_t>& payload,
                                             const PackageRecord& source,
                                             const Profile& profile) {
    std::vector<uint8_t> replacement;
    replacement.reserve((source.end - source.begin) + profile.mapUrl.size() + 16u);
    replacement.push_back(0x07);
    replacement.insert(replacement.end(), profile.mapPackageGuid.begin(), profile.mapPackageGuid.end());
    AppendFString(replacement, profile.mapUrl);
    replacement.insert(replacement.end(),
                       payload.begin() + static_cast<std::ptrdiff_t>(source.nameFieldEnd),
                       payload.begin() + static_cast<std::ptrdiff_t>(source.end));
    return replacement;
}

std::vector<uint8_t> BuildWelcomeReplacement(const WelcomeRecord& source,
                                             const Profile& profile) {
    std::vector<uint8_t> replacement;
    replacement.reserve((source.gameFieldEnd - source.begin) + profile.mapUrl.size() +
                        profile.gameClassPath.size() + 16u);
    replacement.push_back(0x01);
    AppendFString(replacement, profile.mapUrl);
    AppendFString(replacement, profile.gameClassPath);
    return replacement;
}

struct Edit {
    size_t begin;
    size_t end;
    std::vector<uint8_t> replacement;
};

bool ApplyEdits(std::vector<uint8_t>& payload, std::vector<Edit> edits, std::string& error) {
    std::sort(edits.begin(), edits.end(), [](const Edit& left, const Edit& right) {
        return left.begin > right.begin;
    });
    size_t previousBegin = payload.size();
    for (const Edit& edit : edits) {
        if (edit.begin > edit.end || edit.end > payload.size() || edit.end > previousBegin) {
            error = "bootstrap edits overlap or exceed their decoded payload";
            return false;
        }
        payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(edit.begin),
                      payload.begin() + static_cast<std::ptrdiff_t>(edit.end));
        payload.insert(payload.begin() + static_cast<std::ptrdiff_t>(edit.begin),
                       edit.replacement.begin(), edit.replacement.end());
        previousBegin = edit.begin;
    }
    return true;
}

} // namespace

Profile CanonicalProfile() {
    return {
        "VNTE-Resort",
        "Territories",
        "ROGame.ROGameInfoTerritories",
        kTerritoriesClassIndex,
        kResortGuid,
        kCapturedRoGameObjectBase,
        false,
        false
    };
}

std::optional<ArtifactSelection> ResolveArtifactSelection(
    std::string_view requestedVariant, std::string& error) {
    if (requestedVariant.empty()) {
        error.clear();
        return ArtifactSelection{
            "canonical", "data/replication_bootstrap.bin",
            kCanonicalRoGameLayout, kCanonicalOwningPawnLayout, 0u};
    }
    if (requestedVariant == "installed") {
        error.clear();
        return ArtifactSelection{
            "installed", "data/replication_bootstrap_installed.bin",
            kInstalledRoGameLayout, kInstalledOwningPawnLayout, 5u};
    }

    error = "unsupported replication bootstrap variant; expected exact "
            "'installed' or an unset variable";
    return std::nullopt;
}

bool ValidateReplicationArtifact(const std::vector<uint8_t>& bytes,
                                 std::string_view variant,
                                 std::string& error) {
    const std::string_view expectedSha256 =
        variant == "canonical" ? kCanonicalReplicationBootstrapSha256 :
        variant == "installed" ? kInstalledReplicationBootstrapSha256 :
        std::string_view{};
    if (expectedSha256.empty()) {
        error = "unsupported replication bootstrap variant; expected resolved "
                "'canonical' or 'installed'";
        return false;
    }
    if (bytes.size() != kReplicationBootstrapBytes) {
        error = "replication bootstrap requires the pinned 34199-byte artifact";
        return false;
    }
    const std::string actualSha256 = CryptoUtils::SHA256Hex(bytes);
    if (actualSha256 != expectedSha256) {
        error = actualSha256.empty()
            ? "replication bootstrap requires SHA-256 support"
            : "replication bootstrap SHA-256 does not match variant='" +
                  std::string(variant) + "'";
        return false;
    }
    error.clear();
    return true;
}

std::optional<uint32_t> ResolveGameClassRef(
    const ArtifactSelection& selection, std::string_view gameClassPath) {
    if (gameClassPath == "ROGame.ROGameInfoTerritories") {
        return selection.roGame.territoriesGameClassRef != 0
            ? std::optional<uint32_t>(selection.roGame.territoriesGameClassRef)
            : std::nullopt;
    }
    if (gameClassPath == "ROGame.ROGameInfoSkirmish") {
        return selection.roGame.skirmishGameClassRef != 0
            ? std::optional<uint32_t>(selection.roGame.skirmishGameClassRef)
            : std::nullopt;
    }
    if (gameClassPath == "ROGame.ROGameInfoSupremacy") {
        return selection.roGame.supremacyGameClassRef != 0
            ? std::optional<uint32_t>(selection.roGame.supremacyGameClassRef)
            : std::nullopt;
    }
    return std::nullopt;
}

std::optional<uint32_t> ResolveRoGameContentWireRef(
    const ArtifactSelection& selection, uint32_t canonicalRef) noexcept {
    if (canonicalRef == 0u ||
        canonicalRef >= kStaticObjectRefMax ||
        canonicalRef > (kStaticObjectRefMax - 1u) -
                           selection.owningPawn.roGameContentObjectRefOffset) {
        return std::nullopt;
    }
    return canonicalRef +
           selection.owningPawn.roGameContentObjectRefOffset;
}

bool BuildActorArtifactVariant(const std::vector<uint8_t>& canonical,
    std::string_view variant,
    std::vector<uint8_t>& out,
    std::string& error) {
    out.clear();
    if (variant != "canonical" && variant != "installed") {
        error = "unsupported actor bootstrap variant; expected resolved "
                "'canonical' or 'installed'";
        return false;
    }

    if (canonical.size() != kCanonicalActorBootstrapBytes) {
        error = "actor bootstrap requires the pinned 14537-byte canonical capture";
        return false;
    }
    const std::string sourceSha256 = CryptoUtils::SHA256Hex(canonical);
    if (sourceSha256 != kCanonicalActorBootstrapSha256) {
        error = sourceSha256.empty()
            ? "actor bootstrap requires SHA-256 support"
            : "actor bootstrap source SHA-256 does not match the pinned "
              "canonical capture";
        return false;
    }
    if (variant == "canonical") {
        out = canonical;
        error.clear();
        return true;
    }

    out = canonical;
    for (const ActorBootstrapPatch& patch : kInstalledActorBootstrapPatches) {
        if (out[patch.offset] != patch.expected) {
            out.clear();
            error = "installed actor bootstrap source byte mismatch at offset " +
                    std::to_string(patch.offset);
            return false;
        }
        out[patch.offset] = patch.replacement;
    }

    if (CryptoUtils::SHA256Hex(out) != kInstalledActorBootstrapSha256) {
        out.clear();
        error = "installed actor bootstrap candidate SHA-256 mismatch";
        return false;
    }

    error.clear();
    return true;
}

bool ValidateActorArtifactFraming(const std::vector<uint8_t>& bytes,
                                  std::string& error) {
    constexpr size_t kDescriptorBytes = 10;
    if (bytes.empty()) {
        error = "actor bootstrap descriptor stream is empty";
        return false;
    }

    size_t offset = 0;
    size_t records = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kDescriptorBytes) {
            error = "actor bootstrap has a partial trailing descriptor at offset " +
                    std::to_string(offset);
            return false;
        }
        const uint32_t payloadBits = ReadU32(bytes, offset + 6u);
        const size_t payloadBytes = static_cast<size_t>(payloadBits / 8u) +
            (payloadBits % 8u != 0u ? 1u : 0u);
        offset += kDescriptorBytes;
        if (payloadBytes > bytes.size() - offset) {
            error = "actor bootstrap has a truncated payload at record " +
                    std::to_string(records);
            return false;
        }
        offset += payloadBytes;
        ++records;
    }

    error.clear();
    return true;
}

std::optional<Profile> ResolveExactProfile(std::string_view mapUrl,
                                           std::string_view effectiveMode) {
    Profile profile;
    ModeClass defaultMode{"Territories", "ROGame.ROGameInfoTerritories", kTerritoriesClassIndex};

    if (EqualsInsensitive(mapUrl, "VNTE-Resort")) {
        profile = CanonicalProfile();
    } else if (EqualsInsensitive(mapUrl, "VNTE-CuChi")) {
        profile = {"VNTE-CuChi", "Territories", "ROGame.ROGameInfoTerritories",
                   kTerritoriesClassIndex, kCuChiGuid,
                   kCapturedRoGameObjectBase, true, false};
    } else if (EqualsInsensitive(mapUrl, "VNSU-HueCity")) {
        profile = {"VNSU-HueCity", "Supremacy", "ROGame.ROGameInfoSupremacy",
                   kSupremacyClassIndex, kHueCityGuid, 0, true, false};
        defaultMode = {"Supremacy", "ROGame.ROGameInfoSupremacy", kSupremacyClassIndex};
    } else if (EqualsInsensitive(mapUrl, "VNSK-Compound")) {
        profile = {"VNSK-Compound", "Skirmish", "ROGame.ROGameInfoSkirmish",
                   kSkirmishClassIndex, kCompoundGuid, 0, true, false};
        defaultMode = {"Skirmish", "ROGame.ROGameInfoSkirmish", kSkirmishClassIndex};
    } else {
        return std::nullopt;
    }

    const ModeClass selected = ClassForMode(effectiveMode, defaultMode);
    profile.modeName = selected.mode;
    profile.gameClassPath = selected.path;
    profile.gameClassIndex = selected.index;
    profile.experimental = profile.experimental || selected.index != defaultMode.index;
    return profile;
}

bool HasExactProfile(std::string_view mapUrl) {
    return ResolveExactProfile(mapUrl, {}).has_value();
}

Profile ResolveProfile(std::string_view mapUrl, std::string_view effectiveMode) {
    if (std::optional<Profile> exact = ResolveExactProfile(mapUrl, effectiveMode)) {
        return std::move(*exact);
    }

    Profile fallback = CanonicalProfile();
    fallback.usedFallback = true;
    return fallback;
}

std::optional<GuidBytes> ResolveMapPackageGuid(std::string_view mapUrl) {
    for (const MapGuidEntry& entry : kMapPackageGuids) {
        if (EqualsInsensitive(mapUrl, entry.mapUrl)) return entry.guid;
    }
    return std::nullopt;
}

bool Parse(const std::vector<uint8_t>& bytes, Document& out, std::string& error) {
    Document parsed;
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4u) {
            error = "trailing bytes after bootstrap record stream";
            return false;
        }
        const uint32_t length = ReadU32(bytes, offset);
        offset += 4u;
        if (length == 0u || static_cast<size_t>(length) > bytes.size() - offset) {
            error = "zero-length or truncated bootstrap record";
            return false;
        }
        parsed.records.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    if (parsed.records.empty()) {
        error = "bootstrap stream contains no records";
        return false;
    }
    out = std::move(parsed);
    error.clear();
    return true;
}

std::vector<uint8_t> Serialize(const Document& document) {
    size_t total = 0;
    for (const auto& record : document.records) {
        total += 4u + record.size();
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(total);
    for (const auto& record : document.records) {
        if (record.size() > std::numeric_limits<uint32_t>::max()) return {};
        AppendU32(bytes, static_cast<uint32_t>(record.size()));
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    return bytes;
}

bool FindPackage(const Document& document,
                 std::string_view packageName,
                 PackageRecord& out,
                 std::string& error) {
    bool found = false;
    PackageRecord match;
    for (size_t outer = 0; outer < document.records.size(); ++outer) {
        const auto& payload = document.records[outer];
        size_t offset = 0;
        PackageRecord parsed;
        while (TryParsePackageRecord(payload, offset, parsed)) {
            parsed.outerRecord = outer;
            if (EqualsInsensitive(parsed.name, packageName)) {
                if (found) {
                    error = "bootstrap contains duplicate package record '" +
                            std::string(packageName) + "'";
                    return false;
                }
                found = true;
                match = parsed;
            }
            offset = parsed.end;
        }
    }
    if (!found) {
        error = "bootstrap package record not found: " + std::string(packageName);
        return false;
    }
    out = std::move(match);
    error.clear();
    return true;
}

bool FindWelcome(const Document& document,
                 std::string_view mapUrl,
                 std::string_view gameClassPath,
                 WelcomeRecord& out,
                 std::string& error) {
    bool found = false;
    WelcomeRecord match;
    for (size_t outer = 0; outer < document.records.size(); ++outer) {
        const auto& payload = document.records[outer];
        for (size_t offset = 0; offset < payload.size(); ++offset) {
            if (payload[offset] != 0x01) continue;
            WelcomeRecord parsed;
            parsed.outerRecord = outer;
            parsed.begin = offset;
            parsed.mapFieldBegin = offset + 1u;
            size_t cursor = parsed.mapFieldBegin;
            if (!ReadFString(payload, cursor, parsed.mapUrl, cursor)) continue;
            parsed.mapFieldEnd = cursor;
            parsed.gameFieldBegin = cursor;
            if (!ReadFString(payload, cursor, parsed.gameClassPath, cursor)) continue;
            parsed.gameFieldEnd = cursor;
            if (!EqualsInsensitive(parsed.mapUrl, mapUrl) ||
                !EqualsInsensitive(parsed.gameClassPath, gameClassPath)) {
                continue;
            }
            if (found) {
                error = "bootstrap contains duplicate matching Welcome records";
                return false;
            }
            found = true;
            match = std::move(parsed);
        }
    }
    if (!found) {
        error = "bootstrap Welcome record not found for " + std::string(mapUrl) +
                " / " + std::string(gameClassPath);
        return false;
    }
    out = std::move(match);
    error.clear();
    return true;
}

bool BuildVariant(const Document& canonical,
                  const Profile& profile,
                  Document& out,
                  std::string& error) {
    const Profile sourceProfile = CanonicalProfile();
    PackageRecord sourcePackage;
    WelcomeRecord sourceWelcome;
    if (!FindPackage(canonical, sourceProfile.mapUrl, sourcePackage, error)) return false;
    if (sourcePackage.guid != sourceProfile.mapPackageGuid) {
        error = "canonical VNTE-Resort package GUID does not match the pinned retail capture";
        return false;
    }
    if (!FindWelcome(canonical, sourceProfile.mapUrl, sourceProfile.gameClassPath,
                     sourceWelcome, error)) {
        return false;
    }

    Document rebuilt = canonical;
    const auto& packagePayload = canonical.records[sourcePackage.outerRecord];

    std::vector<std::vector<Edit>> edits(rebuilt.records.size());
    edits[sourcePackage.outerRecord].push_back({
        sourcePackage.begin,
        sourcePackage.end,
        BuildPackageReplacement(packagePayload, sourcePackage, profile)
    });
    edits[sourceWelcome.outerRecord].push_back({
        sourceWelcome.begin,
        sourceWelcome.gameFieldEnd,
        BuildWelcomeReplacement(sourceWelcome, profile)
    });

    for (size_t i = 0; i < edits.size(); ++i) {
        if (!edits[i].empty() && !ApplyEdits(rebuilt.records[i], std::move(edits[i]), error)) {
            return false;
        }
    }

    out = std::move(rebuilt);
    error.clear();
    return true;
}

} // namespace RetailBootstrap
