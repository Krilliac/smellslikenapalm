using System;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SendChatToPlayer(string playerId, string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void BroadcastChat(string message);
}

public class WelcomeAndCommands
{
    public static void Initialize()
    {
        ScriptHost.RegisterEventHandler("OnPlayerJoin", nameof(OnPlayerJoin));
        ScriptHost.RegisterEventHandler("OnChatMessage", nameof(OnChatMessage));
        ScriptHelpers.LogInfo("[C#] WelcomeAndCommands initialized");
    }

    public static void OnPlayerJoin(string playerId)
    {
        Native.SendChatToPlayer(playerId, "Welcome to the server! Type !time to get the server time.");
    }

    public static void OnChatMessage(string playerId, string message)
    {
        // Handle !time command
        ScriptHelpers.OnChatCommand(playerId, message, "time", HandleTimeCommand);
    }

    private static void HandleTimeCommand(string[] args, string playerId)
    {
        string time = DateTime.UtcNow.ToString("HH:mm:ss") + " UTC";
        Native.SendChatToPlayer(playerId, $"Server time is {time}");
    }
}