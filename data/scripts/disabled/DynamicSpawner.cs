// DynamicSpawner.cs — Periodically spawns NPC patrol bots at predefined locations.
// Demonstrates: entity spawning, recurring scheduled tasks, admin checks.

using System;
using System.Collections.Generic;

public class DynamicSpawner
{
    private static readonly List<(float x, float y, float z)> _spawnPoints = new()
    {
        (100, 50, 10), (200, 75, 10), (150, 100, 10)
    };

    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.Log("[DynamicSpawner] Initialized, will spawn patrols every 60s");
        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(60), SpawnPatrol);
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
    }

    private static void SpawnPatrol()
    {
        int spawned = 0;
        foreach (var (x, y, z) in _spawnPoints)
        {
            if (ScriptHelpers.Spawn("NPC_PatrolBot", x, y, z))
            {
                spawned++;
                ScriptHelpers.Debug($"[DynamicSpawner] Spawned PatrolBot at ({x},{y},{z})");
            }
        }
        ScriptHelpers.Log($"[DynamicSpawner] Spawned {spawned} patrol bots");
    }
}
