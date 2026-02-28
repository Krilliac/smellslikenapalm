# ADMIN_COMMANDS.md — Administrative & RCON Command Reference

This document provides a **comprehensive reference** for all administrative commands available in the RS2V Custom Server, including syntax, permissions, aliases, usage examples, RCON setup, and admin list management.

For configuration file formats, see [CONFIGURATION.md](CONFIGURATION.md). For security architecture, see [SECURITY.md](SECURITY.md).

---

## Table of Contents

- [1. Overview](#1--overview)
- [2. Permission Levels](#2--permission-levels)
- [3. Accessing Admin Commands](#3--accessing-admin-commands)
  - [3.1 In-Game Chat Commands](#31-in-game-chat-commands)
  - [3.2 RCON (Remote Console)](#32-rcon-remote-console)
- [4. Command Reference](#4--command-reference)
  - [4.1 Server Management](#41-server-management)
  - [4.2 Communication](#42-communication)
  - [4.3 Player Management](#43-player-management)
  - [4.4 Map Management](#44-map-management)
  - [4.5 Game Settings](#45-game-settings)
  - [4.6 Debug & Testing](#46-debug--testing)
  - [4.7 Anti-Cheat](#47-anti-cheat)
  - [4.8 Help](#48-help)
- [5. Admin List Management](#5--admin-list-management)
- [6. RCON Setup Guide](#6--rcon-setup-guide)
- [7. Best Practices](#7--best-practices)

---

## 1 · Overview

The RS2V Custom Server provides 18 administrative commands for managing the server, players, maps, and game settings. Commands can be issued via:

1. **In-game chat** — prefixed with the command name (requires `chat_auth_enabled = true` in `config/server.ini`)
2. **RCON (Remote Console)** — TCP-based remote administration (requires `enable_rcon = true`)

All commands are case-insensitive. The server parses `config/admin_commands.ini` at startup and builds a hash map for O(1) command dispatch.

---

## 2 · Permission Levels

Every admin command requires a minimum permission level. Players are assigned levels via `config/admin_list.txt`.

| Level | Role | Description | Typical Commands |
|---|---|---|---|
| **0** | Player | No admin privileges. Default for all connected players. | `help` |
| **1** | Helper | Read-only server information. Can view status but cannot modify anything. | `status`, `help` |
| **2** | Moderator | Player management. Can kick, ban, broadcast, give items, teleport, and toggle god mode. | `broadcast`, `kick`, `ban`, `banlist`, `give`, `tp`, `god` |
| **3** | Administrator | Full server control. Can shut down, restart, change maps, modify game settings, spawn bots, and configure anti-cheat. | All commands |

**Level inheritance:** Higher levels inherit all permissions of lower levels. A level 3 administrator can use all level 0, 1, and 2 commands.

---

## 3 · Accessing Admin Commands

### 3.1 In-Game Chat Commands

When `chat_auth_enabled = true` (default) and `admin_rcon_only = false` (default), administrators can issue commands directly in the game chat.

**Usage:** Type the command name in the chat input. The server recognizes admin commands by matching the first word against the command registry.

```
shutdown
kick 76561198000000001 AFK too long
broadcast Server restarting in 5 minutes!
```

**Notes:**
- The issuing player must be listed in `config/admin_list.txt` with a sufficient permission level.
- Command output is sent back to the issuing player as a private chat message.
- Other players do not see admin commands or their output (except `broadcast` messages, which are sent to all players).
- When `admin_rcon_only = true` in `config/server.ini`, all in-game chat commands are disabled regardless of admin level.

### 3.2 RCON (Remote Console)

RCON provides TCP-based remote administration without needing to be connected as a player.

**Connecting:**
```bash
# Using a generic RCON client
rcon -H <server_ip> -p <rcon_port> -P <rcon_password>

# Example
rcon -H 192.168.1.100 -p 27020 -P MySecurePassword
```

**Issuing commands:**
```
> status
Server: RS2V Custom Server | Map: carcassonne | Players: 24/64 | Tick: 60Hz | Uptime: 12h 30m

> kick 76561198000000001 AFK
Player 76561198000000001 kicked: AFK

> changemap hill_400
Map changing to hill_400...
```

**Configuration** (in `config/server.ini`):
```ini
[Admin]
enable_rcon     = true
rcon_port       = 27020
rcon_password   = YourSecurePasswordHere
rcon_min_level  = 2
```

See [Section 6](#6--rcon-setup-guide) for a complete RCON setup guide.

---

## 4 · Command Reference

### 4.1 Server Management

---

#### `shutdown`

Immediately stops the server process with a graceful shutdown sequence.

| Property | Value |
|---|---|
| **Aliases** | `quit`, `exit` |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | None |
| **Handler** | `Admin::CmdShutdown` |

**Usage:**
```
shutdown
```

**Behavior:**
1. Sends a disconnect message to all connected players
2. Saves all pending game state (scores, bans, telemetry)
3. Flushes log buffers
4. Closes network sockets
5. Exits the process with code 0

**Example:**
```
> shutdown
[Server] Initiating graceful shutdown...
[Server] 24 players disconnected.
[Server] State saved. Goodbye.
```

---

#### `restart`

Restarts the server after an optional delay.

| Property | Value |
|---|---|
| **Aliases** | `softrestart` |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `[delaySec]` (optional) |
| **Handler** | `Admin::CmdRestart` |

**Usage:**
```
restart              # Restart immediately (0-second delay)
restart 30           # Restart after 30 seconds
```

**Behavior:**
1. If a delay is specified, broadcasts a countdown warning to all players
2. At restart time, performs a graceful shutdown
3. Re-launches the server process with the same configuration

**Example:**
```
> restart 60
[Server] Server restarting in 60 seconds...
[Server] Server restarting in 30 seconds...
[Server] Server restarting in 10 seconds...
[Server] Restarting now.
```

---

#### `status`

Displays current server status including uptime, player count, map, tick rate, and memory usage.

| Property | Value |
|---|---|
| **Aliases** | `svstatus` |
| **Permission** | Level 1 (Helper) |
| **Arguments** | None |
| **Handler** | `Admin::CmdStatus` |

**Usage:**
```
status
```

**Output:**
```
> status
Server: RS2V Custom Server v1.0.0
Map: carcassonne (Conquest)
Players: 24/64
Tick Rate: 60 Hz
Uptime: 2d 5h 30m
Memory: 1.2 GB / 8.0 GB
CPU: 35%
Network: 12.5 Mbps in / 45.2 Mbps out
```

---

### 4.2 Communication

---

#### `broadcast`

Sends a colored server-wide chat message visible to all connected players.

| Property | Value |
|---|---|
| **Aliases** | `bc`, `say` |
| **Permission** | Level 2 (Moderator) |
| **Arguments** | `<message...>` (variadic, at least 1 word) |
| **Handler** | `Admin::CmdBroadcast` |

**Usage:**
```
broadcast Server maintenance in 10 minutes
bc Map vote starting now!
say Good game everyone!
```

**Behavior:**
- The message is displayed to all connected players with a distinct server announcement color.
- The message is logged in the admin command audit log.

**Example:**
```
> broadcast Server will restart at 22:00 UTC for maintenance
[Broadcast] Server will restart at 22:00 UTC for maintenance
```

---

### 4.3 Player Management

---

#### `kick`

Kicks a player from the server by SteamID, with an optional reason.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 2 (Moderator) |
| **Arguments** | `<steamId> [reason...]` |
| **Handler** | `Admin::CmdKick` |

**Usage:**
```
kick 76561198000000001
kick 76561198000000001 AFK for too long
```

**Behavior:**
- The player receives a disconnect message with the reason (if provided).
- The kick is logged in the admin audit log.
- The player can reconnect immediately (kicks do not create bans).

**Example:**
```
> kick 76561198000000001 Team killing
[Admin] Player 76561198000000001 ("Alice") kicked: Team killing
```

---

#### `ban`

Bans a player by SteamID for a specified duration or permanently.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `<steamId> <durationMin\|permanent> [reason...]` |
| **Handler** | `Admin::CmdBan` |

**Usage:**
```
ban 76561198000000001 60 Toxic behavior
ban 76561198000000001 permanent Cheating detected
ban 76561198000000001 1440 Exploiting bugs
```

**Behavior:**
1. The player is immediately disconnected if currently connected
2. An entry is added to `config/ban_list.txt`
3. The ban is enforced on future connection attempts
4. Temporary bans expire after the specified duration (in minutes)
5. `permanent` bans never expire

**Duration examples:**
- `30` = 30 minutes
- `60` = 1 hour
- `1440` = 24 hours (1 day)
- `10080` = 7 days
- `permanent` = never expires

**Example:**
```
> ban 76561198000000001 1440 Repeated team killing
[Admin] Player 76561198000000001 banned for 1440 minutes: Repeated team killing
```

---

#### `unban`

Removes a ban for the specified SteamID.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `<steamId>` |
| **Handler** | `Admin::CmdUnban` |

**Usage:**
```
unban 76561198000000001
```

**Behavior:**
- The ban entry is removed from `config/ban_list.txt`.
- If the player was permanently banned, the permanent entry is removed.
- The player can connect again immediately.

**Example:**
```
> unban 76561198000000001
[Admin] Ban removed for SteamID 76561198000000001
```

---

#### `banlist`

Displays all active bans with remaining time.

| Property | Value |
|---|---|
| **Aliases** | `bans` |
| **Permission** | Level 2 (Moderator) |
| **Arguments** | None |
| **Handler** | `Admin::CmdBanList` |

**Usage:**
```
banlist
```

**Output:**
```
> banlist
Active Bans (3):
  76561198000000010  perm       permanent           Cheating detected via EAC
  76561198000000011  temp       expires in 2d 5h    Toxic behavior
  76561198000000012  temp       expires in 14d      Map exploit abuse
```

---

### 4.4 Map Management

---

#### `changemap`

Changes the current map immediately.

| Property | Value |
|---|---|
| **Aliases** | `map` |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `<mapName>` |
| **Handler** | `Admin::CmdChangeMap` |

**Usage:**
```
changemap hill_400
map carcassonne
```

**Behavior:**
1. Validates that the map name exists in `config/maps.ini`
2. Ends the current round immediately
3. Loads the new map
4. Starts a new round with the map's default game mode

**Example:**
```
> changemap hill_937
[Server] Changing map to hill_937 (Hill 937)...
[Server] Map loaded. Starting round: HotZone
```

---

#### `rotation`

Manages the map rotation list.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `add\|remove\|list <mapName>` |
| **Handler** | `Admin::CmdRotation` |

**Usage:**
```
rotation list                    # Show current rotation
rotation add desert_assault      # Add a map to the rotation
rotation remove skirmish_field   # Remove a map from the rotation
```

**Behavior:**
- `list`: Displays the current map rotation in order.
- `add <mapName>`: Adds a map to the end of the rotation. The map must exist in `config/maps.ini`.
- `remove <mapName>`: Removes a map from the rotation. Does not affect the current map if it's the one being removed.

**Example:**
```
> rotation list
Map Rotation (8 maps):
  1. carcassonne (Conquest)
  2. hill_400 (Elimination)
  3. rubber_plant (Conquest)
  4. hacienda (HotZone)
  5. hill_937 (HotZone)
  6. village (Conquest)
  7. skirmish_field (Elimination)
  8. coastal_assault (Conquest)

> rotation add desert_assault
[Server] Added desert_assault to map rotation (position 9)
```

---

### 4.5 Game Settings

---

#### `timescale`

Adjusts the server game clock speed.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `<scale>` |
| **Handler** | `Admin::CmdTimeScale` |

**Usage:**
```
timescale 1.0     # Normal speed
timescale 0.5     # Half speed (slow motion)
timescale 2.0     # Double speed (fast forward)
```

**Behavior:**
- Modifies the game tick delta time multiplier.
- Affects all time-dependent systems: physics, movement, timers, respawn delays.
- Does not affect network tick rate.
- Value `1.0` is normal speed.
- Useful for testing and debugging.

---

#### `tickrate`

Dynamically sets the server tick rate.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `<rate>` |
| **Handler** | `Admin::CmdTickRate` |

**Usage:**
```
tickrate 60      # Standard tick rate
tickrate 128     # Competitive tick rate
tickrate 30      # Low-spec tick rate
```

**Behavior:**
- Changes the server simulation frequency immediately.
- Higher tick rates improve gameplay responsiveness but increase CPU usage.
- The change persists until the server restarts or the command is issued again.
- Does not modify `config/server.ini`.

---

### 4.6 Debug & Testing

---

#### `give`

Gives an item to a player or all players.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 2 (Moderator) |
| **Arguments** | `<steamId\|all> <itemName> [amount]` |
| **Handler** | `Admin::CmdGiveItem` |

**Usage:**
```
give 76561198000000001 AK47
give all M67 3
give 76561198000000001 Knife 1
```

**Behavior:**
- `steamId`: Give the item to a specific player.
- `all`: Give the item to all connected players.
- `amount`: Number of items/units to give (default: 1).
- Item names must match `WeaponID` values in `config/weapons.ini`.

---

#### `tp`

Teleports a player to specific world coordinates.

| Property | Value |
|---|---|
| **Aliases** | `teleport` |
| **Permission** | Level 2 (Moderator) |
| **Arguments** | `<steamId> <x> <y> <z>` |
| **Handler** | `Admin::CmdTeleport` |

**Usage:**
```
tp 76561198000000001 1000 500 200
teleport 76561198000000001 0 0 0
```

**Behavior:**
- Immediately moves the player to the specified coordinates.
- Coordinates are in the game world's coordinate system (units vary by map).
- No collision checking is performed; use with caution to avoid placing players inside geometry.

---

#### `god`

Toggles god mode (invincibility) for a player.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 2 (Moderator) |
| **Arguments** | `<steamId\|all> <on\|off>` |
| **Handler** | `Admin::CmdGodMode` |

**Usage:**
```
god 76561198000000001 on      # Enable god mode for a player
god 76561198000000001 off     # Disable god mode
god all on                    # Enable for everyone
god all off                   # Disable for everyone
```

**Behavior:**
- In god mode, the player takes no damage and cannot be killed.
- God mode persists across respawns until explicitly disabled.
- Intended for testing and debugging only.

---

#### `spawnbot`

Spawns AI bots on the server.

| Property | Value |
|---|---|
| **Aliases** | `bot` |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `[count]` (optional, default: 1) |
| **Handler** | `Admin::CmdSpawnBot` |

**Usage:**
```
spawnbot          # Spawn 1 bot
spawnbot 5        # Spawn 5 bots
bot 10            # Spawn 10 bots
```

**Behavior:**
- Bots are distributed across teams following the auto-balance rules.
- Bots count toward the server's `max_players` limit.
- Bot AI provides basic movement and combat behavior.
- Bots are useful for testing server capacity and gameplay mechanics.

---

### 4.7 Anti-Cheat

---

#### `eacmode`

Sets the EAC (Easy Anti-Cheat) emulator operating mode.

| Property | Value |
|---|---|
| **Aliases** | _(none)_ |
| **Permission** | Level 3 (Administrator) |
| **Arguments** | `safe\|emulate\|off` |
| **Handler** | `Admin::CmdEACMode` |

**Usage:**
```
eacmode safe       # Passive monitoring only
eacmode emulate    # Full emulation with enforcement
eacmode off        # Disable anti-cheat entirely
```

**Modes:**

| Mode | Description |
|---|---|
| `safe` | Passive monitoring. Suspicious activity is logged but no enforcement action is taken. Use this mode when debugging false positives. |
| `emulate` | Full EAC emulation. Memory scanning, behavioral analysis, and enforcement are active. Detected cheaters are kicked and optionally banned. This is the recommended production mode. |
| `off` | Anti-cheat is completely disabled. No scanning or analysis is performed. Use for development only. |

**Example:**
```
> eacmode safe
[EAC] Anti-cheat mode changed to: safe (passive monitoring)
```

---

### 4.8 Help

---

#### `help`

Shows available admin commands or detailed usage for a specific command.

| Property | Value |
|---|---|
| **Aliases** | `?`, `commands` |
| **Permission** | Level 0 (Player) |
| **Arguments** | `[cmd]` (optional) |
| **Handler** | `Admin::CmdHelp` |

**Usage:**
```
help                 # List all commands you have permission for
help kick            # Show detailed help for the 'kick' command
? ban                # Show detailed help for the 'ban' command
commands             # List all available commands
```

**Behavior:**
- Without arguments: Lists all commands the issuing player has permission to use, based on their admin level.
- With a command name: Shows detailed syntax, aliases, permission level, and description for that command.
- Players at level 0 can only see the `help` command itself.

**Example:**
```
> help
Available Commands (Level 3):
  shutdown (quit, exit)     - Immediately stop the server process
  restart (softrestart)     - Restart the server after optional delay
  status (svstatus)         - Print uptime, player count, map, tickrate
  broadcast (bc, say)       - Send a server-wide chat message
  kick                      - Kick a player by SteamID
  ban                       - Ban a player with duration
  unban                     - Remove ban for a SteamID
  banlist (bans)            - Show active bans
  changemap (map)           - Change the current map
  rotation                  - Manage the map rotation
  timescale                 - Adjust game clock speed
  tickrate                  - Set the server tick rate
  give                      - Give an item to a player
  tp (teleport)             - Teleport a player to coordinates
  god                       - Toggle god mode
  spawnbot (bot)            - Spawn AI bots
  eacmode                   - Set EAC emulator mode
  help (?, commands)        - Show this help

> help kick
Command: kick
Aliases: (none)
Level: 2 (Moderator)
Syntax: kick <steamId> [reason...]
Description: Kick a player by SteamID with optional reason.
```

---

## 5 · Admin List Management

The admin list file (`config/admin_list.txt`) determines who has admin access and at what level.

### File Format

```
# Lines starting with '#' are comments
<SteamID64>    <Level>    <Name/Comment>
```

### Adding an Administrator

1. Find the player's SteamID64 (17-digit number starting with `7656119...`).
2. Add a line to `config/admin_list.txt`:
   ```
   76561198012345678    3    Admin - "PlayerName"
   ```
3. The change takes effect immediately (hot-reloaded).

### Finding a Player's SteamID64

- **In-game**: Use the `status` command to see connected players and their SteamIDs.
- **Steam profile**: Use a SteamID lookup tool with the player's profile URL.
- **Server logs**: SteamIDs are logged when players connect.

### Revoking Admin Access

Remove or comment out the player's line in `config/admin_list.txt`:
```
# 76561198012345678    3    Former Admin - "PlayerName"
```

### Example Admin List

```
# Lead Administrators
76561198000000001    3    Lead Admin - "Krill"
76561198000000005    3    Co-Admin - "Dana"

# Moderators
76561198000000002    2    Moderator - "Alice"
76561198000000003    2    Moderator - "Bob"

# Helpers
76561198000000004    1    Helper - "Charlie"
```

---

## 6 · RCON Setup Guide

### Step 1: Enable RCON in Configuration

In `config/server.ini`, ensure the `[Admin]` section has RCON enabled:

```ini
[Admin]
enable_rcon     = true
rcon_port       = 27020
rcon_password   = YourStrongPasswordHere
rcon_min_level  = 2
admin_list_file = config/admin_list.txt
```

### Step 2: Configure Firewall

Open the RCON port for TCP traffic:

```bash
# Linux (UFW)
sudo ufw allow 27020/tcp

# Linux (iptables)
sudo iptables -A INPUT -p tcp --dport 27020 -j ACCEPT

# Windows Firewall (PowerShell)
New-NetFirewallRule -DisplayName "RS2V RCON" -Direction Inbound -Protocol TCP -LocalPort 27020 -Action Allow
```

**Security recommendation:** Restrict RCON access to trusted IP addresses:
```bash
sudo ufw allow from 203.0.113.10 to any port 27020 proto tcp
```

### Step 3: Connect with an RCON Client

Any Source RCON-compatible client will work:

```bash
# Example using a CLI RCON client
rcon -H your-server-ip -p 27020 -P YourStrongPasswordHere
```

### Step 4: Verify Connectivity

```
> status
Server: RS2V Custom Server v1.0.0
...
```

### Production RCON Security

For production servers, consider these additional measures:

| Measure | Configuration |
|---|---|
| Use TLS encryption | `rcon_use_tls = true` in `[Security.Advanced]` |
| Restrict to administrators | `rcon_min_level = 3` |
| Use RCON-only mode | `admin_rcon_only = true` in `[General]` |
| IP whitelist | Firewall rules limiting RCON access |
| Strong password | Minimum 16 characters, mixed case, numbers, symbols |

---

## 7 · Best Practices

### General Administration

1. **Use RCON for production servers** — Set `admin_rcon_only = true` to prevent in-game chat commands from being intercepted or abused.
2. **Keep the admin list minimal** — Only grant the minimum permission level each person needs. Use level 1 (helper) for community volunteers and level 2 (moderator) for trusted staff.
3. **Log everything** — Admin commands are logged in `logs/admin/commands.log`. Periodically review this log for misuse.
4. **Change the RCON password** — The default password `ChangeMe123` is insecure. Always set a unique, strong password before running a public server.

### Ban Management

1. **Prefer temporary bans** — Use permanent bans only for egregious violations (cheating, severe harassment).
2. **Document ban reasons** — Always include a reason with `ban` commands for audit purposes.
3. **Review the ban list periodically** — Use `banlist` to check for expired or unwarranted bans.
4. **Back up the ban list** — Keep a copy of `config/ban_list.txt` in case of accidental deletion.

### Map Rotation

1. **Test new maps before adding** — Use `changemap` to test a new map before adding it to the rotation with `rotation add`.
2. **Balance the rotation** — Include a mix of map sizes and game modes to keep gameplay varied.
3. **Respect vote weights** — Configure `vote_weight` in `config/maps.ini` to influence which maps appear more often in votes.

---

## Quick Reference Card

| Command | Aliases | Level | Syntax |
|---|---|---|---|
| `shutdown` | `quit`, `exit` | 3 | `shutdown` |
| `restart` | `softrestart` | 3 | `restart [delaySec]` |
| `status` | `svstatus` | 1 | `status` |
| `broadcast` | `bc`, `say` | 2 | `broadcast <message...>` |
| `kick` | — | 2 | `kick <steamId> [reason...]` |
| `ban` | — | 3 | `ban <steamId> <durationMin\|permanent> [reason...]` |
| `unban` | — | 3 | `unban <steamId>` |
| `banlist` | `bans` | 2 | `banlist` |
| `changemap` | `map` | 3 | `changemap <mapName>` |
| `rotation` | — | 3 | `rotation add\|remove\|list <mapName>` |
| `timescale` | — | 3 | `timescale <scale>` |
| `tickrate` | — | 3 | `tickrate <rate>` |
| `give` | — | 2 | `give <steamId\|all> <itemName> [amount]` |
| `tp` | `teleport` | 2 | `tp <steamId> <x> <y> <z>` |
| `god` | — | 2 | `god <steamId\|all> <on\|off>` |
| `spawnbot` | `bot` | 3 | `spawnbot [count]` |
| `eacmode` | — | 3 | `eacmode safe\|emulate\|off` |
| `help` | `?`, `commands` | 0 | `help [cmd]` |

---

**End of ADMIN_COMMANDS.md**

For configuration details, see [CONFIGURATION.md](CONFIGURATION.md). For troubleshooting admin access issues, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
