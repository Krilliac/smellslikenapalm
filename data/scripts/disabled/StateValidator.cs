using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Native
{
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void TeleportPlayer(string playerId, float x, float y, float z);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    public static extern void RegisterEventHandler(string eventName, string methodName);
}

public class StateValidator
{
    // Tracks when players are “frozen” or “stunned”
    private static HashSet<string> _frozen = new HashSet<string>();
    private static HashSet<string> _stunned = new HashSet<string>();

    // Last valid position stored per player
    private static Dictionary<string, (float x, float y, float z)> _lastPos
        = new Dictionary<string,(float,float,float)>();

    public static void Initialize()
    {
        // Hook state‐change and movement events
        Native.RegisterEventHandler("OnPlayerFrozen",  nameof(OnFrozen));
        Native.RegisterEventHandler("OnPlayerUnfrozen",nameof(OnUnfrozen));
        Native.RegisterEventHandler("OnPlayerStunned", nameof(OnStunned));
        Native.RegisterEventHandler("OnPlayerUnstunned",nameof(OnUnstunned));
        Native.RegisterEventHandler("OnPlayerMove",    nameof(OnMove));
        ScriptHelpers.LogInfo("[C#] StateValidator initialized");
    }

    public static void OnFrozen(string playerId)
    {
        _frozen.Add(playerId);
        ScriptHelpers.SendChatToPlayer(playerId, "You are now frozen and cannot move.");
    }

    public static void OnUnfrozen(string playerId)
    {
        _frozen.Remove(playerId);
    }

    public static void OnStunned(string playerId)
    {
        _stunned.Add(playerId);
        ScriptHelpers.SendChatToPlayer(playerId, "You are stunned and cannot act.");
    }

    public static void OnUnstunned(string playerId)
    {
        _stunned.Remove(playerId);
    }

    // Movement event parameters: playerId, x, y, z
    public static void OnMove(string playerId, string xs, string ys, string zs)
    {
        if (!float.TryParse(xs, out var x) ||
            !float.TryParse(ys, out var y) ||
            !float.TryParse(zs, out var z))
            return;

        // If frozen, revert movement
        if (_frozen.Contains(playerId))
        {
            if (_lastPos.TryGetValue(playerId, out var prev))
                Native.TeleportPlayer(playerId, prev.x, prev.y, prev.z);
            return;
        }

        // Otherwise update last valid position
        _lastPos[playerId] = (x, y, z);
    }

    // Example hook for actions (e.g. shooting) 
    // OnPlayerAction: playerId, actionName
    public static void OnPlayerAction(string playerId, string action)
    {
        if (_stunned.Contains(playerId))
        {
            // Cancel action—client will see no effect
            ScriptHelpers.SendChatToPlayer(playerId, $"You are stunned and cannot {action}.");
        }
    }
}