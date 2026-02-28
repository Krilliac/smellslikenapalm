// PerformanceDashboard.cs — Tracks server performance over time and alerts on degradation.
// Demonstrates: metrics tracking, config thresholds, recurring tasks, debug drawing, alerts.

using System;

public class PerformanceDashboard
{
    private static string _taskId;
    private static int _tickWarnings = 0;
    private static int _memWarnings = 0;

    public static void Initialize()
    {
        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(10), CollectMetrics);
        ScriptHelpers.Log("[PerformanceDashboard] Initialized, collecting metrics every 10s");
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
    }

    private static void CollectMetrics()
    {
        float fps = ScriptHelpers.FrameRate();
        float cpu = ScriptHelpers.CpuUsage();
        long mem = ScriptHelpers.MemUsage();
        int tick = ScriptHelpers.TickRate();
        int pps = ScriptHelpers.PacketsPerSec();
        int avgPing = ScriptHelpers.AvgPing();
        int players = ScriptHelpers.Players();

        // Track all metrics for historical analysis
        ScriptHelpers.Track("perf.fps", fps);
        ScriptHelpers.Track("perf.cpu", cpu);
        ScriptHelpers.Track("perf.mem", mem / (1024f * 1024f));
        ScriptHelpers.Track("perf.tick", tick);
        ScriptHelpers.Track("perf.pps", pps);
        ScriptHelpers.Track("perf.avgPing", avgPing);
        ScriptHelpers.Track("perf.players", players);

        // Alert on performance degradation
        float warnFps = ScriptHelpers.CfgGetFloat("Perf.WarnFpsBelow", 30f);
        float warnCpu = ScriptHelpers.CfgGetFloat("Perf.WarnCpuAbove", 85f);
        long warnMem = (long)ScriptHelpers.CfgGetFloat("Perf.WarnMemAboveMB", 2048f) * 1024 * 1024;

        if (fps > 0 && fps < warnFps)
        {
            _tickWarnings++;
            if (_tickWarnings % 6 == 1) // alert every ~minute
                ScriptHelpers.Warn($"[PerfDashboard] Low FPS: {fps:0} (threshold: {warnFps:0})");
        }
        else _tickWarnings = 0;

        if (cpu > warnCpu)
            ScriptHelpers.Warn($"[PerfDashboard] High CPU: {cpu:0.0}% (threshold: {warnCpu:0}%)");

        if (mem > warnMem)
        {
            _memWarnings++;
            if (_memWarnings % 6 == 1)
                ScriptHelpers.Warn($"[PerfDashboard] High memory: {ScriptHelpers.FmtBytes(mem)} (threshold: {ScriptHelpers.FmtBytes(warnMem)})");
        }
        else _memWarnings = 0;

        // Draw 3D performance overlay at world origin
        ScriptHelpers.DrawText(0, 0, 250, $"FPS:{fps:0} CPU:{cpu:0}% Mem:{ScriptHelpers.FmtBytes(mem)}",
            12f, fps < warnFps ? 1 : 0, fps < warnFps ? 0 : 1, 0);
    }
}
