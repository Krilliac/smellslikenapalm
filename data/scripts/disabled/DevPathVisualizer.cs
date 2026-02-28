// DevPathVisualizer.cs — Draws debug lines and spheres to visualize patrol/navigation paths.
// Demonstrates: debug drawing (DrawLine, DrawSphere, DrawArrow), admin checks, recurring tasks.

using System;
using System.Collections.Generic;

public static class DevPathVisualizer
{
    private static readonly List<(float x, float y, float z)> PathNodes = new()
    {
        (0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)
    };

    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.AdminOnly("SERVER", 2, () =>
        {
            ScriptHelpers.Log("[DevPathVisualizer] Path visualization enabled");
            _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(1), DrawPath);
        });
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
        ScriptHelpers.ClearDrawings();
    }

    private static void DrawPath()
    {
        for (int i = 0; i < PathNodes.Count; i++)
        {
            var a = PathNodes[i];
            var b = PathNodes[(i + 1) % PathNodes.Count];

            ScriptHelpers.DrawLine(
                a.x, a.y, a.z + 10, b.x, b.y, b.z + 10,
                duration: 1.5f, thickness: 2f, r: 0, g: 1, b: 0);

            ScriptHelpers.DrawArrow(
                a.x, a.y, a.z + 10, b.x, b.y, b.z + 10,
                duration: 1.5f, thickness: 1f, r: 0, g: 1, b: 1);

            ScriptHelpers.DrawSphere(
                a.x, a.y, a.z + 10,
                radius: 2f, duration: 1.5f, r: 1, g: 0.5f, b: 0);

            ScriptHelpers.DrawText(
                a.x, a.y, a.z + 15, $"Node {i}",
                duration: 1.5f, r: 1, g: 1, b: 1);
        }
    }
}
