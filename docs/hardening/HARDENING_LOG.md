# RS2V Server Hardening Log

Autonomous defensive-hardening pass (self-paced /loop). Each iteration audits one subsystem
for missing null/empty/0/-1/out-of-range/invalid-data checks, unchecked indexing, integer
over/underflow, unbounded loops/allocations on attacker-controlled input, lifetime issues, and
unvalidated wire lengths — then adds NON-FATAL invariant checks + structured logging (log +
safely reject; never crash). Additive only — must NOT change working netcode behavior
(handshake, bootstrap, reliable retransmission, team-menu/TeamInfo replication, ClassNetCache
handles). Server stays LIVE on 7777 via the watchdog.

Baseline already hardened by the RE+harden fleet (commit re bit-exactness invariants):
src/Network/BitWriter, BitReader, PacketCodec, ActorReplication, WireTrace.h.

## Subsystem queue
- [x] Network inbound parse: HandshakeState, ControlReassembler, ControlChannel, ConnectionManager inbound path
- [x] Protocol decoders (src/Protocol/*)
- [x] Security / AuthManager / input validation
- [x] Game state / SpawnSystem / GameMode / ReplicationManager
- [x] Config / ConfigManager
- [x] Utils (StringUtils, PathUtils, MemoryPool, etc.)
- [x] Network: UDPSocket / NetworkManager / BandwidthManager / ClientConnection
- [x] Fuzz the packet/bunch decoders + inbound parse (41 tests, all pass)
- [ ] (overflow) static-analysis pass (cppcheck/clang-tidy), deeper tests for Protocol/Game/Security, doc gaps

## Iteration log
<!-- one line per completed iteration: date | subsystem | what was hardened | commit -->
2026-06-26 | Network inbound parse | NMT-range + login-URL caps (HandshakeState); reassembly byte-cap 256KiB + seq-ahead/payloadBits guards (ControlReassembler); ValidBuffer/StringSane caps (ControlChannel); datagram clamp + checked find() + per-packet bunch cap 4096 (ConnectionManager inbound) | 2d7714d
2026-06-26 | Protocol decoders | BytesRemaining guards + bool-return + reserve clamps (Actor/PropertyReplication); null-handler reject (ProtocolHandler); arg-count clamp (MessageEncoder); hex validation (ProtocolUtils); payload/field bounds (ProtocolDecoder) | 5dcbd39
2026-06-26 | Security/Auth | anti-enumeration timing equalizer + no auto-insert (AuthManager); constant-time verify + param guards (PasswordHasher); bounded token/session maps + empty-token reject (TokenManager/EAC) | 18b81b0
2026-06-26 | Game state | null/bounds guards + chained-deref fix (GameServer/GameMode/SpawnSystem); chat caps (ChatManager/GameServer); uniform_real_distribution UB fix (MapManager); checked find() in dirty loop (ReplicationManager) | fb8520b
2026-06-26 | Config + Utils | URL oversize/option caps (URLOptions, join path); safe numeric coercion (ConfigManager); path-traversal reject (PathUtils); distribution UB fixes (RandomGenerator); Split/ToDouble guards (StringUtils); pool null-bail + worker clamp (MemoryPool/ThreadPool) | 07592a5
2026-06-26 | Network transport | UDPSocket buffer clamps; BandwidthManager client-cap + overflow-safe (bit-identical verdict); ClientConnection guards (CanSend fails-open); NetworkManager/NetworkThread null + anti-busy-spin | 1bb6922
2026-06-26 | Fuzz/tests | 41 malformed-input fuzz tests (BitReader/PacketCodec/ControlReassembler/HandshakeState), ~1.3M calls, all pass, 0 crashes - hardening validated | 0a2f390
2026-06-26 | Static analysis | cppcheck pass (11 findings, mostly non-bugs); fixed uninit Sha256::m_buf; AuthenticationTests 32/32 pass | 02ee824
