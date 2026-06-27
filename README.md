# RS2V Custom Server

<span style="color:#d9534f; font-weight:bold;">⚠️ HEADS-UP: This repository is under active development.</span>
<span style="color:#f0ad4e;">- The public API, file layout, and naming conventions are not yet frozen and may change without notice.</span>

<span style="color:#f0ad4e;">- Some subsystems are still stubs or proofs-of-concept.</span>

<span style="color:#5bc0de;">- The main server target now builds on MSVC and MinGW, but the live netcode is still a work in progress (see Current Status below) — a stock client cannot fully connect yet.</span>

<span style="color:#428bca;">We welcome issues and pull-requests, but please sync often and review <a href="TODO.md">TODO.md</a> before starting major work.</span>

A modular, extensible dedicated-server *emulator* for Rising Storm 2: Vietnam, written in C++17. The codebase is organized around a production-style architecture (telemetry, security, and networking subsystems), but many of those subsystems are still in development — see **Current Status** below before relying on any of them.

## 📌 Current Status (as of this branch)

**What this is:** a from-scratch, clean-room dedicated-**server emulator** for *Rising Storm 2: Vietnam*, a game built on Unreal Engine 3 (`EngineVersion 7258`). It is **not** an official server and is not affiliated with Tripwire Interactive or Epic Games.

**Where it stands today:**

- ✅ **Builds clean** on MSVC (`build/`) and MinGW (`build-mingw/`). The main server target compiles.
- 🚧 **UE3 control-channel handshake is being implemented.** The Hello / Challenge / Login / Welcome message ordinals have been reverse-engineered from the retail client and are being wired up (see `src/Network/ControlChannel.*`, `NetMessages.h`, and `src/Protocol/ReverseEngineering/`). **A stock retail client cannot fully connect yet.**
- 🚧 **Live netcode is still partly placeholder.** The socket, packet, and replication plumbing exists, but end-to-end gameplay traffic is not yet functional.
- ⛔ **C# (.NET) scripting is currently disabled.** The scripting host relies on a deprecated .NET COM hosting API (`ICorRuntimeHost`) that does not build; `ENABLE_SCRIPTING` defaults to `OFF` and the engine needs to be reworked onto a supported hosting API. See `src/Scripting/`.
- 🚧 **Telemetry and security are partially stubbed.** Interfaces and reporters exist, and the EAC layer is an independent *emulation* (not real EAC), but coverage is incomplete.

The rest of this README describes the intended architecture and feature set. Treat feature claims below as **design targets / work in progress** rather than finished, battle-tested functionality. See [TODO.md](TODO.md) for the current roadmap.

## 🌟 Key Features (design goals)

> The items below describe the **target** feature set. Several are partial, stubbed, or not yet wired into the live server — see **Current Status** above.

### 🌐 Networking
- **TCP/UDP Protocol Stack**: Custom socket layer with a packet/replication pipeline
- **UE3-Compatible Protocol**: Targeting the RS2:V "bunch" / control-channel protocol (handshake in progress — see Current Status)
- **Network Quality Management**: Bandwidth accounting and connection-quality monitoring
- **Compression**: Configurable packet compression (zlib, with a built-in fallback stub)

### 🎮 Game Systems
- **Physics**: Rigid-body, collision, and vehicle simulation scaffolding
- **Player Management**: Player lifecycle, team balancing, and statistics tracking
- **Match Management**: Round systems, objective tracking, and configurable game modes
- **Map Systems**: Map loading, rotation management, and spawn-point handling

### 🔒 Security (emulated, partial)
- **Anti-Cheat Emulation**: An independent EAC *emulation* layer plus behavioral/statistical checks (not real Easy Anti-Cheat)
- **Authentication Framework**: Steam-style authentication and session management
- **Input Validation**: Packet validation and movement verification
- **Audit Logging**: Action and security-event logging

### 📊 Telemetry
- **Metrics Collection**: System metrics (CPU, memory, network) and application metrics
- **Multiple Export Formats**: JSON files, a Prometheus endpoint, CSV exports, and in-memory buffers
- **Performance Profiling**: Timing and bottleneck instrumentation
- **Alerting**: Threshold-based monitoring with configurable notifications

