// src/Game/CommandManager.h — Unified server command system
//
// CommandManager is the single source of truth for every administrative,
// developer, moderator, player, console, configuration and automation command
// the server understands. Commands are registered ONCE here and dispatched
// identically regardless of where the request originated:
//
//   * In-game chat   — an authenticated player types "/<cmd> ..." (ChatManager)
//   * Local console  — a line typed on the server's stdin       (ConsoleInput)
//   * Remote (SOAP)  — an HTTP/SOAP request from a tool or AI   (RemoteAdminServer)
//
// Every transport builds a CommandContext (who is asking, at what permission
// level, and where output should go) and dispatches directly or through the
// main-thread queue. Permission is checked centrally against the command's
// minimum level before the handler runs, so a new transport cannot accidentally
// bypass authorization — the gate lives here, not in the caller.
//
// Threading: Execute() is an authoritative-game-thread API. In-game chat already
// reaches it from GameServer::Run; worker-thread transports (console and remote
// admin) must use Enqueue() and wait for the result. GameServer drains the
// bounded queue before ticking mutable subsystems. This keeps handlers from
// racing PlayerManager, BotManager, ConfigManager, and the other single-threaded
// game-state owners.

#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class GameServer;

// Permission tiers, lowest to highest. A request may run a command iff its
// resolved level is >= the command's minimum level. Numeric values are part of
// the admin_list.txt contract (see AdminManager) and the docs — do not reorder.
enum class CommandLevel : int {
    Player    = 0,  // any connected player (default)
    Helper    = 1,  // read-only server info
    Moderator = 2,  // player management (kick/ban/tp/god/...)
    Admin     = 3,  // full server control (maps/config/restart/...)
    Dev       = 4,  // developer/diagnostic commands (regen/timescale/...)
    Console   = 5,  // local console / authenticated remote root — full trust
};

// Where a command request originated. Used for audit logging and to let a
// handler refuse things that only make sense for a given transport.
enum class CommandSource : int {
    InGame,
    Console,
    Remote,
};

// Coarse grouping used by `help` output. Purely presentational.
enum class CommandCategory : int {
    Server,
    Communication,
    Player,
    Map,
    Mods,
    AntiCheat,
    Config,
    Automation,
    Dev,
    Help,
};

// Everything a handler needs about one invocation. Built by the transport and
// passed by reference; handlers read args/level and write output through Reply.
struct CommandContext {
    CommandSource source = CommandSource::Console;
    CommandLevel  level  = CommandLevel::Player;

    // In-game client id when source == InGame, else INVALID_CLIENT_ID.
    uint32_t clientId = UINT32_MAX;

    // Human/audit identity: SteamID for in-game, "console", or a remote label.
    std::string invoker;

    // Parsed arguments (the command name itself is NOT included).
    std::vector<std::string> args;

    // Automation/AI mode: when true, handlers should prefer terse,
    // machine-parseable output (key=value lines) over prose.
    bool machine = false;

    GameServer* server = nullptr;

    // Output sink — the transport decides where replies land (chat message,
    // stdout, or a SOAP response body). May be empty; Reply() is null-safe.
    std::function<void(std::string_view)> out;

    void Reply(std::string_view line) const { if (out) out(line); }
};

// Handler returns true on success, false on user/usage error. The boolean is
// surfaced to automation callers (e.g. SOAP <ok> field); user-facing detail
// goes through ctx.Reply().
using CommandHandler = std::function<bool(CommandContext&)>;

struct CommandDef {
    std::string              name;        // canonical lowercase name
    std::vector<std::string> aliases;     // alternate lowercase names
    CommandLevel             minLevel = CommandLevel::Admin;
    CommandCategory          category = CommandCategory::Server;
    std::string              usage;       // e.g. "kick <steamId> [reason...]"
    std::string              description; // one line
    CommandHandler           handler;
};

// Result returned to worker-thread transports after the authoritative game
// thread has executed (or rejected) a queued command.
struct QueuedCommandResult {
    // False means the request never ran (queue full or server shutting down).
    bool executed = false;
    bool ok = false;
    // Newline-delimited Reply() output, ready for stdout or a SOAP body.
    std::string output;
};

class CommandManager {
public:
    static constexpr uint32_t INVALID_CLIENT_ID = UINT32_MAX;
    static constexpr size_t MAX_QUEUED_COMMANDS = 64;
    static constexpr size_t MAX_COMMANDS_PER_TICK = 16;

    explicit CommandManager(GameServer* server);
    ~CommandManager();

    // Registers every built-in command. Safe to call once.
    void Initialize();
    void Shutdown();

    // Register/replace a command (and its aliases). Later registration of the
    // same name overrides the earlier one — used for tests and extensions.
    void Register(CommandDef def);

    // Resolve a command by name or alias (case-insensitive). nullptr if unknown.
    const CommandDef* Find(std::string_view nameOrAlias) const;

    // Parse a full command line ("<name> arg1 \"arg two\" ...") into ctx.args
    // and dispatch. Returns the handler result, or false on unknown command /
    // permission denial (a reply explaining which is sent through ctx.out).
    bool Execute(CommandContext& ctx, std::string_view commandLine);

    // Copy a transport request into the bounded main-thread queue. The caller's
    // output callback and server pointer are intentionally not retained; output
    // is collected in QueuedCommandResult so worker-local captures cannot outlive
    // their transport request. Rejection returns an already-ready future.
    std::future<QueuedCommandResult> Enqueue(CommandContext ctx,
                                              std::string commandLine);

    // Drain queued work in FIFO order. Must be called only by the authoritative
    // GameServer::Run thread. The per-tick cap prevents command floods from
    // starving simulation/network work.
    size_t ProcessQueued(size_t limit = MAX_COMMANDS_PER_TICK);

    // Reject queued and future submissions. Idempotent and safe from any thread;
    // GameServer calls this before joining command transports during shutdown.
    void StopAccepting(std::string_view reason = "server shutting down");

    // Dispatch an already-parsed command. ctx.args must be populated.
    bool ExecuteParsed(CommandContext& ctx, std::string_view name);

    // Map a numeric admin-list level (0..5) to a CommandLevel, clamped.
    static CommandLevel LevelFromInt(int level);
    static const char* LevelName(CommandLevel level);
    static const char* CategoryName(CommandCategory cat);

    // Resolve the permission level for an in-game client by SteamID via
    // AdminManager. Unlisted players resolve to Player.
    CommandLevel ResolveLevelForClient(uint32_t clientId) const;

    // Split a command line into tokens, honouring double-quoted groups so that
    // "ban 76561... \"go away now\"" yields a single reason token. Public so
    // transports/tests can reuse the exact same tokenisation.
    static std::vector<std::string> Tokenize(std::string_view line);

    const std::vector<CommandDef>& Commands() const { return m_commands; }

private:
    struct PendingCommand {
        CommandContext context;
        std::string commandLine;
        std::promise<QueuedCommandResult> completion;
    };

    // Registration helpers split across translation units to keep each file a
    // single coherent job (engine here, handlers in CommandHandlers.cpp).
    void RegisterBuiltins();

    // Built-in help command implementation (needs registry access).
    bool CmdHelp(CommandContext& ctx);

    GameServer* m_server;
    std::vector<CommandDef> m_commands;

    std::mutex m_queueMutex;
    std::deque<PendingCommand> m_queuedCommands;
    bool m_acceptingQueuedCommands = false;
};
