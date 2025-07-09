// include/Scripting/ICSharpScriptHost.h

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

    /// <summary>
    /// Logs an informational message to the server log.
    /// </summary>
    /// <param name="message">The message to log.</param>
    virtual void LogInfo(const std::string& message) = 0;

    /// <summary>
    /// Logs a warning message to the server log.
    /// </summary>
    /// <param name="message">The warning message.</param>
    virtual void LogWarning(const std::string& message) = 0;

    /// <summary>
    /// Logs an error message to the server log.
    /// </summary>
    /// <param name="message">The error message.</param>
    virtual void LogError(const std::string& message) = 0;

    /// <summary>
    /// Kicks a player from the server.
    /// </summary>
    /// <param name="playerId">Unique identifier for the player (e.g., SteamID).</param>
    /// <param name="reason">Optional reason for the kick.</param>
    virtual void KickPlayer(const std::string& playerId, const std::string& reason) = 0;

    /// <summary>
    /// Sends a chat message to all connected clients.
    /// </summary>
    /// <param name="message">The chat message.</param>
    virtual void BroadcastChat(const std::string& message) = 0;

    /// <summary>
    /// Sends a chat message to a specific player.
    /// </summary>
    /// <param name="playerId">Unique identifier for the player.</param>
    /// <param name="message">The chat message.</param>
    virtual void SendChatToPlayer(const std::string& playerId, const std::string& message) = 0;

    /// <summary>
    /// Spawns an item or entity at a world position.
    /// </summary>
    /// <param name="className">The internal class name of the entity to spawn.</param>
    /// <param name="x">World X coordinate.</param>
    /// <param name="y">World Y coordinate.</param>
    /// <param name="z">World Z coordinate.</param>
    /// <returns>True if the spawn request was accepted.</returns>
    virtual bool SpawnEntity(const std::string& className, float x, float y, float z) = 0;

    /// <summary>
    /// Retrieves a list of all connected player IDs.
    /// </summary>
    /// <returns>Vector of player identifiers.</returns>
    virtual std::vector<std::string> GetConnectedPlayers() const = 0;

    /// <summary>
    /// Registers a C# callback to be invoked when a named server event occurs.
    /// </summary>
    /// <param name="eventName">Name of the event (e.g., "OnPlayerJoin").</param>
    /// <param name="scriptMethod">Fully-qualified C# method to invoke (e.g., "MyScript.OnPlayerJoin").</param>
    virtual void RegisterEventHandler(const std::string& eventName, const std::string& scriptMethod) = 0;

    /// <summary>
    /// Unregisters all script callbacks associated with a server event.
    /// </summary>
    /// <param name="eventName">Name of the event.</param>
    virtual void UnregisterEventHandlers(const std::string& eventName) = 0;

    /// <summary>
    /// Schedules a one-time callback in seconds.
    /// </summary>
    /// <param name="delaySeconds">Delay before invocation.</param>
    /// <param name="scriptMethod">C# method to call when the timer expires.</param>
    virtual void ScheduleCallback(float delaySeconds, const std::string& scriptMethod) = 0;

    /// <summary>
    /// Cancels all scheduled callbacks matching the given method name.
    /// </summary>
    /// <param name="scriptMethod">The C# method previously scheduled.</param>
    virtual void CancelScheduledCallbacks(const std::string& scriptMethod) = 0;
};