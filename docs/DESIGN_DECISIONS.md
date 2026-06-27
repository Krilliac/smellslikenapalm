# Design Decisions Log

> **Audience:** Anyone making an architectural change
>
> **Maturity:** active — append-only

An append-only record of decisions that **rule out an alternative a future slice
could otherwise pick.** The point is to stop the codebase re-litigating settled
questions: if you're about to do something this log says was deliberately rejected,
either follow the decision or add a new entry that supersedes it (with the reason).

When a slice lands a decision like this, append an entry **in the same commit**.
Keep entries short: context, decision, consequences, and what it rules out.

Format:

```
## YYYY-MM-DD — <short title>
**Context:** what prompted the decision.
**Decision:** what we chose.
**Rules out:** the alternative(s) a future slice should not silently pick.
**Consequences:** follow-on obligations / known costs.
```

---

## 2026-06-27 — C++23 is the mandated language baseline
**Context:** The project was pinned to C++17 (`CMAKE_CXX_STANDARD 17`) with
compatibility shims for old toolchains (e.g. `stdc++fs` linkage for GCC < 9). The
codebase is dominated by binary packet parsing, wire-protocol (de)serialisation,
and multithreaded networking — exactly the areas C++20/23 made materially safer
(`std::span`, `std::bit_cast`, `<bit>` endianness, `std::expected`, `std::jthread`,
`std::format`).
**Decision:** Move the baseline to C++23. Set `CMAKE_CXX_STANDARD 23` and raise
`cmake_minimum_required` to 3.20 (the first CMake that recognises standard `23`).
Supported toolchains: GCC 12+, Clang 15+, MSVC VS 2022 — all already used by CI.
cppcheck in CI moves from `--std=c++17` to `--std=c++23`. The full rationale and
the required idioms are in [`CODING_STANDARDS.md`](CODING_STANDARDS.md).
**Rules out:** Writing new code to the C++17 baseline; adding new C++17-era
compatibility shims (e.g. `stdc++fs`) for toolchains older than the C++23 minimums;
hand-rolled `(ptr, len)` buffer APIs where `std::span` fits.
**Consequences:** Toolchains older than GCC 12 / Clang 15 are no longer supported.
The pre-existing `VERSION_LESS 9.0` `stdc++fs` branches in `CMakeLists.txt` are now
unreachable (a C++23 compiler is always ≥ GCC 12); they are left inert for now and
can be removed in a dedicated cleanup. Existing C++17 code keeps compiling — it is a
migration target, to be modernised opportunistically rather than in one large churn.

## 2026-06-27 — Unified command system with one central permission gate
**Context:** Admin commands lived in `AdminManager::HandleAdminCommand` with a
binary `IsAdmin` check and no graded permissions, and `docs/ADMIN_COMMANDS.md`
described a much richer system (levels, RCON, ~18 commands) that the code never
implemented. There was no console or remote-control path, and persisted bans were
never loaded at startup. The task called for admin/dev/mod/player/console/config/
automation commands reachable in-game, from the console, and remotely (SOAP) — by
humans and AI alike.
**Decision:** Introduce `CommandManager` (`src/Game/CommandManager`) as the single
source of truth for commands: one registry, one `CommandContext` abstraction
(invoker + permission level + reply sink), and one central permission gate. Three
thin transports build a context and call it — in-game chat (`ChatManager`), local
console stdin (`ConsoleInput`, Console level), and a SOAP/HTTP endpoint
(`RemoteAdminServer`, for tooling/AI). `AdminManager` is reduced to the
authorization/ban *data store* plus the privileged kick/ban/unban operations the
manager calls; it no longer parses commands. Permission tiers are
Player/Helper/Moderator/Admin/Dev/Console, resolved from `admin_list.txt` levels.
**Rules out:** Per-transport command parsing or authorization (a second command
path is a security and drift hazard — the gate must live in exactly one place);
re-adding command dispatch to `AdminManager`; a binary admin/non-admin model.
**Consequences:** New commands are added once in `CommandHandlers.cpp` and are
immediately reachable from every transport at the correct level. The remote SOAP
endpoint is off unless a port *and* password are set and binds all interfaces
(socket-layer limitation) — operators must firewall it. God mode is honoured in
`DamageSystem` via a `Player` flag. The legacy `[Admin] rcon_*` keys remain inert
(no Source-RCON implementation); the remote transport uses `[RemoteAdmin]`.

<!-- Append new decisions above this line, newest first. -->
