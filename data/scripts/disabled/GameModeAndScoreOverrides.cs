using System;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SetConfigInt(string key, int value);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetConfigInt(string key, int defaultValue);
}

public class ModeSwitcher
{
    private static readonly int[] scoreLimits = { 500, 750, 1000 };
    private static int currentIndex = 0;

    public static void Initialize()
    {
        // Kick off dynamic score changes after 30s
        ScriptHelpers.ScheduleCallback(30f, nameof(AdvanceScoreLimit));
        ScriptHost.RegisterEventHandler("OnMatchEnd", nameof(OnMatchEnd));
        ScriptHelpers.LogInfo("[C#] ModeSwitcher initialized");
    }

    public static void AdvanceScoreLimit()
    {
        currentIndex = (currentIndex + 1) % scoreLimits.Length;
        int newLimit = scoreLimits[currentIndex];
        Native.SetConfigInt("Game.ScoreLimit", newLimit);
        ScriptHelpers.BroadcastChat($"[C#] Score limit dynamically set to {newLimit}!");
        ScriptHelpers.ScheduleCallback(300f, nameof(AdvanceScoreLimit));
    }

    public static void OnMatchEnd()
    {
        int limit = Native.GetConfigInt("Game.ScoreLimit", 1000);
        ScriptHelpers.BroadcastChat($"[C#] Last round ended at score limit {limit}. Good game!");
    }
}