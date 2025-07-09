using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public static class Native {
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern bool SpawnEntity(string className, float x, float y, float z, out int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern float GetEntityHealth(int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void MoveEntityTo(int entityId, float x, float y, float z);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void RemoveEntity(int entityId);
}

public class AIDroneSquad {
    private enum State { Patrol, Pursue, Retreat }
    private class Drone { public int Id; public State State; public float LastState; }
    private static readonly List<Drone> drones = new();
    private static readonly List<(float x,float y,float z)> patrolPoints = new() {
        (0,0,10), (50,0,10), (50,50,10), (0,50,10)
    };

    public static void Initialize() {
        // Schedule the attack loop at 1-second intervals
        ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(1), AttackLoop);
    }

    private static void SpawnInitialDrones() {
        foreach (var p in patrolPoints)
            if (Native.SpawnEntity("AI_Drone", p.x, p.y, p.z, out int id))
                drones.Add(new Drone { Id = id, State = State.Patrol, LastState = Timestamp() });
    }

    private static void AttackLoop() {
        float now = Timestamp();
        foreach (var d in drones.ToArray()) {
            float health = Native.GetEntityHealth(d.Id);
            if (health < 20 && d.State != State.Retreat) {
                d.State = State.Retreat; d.LastState = now;
            }
            switch (d.State) {
                case State.Patrol:
                    var pt = patrolPoints[(int)(now/10)%patrolPoints.Count];
                    Native.MoveEntityTo(d.Id, pt.x, pt.y, pt.z);
                    break;
                case State.Pursue:
                    var target = FindNearestPlayer(Native.GetEntityPosition(d.Id));
                    Native.MoveEntityTo(d.Id, target.x, target.y, target.z);
                    break;
                case State.Retreat:
                    Native.MoveEntityTo(d.Id, 0,0,50);
                    if (now - d.LastState > 15)
                        RemoveDrone(d);
                    break;
            }
            if (d.State == State.Patrol && DetectPlayerInRange(d.Id, 30f)) {
                d.State = State.Pursue; d.LastState = now;
            }
        }
    }

    private static void RemoveDrone(Drone d) {
        Native.RemoveEntity(d.Id);
        drones.Remove(d);
    }

    // Placeholder implementations…
    private static bool DetectPlayerInRange(int id, float range) => range>0;
    private static (float x,float y,float z) FindNearestPlayer((float x,float y,float z) pos)
        => (pos.x+10, pos.y, pos.z);
    private static float Timestamp() =>
        DateTime.UtcNow.Second + DateTime.UtcNow.Minute*60;
}