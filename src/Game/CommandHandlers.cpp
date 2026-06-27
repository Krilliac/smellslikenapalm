// src/Game/CommandHandlers.cpp — built-in command handler implementations.
//
// This file holds CommandManager::RegisterBuiltins() plus every built-in
// command's logic. It is split from CommandManager.cpp so the dispatch engine
// (parsing/permission/help) stays a separate, single-purpose unit. All handlers
// reach game state exclusively through GameServer subsystem accessors and reply
// through CommandContext::Reply — they never touch transports directly.

#include "Game/CommandManager.h"

#include "Game/GameServer.h"
#include "Game/AdminManager.h"
#include "Game/PlayerManager.h"
#include "Game/Player.h"
#include "Game/MapManager.h"
#include "Game/MapVoteManager.h"
#include "Game/WorkshopManager.h"
#include "Game/ModManager.h"
#include "Game/MutatorManager.h"
#include "Config/MapConfig.h"
#include "Config/ServerConfig.h"
#include "Config/ConfigManager.h"
#include "Network/ClientConnection.h"
#include "Math/Vector3.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/CrashHandler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace {

// Resolve a player reference (SteamID or numeric client id) to a client id.
// Returns CommandManager::INVALID_CLIENT_ID if no matching connected player.
uint32_t ResolveTarget(GameServer* server, const std::string& token)
{
    if (!server) return CommandManager::INVALID_CLIENT_ID;
    // SteamID match first — that is the canonical reference in the docs/admin list.
    uint32_t id = server->FindClientBySteamID(token);
    if (id != CommandManager::INVALID_CLIENT_ID) return id;
    // Fall back to a raw client id if it names a live connection.
    if (auto parsed = StringUtils::ToInt(token)) {
        if (*parsed >= 0) {
            uint32_t cid = static_cast<uint32_t>(*parsed);
            if (server->GetClientConnection(cid)) return cid;
        }
    }
    return CommandManager::INVALID_CLIENT_ID;
}

bool ParseFloat(const std::string& s, float& out)
{
    try { size_t pos = 0; out = std::stof(s, &pos); return pos == s.size(); }
    catch (...) {
        // Don't lose the failure silently: the caller turns this into an
        // "invalid argument" reply, but record the offending token for anyone
        // debugging a rejected command. Gated at Debug so it never floods.
        Logger::Debug("[CommandHandlers::ParseFloat] Not a float: '%s'", s.c_str());
        return false;
    }
}

} // namespace

