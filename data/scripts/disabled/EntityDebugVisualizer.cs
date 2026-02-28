// EntityDebugVisualizer.cs — Draws debug visualizations around all active entities.
// Demonstrates: entity iteration, position queries, debug drawing (sphere, text, box), admin checks.

using System;

public static class EntityDebugVisualizer
{
    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.AdminOnly("SERVER", 2, () =>
        {
            ScriptHelpers.Log("[EntityDebugVisualizer] Enabled");
            _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(1), DrawEntities);
        });
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
    }

    private static void DrawEntities()
    {
        int count = ScriptHelpers.EntityCount();
        if (count == 0) return;

        // Draw info about entity types present
        int droneCount = ScriptHelpers.EntityCountOf("AI_Drone");
        int vehicleCount = ScriptHelpers.EntityCountOf("UH1_Huey") +
                           ScriptHelpers.EntityCountOf("M113_APC") +
                           ScriptHelpers.EntityCountOf("T54_Tank");
        int npcCount = ScriptHelpers.EntityCountOf("NPC_PatrolBot");

        ScriptHelpers.DrawText(0, 0, 280,
            $"Entities: {count} total | Drones:{droneCount} Vehicles:{vehicleCount} NPCs:{npcCount}",
            1.5f, 1, 0.8f, 0);

        // Draw spheres and labels for players
        foreach (var pid in ScriptHelpers.GetAllPlayers())
        {
            var (px, py, pz) = ScriptHelpers.PlayerPos(pid);
            int hp = ScriptHelpers.PlayerHealth(pid);
            string name = ScriptHelpers.PlayerName(pid);
            int team = ScriptHelpers.PlayerTeam(pid);

            // Team-colored sphere
            float r = team == 0 ? 0 : 1;
            float g = team == 0 ? 0.5f : 0;
            float b = team == 0 ? 1 : 0;

            ScriptHelpers.DrawSphere(px, py, pz + 2.5f, 0.5f, 1.5f, r, g, b);
            ScriptHelpers.DrawText(px, py, pz + 3.5f,
                $"{name} HP:{hp}", 1.5f, 1, 1, 1);
        }
    }
}
