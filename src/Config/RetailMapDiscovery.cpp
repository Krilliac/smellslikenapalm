// src/Config/RetailMapDiscovery.cpp

#include "Config/RetailMapDiscovery.h"

#include "Utils/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSteamAppId = "418460";
constexpr std::uintmax_t kMaximumLibraryFoldersBytes = 4u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumAppManifestBytes = 1024u * 1024u;
constexpr size_t kMaximumKeyValuesTokens = 65536u;
constexpr size_t kMaximumKeyValuesTokenBytes = 32768u;

struct SupportedRetailMap {
    std::string_view mapName;
    std::string_view displayName;
    std::string_view defaultMode;
};

// Keep this list intentionally narrower than RetailBootstrap's package-GUID
// table. These are the only maps with exact bootstrap URL/game-class profiles;
// discovering any other installed .roe would make it selectable through the
// legacy Resort fallback.
constexpr std::array<SupportedRetailMap, 4> kSupportedRetailMaps{{
    {"VNTE-Resort", "Resort", "Territories"},
    {"VNTE-CuChi", "Cu Chi", "Territories"},
    {"VNSU-HueCity", "Hue City", "Supremacy"},
    {"VNSK-Compound", "Compound", "Skirmish"},
}};

enum class KeyValuesTokenKind {
    Text,
    OpenBrace,
    CloseBrace,
};

struct KeyValuesToken {
    KeyValuesTokenKind kind = KeyValuesTokenKind::Text;
    std::string text;
};

using ScalarPairs = std::vector<std::pair<std::string, std::string>>;

bool EqualsInsensitive(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

std::string TrimAscii(std::string value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (first >= last) return {};
    return std::string(first, last);
}

bool ReadBoundedText(const fs::path& path, std::uintmax_t maximumBytes,
                     std::string& output)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > maximumBytes) return false;

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;

    output.assign(static_cast<size_t>(size), '\0');
    if (size != 0u) {
        input.read(output.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) return false;
    }
    return output.find('\0') == std::string::npos;
}

bool TokenizeKeyValues(std::string_view input,
                       std::vector<KeyValuesToken>& tokens)
{
    tokens.clear();
    size_t cursor = 0;
    while (cursor < input.size()) {
        const unsigned char current = static_cast<unsigned char>(input[cursor]);
        if (std::isspace(current) != 0) {
            ++cursor;
            continue;
        }
        if (input[cursor] == '/' && cursor + 1u < input.size() &&
            input[cursor + 1u] == '/') {
            cursor += 2u;
            while (cursor < input.size() && input[cursor] != '\n') ++cursor;
            continue;
        }
        if (input[cursor] == '{' || input[cursor] == '}') {
            tokens.push_back({input[cursor] == '{'
                                  ? KeyValuesTokenKind::OpenBrace
                                  : KeyValuesTokenKind::CloseBrace,
                              {}});
            ++cursor;
        } else {
            std::string value;
            if (input[cursor] == '"') {
                ++cursor;
                bool closed = false;
                while (cursor < input.size()) {
                    const char ch = input[cursor];
                    if (ch == '"') {
                        ++cursor;
                        closed = true;
                        break;
                    }
                    if (ch == '\\' && cursor + 1u < input.size() &&
                        (input[cursor + 1u] == '\\' ||
                         input[cursor + 1u] == '"')) {
                        value.push_back(input[cursor + 1u]);
                        cursor += 2u;
                    } else {
                        value.push_back(ch);
                        ++cursor;
                    }
                    if (value.size() > kMaximumKeyValuesTokenBytes) return false;
                }
                if (!closed) return false;
            } else {
                const size_t begin = cursor;
                while (cursor < input.size()) {
                    const unsigned char ch =
                        static_cast<unsigned char>(input[cursor]);
                    if (std::isspace(ch) != 0 || input[cursor] == '{' ||
                        input[cursor] == '}') {
                        break;
                    }
                    ++cursor;
                }
                if (cursor == begin ||
                    cursor - begin > kMaximumKeyValuesTokenBytes) {
                    return false;
                }
                value.assign(input.substr(begin, cursor - begin));
            }
            tokens.push_back({KeyValuesTokenKind::Text, std::move(value)});
        }

        if (tokens.size() > kMaximumKeyValuesTokens) return false;
    }
    return true;
}

