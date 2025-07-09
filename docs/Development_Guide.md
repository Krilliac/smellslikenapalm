# RS2V Custom Server Development Guide

This document provides a step-by-step guide for developers contributing to or extending the RS2V Custom Server. It covers repository layout, build requirements, coding conventions, subsystem overviews, testing, debugging, and contribution workflows.

## Table of Contents

1. [Prerequisites & Dependencies](#prerequisites--dependencies)  
2. [Repository Structure](#repository-structure)  
3. [Build & Run Instructions](#build--run-instructions)  
4. [Configuration Management](#configuration-management)  
5. [Core Subsystems Overview](#core-subsystems-overview)  
6. [Adding New Features](#adding-new-features)  
7. [Testing & Validation](#testing--validation)  
8. [Debugging & Logging](#debugging--logging)  
9. [Code Formatting & Conventions](#code-formatting--conventions)  
10. [Contribution Workflow](#contribution-workflow)  
11. [Release & Deployment](#release--deployment)  
12. [Further Reading](#further-reading)  

## Prerequisites & Dependencies

- **Compiler:** C++17-compliant (GCC 9+, Clang 10+, MSVC 2019+).  
- **Build System:** CMake ≥ 3.15.  
- **Libraries:**  
  - OpenSSL (for secure sockets).  
  - (Optional) nlohmann/json if JSON support is added.  
- **Tools:**  
  - Git for version control.  
  - Ninja or Unix Make (optional).  
- **Platform Support:** Windows (Visual Studio), Linux (GCC/Clang), macOS (Clang).  

## Repository Structure

```
smellslikenapalm/
├─ config/               # INI configuration files
├─ data/                 # Game data (maps, modes, teams, weapons)
├─ docs/                 # Documentation (API, Development Guide)
│   ├─ API_Reference.md
│   └─ Development_Guide.md
├─ include/              # Public headers
├─ src/                  # Source code
│   ├─ Config/           # ConfigManager, parsers, validators
│   ├─ Network/          # Networking stack
│   ├─ Security/         # Auth, anti-cheat, ban manager
│   ├─ Game/             # Game logic, session, entities
│   ├─ Utils/            # Logging, file & string utilities
│   └─ main.cpp          # Entry point
├─ scripts/              # Build and maintenance scripts
├─ tests/                # Unit and integration tests
├─ CMakeLists.txt        # Top-level CMake project
├─ README.md             # Project overview & quickstart
└─ LICENSE               # RS2V Server Non-Commercial License
```

## Build & Run Instructions

1. **Clone & prepare**  
   ```bash
   git clone https://github.com/Krilliac/smellslikenapalm.git
   cd smellslikenapalm
   mkdir build && cd build
   ```

2. **Configure with CMake**  
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   ```

3. **Build**  
   ```bash
   cmake --build . --config Release
   ```

4. **Run**  
   ```bash
   ./rs2v_server --config ../config/server.ini
   ```
   - Logs written to `logs/` by default.  
   - Use `--help` for CLI options.

## Configuration Management

- **Unified Config:** `config/server.ini` consolidates core, network, security, logging, and performance settings.  
- **Data Files:**  
  - `config/maps.ini`, `config/game_modes.ini`, `config/teams.ini`, `config/weapons.ini`, etc.  
  - `config/admin_commands.ini`, `admin_list.txt`, `ban_list.txt`, `motd.txt`.  
- **Dynamic Reload:** `ConfigManager::ReloadConfiguration()` applies changes at runtime.  
- **Validation:** `ConfigValidator` ensures value ranges and required keys.

## Core Subsystems Overview

1. **ConfigManager**  
   - Loads/parses INI, exposes key/value store.  
   - Notifies listeners on change.

2. **Network Layer**  
   - `NetworkConfig`, TCP/UDP sockets with reliable transport overlays.  
   - Heartbeat, idle-timeout, packet framing.

3. **Security & Anti-Cheat**  
   - Steam ticket auth, custom fallback tokens.  
   - `BanManager`, EAC emulation modes: off/safe/emulate.  
   - IP blacklist, telemetry blocking.

4. **Game Systems**  
   - `GameConfig`, `MapConfig`: map rotation, modes, team logic.  
   - Session management, player state, scoring.

5. **Utilities**  
   - Logging (`Utils::Logger`), file I/O, string helpers.  
   - ThreadPool, MemoryPool, Profiler.

6. **Admin & RCON**  
   - `admin_commands.ini` defines commands, handlers in `AdminManager`.  
   - RCON server on configurable port, permission levels.

## Adding New Features

1. **Define Config**  
   - Add new key under `server.ini` with comment in `ConfigManager::GetConfigComment()`.  
   - Expose getter in `ServerConfig`.

2. **Consume in Code**  
   - Inject `ServerConfig` into subsystem constructor.  
   - Read via typed getter.

3. **Validation**  
   - Add range checks in `ConfigValidator::Validate…Section()`.

4. **Documentation**  
   - Update `API_Reference.md` and this guide.  
   - Provide sample config snippets.

## Testing & Validation

- **Unit Tests:** Add in `tests/` and integrate with CTest.  
- **Integration Tests:** Spin up server instance, exercise command API.  
- **Validation:** Use `ConfigValidator::ValidateConfigurationFile()` in CI to catch config errors.

## Debugging & Logging

- **Log Levels:** trace, debug, info, warn, error, fatal (set in `[Logging]`).  
- **On-the-Fly Toggles:** Via `DebugConfig` dynamic toggles section `[toggles]` in `server.ini`.  
- **Core Dumps & Profiling:** Enable via `[Profiler]` settings; output JSON reports.

## Code Formatting & Conventions

- **Style:** Google C++ style (4-space indent, CamelCase classes, snake_case variables).  
- **Line Length:** 100 characters max.  
- **Headers:** Include guard `#pragma once`; group system headers before project headers.  
- **Documentation:** Doxygen comments for public APIs; update docs on changes.

## Contribution Workflow

1. **Fork & Branch** – Feature branches off `main`.  
2. **Commit Messages** – `: Brief description`.  
3. **Pull Request** – Target `main`, include description of changes, config additions, and testing steps.  
4. **Review & CI** – Automated build, tests, config validation must pass.  
5. **Merge** – After approvals and CI green.

## Release & Deployment

- **Versioning:** Follow semantic versioning.  
- **Changelog:** Update `CHANGELOG.md`.  
- **Artifacts:** Build binaries via CI; package `config/` defaults.  
- **Deployment:** Upload to hosting, restart server with new version, monitor logs.

## Further Reading

- **API Reference:** `docs/API_Reference.md`  
- **Config Syntax & Examples:** See each file in `config/` directory.  
- **UE3 INI Conventions:** For inspiration on nested sections.