using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern string GetCurrentMap();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern Dictionary<string,int> GetPlayerScores();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern List<string> GetConnectedPlayers();
}

public class Snapshotter
{
    private static readonly string SnapshotsDir =
        Path.Combine(ScriptHelpers.GetDataDirectory(), "snapshots");

    public static void Initialize()
    {
        Directory.CreateDirectory(SnapshotsDir);
        ScriptHost.RegisterEventHandler("OnChatMessage", nameof(OnChat));
        ScriptHelpers.LogInfo("[C#] Snapshotter initialized");
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.OnChatCommand(playerId, message, "snapshot", (args, pid) =>
        {
            var filename = Path.Combine(
                SnapshotsDir,
                $"snapshot_{DateTime.UtcNow:yyyyMMdd_HHmmss}.json"
            );
            var data = new
            {
                map     = Native.GetCurrentMap(),
                time    = DateTime.UtcNow,
                players = Native.GetConnectedPlayers(),
                scores  = Native.GetPlayerScores()
            };
            try
            {
                var json = JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(filename, json);
                ScriptHelpers.SendChatToPlayer(pid, $"Snapshot saved to {filename}");
                ScriptHelpers.LogInfo($"Snapshot created: {filename}");
            }
            catch (Exception ex)
            {
                ScriptHelpers.SendChatToPlayer(pid, $"Snapshot error: {ex.Message}");
                ScriptHelpers.LogError($"Snapshot failed: {ex.Message}");
            }
        });
    }
}