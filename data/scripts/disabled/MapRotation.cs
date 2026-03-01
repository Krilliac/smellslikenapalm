// MapRotation.cs — Configurable map rotation that advances on match end or by admin command.
// Demonstrates: match events, config management, map switching, chat commands, persistence.

using System;
using System.Collections.Generic;
using System.Linq;

public class MapRotation
{
    private static List<string> _rotation;
    private static int _currentIndex;
    private const string SaveKey = "map_rotation";

    public static void Initialize()
    {
        var saved = ScriptHelpers.Load<Dictionary<string, object>>(SaveKey, null);
        if (saved != null && saved.ContainsKey("rotation"))
        {
            _rotation = ((System.Text.Json.JsonElement)saved["rotation"])
                .EnumerateArray()
                .Select(e => e.GetString())
                .Where(s => !string.IsNullOrEmpty(s))
                .ToList();
            _currentIndex = saved.ContainsKey("index") ? ((System.Text.Json.JsonElement)saved["index"]).GetInt32() : 0;
        }
        else
        {
            _rotation = new List<string> { "VNTE_CuChi", "VNTE_HueCity", "VNTE_AnLao", "VNSU_SongBe" };
            _currentIndex = 0;
        }

        ScriptHelpers.OnEvent("OnMatchEnd", nameof(OnMatchEnd));
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log($"[MapRotation] Initialized with {_rotation.Count} maps, current index: {_currentIndex}");
    }

    public static void Cleanup()
    {
        SaveState();
        ScriptHelpers.OffEvent("OnMatchEnd", nameof(OnMatchEnd));
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnMatchEnd()
    {
        _currentIndex = (_currentIndex + 1) % _rotation.Count;
        string nextMap = _rotation[_currentIndex];
        ScriptHelpers.ChatAll($"[MapRotation] Next map: {nextMap} ({_currentIndex + 1}/{_rotation.Count})");

        ScriptHelpers.ScheduleOnce(TimeSpan.FromSeconds(15), () =>
        {
            ScriptHelpers.SwitchMap(nextMap);
            SaveState();
        });
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.OnChatCommand(playerId, message, "nextmap", (args, pid) =>
        {
            int next = (_currentIndex + 1) % _rotation.Count;
            ScriptHelpers.ChatTo(pid, $"Next map: {_rotation[next]} ({next + 1}/{_rotation.Count})");
        });

        ScriptHelpers.OnChatCommand(playerId, message, "maplist", (args, pid) =>
        {
            for (int i = 0; i < _rotation.Count; i++)
            {
                string marker = i == _currentIndex ? " <--" : "";
                ScriptHelpers.ChatTo(pid, $"  {i + 1}. {_rotation[i]}{marker}");
            }
        });

        ScriptHelpers.OnChatCommand(playerId, message, "skipmap", (args, pid) =>
        {
            if (!ScriptHelpers.RequireAdmin(pid, 2)) return;
            OnMatchEnd();
            ScriptHelpers.ChatAll($"[Admin] {ScriptHelpers.PlayerName(pid)} skipped the map");
        });

        ScriptHelpers.OnChatCommand(playerId, message, "addmap", (args, pid) =>
        {
            if (!ScriptHelpers.RequireAdmin(pid, 2)) return;
            if (args.Length < 1) { ScriptHelpers.ChatTo(pid, "Usage: !addmap <mapName>"); return; }
            _rotation.Add(args[0]);
            SaveState();
            ScriptHelpers.ChatTo(pid, $"Added {args[0]} to rotation ({_rotation.Count} maps)");
        });

        ScriptHelpers.OnChatCommand(playerId, message, "removemap", (args, pid) =>
        {
            if (!ScriptHelpers.RequireAdmin(pid, 2)) return;
            if (args.Length < 1) { ScriptHelpers.ChatTo(pid, "Usage: !removemap <mapName>"); return; }
            if (_rotation.Remove(args[0]))
            {
                if (_currentIndex >= _rotation.Count) _currentIndex = 0;
                SaveState();
                ScriptHelpers.ChatTo(pid, $"Removed {args[0]} from rotation");
            }
            else ScriptHelpers.ChatTo(pid, $"Map '{args[0]}' not found in rotation");
        });
    }

    private static void SaveState()
    {
        ScriptHelpers.Save(SaveKey, new Dictionary<string, object>
        {
            ["rotation"] = _rotation,
            ["index"] = _currentIndex
        });
    }
}
