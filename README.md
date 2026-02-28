# RS2V Custom Server

<span style="color:#d9534f; font-weight:bold;">⚠️ HEADS-UP: This repository is under active development.</span>
<span style="color:#f0ad4e;">- The public API, file layout, and naming conventions are not yet frozen and may change without notice.</span>

<span style="color:#f0ad4e;">- Some subsystems are still stubs or proofs-of-concept.</span>

<span style="color:#5bc0de;">- Comprehensive tests exist, but the main target currently does not compile on all platforms.</span>

<span style="color:#428bca;">We welcome issues and pull-requests, but please sync often and review <a href="TODO.md">TODO.md</a> before starting major work.</span>

A highly modular, extensible, and production-ready dedicated server implementation for Rising Storm 2: Vietnam, designed with enterprise-grade architecture, comprehensive telemetry, and advanced security features.

## 🌟 Key Features

### 🌐 Advanced Networking
- **High-Performance Protocol Stack**: Custom TCP/UDP implementations with zero-copy optimizations
- **UE3-Compatible Protocol**: Full "bunch" protocol support for seamless game integration  
- **Adaptive Network Quality**: Automatic bandwidth management, congestion control, and quality monitoring
- **Compression & Encryption**: Configurable packet compression and security layers

### 🎮 Complete Game Systems
- **Physics Engine**: Full rigid-body dynamics, collision detection, and vehicle simulation
- **Player Management**: Comprehensive player lifecycle, team balancing, and statistics tracking
- **Match Management**: Round systems, objective tracking, and configurable game modes
- **Map Systems**: Dynamic map loading, rotation management, and spawn point optimization

### 🔒 Enterprise Security
- **Multi-Layer Anti-Cheat**: EAC integration, behavioral analysis, and statistical anomaly detection
- **Authentication Framework**: Steam integration, secure session management, and role-based access
- **Input Validation**: Comprehensive packet validation, movement verification, and exploit prevention
- **Audit & Compliance**: Complete action logging, security event tracking, and forensic capabilities

### 📊 Production Telemetry
- **Real-Time Monitoring**: System metrics (CPU, memory, network) and application metrics
- **Multiple Export Formats**: JSON files, Prometheus endpoints, CSV exports, and in-memory buffers
- **Performance Profiling**: Automatic timing, bottleneck detection, and optimization recommendations
- **Alerting System**: Threshold-based monitoring with configurable notifications

### 🔧 Extensibility & Scripting
- **Dynamic Plugin System**: Hot-loadable libraries with secure sandboxing
- **Script Hook Architecture**: Runtime packet handling, game logic modification, and event processing
- **API Framework**: Comprehensive C API for third-party integrations
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
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TELEMETRY=ON -DENABLE_EAC=ON

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

#### Ubuntu/Debian
```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libjsoncpp-dev \
                    libsteam-api-dev libgtest-dev zlib1g-dev
```

#### CentOS/RHEL
```bash
sudo yum groupinstall "Development Tools"
sudo yum install cmake3 openssl-devel jsoncpp-devel steam-sdk-devel \
                 gtest-devel zlib-devel
```

#### Windows (vcpkg)
```powershell
vcpkg install openssl nlohmann-json gtest zlib
```

### Build Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TELEMETRY` | `ON` | Build with telemetry system |
| `ENABLE_EAC` | `ON` | Enable Easy Anti-Cheat support |
| `ENABLE_SCRIPTING` | `ON` | Enable dynamic scripting system |
| `ENABLE_COMPRESSION` | `ON` | Build with packet compression |
| `BUILD_TESTS` | `ON` | Build comprehensive test suite |
| `BUILD_BENCHMARKS` | `OFF` | Build performance benchmarks |

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

| Metric | Target | Typical |
|--------|--------|---------|
| **Player Capacity** | 64+ concurrent | 32-64 players |
| **Tick Rate** | 60+ Hz | 60 Hz |
| **Latency** | 80% |
| `rs2v_server_memory_usage_percent` | Memory utilization | >90% |
| `rs2v_server_active_connections` | Player count | 60 |
| `rs2v_server_packet_loss_rate` | Network quality | >5% |
| `rs2v_server_security_violations_total` | Security events | >0/hour |

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- **Tripwire Interactive** - For creating Rising Storm 2: Vietnam
- **Epic Games** - For the Unreal Engine 3 networking architecture inspiration
- **The Community** - For testing, feedback, and contributions

**Made with ❤️ for the Rising Storm 2: Vietnam community**

*For support, questions, or feature requests, please visit our [GitHub Discussions](https://github.com/Krilliac/smellslikenapalm/discussions) or join our [Discord Server](https://discord.gg/sd8HaMc8rh).*

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/Krilliac/smellslikenapalm)
