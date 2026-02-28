// LogLevelController.cs — Admin chat command to change server log levels at runtime.
// Demonstrates: chat commands, config writes, admin permissions, event handlers.

using System;
using System.Collections.Generic;

public class LogLevelController
{
    private static readonly HashSet<string> ValidLevels = new(StringComparer.OrdinalIgnoreCase)
    {
        "trace", "debug", "info", "warn", "error"
    };

    public static void Initialize()
    {
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["log"] = HandleLogCommand
        });
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log("[LogLevelController] Initialized — use !log <module|all> <level>");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCmd(playerId, message);
    }

    private static void HandleLogCommand(string[] args, string playerId)
    {
        if (args.Length != 2)
        {
            ScriptHelpers.ChatTo(playerId, "Usage: !log <module|all> <trace|debug|info|warn|error>");
            return;
        }

        string module = args[0].Equals("all", StringComparison.OrdinalIgnoreCase) ? "" : args[0];
        string level = args[1].ToLower();

        if (!ValidLevels.Contains(level))
        {
            ScriptHelpers.ChatTo(playerId, $"Invalid level. Use: trace, debug, info, warn, error");
            return;
        }

        string configKey = string.IsNullOrEmpty(module)
            ? "Logging.GlobalLevel"
            : $"Logging.{module}.Level";
        ScriptHelpers.CfgSetString(configKey, level);
        ScriptHelpers.CfgSave();

        string target = string.IsNullOrEmpty(module) ? "all modules" : module;
        ScriptHelpers.ChatTo(playerId, $"Log level for '{target}' set to {level}");
        ScriptHelpers.Log($"[LogLevelController] {playerId} set log level: {target} -> {level}");
    }
}
