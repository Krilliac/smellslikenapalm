// VehicleSpawnManager.cs — Manages vehicle spawning at fixed points with cooldown timers.
// Demonstrates: entity spawning with IDs, entity lifecycle, debug drawing, recurring tasks.

using System;
using System.Collections.Generic;

public class VehicleSpawnManager
{
    private class VehicleSpawn
    {
        public string VehicleClass;
        public float X, Y, Z;
        public int SpawnedEntityId;
        public DateTime CooldownUntil;
        public bool IsSpawned;
    }

    private static readonly List<VehicleSpawn> _spawnPoints = new()
    {
        new() { VehicleClass = "UH1_Huey", X = 500, Y = 200, Z = 5 },
        new() { VehicleClass = "M113_APC",  X = 300, Y = 100, Z = 5 },
        new() { VehicleClass = "T54_Tank",  X = -300, Y = 100, Z = 5 },
    };

    private static string _taskId;
    private const int RespawnCooldownSec = 120;

    public static void Initialize()
    {
        // Spawn initial vehicles
        foreach (var sp in _spawnPoints)
        {
            if (ScriptHelpers.SpawnWithId(sp.VehicleClass, sp.X, sp.Y, sp.Z, out int id))
            {
                sp.SpawnedEntityId = id;
                sp.IsSpawned = true;
                ScriptHelpers.Log($"[VehicleSpawnMgr] Spawned {sp.VehicleClass} (id={id}) at ({sp.X},{sp.Y},{sp.Z})");
            }
        }

        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(5), CheckVehicles);
        ScriptHelpers.Log("[VehicleSpawnManager] Initialized");
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
        foreach (var sp in _spawnPoints)
        {
            if (sp.IsSpawned) ScriptHelpers.DestroyEntity(sp.SpawnedEntityId);
        }
    }

    private static void CheckVehicles()
    {
        var now = DateTime.UtcNow;

        foreach (var sp in _spawnPoints)
        {
            if (sp.IsSpawned)
            {
                // Check if vehicle still exists
                if (!ScriptHelpers.EntityValid(sp.SpawnedEntityId))
                {
                    sp.IsSpawned = false;
                    sp.CooldownUntil = now.AddSeconds(RespawnCooldownSec);
                    ScriptHelpers.Log($"[VehicleSpawnMgr] {sp.VehicleClass} destroyed, respawn in {RespawnCooldownSec}s");
                }
                else
                {
                    // Draw marker above vehicle
                    var (ex, ey, ez) = ScriptHelpers.EntityPos(sp.SpawnedEntityId);
                    ScriptHelpers.DrawText(ex, ey, ez + 5, sp.VehicleClass, 6f, 0, 1, 0);
                }
            }
            else if (now >= sp.CooldownUntil)
            {
                // Respawn vehicle
                if (ScriptHelpers.SpawnWithId(sp.VehicleClass, sp.X, sp.Y, sp.Z, out int id))
                {
                    sp.SpawnedEntityId = id;
                    sp.IsSpawned = true;
                    ScriptHelpers.ChatAll($"[Vehicles] {sp.VehicleClass} has respawned!");
                    ScriptHelpers.Log($"[VehicleSpawnMgr] Respawned {sp.VehicleClass} (id={id})");
                }
            }
            else
            {
                // Draw respawn timer at spawn point
                int remaining = (int)(sp.CooldownUntil - now).TotalSeconds;
                ScriptHelpers.DrawText(sp.X, sp.Y, sp.Z + 3,
                    $"{sp.VehicleClass} respawn: {remaining}s", 6f, 1, 0.5f, 0);
                ScriptHelpers.DrawSphere(sp.X, sp.Y, sp.Z, 3f, 6f, 1, 0.3f, 0);
            }
        }
    }
}
