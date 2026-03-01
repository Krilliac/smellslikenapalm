// data/scripts/enabled/ScriptHelpers.cs
//
// Shared helper library for RS2V C# scripts. Wraps all ScriptHost native exports
// into clean C# methods and provides common utilities used across scripts.

using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Text.Json;

public static class ScriptHelpers
{
    // ── Native Bindings ──────────────────────────────────────────────────────
    // These map 1:1 to the extern "C" exports in ScriptHost.cpp

    #region Native — Logging

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogInfo(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogWarning(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogError(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogDebug(string message);

    #endregion

    #region Native — Chat & Communication

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SendChatToPlayer(string playerId, string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BroadcastChat(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SendPrivateMessage(string fromPlayerId, string toPlayerId, string message);

    #endregion

    #region Native — Player Management

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void KickPlayer(string playerId, string reason);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BanPlayer(string playerId, string reason, int durationHours);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void UnbanPlayer(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerAdminLevel(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsPlayerOnline(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetPlayerName(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void GetPlayerPosition(string playerId, out float x, out float y, out float z);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerPosition(string playerId, float x, float y, float z);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerTeam(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerTeam(string playerId, int teamId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerHealth(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerHealth(string playerId, int health);

    #endregion

    #region Native — Player Lists

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetConnectedPlayerCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetConnectedPlayerAt(int index);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerCountPerTeam(int teamId);

    #endregion

    #region Native — Server Info

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetDataDirectory();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetServerName();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetMaxPlayers();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetCurrentTickRate();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetScriptReloadCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetCurrentMap();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetCurrentGameMode();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern long GetServerUptime();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetServerVersion();

    #endregion

    #region Native — Team Management

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetTeamCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetTeamName(int teamId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetTeamSize(int teamId, int maxSize);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetTeamSize(int teamId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetTeamScore(int teamId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetTeamScore(int teamId, int score);

    #endregion

    #region Native — Entity Management

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool SpawnEntity(string className, float x, float y, float z);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool SpawnEntityWithId(string className, float x, float y, float z, out int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void RemoveEntity(int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsEntityValid(int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void MoveEntityTo(int entityId, float x, float y, float z);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void GetEntityPosition(int entityId, out float x, out float y, out float z);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern float GetEntityHealth(int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetEntityHealth(int entityId, float health);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetEntityClass(int entityId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetEntityCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetEntityCountByClass(string className);

    #endregion

    #region Native — Config

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetConfigInt(string key, int value);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetConfigInt(string key, int defaultValue);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetConfigFloat(string key, float value);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern float GetConfigFloat(string key, float defaultValue);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetConfigBool(string key, bool value);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool GetConfigBool(string key, bool defaultValue);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetConfigString(string key, string value);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetConfigString(string key, string defaultValue);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ReloadConfig();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool SaveConfig();

    #endregion

    #region Native — Scheduling & Timing

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeScheduleCallback(float delaySeconds, string methodName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl, EntryPoint = "ScheduleCallback")]
    private static extern void NativeScheduleCallbackRaw(float delaySeconds, string methodName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void CancelScheduledCallbacks(string methodName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern long GetCurrentTimeMillis();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetCurrentTimeString();

    #endregion

    #region Native — Debug Drawing

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void DebugDrawLine(float x1, float y1, float z1,
        float x2, float y2, float z2, float duration, float thickness,
        float r, float g, float b);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void DebugDrawSphere(float x, float y, float z,
        float radius, float duration, float r, float g, float b);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void DebugDrawBox(float x, float y, float z,
        float sizeX, float sizeY, float sizeZ, float duration,
        float r, float g, float b);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void DebugDrawArrow(float x1, float y1, float z1,
        float x2, float y2, float z2, float duration, float thickness,
        float r, float g, float b);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void DebugDrawText(float x, float y, float z,
        string text, float duration, float r, float g, float b);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ClearDebugDrawings();

    #endregion

    #region Native — Script Management

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool ReloadScript(string scriptPath);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsScriptLoaded(string scriptPath);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetLoadedScriptCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetLoadedScriptAt(int index);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool EnableScript(string scriptName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool DisableScript(string scriptName);

    #endregion

    #region Native — Game State

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void StartMatch();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void EndMatch();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void PauseMatch();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ResumeMatch();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsMatchActive();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsMatchPaused();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetMatchTimeRemaining();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetMatchTimeLimit(int seconds);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ChangeMap(string mapName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ChangeGameMode(string gameMode);

    #endregion

    #region Native — Weapon & Inventory

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void GivePlayerWeapon(string playerId, string weaponClass);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void RemovePlayerWeapon(string playerId, string weaponClass);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool PlayerHasWeapon(string playerId, string weaponClass);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GetPlayerPrimaryWeapon(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerAmmo(string playerId, string weaponClass, int ammo);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerAmmo(string playerId, string weaponClass);

    #endregion

    #region Native — Statistics & Scoring

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerKills(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerDeaths(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerKills(string playerId, int kills);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerDeaths(string playerId, int deaths);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AddPlayerKill(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AddPlayerDeath(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerScore(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SetPlayerScore(string playerId, int score);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AddPlayerScore(string playerId, int points);

    #endregion

    #region Native — Network & Performance

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetAveragePlayerPing();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerPing(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern float GetServerCpuUsage();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern long GetServerMemoryUsage();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetNetworkPacketsPerSecond();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern float GetServerFrameRate();

    #endregion

    #region Native — File I/O

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool NativeFileExists(string path);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl, EntryPoint = "FileExists")]
    private static extern bool NativeFileExistsRaw(string path);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl, EntryPoint = "WriteFile")]
    private static extern bool NativeWriteFile(string path, string content);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl, EntryPoint = "ReadFile")]
    private static extern IntPtr NativeReadFile(string path);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl, EntryPoint = "DeleteFile")]
    private static extern bool NativeDeleteFile(string path);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl, EntryPoint = "CreateDirectory")]
    private static extern bool NativeCreateDirectory(string path);

    #endregion

    #region Native — EAC Memory

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool RemoteRead(uint clientId, ulong address, uint length);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool RemoteWrite(uint clientId, ulong address, byte[] buffer, uint length);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool RemoteAlloc(uint clientId, uint length, ulong protection);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BroadcastRemoteRead(ulong address, uint length);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint GetConnectedClientId(int index);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetConnectedClientCount();

    #endregion

    #region Native — Event Registration

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void RegisterEventHandler(string eventName, string methodName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void UnregisterEventHandler(string eventName, string methodName);

    #endregion

    // ── Public API ───────────────────────────────────────────────────────────
    // Friendly wrappers around the native exports for use by scripts.

    #region Logging

    public static void Log(string message) => LogInfo(message);
    public static void Warn(string message) => LogWarning(message);
    public static void Error(string message) => LogError(message);
    public static void Debug(string message) => LogDebug(message);

    #endregion

    #region Chat & Communication

    public static void ChatTo(string playerId, string message)
        => SendChatToPlayer(playerId, message);

    public static void ChatAll(string message)
        => BroadcastChat(message);

    public static void PrivateMsg(string from, string to, string message)
        => SendPrivateMessage(from, to, message);

    #endregion

    #region Permissions

    public static bool RequireAdmin(string playerId, int minLevel = 2)
    {
        if (GetPlayerAdminLevel(playerId) < minLevel)
        {
            SendChatToPlayer(playerId, $"Insufficient permission (need level {minLevel}+).");
            return false;
        }
        return true;
    }

    public static bool HasAdmin(string playerId, int level = 2)
        => GetPlayerAdminLevel(playerId) >= level;

    public static void AdminOnly(string playerId, int level, Action action)
    {
        if (RequireAdmin(playerId, level)) action();
    }

    #endregion

    #region Player Queries

    public static bool IsOnline(string playerId) => IsPlayerOnline(playerId);

    public static string PlayerName(string playerId)
        => Marshal.PtrToStringAnsi(GetPlayerName(playerId)) ?? playerId;

    public static (float x, float y, float z) PlayerPos(string playerId)
    {
        GetPlayerPosition(playerId, out float x, out float y, out float z);
        return (x, y, z);
    }

    public static void TeleportPlayer(string playerId, float x, float y, float z)
        => SetPlayerPosition(playerId, x, y, z);

    public static int PlayerTeam(string playerId) => GetPlayerTeam(playerId);
    public static void SetTeam(string playerId, int teamId) => SetPlayerTeam(playerId, teamId);
    public static int PlayerHealth(string playerId) => GetPlayerHealth(playerId);
    public static void SetHP(string playerId, int hp) => SetPlayerHealth(playerId, hp);
    public static int PlayerPing(string playerId) => GetPlayerPing(playerId);

    public static List<string> GetAllPlayers()
    {
        var result = new List<string>();
        int count = GetConnectedPlayerCount();
        for (int i = 0; i < count; i++)
        {
            var ptr = GetConnectedPlayerAt(i);
            var id = Marshal.PtrToStringAnsi(ptr);
            if (!string.IsNullOrEmpty(id)) result.Add(id);
        }
        return result;
    }

    public static int TeamPlayerCount(int teamId) => GetPlayerCountPerTeam(teamId);

    #endregion

    #region Player Actions

    public static void Kick(string playerId, string reason)
        => KickPlayer(playerId, reason);

    public static void Ban(string playerId, string reason, int hours)
        => BanPlayer(playerId, reason, hours);

    public static void Unban(string playerId) => UnbanPlayer(playerId);

    #endregion

    #region Server Info

    public static string DataDir()
        => Marshal.PtrToStringAnsi(GetDataDirectory()) ?? ".";

    public static string ServerNameStr()
        => Marshal.PtrToStringAnsi(GetServerName()) ?? "Unknown";

    public static int Players() => GetPlayerCount();
    public static int MaxPlayerCount() => GetMaxPlayers();
    public static int TickRate() => GetCurrentTickRate();
    public static int ScriptReloads() => GetScriptReloadCount();

    public static string CurrentMap()
        => Marshal.PtrToStringAnsi(GetCurrentMap()) ?? "Unknown";

    public static string CurrentMode()
        => Marshal.PtrToStringAnsi(GetCurrentGameMode()) ?? "Unknown";

    public static long Uptime() => GetServerUptime();

    public static string Version()
        => Marshal.PtrToStringAnsi(GetServerVersion()) ?? "Unknown";

    #endregion

    #region Team Management

    public static int TeamCount() => GetTeamCount();

    public static string TeamName(int teamId)
        => Marshal.PtrToStringAnsi(GetTeamName(teamId)) ?? $"Team{teamId}";

    public static void ResizeTeam(int teamId, int maxSize) => SetTeamSize(teamId, maxSize);
    public static int TeamSize(int teamId) => GetTeamSize(teamId);
    public static int TeamScore(int teamId) => GetTeamScore(teamId);
    public static void SetScore(int teamId, int score) => SetTeamScore(teamId, score);

    #endregion

    #region Entity Management

    public static bool Spawn(string className, float x, float y, float z)
        => SpawnEntity(className, x, y, z);

    public static bool SpawnWithId(string className, float x, float y, float z, out int entityId)
        => SpawnEntityWithId(className, x, y, z, out entityId);

    public static void DestroyEntity(int entityId) => RemoveEntity(entityId);
    public static bool EntityValid(int entityId) => IsEntityValid(entityId);
    public static void MoveEntity(int entityId, float x, float y, float z) => MoveEntityTo(entityId, x, y, z);

    public static (float x, float y, float z) EntityPos(int entityId)
    {
        GetEntityPosition(entityId, out float x, out float y, out float z);
        return (x, y, z);
    }

    public static float EntityHP(int entityId) => GetEntityHealth(entityId);
    public static void SetEntityHP(int entityId, float hp) => SetEntityHealth(entityId, hp);

    public static string EntityClass(int entityId)
        => Marshal.PtrToStringAnsi(GetEntityClass(entityId)) ?? "Unknown";

    public static int EntityCount() => GetEntityCount();
    public static int EntityCountOf(string className) => GetEntityCountByClass(className);

    #endregion

    #region Configuration

    public static void CfgSetInt(string key, int value) => SetConfigInt(key, value);
    public static int CfgGetInt(string key, int def = 0) => GetConfigInt(key, def);
    public static void CfgSetFloat(string key, float value) => SetConfigFloat(key, value);
    public static float CfgGetFloat(string key, float def = 0f) => GetConfigFloat(key, def);
    public static void CfgSetBool(string key, bool value) => SetConfigBool(key, value);
    public static bool CfgGetBool(string key, bool def = false) => GetConfigBool(key, def);

    public static void CfgSetString(string key, string value) => SetConfigString(key, value);

    public static string CfgGetString(string key, string def = "")
        => Marshal.PtrToStringAnsi(GetConfigString(key, def)) ?? def;

    public static void CfgReload() => ReloadConfig();
    public static bool CfgSave() => SaveConfig();

    #endregion

    #region Game State Control

    public static void MatchStart() => StartMatch();
    public static void MatchEnd() => EndMatch();
    public static void MatchPause() => PauseMatch();
    public static void MatchResume() => ResumeMatch();
    public static bool MatchActive() => IsMatchActive();
    public static bool MatchPaused() => IsMatchPaused();
    public static int MatchTimeLeft() => GetMatchTimeRemaining();
    public static void SetTimeLimit(int seconds) => SetMatchTimeLimit(seconds);
    public static void SwitchMap(string mapName) => ChangeMap(mapName);
    public static void SwitchMode(string mode) => ChangeGameMode(mode);

    #endregion

    #region Weapon & Inventory

    public static void GiveWeapon(string playerId, string weaponClass)
        => GivePlayerWeapon(playerId, weaponClass);

    public static void TakeWeapon(string playerId, string weaponClass)
        => RemovePlayerWeapon(playerId, weaponClass);

    public static bool HasWeapon(string playerId, string weaponClass)
        => PlayerHasWeapon(playerId, weaponClass);

    public static string PrimaryWeapon(string playerId)
        => Marshal.PtrToStringAnsi(GetPlayerPrimaryWeapon(playerId)) ?? "";

    public static void SetAmmo(string playerId, string weaponClass, int ammo)
        => SetPlayerAmmo(playerId, weaponClass, ammo);

    public static int Ammo(string playerId, string weaponClass)
        => GetPlayerAmmo(playerId, weaponClass);

    #endregion

    #region Statistics & Scoring

    public static int Kills(string playerId) => GetPlayerKills(playerId);
    public static int Deaths(string playerId) => GetPlayerDeaths(playerId);
    public static int Score(string playerId) => GetPlayerScore(playerId);
    public static void AddKill(string playerId) => AddPlayerKill(playerId);
    public static void AddDeath(string playerId) => AddPlayerDeath(playerId);
    public static void AddScore(string playerId, int pts) => AddPlayerScore(playerId, pts);

    #endregion

    #region Network & Performance

    public static int AvgPing() => GetAveragePlayerPing();
    public static float CpuUsage() => GetServerCpuUsage();
    public static long MemUsage() => GetServerMemoryUsage();
    public static int PacketsPerSec() => GetNetworkPacketsPerSecond();
    public static float FrameRate() => GetServerFrameRate();

    #endregion

    #region Debug Drawing

    public static void DrawLine(float x1, float y1, float z1,
                                float x2, float y2, float z2,
                                float duration = 1f, float thickness = 1f,
                                float r = 1f, float g = 1f, float b = 1f)
        => DebugDrawLine(x1, y1, z1, x2, y2, z2, duration, thickness, r, g, b);

    public static void DrawSphere(float x, float y, float z,
                                  float radius = 1f, float duration = 1f,
                                  float r = 1f, float g = 1f, float b = 1f)
        => DebugDrawSphere(x, y, z, radius, duration, r, g, b);

    public static void DrawBox(float x, float y, float z,
                               float sizeX, float sizeY, float sizeZ,
                               float duration = 1f,
                               float r = 1f, float g = 1f, float b = 1f)
        => DebugDrawBox(x, y, z, sizeX, sizeY, sizeZ, duration, r, g, b);

    public static void DrawArrow(float x1, float y1, float z1,
                                 float x2, float y2, float z2,
                                 float duration = 1f, float thickness = 1f,
                                 float r = 1f, float g = 1f, float b = 1f)
        => DebugDrawArrow(x1, y1, z1, x2, y2, z2, duration, thickness, r, g, b);

    public static void DrawText(float x, float y, float z,
                                string text, float duration = 1f,
                                float r = 1f, float g = 1f, float b = 1f)
        => DebugDrawText(x, y, z, text, duration, r, g, b);

    public static void ClearDrawings() => ClearDebugDrawings();

    #endregion

    #region Event Registration

    public static void OnEvent(string eventName, string methodName)
        => RegisterEventHandler(eventName, methodName);

    public static void OffEvent(string eventName, string methodName)
        => UnregisterEventHandler(eventName, methodName);

    #endregion

    #region EAC Memory Operations

    public static void ReadMem(uint cid, ulong addr, uint len)
    {
        if (!RemoteRead(cid, addr, len))
            LogError($"ReadMem fail cid={cid}, addr=0x{addr:X}, len={len}");
    }

    public static void WriteMem(uint cid, ulong addr, byte[] buf)
    {
        if (!RemoteWrite(cid, addr, buf, (uint)buf.Length))
            LogError($"WriteMem fail cid={cid}, addr=0x{addr:X}");
    }

    public static void AllocMem(uint cid, uint size, ulong prot)
    {
        if (!RemoteAlloc(cid, size, prot))
            LogError($"AllocMem fail cid={cid}, size={size}");
    }

    public static void ReadAllMem(ulong addr, uint len)
        => BroadcastRemoteRead(addr, len);

    public static uint ClientId(int index) => GetConnectedClientId(index);
    public static int ClientCount() => GetConnectedClientCount();

    #endregion

    #region Chat Command Parsing

    private static Dictionary<string, Action<string[], string>> _cmds
        = new Dictionary<string, Action<string[], string>>();

    public static void RegisterCommands(Dictionary<string, Action<string[], string>> cmds)
    {
        foreach (var kv in cmds)
            _cmds[kv.Key] = kv.Value;
    }

    public static void ProcessChatCmd(string playerId, string msg)
    {
        if (!msg.StartsWith("!")) return;
        var parts = msg.Substring(1).Split(' ');
        if (_cmds.TryGetValue(parts[0], out var handler))
        {
            if (!RequireAdmin(playerId)) return;
            handler(parts.Skip(1).ToArray(), playerId);
        }
    }

    public static void OnChatCommand(string playerId, string message,
                                     string command, Action<string[], string> handler)
    {
        if (!message.StartsWith("!")) return;
        var parts = message.Substring(1).Split(' ');
        if (parts[0].Equals(command, StringComparison.OrdinalIgnoreCase))
            handler(parts.Skip(1).ToArray(), playerId);
    }

    #endregion

    #region Throttle & Debounce

    private static Dictionary<string, DateTime> _last = new();
    private static Dictionary<string, Action> _deb = new();
    private static readonly object _tl = new();

    public static bool Throttle(string key, int seconds)
    {
        lock (_tl)
        {
            var now = DateTime.UtcNow;
            if (_last.TryGetValue(key, out var t) && (now - t).TotalSeconds < seconds)
                return false;
            _last[key] = now;
            return true;
        }
    }

    public static void Debounce(string key, int ms, Action act)
    {
        if (!Throttle(key, ms / 1000)) return;
        _deb[key] = act;
        NativeScheduleCallbackRaw(ms / 1000f, $"ScriptHelpers.ExecuteDeb_{key}");
    }

    public static void ExecuteDeb(string key)
    {
        if (_deb.TryGetValue(key, out var a)) { a(); _deb.Remove(key); }
    }

    #endregion

    #region Metrics & Persistence

    private static readonly string _metf = "metrics";
    public struct Metric { public DateTime T; public float V; }

    public static void Track(string name, float val)
    {
        var d = Load<Dictionary<string, List<Metric>>>(_metf, new());
        if (!d.ContainsKey(name)) d[name] = new();
        d[name].Add(new Metric { T = DateTime.UtcNow, V = val });
        if (d[name].Count > 1000) d[name].RemoveRange(0, d[name].Count - 1000);
        Save(_metf, d);
    }

    public static float Avg(string name, int mins)
    {
        var d = Load<Dictionary<string, List<Metric>>>(_metf, new());
        if (!d.ContainsKey(name)) return 0;
        var cutoff = DateTime.UtcNow.AddMinutes(-mins);
        var vals = d[name].Where(x => x.T > cutoff).Select(x => x.V);
        return vals.Any() ? vals.Average() : 0;
    }

    public static void Save<T>(string filename, T data)
    {
        try
        {
            var dir = Path.Combine(DataDir(), "persistent");
            Directory.CreateDirectory(dir);
            File.WriteAllText(
                Path.Combine(dir, filename + ".json"),
                JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception e) { LogError($"Save {filename}: {e.Message}"); }
    }

    public static T Load<T>(string filename, T defaultValue)
    {
        try
        {
            var path = Path.Combine(DataDir(), "persistent", filename + ".json");
            if (!File.Exists(path)) return defaultValue;
            return JsonSerializer.Deserialize<T>(File.ReadAllText(path));
        }
        catch { return defaultValue; }
    }

    #endregion

    #region Script File Management

    public static void ToggleScript(string name)
    {
        var root = Path.Combine(DataDir(), "scripts");
        var enabled = Path.Combine(root, "enabled", name + ".cs");
        var disabled = Path.Combine(root, "disabled", name + ".cs");
        try
        {
            if (File.Exists(enabled)) File.Move(enabled, disabled);
            else if (File.Exists(disabled)) File.Move(disabled, enabled);
            BroadcastChat($"Toggled script: {name}");
        }
        catch (Exception ex) { LogError($"ToggleScript: {ex.Message}"); }
    }

    public static void ListScripts()
    {
        var root = Path.Combine(DataDir(), "scripts");
        var en = Directory.GetFiles(Path.Combine(root, "enabled"), "*.cs")
            .Select(Path.GetFileNameWithoutExtension);
        var dis = Directory.GetFiles(Path.Combine(root, "disabled"), "*.cs")
            .Select(Path.GetFileNameWithoutExtension);
        BroadcastChat($"Enabled: {string.Join(", ", en)}");
        BroadcastChat($"Disabled: {string.Join(", ", dis)}");
    }

    #endregion

    #region Scheduled Tasks

    private static Dictionary<string, (DateTime next, TimeSpan iv, Action a, bool rep)> _tsk = new();

    public static string ScheduleRecurring(TimeSpan interval, Action action)
    {
        var id = Guid.NewGuid().ToString();
        _tsk[id] = (DateTime.UtcNow + interval, interval, action, true);
        return id;
    }

    public static string ScheduleOnce(TimeSpan delay, Action action)
    {
        var id = Guid.NewGuid().ToString();
        _tsk[id] = (DateTime.UtcNow + delay, delay, action, false);
        return id;
    }

    public static void CancelTask(string id) => _tsk.Remove(id);

    public static void ProcessTasks()
    {
        var now = DateTime.UtcNow;
        foreach (var kv in _tsk.ToList())
        {
            var (id, t) = kv;
            if (now >= t.next)
            {
                try { t.a(); }
                catch (Exception e) { LogError($"Task {id}: {e.Message}"); }
                if (t.rep) _tsk[id] = (now + t.iv, t.iv, t.a, true);
                else _tsk.Remove(id);
            }
        }
    }

    #endregion

    #region Utilities

    public static string FmtSpan(TimeSpan ts)
    {
        if (ts.TotalDays >= 1) return $"{(int)ts.TotalDays}d{ts.Hours}h{ts.Minutes}m";
        if (ts.TotalHours >= 1) return $"{ts.Hours}h{ts.Minutes}m";
        if (ts.TotalMinutes >= 1) return $"{ts.Minutes}m{ts.Seconds}s";
        return $"{ts.Seconds}s";
    }

    public static string FmtBytes(long bytes)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double len = bytes;
        int order = 0;
        while (len >= 1024 && order < units.Length - 1) { order++; len /= 1024; }
        return $"{len:0.##}{units[order]}";
    }

    public static string RandStr(int length)
    {
        const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        var rng = new Random();
        return new string(Enumerable.Range(0, length).Select(_ => chars[rng.Next(chars.Length)]).ToArray());
    }

    #endregion
}
