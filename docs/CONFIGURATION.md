# CONFIGURATION.md — Complete Configuration Reference

This document provides a **comprehensive, field-by-field reference** for every configuration file used by the RS2V Custom Server. It covers file formats, every available setting, default values, valid ranges, and behavioral notes.

For deployment-specific configuration, see [DEPLOYMENT.md](DEPLOYMENT.md). For architecture details, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Table of Contents

- [1. Configuration Overview](#1--configuration-overview)
  - [1.1 Configuration Hierarchy](#11-configuration-hierarchy)
  - [1.2 Hot Reload](#12-hot-reload)
  - [1.3 File Locations](#13-file-locations)
- [2. server.ini — Main Server Configuration](#2--serverini--main-server-configuration)
  - [2.1 \[General\]](#21-general)
  - [2.2 \[DataPaths\]](#22-datapaths)
  - [2.3 \[Network\]](#23-network)
  - [2.4 \[Security\]](#24-security)
  - [2.5 \[Scripting\]](#25-scripting)
  - [2.6 \[Logging\]](#26-logging)
  - [2.7 \[Performance\]](#27-performance)
  - [2.8 \[ThreadPool\]](#28-threadpool)
  - [2.9 \[MemoryPool\]](#29-memorypool)
  - [2.10 \[Profiler\]](#210-profiler)
  - [2.11 \[Telemetry\]](#211-telemetry)
  - [2.12 \[Admin\]](#212-admin)
- [3. server\_production.ini — Production Overrides](#3--server_productionini--production-overrides)
- [4. game\_modes.ini — Game Mode Definitions](#4--game_modesini--game-mode-definitions)
- [5. maps.ini — Map Rotation & Settings](#5--mapsini--map-rotation--settings)
- [6. weapons.ini — Weapon Definitions](#6--weaponsini--weapon-definitions)
- [7. teams.ini — Team Definitions](#7--teamsini--team-definitions)
- [8. gameplay\_settings.ini — Global Gameplay Settings](#8--gameplay_settingsini--global-gameplay-settings)
- [9. admin\_commands.ini — Admin Command Definitions](#9--admin_commandsini--admin-command-definitions)
- [10. Text-Based List Files](#10--text-based-list-files)
  - [10.1 admin\_list.txt](#101-admin_listtxt)
  - [10.2 auth\_tokens.txt](#102-auth_tokenstxt)
  - [10.3 ban\_list.txt](#103-ban_listtxt)
  - [10.4 ip\_blacklist.txt](#104-ip_blacklisttxt)
  - [10.5 motd.txt](#105-motdtxt)
  - [10.6 workshop\_items.txt](#106-workshop_itemstxt)
- [11. eac\_scanner.json — Anti-Cheat Scanner Configuration](#11--eac_scannerjson--anti-cheat-scanner-configuration)
- [12. loadouts.ini — Player Loadout Definitions](#12--loadoutsini--player-loadout-definitions)

---

## 1 · Configuration Overview

### 1.1 Configuration Hierarchy

The RS2V server resolves configuration values using a priority-based hierarchy. Higher-priority sources override lower-priority ones:

```
Priority (highest → lowest):
┌──────────────────────────────────┐
│ 1. Command-Line Arguments        │  --port 8777, --log-level DEBUG
├──────────────────────────────────┤
│ 2. Environment Variables          │  RS2V_PORT=8777, RS2V_LOG_LEVEL=DEBUG
├──────────────────────────────────┤
│ 3. Config Files                   │  config/server.ini, config/server_production.ini
├──────────────────────────────────┤
│ 4. Default Values                 │  Hardcoded in source code
├──────────────────────────────────┤
│ 5. Auto-Detected Values           │  Hardware detection (CPU cores, memory)
└──────────────────────────────────┘
```

**Command-line arguments** always take highest priority. For example, `--port 8777` overrides whatever `port` is set to in `config/server.ini`.

**Environment variables** use the prefix `RS2V_` followed by the uppercase key name with underscores. For example, the `port` key in `[Network]` can be overridden by setting `RS2V_PORT=8777`.

**Config files** can be layered. The production override file (`config/server_production.ini`) is loaded on top of the base `config/server.ini` when specified. Values in the override file replace values from the base file; keys not present in the override fall through to the base.

### 1.2 Hot Reload

The server supports **hot reloading** of certain configuration files without a restart. When a watched file changes on disk, the `ConfigWatcher` subsystem detects the change and re-parses the file.

| File | Hot Reload | Notes |
|---|---|---|
| `server.ini` | Partial | `[Logging]`, `[Telemetry]`, and `[Admin]` sections reload. `[Network]` and `[General]` require restart. |
| `game_modes.ini` | Yes | New modes take effect at the next round transition. |
| `maps.ini` | Yes | Rotation updates immediately; current map unaffected. |
| `weapons.ini` | Yes | Weapon stats update at the next round. |
| `teams.ini` | Yes | Team settings update at the next round. |
| `gameplay_settings.ini` | Yes | Applied immediately to the current round. |
| `admin_commands.ini` | No | Requires server restart. |
| `admin_list.txt` | Yes | Admin permissions update immediately. |
| `ban_list.txt` | Yes | Bans applied immediately to new connections. |
| `ip_blacklist.txt` | Yes | IP blocks applied immediately. |
| `motd.txt` | Yes | New MOTD shown to the next connecting player. |
| `workshop_items.txt` | No | Requires server restart. |
| `eac_scanner.json` | No | Requires server restart. |

Hot reload uses `inotify` on Linux and `ReadDirectoryChangesW` on Windows. The file watcher debounces changes using the `script_reload_interval_ms` setting (default: 500ms).

### 1.3 File Locations

All configuration files reside in the `config/` directory relative to the server root. The `--config` command-line argument specifies the path to the primary INI file:

```bash
./rs2v_server --config config/server.ini
```

Paths inside configuration files (e.g., `map_rotation_file`, `ban_list_file`) are resolved relative to the server root directory, not relative to the config file itself.

---

## 2 · server.ini — Main Server Configuration

**Path:** `config/server.ini`

This is the primary configuration file. It is organized into sections denoted by `[SectionName]` headers. Comments begin with `;` and extend to the end of the line.

### 2.1 [General]

General server identity and behavior settings.

| Key | Type | Default | Description |
|---|---|---|---|
| `server_name` | string | `RS2V Custom Server` | The display name of the server as shown in the server browser and connection messages. Supports spaces and special characters. Maximum 128 characters. |
| `version` | string | `1.0.0` | Server version string. Informational only; used in status responses and telemetry. Does not affect protocol compatibility. |
| `max_players` | integer | `64` | Maximum number of concurrent player connections. Valid range: 1–128. Higher values require more CPU, memory, and bandwidth. See [System Requirements](DEPLOYMENT.md) for hardware guidelines. |
| `map_rotation_file` | path | `config/maps.ini` | Path to the map rotation configuration file, relative to server root. This file defines all available maps, their properties, and the rotation order. See [Section 5](#5--mapsini--map-rotation--settings). |
| `game_modes_file` | path | `config/game_modes.ini` | Path to the game modes configuration file, relative to server root. Defines all available game modes and their rules. See [Section 4](#4--game_modesini--game-mode-definitions). |
| `motd_file` | path | `config/motd.txt` | Path to the Message of the Day file, relative to server root. Contents are displayed to players upon connection. Supports formatting codes. See [Section 10.5](#105-motdtxt). |
| `tick_rate` | integer | `60` | Server simulation tick rate in Hz (ticks per second). Controls how frequently the game loop executes. Higher values improve simulation accuracy and responsiveness but increase CPU usage. Recommended values: 30 (low-spec), 60 (standard), 128 (competitive). |
| `timesync_interval_s` | integer | `30` | Interval in seconds between time synchronization requests sent to connected clients. Lower values improve time accuracy at the cost of increased network traffic. Valid range: 5–300. |
| `enable_announcements` | boolean | `true` | When `true`, the server broadcasts periodic announcements to all connected players (e.g., map change warnings, round countdowns). Set to `false` to suppress automated messages. |
| `data_directory` | path | `data/` | Path to the runtime data directory, relative to server root. Contains map assets, scripts, and other runtime resources. |
| `log_directory` | path | `logs/` | Path to the log output directory, relative to server root. All log files (server, security, performance, telemetry) are written here. Created automatically if it does not exist. |
| `admin_rcon_only` | boolean | `false` | When `true`, admin commands can only be issued via RCON (remote console). In-game chat commands are disabled for all admin levels. When `false`, admins can use both RCON and in-game chat commands based on their permission level. |

### 2.2 [DataPaths]

Paths to data directories used at runtime.

| Key | Type | Default | Description |
|---|---|---|---|
| `maps_path` | path | `data/maps/` | Directory containing map asset files (`.umap`), relative to server root. The server scans this directory for map files referenced in `maps.ini`. |

### 2.3 [Network]

Network stack configuration.

| Key | Type | Default | Description |
|---|---|---|---|
| `port` | integer | `7777` | Primary UDP port the server listens on for game traffic. Valid range: 1024–65535. Ensure this port is open in your firewall for both inbound and outbound UDP traffic. Common values: 7777 (default), 7778, 27015. |
| `bind_address` | string | _(empty)_ | IP address to bind the server socket to. Leave empty or set to `0.0.0.0` to listen on all available interfaces. Set to a specific IP (e.g., `192.168.1.100`) to restrict the server to a single network interface. |
| `max_packet_size` | integer | `1200` | Maximum UDP packet size in bytes. This should be set below the network MTU to avoid fragmentation. Standard Internet MTU is 1500 bytes; the default of 1200 provides headroom for headers and tunneling. Valid range: 512–65535. |
| `client_idle_timeout_s` | integer | `300` | Time in seconds after which an idle client (no packets received) is automatically disconnected. Set to `0` to disable idle timeout (not recommended for public servers). Valid range: 0–3600. |
| `heartbeat_interval_s` | integer | `5` | Interval in seconds between heartbeat packets sent to each connected client. Heartbeats maintain the connection and detect disconnected clients. Valid range: 1–60. |
| `dual_stack` | boolean | `true` | When `true`, the server listens on both IPv4 and IPv6 (dual-stack socket). When `false`, only IPv4 is used. Requires OS-level IPv6 support. |
| `idle_timeout_ms` | integer | `300000` | Idle timeout in milliseconds (alternative to `client_idle_timeout_s` for millisecond precision). If both are set, this value takes precedence. Valid range: 0–3600000. |
| `reliable_transport` | boolean | `true` | When `true`, critical packets (player joins, game state changes, RPCs) use reliable delivery with acknowledgment and retransmission. When `false`, all packets are sent unreliably (not recommended). |

### 2.4 [Security]

Authentication, anti-cheat, and ban management settings.

| Key | Type | Default | Description |
|---|---|---|---|
| `enable_steam_auth` | boolean | `true` | When `true`, the server validates player connections against Steam's authentication servers using Steam session tickets. Requires a valid Steam App ID and network access to Steam servers. When `false`, Steam authentication is bypassed (useful for development/testing only). |
| `fallback_custom_auth` | boolean | `false` | When `true` and Steam authentication fails, the server falls back to custom token-based authentication using `auth_tokens.txt`. This allows players with valid custom tokens to connect when Steam is unavailable. |
| `custom_auth_tokens_file` | path | `config/auth_tokens.txt` | Path to the custom authentication tokens file, relative to server root. See [Section 10.2](#102-auth_tokenstxt). |
| `enable_ban_manager` | boolean | `true` | When `true`, the ban management system is active. Banned players (by SteamID) are rejected during the connection handshake. When `false`, all bans are ignored. |
| `ban_list_file` | path | `config/ban_list.txt` | Path to the active ban list file, relative to server root. See [Section 10.3](#103-ban_listtxt). |
| `enable_anti_cheat` | boolean | `true` | When `true`, the EAC (Easy Anti-Cheat) emulation/proxy subsystem is active. This performs client memory scanning and behavioral analysis. See [SECURITY.md](SECURITY.md) for architecture details. |
| `anti_cheat_mode` | enum | `emulate` | Anti-cheat operating mode. Valid values: `safe` (passive monitoring only, no enforcement), `emulate` (full EAC emulation with enforcement), `off` (anti-cheat disabled entirely). See [SECURITY.md](SECURITY.md) for detailed mode descriptions. |
| `eac_scanner_config_file` | path | `config/eac_scanner.json` | Path to the EAC scanner configuration file (JSON format), relative to server root. See [Section 11](#11--eac_scannerjson--anti-cheat-scanner-configuration). |

### 2.5 [Scripting]

C# scripting engine configuration.

| Key | Type | Default | Description |
|---|---|---|---|
| `enable_csharp_scripting` | boolean | `false` | When `true`, the Roslyn-based C# scripting engine is loaded at startup. Scripts in the `scripts_path` directory are compiled and executed. Requires the `ENABLE_SCRIPTING` CMake option and .NET SDK at build time. |
| `scripts_path` | path | `data/scripts/` | Directory containing C# script files (`.cs`). Scripts in the `enabled/` subdirectory are loaded automatically; scripts in `disabled/` are available for manual activation. |
| `script_reload_interval_ms` | integer | `500` | Debounce interval in milliseconds for the file watcher. When a script file changes, the server waits this long before recompiling to avoid reloading during rapid edits. Valid range: 100–10000. |
| `default_namespace` | string | `DynamicScripts` | Default C# namespace used when wrapping scripts that don't declare their own namespace. |

### 2.6 [Logging]

Log output configuration.

| Key | Type | Default | Description |
|---|---|---|---|
| `log_level` | enum | `info` | Minimum log severity level. Messages below this level are discarded. Valid values (from most to least verbose): `trace`, `debug`, `info`, `warn`, `error`, `fatal`. For development, use `debug` or `trace`. For production, use `info` or `warn`. |
| `log_to_console` | boolean | `true` | When `true`, log messages are written to standard output (stdout). Useful for development and when running under a process supervisor that captures stdout. |
| `log_to_file` | boolean | `true` | When `true`, log messages are written to a file in the `log_directory`. |
| `log_file` | string | `server.log` | Name of the primary log file (relative to `log_directory`). The server creates this file if it does not exist. |
| `log_max_size_mb` | integer | `10` | Maximum size in megabytes of a single log file before rotation. When the file exceeds this size, it is renamed with a numeric suffix and a new file is started. Valid range: 1–1024. |
| `log_max_files` | integer | `5` | Maximum number of rotated log files to keep. Older files beyond this count are deleted. Valid range: 1–100. |
| `log_timestamp_format` | string | `%Y-%m-%d %H:%M:%S` | Format string for timestamps in log output. Uses `strftime` format specifiers. Common alternatives: `%Y-%m-%dT%H:%M:%S%z` (ISO 8601), `%s` (Unix epoch). |

### 2.7 [Performance]

CPU and performance tuning.

| Key | Type | Default | Description |
|---|---|---|---|
| `max_cpu_cores` | integer | `0` | Maximum number of CPU cores the server may use. Set to `0` for automatic detection (uses all available cores). Set to a specific number to limit CPU usage (e.g., `4` to use at most 4 cores). Valid range: 0–256. |
| `cpu_affinity_mask` | integer | `0` | CPU core affinity bitmask. Each bit represents a CPU core (bit 0 = core 0, bit 1 = core 1, etc.). Set to `0` to disable affinity pinning (OS scheduler decides). Example: `0x0F` pins to cores 0–3. |
| `game_tick_rate` | integer | `60` | Game simulation tick rate in Hz. This is an alias for `[General].tick_rate` when set in the `[Performance]` section. If both are specified, `[Performance].game_tick_rate` takes precedence. |
| `dynamic_tuning_enabled` | boolean | `true` | When `true`, the server dynamically adjusts internal parameters (batch sizes, buffer lengths, thread pool sizes) based on current load. When `false`, all tuning parameters remain at their configured values. |

### 2.8 [ThreadPool]

Worker thread pool configuration for background tasks (script execution, file I/O, deferred processing).

| Key | Type | Default | Description |
|---|---|---|---|
| `worker_thread_count` | integer | `0` | Number of worker threads in the pool. Set to `0` for automatic detection (typically `CPU_cores - 2`, minimum 2). Set to a specific number to control concurrency. Valid range: 0–64. |
| `max_task_queue_length` | integer | `256` | Maximum number of tasks that can be queued before the pool starts rejecting new tasks. When the queue is full, task submission blocks until space is available. Valid range: 16–4096. |
| `spill_threshold` | integer | `512` | When the task queue exceeds this length, the pool logs a warning and optionally activates overflow handling (such as rejecting low-priority tasks). Valid range: must be >= `max_task_queue_length`. |

### 2.9 [MemoryPool]

Pre-allocated memory pool configuration for high-frequency object allocation (packets, entities, buffers).

| Key | Type | Default | Description |
|---|---|---|---|
| `preallocate_chunks` | integer | `4` | Number of memory chunks to pre-allocate at startup. Each chunk is 1 MB. Higher values reduce runtime allocation overhead at the cost of initial memory usage. Valid range: 1–64. |
| `max_chunks` | integer | `0` | Maximum number of memory chunks the pool may grow to. Set to `0` for unlimited growth (constrained only by system memory). Valid range: 0–1024. |

### 2.10 [Profiler]

Performance profiler configuration for recording timing data.

| Key | Type | Default | Description |
|---|---|---|---|
| `enable_profiling` | boolean | `true` | When `true`, the profiler records timing data for tagged code sections. This adds minimal overhead (~1% CPU) but provides valuable performance insights. Disable in production if profiling data is not needed. |
| `min_record_ms` | float | `0` | Minimum duration in milliseconds for a profiler record to be stored. Set to `0` to record all sections. Set higher (e.g., `1.0`) to only capture slow operations. |
| `buffer_size` | integer | `1000` | Maximum number of profiler records kept in the circular buffer. Older records are overwritten when the buffer is full. Valid range: 100–100000. |
| `flush_interval_s` | integer | `10` | Interval in seconds between automatic flushes of the profiler buffer to disk. Valid range: 1–3600. |
| `output_format` | enum | `json` | Output format for profiler data. Valid values: `json` (structured JSON), `csv` (comma-separated values). JSON is recommended for tool integration; CSV is useful for spreadsheet analysis. |
| `profiler_output_path` | path | `logs/profiler.json` | Path to the profiler output file, relative to server root. |

### 2.11 [Telemetry]

Telemetry system configuration. For comprehensive telemetry setup including Prometheus and Grafana, see [TELEMETRY.md](TELEMETRY.md).

| Key | Type | Default | Description |
|---|---|---|---|
| `batch_size` | integer | `50` | Number of metric samples collected before writing a batch to reporters. Higher values reduce I/O frequency but increase memory usage and latency of metric availability. Valid range: 1–1000. |
| `flush_interval_s` | integer | `5` | Interval in seconds between forced flushes of telemetry data to reporters, even if the batch is not full. Valid range: 1–300. |

### 2.12 [Admin]

Administrative access and RCON (Remote Console) configuration.

| Key | Type | Default | Description |
|---|---|---|---|
| `enable_rcon` | boolean | `true` | When `true`, the RCON (Remote Console) TCP listener is started, allowing remote administration. When `false`, the server can only be administered via in-game chat commands or direct console access. |
| `rcon_port` | integer | `27020` | TCP port for RCON connections. Valid range: 1024–65535. Must be different from the game `port`. Ensure this port is open in your firewall for TCP traffic. |
| `rcon_password` | string | `ChangeMe123` | Password required for RCON authentication. **You must change this from the default before running a public server.** Use a strong, unique password. Minimum recommended length: 12 characters. |
| `rcon_min_level` | integer | `2` | Minimum admin permission level required to connect via RCON. Valid values: 0 (player), 1 (helper), 2 (moderator), 3 (administrator). Setting to `3` restricts RCON to administrators only. |
| `admin_list_file` | path | `config/admin_list.txt` | Path to the admin list file, relative to server root. This file maps SteamIDs to permission levels. See [Section 10.1](#101-admin_listtxt). |
| `chat_auth_enabled` | boolean | `true` | When `true`, in-game admin commands (chat-based) require the issuing player to be listed in `admin_list.txt` with the appropriate permission level. When `false`, chat-based admin commands are disabled entirely (use RCON instead). |

---

## 3 · server_production.ini — Production Overrides

**Path:** `config/server_production.ini`

This file provides production-optimized overrides for the base `server.ini`. It is loaded as a layer on top of the base configuration when specified:

```bash
./rs2v_server --config config/server_production.ini
```

Key differences from the base configuration:

| Setting | Base Value | Production Value | Rationale |
|---|---|---|---|
| `server_name` | `RS2V Custom Server` | `RS2V Live Server` | Distinguishes production from development |
| `tick_rate` | `60` | `64` | Optimized for stability at scale |
| `timesync_interval_s` | `30` | `60` | Reduces network overhead |
| `enable_announcements` | `true` | `false` | Reduces chat noise |
| `admin_rcon_only` | `false` | `true` | Enforces RCON-only administration |
| `log_level` | `info` | `warn` | Reduces log volume |
| `log_to_console` | `true` | `false` | No console output in daemon mode |
| `log_max_size_mb` | `10` | `100` | Larger log files for production |
| `log_max_files` | `5` | `10` | More log history retained |
| `client_idle_timeout_s` | `300` | `180` | Frees player slots faster |
| `enable_profiling` | `true` | `false` | No profiling overhead in production |
| `preallocate_chunks` | `4` | `8` | More pre-allocated memory |
| `batch_size` (telemetry) | `50` | `100` | Larger telemetry batches |
| `flush_interval_s` (telemetry) | `5` | `30` | Less frequent telemetry I/O |
| `rcon_min_level` | `2` | `3` | Administrators-only RCON |

The production file also includes additional sections not in the base:

- **`[Modules]`**: Explicit enable/disable toggles for subsystems (`enable_physics`, `enable_collision`, `enable_telemetry`, `enable_metrics`, `enable_admin`, `enable_rcon`, `enable_chat_logging`).
- **`[Security.Advanced]`**: Enhanced RCON security (`rcon_use_tls = true`).
- **`[Advanced]`**: Memory limits (`max_memory_mb = 8192`) and monitoring intervals (`perf_monitor_interval_s = 300`).

---

## 4 · game_modes.ini — Game Mode Definitions

**Path:** `config/game_modes.ini`

Defines all available game modes. Each mode is a section with a unique `[ModeID]`. For a detailed guide on game mechanics and strategy for each mode, see [GAME_MODES.md](GAME_MODES.md).

### Schema

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `name` | string | Yes | — | Human-readable display name shown in the server browser and UI. |
| `description` | string | No | _(empty)_ | Brief description of the game mode's objective and rules. |
| `teams` | integer | Yes | — | Number of teams in this mode. Valid range: 2–4. |
| `players_per_team` | integer | Yes | — | Maximum number of players per team. Total player capacity is `teams * players_per_team` (capped by `[General].max_players`). |
| `rounds_per_match` | integer | Yes | — | Total number of rounds in a complete match. For single-round modes (e.g., Conquest), set to `1`. |
| `win_condition` | enum | Yes | — | Victory condition type. Valid values: `elimination`, `territory_control`, `capture_the_flag`, `zone_control`, `multi_zone`. |
| `timelimit_sec` | integer | No | `0` | Per-round time limit in seconds. `0` means no time limit. When the timer expires, the round ends with the current score determining the winner. |
| `score_limit` | integer | No | `0` | Score threshold that triggers a round or match win. `0` means no score limit. |
| `respawn_delay` | integer | No | `0` | Delay in seconds between a player's death and their respawn. `0` means instant respawn (typically used in elimination modes where there is no respawn). |
| `map_list` | string | No | _(all maps)_ | Comma-separated list of MapIDs that support this mode. If omitted, all maps with this mode in their `supported_modes` list are eligible. |
| `friendly_fire` | boolean | No | `false` | When `true`, players can damage teammates. Overrides `gameplay_settings.ini` for this mode. |

### Built-in Game Modes

| ModeID | Name | Teams | Players/Team | Rounds | Win Condition | Time Limit | Score Limit | Respawn |
|---|---|---|---|---|---|---|---|---|
| `Conquest` | Conquest | 2 | 32 | 1 | `territory_control` | 900s | 1000 | 5s |
| `Elimination` | Elimination | 2 | 16 | 9 | `elimination` | 600s | — | 0s |
| `CaptureTheFlag` | Capture the Flag | 2 | 24 | 5 | `capture_the_flag` | 1200s | 3 | 10s |
| `HotZone` | Hot Zone | 2 | 32 | 3 | `zone_control` | 600s | 500 | 7s |
| `Domination` | Domination | 3 | 20 | 1 | `multi_zone` | 1200s | 800 | 5s |

### Example: Adding a Custom Game Mode

```ini
[CustomMode]
name             = Last Stand
description      = Defend the base against waves of attackers. Defenders win by surviving.
teams            = 2
players_per_team = 16
rounds_per_match = 3
win_condition    = elimination
timelimit_sec    = 480
respawn_delay    = 15
friendly_fire    = true
map_list         = hill_400, hacienda
```

---

## 5 · maps.ini — Map Rotation & Settings

**Path:** `config/maps.ini`

Defines all available maps, their properties, and the rotation order. The rotation order follows the section order in the file. For a detailed guide on each map, see [MAPS.md](MAPS.md).

### Schema

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `display_name` | string | Yes | — | Human-readable name shown in the server browser, map vote screen, and MOTD. |
| `file` | path | Yes | — | Map asset filename, relative to the `maps_path` directory (configured in `[DataPaths]`). Typically a `.umap` file. |
| `supported_modes` | string | Yes | — | Comma-separated list of `ModeID` values from `game_modes.ini` that this map supports. |
| `min_players` | integer | No | `2` | Minimum number of connected players required to start a round on this map. |
| `max_players` | integer | No | `64` | Maximum player capacity for this map. Overrides `[General].max_players` if lower. |
| `default_mode` | string | No | _(first in supported_modes)_ | Default game mode if this map is selected without specifying a mode. Must be one of the `supported_modes`. |
| `time_of_day` | enum | No | _(unset)_ | Time of day setting. Valid values: `day`, `dusk`, `night`, `dawn`. Affects lighting and visibility. |
| `weather` | enum | No | _(unset)_ | Weather condition. Valid values: `clear`, `rain`, `fog`. Affects visibility, sound propagation, and vehicle handling. |
| `vote_weight` | integer | No | `50` | Weight used in the map voting system. Higher values make this map more likely to appear as a vote option. Valid range: 1–100. |
| `description` | string | No | _(empty)_ | Brief description of the map's setting and gameplay characteristics. |

### Built-in Maps

| MapID | Display Name | Modes | Max Players | Time | Weather | Weight |
|---|---|---|---|---|---|---|
| `carcassonne` | Carcassonne | Conquest, HotZone, Domination | 64 | Day | Clear | 80 |
| `hill_400` | Hill 400 | Conquest, Elimination, Domination | 48 | Dawn | Fog | 60 |
| `rubber_plant` | Rubber Plant | Conquest, HotZone, CTF | 56 | Day | Rain | 70 |
| `hacienda` | Hacienda | HotZone, Elimination, Domination | 64 | Dusk | Clear | 75 |
| `hill_937` | Hill 937 | HotZone, CTF, Elimination | 64 | Night | Clear | 50 |
| `village` | Village | Conquest, Domination, CTF | 64 | Day | Rain | 65 |
| `skirmish_field` | Skirmish Field | Elimination, Domination | 32 | — | — | 50 |
| `coastal_assault` | Coastal Assault | Conquest, HotZone | 64 | Dawn | Clear | 55 |

---

## 6 · weapons.ini — Weapon Definitions

**Path:** `config/weapons.ini`

Defines all weapons available in-game with their attributes, statistics, and behavior.

### Schema

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `display_name` | string | Yes | — | Human-readable weapon name shown in the UI and kill feed. |
| `category` | enum | Yes | — | Weapon category determining loadout slot and UI grouping. Valid values: `primary`, `secondary`, `melee`, `explosive`. |
| `ammo_type` | string | Yes | — | Ammunition type identifier. Use `none` for melee weapons and thrown explosives. |
| `magazine_size` | integer | Yes | — | Number of rounds per magazine/clip. `0` for melee weapons. |
| `reserve_ammo` | integer | Yes | — | Starting reserve ammunition count. |
| `fire_rate_rpm` | integer | Yes | — | Cyclic fire rate in rounds per minute. `0` for single-shot or melee weapons. |
| `damage` | integer | Yes | — | Base damage per hit (or per bullet for automatic weapons). |
| `range_m` | integer | Yes | — | Effective range in meters. Damage may drop off beyond this distance. |
| `recoil_strength` | float | Yes | — | Normalized recoil intensity. Range: `0.0` (no recoil) to `1.0` (maximum recoil). |
| `spread_crouch` | float | Yes | — | Aim cone in degrees when the player is crouched. Lower values = more accurate. |
| `spread_stand` | float | Yes | — | Aim cone in degrees when the player is standing. |
| `reload_time_s` | float | Yes | — | Time in seconds to complete a full reload. |
| `attachments` | string | No | _(empty)_ | Comma-separated list of available attachments: `optics`, `grip`, `laser`. |
| `special` | enum | No | _(none)_ | Special fire mode. Valid values: `auto` (fully automatic), `burst` (3-round burst), `semi` (semi-automatic), `pump` (pump-action), `single` (single-use), `throw` (thrown), `melee` (melee attack). |
| `description` | string | No | _(empty)_ | Brief description of the weapon's characteristics and history. |

### Built-in Weapons

#### Primary Weapons

| WeaponID | Name | Ammo | Mag | Reserve | RPM | Damage | Range | Recoil | Special |
|---|---|---|---|---|---|---|---|---|---|
| `AK47` | AK-47 Assault Rifle | 7.62x39mm | 30 | 90 | 600 | 35 | 300m | 0.75 | auto |
| `M16A4` | M16A4 Burst Rifle | 5.56x45mm | 20 | 100 | 800 | 29 | 350m | 0.65 | burst |
| `AK74` | AK-74 Assault Rifle | 5.45x39mm | 30 | 90 | 620 | 33 | 310m | 0.70 | auto |

#### Secondary Weapons

| WeaponID | Name | Ammo | Mag | Reserve | Damage | Range | Special |
|---|---|---|---|---|---|---|---|
| `M9` | M9 Pistol | 9x19mm | 15 | 45 | 20 | 50m | semi |
| `PMM` | PMM Pistol | 9x18mm | 8 | 32 | 22 | 45m | semi |

#### Explosives

| WeaponID | Name | Ammo | Reserve | Damage | Range | Special |
|---|---|---|---|---|---|---|
| `M67` | M67 Frag Grenade | none | 3 | 100 | 15m | throw |
| `SPG9` | SPG-9 Recoilless Rifle | 73mm | 5 | 150 | 500m | single |

#### Melee

| WeaponID | Name | Damage | Range | Special |
|---|---|---|---|---|
| `Knife` | Combat Knife | 50 | 2m | melee |

---

## 7 · teams.ini — Team Definitions

**Path:** `config/teams.ini`

Defines the teams available in matches, their properties, and relationships.

### Schema

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `name` | string | Yes | — | Human-readable team name shown in the scoreboard and team selection screen. |
| `color` | string | Yes | — | RGB color for the team's chat messages and UI elements. Format: `R,G,B` with values 0–255. |
| `spawn_points_file` | path | No | _(empty)_ | Path to a file listing spawn point coordinates, relative to `config/` or `data/maps/<map>/`. |
| `default_loadout` | string | No | _(empty)_ | Default loadout ID (references `loadouts.ini`) assigned to players joining this team. |
| `score_multiplier` | float | No | `1.0` | Score multiplier applied to all points earned by players on this team. `1.0` = normal. Use values > 1.0 to give a team a scoring advantage (e.g., for asymmetric modes). |
| `friendly_to` | string | No | _(empty)_ | Comma-separated list of TeamIDs considered friendly (no friendly fire regardless of mode settings). |
| `enemy_to` | string | No | _(empty)_ | Comma-separated list of TeamIDs considered enemies. |
| `max_players` | integer | No | `0` | Maximum players on this team. `0` means unlimited (constrained by server `max_players`). |
| `min_players` | integer | No | `0` | Minimum players required on this team before a round can start. |
| `auto_balance` | boolean | No | `true` | When `true`, the auto-balance system may move players to this team to maintain equal team sizes. |
| `voice_channel` | string | No | _(empty)_ | Voice chat channel name for team-only communication. |
| `description` | string | No | _(empty)_ | Brief description of the team's faction and role. |

### Built-in Teams

| TeamID | Name | Color | Max Players | Auto-Balance | Description |
|---|---|---|---|---|---|
| `Red` | US Army | 255,0,0 | 32 | Yes | Allied forces (US & allies) |
| `Blue` | North Vietnamese Army | 0,0,255 | 32 | Yes | NVA and Viet Cong forces |
| `Spectator` | Spectator | 128,128,128 | 0 | No | Observers only; no gameplay participation |

---

## 8 · gameplay_settings.ini — Global Gameplay Settings

**Path:** `config/gameplay_settings.ini`

Global gameplay parameters that apply across all game modes unless overridden by a specific mode's settings. This file uses a flat key-value format (no section headers).

| Key | Type | Default | Description |
|---|---|---|---|
| `friendly_fire` | boolean | `false` | When `true`, players can damage teammates globally. Individual game modes can override this setting. |
| `respawn_delay_s` | integer | `5` | Default respawn delay in seconds after a player dies. Overridden by the game mode's `respawn_delay` if set. |
| `enable_round_timer` | boolean | `true` | When `true`, rounds have a time limit. The time limit value comes from the game mode's `timelimit_sec`. |
| `round_time_limit_s` | integer | `900` | Default round time limit in seconds. Used when a game mode does not specify its own `timelimit_sec`. |
| `enable_score_limit` | boolean | `true` | When `true`, reaching the score limit ends the round or match. The score limit value comes from the game mode's `score_limit`. |
| `score_limit` | integer | `1000` | Default score limit. Used when a game mode does not specify its own `score_limit`. |
| `enable_vehicles` | boolean | `true` | When `true`, vehicle spawning and vehicle gameplay is enabled. When `false`, all vehicle spawns are disabled. |

---

## 9 · admin_commands.ini — Admin Command Definitions

**Path:** `config/admin_commands.ini`

Defines all administrative commands, their handlers, arguments, permission levels, and aliases. The `AdminManager` parses this file at startup and builds a hash map for O(1) command lookup.

For detailed usage instructions and examples for every command, see [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md).

### Schema

Each command is defined as an INI section:

```ini
[SectionName]
cmd         = <primary command token>
aliases     = <comma-separated aliases>
handler     = <fully-qualified C++ handler>
args        = <space-separated argument placeholders>
minArgs     = <minimum required arguments>
maxArgs     = <maximum allowed arguments>
level       = <permission level 0-3>
description = <single-line help text>
```

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `cmd` | string | Yes | — | Primary command token (case-insensitive). Must be unique across all commands. |
| `aliases` | string | No | _(none)_ | Comma-separated list of alternative names for the command. Each alias must be unique. |
| `handler` | string | Yes | — | Fully-qualified C++ handler function registered in the `AdminManager`. |
| `args` | string | No | _(none)_ | Space-separated argument placeholders. Use `<name>` for required args, `[name]` for optional, `<name...>` for variadic. |
| `minArgs` | integer | No | _(count of required args)_ | Minimum number of arguments the command accepts. |
| `maxArgs` | integer | No | _(count of all args)_ | Maximum number of arguments the command accepts. |
| `level` | integer | Yes | — | Minimum permission level required: `0` = player, `1` = helper, `2` = moderator, `3` = administrator. |
| `description` | string | Yes | — | Single-line help text shown by the `help` command. |

---

## 10 · Text-Based List Files

These files use a simple line-based format where `#` indicates a comment.

### 10.1 admin_list.txt

**Path:** `config/admin_list.txt`

Maps SteamID64 identifiers to permission levels, granting administrative access.

**Format:**
```
<SteamID64>    <Level>    <Name/Comment>
```

**Permission Levels:**

| Level | Role | Capabilities |
|---|---|---|
| `0` | Player | No admin access. Can only use `help` command. |
| `1` | Helper | Basic informational commands (`status`). Can view server state but cannot modify it. |
| `2` | Moderator | Player management commands (`kick`, `ban`, `broadcast`, `give`, `tp`, `god`, `banlist`). Can moderate players and issue items. |
| `3` | Administrator | Full access to all commands including server management (`shutdown`, `restart`, `changemap`, `rotation`, `timescale`, `tickrate`, `spawnbot`, `eacmode`, `unban`). |

**Example:**
```
76561198000000001    3    Lead Admin - "Krill"
76561198000000002    2    Moderator - "Alice"
76561198000000003    1    Helper - "Charlie"
```

### 10.2 auth_tokens.txt

**Path:** `config/auth_tokens.txt`

Fallback authentication tokens used when Steam authentication is unavailable. Tokens are checked line by line when `fallback_custom_auth = true` in `[Security]`.

**Format:** One token per line. Tokens can be SteamID64 values or custom alphanumeric strings.

```
76561198000000001
ABCDEF1234567890
CLIENTTOKEN-001
```

### 10.3 ban_list.txt

**Path:** `config/ban_list.txt`

Active ban list. The server checks this file during the connection handshake to reject banned players.

**Format:**
```
<SteamID64>    <Type>    <ExpiresAt>    <Reason>
```

| Field | Description |
|---|---|
| `SteamID64` | 64-bit Steam ID of the banned player |
| `Type` | `perm` for permanent ban, `temp` for temporary ban |
| `ExpiresAt` | UTC timestamp in `YYYY-MM-DD HH:MM:SS` format, or `permanent` for permanent bans |
| `Reason` | Human-readable reason (optional) |

**Example:**
```
76561198000000010    perm    permanent              Cheating detected via EAC
76561198000000011    temp    2025-07-15 14:30:00    Toxic behavior
```

Temporary bans are automatically removed after their expiration time. The server checks expiration on each connection attempt and during periodic cleanup.

### 10.4 ip_blacklist.txt

**Path:** `config/ip_blacklist.txt`

IP-level deny list. Connections from listed IPs or CIDR ranges are rejected at the network layer before any protocol processing.

**Format:** One IP address or CIDR range per line.

```
# Single IPs
192.0.2.123
203.0.113.45

# CIDR Ranges (blocks the entire subnet)
198.51.100.0/24
```

**Notes:**
- This is checked before Steam authentication, making it effective against DDoS sources.
- Use CIDR notation for blocking entire subnets.
- Changes are hot-reloaded.

### 10.5 motd.txt

**Path:** `config/motd.txt`

Message of the Day displayed to players when they connect to the server. Supports formatting codes and variable substitution.

**Formatting Codes:**

| Code | Effect |
|---|---|
| `[COLOR=R,G,B]...[/COLOR]` | Set text color (RGB values 0–255) |
| `[B]...[/B]` | Bold text |
| `[I]...[/I]` | Italic text |
| `[U]...[/U]` | Underlined text |
| `[URL=link]...[/URL]` | Clickable hyperlink |

**Variable Substitution:**

| Variable | Replaced With |
|---|---|
| `{server_name}` | Current server name from `[General].server_name` |
| `{player_count}` | Current connected player count and max (e.g., "32/64") |
| `{map_name}` | Current map display name |
| `{uptime}` | Server uptime since last restart (e.g., "2d 5h 30m") |
| `{next_map}` | Next map in the rotation |

**Example:**
```
[COLOR=0,255,0][B]Welcome to {server_name}![/B][/COLOR]

[I]Players:[/I] {player_count}   [I]Map:[/I] {map_name}
[I]Uptime:[/I] {uptime}   [I]Next Map:[/I] {next_map}

1. No cheating or exploiting. EAC is enforced.
2. Be respectful.
3. Have fun and play fair!

[COLOR=128,128,128][I]Type [B]/help[/B] in chat for admin commands.[/I][/COLOR]
```

### 10.6 workshop_items.txt

**Path:** `config/workshop_items.txt`

Steam Workshop items for automatic download. When a client connects, these items are required and will be downloaded if not already present.

**Format:**
```
<WorkshopID>    <Type>    <FileName>    <LocalPath>    # <Description>
```

| Field | Description |
|---|---|
| `WorkshopID` | Steam Workshop numeric item ID |
| `Type` | Item type: `map`, `mod`, or `asset` |
| `FileName` | Expected filename after extraction (e.g., `desert_assault.umap`) |
| `LocalPath` | Server-side storage path relative to the data directory |
| `Description` | Human-readable description (optional, after `#`) |

**Example:**
```
123456789    map     desert_assault.umap    maps/desert_assault    # Desert Assault map
223344556    mod     night_ops.pak          mods/night_ops         # Night Operations weather cycle
556677889    asset   vietnam_uniforms.pak   assets/uniforms        # Vietnam-era soldier uniforms
```

---

## 11 · eac_scanner.json — Anti-Cheat Scanner Configuration

**Path:** `config/eac_scanner.json`

JSON configuration for the EAC (Easy Anti-Cheat) memory scanner subsystem. This configures how the server scans client memory for suspicious patterns.

```json
{
  "scanner": {
    "scanIntervalMs": 1000,
    "maxScanThreads": 2,
    "memoryRegions": [
      {
        "name": "GameCode",
        "pattern": "48 8B ?? ?? ?? ?? ?? 48 85 C0",
        "offset": 0
      },
      {
        "name": "ClientModule",
        "pattern": "55 8B EC 83 E4 F8 83 EC 70",
        "offset": 0
      }
    ],
    "maxRegionsToScan": 4,
    "readBlockSize": 4096,
    "suspiciousThreshold": 5,
    "reportOnThreshold": true
  },
  "logging": {
    "enable": true,
    "logFile": "logs/eac_scanner.log",
    "level": "DEBUG"
  }
}
```

### Scanner Settings

| Key | Type | Default | Description |
|---|---|---|---|
| `scanIntervalMs` | integer | `1000` | Interval in milliseconds between memory scans. Lower values increase detection speed but add CPU overhead. |
| `maxScanThreads` | integer | `2` | Maximum number of concurrent scan threads. |
| `memoryRegions` | array | — | List of memory regions to scan, each with a `name`, byte `pattern` (hex with `??` wildcards), and byte `offset`. |
| `maxRegionsToScan` | integer | `4` | Maximum number of memory regions to scan per interval. |
| `readBlockSize` | integer | `4096` | Size in bytes of each memory read operation. |
| `suspiciousThreshold` | integer | `5` | Number of suspicious pattern matches required before triggering a report. |
| `reportOnThreshold` | boolean | `true` | When `true`, a security report is generated when the suspicious threshold is reached. |

### Logging Settings

| Key | Type | Default | Description |
|---|---|---|---|
| `enable` | boolean | `true` | When `true`, EAC scanner events are logged. |
| `logFile` | string | `logs/eac_scanner.log` | Path to the scanner log file, relative to server root. |
| `level` | enum | `DEBUG` | Log level for scanner output. Valid values: `DEBUG`, `INFO`, `WARN`, `ERROR`. |

---

## 12 · loadouts.ini — Player Loadout Definitions

**Path:** `config/loadouts.ini`

Defines player loadout presets that can be assigned to teams via `teams.ini` or selected by players. This file is currently a placeholder for future implementation.

**Expected Schema:**
```ini
[LoadoutID]
primary    = <WeaponID>
secondary  = <WeaponID>
melee      = <WeaponID>
explosive  = <WeaponID>
```

---

**End of CONFIGURATION.md**

For questions about specific configuration options, see [FAQ.md](FAQ.md). For troubleshooting configuration issues, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
