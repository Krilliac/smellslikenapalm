using System;

public class EacMemoryHealthReader
{
    // Example address of health field in client memory
    private const ulong HealthAddress = 0x00ABCDEF1234;
    private const uint ReadLength = 4; // 4 bytes int

    public static void Initialize()
    {
        ScriptHelpers.AdminOnly("SERVER", 3, () =>
        {
            ScriptHelpers.LogInfo("[EAC] MemoryMonitor enabled");
            ScriptHelpers.ScheduleRecurring(TimeSpan.FromSeconds(5), MonitorHealth);
        });
    }

    private static void MonitorHealth()
    {
        // Read from client ID 1 for demonstration
        ScriptHelpers.ReadRemoteMemory(1, HealthAddress, ReadLength);
    }

    // Called by ScriptManager on reply
    // signature: void OnRemoteMemoryRead(string clientIdStr, string addrStr, string lenStr, string hexData)
    public static void OnRemoteMemoryRead(string clientIdStr, string addrStr, string lenStr, string hexData)
    {
        ScriptHelpers.LogInfo($"[EAC] Client {clientIdStr} HealthBytes@{addrStr}: {hexData}");
    }
}