void CommandManager::RegisterBuiltins()
{
    // ---------------------------------------------------------------------
    // Automation / health — Player level, designed for tooling and AI probes.
    // ---------------------------------------------------------------------
    Register({"ping", {}, CommandLevel::Player, CommandCategory::Automation,
        "ping", "Liveness probe; replies 'pong'.",
        [](CommandContext& ctx) { ctx.Reply("pong"); return true; }});

    Register({"version", {"ver"}, CommandLevel::Player, CommandCategory::Automation,
        "version", "Report the server software version.",
        [](CommandContext& ctx) { ctx.Reply("RS2V Custom Server v1.0.0"); return true; }});

    Register({"echo", {}, CommandLevel::Player, CommandCategory::Automation,
        "echo <text...>", "Echo arguments back (connectivity/round-trip test).",
        [](CommandContext& ctx) { ctx.Reply(StringUtils::Join(ctx.args, " ")); return true; }});

    Register({"query", {"q"}, CommandLevel::Helper, CommandCategory::Automation,
        "query", "Machine-readable server snapshot (key=value lines).",
        [](CommandContext& ctx) {
            GameServer* s = ctx.server;
            auto sc = s ? s->GetServerConfig() : nullptr;
            size_t players = s ? s->GetAllConnections().size() : 0;
            std::string map = (s && s->GetMapManager()) ? s->GetMapManager()->GetCurrentMapName() : "";
            ctx.Reply("name=" + (sc ? sc->GetServerName() : std::string("RS2V")));
            ctx.Reply("map=" + map);
            ctx.Reply("players=" + std::to_string(players));
            ctx.Reply("maxplayers=" + std::to_string(sc ? sc->GetMaxPlayers() : 0));
            ctx.Reply("tickrate=" + std::to_string(s ? s->GetTickRate() : 0));
            ctx.Reply("timescale=" + std::to_string(s ? s->GetTimeScale() : 1.0f));
            ctx.Reply("nonfatal_exceptions=" + std::to_string(rs2v::NonFatalExceptionCount()));
            return true;
        }});

    // ---------------------------------------------------------------------
    // Server management
    // ---------------------------------------------------------------------
    Register({"status", {"svstatus"}, CommandLevel::Helper, CommandCategory::Server,
        "status", "Show server name, map, player count, tick rate.",
        [](CommandContext& ctx) {
            GameServer* s = ctx.server;
            auto sc = s ? s->GetServerConfig() : nullptr;
            size_t players = s ? s->GetAllConnections().size() : 0;
            std::string map = (s && s->GetMapManager()) ? s->GetMapManager()->GetCurrentMapName() : "(none)";
            std::ostringstream oss;
            oss << "Server: " << (sc ? sc->GetServerName() : "RS2V Custom Server")
                << " | Map: " << map
                << " | Players: " << players << "/" << (sc ? sc->GetMaxPlayers() : 0)
                << " | Tick: " << (s ? s->GetTickRate() : 0) << "Hz"
                << " | TimeScale: " << (s ? s->GetTimeScale() : 1.0f);
            ctx.Reply(oss.str());
            return true;
        }});

    Register({"players", {"playerlist", "list"}, CommandLevel::Helper, CommandCategory::Server,
        "players", "List connected players (id, SteamID, name, team, ping, K/D/score).",
        [](CommandContext& ctx) {
            GameServer* s = ctx.server;
            if (!s) { ctx.Reply("Server unavailable."); return false; }
            auto* pm = s->GetPlayerManager();
            auto conns = s->GetAllConnections();
            ctx.Reply("Players (" + std::to_string(conns.size()) + "):");
            for (const auto& c : conns) {
                if (!c) continue;
                uint32_t cid = c->GetClientId();
                std::ostringstream oss;
                oss << "  #" << cid
                    << " " << c->GetSteamID()
                    << " \"" << c->GetPlayerName() << "\""
                    << " team=" << c->GetTeamId()
                    << " ping=" << c->GetPing() << "ms";
                if (pm) {
                    oss << " K/D=" << pm->GetPlayerKills(cid) << "/" << pm->GetPlayerDeaths(cid)
                        << " score=" << pm->GetPlayerScore(cid);
                }
                ctx.Reply(oss.str());
            }
            return true;
        }});

    Register({"reload", {}, CommandLevel::Admin, CommandCategory::Server,
        "reload [section]", "Reload configuration from disk.",
        [](CommandContext& ctx) {
            GameServer* s = ctx.server;
            auto cfg = s ? s->GetConfigManager() : nullptr;
            if (!cfg) { ctx.Reply("ConfigManager unavailable."); return false; }
            if (cfg->ReloadConfiguration()) {
                ctx.Reply("Configuration reloaded.");
                return true;
            }
            ctx.Reply("Failed to reload configuration.");
            return false;
        }});

    Register({"tickrate", {}, CommandLevel::Admin, CommandCategory::Server,
        "tickrate <hz>", "Set the server tick rate (1-256 Hz).",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: tickrate <hz>"); return false; }
            auto n = StringUtils::ToInt(ctx.args[0]);
            if (!n) { ctx.Reply("Invalid tick rate: " + ctx.args[0]); return false; }
            if (ctx.server && ctx.server->SetTickRate(*n)) {
                ctx.Reply("Tick rate set to " + std::to_string(*n) + " Hz.");
                return true;
            }
            ctx.Reply("Tick rate must be between 1 and 256.");
            return false;
        }});

    Register({"timescale", {}, CommandLevel::Dev, CommandCategory::Dev,
        "timescale <scale>", "Set simulation speed (0.05-8.0; 1.0 = normal).",
        [](CommandContext& ctx) {
            float scale = 1.0f;
            if (ctx.args.empty() || !ParseFloat(ctx.args[0], scale)) {
                ctx.Reply("Usage: timescale <scale>"); return false;
            }
            if (ctx.server) ctx.server->SetTimeScale(scale);
            ctx.Reply("Time scale set to " + std::to_string(scale));
            return true;
        }});

    Register({"shutdown", {"quit", "exit"}, CommandLevel::Admin, CommandCategory::Server,
        "shutdown", "Gracefully stop the server.",
        [](CommandContext& ctx) {
            ctx.Reply("Server shutting down...");
            if (ctx.server) ctx.server->RequestShutdown();
            return true;
        }});

    Register({"regen", {"regenhandlers"}, CommandLevel::Dev, CommandCategory::Dev,
        "regen", "Regenerate and reload packet handlers.",
        [](CommandContext& ctx) {
            if (!ctx.server) { ctx.Reply("Server unavailable."); return false; }
            ctx.server->Cmd_RegenHandlers(ctx.args);
            ctx.Reply("Packet handler regeneration triggered.");
            return true;
        }});

    Register({"spawnbot", {"bot"}, CommandLevel::Dev, CommandCategory::Dev,
        "spawnbot [count]", "Spawn AI bots (not yet implemented).",
        [](CommandContext& ctx) {
            // STUB: no AI bot subsystem exists yet; surface honestly rather than
            // pretend success. Revisit when a BotManager lands.
            ctx.Reply("spawnbot: no AI bot subsystem is implemented on this server.");
            return false;
        }});

    // ---------------------------------------------------------------------
    // Communication
    // ---------------------------------------------------------------------
    Register({"broadcast", {"bc", "say"}, CommandLevel::Moderator, CommandCategory::Communication,
        "broadcast <message...>", "Send a server-wide chat announcement.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: broadcast <message...>"); return false; }
            std::string msg = StringUtils::Join(ctx.args, " ");
            if (ctx.server) ctx.server->BroadcastChatMessage("[ADMIN] " + msg);
            ctx.Reply("Broadcast sent.");
            return true;
        }});

    // ---------------------------------------------------------------------
    // Player management
    // ---------------------------------------------------------------------
    Register({"kick", {}, CommandLevel::Moderator, CommandCategory::Player,
        "kick <steamId|#id> [reason...]", "Disconnect a player.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: kick <steamId|#id> [reason...]"); return false; }
            auto* admin = ctx.server ? ctx.server->GetAdminManager() : nullptr;
            if (!admin) { ctx.Reply("AdminManager unavailable."); return false; }
            auto conn = ctx.server->GetClientConnection(ResolveTarget(ctx.server, ctx.args[0]));
            const std::string steamId = conn ? conn->GetSteamID() : ctx.args[0];
            bool ok = admin->KickPlayer(ctx.clientId, steamId);
            ctx.Reply(ok ? ("Kicked " + steamId) : ("Player not found: " + ctx.args[0]));
            return ok;
        }});

    Register({"ban", {}, CommandLevel::Admin, CommandCategory::Player,
        "ban <steamId|#id> <minutes|permanent> [reason...]", "Ban a player for a duration.",
        [](CommandContext& ctx) {
            if (ctx.args.size() < 2) { ctx.Reply("Usage: ban <steamId|#id> <minutes|permanent> [reason...]"); return false; }
            auto* admin = ctx.server ? ctx.server->GetAdminManager() : nullptr;
            if (!admin) { ctx.Reply("AdminManager unavailable."); return false; }
            auto conn = ctx.server->GetClientConnection(ResolveTarget(ctx.server, ctx.args[0]));
            const std::string steamId = conn ? conn->GetSteamID() : ctx.args[0];
            int minutes;
            if (StringUtils::EqualsIgnoreCase(ctx.args[1], "permanent") ||
                StringUtils::EqualsIgnoreCase(ctx.args[1], "perm")) {
                minutes = 60 * 24 * 365 * 100; // ~a century == effectively permanent
            } else if (auto n = StringUtils::ToInt(ctx.args[1]); n && *n > 0) {
                minutes = *n;
            } else {
                ctx.Reply("Invalid duration: " + ctx.args[1]); return false;
            }
            std::string reason;
            if (ctx.args.size() > 2) {
                std::vector<std::string> rest(ctx.args.begin() + 2, ctx.args.end());
                reason = StringUtils::Join(rest, " ");
            }
            bool ok = admin->BanPlayer(ctx.clientId, steamId, minutes, reason);
            ctx.Reply(ok ? ("Banned " + steamId + " for " + std::to_string(minutes) + " min")
                         : ("Ban failed for " + steamId));
            return ok;
        }});

    Register({"unban", {}, CommandLevel::Admin, CommandCategory::Player,
        "unban <steamId>", "Remove a ban.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: unban <steamId>"); return false; }
            auto* admin = ctx.server ? ctx.server->GetAdminManager() : nullptr;
            if (!admin) { ctx.Reply("AdminManager unavailable."); return false; }
            bool ok = admin->Unban(ctx.args[0]);
            ctx.Reply(ok ? ("Unbanned " + ctx.args[0]) : ("No active ban for " + ctx.args[0]));
            return ok;
        }});

    Register({"banlist", {"bans"}, CommandLevel::Moderator, CommandCategory::Player,
        "banlist", "List active bans with remaining time.",
        [](CommandContext& ctx) {
            auto* admin = ctx.server ? ctx.server->GetAdminManager() : nullptr;
            if (!admin) { ctx.Reply("AdminManager unavailable."); return false; }
            auto bans = admin->GetActiveBans();
            ctx.Reply("Active bans (" + std::to_string(bans.size()) + "):");
            for (const auto& b : bans) {
                std::string when = b.permanent
                    ? "permanent"
                    : ("expires in ~" + std::to_string(b.remainingSeconds / 60) + " min");
                std::string why = b.reason.empty() ? "" : ("  \"" + b.reason + "\"");
                ctx.Reply("  " + b.steamId + "  " + when + why);
            }
            return true;
        }});

    Register({"admins", {"adminlist"}, CommandLevel::Helper, CommandCategory::Player,
        "admins", "List configured admins and their permission levels.",
        [](CommandContext& ctx) {
            auto* admin = ctx.server ? ctx.server->GetAdminManager() : nullptr;
            if (!admin) { ctx.Reply("AdminManager unavailable."); return false; }
            auto list = admin->GetAdminList();
            ctx.Reply("Admins (" + std::to_string(list.size()) + "):");
            for (const auto& [steamId, level] : list) {
                ctx.Reply("  " + steamId + "  level " + std::to_string(level) +
                          " (" + LevelName(LevelFromInt(level)) + ")");
            }
            return true;
        }});

    Register({"slay", {"kill"}, CommandLevel::Moderator, CommandCategory::Player,
        "slay <steamId|#id>", "Instantly kill a player.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: slay <steamId|#id>"); return false; }
            GameServer* s = ctx.server;
            auto* pm = s ? s->GetPlayerManager() : nullptr;
            uint32_t cid = ResolveTarget(s, ctx.args[0]);
            auto player = (pm && cid != INVALID_CLIENT_ID) ? pm->GetPlayer(cid) : nullptr;
            if (!player) { ctx.Reply("Player not found: " + ctx.args[0]); return false; }
            player->SetHealth(0);
            player->SetState(PlayerState::Dead);
            player->MarkDeath();
            pm->OnPlayerDeath(cid);
            ctx.Reply("Slayed " + ctx.args[0]);
            return true;
        }});

    Register({"god", {}, CommandLevel::Moderator, CommandCategory::Player,
        "god <steamId|#id|all> <on|off>", "Toggle invincibility for a player.",
        [](CommandContext& ctx) {
            if (ctx.args.size() < 2) { ctx.Reply("Usage: god <steamId|#id|all> <on|off>"); return false; }
            GameServer* s = ctx.server;
            auto* pm = s ? s->GetPlayerManager() : nullptr;
            if (!pm) { ctx.Reply("PlayerManager unavailable."); return false; }
            bool enable = StringUtils::EqualsIgnoreCase(ctx.args[1], "on") ||
                          StringUtils::EqualsIgnoreCase(ctx.args[1], "true") ||
                          ctx.args[1] == "1";
            if (StringUtils::EqualsIgnoreCase(ctx.args[0], "all")) {
                int n = 0;
                for (const auto& c : s->GetAllConnections()) {
                    if (!c) continue;
                    if (auto p = pm->GetPlayer(c->GetClientId())) { p->SetGodMode(enable); ++n; }
                }
                ctx.Reply(std::string("God mode ") + (enable ? "on" : "off") + " for " + std::to_string(n) + " players");
                return true;
            }
            uint32_t cid = ResolveTarget(s, ctx.args[0]);
            auto player = (cid != INVALID_CLIENT_ID) ? pm->GetPlayer(cid) : nullptr;
            if (!player) { ctx.Reply("Player not found: " + ctx.args[0]); return false; }
            player->SetGodMode(enable);
            ctx.Reply(std::string("God mode ") + (enable ? "on" : "off") + " for " + ctx.args[0]);
            return true;
        }});

    Register({"tp", {"teleport"}, CommandLevel::Moderator, CommandCategory::Player,
        "tp <steamId|#id> <x> <y> <z>", "Teleport a player to world coordinates.",
        [](CommandContext& ctx) {
            if (ctx.args.size() < 4) { ctx.Reply("Usage: tp <steamId|#id> <x> <y> <z>"); return false; }
            GameServer* s = ctx.server;
            auto* pm = s ? s->GetPlayerManager() : nullptr;
            uint32_t cid = ResolveTarget(s, ctx.args[0]);
            auto player = (pm && cid != INVALID_CLIENT_ID) ? pm->GetPlayer(cid) : nullptr;
            if (!player) { ctx.Reply("Player not found: " + ctx.args[0]); return false; }
            float x, y, z;
            if (!ParseFloat(ctx.args[1], x) || !ParseFloat(ctx.args[2], y) || !ParseFloat(ctx.args[3], z)) {
                ctx.Reply("Invalid coordinates."); return false;
            }
            player->SetPosition(Vector3(x, y, z));
            ctx.Reply("Teleported " + ctx.args[0]);
            return true;
        }});

    Register({"give", {}, CommandLevel::Moderator, CommandCategory::Player,
        "give <steamId|#id|all> <item> [amount]", "Give an item to a player.",
        [](CommandContext& ctx) {
            if (ctx.args.size() < 2) { ctx.Reply("Usage: give <steamId|#id|all> <item> [amount]"); return false; }
            GameServer* s = ctx.server;
            auto* pm = s ? s->GetPlayerManager() : nullptr;
            if (!pm) { ctx.Reply("PlayerManager unavailable."); return false; }
            const std::string& item = ctx.args[1];
            int amount = 1;
            if (ctx.args.size() >= 3) { if (auto n = StringUtils::ToInt(ctx.args[2]); n && *n > 0) amount = *n; }
            if (StringUtils::EqualsIgnoreCase(ctx.args[0], "all")) {
                int n = 0;
                for (const auto& c : s->GetAllConnections()) {
                    if (!c) continue;
                    if (auto p = pm->GetPlayer(c->GetClientId())) { p->AddItem(item, amount); ++n; }
                }
                ctx.Reply("Gave " + std::to_string(amount) + "x " + item + " to " + std::to_string(n) + " players");
                return true;
            }
            uint32_t cid = ResolveTarget(s, ctx.args[0]);
            auto player = (cid != INVALID_CLIENT_ID) ? pm->GetPlayer(cid) : nullptr;
            if (!player) { ctx.Reply("Player not found: " + ctx.args[0]); return false; }
            player->AddItem(item, amount);
            ctx.Reply("Gave " + std::to_string(amount) + "x " + item + " to " + ctx.args[0]);
            return true;
        }});

    Register({"team", {}, CommandLevel::Moderator, CommandCategory::Player,
        "team <steamId|#id> <teamId>", "Move a player to a team.",
        [](CommandContext& ctx) {
            if (ctx.args.size() < 2) { ctx.Reply("Usage: team <steamId|#id> <teamId>"); return false; }
            GameServer* s = ctx.server;
            auto* pm = s ? s->GetPlayerManager() : nullptr;
            uint32_t cid = ResolveTarget(s, ctx.args[0]);
            auto player = (pm && cid != INVALID_CLIENT_ID) ? pm->GetPlayer(cid) : nullptr;
            if (!player) { ctx.Reply("Player not found: " + ctx.args[0]); return false; }
            auto t = StringUtils::ToInt(ctx.args[1]);
            if (!t || *t < 0) { ctx.Reply("Invalid team id: " + ctx.args[1]); return false; }
            player->SetTeam(static_cast<uint32_t>(*t));
            if (auto conn = s->GetClientConnection(cid)) conn->SetTeamId(static_cast<uint32_t>(*t));
            ctx.Reply("Moved " + ctx.args[0] + " to team " + ctx.args[1]);
            return true;
        }});

    // ---------------------------------------------------------------------
    // Map management
    // ---------------------------------------------------------------------
    Register({"changemap", {"map"}, CommandLevel::Admin, CommandCategory::Map,
        "changemap <mapName>", "Load a different map immediately.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: changemap <mapName>"); return false; }
            auto* mm = ctx.server ? ctx.server->GetMapManager() : nullptr;
            if (!mm) { ctx.Reply("MapManager unavailable."); return false; }
            if (mm->LoadMap(ctx.args[0])) {
                ctx.server->BroadcastChatMessage("Admin changed map to " + ctx.args[0]);
                ctx.Reply("Changed map to " + ctx.args[0]);
                return true;
            }
            ctx.Reply("Failed to load map: " + ctx.args[0]);
            return false;
        }});

    Register({"maps", {"maplist"}, CommandLevel::Helper, CommandCategory::Map,
        "maps", "List maps available in configuration.",
        [](CommandContext& ctx) {
            GameServer* s = ctx.server;
            auto sc = s ? s->GetServerConfig() : nullptr;
            if (!sc) { ctx.Reply("ServerConfig unavailable."); return false; }
            MapConfig mc(*sc);
            mc.Initialize();
            auto* mm = s->GetMapManager();
            auto maps = mc.GetAvailableMaps();
            ctx.Reply("Available maps (" + std::to_string(maps.size()) + "):");
            for (const auto& name : maps) {
                std::string mark = (mm && name == mm->GetCurrentMapName()) ? " *" : "";
                ctx.Reply("  " + name + mark);
            }
            return true;
        }});

    Register({"rotation", {}, CommandLevel::Admin, CommandCategory::Map,
        "rotation [list]", "Show the configured map rotation.",
        [](CommandContext& ctx) {
            GameServer* s = ctx.server;
            auto sc = s ? s->GetServerConfig() : nullptr;
            std::string sub = ctx.args.empty() ? "list" : StringUtils::ToLower(ctx.args[0]);
            if (sub == "add" || sub == "remove") {
                ctx.Reply("Rotation is defined in maps.ini; edit it, run 'reload', then 'changemap <id>'.");
                return true;
            }
            if (!sc) { ctx.Reply("ServerConfig unavailable."); return false; }
            MapConfig mc(*sc);
            mc.Initialize();
            auto* mm = s->GetMapManager();
            ctx.Reply("Map rotation:");
            for (const auto& name : mc.GetAvailableMaps()) {
                std::string mark = (mm && name == mm->GetCurrentMapName()) ? " *" : "";
                ctx.Reply("  " + name + mark);
            }
            return true;
        }});

    Register({"startvote", {}, CommandLevel::Moderator, CommandCategory::Map,
        "startvote", "Start an end-of-round map vote.",
        [](CommandContext& ctx) {
            bool ok = ctx.server && ctx.server->StartMapVote();
            ctx.Reply(ok ? "Map vote started." : "Could not start map vote.");
            return ok;
        }});

    // ---------------------------------------------------------------------
    // Mods / workshop / mutators
    // ---------------------------------------------------------------------
    Register({"mods", {}, CommandLevel::Helper, CommandCategory::Mods,
        "mods", "List loaded mods/assets.",
        [](CommandContext& ctx) {
            auto* mods = ctx.server ? ctx.server->GetModManager() : nullptr;
            if (!mods) { ctx.Reply("ModManager unavailable."); return false; }
            const auto& list = mods->GetMods();
            ctx.Reply("Mods/assets (" + std::to_string(list.size()) + "):");
            for (const auto& m : list) {
                ctx.Reply("  " + m.name + (m.isAsset ? " [asset]" : " [mod]") + (m.present ? "" : " (MISSING)"));
            }
            return true;
        }});

    Register({"mutators", {}, CommandLevel::Helper, CommandCategory::Mods,
        "mutators", "List active and available mutators.",
        [](CommandContext& ctx) {
            auto* mut = ctx.server ? ctx.server->GetMutatorManager() : nullptr;
            if (!mut) { ctx.Reply("MutatorManager unavailable."); return false; }
            auto active = mut->GetActiveNames();
            ctx.Reply("Active mutators (" + std::to_string(active.size()) + "):");
            for (const auto& n : active) ctx.Reply("  " + n);
            ctx.Reply("Available: " + StringUtils::Join(mut->GetRegisteredIds(), ", "));
            return true;
        }});

    Register({"workshop", {}, CommandLevel::Admin, CommandCategory::Mods,
        "workshop [list|reload|validate|download]", "Manage Steam Workshop content.",
        [](CommandContext& ctx) {
            auto* ws = ctx.server ? ctx.server->GetWorkshopManager() : nullptr;
            if (!ws) { ctx.Reply("WorkshopManager unavailable."); return false; }
            std::string sub = ctx.args.empty() ? "list" : StringUtils::ToLower(ctx.args[0]);
            if (sub == "list") {
                const auto& items = ws->GetItems();
                ctx.Reply("Workshop items (" + std::to_string(items.size()) + "):");
                for (const auto& it : items) {
                    ctx.Reply("  [" + WorkshopManager::TypeToString(it.type) + "] " +
                              it.fileName + (it.present ? " (ok)" : " (MISSING)"));
                }
                return true;
            }
            if (sub == "reload")   { ws->Reload(); ctx.Reply("Workshop manifest reloaded."); return true; }
            if (sub == "validate") { ctx.Reply("Workshop validate: " + std::to_string(ws->ValidateItems()) + " missing."); return true; }
            if (sub == "download") { ctx.Reply("Workshop download: " + std::to_string(ws->DownloadMissing()) + " fetched."); return true; }
            ctx.Reply("Usage: workshop [list|reload|validate|download]");
            return false;
        }});

    // ---------------------------------------------------------------------
    // Anti-cheat
    // ---------------------------------------------------------------------
    Register({"eacmode", {}, CommandLevel::Admin, CommandCategory::AntiCheat,
        "eacmode <safe|emulate|off>", "Set the EAC emulator operating mode.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: eacmode <safe|emulate|off>"); return false; }
            std::string mode = StringUtils::ToLower(ctx.args[0]);
            if (mode != "safe" && mode != "emulate" && mode != "off") {
                ctx.Reply("Mode must be safe, emulate, or off."); return false;
            }
            auto cfg = ctx.server ? ctx.server->GetConfigManager() : nullptr;
            if (!cfg) { ctx.Reply("ConfigManager unavailable."); return false; }
            cfg->SetString("EAC.mode", mode);
            // GAP: the EAC emulator is owned by main() and reads this at start; a
            // live mode switch needs an EAC control hook — revisit when the EAC
            // emulator exposes one. For now the setting applies on next (re)start.
            ctx.Reply("EAC mode set to '" + mode + "' (applies on next EAC start).");
            return true;
        }});

    // ---------------------------------------------------------------------
    // Configuration
    // ---------------------------------------------------------------------
    Register({"get", {"cvar_get"}, CommandLevel::Admin, CommandCategory::Config,
        "get <Section.key>", "Read a configuration value.",
        [](CommandContext& ctx) {
            if (ctx.args.empty()) { ctx.Reply("Usage: get <Section.key>"); return false; }
            auto cfg = ctx.server ? ctx.server->GetConfigManager() : nullptr;
            if (!cfg) { ctx.Reply("ConfigManager unavailable."); return false; }
            if (!cfg->HasKey(ctx.args[0])) { ctx.Reply(ctx.args[0] + " = (unset)"); return false; }
            ctx.Reply(ctx.args[0] + " = " + cfg->GetString(ctx.args[0]));
            return true;
        }});

    Register({"set", {"cvar_set"}, CommandLevel::Admin, CommandCategory::Config,
        "set <Section.key> <value>", "Set a configuration value (runtime only).",
        [](CommandContext& ctx) {
            if (ctx.args.size() < 2) { ctx.Reply("Usage: set <Section.key> <value>"); return false; }
            auto cfg = ctx.server ? ctx.server->GetConfigManager() : nullptr;
            if (!cfg) { ctx.Reply("ConfigManager unavailable."); return false; }
            std::vector<std::string> rest(ctx.args.begin() + 1, ctx.args.end());
            std::string value = StringUtils::Join(rest, " ");
            cfg->SetString(ctx.args[0], value);
            ctx.Reply(ctx.args[0] + " = " + value + " (use 'saveconfig' to persist)");
            return true;
        }});

    Register({"saveconfig", {"save"}, CommandLevel::Admin, CommandCategory::Config,
        "saveconfig", "Persist the current configuration to disk.",
        [](CommandContext& ctx) {
            auto cfg = ctx.server ? ctx.server->GetConfigManager() : nullptr;
            if (!cfg) { ctx.Reply("ConfigManager unavailable."); return false; }
            bool ok = cfg->SaveAllConfigurations();
            ctx.Reply(ok ? "Configuration saved." : "Failed to save configuration.");
            return ok;
        }});

    Register({"config", {}, CommandLevel::Admin, CommandCategory::Config,
        "config <get|set|save|sections|keys> [...]", "Inspect/modify configuration.",
        [](CommandContext& ctx) {
            auto cfg = ctx.server ? ctx.server->GetConfigManager() : nullptr;
            if (!cfg) { ctx.Reply("ConfigManager unavailable."); return false; }
            std::string sub = ctx.args.empty() ? "" : StringUtils::ToLower(ctx.args[0]);
            if (sub == "sections") {
                for (const auto& s : cfg->GetAllSections()) ctx.Reply("  " + s);
                return true;
            }
            if (sub == "keys" && ctx.args.size() >= 2) {
                for (const auto& k : cfg->GetSectionKeys(ctx.args[1])) ctx.Reply("  " + k);
                return true;
            }
            if (sub == "get" && ctx.args.size() >= 2) {
                ctx.Reply(ctx.args[1] + " = " + cfg->GetString(ctx.args[1]));
                return true;
            }
            if (sub == "set" && ctx.args.size() >= 3) {
                std::vector<std::string> rest(ctx.args.begin() + 2, ctx.args.end());
                cfg->SetString(ctx.args[1], StringUtils::Join(rest, " "));
                ctx.Reply("Set " + ctx.args[1]);
                return true;
            }
            if (sub == "save") {
                bool ok = cfg->SaveAllConfigurations();
                ctx.Reply(ok ? "Saved." : "Save failed.");
                return ok;
            }
            ctx.Reply("Usage: config <get|set|save|sections|keys> [...]");
            return false;
        }});
}
