using System;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SetLogLevel(string module, string level);
}

public class LogLevelController
{
    public static void Initialize()
    {
        // Register !log command and require admin
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["log"] = HandleLogCommand
        });
        ScriptHost.RegisterEventHandler("OnChatMessage", nameof(OnChat));
        ScriptHelpers.LogInfo("[C#] LogLevelController initialized");
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCommand(playerId, message);
    }

    private static void HandleLogCommand(string[] args, string playerId)
    {
        if (args.Length != 2)
        {
            ScriptHelpers.SendChatToPlayer(playerId, "Usage: !log <module|all> <trace|debug|info|warn|error>");
            return;
        }

        string module = args[0].Equals("all", StringComparison.OrdinalIgnoreCase) ? "" : args[0];
        string level  = args[1];
        Native.SetLogLevel(module, level);
        ScriptHelpers.SendChatToPlayer(playerId, $"Log level for '{(string.IsNullOrEmpty(module) ? "all" : module)}' set to {level}");
    }
}