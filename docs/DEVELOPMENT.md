# DEVELOPMENT.md — Contributor Guide

Welcome to the RS2V Custom Server development guide.  
This document explains **how to build, debug, test, and contribute** to the project.  
If you are looking for runtime configuration or API details, see **API.md** and **DEPLOYMENT.md** instead.

## 1 · Project Overview

| Layer | Key Directories | Primary Languages |
|-------|-----------------|-------------------|
| Core engine | `src/` | C++23 |
| Telemetry | `telemetry/` | C++23 |
| Scripting  | `src/Scripting/` | C# (.NET) & C++ — **disabled** (does not build) |
| Tests      | `tests/`   | C++23 (native framework, no GoogleTest) |

The repository uses **CMake** as its single build system and **GitHub Actions** as CI.

## 2 · Local Build Workflow

### 2.1 Prerequisites

| Tool | Minimum Version | Install Hint |
|------|-----------------|--------------|
| **CMake** | 3.20 | `brew install cmake` / `apt install cmake` |
| **Compiler (C++23)** | GCC 12 / Clang 15 / MSVC VS 2022 | Set `CXX=clang++` if you have multiple |
| **Conan** (optional) | 2.0 | Dependency cache |
| **.NET SDK** | 7.0 | Required for C# scripting |
| **Python** | 3.9 | Build scripts |

### 2.2 Quick Build

```bash
git clone https://github.com/Krilliac/smellslikenapalm.git
cd smellslikenapalm
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TELEMETRY=ON
cmake --build . --parallel
```

The binary is `build/bin/rs2v_server`.

### 2.3 Common CMake Options

| Flag | Default | Description |
|------|---------|-------------|
| `-DENABLE_TELEMETRY` | `ON`  | Build telemetry subsystem |
| `-DENABLE_SCRIPTING` | `OFF` | Embed the C# scripting host. **Currently does not build** (deprecated .NET COM host) — disabled pending a rework. |
| `-DENABLE_COMPRESSION` | `ON` | Build packet compression (zlib if available, otherwise a built-in stub) |
| `-DBUILD_TESTS`      | `OFF` | Compile the native test suite (no GoogleTest dependency) |

Add `-G "Visual Studio 17 2022"` on Windows.

## 3 · Coding Standards

The full, enforceable standard is [`CODING_STANDARDS.md`](CODING_STANDARDS.md) —
read it before writing code. The table below is a quick summary; where the two
differ, `CODING_STANDARDS.md` wins.

| Rule | Short Form |
|------|------------|
| **Language level** | C++23 (`-std=c++23`) |
| **Naming** | `PascalCase` classes/methods, `camelCase` locals, `m_` prefix members, `UPPER_SNAKE` macros/constants |
| **Headers** | `.h` for interfaces, `.inl` for header-only impl |
| **Namespaces** | Top-level subsystem: `Network`, `Telemetry`, `Physics`, etc. |
| **Includes** | Use forward declarations where possible, include order: STL → third-party → project |
| **Pointers** | `std::unique_ptr` for ownership, raw pointer for non-owning, `std::shared_ptr` only when necessary |
| **Exceptions** | Throw only for programmer errors; recoverable errors return `bool` + `std::error_code` |
| **Thread Safety** | Annotate functions with `// Thread-Safe` or `// Not Thread-Safe` in headers |
| **Formatting** | `clang-format` with profile `utils/.clang-format` (run `make format`) |

> **Tip:** Enable format-on-save to keep diffs clean.

## 4 · Branch & Commit Strategy

1. `main` is **always releasable** (may not compile until v1.0 but must pass CI).  
2. `develop` is integration; rebased/force-pushed after every sprint.  
3. **Feature branches**:  
   ```
   git checkout -b feat/-
   ```
4. **Commit style**

```
: : 

Body: optional 80-char lines explaining *why*, not *what*.
```

| Type | Purpose |
|------|---------|
| `feat` | New feature |
| `fix`  | Bug fix |
| `docs` | Documentation |
| `test` | Tests |
| `refactor` | Internal change |
| `perf` | Performance |
| `build` | Build system |

Squash before merging; keep history linear.

## 5 · Running & Debugging

### 5.1 Run From IDE

| IDE | Configuration |
|-----|---------------|
| **VS Code** | Use `launch.json` in repo root. Includes ASan, gdb-pretty-printers, and hot-reload for C# scripts. |
| **CLion** | Import CMake project; enable `Run | Attach Debugger to Process` for hot attach. |
| **Visual Studio** | Open generated `RS2V.sln`; set `rs2v_server` as startup; copy `config/` to `$(TargetDir)` in post-build. |

### 5.2 Typical Flags

