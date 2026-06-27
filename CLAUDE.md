# RS2V Custom Server — Claude Code Context

## What is this?

RS2V Custom Server (codename *smellslikenapalm*) is a from-scratch, clean-room
**dedicated-server emulator** for *Rising Storm 2: Vietnam* — an Unreal Engine 3
game (`EngineVersion 7258`). It is written in C++ and organized around a
production-style architecture: a custom UE3-compatible netcode stack, an
authoritative game simulation, an anti-cheat *emulation* layer, telemetry, and a
configuration system.

It is **not** an official server and is **not** affiliated with Tripwire
Interactive or Epic Games. Everything here is reverse-engineered from the retail
client's observable wire behaviour and reconstructed independently.

This is a real, in-progress codebase — not greenfield. Treat the existing
subsystems as load-bearing: other people's reverse-engineering work is encoded
in the protocol tables, the handshake ordinals, and the replication layout.
Changing them blindly breaks the one thing that is hard to re-derive — bit-exact
compatibility with a client you do not control.

### Project pillars (do not drift from these)

- **Netcode**: A custom TCP/UDP socket layer feeding a packet → bunch →
  replication pipeline that mirrors the UE3 control channel. The wire format is
  dictated by the retail client; we conform to it, we do not redesign it.
- **Protocol**: UE3 "bunch"/control-channel protocol. Handshake = Hello →
  Challenge → Login → Welcome. Message ordinals, NetGUID/PackageMap, and
  property replication are reverse-engineered and live in `src/Protocol/` and
  `docs/RS2V_*` / `docs/UE3_*`.
- **Authoritative simulation**: The server is the single source of truth for game
  state — players, teams, rounds, objectives, physics, vehicles, weapons,
  damage. Clients propose; the server disposes.
- **Security / anti-cheat**: An **independent EAC emulation** (not real Easy
  Anti-Cheat) plus behavioural/statistical checks, authentication, input
  validation, and audit logging. The client is **adversarial** — every byte off
  the wire is untrusted until validated.
- **Telemetry**: Metrics collection with multiple exporters (JSON, Prometheus,
  CSV, in-memory).
- **Configuration**: INI-driven config with validation and hot-reload.

### What RS2V Server is **not**

- Not an official Tripwire/Epic product, and not a redistribution of any of their
  code or assets. Clean-room only — reconstruct from observed behaviour, never
  from decompiled game code.
- Not a full UE3 engine reimplementation. We implement exactly the server-side
  surface a retail client needs to connect and play — no editor, no rendering, no
  client.
- Not a cheat or a real anti-cheat. The EAC layer is an *emulation* that satisfies
  the client's expectations; it is not a security product and must never be
  described as one.
- Not frozen. The public API, file layout, and naming conventions may still
  change. See [`TODO.md`](TODO.md) for the live roadmap.

## Trust boundary (DO NOT VIOLATE)

**The client is untrusted. The server is authoritative.** Every effect a connected
client can have on game state must pass through server-side validation. The netcode
and protocol layers translate wire shapes; they never grant authority.

Concrete rules every networking/game translation unit must follow:

1. **Every read off the wire is bounds-checked.** Packet, bunch, and bitstream
   reads go through the safe reader APIs (`BitReader`/`PacketParser` and friends),
   which clamp and report short reads — they never trust a length field from the
   client. A malformed or hostile packet must fail closed (drop + log), never
   read out of bounds, never crash the server.
2. **The server owns game truth.** Position, health, ammo, score, team, round
   state — the client *requests* changes; the server validates against the rules
   (movement bounds, cooldowns, line-of-sight, ownership) before applying. A
   client-asserted value is a hint, not a fact.
3. **Auth and identity are server-owned.** Steam-style session validation, ban
   lists, and admin permission levels are decided server-side. The EAC emulation
   and any client-supplied identity token are *probe-satisfying facades* — they do
   not by themselves grant trust.
4. **Validate, then act — never the reverse.** Input validation
   (`src/Security/`, movement/packet validators) runs before state mutation, not
   after. "We'll sanitise it later" is a security bug even if it compiles.
5. **One source of truth per resource.** One authoritative entity table, one
   config manager, one player registry — reachable from multiple call sites but
   with one owner. No parallel shadow copies that can drift.

