using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

// Movement validation and anticheat checks already done on the Core side, including this for example/extra functionality.
public class MovementValidator
{
    // Native bindings for position and teleportation
    private static class Native
    {
        [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
        public static extern void TeleportPlayer(string playerId, float x, float y, float z);

        // Hook into the server’s tick or movement event
        [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
        public static extern void RegisterEventHandler(string eventName, string methodName);
    }

    // Configuration: max speed in units/sec
    private const float MaxSpeed = 600f;

    // Track last known positions and timestamps
    private static readonly Dictionary<string, (float x, float y, float z, DateTime time)> _lastPos
        = new Dictionary<string, (float, float, float, DateTime)>();

    public static void Initialize()
    {
        // Register for movement updates
        Native.RegisterEventHandler("OnPlayerMove", nameof(OnPlayerMove));
        ScriptHelpers.LogInfo("[C#] MovementValidator initialized");
    }

    // Called by the server each time a client reports a new position
    public static void OnPlayerMove(string playerId, string xStr, string yStr, string zStr)
    {
        if (!ScriptHelpers.HasAdminLevel(playerId, 0)) // check that player is connected
            return;

        if (!float.TryParse(xStr, out var x) ||
            !float.TryParse(yStr, out var y) ||
            !float.TryParse(zStr, out var z))
            return;

        var now = DateTime.UtcNow;
        if (_lastPos.TryGetValue(playerId, out var prev))
        {
            var dt = (now - prev.time).TotalSeconds;
            if (dt > 0)
            {
                // Compute distance moved
                var dx = x - prev.x;
                var dy = y - prev.y;
                var dz = z - prev.z;
                var dist = Math.Sqrt(dx*dx + dy*dy + dz*dz);

                // If speed exceeds limit, teleport back and log
                var speed = dist / dt;
                if (speed > MaxSpeed)
                {
                    ScriptHelpers.LogWarning(
                        $"[AntiCheat] {playerId} moved {dist:0.##}u in {dt:0.###}s → {speed:0.##}u/s (max {MaxSpeed})"
                    );
                    // Snap-back
                    Native.TeleportPlayer(playerId, prev.x, prev.y, prev.z);
                    return;
                }
            }
        }

        // Accept movement and update record
        _lastPos[playerId] = (x, y, z, now);
    }
}