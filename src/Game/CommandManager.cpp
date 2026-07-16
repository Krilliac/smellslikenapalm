// src/Game/CommandManager.cpp — command registry, parsing and dispatch engine.
//
// The built-in command HANDLERS live in CommandHandlers.cpp; this file owns the
// transport-agnostic machinery: tokenisation, lookup, the central permission
// gate, audit logging and the `help` command.

#include "Game/CommandManager.h"

#include "Game/GameServer.h"
#include "Game/AdminManager.h"
#include "Network/ClientConnection.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/CrashHandler.h"

#include <algorithm>
#include <deque>
#include <sstream>
#include <utility>

CommandManager::CommandManager(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[CommandManager::CommandManager] Entry: server=%p", static_cast<void*>(server));
}

CommandManager::~CommandManager()
{
    Logger::Trace("[CommandManager::~CommandManager] Entry");
    StopAccepting("command manager destroyed");
}

void CommandManager::Initialize()
{
    Logger::Trace("[CommandManager::Initialize] Entry");
    StopAccepting("command manager reinitialized");
    m_commands.clear();
    RegisterBuiltins();

    // `help` needs the registry, so it is registered here rather than in the
    // handler file. It is Player-level: anyone may discover what they can run.
    Register(CommandDef{
        "help", {"?", "commands"}, CommandLevel::Player, CommandCategory::Help,
        "help [command]",
        "List commands you may use, or show detail for one command.",
        [this](CommandContext& ctx) { return CmdHelp(ctx); }
    });

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_acceptingQueuedCommands = true;
    }

    Logger::Info("CommandManager: registered %zu commands", m_commands.size());
    Logger::Trace("[CommandManager::Initialize] Exit");
}

void CommandManager::Shutdown()
{
    Logger::Trace("[CommandManager::Shutdown] Entry");
    StopAccepting("server shutting down");
    m_commands.clear();
    Logger::Trace("[CommandManager::Shutdown] Exit");
}

void CommandManager::Register(CommandDef def)
{
    // Normalise name + aliases to lowercase so lookup is case-insensitive.
    def.name = StringUtils::ToLower(def.name);
    for (auto& a : def.aliases) a = StringUtils::ToLower(a);

    // Replace an existing command of the same name (idempotent registration).
    auto it = std::find_if(m_commands.begin(), m_commands.end(),
                           [&](const CommandDef& c) { return c.name == def.name; });
    if (it != m_commands.end()) {
        Logger::Debug("[CommandManager::Register] Replacing existing command '%s'", def.name.c_str());
        *it = std::move(def);
    } else {
        m_commands.push_back(std::move(def));
    }
}

const CommandDef* CommandManager::Find(std::string_view nameOrAlias) const
{
    const std::string key = StringUtils::ToLower(std::string(nameOrAlias));
    for (const auto& c : m_commands) {
        if (c.name == key) return &c;
        if (std::find(c.aliases.begin(), c.aliases.end(), key) != c.aliases.end()) return &c;
    }
    return nullptr;
}

std::vector<std::string> CommandManager::Tokenize(std::string_view line)
{
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuotes = false;
    bool haveToken = false;  // distinguishes "" (a real empty quoted arg) from no token

    for (char c : line) {
        if (c == '"') {
            inQuotes = !inQuotes;
            haveToken = true;  // a quote starts a token even if it stays empty
            continue;
        }
        if (!inQuotes && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (haveToken) {
                tokens.push_back(cur);
                cur.clear();
                haveToken = false;
            }
            continue;
        }
        cur.push_back(c);
        haveToken = true;
    }
    if (haveToken) tokens.push_back(cur);
    return tokens;
}

bool CommandManager::Execute(CommandContext& ctx, std::string_view commandLine)
{
    auto tokens = Tokenize(commandLine);
    if (tokens.empty()) {
        ctx.Reply("Empty command.");
        return false;
    }
    const std::string name = tokens.front();
    ctx.args.assign(tokens.begin() + 1, tokens.end());
    return ExecuteParsed(ctx, name);
}

