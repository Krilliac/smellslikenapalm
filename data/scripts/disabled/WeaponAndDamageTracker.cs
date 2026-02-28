// WeaponAndDamageTracker.cs — Tracks weapon fire events, damage dealt, and kill statistics per weapon.
// Demonstrates: kill/damage event handling, weapon queries, player stats, persistence, chat commands.

using System;
using System.Collections.Generic;
using System.Linq;

public class WeaponAndDamageTracker
{
    private class WeaponStat
    {
        public int Kills;
        public int Headshots;
        public float TotalDamage;
        public float LongestKillDist;
    }

    private static Dictionary<string, Dictionary<string, WeaponStat>> _playerWeaponStats;
    private const string SaveKey = "weapon_stats";

    public static void Initialize()
    {
        _playerWeaponStats = ScriptHelpers.Load(SaveKey,
            new Dictionary<string, Dictionary<string, WeaponStat>>());

        ScriptHelpers.OnEvent("OnPlayerKill", nameof(OnPlayerKill));
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.OnEvent("OnServerShutdown", nameof(OnShutdown));
        ScriptHelpers.Log("[WeaponAndDamageTracker] Initialized — use !weaponstats, !topweapon");
    }

    public static void Cleanup()
    {
        OnShutdown();
        ScriptHelpers.OffEvent("OnPlayerKill", nameof(OnPlayerKill));
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.OffEvent("OnServerShutdown", nameof(OnShutdown));
    }

    public static void OnPlayerKill(string killerId, string victimId)
    {
        string weapon = ScriptHelpers.PrimaryWeapon(killerId);
        if (string.IsNullOrEmpty(weapon)) weapon = "Unknown";

        if (!_playerWeaponStats.ContainsKey(killerId))
            _playerWeaponStats[killerId] = new Dictionary<string, WeaponStat>();
        if (!_playerWeaponStats[killerId].ContainsKey(weapon))
            _playerWeaponStats[killerId][weapon] = new WeaponStat();

        var stat = _playerWeaponStats[killerId][weapon];
        stat.Kills++;

        // Check kill distance
        var (kx, ky, kz) = ScriptHelpers.PlayerPos(killerId);
        var (vx, vy, vz) = ScriptHelpers.PlayerPos(victimId);
        float dx = kx - vx, dy = ky - vy, dz = kz - vz;
        float dist = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > stat.LongestKillDist)
            stat.LongestKillDist = dist;
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.OnChatCommand(playerId, message, "weaponstats", (args, pid) =>
        {
            if (!_playerWeaponStats.ContainsKey(pid) || _playerWeaponStats[pid].Count == 0)
            {
                ScriptHelpers.ChatTo(pid, "No weapon stats recorded yet.");
                return;
            }

            var top = _playerWeaponStats[pid]
                .OrderByDescending(kv => kv.Value.Kills)
                .Take(5)
                .Select(kv => $"{kv.Key}:{kv.Value.Kills}k");
            ScriptHelpers.ChatTo(pid, "Your weapons: " + string.Join(", ", top));
        });

        ScriptHelpers.OnChatCommand(playerId, message, "topweapon", (args, pid) =>
        {
            // Global weapon kill counts
            var globalWeapons = new Dictionary<string, int>();
            foreach (var player in _playerWeaponStats.Values)
                foreach (var (weapon, stat) in player)
                {
                    if (!globalWeapons.ContainsKey(weapon)) globalWeapons[weapon] = 0;
                    globalWeapons[weapon] += stat.Kills;
                }

            var top = globalWeapons
                .OrderByDescending(kv => kv.Value)
                .Take(5)
                .Select(kv => $"{kv.Key}:{kv.Value}");
            ScriptHelpers.ChatAll("[TopWeapons] " + string.Join(", ", top));
        });

        ScriptHelpers.OnChatCommand(playerId, message, "longestshot", (args, pid) =>
        {
            float longest = 0;
            string weapon = "";
            if (_playerWeaponStats.ContainsKey(pid))
            {
                foreach (var (w, s) in _playerWeaponStats[pid])
                {
                    if (s.LongestKillDist > longest)
                    {
                        longest = s.LongestKillDist;
                        weapon = w;
                    }
                }
            }
            ScriptHelpers.ChatTo(pid, longest > 0
                ? $"Your longest kill: {longest:0}m with {weapon}"
                : "No kills recorded yet.");
        });
    }

    public static void OnShutdown()
    {
        ScriptHelpers.Save(SaveKey, _playerWeaponStats);
        ScriptHelpers.Log("[WeaponAndDamageTracker] Stats saved");
    }
}
