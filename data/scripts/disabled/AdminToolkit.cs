// AdminToolkit.cs — Comprehensive admin command suite: kick, ban, heal, teleport, manage match.
// Demonstrates: chat commands, player management, game state control, weapon/inventory, scoring.

using System;
using System.Collections.Generic;
using System.Linq;

public class AdminToolkit
{
    public static void Initialize()
    {
        ScriptHelpers.RegisterCommands(new Dictionary<string, Action<string[], string>>
        {
            ["kick"]       = CmdKick,
            ["ban"]        = CmdBan,
            ["unban"]      = CmdUnban,
            ["heal"]       = CmdHeal,
            ["tp"]         = CmdTeleport,
            ["tphere"]     = CmdTeleportHere,
            ["give"]       = CmdGive,
            ["take"]       = CmdTake,
            ["ammo"]       = CmdAmmo,
            ["setscore"]   = CmdSetScore,
            ["setteam"]    = CmdSetTeam,
            ["startmatch"] = CmdStartMatch,
            ["endmatch"]   = CmdEndMatch,
            ["pause"]      = CmdPause,
            ["resume"]     = CmdResume,
            ["map"]        = CmdMap,
            ["mode"]       = CmdMode,
            ["timelimit"]  = CmdTimeLimit,
            ["players"]    = CmdPlayers,
            ["info"]       = CmdInfo,
            ["scripts"]    = CmdScripts,
            ["toggle"]     = CmdToggle,
            ["reloadcfg"]  = CmdReloadCfg,
        });
        ScriptHelpers.OnEvent("OnChatMessage", nameof(OnChat));
        ScriptHelpers.Log("[AdminToolkit] Initialized with admin commands");
    }

    public static void Cleanup()
    {
        ScriptHelpers.OffEvent("OnChatMessage", nameof(OnChat));
    }

    public static void OnChat(string playerId, string message)
    {
        ScriptHelpers.ProcessChatCmd(playerId, message);
    }

    private static void CmdKick(string[] args, string admin)
    {
        if (args.Length < 1) { ScriptHelpers.ChatTo(admin, "Usage: !kick <player> [reason]"); return; }
        string reason = args.Length > 1 ? string.Join(" ", args.Skip(1)) : "Kicked by admin";
        ScriptHelpers.Kick(args[0], reason);
        ScriptHelpers.ChatAll($"[Admin] {ScriptHelpers.PlayerName(args[0])} kicked: {reason}");
    }

    private static void CmdBan(string[] args, string admin)
    {
        if (args.Length < 2 || !int.TryParse(args[1], out var hours))
        { ScriptHelpers.ChatTo(admin, "Usage: !ban <player> <hours> [reason]"); return; }
        string reason = args.Length > 2 ? string.Join(" ", args.Skip(2)) : "Banned by admin";
        ScriptHelpers.Ban(args[0], reason, hours);
        ScriptHelpers.ChatAll($"[Admin] {ScriptHelpers.PlayerName(args[0])} banned for {hours}h: {reason}");
    }

    private static void CmdUnban(string[] args, string admin)
    {
        if (args.Length != 1) { ScriptHelpers.ChatTo(admin, "Usage: !unban <player>"); return; }
        ScriptHelpers.Unban(args[0]);
        ScriptHelpers.ChatTo(admin, $"Unbanned {args[0]}");
    }

    private static void CmdHeal(string[] args, string admin)
    {
        string target = args.Length > 0 ? args[0] : admin;
        int hp = args.Length > 1 && int.TryParse(args[1], out var h) ? h : 100;
        ScriptHelpers.SetHP(target, hp);
        ScriptHelpers.ChatTo(admin, $"Set {ScriptHelpers.PlayerName(target)} HP to {hp}");
    }

    private static void CmdTeleport(string[] args, string admin)
    {
        if (args.Length != 4 || !float.TryParse(args[1], out var x) ||
            !float.TryParse(args[2], out var y) || !float.TryParse(args[3], out var z))
        { ScriptHelpers.ChatTo(admin, "Usage: !tp <player> <x> <y> <z>"); return; }
        ScriptHelpers.TeleportPlayer(args[0], x, y, z);
        ScriptHelpers.ChatTo(admin, $"Teleported {ScriptHelpers.PlayerName(args[0])} to ({x},{y},{z})");
    }

    private static void CmdTeleportHere(string[] args, string admin)
    {
        if (args.Length != 1) { ScriptHelpers.ChatTo(admin, "Usage: !tphere <player>"); return; }
        var (x, y, z) = ScriptHelpers.PlayerPos(admin);
        ScriptHelpers.TeleportPlayer(args[0], x, y, z);
        ScriptHelpers.ChatTo(admin, $"Teleported {ScriptHelpers.PlayerName(args[0])} to your position");
    }

    private static void CmdGive(string[] args, string admin)
    {
        if (args.Length != 2) { ScriptHelpers.ChatTo(admin, "Usage: !give <player> <weapon>"); return; }
        ScriptHelpers.GiveWeapon(args[0], args[1]);
        ScriptHelpers.ChatTo(admin, $"Gave {args[1]} to {ScriptHelpers.PlayerName(args[0])}");
    }

    private static void CmdTake(string[] args, string admin)
    {
        if (args.Length != 2) { ScriptHelpers.ChatTo(admin, "Usage: !take <player> <weapon>"); return; }
        ScriptHelpers.TakeWeapon(args[0], args[1]);
        ScriptHelpers.ChatTo(admin, $"Removed {args[1]} from {ScriptHelpers.PlayerName(args[0])}");
    }

