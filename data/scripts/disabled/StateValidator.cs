// StateValidator.cs — Enforces frozen/stunned player states by reverting movement and blocking actions.
// Demonstrates: multiple event handlers, player state tracking, position manipulation.

using System;
using System.Collections.Generic;

public class StateValidator
{
    private static HashSet<string> _frozen = new();
    private static HashSet<string> _stunned = new();
    private static Dictionary<string, (float x, float y, float z)> _lastPos = new();

    public static void Initialize()
    {
        ScriptHelpers.OnEvent("OnPlayerFrozen", nameof(OnFrozen));
        ScriptHelpers.OnEvent("OnPlayerUnfrozen", nameof(OnUnfrozen));
        ScriptHelpers.OnEvent("OnPlayerStunned", nameof(OnStunned));
        ScriptHelpers.OnEvent("OnPlayerUnstunned", nameof(OnUnstunned));
        ScriptHelpers.OnEvent("OnPlayerMove", nameof(OnMove));
        ScriptHelpers.OnEvent("OnPlayerAction", nameof(OnPlayerAction));
        ScriptHelpers.OnEvent("OnPlayerLeave", nameof(OnPlayerLeave));
        ScriptHelpers.Log("[StateValidator] Initialized");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnPlayerFrozen", nameof(OnFrozen));
        ScriptHelpers.OffEvent("OnPlayerUnfrozen", nameof(OnUnfrozen));
        ScriptHelpers.OffEvent("OnPlayerStunned", nameof(OnStunned));
        ScriptHelpers.OffEvent("OnPlayerUnstunned", nameof(OnUnstunned));
        ScriptHelpers.OffEvent("OnPlayerMove", nameof(OnMove));
        ScriptHelpers.OffEvent("OnPlayerAction", nameof(OnPlayerAction));
        ScriptHelpers.OffEvent("OnPlayerLeave", nameof(OnPlayerLeave));
        _frozen.Clear();
        _stunned.Clear();
        _lastPos.Clear();
    }

    public static void OnFrozen(string playerId)
    {
        _frozen.Add(playerId);
        ScriptHelpers.ChatTo(playerId, "You are now frozen and cannot move.");
    }

    public static void OnUnfrozen(string playerId)
    {
        _frozen.Remove(playerId);
        ScriptHelpers.ChatTo(playerId, "You are no longer frozen.");
    }

    public static void OnStunned(string playerId)
    {
        _stunned.Add(playerId);
        ScriptHelpers.ChatTo(playerId, "You are stunned and cannot act.");
    }

    public static void OnUnstunned(string playerId)
    {
        _stunned.Remove(playerId);
        ScriptHelpers.ChatTo(playerId, "You are no longer stunned.");
    }

    public static void OnMove(string playerId, string xs, string ys, string zs)
    {
        if (!float.TryParse(xs, out var x) ||
            !float.TryParse(ys, out var y) ||
            !float.TryParse(zs, out var z))
            return;

        if (_frozen.Contains(playerId))
        {
            if (_lastPos.TryGetValue(playerId, out var prev))
                ScriptHelpers.TeleportPlayer(playerId, prev.x, prev.y, prev.z);
            return;
        }

        _lastPos[playerId] = (x, y, z);
    }

    public static void OnPlayerAction(string playerId, string action)
    {
        if (_stunned.Contains(playerId))
            ScriptHelpers.ChatTo(playerId, $"You are stunned and cannot {action}.");
    }

    public static void OnPlayerLeave(string playerId)
    {
        _frozen.Remove(playerId);
        _stunned.Remove(playerId);
        _lastPos.Remove(playerId);
    }
}
