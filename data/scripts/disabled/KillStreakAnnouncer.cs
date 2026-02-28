// KillStreakAnnouncer.cs — Announces kill streaks, multi-kills, and first blood.
// Demonstrates: kill events, player tracking, timed state, chat broadcasting, scoring bonuses.

using System;
using System.Collections.Generic;

public class KillStreakAnnouncer
{
    private class PlayerStreak
    {
        public int CurrentStreak;
        public int BestStreak;
        public DateTime LastKillTime;
        public int MultiKillCount;
    }

    private static Dictionary<string, PlayerStreak> _streaks = new();
    private static bool _firstBloodClaimed = false;
    private const double MultiKillWindow = 5.0; // seconds

    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnPlayerKill", nameof(OnPlayerKill));
        ScriptHelpers.OnEvent("OnMatchStart", nameof(OnMatchStart));
        ScriptHelpers.Log("[KillStreakAnnouncer] Initialized");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnPlayerKill", nameof(OnPlayerKill));
        ScriptHelpers.OffEvent("OnMatchStart", nameof(OnMatchStart));
    }

    public static void OnMatchStart()
    {
        _firstBloodClaimed = false;
        _streaks.Clear();
    }

    public static void OnPlayerKill(string killerId, string victimId)
    {
        if (killerId == victimId) return; // ignore suicide

        string killerName = ScriptHelpers.PlayerName(killerId);

        // First blood
        if (!_firstBloodClaimed)
        {
            _firstBloodClaimed = true;
            ScriptHelpers.ChatAll($"FIRST BLOOD! {killerName} draws first blood!");
            ScriptHelpers.AddScore(killerId, 50);
        }

        // Get or create streak tracker
        if (!_streaks.ContainsKey(killerId))
            _streaks[killerId] = new PlayerStreak();

        var streak = _streaks[killerId];
        var now = DateTime.UtcNow;

        // Reset victim streak
        if (_streaks.ContainsKey(victimId))
        {
            var victimStreak = _streaks[victimId];
            if (victimStreak.CurrentStreak >= 5)
                ScriptHelpers.ChatAll($"{killerName} ended {ScriptHelpers.PlayerName(victimId)}'s {victimStreak.CurrentStreak}-kill streak!");
            victimStreak.CurrentStreak = 0;
        }

        // Update killer streak
        streak.CurrentStreak++;
        if (streak.CurrentStreak > streak.BestStreak)
            streak.BestStreak = streak.CurrentStreak;

        // Check multi-kill (kills within window)
        if ((now - streak.LastKillTime).TotalSeconds <= MultiKillWindow)
        {
            streak.MultiKillCount++;
            switch (streak.MultiKillCount)
            {
                case 2: ScriptHelpers.ChatAll($"DOUBLE KILL! {killerName}"); ScriptHelpers.AddScore(killerId, 25); break;
                case 3: ScriptHelpers.ChatAll($"TRIPLE KILL! {killerName}"); ScriptHelpers.AddScore(killerId, 50); break;
                case 4: ScriptHelpers.ChatAll($"QUAD KILL! {killerName}!"); ScriptHelpers.AddScore(killerId, 100); break;
                default: ScriptHelpers.ChatAll($"RAMPAGE! {killerName} ({streak.MultiKillCount}x kill)!"); ScriptHelpers.AddScore(killerId, 150); break;
            }
        }
        else streak.MultiKillCount = 1;

        streak.LastKillTime = now;

        // Announce kill streaks at milestones
        switch (streak.CurrentStreak)
        {
            case 5:  ScriptHelpers.ChatAll($"{killerName} is on a 5 KILL STREAK!"); break;
            case 10: ScriptHelpers.ChatAll($"{killerName} is UNSTOPPABLE! (10 kills)"); ScriptHelpers.AddScore(killerId, 100); break;
            case 15: ScriptHelpers.ChatAll($"{killerName} is GODLIKE! (15 kills)"); ScriptHelpers.AddScore(killerId, 200); break;
            case 20: ScriptHelpers.ChatAll($"{killerName} is LEGENDARY! (20 kills)"); ScriptHelpers.AddScore(killerId, 500); break;
            case 25: ScriptHelpers.ChatAll($"{killerName} is BEYOND GODLIKE! (25+ kills)"); ScriptHelpers.AddScore(killerId, 1000); break;
        }
    }
}
