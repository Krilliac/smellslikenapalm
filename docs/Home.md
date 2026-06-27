# RS2V Custom Server — Documentation Home

Welcome to the **RS2V Custom Server** documentation. This page serves as the central index for all project documentation, organized by topic and audience.

RS2V Custom Server is a clean-room dedicated-server **emulator** for Rising Storm 2: Vietnam (an Unreal Engine 3 game, `EngineVersion 7258`), built in C++23. It is organized around a production-style architecture with telemetry, security, and networking subsystems.

> **Status:** This project is under active development. The main server target builds on MSVC and MinGW, but the UE3 control-channel handshake is still being implemented and **a stock client cannot fully connect yet**. C# scripting is currently **disabled** (it does not build), and telemetry/security are partially stubbed. The public API, file layout, and naming conventions are not yet frozen and may change without notice. See the README's *Current Status* section and [TODO.md](../TODO.md) for the current roadmap.

---

## Quick Navigation

| I want to... | Go to |
|---|---|
| **Get the server running quickly** | [Quick Start](#quick-start) |
| **Understand every configuration option** | [Configuration Reference](CONFIGURATION.md) |
| **Learn the admin commands** | [Admin Commands Reference](ADMIN_COMMANDS.md) |
| **Set up game modes** | [Game Modes Guide](GAME_MODES.md) |
| **Configure maps and rotations** | [Maps & Map Management](MAPS.md) |
| **Deploy to production** | [Deployment Guide](DEPLOYMENT.md) |
| **Write C# scripts or native plugins** | [Scripting Guide](SCRIPTING.md) |
| **Monitor with Prometheus/Grafana** | [Telemetry Guide](TELEMETRY.md) |
| **Understand the architecture** | [Architecture Overview](ARCHITECTURE.md) |
| **Contribute code** | [Contributing Guidelines](../CONTRIBUTING.md) |
| **Find answers to common questions** | [FAQ](FAQ.md) |
| **Troubleshoot an issue** | [Troubleshooting Guide](TROUBLESHOOTING.md) |

---

## Documentation by Audience

### For Server Operators

These documents cover everything you need to install, configure, run, and maintain an RS2V server in production.

| Document | Description |
|---|---|
| [Configuration Reference](CONFIGURATION.md) | **Complete reference** for every configuration file and setting. Covers `server.ini`, `game_modes.ini`, `maps.ini`, `weapons.ini`, `teams.ini`, `gameplay_settings.ini`, `admin_commands.ini`, all text-based list files, and the `eac_scanner.json` anti-cheat configuration. Includes configuration hierarchy, environment variables, and hot-reload behavior. |
| [Admin Commands Reference](ADMIN_COMMANDS.md) | **Comprehensive command reference** for all 18 administrative commands. Covers permission levels, syntax, aliases, arguments, usage examples, RCON setup and connection, in-game chat command usage, and admin list file management. |
| [Game Modes Guide](GAME_MODES.md) | **Detailed guide** to all five built-in game modes (Conquest, Elimination, Capture the Flag, Hot Zone, Domination). Covers win conditions, team configurations, scoring, timing, map compatibility, and how to create custom game modes. |
| [Maps & Map Management](MAPS.md) | **Map reference** for all eight built-in maps. Covers map properties, rotation configuration, adding custom maps, Steam Workshop integration, and the map voting system. |
| [Deployment Guide](DEPLOYMENT.md) | **Production deployment** procedures including bare-metal installation, Docker containerization, Kubernetes orchestration, systemd service management, cloud deployment strategies, and scaling considerations. |
| [Telemetry Guide](TELEMETRY.md) | **Monitoring setup** for Prometheus metrics endpoints, Grafana dashboard configuration, file-based metrics reporting, alerting thresholds, and performance profiling. |
| [Security Guide](SECURITY.md) | **Security architecture** covering Steam authentication, EAC anti-cheat integration, input validation, movement verification, rate limiting, ban management, RCON security, and the defense-in-depth model. |
| [Troubleshooting Guide](TROUBLESHOOTING.md) | **Diagnostic procedures** for common issues including server startup failures, network connectivity problems, performance degradation, authentication errors, and anti-cheat false positives. |
| [FAQ](FAQ.md) | **Frequently asked questions** covering build prerequisites, platform support, runtime operation, administration, networking, anti-cheat, scripting, and telemetry. |

### For Developers & Contributors

These documents cover building from source, the internal architecture, the API surface, coding standards, and how to contribute.

| Document | Description |
|---|---|
| [Development Guide](DEVELOPMENT.md) | **Build from source** instructions, local development workflow, CMake options, coding standards, debugging techniques, test execution, and CI/CD pipeline overview. |
| [Architecture Overview](ARCHITECTURE.md) | **System design** document covering the modular architecture, subsystem interactions, data flow patterns, threading model, memory management, networking internals, physics engine, replication system, and security architecture. |
| [API Reference](API.md) | **Complete API reference** for all public classes and interfaces: `ConfigManager`, `ServerConfig`, `NetworkManager`, `Packet`, `ReplicationManager`, `TelemetryManager`, `ScriptHost`, `SecurityManager`, `PhysicsEngine`, `GameServer`, `PlayerManager`, `TeamManager`, and the testing utilities. |
| [Scripting Guide](SCRIPTING.md) | **Plugin development** guide for C# scripting (hot-reload, sandbox security, event hooks) and native C++ plugin development (handler libraries, dynamic loading, API bindings). **Note:** the C# scripting host is currently disabled (does not build) — this documents the intended design. |
| [Contributing Guidelines](../CONTRIBUTING.md) | **How to contribute** including fork/branch workflow, coding standards, commit message conventions, testing requirements, pull request process, and issue reporting guidelines. |

---

## Quick Start

### Prerequisites

| Requirement | Minimum | Recommended |
|---|---|---|
| C++ Compiler (C++23) | GCC 12, Clang 15, or MSVC VS 2022 | GCC 13+, Clang 18+ |
| CMake | 3.20 | 3.25+ |
| OpenSSL | 1.1.0 (optional) | 3.0+ |
| zlib | 1.2.11 (optional) | 1.3+ |
| .NET SDK | 7.0 (for scripting) | 8.0+ |

### Build from Source

```bash
# 1. Clone the repository
git clone https://github.com/Krilliac/smellslikenapalm.git
cd smellslikenapalm

# 2. Create a build directory
mkdir build && cd build

# 3. Configure with CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# 4. Build (uses all available cores)
cmake --build . --parallel

# 5. The binary is at:
#    build/rs2v_server  (Linux/macOS)
#    build/Release/rs2v_server.exe  (Windows)
```

### CMake Build Options

| Option | Default | Description |
|---|---|---|
| `ENABLE_TELEMETRY` | `ON` | Build with the telemetry subsystem (Prometheus metrics, file reporters, alerting) |
| `ENABLE_SCRIPTING` | `OFF` | Build with C# scripting support. **Currently does not build** — the host uses a deprecated .NET COM hosting API and is disabled pending a rework. |
| `ENABLE_COMPRESSION` | `ON` | Build with packet compression (uses zlib if available, otherwise built-in stub) |
| `BUILD_TESTS` | `OFF` | Build the native test suite (no GoogleTest dependency) |

### Run the Server

```bash
# Run with default configuration
./rs2v_server --config config/server.ini

# Run with a specific port
./rs2v_server --config config/server.ini --port 7777

# Run with verbose logging for development
./rs2v_server --config config/server.ini --log-level DEBUG

# Run with production settings
./rs2v_server --config config/server_production.ini
```

### Docker Quick Start

```bash
# Build the container
docker build -t rs2v-server .

# Run with persistent configuration and logs
docker run -d \
  --name rs2v-server \
  -p 7777:7777/udp \
  -p 9100:9100/tcp \
  -v $(pwd)/config:/app/config \
  -v $(pwd)/logs:/app/logs \
  rs2v-server
```

---

## Project Structure

```
smellslikenapalm/
├── src/                        # C++ source code
│   ├── main.cpp                # Entry point
│   ├── Config/                 # Configuration management
│   ├── Game/                   # Game logic (modes, maps, players, teams)
│   ├── Network/                # Networking stack (UDP, TCP, protocol)
│   ├── Physics/                # Physics engine (rigid bodies, collision)
│   ├── Protocol/               # UE3 control-channel / replication protocol (handshake WIP)
│   ├── Scripting/              # C# scripting host (.NET COM hosting — currently disabled)
│   ├── Security/               # Anti-cheat emulation, auth, input validation
│   ├── Time/                   # Time management and synchronization
│   └── Utils/                  # Shared utilities (logging, threading, crypto)
├── telemetry/                  # Telemetry subsystem
├── config/                     # Configuration files
│   ├── server.ini              # Main server configuration
│   ├── server_production.ini   # Production overrides
│   ├── game_modes.ini          # Game mode definitions
│   ├── maps.ini                # Map rotation and settings
│   ├── weapons.ini             # Weapon definitions and stats
│   ├── teams.ini               # Team definitions
│   ├── gameplay_settings.ini   # Global gameplay settings
│   ├── admin_commands.ini      # Admin command definitions
│   ├── admin_list.txt          # Admin SteamID list with permission levels
│   ├── auth_tokens.txt         # Fallback auth tokens
│   ├── ban_list.txt            # Active ban list
│   ├── ip_blacklist.txt        # IP/CIDR deny list
│   ├── motd.txt                # Message of the Day
│   ├── workshop_items.txt      # Steam Workshop items for auto-download
│   ├── eac_scanner.json        # EAC scanner configuration
│   └── loadouts.ini            # Player loadout definitions
├── data/                       # Runtime data
│   ├── maps/                   # Map assets
│   └── scripts/                # C# scripts
│       ├── enabled/            # Active scripts (loaded at startup)
│       └── disabled/           # Inactive scripts (available for activation)
├── docs/                       # Documentation (you are here)
├── tests/                      # Native test suite (no GoogleTest) + fuzz tier
├── .github/                    # GitHub configuration
│   ├── ISSUE_TEMPLATE/         # Bug report & feature request templates
│   └── workflows/              # CI/CD (Codacy)
├── CMakeLists.txt              # Build system
├── LICENSE                     # Non-commercial open source license
├── README.md                   # Project overview
├── TODO.md                     # Development roadmap
├── CHANGELOG.md                # Release history
├── CONTRIBUTORS.md             # Project contributors
└── CONTRIBUTING.md             # Contribution guidelines
```

---

## Full Documentation Index

### Root-Level Documents

| File | Description |
|---|---|
| [README.md](../README.md) | Project overview, key features, quick start, and high-level architecture |
| [TODO.md](../TODO.md) | Development roadmap and task tracking |
| [CHANGELOG.md](../CHANGELOG.md) | Version history and release notes |
| [CONTRIBUTORS.md](../CONTRIBUTORS.md) | Project contributors and acknowledgments |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | How to contribute to the project |
| [LICENSE](../LICENSE) | RS2V Server Non-Commercial Open Source License |

### Documentation Directory (`docs/`)

| File | Audience | Description |
|---|---|---|
| [Home.md](Home.md) | Everyone | This page — central documentation index |
| [CONFIGURATION.md](CONFIGURATION.md) | Operators | Complete configuration file reference |
| [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md) | Operators | Admin and RCON command reference |
| [GAME_MODES.md](GAME_MODES.md) | Operators | Game modes guide with all five modes |
| [MAPS.md](MAPS.md) | Operators | Maps reference and map management |
| [FAQ.md](FAQ.md) | Everyone | Frequently asked questions |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Operators | Production deployment guide |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | Operators | Diagnostic procedures and common fixes |
| [SECURITY.md](SECURITY.md) | Operators/Devs | Security architecture and best practices |
| [TELEMETRY.md](TELEMETRY.md) | Operators/Devs | Telemetry system and monitoring setup |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Developers | System design and internal architecture |
| [API.md](API.md) | Developers | Complete API reference for all subsystems |
| [SCRIPTING.md](SCRIPTING.md) | Modders/Devs | Plugin and script development guide |
| [DEVELOPMENT.md](DEVELOPMENT.md) | Developers | Build, debug, test, and contribute |

---

## External Resources

- **GitHub Repository**: [github.com/Krilliac/smellslikenapalm](https://github.com/Krilliac/smellslikenapalm)
- **Issue Tracker**: [GitHub Issues](https://github.com/Krilliac/smellslikenapalm/issues)
- **Discussions**: [GitHub Discussions](https://github.com/Krilliac/smellslikenapalm/discussions)
- **Discord**: [discord.gg/sd8HaMc8rh](https://discord.gg/sd8HaMc8rh)

---

## License

This project is licensed under the **RS2V Server Non-Commercial Open Source License**. Key terms:

- Source code must accompany all distributions (including derivatives)
- Commercial use is strictly prohibited
- No closed-source distribution
- No reverse engineering of proprietary clients or anti-cheat systems

See [LICENSE](../LICENSE) for the full text.

---

*This documentation is maintained alongside the codebase. For corrections or additions, please open a pull request or file an issue.*
