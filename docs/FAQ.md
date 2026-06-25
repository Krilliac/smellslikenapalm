# FAQ.md — Frequently Asked Questions

This document answers **common questions** about building, running, configuring, and troubleshooting the RS2V Custom Server.

For detailed configuration, see [CONFIGURATION.md](CONFIGURATION.md). For diagnostic procedures, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## Table of Contents

- [Building & Compiling](#building--compiling)
- [Running the Server](#running-the-server)
- [Configuration](#configuration)
- [Administration](#administration)
- [Networking](#networking)
- [Anti-Cheat (EAC)](#anti-cheat-eac)
- [Scripting & Plugins](#scripting--plugins)
- [Telemetry & Monitoring](#telemetry--monitoring)
- [Game Modes & Maps](#game-modes--maps)
- [Performance](#performance)
- [Licensing & Legal](#licensing--legal)

---

## Building & Compiling

### What are the build prerequisites?

You need:
- **C++17 compiler**: GCC 8+, Clang 7+, or MSVC 2019+
- **CMake 3.15+**
- **Threads support** (POSIX threads on Linux, Windows threads on Windows)

Optional dependencies:
- **OpenSSL 1.1.0+** — for Base64 encoding and cryptographic functions. If not found, a built-in implementation is used.
- **zlib 1.2.11+** — for packet compression. If not found, a built-in stub is used.
- **.NET SDK 7.0+** — required only if building with C# scripting support (`ENABLE_SCRIPTING=ON`). Note: scripting is currently **disabled by default and does not build** (see the Scripting & Plugins section).

See [DEVELOPMENT.md](DEVELOPMENT.md) for platform-specific installation commands.

---

### What platforms are supported?

| Platform | Status | Compiler |
|---|---|---|
| **Linux** (Ubuntu 18.04+, CentOS 7+, Debian 10+) | Fully supported | GCC 8+, Clang 7+ |
| **Windows** (Windows 10+) | Fully supported | MSVC 2019+, MinGW |
| **macOS** | Experimental | Clang (Xcode 11+) |

Linux is the primary development and deployment platform. Windows is fully supported for both development and production. macOS support is experimental.

---

### How do I build from source?

```bash
git clone https://github.com/Krilliac/smellslikenapalm.git
cd smellslikenapalm
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

The binary is output to `build/rs2v_server` (Linux) or `build/Release/rs2v_server.exe` (Windows).

---

### What CMake build options are available?

| Option | Default | Description |
|---|---|---|
| `ENABLE_TELEMETRY` | `ON` | Build with telemetry subsystem (Prometheus metrics, file reporters) |
| `ENABLE_SCRIPTING` | `OFF` | Build with C# scripting support (requires .NET SDK). **Currently does not build** — disabled pending a rework (see below). |
| `ENABLE_COMPRESSION` | `ON` | Build with packet compression (uses zlib if available) |
| `BUILD_TESTS` | `OFF` | Build the Google Test suite |

Example with all options:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DENABLE_TELEMETRY=ON \
         -DENABLE_SCRIPTING=OFF \
         -DENABLE_COMPRESSION=ON \
         -DBUILD_TESTS=ON
```

---

### The build fails with "OpenSSL not found" — is that an error?

No. OpenSSL is optional. You will see this message:
```
OpenSSL not found — Base64 will use built-in implementation
```

This is informational, not an error. The server will build and run correctly without OpenSSL, using a built-in Base64 encoder. If you need OpenSSL for TLS/RCON encryption, install it:

```bash
# Ubuntu/Debian
sudo apt install libssl-dev

# CentOS/RHEL
sudo yum install openssl-devel

# Windows (vcpkg)
vcpkg install openssl
```

---

### The build fails with "zlib not found" — is that an error?

No. Like OpenSSL, zlib is optional. The message:
```
zlib not found — compression will use built-in stub
```

means compression will use a basic built-in implementation. For production servers, install zlib for better compression performance:

```bash
# Ubuntu/Debian
sudo apt install zlib1g-dev

# CentOS/RHEL
sudo yum install zlib-devel
```

---

### How do I build on Windows with Visual Studio?

```powershell
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Or open the generated `.sln` file in Visual Studio and build from the IDE.

---

### How do I run the tests?

Build with tests enabled, then run:
```bash
cmake .. -DBUILD_TESTS=ON
cmake --build . --parallel
ctest --verbose
```

---

## Running the Server

### How do I start the server?

```bash
./rs2v_server --config config/server.ini
```

For production with optimized settings:
```bash
./rs2v_server --config config/server_production.ini
```

---

### What command-line arguments does the server accept?

| Argument | Description | Example |
|---|---|---|
| `--config <path>` | Path to the primary configuration file | `--config config/server.ini` |
| `--port <number>` | Override the game port | `--port 7777` |
| `--log-level <level>` | Override log level | `--log-level DEBUG` |
| `--no-eac` | Disable Easy Anti-Cheat | `--no-eac` |
| `--enable-telemetry` | Force enable telemetry | `--enable-telemetry` |
| `--prometheus-port <number>` | Prometheus metrics port | `--prometheus-port 9100` |

Command-line arguments take the highest priority and override values in configuration files.

---

### How do I run the server as a background service?

**Linux (systemd):**

Create `/etc/systemd/system/rs2v-server.service`:
```ini
[Unit]
Description=RS2V Custom Server
After=network.target

[Service]
Type=simple
User=rs2v
WorkingDirectory=/opt/rs2v
ExecStart=/opt/rs2v/rs2v_server --config config/server_production.ini
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Then:
```bash
sudo systemctl daemon-reload
sudo systemctl enable rs2v-server
sudo systemctl start rs2v-server
```

See [DEPLOYMENT.md](DEPLOYMENT.md) for complete deployment guides including Docker and Kubernetes.

---

### How do I stop the server gracefully?

1. **RCON**: Connect via RCON and issue `shutdown`
2. **In-game**: If an admin is connected, use the `shutdown` command in chat
3. **Signal**: Send `SIGTERM` to the process: `kill $(pidof rs2v_server)`
4. **systemd**: `sudo systemctl stop rs2v-server`

Avoid using `kill -9` (SIGKILL) as it prevents graceful shutdown (state saving, player disconnection).

---

### Can I reload configuration without restarting?

Yes, for most configuration files. The server hot-reloads changes to:
- `game_modes.ini`, `maps.ini`, `weapons.ini`, `teams.ini`, `gameplay_settings.ini`
- `admin_list.txt`, `ban_list.txt`, `ip_blacklist.txt`, `motd.txt`
- Parts of `server.ini` (`[Logging]`, `[Telemetry]`, `[Admin]` sections)

Files that require a restart: `admin_commands.ini`, `workshop_items.txt`, `eac_scanner.json`, and `server.ini` sections `[General]` and `[Network]`.

See [CONFIGURATION.md](CONFIGURATION.md) for the complete hot-reload matrix.

---

## Configuration

### Where are the configuration files?

All configuration files are in the `config/` directory relative to the server root:

```
config/
├── server.ini              # Main server configuration
├── server_production.ini   # Production overrides
├── game_modes.ini          # Game mode definitions
├── maps.ini                # Map rotation and settings
├── weapons.ini             # Weapon definitions
├── teams.ini               # Team definitions
├── gameplay_settings.ini   # Global gameplay settings
├── admin_commands.ini      # Admin command definitions
├── admin_list.txt          # Admin SteamID list
├── auth_tokens.txt         # Fallback auth tokens
├── ban_list.txt            # Active ban list
├── ip_blacklist.txt        # IP deny list
├── motd.txt                # Message of the Day
├── workshop_items.txt      # Steam Workshop items
├── eac_scanner.json        # Anti-cheat scanner config
└── loadouts.ini            # Player loadouts (placeholder)
```

See [CONFIGURATION.md](CONFIGURATION.md) for a complete reference of every setting.

---

### What is the configuration priority order?

From highest to lowest priority:
1. **Command-line arguments** (`--port 8777`)
2. **Environment variables** (`RS2V_PORT=8777`)
3. **Config files** (`config/server.ini`)
4. **Default values** (hardcoded)
5. **Auto-detected values** (hardware detection)

---

### How do I set up a production server?

Use the production configuration file which has optimized settings:
```bash
./rs2v_server --config config/server_production.ini
```

Key production differences:
- Log level set to `warn` (less verbose)
- Console output disabled
- RCON-only administration
- Larger log file retention
- More pre-allocated memory
- Profiling disabled

See [DEPLOYMENT.md](DEPLOYMENT.md) for complete production deployment procedures.

---

## Administration

### How do I add an admin?

Add their SteamID64 to `config/admin_list.txt`:
```
76561198012345678    3    Admin - "PlayerName"
```

The number (0–3) is the permission level. Changes take effect immediately (hot-reloaded).

See [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md) for permission level details.

---

### How do I connect via RCON?

1. Ensure RCON is enabled in `config/server.ini`:
   ```ini
   [Admin]
   enable_rcon   = true
   rcon_port     = 27020
   rcon_password = YourSecurePassword
   ```
2. Open the RCON port in your firewall
3. Connect with any Source RCON-compatible client:
   ```bash
   rcon -H server_ip -p 27020 -P YourSecurePassword
   ```

See [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md) for the complete RCON setup guide.

---

### How do I ban a player?

Using the `ban` command (requires level 3 admin):
```
ban 76561198012345678 1440 Reason for ban
ban 76561198012345678 permanent Cheating
```

Duration is in minutes. Use `permanent` for permanent bans.

To remove a ban:
```
unban 76561198012345678
```

---

### How do I change the map?

```
changemap hill_400
```

This switches immediately. The map name must match a `[MapID]` section in `config/maps.ini`.

---

## Networking

### What ports does the server need?

| Port | Protocol | Purpose | Required |
|---|---|---|---|
| 7777 | UDP | Game traffic (configurable) | Yes |
| 27020 | TCP | RCON remote administration | If RCON enabled |
| 9100 | TCP | Prometheus metrics endpoint | If telemetry enabled |

### What firewall rules do I need?

```bash
# Linux (UFW)
sudo ufw allow 7777/udp      # Game traffic
sudo ufw allow 27020/tcp     # RCON (restrict to admin IPs)
sudo ufw allow 9100/tcp      # Prometheus (restrict to monitoring network)

# Linux (iptables)
sudo iptables -A INPUT -p udp --dport 7777 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 27020 -s 203.0.113.0/24 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 9100 -s 10.0.0.0/8 -j ACCEPT
```

---

### How much bandwidth does the server use?

Bandwidth scales with player count and tick rate. Approximate values at 60Hz tick rate:

| Players | Inbound | Outbound | Total |
|---|---|---|---|
| 16 | ~3 Mbps | ~12 Mbps | ~15 Mbps |
| 32 | ~6 Mbps | ~25 Mbps | ~31 Mbps |
| 64 | ~12 Mbps | ~50 Mbps | ~62 Mbps |

Outbound traffic is higher because the server sends state updates to all players.

---

### Can the server run on IPv6?

Yes. Set `dual_stack = true` in `[Network]` (enabled by default). The server will listen on both IPv4 and IPv6 addresses.

---

## Anti-Cheat (EAC)

### What EAC modes are available?

| Mode | Description | Use Case |
|---|---|---|
| `safe` | Passive monitoring only, no enforcement | Debugging false positives |
| `emulate` | Full EAC emulation with enforcement | Production servers |
| `off` | Anti-cheat disabled entirely | Development/testing |

Configure in `config/server.ini`:
```ini
[Security]
enable_anti_cheat = true
anti_cheat_mode   = emulate
```

Or change at runtime:
```
eacmode emulate
```

---

### How does the anti-cheat work?

The server implements EAC emulation through:
1. **Client memory scanning** — Checks for known cheat signatures in configurable memory regions
2. **Behavioral analysis** — Detects anomalous patterns like impossible movement speeds or aim accuracy
3. **Input validation** — Verifies that client inputs are physically possible
4. **Statistical anomaly detection** — Flags players whose performance metrics deviate significantly from normal

See [SECURITY.md](SECURITY.md) for the complete security architecture.

---

### Is this actual EAC or an emulator?

This is an **independent emulation** of EAC behavior, developed solely from publicly available information and legitimate runtime observations. No proprietary EAC code or assets are used. See the [LICENSE](../LICENSE) for legal details.

---

## Scripting & Plugins

### How do I enable C# scripting?

> **Status:** C# scripting is currently **disabled** and **does not build**. The host relies on a deprecated .NET COM hosting API (`ICorRuntimeHost`) and needs to be reworked onto a supported hosting API before it can be used. `ENABLE_SCRIPTING` defaults to `OFF`. The steps below describe the intended workflow once the host is restored.

1. Build with scripting support: `cmake .. -DENABLE_SCRIPTING=ON`
2. Enable in configuration:
   ```ini
   [Scripting]
   enable_csharp_scripting = true
   scripts_path = data/scripts/
   ```
3. Place `.cs` scripts in `data/scripts/enabled/`

---

### How do I write a script?

Scripts are C# files that hook into server events. Example:

```csharp
using System;

public class WelcomeScript
{
    public static void OnPlayerConnected(PlayerInfo player)
    {
        Server.BroadcastMessage($"Welcome {player.Name} to the server!");
    }
}
```

See [SCRIPTING.md](SCRIPTING.md) for the complete scripting API reference.

---

### Do scripts hot-reload?

Yes. When a `.cs` file in `data/scripts/enabled/` is modified, the server detects the change and recompiles the script automatically. The debounce interval is configurable via `script_reload_interval_ms` (default: 500ms).

---

### What example scripts are included?

The project includes a set of example scripts in `data/scripts/disabled/` (these target the scripting host, which is currently disabled — see above). A representative subset:

| Script | Purpose |
|---|---|
| `OnPlayerJoinWelcomeAndCommands.cs` | Welcome message and command list on player join |
| `FactionBalancer.cs` | Automatic team balancing |
| `PersistentLeaderboard.cs` | Persistent score tracking |
| `MetricsReporter.cs` | Custom telemetry metrics |
| `MovementValidator.cs` | Anti-cheat movement validation |
| `DynamicSpawner.cs` | Dynamic spawn point management |
| `GameModeAndScoreOverrides.cs` | Custom game mode scoring |
| `LogLevelController.cs` | Runtime log level adjustment |
| `Snapshotter.cs` | Game state snapshotting |
| `StateValidator.cs` | State consistency validation |

To enable a script, move it from `disabled/` to `enabled/`.

---

## Telemetry & Monitoring

### How do I set up Prometheus monitoring?

1. Build with telemetry: `cmake .. -DENABLE_TELEMETRY=ON`
2. Start the server — the Prometheus endpoint is available at `http://localhost:9100/metrics`
3. Add to your Prometheus `prometheus.yml`:
   ```yaml
   scrape_configs:
     - job_name: 'rs2v'
       static_configs:
         - targets: ['your-server:9100']
   ```

See [TELEMETRY.md](TELEMETRY.md) for Grafana dashboard setup and alerting configuration.

---

### What metrics are exposed?

Key metrics include:
- `rs2v_server_active_connections` — Current player count
- `rs2v_server_cpu_usage_percent` — CPU utilization
- `rs2v_server_memory_usage_percent` — Memory utilization
- `rs2v_server_tick_rate_hz` — Current tick rate
- `rs2v_server_packet_loss_rate` — Network packet loss
- `rs2v_server_security_violations_total` — Security event counter

---

## Game Modes & Maps

### What game modes are available?

Five built-in modes: **Conquest**, **Elimination**, **Capture the Flag**, **Hot Zone**, and **Domination**. See [GAME_MODES.md](GAME_MODES.md) for detailed descriptions.

### How many maps are included?

Eight built-in maps: Carcassonne, Hill 400, Rubber Plant, Hacienda, Hill 937, Village, Skirmish Field, and Coastal Assault. See [MAPS.md](MAPS.md) for details.

### Can I create custom game modes?

Yes. Add a new section to `config/game_modes.ini` using one of the existing win condition types. For completely custom logic, use the C# scripting system. See [GAME_MODES.md](GAME_MODES.md).

### Can I add custom maps?

Yes. Place the `.umap` file in `data/maps/`, add a section to `config/maps.ini`, and the map is available for use. See [MAPS.md](MAPS.md).

---

## Performance

### What hardware do I need?

| Component | Minimum | Recommended |
|---|---|---|
| CPU | 2 cores, 2.5 GHz | 4+ cores, 3.0 GHz+ |
| Memory | 4 GB RAM | 8 GB+ RAM |
| Storage | 10 GB free | 50 GB+ SSD |
| Network | 100 Mbps | 1 Gbps+ |

### How do I optimize server performance?

1. Use the production config: `--config config/server_production.ini`
2. Disable profiling: `enable_profiling = false`
3. Increase memory pre-allocation: `preallocate_chunks = 8`
4. Use dynamic tuning: `dynamic_tuning_enabled = true`
5. Match tick rate to player count: 60 Hz for casual, 128 Hz for competitive
6. Pin to specific CPU cores if sharing hardware: `cpu_affinity_mask = 0x0F`

---

## Licensing & Legal

### What license is the project under?

The **RS2V Server Non-Commercial Open Source License**. Key terms:
- Source code must accompany all distributions
- Commercial use is strictly prohibited
- No closed-source distribution
- No reverse engineering of proprietary clients or anti-cheat systems

See [LICENSE](../LICENSE) for the full text.

### Is this affiliated with Tripwire Interactive?

No. This is an independent, open-source project not affiliated with, endorsed by, or associated with Tripwire Interactive, Antimatter Games, Epic Games, or Easy Anti-Cheat.

### Can I sell servers running this software?

No. The license strictly prohibits commercial use, including selling, licensing, or distributing for profit.

---

**End of FAQ.md**

Didn't find your answer? Check [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for diagnostic procedures or open an issue on [GitHub](https://github.com/Krilliac/smellslikenapalm/issues).
