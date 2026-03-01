// GameModeAndScoreOverrides.cs — Dynamically cycles score limits and handles match-end events.
// Demonstrates: config read/write, match events, scheduled callbacks, chat broadcasting.

using System;

public class GameModeAndScoreOverrides
{
    private static readonly int[] ScoreLimits = { 500, 750, 1000 };
    private static int _currentIndex = 0;
    private static string _taskId;

    public static void Initialize()
    {
        _taskId = ScriptHelpers.ScheduleOnce(TimeSpan.FromSeconds(30), AdvanceScoreLimit);
        ScriptHelpers.OnEvent("OnMatchEnd", nameof(OnMatchEnd));
        ScriptHelpers.Log("[GameModeAndScoreOverrides] Initialized, first score change in 30s");
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
        ScriptHelpers.OffEvent("OnMatchEnd", nameof(OnMatchEnd));
    }

    private static void AdvanceScoreLimit()
    {
        _currentIndex = (_currentIndex + 1) % ScoreLimits.Length;
        int newLimit = ScoreLimits[_currentIndex];
        ScriptHelpers.CfgSetInt("Game.ScoreLimit", newLimit);
        ScriptHelpers.ChatAll($"[ScoreOverride] Score limit set to {newLimit}!");
        ScriptHelpers.Log($"[GameModeAndScoreOverrides] Score limit changed to {newLimit}");
        _taskId = ScriptHelpers.ScheduleOnce(TimeSpan.FromMinutes(5), AdvanceScoreLimit);
    }

    public static void OnMatchEnd()
    {
        int limit = ScriptHelpers.CfgGetInt("Game.ScoreLimit", 1000);
        ScriptHelpers.ChatAll($"[ScoreOverride] Round ended at score limit {limit}. Good game!");
    }
}
