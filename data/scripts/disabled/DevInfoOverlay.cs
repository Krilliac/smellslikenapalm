// DevInfoOverlay.cs — Draws a debug text overlay with live server stats.
// Demonstrates: debug drawing (DrawText), server info queries, recurring tasks, admin checks.

using System;

public static class DevInfoOverlay
{
    private const float X = 0, Y = 0, Z = 200;
    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.AdminOnly("SERVER", 1, () =>
        {
            ScriptHelpers.Log("[DevInfoOverlay] Overlay enabled");
            _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(2), DrawInfo);
        });
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
    }

    private static void DrawInfo()
    {
        int players = ScriptHelpers.Players();
        int maxPlayers = ScriptHelpers.MaxPlayerCount();
        int tickRate = ScriptHelpers.TickRate();
        float fps = ScriptHelpers.FrameRate();
        float cpu = ScriptHelpers.CpuUsage();
        long mem = ScriptHelpers.MemUsage();
        int avgPing = ScriptHelpers.AvgPing();
        float tickAvg = ScriptHelpers.Avg("tickTime", 1);
        string map = ScriptHelpers.CurrentMap();

        ScriptHelpers.DrawText(X, Y, Z,
            $"Players: {players}/{maxPlayers}", 2.5f, 1, 1, 0);
        ScriptHelpers.DrawText(X, Y, Z - 15,
            $"Map: {map} | Tick: {tickRate}Hz | FPS: {fps:0}", 2.5f, 1, 1, 0);
        ScriptHelpers.DrawText(X, Y, Z - 30,
            $"CPU: {cpu:0.0}% | Mem: {ScriptHelpers.FmtBytes(mem)} | Ping: {avgPing}ms", 2.5f, 0, 1, 1);
        ScriptHelpers.DrawText(X, Y, Z - 45,
            $"TickAvg: {tickAvg:0.00}ms | Uptime: {ScriptHelpers.FmtSpan(TimeSpan.FromSeconds(ScriptHelpers.Uptime()))}", 2.5f, 0, 1, 1);
    }
}