std::future<QueuedCommandResult> CommandManager::Enqueue(
    CommandContext ctx, std::string commandLine)
{
    // Never retain a transport-owned callback or a possibly stale caller-supplied
    // server pointer across threads. ProcessQueued installs both authoritative
    // values immediately before dispatch.
    ctx.args.clear();
    ctx.out = {};
    ctx.server = nullptr;

    PendingCommand pending{
        std::move(ctx), std::move(commandLine),
        std::promise<QueuedCommandResult>{}};
    std::future<QueuedCommandResult> future = pending.completion.get_future();

    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (!m_acceptingQueuedCommands) {
        pending.completion.set_value(
            QueuedCommandResult{false, false,
                                "Command rejected: server shutting down.\n"});
        return future;
    }
    if (m_queuedCommands.size() >= MAX_QUEUED_COMMANDS) {
        pending.completion.set_value(
            QueuedCommandResult{false, false,
                                "Command rejected: command queue full.\n"});
        return future;
    }

    m_queuedCommands.push_back(std::move(pending));
    return future;
}

size_t CommandManager::ProcessQueued(size_t limit)
{
    if (limit == 0) return 0;

    std::deque<PendingCommand> work;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const size_t count = std::min(limit, m_queuedCommands.size());
        for (size_t i = 0; i < count; ++i) {
            work.push_back(std::move(m_queuedCommands.front()));
            m_queuedCommands.pop_front();
        }
    }

    for (PendingCommand& pending : work) {
        QueuedCommandResult result;
        result.executed = true;
        pending.context.server = m_server;
        pending.context.out = [&result](std::string_view line) {
            result.output.append(line.data(), line.size());
            result.output.push_back('\n');
        };

        // Execute() guards handlers, while this outer guard also covers parsing,
        // lookup, logging, and output collection. Every accepted promise is
        // fulfilled even if non-handler dispatch code throws.
        const bool completed = rs2v::Guard("queued command dispatch", [&] {
            result.ok = Execute(pending.context, pending.commandLine);
        });
        if (!completed) {
            result.ok = false;
            result.output.append("Command failed: internal dispatch error.\n");
        }
        pending.completion.set_value(std::move(result));
    }
    return work.size();
}

void CommandManager::StopAccepting(std::string_view reason)
{
    std::deque<PendingCommand> rejected;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_acceptingQueuedCommands = false;
        rejected.swap(m_queuedCommands);
    }

    std::string output = "Command rejected: ";
    output.append(reason.data(), reason.size());
    output.append(".\n");
    for (PendingCommand& pending : rejected) {
        pending.completion.set_value(
            QueuedCommandResult{false, false, output});
    }
}

bool CommandManager::ExecuteParsed(CommandContext& ctx, std::string_view name)
{
    const CommandDef* cmd = Find(name);
    if (!cmd) {
        Logger::Debug("[CommandManager] Unknown command '%.*s' from %s",
                      static_cast<int>(name.size()), name.data(), ctx.invoker.c_str());
        ctx.Reply("Unknown command: " + std::string(name) + " (try 'help').");
        return false;
    }

    // Central permission gate — the one place authorization is decided.
    if (static_cast<int>(ctx.level) < static_cast<int>(cmd->minLevel)) {
        Logger::Warn("[CommandManager] DENY '%s' for %s (level %s < required %s)",
                     cmd->name.c_str(), ctx.invoker.c_str(),
                     LevelName(ctx.level), LevelName(cmd->minLevel));
        ctx.Reply("Permission denied: '" + cmd->name + "' requires " +
                  LevelName(cmd->minLevel) + " (you are " + LevelName(ctx.level) + ").");
        return false;
    }

    // Audit every accepted command with who/where/what. This is the security
    // record the docs promise; keep it unconditional (INFO).
    {
        const std::string joined = StringUtils::Join(ctx.args, " ");
        const char* src = (ctx.source == CommandSource::InGame) ? "ingame"
                        : (ctx.source == CommandSource::Console) ? "console" : "remote";
        Logger::Info("[CMD] %s/%s '%s' %s", src, ctx.invoker.c_str(),
                     cmd->name.c_str(), joined.c_str());
    }

    if (!cmd->handler) {
        ctx.Reply("Command '" + cmd->name + "' has no handler.");
        return false;
    }

    // A handler must never take the server down over hostile/garbled input. Run
    // it under the non-fatal guard so any exception is diagnosed via the crash
    // handler and the server keeps running; the user gets a generic failure.
    bool ok = false;
    bool completed = rs2v::Guard("command handler", [&] { ok = cmd->handler(ctx); });
    if (!completed) {
        ctx.Reply("Command failed: internal error.");
        return false;
    }
    return ok;
}

