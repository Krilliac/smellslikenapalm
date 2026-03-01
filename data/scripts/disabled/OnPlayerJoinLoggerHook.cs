// OnPlayerJoinLoggerHook.cs — Logs every player join event to the server log.
// Demonstrates: event handlers, player name queries, simple logging.

using System;

public class OnPlayerJoinLoggerHook
{
    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnPlayerJoin", nameof(HandlePlayerJoin));
        ScriptHelpers.Log("[OnPlayerJoinLoggerHook] Initialized");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnPlayerJoin", nameof(HandlePlayerJoin));
    }

    public static void HandlePlayerJoin(string steamId)
    {
        string name = ScriptHelpers.PlayerName(steamId);
        ScriptHelpers.Log($"[PlayerJoin] {name} ({steamId}) connected");
    }
}
