# RS2V Custom Server – **Comprehensive API Reference** (`API.md`)  
*Version 0.9.0-alpha · Last updated 2025-07-12*

> ⚠️ **Work-in-progress.**  
> Public symbols, file layout, and configuration keys may change until **v1.0.0** ships.  
> The main target **does not compile** yet on all platforms.

## Contents
1. Core Conventions  
2. Build & Compile Flags  
3. Error-Handling Contract  
4. Configuration Schemas  
5. Lifecycle API  
6. Utilities (Logging, Threading, Memory)  
7. Networking API  
8. Replication API  
9. Telemetry API  
10. Scripting & Plugin API (C# + Native)  
11. Security & Anti-Cheat API  
12. Physics API  
13. Gameplay API  
14. Testing & Diagnostics  
15. Glossary  

## 1 Core Conventions
| Topic | Convention |
|-------|------------|
| Header paths | `Server//…`, `telemetry/…` |
| Namespaces | Low-level → `Utils`, domain → subsystem (`Network`, `Physics`, `Telemetry`, …) |
| Resource ownership | `std::unique_ptr` for transfers; raw refs for views |
| Time | Always `std::chrono` (never raw integers) |
| Thread-safety notes | *Thread-safe*, *Thread-safe (internal lock)*, or *Not Thread-safe* stated per API |
| Return style | Logic errors throw; recoverable system errors -> `bool` + error-code |

## 2 Build & Compile Flags (CMake)

| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_TELEMETRY` | `ON` | Compiles TelemetryManager & Reporters |
| `ENABLE_SCRIPTING` | `ON` | Enables C# Roslyn scripting & native plugin loader |
| `ENABLE_EAC` | `ON` | Builds Easy Anti-Cheat proxy |
| `ENABLE_COMPRESSION` | `ON` | Links zlib / Brotli and activates `CompressionHandler` |
| `BUILD_TESTS` | `ON` | Builds 70 + GoogleTest suites |
| `BUILD_BENCHMARKS` | `OFF` | Google-Benchmark micro-benchmarks |

## 3 Error-Handling Contract
| Layer | Model | Typical Error Class |
|-------|-------|---------------------|
| Utilities | Throw `std::invalid_argument`, `std::system_error` | — |
| Subsystems | Return `bool`; on fatal invariant throw | — |
| API boundaries | Fail-fast: throw domain-specific (`Network::ProtocolError`) | — |
| Script host | Convert exceptions to `ScriptError` objects | — |

## 4 Configuration Schemas (INI + JSON)

### 4.1 `server.ini` (excerpt)

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

## 5 Lifecycle API

### 5.1 `Server::Bootstrap`

| Prototype | Thread-safe | Throws |
|-----------|-------------|--------|
| `bool Initialize(const std::string& cfgPath)` | ✅ | `std::runtime_error` (bad config) |
| `void Run()` | blocks | — |
| `void Shutdown()` | ✅ idempotent | — |
| `State GetState() const noexcept` | lock-free | — |

State diagram: **Uninit → Init → Running → ShuttingDown → Stopped**

## 6 Utilities

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

## 7 Networking API

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

## 8 Replication API

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

## 9 Telemetry API

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

## 10 Scripting & Plugin API (C# and Native)

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

## 11 Security & Anti-Cheat API

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

## 12 Physics API

| Class | Highlight |
|-------|-----------|
| `Physics::PhysicsEngine` | Rigid bodies, broad-phase sweep & prune, narrow-phase SAT |
| `CollisionDetection` | RayCast, SphereCast, LineOfSight |
| `VehicleManager` | 4-,6-,8-wheel suspension, engine torque curves |
| `ProjectileManager` | Physics-based trajectory, penetration, ricochet |

**Tick order:** physics → projectiles → game logic.

## 13 Gameplay API

| Class | Responsibility |
|-------|----------------|
| `Game::GameServer` | World root, main loop, tick manager |
| `Game::GameMode` | Match rules, score, win condition |
| `PlayerManager` | Lifecycle, inventory, stats |
| `TeamManager` | Balancing, score, auto-assign |
| `RoundManager` | Start / half-time / overtime / end |
| `VehicleManager` | Creation, control, replication |

All gameplay callbacks run on the **main** thread for determinism.

## 14 Testing & Diagnostics

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

## Glossary
| Term | Meaning |
|------|---------|
| **Snapshot** | Immutable metrics or replication state capture |
| **Bunch** | UE3 network payload containing one or more RPC fragments |
| **Tick** | Fixed 16.67 ms simulation slice (60 Hz) |
| **Reliable** | Packet delivered or re-sent until ACK |
| **Metric** | Quantitative telemetry datum |

### End of `API.md`  
For clarifications or updates, file an issue with the **documentation** label.