bool ExtractScalarPairs(std::string_view input, ScalarPairs& pairs)
{
    std::vector<KeyValuesToken> tokens;
    if (!TokenizeKeyValues(input, tokens)) return false;

    pairs.clear();
    size_t depth = 0;
    size_t cursor = 0;
    while (cursor < tokens.size()) {
        const KeyValuesToken& token = tokens[cursor];
        if (token.kind == KeyValuesTokenKind::CloseBrace) {
            if (depth == 0u) return false;
            --depth;
            ++cursor;
            continue;
        }
        if (token.kind != KeyValuesTokenKind::Text ||
            cursor + 1u >= tokens.size()) {
            return false;
        }

        const KeyValuesToken& value = tokens[cursor + 1u];
        if (value.kind == KeyValuesTokenKind::Text) {
            pairs.emplace_back(token.text, value.text);
            cursor += 2u;
        } else if (value.kind == KeyValuesTokenKind::OpenBrace) {
            ++depth;
            cursor += 2u;
        } else {
            return false;
        }
    }
    return depth == 0u;
}

bool FindConsistentValue(const ScalarPairs& pairs, std::string_view key,
                         std::string& value)
{
    bool found = false;
    std::string selected;
    for (const auto& [candidateKey, candidateValue] : pairs) {
        if (!EqualsInsensitive(candidateKey, key)) continue;
        if (!found) {
            selected = candidateValue;
            found = true;
        } else if (selected != candidateValue) {
            return false;
        }
    }
    if (!found) return false;
    value = std::move(selected);
    return true;
}

bool IsNumericKey(std::string_view value)
{
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

bool PathComponentEquals(const fs::path& lhs, const fs::path& rhs)
{
#ifdef _WIN32
    return EqualsInsensitive(lhs.string(), rhs.string());
#else
    return lhs == rhs;
#endif
}

bool IsContainedPath(const fs::path& root, const fs::path& candidate)
{
    const fs::path normalizedRoot = root.lexically_normal();
    const fs::path normalizedCandidate = candidate.lexically_normal();
    auto rootPart = normalizedRoot.begin();
    auto candidatePart = normalizedCandidate.begin();
    while (rootPart != normalizedRoot.end()) {
        if (candidatePart == normalizedCandidate.end() ||
            !PathComponentEquals(*rootPart, *candidatePart)) {
            return false;
        }
        ++rootPart;
        ++candidatePart;
    }
    return true;
}

std::optional<fs::path> CanonicalDirectory(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_directory(path, ec) || ec) return std::nullopt;
    fs::path canonical = fs::canonical(path, ec);
    if (ec || !fs::is_directory(canonical, ec) || ec) return std::nullopt;
    return canonical;
}

std::optional<fs::path> CanonicalRegularFile(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return std::nullopt;
    fs::path canonical = fs::canonical(path, ec);
    if (ec || !fs::is_regular_file(canonical, ec) || ec) return std::nullopt;
    return canonical;
}

std::string PathIdentity(const fs::path& path)
{
    std::string identity = path.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
#endif
    return identity;
}

void AddCanonicalDirectory(const fs::path& candidate,
                           std::vector<fs::path>& directories,
                           std::set<std::string>& identities)
{
    const std::optional<fs::path> canonical = CanonicalDirectory(candidate);
    if (!canonical.has_value()) return;
    if (identities.insert(PathIdentity(*canonical)).second) {
        directories.push_back(*canonical);
    }
}

bool IsSafeInstallDirectoryName(const std::string& value)
{
    if (value.empty() || value == "." || value == ".." ||
        value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos) {
        return false;
    }
    const fs::path path(value);
    return !path.is_absolute() && !path.has_root_path() &&
           std::distance(path.begin(), path.end()) == 1;
}

void DiscoverDeclaredLibraries(const fs::path& steamRoot,
                               std::vector<fs::path>& libraries,
                               std::set<std::string>& identities)
{
    AddCanonicalDirectory(steamRoot, libraries, identities);

    const fs::path foldersPath =
        steamRoot / "steamapps" / "libraryfolders.vdf";
    const std::optional<fs::path> foldersFile =
        CanonicalRegularFile(foldersPath);
    if (!foldersFile.has_value() ||
        !IsContainedPath(steamRoot, *foldersFile)) {
        return;
    }

    std::string text;
    ScalarPairs pairs;
    if (!ReadBoundedText(*foldersFile, kMaximumLibraryFoldersBytes, text) ||
        !ExtractScalarPairs(text, pairs)) {
        Logger::Warn("RetailMapDiscovery: refusing malformed Steam library file: %s",
                     foldersFile->string().c_str());
        return;
    }

    for (const auto& [key, value] : pairs) {
        if (!EqualsInsensitive(key, "path") && !IsNumericKey(key)) continue;
        const fs::path declared(value);
        if (!declared.is_absolute()) continue;
        AddCanonicalDirectory(declared, libraries, identities);
    }
}

