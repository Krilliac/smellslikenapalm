using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

public static class Native {
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern Dictionary<string,int> GetPlayerCountsPerTeam();

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern Dictionary<string,float> GetPlayerLatencies();

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SetTeamSize(string teamId, int size);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void BroadcastChat(string message);
}

public class FactionBalancer {
    public static void Initialize() {
        ScriptHelpers.LogInfo("[C#] FactionBalancer initializing dynamic balancing");
        ScriptHelpers.ScheduleRecurring(TimeSpan.FromMinutes(2), BalanceTeams);
    }

    public static void BalanceTeams() {
        var counts = Native.GetPlayerCountsPerTeam();
        var lats   = Native.GetPlayerLatencies();

        // Compute average latency per team
        var avgLat = counts.Keys.ToDictionary(
            team => team,
            team => lats.Where(kv => kv.Key.StartsWith(team))
                        .Select(kv => kv.Value)
                        .DefaultIfEmpty(50f)
                        .Average()
        );

        float totalInv = avgLat.Sum(kv => 1f / Math.Max(kv.Value,1f));
        int totalPlayers = counts.Values.Sum();

        foreach (var team in counts.Keys) {
            int target = (int)Math.Round(
                (1f / Math.Max(avgLat[team],1f)) / totalInv * totalPlayers
            );
            Native.SetTeamSize(team, target);
            Native.BroadcastChat(
                $"[C#] {team} size → {target} (avg latency {avgLat[team]:0}ms)"
            );
        }
    }
}