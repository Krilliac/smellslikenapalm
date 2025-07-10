using System;
using System.Collections.Generic;

public static class DevPathVisualizer
{
    // Simulated path node positions (replace with real data if available)
    private static readonly List<(float x, float y, float z)> PathNodes = new()
    {
        (0,0,0), (100,0,0), (100,100,0), (0,100,0)
    };

    public static void Initialize()
    {
        // Only admin developers
        ScriptHelpers.AdminOnly("SERVER", 2, () =>
        {
            ScriptHelpers.LogInfo("[Dev] PathVisualizer enabled");
            ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(1), DrawPath);
        });
    }

    private static void DrawPath()
    {
        for (int i = 0; i < PathNodes.Count; i++)
        {
            var a = PathNodes[i];
            var b = PathNodes[(i + 1) % PathNodes.Count];
            ScriptHelpers.DebugDrawLine(
                a.x, a.y, a.z + 10,
                b.x, b.y, b.z + 10,
                duration: 1f, thickness: 2f,
                r: 0, g: 1, b: 0
            );
        }
    }
}