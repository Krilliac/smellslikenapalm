# RS2V Server Hardening Campaign — Executive Summary

A 12-iteration autonomous hardening + review loop run against the from-scratch RS2V
(UE3 7258) server emulator. Goal: make the inbound/attacker-facing surface survive
arbitrary wire input **without changing the working netcode** (handshake, bootstrap,
reliable retransmission, team-menu/TeamInfo replication, ClassNetCache handles), then
have correctness reviews catch real client-breaking bugs the live capture hadn't.

Two distinct workstreams ran in the same loop:
1. **Defensive hardening** — additive, non-fatal invariant guards (log + safely reject,
   never crash) across six subsystems, validated by fuzzing + static analysis.
2. **Correctness review** — three focused reviews against the RE ground-truth that found
   and fixed **real bugs** blocking the team->role->spawn progression.

Full detail: [HARDENING_LOG.md](HARDENING_LOG.md), [FUZZ_TESTS.md](FUZZ_TESTS.md),
[STATIC_ANALYSIS.md](STATIC_ANALYSIS.md).

---

## 1. What was hardened (6 subsystems)

Each pass audited one subsystem for the same classes of defect — missing
null/empty/0/-1/out-of-range checks, unchecked indexing, integer over/underflow,
unbounded loops/allocations on attacker-controlled input, lifetime issues, and
unvalidated wire lengths — and added non-fatal invariant checks with structured logging.
Additive only; baseline bit-exactness invariants (BitWriter/BitReader/PacketCodec/
ActorReplication) were already in place.

