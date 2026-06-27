# Coding Standards

> **Audience:** Anyone writing C++ in this repo
>
> **Maturity:** active — this is the enforceable standard, not a wishlist

This is the authoritative coding standard for RS2V Custom Server. [`CLAUDE.md`](../CLAUDE.md)
summarises it; this page is the long form. When a rule here conflicts with older
code, the rule wins and the old code is a migration target — but migrate
opportunistically, not in giant churn commits (see [Anti-Bloat](ANTI_BLOAT.md)).

## The C++23 mandate

**C++23 is the mandated baseline.** The project builds with `CMAKE_CXX_STANDARD 23`
(`CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`). The previous C++17
baseline is retired. Write new code to C++23; do not target C++17 idioms "to be
safe."

**Supported toolchains** (all have the C++23 surface this project uses):

| Toolchain | Minimum | Notes |
|-----------|---------|-------|
| GCC       | 12+     | CI uses g++-12. `-std=c++23` (a.k.a. `c++2b` on 12). |
| Clang     | 15+     | CI uses clang++-15. |
| MSVC      | VS 2022 (19.3x) | `/std:c++latest` where a feature isn't in `/std:c++23` yet. |

CMake `cmake_minimum_required(VERSION 3.20)` — `CMAKE_CXX_STANDARD 23` is only
recognised from CMake 3.20.

If a specific C++23 library feature is missing on a supported compiler (e.g. a
partial `<format>` or `std::expected` implementation), prefer a thin project-local
shim in `src/Utils/` over downgrading the whole file's idioms — and record the gap
in [Design Decisions](DESIGN_DECISIONS.md).

## Why C++23 here, specifically

This is a packet-parsing, binary-protocol, multithreaded network server. C++23 maps
almost one-to-one onto the things this codebase already does by hand, more safely:

| Hand-rolled today | C++23 replacement | Win |
|-------------------|-------------------|-----|
| `(uint8_t* buf, size_t len)` pairs | `std::span<const std::byte>` | bounds carried with the data; no desync between ptr and len |
| `reinterpret_cast` + manual shifts | `std::bit_cast`, `<bit>` | defined behaviour, no aliasing UB |
| host-order assumptions | `std::byteswap`, `std::endian` | explicit, portable wire byte order |
| error-code out-params / sentinels | `std::expected<T, E>` + `[[nodiscard]]` | un-ignorable failures on parse/validate paths |
| `char*` / `std::string` copies for params | `std::string_view` | no allocation to read |
| `printf`/stringstream logging | `std::format` / `std::print` | type-safe, faster, no format/arg mismatch |
| raw `std::thread` + manual join | `std::jthread` + `std::stop_token` | RAII join, cooperative cancellation |

## Modern C++ — required idioms

### Buffers and binary I/O (the netcode core)
- **Use `std::span<std::byte>` / `std::span<const std::byte>` for every buffer.**
  Never thread a raw pointer + length by hand across an API boundary.
- **Use `std::bit_cast` for POD (de)serialisation**, never `reinterpret_cast` of a
  buffer into a struct. Pair it with explicit `std::byteswap` / `std::endian`
  conversions per the documented wire byte order.
- **Every read off the wire is bounds-checked** through the safe reader APIs
  (`BitReader` / packet parsers). A length or count field from the client is
  hostile until clamped. Failing closed (drop + log) is correct; reading out of
  bounds or crashing is a security bug. The bar is the fuzz suite
  (`tests/*FuzzTests.cpp`).

### Error handling
- **Prefer `std::expected<T, E>`** for fallible operations on parse / validate /
  config paths. Mark fallible functions `[[nodiscard]]`.
- Reserve exceptions for truly exceptional, non-hot-path conditions (startup,
  config load). Do not throw across the per-packet / per-tick hot path.
- Never use `errno`-style globals or bare `bool`/`-1` sentinels for new code.

### Types and values
- `enum class` always — never unscoped `enum`. Give it an explicit underlying type
  when it goes on the wire or into config.
- `std::optional<T>` for "maybe absent." `std::variant` over tagged unions.
- `std::string_view` for non-owning string params; `std::span` for non-owning
  ranges. Return owning types (`std::string`, `std::vector`) only when you own.
- Designated initializers for aggregates; structured bindings for tuples/pairs.
- `constexpr` / `consteval` / `if consteval` wherever evaluation can be compile-time
  — protocol tables and lookup maps especially.
- `concepts` over SFINAE; ranges/views where they make intent clearer (don't force
  a pipeline where a plain loop reads better).
- `std::format` / `std::print` for formatting; never `sprintf` into a fixed buffer.
- `std::source_location` for log call-site context instead of `__FILE__`/`__LINE__`
  macros.