CommandLevel CommandManager::LevelFromInt(int level)
{
    if (level < 0) level = 0;
    if (level > static_cast<int>(CommandLevel::Console)) level = static_cast<int>(CommandLevel::Console);
    return static_cast<CommandLevel>(level);
}

const char* CommandManager::LevelName(CommandLevel level)
{
    switch (level) {
        case CommandLevel::Player:    return "Player";
        case CommandLevel::Helper:    return "Helper";
        case CommandLevel::Moderator: return "Moderator";
        case CommandLevel::Admin:     return "Admin";
        case CommandLevel::Dev:       return "Dev";
        case CommandLevel::Console:   return "Console";
    }
    return "Unknown";
}

const char* CommandManager::CategoryName(CommandCategory cat)
{
    switch (cat) {
        case CommandCategory::Server:        return "Server";
        case CommandCategory::Communication: return "Communication";
        case CommandCategory::Player:        return "Player";
        case CommandCategory::Map:           return "Map";
        case CommandCategory::Mods:          return "Mods";
        case CommandCategory::AntiCheat:     return "Anti-Cheat";
        case CommandCategory::Config:        return "Config";
        case CommandCategory::Automation:    return "Automation";
        case CommandCategory::Dev:           return "Dev";
        case CommandCategory::Help:          return "Help";
    }
    return "Other";
}

CommandLevel CommandManager::ResolveLevelForClient(uint32_t clientId) const
{
    if (!m_server) return CommandLevel::Player;
    auto conn = m_server->GetClientConnection(clientId);
    if (!conn) return CommandLevel::Player;
    auto* admin = m_server->GetAdminManager();
    if (!admin) return CommandLevel::Player;
    return LevelFromInt(admin->GetPermissionLevel(conn->GetSteamID()));
}

bool CommandManager::CmdHelp(CommandContext& ctx)
{
    if (!ctx.args.empty()) {
        const CommandDef* cmd = Find(ctx.args[0]);
        if (!cmd) {
            ctx.Reply("No such command: " + ctx.args[0]);
            return false;
        }
        ctx.Reply("Command: " + cmd->name);
        if (!cmd->aliases.empty())
            ctx.Reply("  Aliases: " + StringUtils::Join(cmd->aliases, ", "));
        ctx.Reply(std::string("  Level:   ") + LevelName(cmd->minLevel) +
                  "  (" + CategoryName(cmd->category) + ")");
        ctx.Reply("  Usage:   " + cmd->usage);
        ctx.Reply("  " + cmd->description);
        return true;
    }

    // List only the commands this requester may actually run, grouped nothing
    // fancy — sorted by name for stable, scannable output.
    std::vector<const CommandDef*> visible;
    for (const auto& c : m_commands) {
        if (static_cast<int>(ctx.level) >= static_cast<int>(c.minLevel))
            visible.push_back(&c);
    }
    std::sort(visible.begin(), visible.end(),
              [](const CommandDef* a, const CommandDef* b) { return a->name < b->name; });

    ctx.Reply("Available commands (" + std::string(LevelName(ctx.level)) + ", " +
              std::to_string(visible.size()) + "):");
    for (const auto* c : visible) {
        ctx.Reply("  " + c->name + " — " + c->description);
    }
    ctx.Reply("Use 'help <command>' for usage.");
    return true;
}
