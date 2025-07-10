using System;
using System.Runtime.InteropServices;

public class EacMemoryHealthWriter
{
    // Example address of health in client memory
    private const ulong HealthAddr = 0x00ABCDEF1234;

    public static void Initialize()
    {
        // Register chat command: !writehealth <clientId> <newHealth>
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["writehealth"] = HandleWriteHealth
        });
        ScriptHost.RegisterEventHandler("OnChatMessage", nameof(OnChat));
        ScriptHelpers.LogInfo("[EAC] MemoryWriter initialized");
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCommand(playerId, message);
    }

    private static void HandleWriteHealth(string[] args, string adminId)
    {
        if (args.Length != 2 ||
            !uint.TryParse(args[0], out var clientId) ||
            !int.TryParse(args[1], out var newHealth))
        {
            ScriptHelpers.SendChatToPlayer(adminId, "Usage: !writehealth <clientId> <value>");
            return;
        }

        // Convert int to bytes (little-endian)
        var buf = BitConverter.GetBytes(newHealth);
        if (!BitConverter.IsLittleEndian)
            Array.Reverse(buf);

        ScriptHelpers.WriteRemoteMemory(clientId, HealthAddr, buf);
        ScriptHelpers.SendChatToPlayer(adminId, $"Write requested: client {clientId}, health={newHealth}");
    }
}