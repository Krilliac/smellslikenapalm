using System;
using System.Runtime.InteropServices;

public static class Native {
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetCurrentTickRate();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetPlayerCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetScriptReloadCount();
}

public class MetricsReporter {
    public static void Initialize() {
        // Schedule first report on server start
        ScriptHost.RegisterEventHandler("OnServerStart", nameof(Report));
    }

    public static void Report() {
        int ticks   = Native.GetCurrentTickRate();
        int players = Native.GetPlayerCount();
        int reloads = Native.GetScriptReloadCount();

        ScriptHelpers.BroadcastChat(
            $"[Metrics] TickRate={ticks}Hz Players={players} ScriptReloads={reloads}"
        );
        // Reschedule via helper
        ScriptHelpers.ScheduleCallback(60f, nameof(Report));
    }
}