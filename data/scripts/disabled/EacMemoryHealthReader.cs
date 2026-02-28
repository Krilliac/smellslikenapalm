// EacMemoryHealthReader.cs — Periodically reads client memory health values via EAC.
// Demonstrates: EAC memory read, recurring scheduled tasks, admin permission checks.

using System;

public class EacMemoryHealthReader
{
    private const ulong HealthAddress = 0x00ABCDEF1234;
    private const uint ReadLength = 4;

    private static string _taskId;

    public static void Initialize()
    {
        ScriptHelpers.AdminOnly("SERVER", 3, () =>
        {
            ScriptHelpers.Log("[EacMemoryHealthReader] Health monitoring enabled");
            _taskId = ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(5), MonitorHealth);
        });
    }

    public static void Cleanup()
    {
        if (_taskId != null) ScriptHelpers.CancelTask(_taskId);
    }

    private static void MonitorHealth()
    {
        int clientCount = ScriptHelpers.ClientCount();
        for (int i = 0; i < clientCount; i++)
        {
            uint cid = ScriptHelpers.ClientId(i);
            ScriptHelpers.ReadMem(cid, HealthAddress, ReadLength);
        }
    }

    public static void OnRemoteMemoryRead(string clientIdStr, string addrStr, string lenStr, string hexData)
    {
        ScriptHelpers.Log($"[EacMemoryHealthReader] Client {clientIdStr} Health@0x{addrStr}: {hexData}");
    }
}
