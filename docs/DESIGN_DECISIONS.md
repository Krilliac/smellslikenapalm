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

## 2026-07-15 — Pin compiler and standard-library pairs for C++23
**Context:** The code now uses `std::expected`. The old Linux matrix paired
Clang 15 with whichever libstdc++ headers `ubuntu-latest` provided and retained
GCC 12 as a nominal minimum. On Ubuntu 24.04, neither pairing supplied the full
C++23 library surface the project consumes.
**Decision:** Supersede the Linux toolchain floors from the 2026-06-27 decision.
Pin CI to Ubuntu 24.04 and verify GCC 14 with libstdc++ 14 plus Clang 18 with
libc++ 18. Treat the compiler and standard library as one supported toolchain.
Keep MSVC VS 2022 as the Windows baseline.
**Rules out:** Floating `ubuntu-latest` as the Linux compiler contract; pairing
Clang 18 with an unverified libstdc++; claiming support from language-mode flags
alone when required library types are absent.
**Consequences:** The supported floors are GCC 14/libstdc++ 14, Clang 18/libc++
18, and MSVC VS 2022. Future compiler bumps must name and test the standard
library too.

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

## 2026-06-27 — Single authoritative ban store; non-fatal exception recovery
**Context:** Two ban stores pointed at the same `config/ban_list.txt` in
incompatible formats: `AdminManager` (`steamId epochSeconds`, used by the command
system + anti-cheat) and `SecurityManager::m_banManager` (`steamId|T/P|expiry|reason`,
the store that actually enforces bans at connect time). `AdminManager`'s
`IsBanned` had no callers, so its list was a shadow that never enforced anything
yet wrote the shared file — which `BanManager`'s destructor then truncated on
shutdown. The shipped sample file was in a *third* format neither could parse, and
`BanManager` serialised `steady_clock` epochs (per-process), so temporary bans
were meaningless after a restart. Separately, a recoverable exception escaping a
subsystem boundary (e.g. one bad packet, one bad command) reached
`std::terminate` and killed the whole server.
**Decision:** (1) Make `SecurityManager`/`BanManager` the single ban owner.
`AdminManager` ban ops delegate via `GameServer` → `ConnectionLoginBridge`
(which keeps the Security headers — and their `ClientAddress` clash — out of the
Game TUs) and surface bans as a neutral `BanRecord`. `AdminManager` drops its
shadow map and file I/O. `BanManager` switches to `system_clock` so temporary
bans persist correctly. (2) Add `rs2v::ReportNonFatalException` / `rs2v::Guard`
to the crash handler — the complement of the existing `std::set_terminate` path:
catch at a boundary, log with the same diagnostics, and continue. Guards wrap the
game tick, per-packet dispatch, command dispatch, and the console/remote worker
threads; `query` reports the recovered-exception count.
**Rules out:** A second ban list anywhere; AdminManager owning ban persistence;
persisting `steady_clock` time points; letting a recoverable per-boundary
exception propagate to `std::terminate`.
**Consequences:** One ban file, one format (pipe-delimited, wall-clock expiry),
machine-managed. Reaching the security layer from Game code goes through the
bridge's neutral forwarders, never a raw `SecurityManager*`. A throwing handler or
malformed packet now logs a `NON-FATAL EXCEPTION` banner and the server keeps
running; only genuinely unrecoverable faults (signals, uncaught exceptions that
escape all guards) remain fatal.

## 2026-06-27 — Guard every thread entry point, not just the game tick
**Context:** The non-fatal `rs2v::Guard` recovery (entry above) was wired into the
main game tick, per-packet dispatch, command dispatch, and the console/remote
worker threads — but several other thread entry points still ran their work
unguarded. An uncaught exception on a worker thread does not unwind into `main`;
it calls `std::terminate` and kills the whole server. The unguarded loops
included the two that process untrusted network bytes: `NetworkThread::RunLoop`
(`PumpNetwork` + tick callback) and `EACServerEmulator::RunLoop` (`HandlePacket`),
plus the `Timer` callback threads, the detached auto-handler-regen thread,
`ProtocolDecoder::WorkerLoop`, and `TelemetryManager::SamplingLoop` (which caught
`std::exception` but not a non-`std::` throw).
**Decision:** Wrap the per-iteration body of each remaining thread entry point in
`rs2v::Guard` (or, for `SamplingLoop`, add the missing `catch (...)` arm that
mirrors its existing handler). A throw from one bad packet/tick/callback is now
reported as a `NON-FATAL EXCEPTION` and the loop keeps serving, identical to the
game tick. Separately, catch blocks in three parsers that silently swallowed the
error (`CommandHandlers::ParseFloat`, `MapManager` lighting/ambient parse,
`ChatManager` `/votemap`) now log the offending input (gated at Debug/Warn).
**Rules out:** A thread entry point invoking subsystem work without a top-level
guard; relying on a single `catch (const std::exception&)` on a thread loop
(a non-`std::` throw would still terminate); catch blocks that discard the error
text on a parse failure.
**Consequences:** No single thread can take the process down via a recoverable
exception — the only fatal paths left are signals and throws that escape *all*
guards. The recovered-exception count (`NonFatalExceptionCount`) now also covers
these threads.

