// MovementValidator.cs — Script-side speed check that snaps players back on impossible movement.
// Demonstrates: movement event handling, player position queries, teleportation, logging.
// Note: core anticheat does this natively; this is an example of augmenting it from scripts.

using System;
using System.Collections.Generic;

public class MovementValidator
{
    private const float MaxSpeed = 600f;

    private static readonly Dictionary<string, (float x, float y, float z, DateTime time)> _lastPos = new();

    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnPlayerMove", nameof(OnPlayerMove));
        ScriptHelpers.OnEvent("OnPlayerLeave", nameof(OnPlayerLeave));
        ScriptHelpers.Log("[MovementValidator] Initialized, max speed = " + MaxSpeed);
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnPlayerMove", nameof(OnPlayerMove));
        ScriptHelpers.OffEvent("OnPlayerLeave", nameof(OnPlayerLeave));
        _lastPos.Clear();
    }

    public static void OnPlayerMove(string playerId, string xStr, string yStr, string zStr)
    {
        if (!ScriptHelpers.IsOnline(playerId)) return;

        if (!float.TryParse(xStr, out var x) ||
            !float.TryParse(yStr, out var y) ||
            !float.TryParse(zStr, out var z))
            return;

        var now = DateTime.UtcNow;
        if (_lastPos.TryGetValue(playerId, out var prev))
        {
            var dt = (now - prev.time).TotalSeconds;
            if (dt > 0.001)
            {
                var dx = x - prev.x;
                var dy = y - prev.y;
                var dz = z - prev.z;
                var dist = Math.Sqrt(dx * dx + dy * dy + dz * dz);
                var speed = dist / dt;

                if (speed > MaxSpeed)
                {
                    ScriptHelpers.Warn(
                        $"[MovementValidator] {playerId} speed={speed:0}u/s " +
                        $"(dist={dist:0.#}u in {dt:0.###}s, max={MaxSpeed})");
                    ScriptHelpers.TeleportPlayer(playerId, prev.x, prev.y, prev.z);
                    return;
                }
            }
        }

        _lastPos[playerId] = (x, y, z, now);
    }

    public static void OnPlayerLeave(string playerId)
    {
        _lastPos.Remove(playerId);
    }
}
