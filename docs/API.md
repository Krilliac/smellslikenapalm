# RS2V Custom Server – **Complete API Reference** (`API.md`)  
*Version 0.9.0-alpha · Last updated 2025-07-12*

> ⚠️ **HEADS-UP:** This repository is under **active development**.  
> -  The public API, file layout, and naming conventions are **not yet frozen** and may change without notice.  
> -  Some subsystems are still stubs or proofs-of-concept.  
> -  Comprehensive tests exist, but the **main target** currently **does not compile** on all platforms.  
> -  Expect breaking commits, temporary build failures, and force-pushes while we stabilise the architecture.  
> -  Use the `develop` branch at your own risk; the `main` branch is rebased regularly.  
> We welcome issues and pull-requests, but please sync often and review [TODO.md](TODO.md) before starting major work.  
> For a stable, feature-complete server build, follow releases tagged `v1.x` (scheduled after the CI pipeline turns 🌕 green).

## Contents
1. [Core Conventions](#1-core-conventions)  
2. [Build & Compile Flags](#2-build--compile-flags-cmake)  
3. [Error-Handling Contract](#3-error-handling-contract)  
4. [Configuration System](#4-configuration-system)  
5. [Lifecycle API](#5-lifecycle-api)  
6. [Utilities](#6-utilities)  
7. [Networking API](#7-networking-api)  
8. [Replication API](#8-replication-api)  
9. [Telemetry API](#9-telemetry-api)  
10. [Scripting & Plugin API](#10-scripting--plugin-api-c--native)  
11. [Security & Anti-Cheat API](#11-security--anti-cheat-api)  
12. [Physics API](#12-physics-api)  
13. [Gameplay API](#13-gameplay-api)  
14. [Testing & Diagnostics](#14-testing--diagnostics)  
15. [Glossary](#15-glossary)  

## 1. Core Conventions
| Topic | Convention |
|-------|------------|
| Header paths | `Server//…`, `telemetry/…` |
| Namespaces | Low-level → `Utils`, domain → subsystem (`Network`, `Physics`, `Telemetry`, …) |
| Resource ownership | `std::unique_ptr` for transfers; raw refs for views |
| Time | Always `std::chrono` (never raw integers) |
| Thread-safety notes | *Thread-safe*, *Thread-safe (internal lock)*, or *Not Thread-safe* stated per API |
| Return style | Logic errors throw; recoverable system errors -> `bool` + error-code |

## 2. Build & Compile Flags (CMake)

| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_TELEMETRY` | `ON` | Compiles TelemetryManager & Reporters |
| `ENABLE_SCRIPTING` | `ON` | Enables C# Roslyn scripting & native plugin loader |
| `ENABLE_EAC` | `ON` | Builds Easy Anti-Cheat proxy |
| `ENABLE_COMPRESSION` | `ON` | Links zlib / Brotli and activates `CompressionHandler` |
| `BUILD_TESTS` | `ON` | Builds 70+ GoogleTest suites |
| `BUILD_BENCHMARKS` | `OFF` | Google-Benchmark micro-benchmarks |

## 3. Error-Handling Contract
| Layer | Model | Typical Error Class |
|-------|-------|---------------------|
| Utilities | Throw `std::invalid_argument`, `std::system_error` | — |
| Subsystems | Return `bool`; on fatal invariant throw | — |
| API boundaries | Fail-fast: throw domain-specific (`Network::ProtocolError`) | — |
| Script host | Convert exceptions to `ScriptError` objects | — |

## 4. Configuration System

### 4.1 `ConfigManager`

**Header:** `Server/Config/ConfigManager.h`

Manages loading, saving, and querying the unified `config/server.ini`.

#### 4.1.1 Constructors & Lifecycle

| Method | Thread Safety | Description |
|--------|---------------|-------------|
| `ConfigManager()` | Thread-Safe | Default constructor |
| `~ConfigManager()` | Thread-Safe | Destructor |

#### 4.1.2 Initialization

| Method | Returns | Description |
|--------|---------|-------------|
| `bool Initialize()` | `bool` | Ensures `config/` exists, loads `config/server.ini`, sets up watchers |

#### 4.1.3 Loading & Saving

| Method | Returns | Description |
|--------|---------|-------------|
| `bool ReloadConfiguration()` | `bool` | Reloads from `server.ini`, notifies listeners |
| `bool ImportConfiguration(const std::string& sourceFile)` | `bool` | Merges another INI into current settings |
| `bool ExportConfiguration(const std::string& targetFile)` | `bool` | Writes current settings to a new file |
| `void ResetToDefaults()` | `void` | Clears and repopulates with built-in defaults |
| `bool SaveAllConfigurations()` | `bool` | Writes primary config file |
| `bool SaveConfiguration(const std::string& configFile)` | `bool` | Writes one file |

#### 4.1.4 Accessors & Mutators

| Method | Returns | Description |
|--------|---------|-------------|
| `std::string GetString(const std::string& key, const std::string& defaultValue = "")` | `string` | Get string value |
| `int GetInt(const std::string& key, int defaultValue = 0)` | `int` | Get integer value |
| `bool GetBool(const std::string& key, bool defaultValue = false)` | `bool` | Get boolean value |
| `float GetFloat(const std::string& key, float defaultValue = 0.0f)` | `float` | Get float value |
| `void SetString(const std::string& key, const std::string& value)` | `void` | Set string value |
| `void SetInt(const std::string& key, int value)` | `void` | Set integer value |
| `void SetBool(const std::string& key, bool value)` | `void` | Set boolean value |
| `void SetFloat(const std::string& key, float value)` | `void` | Set float value |

#### 4.1.5 Structure Queries

| Method | Returns | Description |
|--------|---------|-------------|
| `bool HasKey(const std::string& key)` | `bool` | Check if key exists |
| `void RemoveKey(const std::string& key)` | `void` | Remove key |
| `std::vector GetSectionKeys(const std::string& section)` | `vector` | Get all keys in section |
| `std::vector GetAllSections()` | `vector` | Get all section names |

### 4.2 `ServerConfig`

**Header:** `Server/Config/ServerConfig.h`

Typed getters for every key in `config/server.ini`.

#### 4.2.1 Constructor

| Method | Description |
|--------|-------------|
| `ServerConfig(const std::shared_ptr& mgr)` | Initialize with ConfigManager reference |

#### 4.2.2 General Section

| Method | Returns | Default | Description |
|--------|---------|---------|-------------|
| `std::string GetServerName()` | `string` | `"RS2V Server"` | Server display name |
| `int GetMaxPlayers()` | `int` | `64` | Maximum concurrent players |
| `std::string GetMapRotationFile()` | `string` | `"maps.ini"` | Map rotation config file |
| `std::string GetGameModesFile()` | `string` | `"gamemodes.ini"` | Game modes config file |
| `std::string GetMotdFile()` | `string` | `"motd.txt"` | Message of the day file |
| `int GetTickRate()` | `int` | `60` | Server tick rate (Hz) |
| `int GetTimeSyncInterval()` | `int` | `1000` | Time sync interval (ms) |
| `bool IsAnnouncementsEnabled()` | `bool` | `true` | Enable announcements |
| `std::string GetDataDirectory()` | `string` | `"data/"` | Data directory path |
| `std::string GetLogDirectory()` | `string` | `"logs/"` | Log directory path |
| `bool IsAdminRconOnly()` | `bool` | `false` | Admin access RCON only |

#### 4.2.3 Network Section

| Method | Returns | Default | Description |
|--------|---------|---------|-------------|
| `int GetPort()` | `int` | `7777` | Server port |
| `std::string GetBindAddress()` | `string` | `"0.0.0.0"` | Bind address |
| `int GetMaxPacketSize()` | `int` | `1500` | Maximum packet size |
| `int GetClientIdleTimeout()` | `int` | `60` | Client idle timeout (seconds) |
| `int GetHeartbeatInterval()` | `int` | `30` | Heartbeat interval (seconds) |
| `bool IsDualStack()` | `bool` | `true` | IPv4/IPv6 dual stack |
| `bool IsReliableTransport()` | `bool` | `true` | Use reliable transport |

#### 4.2.4 Security Section

| Method | Returns | Default | Description |
|--------|---------|---------|-------------|
| `bool IsSteamAuthEnabled()` | `bool` | `true` | Enable Steam authentication |
| `bool IsFallbackCustomAuth()` | `bool` | `false` | Use custom auth fallback |
| `std::string GetCustomAuthTokensFile()` | `string` | `"tokens.txt"` | Custom auth tokens file |
| `bool IsBanManagerEnabled()` | `bool` | `true` | Enable ban manager |
| `std::string GetBanListFile()` | `string` | `"banlist.txt"` | Ban list file |
| `bool IsAntiCheatEnabled()` | `bool` | `true` | Enable anti-cheat |
| `std::string GetAntiCheatMode()` | `string` | `"EAC"` | Anti-cheat mode |
| `std::string GetEacScannerConfigFile()` | `string` | `"eac.ini"` | EAC scanner config |

### 4.3 Configuration Schema (INI + JSON)

#### 4.3.1 `server.ini` (excerpt)

| Section | Key | Type | Default | Description |
|---------|-----|------|---------|-------------|
| `[Server]` | `Name` | string | `"RS2V Server"` | Listed in browser |
| 〃 | `Port` | uint16 | `7777` | UDP listen |
| 〃 | `MaxPlayers` | uint | `64` | Hard cap |
| `[Network]` | `MaxBandwidthMbps` | float | `100.0` | Throttle |
| 〃 | `CompressionEnabled` | bool | `true` | zlib/Brotli |
| `[Telemetry]` | `Enabled` | bool | `true` | Master switch |
| 〃 | `PrometheusPort` | int | `9100` | `/metrics` HTTP |

*Complete key tables are in `docs/SCHEMAS.md`.*

## 5. Lifecycle API

### 5.1 `Server::Bootstrap`

| Prototype | Thread-safe | Throws |
|-----------|-------------|--------|
| `bool Initialize(const std::string& cfgPath)` | ✅ | `std::runtime_error` (bad config) |
| `void Run()` | blocks | — |
| `void Shutdown()` | ✅ idempotent | — |
| `State GetState() const noexcept` | lock-free | — |

State diagram: **Uninit → Init → Running → ShuttingDown → Stopped**

## 6. Utilities

### 6.1 `Utils::Logger`
| Level | Macro | Example |
|-------|-------|---------|
| `DEBUG` | `Logger::Debug` | `Logger::Debug("x=%.2f", x);` |
| `INFO`  | `Logger::Info`  | — |
| `WARN`  | `Logger::Warn`  | — |
| `ERROR` | `Logger::Error` | — |
| `FATAL` | `Logger::Fatal` | abort + dump |

Features — rotating files, colour console, JSON mode (`LogFormat=JSON`).

### 6.2 `Utils::ThreadPool`

```cpp
ThreadPool pool(std::thread::hardware_concurrency());
auto fut = pool.Enqueue([] { return 42; });
```

### 6.3 `Utils::MemoryPool`

Fixed-size block allocator (64-B → 4 KiB slabs), O(1) allocate/free, lock-free per-thread caches.

## 7. Networking API

### 7.1 `Network::NetworkManager`

```cpp
class NetworkManager final {
public:
    bool Initialize(const NetworkConfig&);      // ✅
    void Shutdown();                            // ✅
    bool SendPacket(uint32_t id, const Packet&);// lock-free
    bool Broadcast(const Packet&, bool rel);    // lock-free
    bool Tick(std::chrono::milliseconds dt);    // main thread
    void SetPacketHandler(PacketCallback);      // once
};
```

`PacketCallback` is invoked on main thread.

### 7.2 `Network::Packet`

```cpp
struct Packet {
    PacketType type;
    uint32_t   clientId;      // 0 = server
    std::span payload;
    bool compressed : 1;
    bool reliable   : 1;

    template const T& As() const;      // unchecked cast
    size_t SizeBytes() const noexcept;
};
```

### 7.3 `Network::PacketSerializer`

| Function | Complexity | Note |
|----------|------------|------|
| `Serialize(const Packet&)` | O(n) | CRC-32, optional compression |
| `Deserialize(span)` | O(1) | Validates header |

## 8. Replication API

### 8.1 `Replication::ReplicationManager`

```cpp
bool  RegisterActor(uint32_t);
bool  UnregisterActor(uint32_t);
template
bool  SetProperty(uint32_t id, std::string_view name, const T& value); // any trivially-copyable
Snapshot CreateSnapshot(bool full = false) const;  // ✅
bool      ApplySnapshot(const Snapshot&);          // client-side
```

`Snapshot.sequence` is monotonic; old snapshots are ignored.

## 9. Telemetry API

### 9.1 `Telemetry::TelemetryManager`

| Method | Effect |
|--------|--------|
| `Initialize(TelemetryConfig)` | mkdir, init reporters |
| `AddReporter(unique_ptr)` | attach |
| `StartSampling()` | bg thread, default 1 Hz |
| `ForceSample()` | sync snapshot |
| `Shutdown()` | stop thread, flush |

### 9.2 Quick hooks

```cpp
TELEMETRY_INCREMENT_PACKETS_PROCESSED();
TELEMETRY_UPDATE_LATENCY(avgMs);
```

### 9.3 Reporters

| Class | Output | Key File |
|-------|--------|----------|
| **FileMetricsReporter** | rotating JSON | `telemetry/FileMetricsReporter.cpp` |
| **PrometheusMetricsReporter** | `/metrics` HTTP | `telemetry/PrometheusReporter.cpp` |
| **MemoryMetricsReporter** | ring buffer | `MetricsReporter.h` |
| **CSVMetricsReporter** | CSV rows | 〃 |
| **AlertMetricsReporter** | threshold → webhook | 〃 |

## 10. Scripting & Plugin API (C# and Native)

### 10.1 C# Runtime – `Scripting::ScriptHost`

| Prototype | Thread-safe | Notes |
|-----------|-------------|-------|
| `Initialise(pluginDir, refs)` | ✅ | Loads .NET 7 runtime |
| `LoadScript(name, src)` | ✅ | Roslyn compile |
| `Invoke(script, method, args…)` | guarded | per-script lock |
| `BroadcastEvent(hook, payload)` | ✅ | async fan-out |
| `SetExecutionTimeout(ms)` | ✅ | cooperative cancellation |

**Hooks recognised**

| Method | Fired |
|--------|-------|
| `OnServerStart()` | after bootstrap |
| `OnTick(float dt)` | every tick |
| `OnPlayerJoin(Player)` | auth OK |
| `OnChat(Player, string)` | chat msg |

Execution limits — 500 ms CPU, 64 MiB heap (configurable).

### 10.2 Native Plugins – `HandlerLibraryManager`

```cpp
extern "C" bool RS2V_RegisterHandlers(HandlerLibraryManager&);
```

A plugin exports packet handlers:

```cpp
void Handle_CHAT_MESSAGE(const PacketAnalysisResult& r);
```

Load at runtime via `LoadLibrary("mods/myplugin.dll")`.

## 11. Security & Anti-Cheat API

| Subsystem | Key Class | Purpose |
|-----------|-----------|---------|
| **Auth** | `AuthManager` | Steam ticket validation, lockout |
| **EAC** | `EACProxy` | Talks to local Easy Anti-Cheat daemon |
| **Ban** | `BanManager` | IP / SteamID / HWID bans |
| **Movement** | `MovementValidator` | Speed, acceleration, teleport check |
| **Network** | `NetworkBlocker` | Flood, malformed packets |

### 11.1 Example – speed-hack detection

```cpp
void MovementValidator::Validate(uint32_t cid, const MoveCmd& cmd)
{
    if (cmd.speed > kMax) {
        TELEMETRY_INCREMENT_SPEED_HACK();
        Server::KickClient(cid, "Speed hack");
        TELEMETRY_INCREMENT_KICK();
    }
}
```

## 12. Physics API

| Class | Highlight |
|-------|-----------|
| `Physics::PhysicsEngine` | Rigid bodies, broad-phase sweep & prune, narrow-phase SAT |
| `CollisionDetection` | RayCast, SphereCast, LineOfSight |
| `VehicleManager` | 4-,6-,8-wheel suspension, engine torque curves |
| `ProjectileManager` | Physics-based trajectory, penetration, ricochet |

**Tick order:** physics → projectiles → game logic.

## 13. Gameplay API

| Class | Responsibility |
|-------|----------------|
| `Game::GameServer` | World root, main loop, tick manager |
| `Game::GameMode` | Match rules, score, win condition |
| `PlayerManager` | Lifecycle, inventory, stats |
| `TeamManager` | Balancing, score, auto-assign |
| `RoundManager` | Start / half-time / overtime / end |
| `VehicleManager` | Creation, control, replication |

All gameplay callbacks run on the **main** thread for determinism.

## 14. Testing & Diagnostics

| Suite | Scope | File |
|-------|-------|------|
| **NetworkTests** | serializer, protocol framing | `tests/NetworkTests.cpp` |
| **TelemetryTests** | snapshot accuracy, reporter health | `tests/TelemetryTests.cpp` |
| **SecurityTests** | auth, ban, cheat detection | `tests/SecurityTests.cpp` |
| **PhysicsTests** | collisions, dynamics | `tests/PhysicsTests.cpp` |
| **IntegrationTests** | boot → shutdown flow | `tests/IntegrationTests.cpp` |

Run all:  
```bash
ctest --output-on-failure
```

## 15. Glossary
| Term | Meaning |
|------|---------|
| **Snapshot** | Immutable metrics or replication state capture |
| **Bunch** | UE3 network payload containing one or more RPC fragments |
| **Tick** | Fixed 16.67 ms simulation slice (60 Hz) |
| **Reliable** | Packet delivered or re-sent until ACK |
| **Metric** | Quantitative telemetry datum |

### End of `API.md`  
For clarifications or updates, file an issue with the **documentation** label.
