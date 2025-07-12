*Version 0.9.0-alpha · Last updated 2025-07-12*  
*Stability: 🚧 Work-in-progress — APIs may change until v1.0.0*

This document describes the **C# scripting runtime**, **native plugin system**, **API bindings**, and **best practices** for extending the RS2V Custom Server.

## Table of Contents

1. [Overview](#1-overview)  
2. [C# Scripting (Roslyn)](#2-c-scripting-roslyn)  
   2.1 [ScriptHost Lifecycle](#21-scripthost-lifecycle)  
   2.2 [Loading & Hot-Reload](#22-loading--hot-reload)  
   2.3 [API Bindings (`RS2V.API`)](#23-api-bindings-rs2vapi)  
   2.4 [Hook Methods](#24-hook-methods)  
   2.5 [Security & Sandboxing](#25-security--sandboxing)  
   2.6 [Performance Tips](#26-performance-tips)  
3. [Native Plugins (C++)](#3-native-plugins-c)  
   3.1 [HandlerLibraryManager](#31-handlerlibrarymanager)  
   3.2 [Exported ABI](#32-exported-abi)  
   3.3 [Plugin Best Practices](#33-plugin-best-practices)  
4. [Folder Layout & References](#4-folder-layout--references)  
5. [Troubleshooting](#5-troubleshooting)  
6. [Roadmap & Further Reading](#6-roadmap--further-reading)  

## 1. Overview

- **Language**: C# 10 (.NET 7) via Roslyn scripting API  
- **Native**: C++ plugins loaded at runtime  
- **Isolation**: Each script runs in a collectible `AssemblyLoadContext`  
- **Use cases**: Game-logic hooks, admin commands, telemetry extensions, gameplay mods  

## 2. C# Scripting (Roslyn)

### 2.1 ScriptHost Lifecycle

**Header:** `Server/Scripting/ScriptHost.h`

```cpp
class ScriptHost {
public:
  bool Initialise(const std::filesystem::path& pluginDir,
                  const std::vector& refDirs={});
  void Shutdown();                     // Idempotent, unloads all contexts

  bool LoadScript(std::string_view name,
                  std::string_view source);    // Compile & cache
  bool ReloadScript(std::string_view name,
                    std::string_view source);  // Hot-reload
  bool UnloadScript(std::string_view name);

  template
  std::optional Invoke(std::string_view name,
                            std::string_view entry,
                            Args&&... args);
  void BroadcastEvent(std::string_view hook,
                      const VariantMap& payload);

  void SetExecutionTimeout(std::chrono::milliseconds ms);
  void SetMaxMemoryBytes(size_t bytes);

  std::vector GetLoadedScripts() const;
  std::optional  GetLastError() const;
};
```

- **Thread-Safety**: `Initialise`, `LoadScript`, `BroadcastEvent` are thread-safe.  
- **Timeout**: Default 500 ms; aborts long-running scripts.  
- **Memory**: Default 64 MiB per script; aborts on over-use.

### 2.2 Loading & Hot-Reload

1. **Startup**:  
   ```cpp
   host.Initialise("scripts/", {"plugins/refs/"});
   ```
2. **Load all `.csx`** in `scripts/`.  
3. **Hot-reload** on file change:  
   - Server watches `scripts/` via `FileWatcher`.  
   - Calls `ReloadScript(name, newSource)` atomically.

### 2.3 API Bindings (`RS2V.API`)

Scripts implicitly import `RS2V.API`. Key members:

| API                         | Signature                                                      | Notes                                       |
|-----------------------------|----------------------------------------------------------------|---------------------------------------------|
| `void BroadcastChat(string)`| Send global chat with server prefix                             | Thread-safe                                 |
| `Player? GetPlayer(uint id)`| Fetch read-only snapshot (id, name, team, pos, health, score)  | Returns `null` if not found                 |
| `void KickPlayer(uint id, string reason)`| Disconnect player                          | Records telemetry and security event        |
| `void SpawnProjectile(Vector3 pos, Vector3 dir, string archetype)`| Server-authoritative spawn | Adds to physics and replication systems     |
| `MetricHandle CreateGauge(string name)`| User-defined telemetry gauge            | Visible in telemetry dashboards             |
| `void Log(LogLevel level, string msg)`| Proxy to core Logger                         | Choose `DEBUG/INFO/WARN/ERROR/FATAL`        |
| `Task Delay(int ms)`        | Async delay without blocking server main loop                  | Requires `async` entry method               |

Scripts may reference extension assemblies in `plugins/refs/` via `#r "MyMod.dll"`.

### 2.4 Hook Methods

Scripts discover hooks by **method names**:

| Hook                | Signature                                           | When Fired                              |
|---------------------|-----------------------------------------------------|-----------------------------------------|
| `void OnServerStart()`       | —                                             | After `ScriptHost.Initialise`           |
| `void OnPlayerJoin(Player p)`| Player snapshot                                 | On successful authentication            |
| `void OnPlayerLeave(Player p)`| Player snapshot                                | On disconnect                           |
| `void OnTick(float dt)`      | Delta time                                     | Every server tick                       |
| `void OnChat(Player p, string msg)`| Chat event                               | On chat receive                         |
| `void OnReplication(Snapshot snap)`| Telemetry snapshot                        | After each telemetry sample             |

### 2.5 Security & Sandboxing

- **SandboxConfig**:  
  ```cpp
  struct SandboxConfig {
    bool allowFileIO=false;
    bool allowNetwork=false;
    std::vector allowedAssemblies;
    std::vector blockedNamespaces;
    std::chrono::milliseconds execTimeout{500};
    size_t maxMemoryBytes{64*1024*1024};
  };
  ```
- **Enforcement**:  
  - Injection of cancellation tokens  
  - Custom `AssemblyLoadContext` with `Collectible` flag  
  - Restrict P/Invoke and reflection permissions

### 2.6 Performance Tips

- **Minimize work in `OnTick`**: offload heavy logic via `Task.Run()`  
- **Profile with `Stopwatch`**: abort >5 ms methods  
- **Cache lookups**: avoid repeated calls to API  
- **Batch operations**: aggregate metrics/logs  

Example:
```csharp
using RS2V.API;
using System.Diagnostics;

void OnTick(float dt) {
  var sw = Stopwatch.StartNew();
  // ... logic ...
  sw.Stop();
  if (sw.ElapsedMilliseconds>5)
    Log(LogLevel.WARN, $"Slow OnTick: {sw.ElapsedMilliseconds}ms");
}
```

## 3. Native Plugins (C++)

### 3.1 HandlerLibraryManager

**Header:** `Server/Scripting/HandlerLibraryManager.h`

```cpp
class HandlerLibraryManager {
public:
  bool LoadLibrary(const std::filesystem::path& path);
  bool UnloadLibrary(const std::string& name);
  using HandlerFn = void(*)(const PacketAnalysis&);
  bool RegisterHandler(const std::string& tag, HandlerFn fn);
  HandlerFn GetHandler(const std::string& tag) const;
  std::vector GetLoadedLibraries() const;
};
```

- **Thread-Safety**: Library loading/unloading is serialized; handler lookup is lock-free.

### 3.2 Exported ABI

Plugins export:
```cpp
extern "C" bool RS2V_RegisterHandlers(HandlerLibraryManager&);
```
Implementation example:
```cpp
bool RS2V_RegisterHandlers(HandlerLibraryManager& mgr) {
  return mgr.RegisterHandler("CHAT_MESSAGE", &HandleChatMessage);
}
```

### 3.3 Plugin Best Practices

- **Isolate state**: use only local/static variables  
- **Error handling**: catch exceptions in handlers; avoid propagation  
- **Minimal dependencies**: rely on core APIs; include only needed headers  
- **Version checks**: verify host version via `SCRIPTING_API_VERSION` macro  

## 4. Folder Layout & References

```
/scripts/               # C# .csx script files
/plugins/refs/          # C# assemblies for scripts
/plugins/native/        # C++ plugin DLLs / .so files
Server/Scripting/       # ScriptHost & HandlerLibraryManager
```

- **.csx** scripts auto-compiled at startup  
- **.dll** reference assemblies must target .NET 7  
- **native** plugins must match host architecture and export the ABI function

## 5. Troubleshooting

- **Script compilation errors**: inspect `/opt/rs2v/logs/server.log` for Roslyn messages  
- **Hot-reload failures**: ensure file watcher has permissions; check `GetLastError()`  
- **Sandbox violations**: review `SecuritySandbox` logs in `security.log`  
- **Missing API types**: confirm scripts reference `using RS2V.API;` and `plugins/refs/RS2V.API.dll`  

Use the [Script Debug Info](scripts/collect_debug_info.sh) tool to gather environment and logs.

## 6. Roadmap & Further Reading

| Milestone       | Status        | ETA         |
|-----------------|---------------|-------------|
| M1 Roslyn Host  | ✅ Completed  | July 2025   |
| M2 Sandbox I/O  | 🔄 In Progress| Aug 2025    |
| M3 IL Weaving   | 🔲 Planned    | Q4 2025     |
| M4 NuGet Cache  | 🔲 Planned    | Q1 2026     |
| M5 Delta Reload | 🔲 Planned    | Q2 2026     |

Review **TODO.md** for detailed tasks on scripting enhancements.  
For community examples, see `/scripts/examples/`.

**End of SCRIPTING.md**