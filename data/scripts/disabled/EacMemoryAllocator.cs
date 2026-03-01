// EacMemoryAllocator.cs — Admin command to request remote memory allocation on a client via EAC.
// Demonstrates: EAC memory allocation, chat commands, event handlers, admin permissions.

using System;
using System.Collections.Generic;

public class EacMemoryAllocator
{
    private static readonly byte[] Stub = new byte[16];

    public static void Initialize()
    {
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["allocstub"] = HandleAllocStub
        });
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log("[EacMemoryAllocator] Initialized — use !allocstub <clientId>");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCmd(playerId, message);
    }

    private static void HandleAllocStub(string[] args, string adminId)
    {
        if (args.Length != 1 || !uint.TryParse(args[0], out var clientId))
        {
            ScriptHelpers.ChatTo(adminId, "Usage: !allocstub <clientId>");
            return;
        }

        const ulong PAGE_EXECUTE_READWRITE = 0x40;
        ScriptHelpers.AllocMem(clientId, (uint)Stub.Length, PAGE_EXECUTE_READWRITE);
        ScriptHelpers.ChatTo(adminId, $"Alloc request sent for client {clientId}");
    }

    public static void OnRemoteMemoryAlloc(string clientIdStr, string baseAddrStr)
    {
        ScriptHelpers.Log($"[EacMemoryAllocator] Client {clientIdStr} allocated at 0x{baseAddrStr}");
        if (uint.TryParse(clientIdStr, out var cid) &&
            ulong.TryParse(baseAddrStr, out var addr))
        {
            ScriptHelpers.WriteMem(cid, addr, Stub);
            ScriptHelpers.Log($"[EacMemoryAllocator] Stub written to client {cid} at 0x{addr:X}");
        }
    }
}
