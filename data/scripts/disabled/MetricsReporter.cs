// MetricsReporter.cs — Periodically broadcasts server metrics to chat and tracks them.
// Demonstrates: server info queries, performance monitoring, recurring tasks, metrics tracking.

using System;

public class MetricsReporter
{
    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnServerStart", nameof(OnServerStart));
        ScriptHelpers.Log("[MetricsReporter] Initialized, reporting every 60s after server start");
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
        ScriptHelpers.OffEvent("OnServerStart", nameof(OnServerStart));
    }

    public static void OnServerStart()
    {
        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(60), Report);
    }

    private static void Report()
    {
        int tickRate = ScriptHelpers.TickRate();
        int players = ScriptHelpers.Players();
        int reloads = ScriptHelpers.ScriptReloads();
        int pps = ScriptHelpers.PacketsPerSec();
        float fps = ScriptHelpers.FrameRate();
        float cpu = ScriptHelpers.CpuUsage();
        long mem = ScriptHelpers.MemUsage();
        int avgPing = ScriptHelpers.AvgPing();

        ScriptHelpers.Track("tickRate", tickRate);
        ScriptHelpers.Track("playerCount", players);
        ScriptHelpers.Track("avgPing", avgPing);
        ScriptHelpers.Track("cpu", cpu);

        ScriptHelpers.ChatAll(
            $"[Metrics] Tick={tickRate}Hz Players={players} FPS={fps:0} " +
            $"CPU={cpu:0.0}% Mem={ScriptHelpers.FmtBytes(mem)} " +
            $"PPS={pps} Ping={avgPing}ms Reloads={reloads}");
    }
}
