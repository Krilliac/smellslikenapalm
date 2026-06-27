# RS2V Custom Server

A from-scratch, clean-room dedicated-**server emulator** for *Rising Storm 2:
Vietnam* — an Unreal Engine 3 game (`EngineVersion 7258`) — written in C++23.

It is **not** an official server and is **not** affiliated with Tripwire
Interactive or Epic Games. Everything here is reverse-engineered from the retail
client's observable wire behaviour and reconstructed independently — no decompiled
game code.

> **Status: under active development.** The server builds (MSVC and MinGW), but the
> UE3 control-channel handshake is still being implemented and **a stock retail client
> cannot fully connect yet.** The public API, file layout, and naming conventions
> are not frozen and may change. See [Current status](#current-status) and
> [`TODO.md`](TODO.md) before relying on any subsystem or starting major work.

## What is this?

RS2V reconstructs the *server side* of RS2:V. The retail client dictates the wire
format; this project conforms to it rather than redesigning it. It implements
exactly the server-side surface a client needs to connect and play — there is no
editor, no rendering, and no client.

The architecture is organised around five pillars:

- **Netcode** — a custom TCP/UDP socket layer feeding a packet → bunch →
  replication pipeline that mirrors the UE3 control channel.
- **Protocol** — the UE3 "bunch"/control-channel protocol (handshake: Hello →
  Challenge → Login → Welcome). Message ordinals, NetGUID/PackageMap, and property
  replication are reverse-engineered and documented under [`docs/`](docs/).
- **Authoritative simulation** — the server is the single source of truth for game
  state (players, teams, rounds, objectives, physics, vehicles, weapons, damage).
  Clients propose; the server validates and disposes.
- **Security** — an independent **EAC *emulation*** (not real Easy Anti-Cheat)
  plus behavioural/statistical checks, authentication, and input validation. The
  client is treated as adversarial.
- **Telemetry & configuration** — metrics with multiple exporters, and an
  INI-driven config system with validation and hot-reload.

## Current status

| Area | State |
|------|-------|
| Build (Linux GCC/Clang, Windows MSVC) | **Working** — `rs2v_server` and the test suite compile. |
| UE3 control-channel handshake | **In progress** — ordinals reverse-engineered and being wired up; a stock client cannot fully connect yet. |
| Live netcode (end-to-end gameplay) | **Partial** — socket/packet/replication plumbing exists; gameplay traffic is not yet functional. |
| Authoritative game simulation | **Partial** — players, teams, rounds, objectives, maps, vehicles, weapons exist in varying maturity. |
| Security / EAC emulation | **Partial** — an emulation layer and validators exist; coverage is incomplete. |
| Telemetry | **Partial** — collectors and exporters (JSON / Prometheus / CSV) exist; coverage is incomplete. |
| C# (.NET) scripting | **Disabled** — depends on a deprecated .NET COM hosting API (`ICorRuntimeHost`) that does not build; `ENABLE_SCRIPTING` is `OFF`. |

The rest of this README describes how to build and run what exists today. Treat
broader feature descriptions elsewhere in the docs as design targets unless the
status table above says otherwise.

## Architecture

```
src/
  Config/      INI parsing, config managers, validation, hot-reload
  Game/        Authoritative simulation: server loop, players, teams, rounds,
               objectives, maps, vehicles, weapons, damage, admin, chat
  Generated/   Build-time packet-handler code generator + generated output
  Math/        Vector/math primitives
  Network/     Sockets, packets, bunches, replication, bandwidth, compression
  Physics/     Rigid-body, collision, vehicle/helicopter physics
  Protocol/    UE3 control-channel protocol, packet types, reverse engineering
  Scripting/   C# (.NET) scripting host (disabled — see Current status)
  Security/    EAC emulation, auth, input/movement validation, audit logging
  Time/        Clocks, timers, scheduling
  Utils/       Logger, string/byte helpers, thread pool, memory pool, crypto
  main.cpp     Server entry point
telemetry/     Metrics collection + exporters (JSON / Prometheus / CSV)
config/        Default INI config files shipped with the server
data/          Static game data (maps, weapons, etc.)
tests/         Native test suite (no GoogleTest dependency) + fuzz tier
tools/         Reverse-engineering / capture / codegen helper scripts
docs/          Canonical documentation home
```

The packet-handler layer is **code-generated at build time**: `PacketHandlerCodeGen`
(`src/Generated/CodeGen.cpp`) emits one `Handle_<TAG>` stub per packet type plus a
static registry into the build tree, which `rs2v_core` then links. Do not hand-edit
generated output — change the generator.

For the full design, see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Building

**Toolchain (C++23 baseline):** GCC 12+, Clang 15+, or MSVC (VS 2022); CMake 3.20+.
OpenSSL and zlib are optional (the build falls back to built-in implementations if
they are absent). The test suite uses the project's own native framework — no
GoogleTest, no network, builds and runs offline.

```bash
# Clone
git clone https://github.com/Krilliac/smellslikenapalm.git
cd smellslikenapalm

# Configure (Linux, Ninja, with tests)
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TELEMETRY=ON -DENABLE_SCRIPTING=OFF \
  -DENABLE_COMPRESSION=ON -DBUILD_TESTS=ON

# Build
cmake --build build --parallel $(nproc)

# Test
cd build && ctest --output-on-failure --timeout 120
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TELEMETRY` | `ON` | Build the telemetry system. |
| `ENABLE_SCRIPTING` | `OFF` | Build the C# scripting host (currently does not build — see Current status). |
| `ENABLE_COMPRESSION` | `ON` | Packet compression (zlib if found, otherwise a built-in stub). |
| `BUILD_TESTS` | `OFF` | Build the native test suite. |
| `BUILD_RE_SELFTEST` | `OFF` | Build the standalone, network-free reverse-engineering self-test. |

## Running

```bash
# From the build directory; paths are relative to the working directory.
./rs2v_server -c ../config/server.ini

# Override the port; show help / version.
./rs2v_server -c ../config/server.ini -p 7777
./rs2v_server --help
./rs2v_server --version
```

The command-line surface is intentionally small — `-c/--config`, `-p/--port`,
`-h/--help`, `-v/--version`. Everything else (telemetry, security, RCON, game
rules, map rotation) is configured through the INI files in `config/`, which
support hot-reload. The default config is `config/server.ini`.

### Docker

```bash
docker compose up --build
```

`docker-compose.yml` builds the image, mounts `config/`, `data/`, and `logs/`, and
exposes the game and metrics ports. See [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md)
for production deployment.

## Configuration

The server reads layered INI files from `config/` (`server.ini`, `game_modes.ini`,
`maps.ini`, `weapons.ini`, `teams.ini`, `gameplay_settings.ini`,
`admin_commands.ini`, the text-based list files, and `eac_scanner.json`). Keys are
validated on load and most support hot-reload. The complete reference — every file,
key, default, and validation rule — is in
[`docs/CONFIGURATION.md`](docs/CONFIGURATION.md).

## Documentation

The canonical documentation home is [`docs/`](docs/); start at
[`docs/Home.md`](docs/Home.md) or [`docs/_Sidebar.md`](docs/_Sidebar.md). A GitHub
wiki mirror is generated from `docs/` via [`docs/sync-wiki.sh`](docs/sync-wiki.sh).

**Operating the server**
- [Configuration Reference](docs/CONFIGURATION.md)
- [Admin Commands](docs/ADMIN_COMMANDS.md)
- [Game Modes](docs/GAME_MODES.md) · [Maps](docs/MAPS.md)
- [Deployment](docs/DEPLOYMENT.md) · [Troubleshooting](docs/TROUBLESHOOTING.md) · [FAQ](docs/FAQ.md)

**Engineering**
- [Architecture](docs/ARCHITECTURE.md) · [API Reference](docs/API.md)
- [Coding Standards (C++23)](docs/CODING_STANDARDS.md) · [Anti-Bloat](docs/ANTI_BLOAT.md) · [Design Decisions](docs/DESIGN_DECISIONS.md)
- [Netcode](docs/NETCODE.md) · [Security](docs/SECURITY.md) · [Telemetry](docs/TELEMETRY.md) · [Scripting](docs/SCRIPTING.md)
- AI-assisted development context: [`CLAUDE.md`](CLAUDE.md), [`AGENTS.md`](AGENTS.md), [`CLAUDE_PARALLEL.md`](CLAUDE_PARALLEL.md)

**Protocol / reverse engineering**
- [Control-Channel Wire Spec (7258)](docs/RS2V_ControlChannel_WireSpec_7258.md)
- [Actor Replication](docs/RS2V_ActorReplication_7258.md) · [Post-Join Replication](docs/RS2V_PostJoin_Replication_7258.md)
- [UE3 Actor Channel](docs/UE3_ActorChannel.md) · [UE3 Property Replication](docs/UE3_PropertyReplication.md) · [UE3 NetGUID/PackageMap](docs/UE3_NetGUID_PackageMap.md)
- Deeper RE notes under [`docs/re/`](docs/re/) and hardening notes under [`docs/hardening/`](docs/hardening/)

## Contributing

Issues and pull requests are welcome. Before starting major work, read
[`CONTRIBUTING.md`](CONTRIBUTING.md), skim [`TODO.md`](TODO.md), and — if you are
working with an AI assistant — [`CLAUDE.md`](CLAUDE.md), which captures the
project's conventions, the trust boundary (the client is untrusted; the server is
authoritative), and the C++23 coding standard.