std::optional<fs::path> ResolveInstalledMapsDirectory(
    const fs::path& libraryRoot)
{
    const fs::path manifestPath =
        libraryRoot / "steamapps" / "appmanifest_418460.acf";
    const std::optional<fs::path> manifest =
        CanonicalRegularFile(manifestPath);
    if (!manifest.has_value() || !IsContainedPath(libraryRoot, *manifest)) {
        return std::nullopt;
    }

    std::string text;
    ScalarPairs pairs;
    if (!ReadBoundedText(*manifest, kMaximumAppManifestBytes, text) ||
        !ExtractScalarPairs(text, pairs)) {
        Logger::Warn("RetailMapDiscovery: refusing malformed app manifest: %s",
                     manifest->string().c_str());
        return std::nullopt;
    }

    std::string appId;
    std::string installDirectory;
    if (!FindConsistentValue(pairs, "appid", appId) ||
        TrimAscii(appId) != kSteamAppId ||
        !FindConsistentValue(pairs, "installdir", installDirectory)) {
        return std::nullopt;
    }
    installDirectory = TrimAscii(std::move(installDirectory));
    if (!IsSafeInstallDirectoryName(installDirectory)) return std::nullopt;

    const std::optional<fs::path> commonDirectory =
        CanonicalDirectory(libraryRoot / "steamapps" / "common");
    if (!commonDirectory.has_value() ||
        !IsContainedPath(libraryRoot, *commonDirectory)) {
        return std::nullopt;
    }

    const std::optional<fs::path> installRoot =
        CanonicalDirectory(*commonDirectory / installDirectory);
    if (!installRoot.has_value() ||
        !IsContainedPath(*commonDirectory, *installRoot)) {
        return std::nullopt;
    }

    const std::optional<fs::path> mapsRoot = CanonicalDirectory(
        *installRoot / "ROGame" / "BrewedPC" / "Maps");
    if (!mapsRoot.has_value() || !IsContainedPath(*installRoot, *mapsRoot)) {
        return std::nullopt;
    }
    return mapsRoot;
}

const SupportedRetailMap* SupportedMapForFileName(std::string_view fileName)
{
    for (const SupportedRetailMap& map : kSupportedRetailMaps) {
        const std::string expected = std::string(map.mapName) + ".roe";
        if (EqualsInsensitive(fileName, expected)) return &map;
    }
    return nullptr;
}

void ScanMapsDirectory(
    const fs::path& mapsRoot,
    std::map<std::string, std::vector<fs::path>>& packageCandidates)
{
    std::error_code ec;
    fs::recursive_directory_iterator cursor(
        mapsRoot, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) return;

    while (cursor != end) {
        const fs::directory_entry entry = *cursor;
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && !entryError) {
            const SupportedRetailMap* supported =
                SupportedMapForFileName(entry.path().filename().string());
            if (supported != nullptr) {
                const std::optional<fs::path> canonical =
                    CanonicalRegularFile(entry.path());
                if (canonical.has_value() &&
                    IsContainedPath(mapsRoot, *canonical)) {
                    auto& candidates =
                        packageCandidates[std::string(supported->mapName)];
                    const std::string identity = PathIdentity(*canonical);
                    const bool duplicate = std::any_of(
                        candidates.begin(), candidates.end(),
                        [&](const fs::path& existing) {
                            return PathIdentity(existing) == identity;
                        });
                    if (!duplicate) candidates.push_back(*canonical);
                }
            }
        }

        cursor.increment(ec);
        if (ec) ec.clear();
    }
}

#ifdef _WIN32
std::optional<std::wstring> ReadRegistryString(
    HKEY hive, const wchar_t* subkey, const wchar_t* valueName,
    REGSAM registryView)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, subkey, 0, KEY_QUERY_VALUE | registryView, &key) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    const LONG sizeResult =
        RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (sizeResult != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0u ||
        bytes > 64u * 1024u) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1u, L'\0');
    DWORD readBytes = bytes;
    const LONG readResult = RegQueryValueExW(
        key, valueName, nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer.data()), &readBytes);
    RegCloseKey(key);
    if (readResult != ERROR_SUCCESS) return std::nullopt;

    std::wstring value(buffer.data());
    if (type == REG_EXPAND_SZ) {
        const DWORD expandedSize =
            ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expandedSize == 0u || expandedSize > 32768u) return std::nullopt;
        std::wstring expanded(expandedSize, L'\0');
        const DWORD written = ExpandEnvironmentStringsW(
            value.c_str(), expanded.data(), expandedSize);
        if (written == 0u || written > expandedSize) {
            return std::nullopt;
        }
        // The successful count includes the trailing NUL. Keep the expansion
        // self-owned and explicitly bounded; the required size can also grow
        // if the environment changes between the two API calls.
        expanded.resize(written - 1u);
        value = std::move(expanded);
    }
    return value.empty() ? std::nullopt
                         : std::optional<std::wstring>(std::move(value));
}

