#pragma once

#include <string>
#include <vector>

/// <summary>
/// Interface exposed to C# scripts for calling back into the native RS2V Server engine.
/// Scripts can use these methods to log messages, kick players, spawn items, etc.
/// </summary>
class ICSharpScriptHost {
public:
    virtual ~ICSharpScriptHost() = default;

    /// <summary>Logs an informational message to the server log.</summary>
    virtual void LogInfo(const std::string& message) = 0;

    /// <summary>Logs a warning message to the server log.</summary>
    virtual void LogWarning(const std::string& message) = 0;

    /// <summary>Logs an error message to the server log.</summary>
    virtual void LogError(const std::string& message) = 0;

    /// <summary>Kicks a player from the server.</summary>
    virtual void KickPlayer(const std::string& playerId, const std::string& reason) = 0;

    /// <summary>Sends a chat message to all connected clients.</summary>
    virtual void BroadcastChat(const std::string& message) = 0;

    /// <summary>Sends a chat message to a specific player.</summary>
    virtual void SendChatToPlayer(const std::string& playerId, const std::string& message) = 0;

    /// <summary>Spawns an item or entity at a world position.</summary>
    virtual bool SpawnEntity(const std::string& className, float x, float y, float z) = 0;

    /// <summary>Retrieves a list of all connected player IDs.</summary>
    virtual std::vector<std::string> GetConnectedPlayers() const = 0;

    /// <summary>Registers a C# callback to be invoked when a named server event occurs.</summary>
    virtual void RegisterEventHandler(const std::string& eventName, const std::string& scriptMethod) = 0;

    /// <summary>Unregisters all script callbacks associated with a server event.</summary>
    virtual void UnregisterEventHandlers(const std::string& eventName) = 0;

    /// <summary>Schedules a one-time callback in seconds.</summary>
    virtual void ScheduleCallback(float delaySeconds, const std::string& scriptMethod) = 0;

    /// <summary>Cancels all scheduled callbacks matching the given method name.</summary>
    virtual void CancelScheduledCallbacks(const std::string& scriptMethod) = 0;
};