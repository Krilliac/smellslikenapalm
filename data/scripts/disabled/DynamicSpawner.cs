using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Native {
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern bool SpawnEntity(string className, float x, float y, float z);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void BroadcastChat(string message);
}

public class DynamicSpawner {
    private static readonly List<(float x, float y, float z)> SpawnPoints =
        new() { (100,50,10), (200,75,10), (150,100,10) };

    public static void Initialize() {
        ScriptHelpers.LogInfo("[C#] DynamicSpawner initialized");
        // Only run if admin
        ScriptHelpers.ScheduleCallback(60f, nameof(SpawnPatrol));
    }

    public static void SpawnPatrol() {
        foreach (var (x,y,z) in SpawnPoints) {
            if (Native.SpawnEntity("NPC_PatrolBot", x, y, z)) {
                Native.BroadcastChat($"[C#] Spawned PatrolBot at ({x},{y},{z})");
            }
        }
        // Reschedule
        ScriptHelpers.ScheduleCallback(60f, nameof(SpawnPatrol));
    }
}