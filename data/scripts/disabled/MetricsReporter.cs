using System;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetCurrentTickRate();

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetPlayerCount();

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetScriptReloadCount();
}

public class MetricsReporter
{
    public static void Initialize()
    {
        ScriptHost.RegisterEventHandler("OnServerStart", nameof(Report));
        ScriptHelpers.LogInfo("[C#] MetricsReporter initialized: broadcasting every 60s");
    }

    public static void Report()
    {
        int ticks   = Native.GetCurrentTickRate();
        int players = Native.GetPlayerCount();
        int reloads = Native.GetScriptReloadCount();

        ScriptHelpers.BroadcastChat(
            $"[Metrics] TickRate={ticks}Hz Players={players} ScriptReloads={reloads}"
        );
        ScriptHelpers.ScheduleCallback(60f, nameof(Report));
    }
}