    private static void CmdAmmo(string[] args, string admin)
    {
        if (args.Length != 3 || !int.TryParse(args[2], out var ammo))
        { ScriptHelpers.ChatTo(admin, "Usage: !ammo <player> <weapon> <amount>"); return; }
        ScriptHelpers.SetAmmo(args[0], args[1], ammo);
        ScriptHelpers.ChatTo(admin, $"Set {args[1]} ammo to {ammo} for {ScriptHelpers.PlayerName(args[0])}");
    }

    private static void CmdSetScore(string[] args, string admin)
    {
        if (args.Length != 2 || !int.TryParse(args[1], out var score))
        { ScriptHelpers.ChatTo(admin, "Usage: !setscore <player> <score>"); return; }
        ScriptHelpers.AddScore(args[0], score - ScriptHelpers.Score(args[0]));
        ScriptHelpers.ChatTo(admin, $"Set {ScriptHelpers.PlayerName(args[0])} score to {score}");
    }

    private static void CmdSetTeam(string[] args, string admin)
    {
        if (args.Length != 2 || !int.TryParse(args[1], out var team))
        { ScriptHelpers.ChatTo(admin, "Usage: !setteam <player> <teamId>"); return; }
        ScriptHelpers.SetTeam(args[0], team);
        ScriptHelpers.ChatTo(admin, $"Moved {ScriptHelpers.PlayerName(args[0])} to team {ScriptHelpers.TeamName(team)}");
    }

    private static void CmdStartMatch(string[] args, string admin)
    { ScriptHelpers.MatchStart(); ScriptHelpers.ChatAll("[Admin] Match started!"); }

    private static void CmdEndMatch(string[] args, string admin)
    { ScriptHelpers.MatchEnd(); ScriptHelpers.ChatAll("[Admin] Match ended!"); }

    private static void CmdPause(string[] args, string admin)
    { ScriptHelpers.MatchPause(); ScriptHelpers.ChatAll("[Admin] Match paused"); }

    private static void CmdResume(string[] args, string admin)
    { ScriptHelpers.MatchResume(); ScriptHelpers.ChatAll("[Admin] Match resumed"); }

    private static void CmdMap(string[] args, string admin)
    {
        if (args.Length != 1) { ScriptHelpers.ChatTo(admin, "Usage: !map <mapName>"); return; }
        ScriptHelpers.ChatAll($"[Admin] Changing map to {args[0]}...");
        ScriptHelpers.SwitchMap(args[0]);
    }

    private static void CmdMode(string[] args, string admin)
    {
        if (args.Length != 1) { ScriptHelpers.ChatTo(admin, "Usage: !mode <gameMode>"); return; }
        ScriptHelpers.ChatAll($"[Admin] Changing mode to {args[0]}...");
        ScriptHelpers.SwitchMode(args[0]);
    }

    private static void CmdTimeLimit(string[] args, string admin)
    {
        if (args.Length != 1 || !int.TryParse(args[0], out var secs))
        { ScriptHelpers.ChatTo(admin, "Usage: !timelimit <seconds>"); return; }
        ScriptHelpers.SetTimeLimit(secs);
        ScriptHelpers.ChatTo(admin, $"Time limit set to {ScriptHelpers.FmtSpan(TimeSpan.FromSeconds(secs))}");
    }

    private static void CmdPlayers(string[] args, string admin)
    {
        var players = ScriptHelpers.GetAllPlayers();
        ScriptHelpers.ChatTo(admin, $"Online ({players.Count}/{ScriptHelpers.MaxPlayerCount()}):");
        foreach (var p in players)
        {
            string name = ScriptHelpers.PlayerName(p);
            int team = ScriptHelpers.PlayerTeam(p);
            int ping = ScriptHelpers.PlayerPing(p);
            int hp = ScriptHelpers.PlayerHealth(p);
            ScriptHelpers.ChatTo(admin, $"  {name} T{team} HP:{hp} Ping:{ping}ms");
        }
    }

    private static void CmdInfo(string[] args, string admin)
    {
        ScriptHelpers.ChatTo(admin, $"Server: {ScriptHelpers.ServerNameStr()} v{ScriptHelpers.Version()}");
        ScriptHelpers.ChatTo(admin, $"Map: {ScriptHelpers.CurrentMap()} | Mode: {ScriptHelpers.CurrentMode()}");
        ScriptHelpers.ChatTo(admin, $"Uptime: {ScriptHelpers.FmtSpan(TimeSpan.FromSeconds(ScriptHelpers.Uptime()))}");
        ScriptHelpers.ChatTo(admin, $"Tick: {ScriptHelpers.TickRate()}Hz | FPS: {ScriptHelpers.FrameRate():0}");
        ScriptHelpers.ChatTo(admin, $"CPU: {ScriptHelpers.CpuUsage():0.0}% | Mem: {ScriptHelpers.FmtBytes(ScriptHelpers.MemUsage())}");
    }

    private static void CmdScripts(string[] args, string admin)
    {
        ScriptHelpers.ListScripts();
    }

    private static void CmdToggle(string[] args, string admin)
    {
        if (args.Length != 1) { ScriptHelpers.ChatTo(admin, "Usage: !toggle <scriptName>"); return; }
        ScriptHelpers.ToggleScript(args[0]);
    }

    private static void CmdReloadCfg(string[] args, string admin)
    {
        ScriptHelpers.CfgReload();
        ScriptHelpers.ChatTo(admin, "Configuration reloaded");
    }
}
