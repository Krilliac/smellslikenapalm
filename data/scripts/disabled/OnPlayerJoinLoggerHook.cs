using System;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void LogInfo(string message);
}

public class PlayerJoinLogger
{
    public static void Initialize()
    {
        ScriptHost.RegisterEventHandler("OnPlayerJoin", nameof(HandlePlayerJoin));
        ScriptHelpers.LogInfo("[C#] PlayerJoinLogger initialized");
    }

    public static void HandlePlayerJoin(string steamId)
    {
        ScriptHelpers.LogInfo($"[C#] Player joined: {steamId}");
    }
}