```
./rs2v_server --config configs/development.ini --log-level DEBUG --enable-telemetry
```

* Use `--break-on-error` to trigger a debug breakpoint on the first `Logger::Error`.  
* Pass `--script-trace` to dump every script invocation.

### 5.3 Debugging Tips

| Subsystem | Technique |
|-----------|-----------|
| **Network** | `scripts/netdump.py` converts pcap to JSON; Wireshark filter `udp.port==7777` |
| **Physics** | `PhysicsEngine::DebugDraw()` renders to ImGui overlay (toggle with `F3`) |
| **Telemetry** | Hit `http://localhost:9100/metrics` – look for anomalies |

## 6 · Testing

### 6.1 Unit Tests

```
ctest --output-on-failure
```

You can run individual suites:

```
ctest -R "Network.*"
```

### 6.2 Integration Tests

`tests/IntegrationTests.cpp` boots a headless server for 30 s with 8 bot clients.  
Heavy; tagged `long`. Run with:

```
ctest -L long
```

### 6.3 Performance Benchmarks

```
cmake .. -DBUILD_BENCHMARKS=ON
./benchmarks/network_benchmark
```

> Benchmarks are **opt-in** and compile in `Release` only.

## 7 · C# Script Development

1. Place `.csx` files under `plugins/`.  
2. At startup, the `ScriptHost` automatically compiles every script in that folder.  
3. Hot-reload: touch the file; the server detects change and recompiles on next tick.  
4. Reference external assemblies by dropping `.dll` into `plugins/refs/`.

Example:

```csharp
using RS2V.API;
void OnChat(Player p, string msg)
{
    if (msg.StartsWith("!hp"))
        Chat.SendPrivate(p.Id, $"HP: {p.Health}");
}
```

## 8 · Continuous Integration

* **GitHub Actions**  
  * Linux (gcc 12 / clang 15) debug + release  
  * Windows (MSVC v143) release  
  * macOS (Apple Clang 14) release  
* Steps  
  1. Cache Conan/NuGet  
  2. Configure CMake  
  3. Build  
  4. Run unit tests  
  5. Upload artifacts  
  6. Post status to Discord #ci-build-status via webhook  

A failing status must be **green** before merge – no exceptions.

## 9 · Static Analysis & Linters

| Tool | Invocation | CI Gate |
|------|------------|---------|
| **clang-tidy** | `make tidy` | Warnings = PR block |
| **cppcheck** | `make cppcheck` | Informational |
| **clang-format** | `make format` | Auto-fix on PR via action |
| **include-what-you-use** | `make iwyu` | Advisory |

## 10 · Contributing Workflow

1. **Open an issue** first for large changes. Label `proposal`.  
2. Fork → feature branch → commit.  
3. Ensure:  
   * All tests pass (`ctest`)  
   * `clang-format` passes (`make check-format`)  
   * No new `clang-tidy` warnings  
4. Push & open PR against `develop`.  
5. Fill in the PR template: description, screenshots, checklist, linked issue.  
6. One maintainer review + passing CI → squash & merge.

## 11 · Useful Make Targets

| Target | Action |
|--------|--------|
| `make format` | Run clang-format on code |
| `make tidy`   | Run clang-tidy static analysis |
| `make fuzz`   | Build with libFuzzer & sanitizers |
| `make docs`   | Generate Doxygen under `build/docs/html` |
| `make clean`  | Remove `build/` directory |

## 12 · FAQ

| Question | Answer |
|----------|--------|
| **The server exits with “Cannot locate Steam SDK”.** | Ensure `STEAM_SDK_PATH` env variable points to the Steamworks SDK root or install `steam-sdk` package. |
| **`undefined reference to _imp_WSAStartup` on Windows build** | Add `-lws2_32` to `target_link_libraries` in your local CMakeLists.txt. |
| **ASan complains about `alloc-dealloc-mismatch`.** | Re-run with `-DENABLE_ASAN=OFF`; some third-party libs use `malloc`+`delete`. |
| **Prometheus endpoint returns 404.** | Set `[Telemetry] Enabled=true` and ensure `PrometheusPort` is not blocked by firewall. |

## 13 · Roadmap Snapshot

| Milestone | ETA | Highlight |
|-----------|-----|-----------|
| **0.9.0** | Sep 2025 | Beta, compile on Linux & Windows |
| **0.9.5** | Dec 2025 | Feature-freeze, scripting sandbox hardened |
| **1.0.0** | Q1 2026 | CI green, binary releases, Linux container |
| **1.1.0** | Q2 2026 | Horizontal clustering, load balancer |

See **TODO.md** for the canonical task list.

For support jump into help-and-questions on Discord or open an issue labeled *support*.