| # | Subsystem | Guard classes added | Commit |
|---|-----------|---------------------|--------|
| 1 | **Network inbound parse** (HandshakeState, ControlReassembler, ControlChannel, ConnectionManager) | NMT-range + login-URL caps; reassembly 256 KiB byte-cap + seq-ahead/payloadBits guards; ValidBuffer/StringSane caps; datagram clamp + checked `find()` + per-packet bunch cap 4096 | `2d7714d` |
| 2 | **Protocol decoders** (src/Protocol/*) | BytesRemaining guards + bool-return + reserve clamps; null-handler reject; arg-count clamp; hex validation; payload/field bounds | `5dcbd39` |
| 3 | **Security / Auth** (AuthManager, PasswordHasher, TokenManager/EAC) | anti-enumeration timing equalizer + no auto-insert; constant-time verify + param guards; bounded token/session maps + empty-token reject | `18b81b0` |
| 4 | **Game state** (GameServer, GameMode, SpawnSystem, ChatManager, MapManager, ReplicationManager) | null/bounds guards + chained-deref fix; chat caps; `uniform_real_distribution` UB fix; checked `find()` in dirty loop | `fb8520b` |
| 5 | **Config + Utils** (ConfigManager, URLOptions, PathUtils, StringUtils, RandomGenerator, MemoryPool/ThreadPool) | URL oversize/option caps; safe numeric coercion; path-traversal reject; distribution UB fixes; Split/ToDouble guards; pool null-bail + worker clamp | `07592a5` |
| 6 | **Network transport** (UDPSocket, BandwidthManager, ClientConnection, NetworkManager/NetworkThread) | socket buffer clamps; client-cap + overflow-safe bandwidth (bit-identical verdict); fail-open CanSend guards; null guards + anti-busy-spin | `1bb6922` |

---

## 2. Fuzz + static-analysis validation

**Fuzzing — 41 tests, ~1.3M inputs, 0 crashes** (`0a2f390`). GoogleTest suites with a
per-loop watchdog (a hang fails hard) and a fixed seed (failures reproduce exactly),
hammering the attacker-facing decoders with random / mutated / boundary / lying-length
input, and asserting valid input still round-trips byte- and field-exactly:

| Suite | Tests | Under test | Approx. iterations |
|-------|------:|-----------|--------------------|
| BitReaderFuzzTests | 20 | BitReader / BitWriter bounds contract | ~9k loops + sweeps |
| PacketCodecFuzzTests | 6 | UE3 packet+bunch framing | ~170k |
| ControlReassemblerFuzzTests | 7 | inbound reassembler DoS caps | ~400k |
| HandshakeStateFuzzTests | 8 | handshake state machine | ~700k |

No crash, no out-of-bounds, no hang, no unbounded allocation across all ~1.3M calls.
Critical OOB invariant asserted on every decode: `payload.size()*8 >= payloadBits`
(a consumer copying `payloadBits` can't run past the buffer). **No production fix
required** — the existing non-fatal hardening held against every input.

> One documented intentional semantic (not a bug): `PacketCodec::Decode` is UE3-lenient —
> it returns `ok==true` for any datagram with a readable 14-bit PacketId. A stricter
> reject policy would be a production design decision, not a decoder-safety defect.

**Static analysis — cppcheck 2.20.0** over Network/Protocol/Security/Game/Config/Utils
(`02ee824`). Only 11 findings, all dispositioned: 1 fixed (`Sha256::m_buf` uninitialized —
defense-in-depth, was written-before-read), the rest NOT-A-BUG (idiomatic sockaddr casts,
intentional float->bytes map serialization) or minor non-security perf. No memory-safety,
OOB, overflow, leak, or use-after-free findings — consistent with the fuzz results.

---

## 3. Real bugs found + fixed (the high-value outcome)

The correctness reviews — not the fuzzers — found the bugs that actually blocked the
client. These were live-path defects invisible to crash-fuzzing because they produce
*wrong but well-formed* wire data.

### Iteration 9 — Netcode correctness review (`396bc58`)
- **UE3 per-param "Send" presence bit** — the function-RPC param framing omitted UE3's
  per-parameter Send-presence bit. `SelectTeam` decode dropped **team 0** and
  `ChangedTeams` was bit-misaligned — this was the blocker stopping team-select from
  advancing to role-select. Fixed the framing to match UE3.
- **`kMaxChannels` 1023 -> 1024** — off-by-one channel ceiling.

### Iteration 10 — Property-encoding review (`fc01168`)
Fixed **3 latent ActorReplication-helper bugs** before they could corrupt the spawn path:
enum-byte width, `appRound` rounding, and a `None`/self-check bug in the property-value
codec. The live menu path was verified still correct.

### Iteration 11 — Player-lifecycle review (`0d65060`)
Fixed **4 real bugs** in team assignment / lifecycle:
- **`SelectTeam` not persisted server-side** — the team->spawn **BLOCKER**: the picked team
  was replicated to the client but never committed on the server, so spawn couldn't proceed.
- spectator force-respawn,
- stale `TeamManager` entry on disconnect,
- NVA->US team mapping error.

(Earlier in the same campaign window, `02ee824` also fixed the uninitialized SHA-256 buffer
noted above.)

---

## 4. Remaining known gaps

- **Control-channel retransmission** — an edge-case path in reliable retransmit is still
  unhardened/untested (flagged at `396bc58`). The general intermittent reliable-channel
  soft-lock was fixed earlier (`166f892`), but the control-channel retransmit edge case
  remains.
- **Unwired tests** — the new Team / Player / Game correctness tests authored during the
  lifecycle review (`0d65060`) are not yet registered/wired into the test build.
- **role -> spawn path** — the team->role progression is now unblocked, but the
  role-selection -> actual pawn spawn path is still to be implemented end-to-end. The
  ActorReplication property-codec helpers were pre-validated (`fc01168`) in anticipation
  of this wiring.

---

*Net result:* six subsystems defensively hardened and proven (41 fuzz tests / ~1.3M inputs
/ 0 crashes, clean cppcheck), and — more importantly — the team-select -> role-select
progression unblocked by fixing four genuine client-breaking bugs (the UE3 Send-bit, the
server-side `SelectTeam` persist, `kMaxChannels`, and the property-codec/lifecycle bugs)
that fuzzing alone would never have surfaced.
