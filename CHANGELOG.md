# Changelog

All notable changes to the RS2V Custom Server project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added
- Comprehensive wiki-style documentation (Home, Configuration Reference, Admin Commands, Game Modes, Maps, FAQ)
- CONTRIBUTING.md with development workflow and coding standards
- Full CHANGELOG.md and CONTRIBUTORS.md

---

## [0.9.0-alpha] - 2025-06-01

Initial alpha release of the RS2V Custom Server.

### Added

#### Core Engine
- C++17 modular server architecture with clear separation of concerns
- CMake 3.15+ build system with configurable feature flags (`ENABLE_TELEMETRY`, `ENABLE_SCRIPTING`, `ENABLE_COMPRESSION`, `BUILD_TESTS`)
- Cross-platform support for Linux (GCC 8+, Clang 7+) and Windows (MSVC 2019+)
- Multi-threaded design with main thread, network I/O thread, telemetry thread, and configurable thread pool

#### Networking
- High-performance UDP networking stack with custom protocol implementation
- UE3-compatible "bunch" protocol for seamless game client integration
- Packet serialization with configurable compression (zlib when available)
- Reliable and unreliable transport modes
- Client connection management with heartbeat monitoring and idle timeout
- Rate limiting and DDoS protection
- Dual-stack IPv4/IPv6 support

#### Game Systems
- Complete game server lifecycle management (`GameServer`)
- Player management with connection tracking, team assignment, and statistics (`PlayerManager`)
- Team management with auto-balancing and configurable faction relationships (`TeamManager`)
- Round management with configurable game modes and win conditions (`RoundManager`)
- Map management with rotation, hot-reload, and voting system (`MapManager`)
- Five built-in game modes: Conquest, Elimination, Capture the Flag, Hot Zone, Domination
- Eight built-in maps: Carcassonne, Hill 400, Rubber Plant, Hacienda, Hill 937, Village, Skirmish Field, Coastal Assault
- Eight weapon definitions with full attribute configuration
- Configurable gameplay settings (friendly fire, respawn delay, timers, score limits, vehicles)

#### Physics
- Physics engine with rigid body dynamics and collision detection
- Broad-phase (sweep and prune) and narrow-phase (SAT) collision detection
- Vehicle physics simulation
- Projectile physics with trajectory calculation

#### Security
- Multi-layer anti-cheat system with EAC emulation (safe/emulate/off modes)
- Steam authentication integration with session ticket validation
- Custom fallback authentication token system
- Movement validation and behavioral analysis
- Ban management (temporary and permanent bans by SteamID)
- IP blacklisting with CIDR range support
- RCON (Remote Console) with password authentication
- Input validation and packet integrity checking
- HMAC-based packet integrity verification
- Rate limiting per client

#### Telemetry & Monitoring
- Telemetry subsystem with multiple reporter backends
- Prometheus metrics endpoint (HTTP)
- File-based metrics reporting (JSON)
- In-memory circular buffer for real-time metrics
- System metrics (CPU, memory, network utilization)
- Application metrics (player count, tick rate, packet loss)
- Alerting system with configurable thresholds
- Performance profiler with timing data (JSON/CSV output)

#### Scripting & Extensibility
- C# scripting engine powered by Roslyn compiler
- Hot-reload of C# scripts with file watcher and debouncing
- Sandboxed script execution environment
- 17 example scripts covering gameplay, anti-cheat, telemetry, and administration
- Native C++ handler library plugin system with dynamic loading
- Event bus for inter-component communication

#### Configuration
- Hierarchical configuration system (CLI > env vars > config files > defaults)
- INI-based configuration with section grouping
- Hot-reload for most configuration files via file watcher
- Production configuration overlay (`server_production.ini`)
- 16 configuration files covering server, game modes, maps, weapons, teams, gameplay, admin commands, bans, auth, MOTD, and more

#### Administration
- 18 administrative commands with permission levels (0–3)
- In-game chat command support
- RCON remote administration
- Admin list with SteamID-to-permission mapping
- Ban/unban/kick with audit logging
- Map rotation management (add/remove/list)
- Dynamic tick rate and time scale adjustment
- Bot spawning for testing
- Message of the Day (MOTD) with formatting codes and variable substitution

#### Documentation
- README with project overview, quick start, and architecture diagram
- API Reference (docs/API.md)
- Architecture overview (docs/ARCHITECTURE.md)
- Deployment guide (docs/DEPLOYMENT.md)
- Development guide (docs/DEVELOPMENT.md)
- Scripting guide (docs/SCRIPTING.md)
- Security guide (docs/SECURITY.md)
- Telemetry guide (docs/TELEMETRY.md)
- Troubleshooting guide (docs/TROUBLESHOOTING.md)
- TODO.md development roadmap
- GitHub issue templates (bug report, feature request)
- Codacy CI workflow

### Known Issues
- The main target does not compile on all platforms (see README warning)
- Some subsystems are still stubs or proofs-of-concept
- The public API and file layout are not yet frozen and may change
- `loadouts.ini` is a placeholder (not yet implemented)

---

[Unreleased]: https://github.com/Krilliac/smellslikenapalm/compare/v0.9.0-alpha...HEAD
[0.9.0-alpha]: https://github.com/Krilliac/smellslikenapalm/releases/tag/v0.9.0-alpha
