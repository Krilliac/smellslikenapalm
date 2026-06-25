// src/Network/URLOptions.h
//
// Parser for UE3 FURL-style connect/options strings sent by the client in
// NMT_Login (and related control messages). A typical string looks like:
//
//   MapName?Name=PlayerName?Team=1?Character=...?Password=secret?SpectatorOnly=1?SplitJoin
//
// or with a full map path / portal prefix:
//
//   /Game/Maps/VNTE-Map.VNTE-Map?Name=Foo?Team=0
//
// Semantics mirror Engine/GameInfo.uc (GrabOption / GetKeyValue / ParseOption /
// HasOption):
//   - The portion before the first '?' is the map / URL.
//   - Options are separated by '?'. Each option is "Key=Value" or a bare "Key"
//     flag (Value == "").
//   - Key lookups are case-insensitive (UnrealScript string '==' is
//     case-insensitive).
//   - On duplicate keys, the FIRST occurrence wins (ParseOption returns on the
//     first match) -- see GameInfo.uc:530-549.
//   - Values are kept RAW; UE3 does not URL-decode option values.
//
// This project does not wrap engine code in a namespace, so neither does this.

#pragma once

#include <string>
#include <vector>
#include <utility>

class URLOptions {
public:
    URLOptions() = default;

    // Parse a UE3 FURL-style string. Never throws; bad/empty input yields an
    // empty map and no options.
    static URLOptions Parse(const std::string& url);

    // The portion of the URL before the first '?' (map name or map path/portal).
    const std::string& Map() const { return m_map; }

    // Case-insensitive presence test (matches HasOption).
    bool Has(const std::string& key) const;

    // Case-insensitive value lookup (matches ParseOption / GetOption). Returns
    // def when the key is absent. A bare flag ("Key" with no '=') has value "".
    std::string Get(const std::string& key, const std::string& def = "") const;

    // Integer accessor (matches GetIntOption: returns def if key absent, else
    // int(value); non-numeric values parse to 0 like UnrealScript int()).
    int GetInt(const std::string& key, int def) const;

    // Bool accessor. UE3 treats a value of "1" as true (e.g. SpectatorOnly==1).
    // A bare flag (present, empty value) is treated as true. Absent -> def.
    bool GetBool(const std::string& key, bool def = false) const;

    // Raw, ordered key/value pairs as parsed (keys preserve original case).
    const std::vector<std::pair<std::string, std::string>>& Raw() const { return m_options; }

    // ---- Convenience accessors for the well-known RS2/UE3 login options ----

    // "Name" -- player name (GameInfo.uc:1031). Empty when not supplied.
    std::string PlayerName() const { return Get("Name", ""); }

    // "Team" -- desired team index. Default 255 == "no preference"
    // (GameInfo.uc:1032: GetIntOption(Options, "Team", 255)).
    int Team() const { return GetInt("Team", 255); }

    // "Password" -- server/game password (GameInfo.uc:1033).
    std::string Password() const { return Get("Password", ""); }

    // "Character" -- requested character (GameInfo.uc:1104).
    std::string Character() const { return Get("Character", ""); }

    // "SpectatorOnly" -- true when value == "1" (GameInfo.uc:1030).
    bool SpectatorOnly() const { return GetBool("SpectatorOnly", false); }

    // "SplitJoin" -- bare flag for split-screen join (LocalPlayer::SendSplitJoin).
    bool SplitJoin() const { return Has("SplitJoin"); }

private:
    std::string m_map;
    std::vector<std::pair<std::string, std::string>> m_options;
};
