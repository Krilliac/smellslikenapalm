using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Text.Json;
using System.Linq;
using System.Threading;

public static class ScriptHelpers
{
    #region Native Bindings
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerAdminLevel(string playerId);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SendChatToPlayer(string playerId, string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BroadcastChat(string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogInfo(string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogWarning(string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogError(string message);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ScheduleCallback(float delaySeconds, string methodName);

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern string GetDataDirectory();

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerCount();

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsPlayerOnline(string playerId);
    #endregion

    #region Permission & Access Control
    /// <summary>
    /// Requires admin permission and returns false if insufficient
    /// </summary>
    public static bool RequireAdmin(string playerId, int minLevel = 2)
    {
        int level = GetPlayerAdminLevel(playerId);
        if (level < minLevel)
        {
            SendChatToPlayer(playerId, $"Insufficient permission (need level ≥ {minLevel}).");
            return false;
        }
        return true;
    }

    /// <summary>
    /// Check if player has specific admin level without notification
    /// </summary>
    public static bool HasAdminLevel(string playerId, int level)
    {
        return GetPlayerAdminLevel(playerId) >= level;
    }

    /// <summary>
    /// Execute action only if player has admin permission
    /// </summary>
    public static void AdminOnly(string playerId, int minLevel, Action action)
    {
        if (RequireAdmin(playerId, minLevel))
            action?.Invoke();
    }
    #endregion

    #region Command Parsing & Dispatch
    /// <summary>
    /// Parse chat commands and invoke handler with admin check
    /// </summary>
    public static void OnChatCommand(string playerId, string message, string command, Action<string[], string> handler)
    {
        if (!message.StartsWith("!" + command, StringComparison.OrdinalIgnoreCase))
            return;

        if (!RequireAdmin(playerId)) return;

        var parts = message.Substring(command.Length + 1).Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        handler(parts, playerId);
    }

    /// <summary>
    /// Register multiple commands with a single handler
    /// </summary>
    public static void RegisterCommands(Dictionary<string, Action<string[], string>> commands)
    {
        // Store commands for later lookup in message handlers
        _registeredCommands = commands;
    }

    private static Dictionary<string, Action<string[], string>> _registeredCommands = new();

    /// <summary>
    /// Process any registered chat command
    /// </summary>
    public static void ProcessChatCommand(string playerId, string message)
    {
        if (!message.StartsWith("!")) return;

        var cmdPart = message.Substring(1).Split(' ')[0];
        if (_registeredCommands.ContainsKey(cmdPart))
        {
            OnChatCommand(playerId, message, cmdPart, _registeredCommands[cmdPart]);
        }
    }
    #endregion

    #region Script Management
    /// <summary>
    /// Toggle script between enabled/disabled folders
    /// </summary>
    public static void ToggleScript(string scriptName, string dataDir, string playerId = null)
    {
        string root = Path.Combine(dataDir, "scripts");
        string enabledDir = Path.Combine(root, "enabled");
        string disabledDir = Path.Combine(root, "disabled");
        string file = scriptName.EndsWith(".cs") ? scriptName : scriptName + ".cs";

        Directory.CreateDirectory(enabledDir);
        Directory.CreateDirectory(disabledDir);

        string enabledPath = Path.Combine(enabledDir, file);
        string disabledPath = Path.Combine(disabledDir, file);

        try
        {
            if (File.Exists(enabledPath))
            {
                File.Move(enabledPath, disabledPath);
                SendMessageToPlayer(playerId, $"Script '{file}' disabled.");
            }
            else if (File.Exists(disabledPath))
            {
                File.Move(disabledPath, enabledPath);
                SendMessageToPlayer(playerId, $"Script '{file}' enabled.");
            }
            else
            {
                SendMessageToPlayer(playerId, $"Script '{file}' not found.");
            }
        }
        catch (Exception ex)
        {
            LogError($"Failed to toggle script {file}: {ex.Message}");
            SendMessageToPlayer(playerId, $"Error toggling script: {ex.Message}");
        }
    }

    /// <summary>
    /// List all available scripts (enabled and disabled)
    /// </summary>
    public static void ListScripts(string playerId)
    {
        if (!RequireAdmin(playerId)) return;

        string dataDir = GetDataDirectory();
        string scriptsDir = Path.Combine(dataDir, "scripts");
        string enabledDir = Path.Combine(scriptsDir, "enabled");
        string disabledDir = Path.Combine(scriptsDir, "disabled");

        var enabled = Directory.Exists(enabledDir) ? 
            Directory.GetFiles(enabledDir, "*.cs").Select(Path.GetFileNameWithoutExtension) : 
            Enumerable.Empty<string>();

        var disabled = Directory.Exists(disabledDir) ? 
            Directory.GetFiles(disabledDir, "*.cs").Select(Path.GetFileNameWithoutExtension) : 
            Enumerable.Empty<string>();

        SendChatToPlayer(playerId, $"Enabled: {string.Join(", ", enabled)}");
        SendChatToPlayer(playerId, $"Disabled: {string.Join(", ", disabled)}");
    }
    #endregion

    #region Throttling & Rate Limiting
    private static Dictionary<string, DateTime> _lastCallTimes = new();
    private static readonly object _throttleLock = new object();

    /// <summary>
    /// Throttle execution to prevent spam - returns true if allowed
    /// </summary>
    public static bool Throttle(string key, int cooldownSeconds)
    {
        lock (_throttleLock)
        {
            var now = DateTime.UtcNow;
            if (_lastCallTimes.TryGetValue(key, out DateTime lastCall))
            {
                if ((now - lastCall).TotalSeconds < cooldownSeconds)
                    return false;
            }
            _lastCallTimes[key] = now;
            return true;
        }
    }

    /// <summary>
    /// Debounce execution - only execute if not called recently
    /// </summary>
    public static void Debounce(string key, int delayMs, Action action)
    {
        if (!Throttle(key, delayMs / 1000))
            return;

        ScheduleCallback(delayMs / 1000f, $"ScriptHelpers.ExecuteDebounced_{key.Replace(".", "_")}");
        _debouncedActions[key] = action;
    }

    private static Dictionary<string, Action> _debouncedActions = new();

    /// <summary>
    /// Execute a previously debounced action
    /// </summary>
    public static void ExecuteDebouncedAction(string key)
    {
        if (_debouncedActions.TryGetValue(key, out Action action))
        {
            action?.Invoke();
            _debouncedActions.Remove(key);
        }
    }
    #endregion

    #region Conditional Execution
    /// <summary>
    /// Schedule callback only if condition is met
    /// </summary>
    public static void ScheduleIf(Func<bool> condition, float delaySeconds, Action action)
    {
        if (condition())
        {
            var key = Guid.NewGuid().ToString();
            _conditionalActions[key] = action;
            ScheduleCallback(delaySeconds, $"ScriptHelpers.ExecuteConditional_{key}");
        }
    }

    private static Dictionary<string, Action> _conditionalActions = new();

    /// <summary>
    /// Execute action only if player count exceeds threshold
    /// </summary>
    public static void ExecuteIfPlayerCount(int minPlayers, Action action)
    {
        if (GetPlayerCount() >= minPlayers)
            action?.Invoke();
    }

    /// <summary>
    /// Broadcast only if issued by admin
    /// </summary>
    public static void BroadcastIfAdmin(string playerId, string message, int minLevel = 2)
    {
        if (HasAdminLevel(playerId, minLevel))
            BroadcastChat($"[Admin] {message}");
    }
    #endregion

    #region Data Persistence
    /// <summary>
    /// Save data to JSON file
    /// </summary>
    public static void SaveData<T>(string filename, T data)
    {
        try
        {
            string dataDir = Path.Combine(GetDataDirectory(), "persistent");
            Directory.CreateDirectory(dataDir);
            string path = Path.Combine(dataDir, filename + ".json");
            string json = JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(path, json);
        }
        catch (Exception ex)
        {
            LogError($"Failed to save data to {filename}: {ex.Message}");
        }
    }

    /// <summary>
    /// Load data from JSON file
    /// </summary>
    public static T LoadData<T>(string filename, T defaultValue = default)
    {
        try
        {
            string dataDir = Path.Combine(GetDataDirectory(), "persistent");
            string path = Path.Combine(dataDir, filename + ".json");
            if (!File.Exists(path)) return defaultValue;

            string json = File.ReadAllText(path);
            return JsonSerializer.Deserialize<T>(json);
        }
        catch (Exception ex)
        {
            LogError($"Failed to load data from {filename}: {ex.Message}");
            return defaultValue;
        }
    }

    /// <summary>
    /// Append line to log file with timestamp
    /// </summary>
    public static void AppendToLog(string logName, string message)
    {
        try
        {
            string logsDir = Path.Combine(GetDataDirectory(), "logs");
            Directory.CreateDirectory(logsDir);
            string path = Path.Combine(logsDir, logName + ".log");
            string timestamped = $"[{DateTime.UtcNow:yyyy-MM-dd HH:mm:ss}] {message}";
            File.AppendAllText(path, timestamped + Environment.NewLine);
        }
        catch (Exception ex)
        {
            LogError($"Failed to append to log {logName}: {ex.Message}");
        }
    }
    #endregion

    #region Server Monitoring & Metrics
    /// <summary>
    /// Track metric over time and save to data
    /// </summary>
    public static void TrackMetric(string metricName, float value)
    {
        var metrics = LoadData<Dictionary<string, List<MetricEntry>>>("metrics", new());
        if (!metrics.ContainsKey(metricName))
            metrics[metricName] = new List<MetricEntry>();

        metrics[metricName].Add(new MetricEntry 
        { 
            Timestamp = DateTime.UtcNow, 
            Value = value 
        });

        // Keep only last 1000 entries per metric
        if (metrics[metricName].Count > 1000)
            metrics[metricName] = metrics[metricName].TakeLast(1000).ToList();

        SaveData("metrics", metrics);
    }

    public class MetricEntry
    {
        public DateTime Timestamp { get; set; }
        public float Value { get; set; }
    }

    /// <summary>
    /// Get average of metric over last N minutes
    /// </summary>
    public static float GetMetricAverage(string metricName, int lastMinutes)
    {
        var metrics = LoadData<Dictionary<string, List<MetricEntry>>>("metrics", new());
        if (!metrics.ContainsKey(metricName)) return 0f;

        var cutoff = DateTime.UtcNow.AddMinutes(-lastMinutes);
        var recent = metrics[metricName].Where(m => m.Timestamp > cutoff);
        return recent.Any() ? recent.Average(m => m.Value) : 0f;
    }
    #endregion

    #region Player Management
    /// <summary>
    /// Execute action for all online players with admin level
    /// </summary>
    public static void ForEachAdminOnline(int minLevel, Action<string> action)
    {
        // This would need a native call to get all online players
        // For now, placeholder implementation
        LogInfo($"ForEachAdminOnline called with level {minLevel}");
    }

    /// <summary>
    /// Send message to player if online, otherwise queue for next login
    /// </summary>
    public static void SendMessageOrQueue(string playerId, string message)
    {
        if (IsPlayerOnline(playerId))
        {
            SendChatToPlayer(playerId, message);
        }
        else
        {
            var queue = LoadData<Dictionary<string, List<string>>>("message_queue", new());
            if (!queue.ContainsKey(playerId))
                queue[playerId] = new List<string>();
            queue[playerId].Add(message);
            SaveData("message_queue", queue);
        }
    }

    /// <summary>
    /// Process queued messages for a player who just joined
    /// </summary>
    public static void ProcessQueuedMessages(string playerId)
    {
        var queue = LoadData<Dictionary<string, List<string>>>("message_queue", new());
        if (queue.ContainsKey(playerId) && queue[playerId].Any())
        {
            foreach (var message in queue[playerId])
            {
                SendChatToPlayer(playerId, $"[Queued] {message}");
            }
            queue[playerId].Clear();
            SaveData("message_queue", queue);
        }
    }
    #endregion

    #region Utility Functions
    /// <summary>
    /// Format time span in human readable format
    /// </summary>
    public static string FormatTimeSpan(TimeSpan span)
    {
        if (span.TotalDays >= 1)
            return $"{(int)span.TotalDays}d {span.Hours}h {span.Minutes}m";
        if (span.TotalHours >= 1)
            return $"{span.Hours}h {span.Minutes}m";
        if (span.TotalMinutes >= 1)
            return $"{span.Minutes}m {span.Seconds}s";
        return $"{span.Seconds}s";
    }

    /// <summary>
    /// Format bytes in human readable format
    /// </summary>
    public static string FormatBytes(long bytes)
    {
        string[] sizes = { "B", "KB", "MB", "GB", "TB" };
        double len = bytes;
        int order = 0;
        while (len >= 1024 && order < sizes.Length - 1)
        {
            order++;
            len /= 1024;
        }
        return $"{len:0.##} {sizes[order]}";
    }

    /// <summary>
    /// Generate random string for tokens/IDs
    /// </summary>
    public static string GenerateRandomString(int length)
    {
        const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        var random = new Random();
        return new string(Enumerable.Repeat(chars, length)
            .Select(s => s[random.Next(s.Length)]).ToArray());
    }

    /// <summary>
    /// Send message to player or broadcast if player is null
    /// </summary>
    private static void SendMessageToPlayer(string playerId, string message)
    {
        if (!string.IsNullOrEmpty(playerId))
            SendChatToPlayer(playerId, message);
        else
            BroadcastChat(message);
    }

    /// <summary>
    /// Safe file deletion with error handling
    /// </summary>
    public static bool SafeDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
                return true;
            }
            return false;
        }
        catch (Exception ex)
        {
            LogError($"Failed to delete file {path}: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Create backup of file with timestamp
    /// </summary>
    public static bool BackupFile(string filePath)
    {
        try
        {
            if (!File.Exists(filePath)) return false;
            
            string backupPath = $"{filePath}.backup_{DateTime.UtcNow:yyyyMMdd_HHmmss}";
            File.Copy(filePath, backupPath);
            LogInfo($"Created backup: {backupPath}");
            return true;
        }
        catch (Exception ex)
        {
            LogError($"Failed to backup file {filePath}: {ex.Message}");
            return false;
        }
    }
    #endregion

    #region Scheduled Tasks
    private static Dictionary<string, ScheduledTask> _scheduledTasks = new();

    public class ScheduledTask
    {
        public string Id { get; set; }
        public DateTime NextRun { get; set; }
        public TimeSpan Interval { get; set; }
        public Action Action { get; set; }
        public bool Repeating { get; set; }
    }

    /// <summary>
    /// Schedule a recurring task
    /// </summary>
    public static string ScheduleRecurring(TimeSpan interval, Action action)
    {
        string id = Guid.NewGuid().ToString();
        _scheduledTasks[id] = new ScheduledTask
        {
            Id = id,
            NextRun = DateTime.UtcNow.Add(interval),
            Interval = interval,
            Action = action,
            Repeating = true
        };
        return id;
    }

    /// <summary>
    /// Cancel a scheduled task
    /// </summary>
    public static void CancelTask(string taskId)
    {
        _scheduledTasks.Remove(taskId);
    }

    /// <summary>
    /// Process all scheduled tasks (call this from a tick handler)
    /// </summary>
    public static void ProcessScheduledTasks()
    {
        var now = DateTime.UtcNow;
        var toExecute = _scheduledTasks.Values.Where(t => t.NextRun <= now).ToList();
        
        foreach (var task in toExecute)
        {
            try
            {
                task.Action?.Invoke();
                
                if (task.Repeating)
                {
                    task.NextRun = now.Add(task.Interval);
                }
                else
                {
                    _scheduledTasks.Remove(task.Id);
                }
            }
            catch (Exception ex)
            {
                LogError($"Scheduled task {task.Id} failed: {ex.Message}");
            }
        }
    }
    #endregion
}