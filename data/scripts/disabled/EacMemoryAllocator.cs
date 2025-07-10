using System;
using System.Runtime.InteropServices;

public class EacMemoryAllocator
{
    // Example small machine-code sequence (NOP sled)
    private static readonly byte[] Stub = new byte[16]; 

    public static void Initialize()
    {
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["allocstub"] = HandleAllocStub
        });
        ScriptHost.RegisterEventHandler("OnChatMessage", nameof(OnChat));
        ScriptHelpers.LogInfo("[EAC] MemoryAllocator initialized");
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCommand(playerId, message);
    }

    private static void HandleAllocStub(string[] args, string adminId)
    {
        if (args.Length != 1 || !uint.TryParse(args[0], out var clientId))
        {
            ScriptHelpers.SendChatToPlayer(adminId, "Usage: !allocstub <clientId>");
            return;
        }

        // Request remote allocation with RXW permissions (0x40|0x20|0x10)
        const ulong PAGE_EXECUTE_READWRITE = 0x40 | 0x20 | 0x10;
        ScriptHelpers.AllocateRemoteMemory(clientId, (uint)Stub.Length, PAGE_EXECUTE_READWRITE);
        ScriptHelpers.SendChatToPlayer(adminId, $"Alloc request sent for client {clientId}");
    }

    // This method must exist for ScriptManager to call back on allocation
    // Signature in your ScriptManager: void OnRemoteMemoryAlloc(uint clientId, ulong baseAddr)
    public static void OnRemoteMemoryAlloc(string clientIdStr, string baseAddrStr)
    {
        ScriptHelpers.LogInfo($"[EAC] Client {clientIdStr} allocated at 0x{baseAddrStr}");
        // Optionally, send the stub bytes immediately after allocation
        if (uint.TryParse(clientIdStr, out var cid) && 
            ulong.TryParse(baseAddrStr, System.Globalization.NumberStyles.HexNumber, null, out var addr))
        {
            ScriptHelpers.WriteRemoteMemory(cid, addr, Stub);
            ScriptHelpers.SendChatToPlayer(clientIdStr, $"Stub written to 0x{addr:X}");
        }
    }
}