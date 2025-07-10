using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void BroadcastChat(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void RegisterEventHandler(string eventName, string methodName);
}

public class PersistentLeaderboard
{
    private static Dictionary<string,int> _scores;
    private const string SaveKey = "leaderboard";

    public static void Initialize()
    {
        // Load persisted scores
        _scores = ScriptHelpers.LoadData<Dictionary<string,int>>(SaveKey, new Dictionary<string,int>());
        Native.BroadcastChat("[C#] Leaderboard loaded");
        Native.RegisterEventHandler("OnPlayerKill", nameof(OnPlayerKill));
        Native.RegisterEventHandler("OnServerShutdown", nameof(OnShutdown));
        ScriptHelpers.LogInfo("[C#] PersistentLeaderboard initialized");
    }

    public static void OnPlayerKill(string killerId)
    {
        if (!_scores.ContainsKey(killerId)) _scores[killerId] = 0;
        _scores[killerId]++;
        if (_scores[killerId] >= 5)
            BroadcastTop10();
    }

    private static void BroadcastTop10()
    {
        var top10 = _scores
            .OrderByDescending(kv => kv.Value)
            .Take(10)
            .Select(kv => $"{kv.Key}:{kv.Value}");
        Native.BroadcastChat("[C#] Top10 Kills → " + string.Join(", ", top10));
    }

    public static void OnShutdown()
    {
        // Persist to disk
        ScriptHelpers.SaveData(SaveKey, _scores);
        Native.BroadcastChat("[C#] Leaderboard saved");
        ScriptHelpers.LogInfo("[C#] Leaderboard persisted on shutdown");
    }
}