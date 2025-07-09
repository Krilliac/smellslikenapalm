using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Linq;

public static class Native {
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void BroadcastChat(string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void RegisterEventHandler(string eventName, string methodName);
}

public class Leaderboard {
    private static Dictionary<string,int> scores;
    private static readonly string saveFile = Path.Combine(ScriptHelpers.GetDataDirectory(), "leaderboard.json");

    public static void Initialize() {
        scores = ScriptHelpers.LoadData<Dictionary<string,int>>("leaderboard", new Dictionary<string,int>());
        Native.BroadcastChat("[C#] Leaderboard loaded");
        Native.RegisterEventHandler("OnPlayerKill", nameof(OnPlayerKill));
        Native.RegisterEventHandler("OnServerShutdown", nameof(OnShutdown));
    }

    public static void OnPlayerKill(string killerId) {
        if (!scores.ContainsKey(killerId)) scores[killerId] = 0;
        scores[killerId]++;
        if (scores[killerId] >= 5) BroadcastTop10();
    }

    public static void BroadcastTop10() {
        var top10 = scores
            .OrderByDescending(kv => kv.Value)
            .Take(10)
            .Select(kv => $"{kv.Key}:{kv.Value}");
        Native.BroadcastChat("[C#] Top10 Kills → " + string.Join(", ", top10));
    }

    public static void OnShutdown() {
        ScriptHelpers.SaveData("leaderboard", scores);
        Native.BroadcastChat("[C#] Leaderboard saved");
    }
}