void AppendRegistrySteamRoots(std::vector<fs::path>& roots)
{
    struct RegistryLocation {
        HKEY hive;
        const wchar_t* subkey;
        REGSAM view;
    };
    const RegistryLocation locations[] = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0},
        {HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", KEY_WOW64_64KEY},
        {HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", KEY_WOW64_32KEY},
    };
    constexpr const wchar_t* values[] = {
        L"SteamPath", L"InstallPath", L"SteamExe"};

    for (const RegistryLocation& location : locations) {
        for (const wchar_t* valueName : values) {
            const std::optional<std::wstring> value = ReadRegistryString(
                location.hive, location.subkey, valueName, location.view);
            if (!value.has_value()) continue;
            fs::path path(*value);
            if (EqualsInsensitive(fs::path(valueName).string(), "SteamExe") ||
                EqualsInsensitive(path.filename().string(), "steam.exe")) {
                path = path.parent_path();
            }
            roots.push_back(std::move(path));
        }
    }
}
#endif

void AppendEnvironmentSteamRoots(std::vector<fs::path>& roots)
{
#ifdef _WIN32
    for (const char* variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const char* value = std::getenv(variable);
        if (value != nullptr && *value != '\0') {
            roots.emplace_back(fs::path(value) / "Steam");
        }
    }
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".steam" / "steam");
        roots.emplace_back(fs::path(home) / ".local" / "share" / "Steam");
    }
#endif
}

std::vector<fs::path> PlatformSteamRoots()
{
    std::vector<fs::path> roots;
#ifdef _WIN32
    AppendRegistrySteamRoots(roots);
#endif
    AppendEnvironmentSteamRoots(roots);
    return roots;
}

} // namespace

namespace RetailMapDiscovery {

std::vector<RetailMapPackage> Discover()
{
    return DiscoverFromSteamRoots(PlatformSteamRoots());
}

std::vector<RetailMapPackage> DiscoverFromSteamRoots(
    const std::vector<std::filesystem::path>& steamRoots)
{
    std::vector<fs::path> canonicalSteamRoots;
    std::set<std::string> steamIdentities;
    for (const fs::path& root : steamRoots) {
        AddCanonicalDirectory(root, canonicalSteamRoots, steamIdentities);
    }

    std::vector<fs::path> libraryRoots;
    std::set<std::string> libraryIdentities;
    for (const fs::path& root : canonicalSteamRoots) {
        DiscoverDeclaredLibraries(root, libraryRoots, libraryIdentities);
    }

    std::vector<fs::path> mapsDirectories;
    std::set<std::string> mapsIdentities;
    for (const fs::path& library : libraryRoots) {
        const std::optional<fs::path> maps =
            ResolveInstalledMapsDirectory(library);
        if (maps.has_value() &&
            mapsIdentities.insert(PathIdentity(*maps)).second) {
            mapsDirectories.push_back(*maps);
        }
    }

    std::map<std::string, std::vector<fs::path>> packageCandidates;
    for (const fs::path& maps : mapsDirectories) {
        ScanMapsDirectory(maps, packageCandidates);
    }

    std::vector<RetailMapPackage> discovered;
    for (const SupportedRetailMap& supported : kSupportedRetailMaps) {
        const auto candidate =
            packageCandidates.find(std::string(supported.mapName));
        if (candidate == packageCandidates.end()) continue;
        if (candidate->second.size() != 1u) {
            Logger::Warn(
                "RetailMapDiscovery: refusing ambiguous package for %s (%zu candidates)",
                std::string(supported.mapName).c_str(),
                candidate->second.size());
            continue;
        }

        discovered.push_back({std::string(supported.mapName),
                              std::string(supported.displayName),
                              std::string(supported.defaultMode),
                              candidate->second.front()});
    }
    return discovered;
}

} // namespace RetailMapDiscovery