## 2026-07-15 — Bound ch2 reliability and possession recovery to explicit lifecycles
**Context:** Reliable ch2 actor RPCs used a scalar increment with no representation
of modulo wrap or unacknowledged values. The fixed owning-pawn channel graph could
also outlive one pawn life, so a delayed `AskForPawn` needed a stronger identity
than “the graph is open.”
**Decision:** Use a channel-agnostic `OutboundReliableSequencer` instance for the
explicit ch2 actor sequence space. It reserves contiguous batches atomically in
modulo 1024 (where sequence 0 is valid), permits at most 127 ordinary reliable
records from the oldest unresolved sequence through the cursor (including out-of-order ACKed
tombstones), commits a batch only after `pendingReliable` owns its retry, and releases values
only when an associated packet attempt is acknowledged. Retransmission keeps the
original ChSequence. Separately, bind owning-pawn graph publication, recovery,
and possession acknowledgement to the same nonzero pawn generation. Charge the
recovery count and interval only after the response is successfully queued.
**Rules out:** A monotonic scalar ch2 counter; treating wrapped sequence 0 as an
invalid sentinel; releasing a sequence after the first send attempt; consuming
recovery budget before queueing; or using only `pawnGraphOpen` to authorize a
response for a possibly stale pawn life.
**Consequences:** A full issuance window or rejected queue is transient
backpressure and cannot manufacture a sequence gap. Bootstrap must adopt its
externally assigned ch2-open sequence, ACK processing must release tracked ch2
values, and every pawn death/reset/travel/publication-failure path must invalidate
the recovery generation. This is an emulator-side safety decision, not a claim
about the retail server's internal allocator or pawn-lifecycle representation.
Because h42 is parameterless and h44 identifies only the reused ch209 channel,
generation binding cannot distinguish a prior-life message that arrives after a
new generation is already live; it deliberately guarantees lifecycle gating,
not per-message life correlation absent from the observed wire format.

## 2026-07-15 — Make outbound reliability transactional across both modular spaces
**Context:** ch0 publication could consume its reliable sequence before retry ownership,
payload-only bounds ignored packet headers and ACK framing, a full reliable window could
drop a required lifecycle message, retry ledgers used repeating raw 14-bit PacketIds, and
actor/retransmit paths could hand bytes to UDP before recording their attempt.
**Decision:** Prepare one complete, MaxPacket-bounded ch0 packet without publishing its
reserved sequence; insert the retry ledger by monotonic packet serial; then atomically
commit PacketId, the fitting queued-ACK prefix, and ChSequence before UDP handoff. Apply
the same full-packet bound and FIFO ACK-prefix fitting to every raw/ACK-only builder, and
record every reliable actor or retry attempt before its UDP handoff. Retain window-blocked
ch0 messages in a bounded FIFO, but preflight impossible payloads before queueing and hold
Join completion behind the earlier ch0 drain. Keep PacketIds monotonic internally, wrap
only the 14-bit wire value, and separate the ACK-unwrapping reference from the actual
peer-ACK high-water. Locally retire bunch-less identities only while all live retry ledgers
have a fresh attempt inside the half-range; stop bunch-bearing allocation before exact
8192-packet ambiguity. Publish capture f1484's owning ch2 OPEN and NMT 0x24 as one
prepared packet and one retry ledger rather than relying on cross-datagram UDP order.
**Rules out:** Dropping required ch0 work as ordinary backpressure; advancing ch0 state
before a retry owner exists; checking only `BunchDataBits`; matching ACKs by raw PacketId;
or allowing either modular allocator to lap an unresolved value.
**Consequences:** A failed initial or retry UDP send remains ledger-owned, queued ACK
suffixes remain ordered, same-raw ACKs cannot retire an older packet generation, and
bunch-less liveness cannot make an outstanding attempt ambiguous. Actor bootstrap cannot
overtake deferred ch0 work or expose NMT 0x24 without its owning-controller open.
Oversized messages, bounded-FIFO exhaustion, stale-attempt
half-range pressure, or transaction inconsistency fail the connection closed.

<!-- Append new decisions above this line, newest first. -->