The reviewable signal: *"could a malicious client craft a packet that makes the
server read out of bounds, crash, desync, or grant the client something the rules
forbid?"* If yes, the validation is wrong — fix the gate, don't extend the
unchecked path.

## Definition of Done — before you call a slice complete

Compiling is not "done." Before committing / opening a PR, walk this list — the
obligations most often missed:

- [ ] **Re-scan every signal**, not just the one you started on — build, tests,
      cppcheck, CI. Fix what surfaces. (→ [Fix Anything You Surface](#fix-anything-you-surface--no-deferring))
- [ ] **Landed a `TODO.md` item?** Delete/check off its line in the same commit.
- [ ] **Update the owning doc** under [`docs/`](docs/) to reflect the new state —
      including any protocol/handshake spec (`docs/RS2V_*`, `docs/UE3_*`) if a
      slice changed wire behaviour.
- [ ] **Append to [`docs/DESIGN_DECISIONS.md`](docs/DESIGN_DECISIONS.md)** if the
      change rules out an alternative a future slice could otherwise pick.
- [ ] **New subsystem / protocol / config surface?** Add or amend a `docs/` page
      and add it to [`docs/_Sidebar.md`](docs/_Sidebar.md).
- [ ] **Tests added/updated** for the behaviour you changed — this project has a
      large `tests/` suite and a fuzz tier; new netcode/parsing paths need
      coverage, especially adversarial inputs.
- [ ] **STUB markers** present on deliberate omissions, absent on code that does
      its job.

## Session start (run at the beginning of every session)

**Step 1 — Git sync** (see [Git Sync Workflow](#git-sync-workflow) below):
Sync your branch with the latest upstream `main` **before** reading code or making
changes. Feature branches diverge as PRs merge; without rebasing you work on stale
code.

**Step 2 — Read the docs:** The canonical documentation home is [`docs/`](docs/).
Start at [`docs/Home.md`](docs/Home.md) or [`docs/_Sidebar.md`](docs/_Sidebar.md)
for the table of contents. Pending and deferred work lives in
[`TODO.md`](TODO.md). The protocol/reverse-engineering specs
(`docs/RS2V_*`, `docs/UE3_*`) are the hard-won record of how the retail client
behaves — read the relevant one before touching netcode.

**Step 3 — Bloat check (for code tasks):**

```bash
find src telemetry \( -name '*.cpp' -o -name '*.h' \) | xargs wc -l | sort -rn | head -15
```

If the task involves a file over the threshold (see [Anti-Bloat](#anti-bloat-guidelines)),
trim it first.

**Step 4 — Parallel-session check:** if other Claude Code sessions may be running
concurrently, follow [`CLAUDE_PARALLEL.md`](./CLAUDE_PARALLEL.md) — run
`tools/parallel/status.sh` and claim your subsystem before editing.

## Parallel Sessions

See [`CLAUDE_PARALLEL.md`](./CLAUDE_PARALLEL.md). RS2V may be worked on by several
Claude Code sessions at once. File ownership is coordinated through the tracked
coordinator `PARALLEL_WORK.md` and the helpers under `tools/parallel/`:

```bash
tools/parallel/status.sh                          # See active/completed sessions + conflicts
tools/parallel/claim.sh <sub> "<files>" "<desc>"  # Claim a subsystem before editing
tools/parallel/release.sh <sub>                   # Push your session branch when done
tools/parallel/release.sh <sub> --merge           # ...and merge to main (explicit opt-in)
```

`claim.sh` rebases on `origin/main`, warns on already-claimed files, and keeps you
on your `claude/*` session branch. `release.sh` pushes with `--force-with-lease`;
`--merge` is the explicit opt-in required before touching `main` (CI must be green
first). Do not hand-edit `PARALLEL_WORK.md`.

## Anti-Bloat Guidelines

AI-assisted development has a structural bias toward complexity: adding features
"just in case," creating helpers for single uses, over-engineering simple problems,
building systems without wiring them in. In a netcode-heavy server — where a wrong
abstraction calcifies into the wire protocol or the replication layout — this bias
is **more** dangerous than in ordinary application code. The goal is **sanity, not
sacrifice** — keep code clean without stripping legitimate verbosity or
readability. Full rationale: [`docs/ANTI_BLOAT.md`](docs/ANTI_BLOAT.md).

### Sensible thresholds (not hard limits)

| Thing | Threshold | What to do |
|-------|-----------|------------|
| `.cpp` file size | ~500 lines | Split if doing multiple jobs; leave if one coherent unit |
| `.h` file size | ~300 lines | Split if unrelated types; data-heavy headers are fine |
| Public methods per class | ~15 | Ask: "Does each method earn its place?" |
| Function length | ~60 lines | Split if nested branching; clear linear flow is fine |
| Parallel subsystems doing the same thing | 0 | Remove the duplicate |

### Before writing code — checklist

1. **Does this already exist?** Search before writing — especially for primitives
   (buffer readers, byte-swap, string helpers, config accessors).
2. **Will this be called?** If you can't name the caller, don't write it.
3. **Can existing code do this with a small change?** Prefer editing over adding.
4. **Is this a one-time use?** Inline it — no helper function, no new class.
5. **Am I future-proofing?** Stop. Write only what is needed today.
6. **Adding a new packet type / message ordinal?** The wire format is an ABI with
   a client you do not control. Once a tag is on the wire, it is forever. Be sure.
7. **Is the code dead?** Delete it — don't comment it out; git history exists.
8. **Is a system built but not wired in?** Either wire it in or delete it.

## Coding Standards

The full, enforceable standard lives in
[`docs/CODING_STANDARDS.md`](docs/CODING_STANDARDS.md). The essentials:

- **C++23 is the mandated baseline.** The project targets `CMAKE_CXX_STANDARD 23`
  (GCC 12+, Clang 15+, MSVC 19.3x / VS 2022). New code uses modern C++23 idioms;
  do **not** write to the old C++17 baseline. In particular, for this codebase:
  - `std::span` / `std::byte` for all packet and buffer handling — no raw
    `(ptr, len)` pairs threaded by hand.
  - `std::expected<T, E>` (and `[[nodiscard]]`) for fallible operations on the
    parsing/validation paths — prefer it over error-code out-params and over
    exceptions in hot netcode.
  - `<bit>`: `std::byteswap`, `std::endian`, `std::bit_cast` for endianness and
    POD (de)serialisation instead of `reinterpret_cast` + manual shifts.
  - `std::string_view` for non-owning string params; `std::format` / `std::print`
    for formatting; `std::optional` for "maybe absent"; `enum class` always.
  - `std::jthread` + `std::stop_token`, `std::scoped_lock`, `std::atomic`,
    `std::shared_mutex` for concurrency. Document which locks each subsystem owns
    at the top of its header.
  - Ranges and views where they make intent clearer; structured bindings;
    `constexpr`/`consteval`/`if consteval`; `concepts` over SFINAE.
- **Memory safety**: RAII everywhere. `std::unique_ptr`/`std::shared_ptr` owning,
  raw pointers non-owning and non-null by contract. **No naked `new`/`delete`.**
  No C-style casts. Prefer `std::array`/`std::vector`/`std::span` over C arrays
  and pointer arithmetic.
- **Const-correctness**: `const` on every non-mutating method and parameter;
  `constexpr` wherever it works; `noexcept` where the contract guarantees it.
- **Naming** (match existing code): PascalCase classes/methods, camelCase locals,
  `m_` prefix members, `UPPER_SNAKE` macros/constants. `#pragma once` headers,
  forward-declare to cut include bloat.
- **Zero warnings**: `-Wall -Wextra -Wpedantic` (GCC/Clang), `/W4 /permissive-`
  (MSVC). Treat warnings as fix targets; a warning-free build is the floor.
- **Security-first parsing**: every wire read bounds-checked, every length field
  treated as hostile, every deserialisation total over malformed input. See the
  fuzz tests in `tests/*FuzzTests.cpp` for the bar.
- **Stub markers**: any handler/path whose v0 implementation deliberately omits the
  real semantics carries a `// STUB:` comment on the line that bakes in the
  omission. A path that is correct but with a known limitation carries
  `// GAP: <what's missing> — <when to revisit>`. Both are greppable:
  `git grep -nE "// (STUB|GAP):"`. Do **not** annotate code that does its job.

## Architecture (real directory layout)

```
src/
  Config/        — INI parsing, config managers, validation, hot-reload
  Game/          — Authoritative simulation: server loop, players, teams, rounds,
                   objectives, maps, vehicles, weapons, damage, admin, chat
  Generated/     — Build-time packet-handler code generator (CodeGen.cpp) + output
  Math/          — Vector/math primitives
  Network/       — Sockets, packets, bunches, replication, bandwidth, compression
  Physics/       — Rigid-body, collision, vehicle/helicopter physics
  Protocol/      — UE3 control-channel protocol, packet types, reverse-engineering
  Scripting/     — C# (.NET) scripting host (currently disabled — see CMake notes)
  Security/      — EAC emulation, auth, input/movement validation, audit logging
  Time/          — Clocks, timers, scheduling
  Utils/         — Logger, string/byte helpers, thread pool, memory pool, crypto
  main.cpp       — Server entry point
telemetry/       — Metrics collection + exporters (JSON / Prometheus / CSV)
config/          — Default INI config files shipped with the server
data/            — Static game data (maps, weapons, etc.)
tests/           — GoogleTest-free native test suite + fuzz tier
tools/           — Reverse-engineering / capture / codegen helper scripts
docs/            — Canonical documentation home (the wiki)
deploy/          — Deployment assets
```

The packet-handler layer is **code-generated** at build time:
`PacketHandlerCodeGen` (`src/Generated/CodeGen.cpp`) emits one `Handle_<TAG>` stub
per packet type plus a static registry into the build tree. `rs2v_core` consumes
the generated registry. Do not hand-edit generated output — change the generator.

## Build

```bash
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

Toolchain baseline (C++23): GCC 12+, Clang 15+, or MSVC (VS 2022). CMake 3.20+.
`ENABLE_SCRIPTING` is **OFF** by default — the C# host depends on a deprecated
.NET COM hosting API (`ICorRuntimeHost`) that does not build and needs reworking
onto a supported hosting API before it can be re-enabled.

## Git Sync Workflow

Run before every session start and before every commit/push. Default upstream
branch is `main`.

```bash
git fetch origin main
git log --oneline HEAD..origin/main | wc -l   # check if behind
git rebase origin/main                         # if behind, rebase
# If conflicts: resolve, git add <files>, git rebase --continue
```

**Rules:**
- **Never** commit or push while behind the base branch. Rebase first.
- All Claude-driven development happens on the feature branch the harness checked
  out for the session (`claude/<slug>`). Merge target is `main`. Do not push to
  other branches without explicit permission.

## Pre-commit checks

Run checks **appropriate to the files you changed**.

### Docs-only changes (`.md`, `docs/`)
Proofread. Keep [`docs/_Sidebar.md`](docs/_Sidebar.md) in sync if you add/rename a
page. If a GitHub wiki mirror exists, run `docs/sync-wiki.sh` (see that script).

### Code changes (`.h`, `.cpp`, `CMakeLists.txt`)

```bash
# 1. Configure + build (mirror CI)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --parallel $(nproc) 2>&1 | tail -40

# 2. Tests
cd build && ctest --output-on-failure --timeout 120 && cd ..

# 3. Static analysis (mirror CI)
cppcheck --enable=warning,performance,portability --inline-suppr \
  --error-exitcode=1 -I src/ -I telemetry/ --std=c++23 src/ telemetry/
```

If any step fails, fix before committing.

## Post-PR checks

After creating or pushing to a PR, **always** poll CI and fix failures before
moving on. Use the GitHub MCP tools available in this environment — do not shell
out to `gh`. CI runs Linux (gcc-12 + clang-15) and Windows builds, the full test
suite, cppcheck, and a Docker build. Every red check is a fix target.

## Fix Anything You Surface — No Deferring

Every kind of work — a feature, a bug fix, a refactor, an audit, reading a test
log, running CI, running cppcheck — reveals the next layer of issues. **Fix
everything that surfaces, even if it predates your slice, even if it's outside the
obvious boundary of your task.** A "not my code" regression is still visible to
whoever picks up the codebase next; deferring just buries the cost.

This supersedes the anti-bloat "don't add beyond the task" rule **only** when the
extra work fixes something concretely broken right now (a failing test, a non-zero
CI signal, a warning, a crash, a wrong value returned from a real call, a stale
comment). It does **not** license speculative refactoring or "while I'm here"
rewrites — those remain anti-bloat targets.

The discipline:

- **After every change, re-scan every surface that produces signal**, not just the
  one you started with:
  - Build: `cmake --build build` — every `warning:` is a fix target, every error is.
  - Tests: `ctest --output-on-failure` — every non-PASS is a fix target.
  - cppcheck: the CI invocation above — every reported issue is a fix target.
  - Fuzz tests (`tests/*FuzzTests.cpp`): a new crash/finding is a fix target.
  - CI: every red check on the branch's PR. Poll until green.
- **Scope is whatever the signal exposes**, not whatever fits the commit message.
- **A symptom-cluster gets one investigation, not N.** When N similar failures
  appear, trace ONE to root cause before touching the others — the root usually
  explains the cluster.
- **Class-of-bug pattern matching** — recurring shapes in a netcode server:
  - **Unchecked wire read.** A length/count field from the client drives a read or
    allocation without a bound. Symptom: crash/OOM/overread on a crafted packet.
    Fix: clamp + fail closed; add a fuzz case.
  - **Endianness / packing drift.** A struct read with `reinterpret_cast` assumes
    host byte order or layout. Symptom: values correct on one platform, garbage on
    another. Fix: explicit `std::bit_cast` + `std::byteswap` per the wire spec.
  - **Desync from non-authoritative trust.** Server applies a client-asserted value
    without validation. Symptom: client sees/does something the rules forbid. Fix:
    validate server-side before mutating state.
  - **Stale-comment / stale-spec drift.** A comment or `docs/` spec claims behaviour
    the code no longer implements. Fix both.
  - **Refcount / lifetime asymmetry.** An acquire path adds ownership a release path
    doesn't drop on every exit. Walk EVERY exit and verify hand-off or rollback.
- **One run is not enough for intermittent symptoms.** Races, ASLR, hash-order, and
  scheduling-dependent crashes hit proportionally in production. Re-run to confirm
  intermittency, then look for collisions and lifetime asymmetries. Intermittent
  bugs ARE bugs.
- **"No deferring" is the default, not the exception.** Do not file "address later"
  notes in commit messages or stash issues in a TODO. Fix it now, or argue
  concretely why the fix can't land this session.

## Wiring Things In — Functionality Is Not Optional

A system that exists but is never initialized, called, or connected is **worse than
not existing** — it rots until a refactor accidentally re-enables it.

- **Every packet handler must be in the generated registry / dispatch.** A handler
  that compiles but isn't dispatched is dead code.
- **Every subsystem with an `Init()`/`Start()` must be wired into the server
  bring-up** in a known order.
- **Every config key read must have a default and validation;** every validator
  must be called on the path it guards.
- **Every metric/sink must have a source.** If telemetry receives data, something
  must emit it.

If you find a subsystem built but not wired in: **wire it in immediately, or delete
it.**

## Diagnostic Logging — Keep It, Gate It

When diagnosing a bug you almost always add log lines to localise the failure.
**Don't strip them once the bug is fixed** — they're exactly what the next debugger
will want. Gate them through the project's `Logger` levels so they don't flood the
console in production: a failure summary at WARN/ERROR, verbose detail at DEBUG.
Reserve unconditional output for the structural sentinels that tests grep for. A
clean run stays quiet at default level; a regression leaves a WARN sentinel +
DEBUG-gated detail behind it without anyone re-adding prints.

## Reusable Tooling — Save It, Don't Re-Derive It

When you write a script, harness, or one-off tool with value beyond the immediate
task — a packet capture decoder, a repro driver, a log correlator, a netfield
extractor — **commit it under `tools/` instead of leaving it in `/tmp`.** The next
session that needs it should `ls tools/` and find it. Match the existing siblings'
shape, parameterise the hard-coded paths/timeouts, keep it dependency-light, and
syntax-check it before committing. Commit the tool with the work it supported.

## Documentation home

The single canonical documentation home is [`docs/`](docs/). Subsystem guides,
the protocol/reverse-engineering specs, the configuration and admin references, the
coding standard, the design-decisions log, and the project history all live there.
[`docs/_Sidebar.md`](docs/_Sidebar.md) is the table of contents.

### When to write a new docs page

Add a page when: a new subsystem lands and is wired into the server; a new protocol
message/spec is reverse-engineered; a new config surface or admin command is added;
or a standalone topic accrues enough cross-references that one canonical page beats
inlining everywhere. **Not** when: the topic is a one-paragraph addendum to an
existing page (amend it), or a transient TODO (add it to [`TODO.md`](TODO.md)).

### At session end

If you discovered something that durably changes a docs page (a new known-limit, a
new threshold, a corrected ordinal), update that page in the **same commit** as the
code. Don't accumulate a separate notes file.
