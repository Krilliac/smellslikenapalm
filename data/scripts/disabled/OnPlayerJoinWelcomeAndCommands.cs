// OnPlayerJoinWelcomeAndCommands.cs — Welcomes players and provides basic chat commands.
// Demonstrates: event handlers, per-player chat, chat command parsing.

using System;

public class OnPlayerJoinWelcomeAndCommands
{
    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnPlayerJoin", nameof(OnPlayerJoin));
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChatMessage));
        ScriptHelpers.Log("[WelcomeAndCommands] Initialized");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnPlayerJoin", nameof(OnPlayerJoin));
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChatMessage));
    }

    public static void OnPlayerJoin(string playerId)
    {
        string name = ScriptHelpers.PlayerName(playerId);
        string serverName = ScriptHelpers.ServerNameStr();
        ScriptHelpers.ChatTo(playerId,
            $"Welcome to {serverName}, {name}! Type !help for available commands.");
    }

    public static void OnChatMessage(string playerId, string message)
    {
        ScriptHelpers.OnChatCommand(playerId, message, "time", HandleTime);
        ScriptHelpers.OnChatCommand(playerId, message, "help", HandleHelp);
        ScriptHelpers.OnChatCommand(playerId, message, "stats", HandleStats);
        ScriptHelpers.OnChatCommand(playerId, message, "ping", HandlePing);
    }

    private static void HandleTime(string[] args, string playerId)
    {
        ScriptHelpers.ChatTo(playerId,
            $"Server time: {DateTime.UtcNow:HH:mm:ss} UTC | Uptime: {ScriptHelpers.FmtSpan(TimeSpan.FromSeconds(ScriptHelpers.Uptime()))}");
    }

    private static void HandleHelp(string[] args, string playerId)
    {
        ScriptHelpers.ChatTo(playerId, "Commands: !time, !help, !stats, !ping");
    }

    private static void HandleStats(string[] args, string playerId)
    {
        int kills = ScriptHelpers.Kills(playerId);
        int deaths = ScriptHelpers.Deaths(playerId);
        int score = ScriptHelpers.Score(playerId);
        float kd = deaths > 0 ? (float)kills / deaths : kills;
        ScriptHelpers.ChatTo(playerId, $"K:{kills} D:{deaths} K/D:{kd:0.00} Score:{score}");
    }

    private static void HandlePing(string[] args, string playerId)
    {
        int ping = ScriptHelpers.PlayerPing(playerId);
        int avg = ScriptHelpers.AvgPing();
        ScriptHelpers.ChatTo(playerId, $"Your ping: {ping}ms (server avg: {avg}ms)");
    }
}
