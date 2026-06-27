// tests/CommandManagerTests.cpp
//
// Unit tests for the unified command system (src/Game/CommandManager) and the
// SOAP transport's pure helpers (src/Network/RemoteAdminServer). These exercise
// the transport-agnostic machinery — tokenisation, alias resolution, the central
// permission gate, help visibility and the defensive SOAP XML parser — without
// standing up a live GameServer (passed as nullptr; the tested commands either
// need no server or null-guard their server access).

#include "TestFramework.h"

#include "Game/CommandManager.h"
#include "Game/GameServer.h"
#include "Network/RemoteAdminServer.h"

#include <string>
#include <vector>
#include <algorithm>

namespace {

// Build a context that captures every Reply() line for assertions.
struct Captured {
    CommandContext ctx;
    std::vector<std::string> lines;
};

std::shared_ptr<Captured> MakeCtx(CommandLevel level, CommandSource src = CommandSource::Console)
{
    auto c = std::make_shared<Captured>();
    c->ctx.level   = level;
    c->ctx.source  = src;
    c->ctx.invoker = "test";
    c->ctx.server  = nullptr;
    auto* raw = c.get();
    c->ctx.out = [raw](std::string_view s) { raw->lines.push_back(std::string(s)); };
    return c;
}

bool AnyLineContains(const std::vector<std::string>& lines, const std::string& needle)
{
    return std::any_of(lines.begin(), lines.end(),
        [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

} // namespace

TEST(CommandManager, TokenizeHandlesQuotesAndWhitespace) {
    auto t = CommandManager::Tokenize("kick 76561198000000001 \"go away now\"");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0], "kick");
    EXPECT_EQ(t[1], "76561198000000001");
    EXPECT_EQ(t[2], "go away now");

    // Collapses runs of spaces/tabs; trims edges.
    auto t2 = CommandManager::Tokenize("  say    hello\tworld ");
    ASSERT_EQ(t2.size(), 3u);
    EXPECT_EQ(t2[0], "say");
    EXPECT_EQ(t2[1], "hello");
    EXPECT_EQ(t2[2], "world");

    EXPECT_TRUE(CommandManager::Tokenize("   ").empty());
}

TEST(CommandManager, LevelFromIntClampsToRange) {
    EXPECT_EQ(CommandManager::LevelFromInt(-5), CommandLevel::Player);
    EXPECT_EQ(CommandManager::LevelFromInt(0),  CommandLevel::Player);
    EXPECT_EQ(CommandManager::LevelFromInt(2),  CommandLevel::Moderator);
    EXPECT_EQ(CommandManager::LevelFromInt(5),  CommandLevel::Console);
    EXPECT_EQ(CommandManager::LevelFromInt(99), CommandLevel::Console);
}

TEST(CommandManager, FindResolvesNamesAndAliasesCaseInsensitively) {
    CommandManager cm(nullptr);
    cm.Initialize();
    EXPECT_TRUE(cm.Find("kick") != nullptr);
    EXPECT_TRUE(cm.Find("KICK") != nullptr);
    EXPECT_TRUE(cm.Find("bc") != nullptr);          // alias of broadcast
    EXPECT_EQ(cm.Find("bc"), cm.Find("broadcast"));
    EXPECT_TRUE(cm.Find("?") != nullptr);           // alias of help
    EXPECT_TRUE(cm.Find("does-not-exist") == nullptr);
}

TEST(CommandManager, PermissionGateDeniesUnderprivileged) {
    CommandManager cm(nullptr);
    cm.Initialize();

    auto player = MakeCtx(CommandLevel::Player);
    bool ok = cm.Execute(player->ctx, "kick 76561198000000001");
    EXPECT_FALSE(ok);
    EXPECT_TRUE(AnyLineContains(player->lines, "Permission denied"));
}

TEST(CommandManager, PermissionGateAllowsAtOrAboveLevel) {
    CommandManager cm(nullptr);
    cm.Initialize();

    // Console outranks every command; the shutdown handler null-guards server.
    auto console = MakeCtx(CommandLevel::Console);
    bool ok = cm.Execute(console->ctx, "shutdown");
    EXPECT_TRUE(ok);
    EXPECT_FALSE(AnyLineContains(console->lines, "Permission denied"));
}

TEST(CommandManager, PlayerLevelCommandsRunForPlayers) {
    CommandManager cm(nullptr);
    cm.Initialize();

    auto p1 = MakeCtx(CommandLevel::Player);
    EXPECT_TRUE(cm.Execute(p1->ctx, "ping"));
    EXPECT_TRUE(AnyLineContains(p1->lines, "pong"));

    auto p2 = MakeCtx(CommandLevel::Player);
    EXPECT_TRUE(cm.Execute(p2->ctx, "echo hello there"));
    EXPECT_TRUE(AnyLineContains(p2->lines, "hello there"));
}

TEST(CommandManager, UnknownCommandReportsAndFails) {
    CommandManager cm(nullptr);
    cm.Initialize();
    auto c = MakeCtx(CommandLevel::Console);
    EXPECT_FALSE(cm.Execute(c->ctx, "definitely_not_a_command"));
    EXPECT_TRUE(AnyLineContains(c->lines, "Unknown command"));
}

TEST(CommandManager, HelpListsOnlyPermittedCommands) {
    CommandManager cm(nullptr);
    cm.Initialize();

    auto player = MakeCtx(CommandLevel::Player);
    EXPECT_TRUE(cm.Execute(player->ctx, "help"));
    EXPECT_TRUE(AnyLineContains(player->lines, "ping"));
    EXPECT_TRUE(AnyLineContains(player->lines, "help"));
    // A Moderator-only command must not appear for a Player.
    EXPECT_FALSE(AnyLineContains(player->lines, "kick"));

    auto admin = MakeCtx(CommandLevel::Admin);
    EXPECT_TRUE(cm.Execute(admin->ctx, "help"));
    EXPECT_TRUE(AnyLineContains(admin->lines, "kick"));
}

TEST(CommandManager, HelpDetailShowsUsage) {
    CommandManager cm(nullptr);
    cm.Initialize();
    auto c = MakeCtx(CommandLevel::Admin);
    EXPECT_TRUE(cm.Execute(c->ctx, "help ban"));
    EXPECT_TRUE(AnyLineContains(c->lines, "Usage"));
    EXPECT_TRUE(AnyLineContains(c->lines, "minutes"));
}

TEST(CommandManager, QueryEmitsMachineReadableSnapshot) {
    CommandManager cm(nullptr);
    cm.Initialize();
    auto c = MakeCtx(CommandLevel::Helper);
    EXPECT_TRUE(cm.Execute(c->ctx, "query"));
    EXPECT_TRUE(AnyLineContains(c->lines, "players="));
    EXPECT_TRUE(AnyLineContains(c->lines, "tickrate="));
}

TEST(CommandManager, RegisterOverridesExistingCommand) {
    CommandManager cm(nullptr);
    cm.Initialize();
    bool ran = false;
    cm.Register(CommandDef{"ping", {}, CommandLevel::Player, CommandCategory::Automation,
        "ping", "overridden",
        [&ran](CommandContext& ctx) { ran = true; ctx.Reply("override"); return true; }});
    auto c = MakeCtx(CommandLevel::Player);
    EXPECT_TRUE(cm.Execute(c->ctx, "ping"));
    EXPECT_TRUE(ran);
    EXPECT_TRUE(AnyLineContains(c->lines, "override"));
}

// --- SOAP transport pure helpers ---

TEST(RemoteAdminServer, ExtractTagParsesAndDecodes) {
    std::string xml =
        "<soap:Envelope><soap:Body><ExecuteCommand>"
        "<password>secret</password>"
        "<command>say hello &amp; &lt;world&gt;</command>"
        "</ExecuteCommand></soap:Body></soap:Envelope>";
    EXPECT_EQ(RemoteAdminServer::ExtractTag(xml, "password"), "secret");
    EXPECT_EQ(RemoteAdminServer::ExtractTag(xml, "command"), "say hello & <world>");
}

TEST(RemoteAdminServer, ExtractTagFailsClosedOnMalformedInput) {
    EXPECT_EQ(RemoteAdminServer::ExtractTag("", "command"), "");
    EXPECT_EQ(RemoteAdminServer::ExtractTag("<command>no close", "command"), "");
    EXPECT_EQ(RemoteAdminServer::ExtractTag("<commandextra>x</commandextra>", "command"), "");
    EXPECT_EQ(RemoteAdminServer::ExtractTag("<other>v</other>", "command"), "");
}

TEST(RemoteAdminServer, BuildSoapResponseEscapesOutput) {
    std::string r = RemoteAdminServer::BuildSoapResponse(true, "a<b>&c");
    EXPECT_TRUE(r.find("<ok>true</ok>") != std::string::npos);
    EXPECT_TRUE(r.find("a&lt;b&gt;&amp;c") != std::string::npos);

    std::string f = RemoteAdminServer::BuildSoapResponse(false, "nope");
    EXPECT_TRUE(f.find("<ok>false</ok>") != std::string::npos);
}

RS2V_TEST_MAIN()
