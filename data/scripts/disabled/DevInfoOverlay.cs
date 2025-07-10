using System;
using System.Runtime.InteropServices;
using System.Linq;

public static class DevInfoOverlay
{
    private const float X = 0, Y = 0, Z = 200; // world location above spawn

    public static void Initialize()
    {
        ScriptHelpers.AdminOnly("SERVER", 1, () =>
        {
            ScriptHelpers.LogInfo("[Dev] InfoOverlay enabled");
            ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(2), DrawInfo);
        });
    }

    private static void DrawInfo()
    {
        int players = GetPlayerCount();
        float tickAvg = ScriptHelpers.GetMetricAverage("tickTime", 1);

        string msg1 = $"Players: {players}";
        string msg2 = $"TickAvg(ms): {tickAvg:0.00}";

        ScriptHelpers.DebugDrawText(X, Y, Z, msg1, 2f, 1, 1, 0);
        ScriptHelpers.DebugDrawText(X, Y, Z - 20, msg2, 2f, 1, 1, 0);
    }

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerCount();
}