### Memory and ownership
- **RAII everywhere.** Every resource (socket, file, lock, allocation) is owned by
  an object whose destructor releases it.
- `std::unique_ptr` for sole ownership, `std::shared_ptr` only when ownership is
  genuinely shared. Raw pointers are **non-owning** and non-null by contract.
- **No naked `new` / `delete`.** Use `std::make_unique` / `std::make_shared`, or the
  project's pools (`src/Utils/MemoryPool*`) where pooling is intended.
- Prefer `std::array` / `std::vector` / `std::span` over C arrays and pointer
  arithmetic. No C-style casts — use `static_cast` / `std::bit_cast` and explain any
  `reinterpret_cast` that genuinely can't be avoided.

### Concurrency
- `std::jthread` + `std::stop_token` for owned threads; cooperative cancellation
  over kill flags.
- `std::scoped_lock` / `std::unique_lock`, `std::shared_mutex` for read-mostly
  state, `std::atomic` for lock-free counters/flags. Never a bare `lock()` /
  `unlock()` pair.
- **Document lock ownership at the top of each header** that owns shared state:
  which lock guards which fields, and the lock-ordering rule. A sleeping wait must
  never be held across the per-tick critical section.
- No global mutable state. If it looks like a singleton, it's almost always a
  per-server or per-connection structure.

### Const-correctness & interface hygiene
- `const` on every non-mutating method and parameter. `constexpr` where it works.
- `noexcept` where the contract guarantees no throw (move ops, swap, hot-path
  accessors).
- `[[nodiscard]]` on factory functions, fallible returns, and anything whose result
  must be observed.
- `#pragma once` headers; forward-declare to cut transitive include bloat (the
  `GameServer.h` style of forward-declaring collaborators is the model).

## Naming (match the existing code)

| Thing | Convention | Example |
|-------|------------|---------|
| Classes / methods | PascalCase | `class RoundManager`, `void StartRound()` |
| Local variables | camelCase | `auto playerCount = ...` |
| Member variables | `m_` prefix | `m_activePlayers` |
| Macros / constants | UPPER_SNAKE | `MAX_PLAYERS`, `RS2V_HAS_ZLIB` |
| Files | match folder style | `RoundManager.h` / `RoundManager.cpp` |

## Formatting

- Match the surrounding file. The project uses 4-space indent and reads as
  K&R/Allman-ish braces — follow the file you're in rather than reformatting it.
- 120-column soft limit. LF line endings.
- One statement per line; braces on non-trivial loop/if bodies.
- **Never sacrifice readability to hit a line count.** Keep "why" comments,
  descriptive names (`pageTableEntryMask` > `ptm`), and vertical whitespace between
  logical sections. The test: *does this make sense to someone reading it for the
  first time while a server is on fire in production?*

## Warnings & static analysis (the floor, not the ceiling)

- **Zero warnings.** GCC/Clang build with `-Wall -Wextra -Wpedantic`; MSVC with
  `/W4 /permissive-`. Every `warning:` is a fix target.
- **cppcheck** runs in CI (`--enable=warning,performance,portability`,
  `--std=c++23`, `--error-exitcode=1`). Fix findings; use `// cppcheck-suppress`
  inline only with a one-line justification.
- New behaviour ships with tests. Netcode / parsing / validation paths ship with
  **adversarial** tests — feed malformed and hostile input, not just the happy path.

## STUB / GAP markers

Any handler or path whose v0 implementation deliberately omits the real semantics
carries a marker on the line that bakes in the omission:

- `// STUB:` — returns a constant / does nothing / returns the wrong target. Real
  callers WILL behave incorrectly. Stays until a real implementation lands.
- `// GAP: <missing> — <revisit>` — correct for the happy path, but a documented
  edge case is unimplemented (e.g. "no IPv6", "oversize bunch unhandled").

Both are greppable — `git grep -nE "// (STUB|GAP):"` is the live gap inventory.
**Do not** annotate code that does its job; the convention exists to bound the gap
list, not to decorate every line.

## Security-first parsing (non-negotiable)

The client is adversarial. For every byte that comes off the wire:

1. Bound every read against the actual buffer length — never trust a client length
   field.
2. Validate every client-asserted value against the rules before it touches
   authoritative state (movement bounds, cooldowns, ownership, team).
3. Fail closed: a malformed packet is dropped and logged, never partially applied.
4. No UB on hostile input — no overread, no signed overflow, no out-of-range index.

See [`SECURITY.md`](SECURITY.md) and the hardening notes in
[`hardening/`](hardening/) for the threat model and the fuzz/static-analysis bar.