### 🔧 Extensibility & Scripting (scripting currently disabled)
- **Plugin System**: Loadable native handler libraries
- **Script Hook Architecture**: Event/packet hooks (the C# scripting host is **disabled** pending a rework — see Current Status)
- **Configuration Management**: Live reloading, hierarchical configs, and environment-specific settings

## 📋 Table of Contents

- [Quick Start](#-quick-start)
- [Architecture Overview](#-architecture-overview)
- [Installation](#-installation)
- [Configuration](#-configuration)
- [Usage](#-usage)
- [Development](#-development)
- [Documentation](#-documentation)
- [Contributing](#-contributing)
- [License](#-license)

## 🚀 Quick Start

### Prerequisites
- **C++17 Compatible Compiler** (GCC 8+, Clang 7+, MSVC 2019+)
- **CMake 3.15+**
- **OpenSSL 1.1.0+**
- **Steam SDK** (for authentication)
- **nlohmann/json** (for configuration)

### Build & Run
```bash
# Clone and build
git clone https://github.com/Krilliac/smellslikenapalm.git
cd smellslikenapalm
mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TELEMETRY=ON

# Build
cmake --build . --config Release --parallel

# Run with default configuration
./rs2v_server --config ../configs/server.ini --port 7777
```

### Docker Quick Start
```bash
# Build container
docker build -t rs2v-server .

# Run with persistent configs
docker run -d \
  --name rs2v-server \
  -p 7777:7777/udp \
  -p 9100:9100/tcp \
  -v $(pwd)/configs:/app/configs \
  -v $(pwd)/logs:/app/logs \
  rs2v-server
```

## 🏗 Architecture Overview

The RS2V server is built on a modular, event-driven architecture designed for scalability and maintainability:

```
┌─────────────────┬─────────────────┬─────────────────┐
│   Game Logic    │    Security     │   Telemetry     │
├─────────────────┼─────────────────┼─────────────────┤
│ • GameServer    │ • EAC Proxy     │ • Metrics       │
│ • PlayerManager │ • Anti-Cheat    │ • Prometheus    │
│ • TeamManager   │ • AuthManager   │ • File Reporter │
│ • MapManager    │ • BanManager    │ • Alerting      │
└─────────────────┴─────────────────┴─────────────────┘
┌─────────────────┬─────────────────┬─────────────────┐
│   Networking    │     Physics     │   Utilities     │
├─────────────────┼─────────────────┼─────────────────┤
│ • TCP/UDP Stack │ • Rigid Bodies  │ • ThreadPool    │
│ • Protocol      │ • Collisions    │ • MemoryPool    │
│ • Replication   │ • Vehicles      │ • Config Mgmt   │
│ • Compression   │ • Projectiles   │ • File I/O      │
└─────────────────┴─────────────────┴─────────────────┘
```

### Core Subsystems

| Subsystem | Description | Key Components |
|-----------|-------------|----------------|
| **Network** | High-performance networking with protocol handling | `NetworkManager`, `PacketSerializer`, `CompressionHandler` |
| **Game Logic** | Complete game state management and rules | `GameServer`, `GameMode`, `PlayerManager`, `TeamManager` |
| **Physics** | Real-time physics simulation and collision | `PhysicsEngine`, `CollisionDetection`, `VehicleManager` |
| **Security** | Multi-layer cheat prevention and authentication | `EACProxy`, `AuthManager`, `MovementValidator` |
| **Telemetry** | Production monitoring and analytics | `TelemetryManager`, `MetricsReporter`, `PrometheusReporter` |
| **Scripting** | Dynamic plugin and modification support | `HandlerLibraryManager`, `ScriptEngine` |

## 📦 Installation

### System Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| **CPU** | 2 cores, 2.5GHz | 4+ cores, 3.0GHz+ |
| **Memory** | 4GB RAM | 8GB+ RAM |
| **Storage** | 10GB free space | 50GB+ SSD |
| **Network** | 100Mbps | 1Gbps+ |
| **OS** | Windows 10, Ubuntu 18.04, CentOS 7 | Windows 11, Ubuntu 22.04, Rocky Linux 8 |

### Dependencies Installation

> **Note on testing:** the test suite uses the project's own native test
> framework (`tests/TestFramework.h` + `tests/TestMock.h`) and has **no external
> test dependency** — GoogleTest/GoogleMock are not required and are no longer
> fetched at configure time. The suite builds and runs fully offline.

#### Ubuntu/Debian
```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libjsoncpp-dev \
                    libsteam-api-dev zlib1g-dev
```

#### CentOS/RHEL
```bash
sudo yum groupinstall "Development Tools"
sudo yum install cmake3 openssl-devel jsoncpp-devel steam-sdk-devel \
                 zlib-devel
```

#### Windows (vcpkg)
```powershell
vcpkg install openssl nlohmann-json zlib
```

### Build Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TELEMETRY` | `ON` | Build with telemetry system |
| `ENABLE_SCRIPTING` | `OFF` | Enable the C# scripting host (currently does not build — see Current Status) |
| `ENABLE_COMPRESSION` | `ON` | Build with packet compression (zlib if available, otherwise a built-in stub) |
| `BUILD_TESTS` | `OFF` | Build the Google Test suite |

## ⚙️ Configuration

The server uses a hierarchical configuration system with live reloading support.

### Primary Configuration (`configs/server.ini`)
```ini
[Server]
Name=RS2V Production Server
Port=7777
MaxPlayers=64
TickRate=60
LogLevel=INFO
AdminPassword=your_secure_password

[Network]
MaxBandwidthMbps=100.0
CompressionEnabled=true
HeartbeatInterval=1000
PacketTimeout=5000

[Game]
GameMode=Conquest
FriendlyFire=false
RespawnTime=10
MaxScore=500
TimeLimit=1800

[Security]
EnableEAC=true
EACServerKey=your_eac_key
MaxLoginAttempts=3
BanDurationMinutes=30

[Telemetry]
Enabled=true
SamplingInterval=1000
PrometheusPort=9100
MetricsDirectory=logs/telemetry
```

### Configuration Files Structure
```
configs/
├── server.ini           # Main server configuration
├── network.ini          # Network-specific settings
├── security.ini         # Security and anti-cheat
├── game.ini            # Game rules and mechanics
├── maps.ini            # Map rotation and settings
├── telemetry.ini       # Monitoring configuration
└── environments/
    ├── development.ini  # Dev environment overrides
    ├── staging.ini      # Staging environment
    └── production.ini   # Production environment
```

## 🎯 Usage

### Basic Server Operations

#### Starting the Server
```bash
# Standard operation
./rs2v_server --config configs/server.ini --port 7777

# Development mode with verbose logging
./rs2v_server --config configs/development.ini --log-level DEBUG --no-eac

# Production mode with telemetry
./rs2v_server --config configs/production.ini --enable-telemetry --prometheus-port 9100
```

#### Command Line Options
| Option | Description | Example |
|--------|-------------|---------|
| `--config ` | Primary configuration file | `--config server.ini` |
| `--port ` | Override server port | `--port 7777` |
| `--log-level ` | Set logging level | `--log-level DEBUG` |
| `--no-eac` | Disable Easy Anti-Cheat | `--no-eac` |
| `--enable-telemetry` | Force enable telemetry | `--enable-telemetry` |
| `--prometheus-port ` | Prometheus metrics port | `--prometheus-port 9100` |

### Administrative Commands

#### In-Game Admin Commands
```
!kick  [reason]           # Kick a player
!ban   [reason] # Ban a player  
!unban                   # Unban a player
!changemap               # Change current map
!setnextmap              # Set next map in rotation
!balance                          # Force team balance
!restart                          # Restart current round
```

#### RCON Commands
```bash
# Connect via RCON
rcon_connect localhost:7778 your_rcon_password

# Server management
rcon_exec "status"                # Show server status
rcon_exec "players"               # List connected players
rcon_exec "maps"                  # Show available maps
rcon_exec "config reload"         # Reload configuration
rcon_exec "telemetry status"      # Show telemetry status
```

### Monitoring & Telemetry

#### Prometheus Metrics Endpoint
```bash
# View metrics in browser
http://localhost:9100/metrics

# Sample key metrics
curl -s http://localhost:9100/metrics | grep rs2v_server_active_connections
curl -s http://localhost:9100/metrics | grep rs2v_server_cpu_usage_percent
```

#### Log Files
```
logs/
├── server.log           # Main server logs
├── security.log         # Security events and violations
├── performance.log      # Performance metrics and warnings
├── telemetry/
│   ├── metrics_*.json   # Telemetry snapshots
│   └── alerts.log       # Threshold alerts
└── admin/
    ├── commands.log     # Admin command audit
    └── bans.log         # Ban/unban history
```

## 🔧 Development

### Building for Development

```bash
# Development build with all features
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DBUILD_TESTS=ON \
         -DBUILD_BENCHMARKS=ON \
         -DENABLE_ASAN=ON

# Run tests
make test

# Run specific test suite
./tests/NetworkTests
./tests/TelemetryTests
```

### Code Style & Standards

We follow modern C++ best practices:
- **C++17 Standard**: Modern language features and STL
- **RAII**: Resource management and exception safety
- **Thread Safety**: All public APIs are thread-safe unless documented
- **Performance**: Zero-copy where possible, minimal allocations in hot paths
- **Testing**: Comprehensive unit and integration test coverage

### Testing Framework

The project includes comprehensive test coverage:

```bash
# Run all tests
ctest --verbose

# Run specific test categories
ctest -R "Network.*" --verbose    # Network tests
ctest -R "Security.*" --verbose   # Security tests
ctest -R "Telemetry.*" --verbose  # Telemetry tests
```

## 📚 Documentation

### Available Documentation

> **Start here:** [Documentation Home](docs/Home.md) — Central index for all project documentation.

| Document | Description | Status |
|----------|-------------|--------|
| **[Documentation Home](docs/Home.md)** | Central wiki index and navigation hub | ✅ Complete |
| **[Configuration Reference](docs/CONFIGURATION.md)** | Complete reference for every config file and setting | ✅ Complete |
| **[Admin Commands](docs/ADMIN_COMMANDS.md)** | All 18 admin/RCON commands with examples | ✅ Complete |
| **[Game Modes Guide](docs/GAME_MODES.md)** | All 5 game modes with mechanics and strategy | ✅ Complete |
| **[Maps Guide](docs/MAPS.md)** | All 8 maps, rotation, custom maps, Workshop | ✅ Complete |
| **[FAQ](docs/FAQ.md)** | Frequently asked questions | ✅ Complete |
| **[API Reference](docs/API.md)** | Complete API reference | ✅ Complete |
| **[Architecture](docs/ARCHITECTURE.md)** | Detailed system architecture | ✅ Complete |
| **[Security](docs/SECURITY.md)** | Security architecture and best practices | ✅ Complete |
| **[Telemetry](docs/TELEMETRY.md)** | Telemetry system guide | ✅ Complete |
| **[Scripting](docs/SCRIPTING.md)** | Plugin development guide | ✅ Complete |
| **[Deployment](docs/DEPLOYMENT.md)** | Production deployment guide | ✅ Complete |
| **[Development](docs/DEVELOPMENT.md)** | Build, debug, test, contribute | ✅ Complete |
| **[Troubleshooting](docs/TROUBLESHOOTING.md)** | Common issues and solutions | ✅ Complete |
| **[Contributing](CONTRIBUTING.md)** | Contribution guidelines | ✅ Complete |
| **[Changelog](CHANGELOG.md)** | Version history and release notes | ✅ Complete |
| **[TODO](TODO.md)** | Development roadmap and task tracking | ✅ Complete |

### Online Resources

- **Project Wiki**: [GitHub Wiki](https://github.com/Krilliac/smellslikenapalm/wiki)
- **API Documentation**: [Generated Docs](https://krilliac.github.io/smellslikenapalm/)
- **Issue Tracker**: [GitHub Issues](https://github.com/Krilliac/smellslikenapalm/issues)
- **Discussions**: [GitHub Discussions](https://github.com/Krilliac/smellslikenapalm/discussions)

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guidelines](CONTRIBUTING.md) for details.

### Development Workflow

1. **Fork** the repository
2. **Create** a feature branch (`git checkout -b feature/amazing-feature`)
3. **Commit** your changes (`git commit -m 'Add amazing feature'`)
4. **Push** to the branch (`git push origin feature/amazing-feature`)
5. **Open** a Pull Request

### Reporting Issues

When reporting issues, please include:
- Server version and build configuration
- Operating system and hardware specs
- Steps to reproduce the issue
- Relevant log files and error messages
- Expected vs actual behavior

## 🏆 Performance & Benchmarks

### Performance Targets

> These are **design targets**, not measured benchmarks. End-to-end gameplay is not yet functional (see Current Status), so no representative live numbers are available.

| Metric | Target |
|--------|--------|
| **Player Capacity** | 64+ concurrent |
| **Tick Rate** | 60+ Hz |
| **Latency overhead** | low, sub-frame |
| **Memory** | bounded, pre-allocated pools |

## 📄 License

This project is licensed under the **RS2V Server Non-Commercial Open Source License** (modeled after emulator projects such as TrinityCore and MaNGOS) — see the [LICENSE](LICENSE) file for details. Commercial use and closed-source distribution are prohibited.

## 🙏 Acknowledgments

- **Tripwire Interactive** - For creating Rising Storm 2: Vietnam
- **Epic Games** - For the Unreal Engine 3 networking architecture inspiration
- **The Community** - For testing, feedback, and contributions

**Made with ❤️ for the Rising Storm 2: Vietnam community**

*For support, questions, or feature requests, please visit our [GitHub Discussions](https://github.com/Krilliac/smellslikenapalm/discussions) or join our [Discord Server](https://discord.gg/sd8HaMc8rh).*

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/Krilliac/smellslikenapalm)
