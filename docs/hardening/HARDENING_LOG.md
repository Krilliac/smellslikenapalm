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
- [ ] Security / AuthManager / input validation
- [ ] Game state / SpawnSystem / GameMode / ReplicationManager
- [ ] Config / ConfigManager
- [ ] Utils (StringUtils, PathUtils, MemoryPool, etc.)
- [ ] Network: UDPSocket / NetworkManager / BandwidthManager / ClientConnection
- [ ] (overflow) deepen tests, fuzz packet/bunch decoders, fill doc gaps

## Iteration log
<!-- one line per completed iteration: date | subsystem | what was hardened | commit -->
2026-06-26 | Network inbound parse | NMT-range + login-URL caps (HandshakeState); reassembly byte-cap 256KiB + seq-ahead/payloadBits guards (ControlReassembler); ValidBuffer/StringSane caps (ControlChannel); datagram clamp + checked find() + per-packet bunch cap 4096 (ConnectionManager inbound) | 2d7714d
2026-06-26 | Protocol decoders | BytesRemaining guards + bool-return + reserve clamps (Actor/PropertyReplication); null-handler reject (ProtocolHandler); arg-count clamp (MessageEncoder); hex validation (ProtocolUtils); payload/field bounds (ProtocolDecoder) | 5dcbd39
