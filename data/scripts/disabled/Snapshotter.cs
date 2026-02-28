// Snapshotter.cs — Admin command that saves a JSON snapshot of current server state.
// Demonstrates: file I/O, player iteration, map/score queries, chat commands, JSON serialization.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

public class Snapshotter
{
    private static string SnapshotsDir;

    public static void Initialize()
    {
        SnapshotsDir = Path.Combine(ScriptHelpers.DataDir(), "snapshots");
        Directory.CreateDirectory(SnapshotsDir);
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log("[Snapshotter] Initialized — use !snapshot to save state");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.OnChatCommand(playerId, message, "snapshot", (args, pid) =>
        {
            if (!ScriptHelpers.RequireAdmin(pid, 2)) return;

            var players = ScriptHelpers.GetAllPlayers();
            var playerData = players.Select(p => new Dictionary<string, object>
            {
                ["id"] = p,
                ["name"] = ScriptHelpers.PlayerName(p),
                ["team"] = ScriptHelpers.PlayerTeam(p),
                ["kills"] = ScriptHelpers.Kills(p),
                ["deaths"] = ScriptHelpers.Deaths(p),
                ["score"] = ScriptHelpers.Score(p),
                ["ping"] = ScriptHelpers.PlayerPing(p),
                ["health"] = ScriptHelpers.PlayerHealth(p)
            }).ToList();

            // Gather team scores
            int teamCount = ScriptHelpers.TeamCount();
            var teamData = new List<Dictionary<string, object>>();
            for (int t = 0; t < teamCount; t++)
            {
                teamData.Add(new Dictionary<string, object>
                {
                    ["id"] = t,
                    ["name"] = ScriptHelpers.TeamName(t),
                    ["score"] = ScriptHelpers.TeamScore(t),
                    ["playerCount"] = ScriptHelpers.TeamPlayerCount(t)
                });
            }

            var snapshot = new Dictionary<string, object>
            {
                ["timestamp"] = DateTime.UtcNow.ToString("o"),
                ["map"] = ScriptHelpers.CurrentMap(),
                ["gameMode"] = ScriptHelpers.CurrentMode(),
                ["matchActive"] = ScriptHelpers.MatchActive(),
                ["matchTimeRemaining"] = ScriptHelpers.MatchTimeLeft(),
                ["serverName"] = ScriptHelpers.ServerNameStr(),
                ["serverVersion"] = ScriptHelpers.Version(),
                ["uptime"] = ScriptHelpers.Uptime(),
                ["tickRate"] = ScriptHelpers.TickRate(),
                ["players"] = playerData,
                ["teams"] = teamData
            };

            var filename = Path.Combine(SnapshotsDir,
                $"snapshot_{DateTime.UtcNow:yyyyMMdd_HHmmss}.json");

            try
            {
                var json = JsonSerializer.Serialize(snapshot,
                    new JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(filename, json);
                ScriptHelpers.ChatTo(pid, $"Snapshot saved: {Path.GetFileName(filename)}");
                ScriptHelpers.Log($"[Snapshotter] Snapshot created: {filename}");
            }
            catch (Exception ex)
            {
                ScriptHelpers.ChatTo(pid, $"Snapshot error: {ex.Message}");
                ScriptHelpers.Error($"[Snapshotter] Failed: {ex.Message}");
            }
        });
    }
}
