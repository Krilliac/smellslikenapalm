// FactionBalancer.cs — Dynamically rebalances teams based on player count and average ping.
// Demonstrates: team management, player list iteration, ping queries, recurring tasks.

using System;
using System.Collections.Generic;
using System.Linq;

public static class FactionBalancer
{
    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.Log("[FactionBalancer] Initializing dynamic team balancing");
        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromMinutes(2), BalanceTeams);
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
    }

    private static void BalanceTeams()
    {
        int teamCount = ScriptHelpers.TeamCount();
        if (teamCount < 2) return;

        var players = ScriptHelpers.GetAllPlayers();
        if (players.Count == 0) return;

        var teamData = new Dictionary<int, (int count, float avgPing)>();
        for (int t = 0; t < teamCount; t++)
        {
            int count = ScriptHelpers.TeamPlayerCount(t);
            var teamPlayers = players.Where(p => ScriptHelpers.PlayerTeam(p) == t).ToList();
            float avgPing = teamPlayers.Count > 0
                ? teamPlayers.Average(p => (float)ScriptHelpers.PlayerPing(p))
                : 50f;
            teamData[t] = (count, avgPing);
        }

        float totalInverse = teamData.Sum(kv => 1f / Math.Max(kv.Value.avgPing, 1f));
        int totalPlayers = players.Count;

        foreach (var (teamId, data) in teamData)
        {
            int target = (int)Math.Round(
                (1f / Math.Max(data.avgPing, 1f)) / totalInverse * totalPlayers);
            target = Math.Max(target, 1);

            ScriptHelpers.ResizeTeam(teamId, target);
            string teamName = ScriptHelpers.TeamName(teamId);
            ScriptHelpers.ChatAll(
                $"[Balance] {teamName}: {data.count} -> target {target} (avg ping {data.avgPing:0}ms)");
        }

        ScriptHelpers.Log($"[FactionBalancer] Balanced {teamCount} teams across {totalPlayers} players");
    }
}
