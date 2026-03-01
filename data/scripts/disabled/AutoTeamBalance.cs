// AutoTeamBalance.cs — Automatically balances teams when player count difference exceeds a threshold.
// Demonstrates: player join/leave events, team queries, player reassignment, throttling.

using System;
using System.Collections.Generic;
using System.Linq;

public class AutoTeamBalance
{
    private const int MaxDifference = 2;

    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnPlayerJoin", nameof(OnPlayerJoin));
        ScriptHelpers.OnEvent("OnPlayerLeave", nameof(OnPlayerLeave));
        ScriptHelpers.Log($"[AutoTeamBalance] Initialized, max team difference: {MaxDifference}");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnPlayerJoin", nameof(OnPlayerJoin));
        ScriptHelpers.OffEvent("OnPlayerLeave", nameof(OnPlayerLeave));
    }

    public static void OnPlayerJoin(string playerId)
    {
        // Auto-assign to smallest team
        int teamCount = ScriptHelpers.TeamCount();
        if (teamCount < 2) return;

        int smallestTeam = 0;
        int smallestCount = int.MaxValue;
        for (int t = 0; t < teamCount; t++)
        {
            int count = ScriptHelpers.TeamPlayerCount(t);
            if (count < smallestCount)
            {
                smallestCount = count;
                smallestTeam = t;
            }
        }

        ScriptHelpers.SetTeam(playerId, smallestTeam);
        ScriptHelpers.Debug($"[AutoTeamBalance] Assigned {playerId} to {ScriptHelpers.TeamName(smallestTeam)}");
    }

    public static void OnPlayerLeave(string playerId)
    {
        if (!ScriptHelpers.Throttle("autobalance", 10)) return;
        CheckBalance();
    }

    private static void CheckBalance()
    {
        int teamCount = ScriptHelpers.TeamCount();
        if (teamCount < 2) return;

        var counts = new int[teamCount];
        for (int t = 0; t < teamCount; t++)
            counts[t] = ScriptHelpers.TeamPlayerCount(t);

        int max = counts.Max();
        int min = counts.Min();
        int maxTeam = Array.IndexOf(counts, max);
        int minTeam = Array.IndexOf(counts, min);

        if (max - min <= MaxDifference) return;

        // Move players from largest to smallest team
        int toMove = (max - min) / 2;
        var players = ScriptHelpers.GetAllPlayers()
            .Where(p => ScriptHelpers.PlayerTeam(p) == maxTeam)
            .OrderBy(p => ScriptHelpers.Score(p))  // move lowest-score players first
            .Take(toMove);

        foreach (var p in players)
        {
            ScriptHelpers.SetTeam(p, minTeam);
            ScriptHelpers.ChatTo(p, $"You have been moved to {ScriptHelpers.TeamName(minTeam)} for balance.");
        }

        ScriptHelpers.ChatAll($"[Balance] Teams rebalanced: {toMove} player(s) moved to {ScriptHelpers.TeamName(minTeam)}");
        ScriptHelpers.Log($"[AutoTeamBalance] Moved {toMove} players from team {maxTeam} to {minTeam}");
    }
}
