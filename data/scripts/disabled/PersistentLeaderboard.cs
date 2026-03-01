// PersistentLeaderboard.cs — Tracks kill counts persistently and broadcasts top-10 leaderboards.
// Demonstrates: JSON persistence (Save/Load), event handlers, player stats, sorted output.

using System;
using System.Collections.Generic;
using System.Linq;

public class PersistentLeaderboard
{
    private static Dictionary<string, int> _scores;
    private const string SaveKey = "leaderboard";

    public static void Initialize()
    {
        _scores = ScriptHelpers.Load<Dictionary<string, int>>(SaveKey, new Dictionary<string, int>());
        ScriptHelpers.ChatAll($"[Leaderboard] Loaded {_scores.Count} player records");
        ScriptHelpers.OnEvent("OnPlayerKill", nameof(OnPlayerKill));
        ScriptHelpers.OnEvent("OnServerShutdown", nameof(OnShutdown));
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log("[PersistentLeaderboard] Initialized");
    }

    public static void Cleanup()
    {
        OnShutdown();
        ScriptHelpers.OffEvent("OnPlayerKill", nameof(OnPlayerKill));
        ScriptHelpers.OffEvent("OnServerShutdown", nameof(OnShutdown));
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnPlayerKill(string killerId, string victimId)
    {
        if (!_scores.ContainsKey(killerId)) _scores[killerId] = 0;
        _scores[killerId]++;

        if (_scores[killerId] % 10 == 0)
            BroadcastTop10();
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.OnChatCommand(playerId, message, "top", (args, pid) => BroadcastTop10());
        ScriptHelpers.OnChatCommand(playerId, message, "rank", (args, pid) =>
        {
            int rank = _scores.OrderByDescending(kv => kv.Value)
                .Select((kv, i) => new { kv.Key, Index = i + 1 })
                .FirstOrDefault(x => x.Key == pid)?.Index ?? 0;
            int kills = _scores.ContainsKey(pid) ? _scores[pid] : 0;
            ScriptHelpers.ChatTo(pid, rank > 0
                ? $"Your rank: #{rank} with {kills} kills"
                : "No kills recorded yet");
        });
    }

    private static void BroadcastTop10()
    {
        var top = _scores
            .OrderByDescending(kv => kv.Value)
            .Take(10)
            .Select((kv, i) => $"#{i + 1} {ScriptHelpers.PlayerName(kv.Key)}:{kv.Value}");
        ScriptHelpers.ChatAll("[Leaderboard] " + string.Join(", ", top));
    }

    public static void OnShutdown()
    {
        ScriptHelpers.Save(SaveKey, _scores);
        ScriptHelpers.Log($"[PersistentLeaderboard] Saved {_scores.Count} records");
    }
}
