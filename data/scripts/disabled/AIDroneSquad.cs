// AIDroneSquad.cs — Spawns a squad of AI drone entities that patrol, pursue players, and retreat.
// Demonstrates: entity spawning, entity position/health queries, recurring tasks, state machines.

using System;
using System.Collections.Generic;

public class AIDroneSquad
{
    private enum State { Patrol, Pursue, Retreat }

    private class Drone
    {
        public int Id;
        public State State;
        public float LastStateTime;
        public int PatrolIndex;
    }

    private static readonly List<Drone> _drones = new();
    private static readonly List<(float x, float y, float z)> _patrolPoints = new()
    {
        (0, 0, 10), (50, 0, 10), (50, 50, 10), (0, 50, 10)
    };

    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.Log("[AIDroneSquad] Initializing drone squad");

        foreach (var pt in _patrolPoints)
        {
            if (ScriptHelpers.SpawnWithId("AI_Drone", pt.x, pt.y, pt.z, out int id))
            {
                _drones.Add(new Drone
                {
                    Id = id,
                    State = State.Patrol,
                    LastStateTime = Timestamp(),
                    PatrolIndex = _drones.Count
                });
                ScriptHelpers.Log($"[AIDroneSquad] Spawned drone entity {id} at ({pt.x},{pt.y},{pt.z})");
            }
        }

        _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(1), UpdateDrones);
        ScriptHelpers.Log($"[AIDroneSquad] {_drones.Count} drones active, update loop started");
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
        foreach (var d in _drones)
            ScriptHelpers.DestroyEntity(d.Id);
        _drones.Clear();
        ScriptHelpers.Log("[AIDroneSquad] Cleaned up all drones");
    }

    private static void UpdateDrones()
    {
        float now = Timestamp();

        foreach (var d in _drones.ToArray())
        {
            if (!ScriptHelpers.EntityValid(d.Id))
            {
                _drones.Remove(d);
                continue;
            }

            float health = ScriptHelpers.EntityHP(d.Id);

            // Transition to retreat if low health
            if (health < 20 && d.State != State.Retreat)
            {
                d.State = State.Retreat;
                d.LastStateTime = now;
                ScriptHelpers.Debug($"[AIDroneSquad] Drone {d.Id} retreating (hp={health:0})");
            }

            switch (d.State)
            {
                case State.Patrol:
                    var pt = _patrolPoints[d.PatrolIndex % _patrolPoints.Count];
                    ScriptHelpers.MoveEntity(d.Id, pt.x, pt.y, pt.z);

                    if (now - d.LastStateTime > 10)
                    {
                        d.PatrolIndex = (d.PatrolIndex + 1) % _patrolPoints.Count;
                        d.LastStateTime = now;
                    }

                    if (IsPlayerNearEntity(d.Id, 30f))
                    {
                        d.State = State.Pursue;
                        d.LastStateTime = now;
                        ScriptHelpers.Debug($"[AIDroneSquad] Drone {d.Id} pursuing player");
                    }
                    break;

                case State.Pursue:
                    var target = FindNearestPlayerPos(d.Id);
                    ScriptHelpers.MoveEntity(d.Id, target.x, target.y, target.z);

                    if (now - d.LastStateTime > 15 && !IsPlayerNearEntity(d.Id, 50f))
                    {
                        d.State = State.Patrol;
                        d.LastStateTime = now;
                    }
                    break;

                case State.Retreat:
                    ScriptHelpers.MoveEntity(d.Id, 0, 0, 50);
                    if (now - d.LastStateTime > 15)
                    {
                        ScriptHelpers.DestroyEntity(d.Id);
                        _drones.Remove(d);
                        ScriptHelpers.Log($"[AIDroneSquad] Drone {d.Id} destroyed after retreat");
                    }
                    break;
            }
        }
    }

    private static bool IsPlayerNearEntity(int entityId, float range)
    {
        var (ex, ey, ez) = ScriptHelpers.EntityPos(entityId);
        foreach (var pid in ScriptHelpers.GetAllPlayers())
        {
            var (px, py, pz) = ScriptHelpers.PlayerPos(pid);
            float dx = px - ex, dy = py - ey, dz = pz - ez;
            if (Math.Sqrt(dx * dx + dy * dy + dz * dz) <= range)
                return true;
        }
        return false;
    }

    private static (float x, float y, float z) FindNearestPlayerPos(int entityId)
    {
        var (ex, ey, ez) = ScriptHelpers.EntityPos(entityId);
        float bestDist = float.MaxValue;
        (float x, float y, float z) best = (ex, ey, ez);

        foreach (var pid in ScriptHelpers.GetAllPlayers())
        {
            var (px, py, pz) = ScriptHelpers.PlayerPos(pid);
            float dx = px - ex, dy = py - ey, dz = pz - ez;
            float dist = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = (px, py, pz);
            }
        }
        return best;
    }

    private static float Timestamp()
        => (float)(DateTime.UtcNow - DateTime.UnixEpoch).TotalSeconds;
}
