using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class DynamicSpawner
{
    private static readonly List<(float x, float y, float z)> spawnPoints = new()
    {
        (100, 50, 10), (200, 75, 10), (150, 100, 10)
    };

    private static class Native
    {
        [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
        public static extern bool SpawnEntity(string className, float x, float y, float z);

        [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
        public static extern void BroadcastChat(string message);
    }

    public static void Initialize()
    {
        ScriptHelpers.LogInfo("[C#] DynamicSpawner initialized");
        ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(60), SpawnPatrol);
    }

    public static void SpawnPatrol()
    {
        if (!ScriptHelpers.HasAdmin("SERVER")) return;

        foreach (var (x, y, z) in spawnPoints)
        {
            if (Native.SpawnEntity("NPC_PatrolBot", x, y, z))
                Native.BroadcastChat($"[C#] Spawned PatrolBot at ({x},{y},{z})");
        }
    }
}