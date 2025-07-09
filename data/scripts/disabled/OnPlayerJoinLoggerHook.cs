using System;
using System.Runtime.InteropServices;

public static class Native {
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void LogInfo(string message);
}

public class PlayerJoinLogger {
    public static void Initialize() {
        Native.LogInfo("[C#] PlayerJoinLogger initialized");
        ScriptHost.RegisterEventHandler("OnPlayerJoin", nameof(HandlePlayerJoin));
    }

    public static void HandlePlayerJoin(string steamId) {
        Native.LogInfo($"[C#] Player joined: {steamId}");
    }
}