Development happens on feature branches; the merge target is `main`. Sync (rebase
on `main`) before committing, keep the build and tests green, and run `cppcheck`
as CI does (see [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)).

## License

Licensed under the **RS2V Server Non-Commercial Open Source License** (modeled
after emulator projects such as TrinityCore and MaNGOS). Commercial use and
closed-source distribution are prohibited. See [`LICENSE`](LICENSE).

## Acknowledgments

- Tripwire Interactive — for *Rising Storm 2: Vietnam*.
- Epic Games — for the Unreal Engine 3 networking architecture this work studies
  from the outside.
- The community — for testing, captures, and feedback.

Support and discussion: [GitHub Issues](https://github.com/Krilliac/smellslikenapalm/issues)
· [GitHub Discussions](https://github.com/Krilliac/smellslikenapalm/discussions)
· [Discord](https://discord.gg/sd8HaMc8rh).

## Performance targets

> These are **design targets**, not measured benchmarks. End-to-end gameplay is not
> yet functional (see [Current status](#current-status)), so no representative live
> numbers are available.

| Metric | Target |
|--------|--------|
| **Player Capacity** | 64+ concurrent |
| **Tick Rate** | 60+ Hz |
| **Latency overhead** | low, sub-frame |
| **Memory** | bounded, pre-allocated pools |
