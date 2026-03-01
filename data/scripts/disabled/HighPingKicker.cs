// HighPingKicker.cs — Warns and kicks players with consistently high ping.
// Demonstrates: player ping queries, recurring tasks, throttle, per-player warnings, config.

using System;
using System.Collections.Generic;

public class HighPingKicker
{
    private static Dictionary<string, int> _warnings = new();
    private static string _taskId;

    public static void Initialize()
    {
        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(30), CheckPings);
        ScriptHelpers.OnEvent("OnPlayerLeave", nameof(OnPlayerLeave));
        ScriptHelpers.Log("[HighPingKicker] Initialized");
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
        ScriptHelpers.OffEvent("OnPlayerLeave", nameof(OnPlayerLeave));
    }

    public static void OnPlayerLeave(string playerId)
    {
        _warnings.Remove(playerId);
    }

    private static void CheckPings()
    {
        int maxPing = ScriptHelpers.CfgGetInt("PingKicker.MaxPing", 350);
        int maxWarnings = ScriptHelpers.CfgGetInt("PingKicker.MaxWarnings", 3);

        foreach (var pid in ScriptHelpers.GetAllPlayers())
        {
            int ping = ScriptHelpers.PlayerPing(pid);
            if (ping <= maxPing)
            {
                // Reset warning counter on good ping
                if (_warnings.ContainsKey(pid) && _warnings[pid] > 0)
                    _warnings[pid] = Math.Max(0, _warnings[pid] - 1);
                continue;
            }

            if (!_warnings.ContainsKey(pid)) _warnings[pid] = 0;
            _warnings[pid]++;

            string name = ScriptHelpers.PlayerName(pid);
            if (_warnings[pid] >= maxWarnings)
            {
                ScriptHelpers.Kick(pid, $"High ping ({ping}ms, limit {maxPing}ms)");
                ScriptHelpers.ChatAll($"[Server] {name} kicked for high ping ({ping}ms)");
                ScriptHelpers.Log($"[HighPingKicker] Kicked {name} (ping={ping}ms, warnings={_warnings[pid]})");
                _warnings.Remove(pid);
            }
            else
            {
                ScriptHelpers.ChatTo(pid,
                    $"WARNING: Your ping is {ping}ms (limit: {maxPing}ms). " +
                    $"Warning {_warnings[pid]}/{maxWarnings}");
            }
        }
    }
}
