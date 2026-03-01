// EacMemoryHealthWriter.cs — Admin command to write health values to client memory via EAC.
// Demonstrates: EAC memory write, chat commands, byte conversion, admin permissions.

using System;
using System.Collections.Generic;

public class EacMemoryHealthWriter
{
    private const ulong HealthAddr = 0x00ABCDEF1234;

    public static void Initialize()
    {
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["writehealth"] = HandleWriteHealth
        });
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log("[EacMemoryHealthWriter] Initialized — use !writehealth <clientId> <value>");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCmd(playerId, message);
    }

    private static void HandleWriteHealth(string[] args, string adminId)
    {
        if (args.Length != 2 ||
            !uint.TryParse(args[0], out var clientId) ||
            !int.TryParse(args[1], out var newHealth))
        {
            ScriptHelpers.ChatTo(adminId, "Usage: !writehealth <clientId> <value>");
            return;
        }

        var buf = BitConverter.GetBytes(newHealth);
        if (!BitConverter.IsLittleEndian)
            Array.Reverse(buf);

        ScriptHelpers.WriteMem(clientId, HealthAddr, buf);
        ScriptHelpers.ChatTo(adminId, $"Write requested: client {clientId}, health={newHealth}");
    }

    public static void OnRemoteMemoryWriteAck(string clientIdStr, string successStr)
    {
        ScriptHelpers.Log($"[EacMemoryHealthWriter] Write ack from client {clientIdStr}: {successStr}");
    }
}
