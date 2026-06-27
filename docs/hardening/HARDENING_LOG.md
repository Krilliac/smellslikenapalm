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
2026-06-26 | Netcode correctness review | FOUND+FIXED real client-breaking bugs: UE3 per-param Send presence bit (SelectTeam decode dropped team-0; ChangedTeams misaligned) + kMaxChannels 1023->1024. TODO: control-channel retransmit (edge-case) | 396bc58
2026-06-26 | Property-encoding review | fixed 3 latent ActorRepl-helper bugs (enum-byte width, appRound, None self-check) before spawn wiring; live path verified correct | fc01168
2026-06-26 | Player-lifecycle review | FIXED 4 real bugs: SelectTeam not persisted server-side (team->spawn BLOCKER), spectator force-respawn, stale TeamManager on disconnect, NVA->US mapping. TODO: wire Team/Player/GameTests | 0d65060
2026-06-26 | Consolidation | docs/ARCHITECTURE.md + docs/NETCODE.md (incl Send-bit) + docs/hardening/SUMMARY.md; game tests un-wireable (stale APIs, annotated-excluded) | 6b1a6e3
2026-06-26 | Team->role advance (netcode) | FIXED the team-select crash: send ChangedTeams (h172) with GameTypeClass=ROGameInfoTerritories static ref 69601 (client crashed on default-none param -> empty squads -> null RoleClass deref). Client binds PRI.Team itself (3618); squads build locally. 3-agent RE + UE3-src param-layout validation + round-trip test. | b916a8e
2026-06-26 | KNOWN ISSUE (build) | src/Network/ActorReplication.cpp and src/Protocol/ActorReplication.cpp collide on obj name ActorReplication.obj in rs2v_core's flat intermediate dir -> only one's symbols survive in rs2v_core.lib (Protocol wins; ReplicationManager needs it). Harmless today (no rs2v_core code referenced Network/'s ActorRepl until now; worked around by inlining). FIX LATER: give CMake per-source unique obj names or rename one file. | (open)
2026-06-26 | Pkt-dispatch security review (fleet) | 3-agent review+verify (wf_fff418dc-46f): 6/15 confirmed attacker-reachable. FIXED the HIGH one: wire-driven OOB read up to 4GB in legacy Packet readers (DecodeString/ReadUInt/ReadFloat/ReadBytes lacked bounds checks) -> additive guards mirroring ReadUInt16/32/64 + NetworkPacketSafetyTests (5). 16/16 green, mock react PASS. | 067ecf6
2026-06-26 | OPEN findings (from wf_fff418dc-46f) for later ticks: (1) no auth/connection-state gate on string-tag dispatch — pre-login client can drive gameplay handlers (GameServer::Run ~448-488; needs IsLoggedIn gate, behavioral so verify carefully); (2) legacy FromBuffer path reachable by any non-UE3 datagram (ConnectionManager.cpp:1039-1043) — the reachability bridge; (3) HandleVehicleAction trusts attacker heliId (no pilotId/seat check, GameServer.cpp:937-1002, griefing); (4) HandleWeaponFire trusts attacker victimId (GameServer.cpp:1040-1078, no attacker-alive/range/LOS check); (5) AdminManager BanPlayer std::stoi(args[1]) unguarded throw (admin-only, not wire-reachable) + privileged subops don't re-check IsAdmin (currently only HandleAdminCommand-reachable, safe today). | (open)
2026-06-26 | Vehicle-action ownership (finding #3) | FIXED: HandleVehicleAction Start/Stop engine (cases 3/4) trusted attacker heliId -> any client could start/stop any heli's engine (griefing). Added requesterPilotsHeli() ownership gate mirroring the case-2 control path. Additive. 16/16 green, mock react PASS. | af0d970

## Game-logic validation pass (vs decompiled RS2 UnrealScript at D:\RE-Tools\rs2-source)
Fleet wf_19008ffa-b00 (4 dims x review+verify, 24 agents): **15/20 confirmed-real** discrepancies where our C++ game
logic diverges from the source in a way that produces wrong gameplay. Applying highest-confidence additive/correctness
fixes one per tick. Full result: tasks/w9luafrwc.output. Confirmed-real backlog (✔=fixed):
- ✔ Objective capture froze whenever ANY defender was present (ObjectiveSystem) - source advances the dominant force
- ✔ HIGH Hitzone scaling applied TWICE (headshot ~100x, limbs compounded) - Weapon/WeaponDatabase + DamageSystem
- ✔ HIGH Limb damage flat *0.4/0.5 vs source per-zone ZoneHealth cap (hand/foot=10, forearm/calf=20, thigh=35)
- ✔ HIGH Respawn not blocked when reinforcements depleted (PlayerManager) - added the gate (death already debits)
- ✔ HIGH Auto-respawn ignores ready-to-deploy state (force-deploys dead players) (PlayerManager) - added ready-gate wired to deploy RPC
- [ ] HIGH Supremacy win model: two 250 pools drain-to-0 vs source single signed TotalMapScore +/-TargetScore(50)
- [ ] (more in tasks/w9luafrwc.output - linked-objective HQ graph, etc.)

2026-06-26 | Capture contest logic (game-logic finding) | FIXED: ObjectiveSystem::ProcessCapture froze ALL capture progress whenever both teams had a capper in the zone (one defender stalls any attacking force). Source ROGameInfoTerritories.CaptureTimer compares TeamCapValue[0] vs [1] - the greater force keeps capturing ('>'=attackers advance, '<'=defenders regain, '=='=standoff). Rewrote contested branch to advance/regain by net force (diminishing returns), only a true tie stalls. Additive (capper-count approximation; squad/leader bonus inputs not tracked at this layer). Build green, mock react PASS. | 5c591fd
2026-06-26 | Hit-zone double-scaling (game-logic finding) | FIXED: hit zone was scaled TWICE - WeaponDatabase::CalculateDamage multiplied by ballistics.headshot/limbMultiplier AND DamageSystem::CalculateFinalDamage multiplied by GetHitZoneMultiplier on the same hit (headshots ~10x*10x=~100x, limbs ~0.4x*0.4x). Source applies the zone effect once in ROPawn.TakeDamage (weapon yields base damage). Removed the zone block from the weapon layer; DamageSystem (full HitZone) is now the single zone-application point. Explosions (Chest, bypass CalculateDamage) unaffected. Build green, mock react PASS. | 2f9f387
2026-06-26 | Reinforcement-depletion respawn gate (game-logic finding) | FIXED: PlayerManager::Update auto-respawned any dead teamed player on the timer with NO check on reinforcement tickets - respawns were effectively infinite even after a team bled out. Source ROGameInfo.PlayerShouldRespawn returns false when Team.ReinforcementsRemaining<=0. Added the gate (skip respawn when ts->GetInitialTickets(team)>0 && !ts->HasTickets(team)); the per-death cost is already debited via TicketSystem::OnPlayerKilled so NO per-spawn decrement was added (would double-count). Teams with no ticket pool are never gated (preserves unlimited modes). Build green, mock react PASS. | 4e2a23a
2026-06-26 | Hit-zone damage CAP model (game-logic finding) | FIXED: hit zone scaled damage by a flat multiplier (arms*0.5, legs*0.4), so a 300-dmg rocket / 95-dmg sniper to a limb still dealt dozens of points unbounded. Source ROPawn.TakeDamage (8245-8252) CAPS each hit at the zone's ZoneHealth (Min(dmg,ZoneHealth)) and forces instant-death zones lethal (Max(dmg,HealthMax)). Replaced GetHitZoneMultiplier with ApplyHitZoneEffect: head=instant death, torso/abdomen cap 100, arm cap 30 (upper-arm), leg cap 35 (thigh); coarse HitZone enum maps each limb to its proximal sub-zone. Also reordered friendly-fire scale BEFORE the zone clamp (matches source AdjustDamage-then-clamp). Build green, mock react PASS. | bc2d34f

## Pawn-spawn autonomous validation (tooling)
2026-06-26 | mock_client.py `spawn` mode | NEW: drives the FULL menu->spawn path as a UE3 client (handshake -> Join -> SelectTeam(170) -> SelectRoleByClass(175)) and verifies the server reacts by opening the pawn channel. Lets us validate the spawn SEND path server-side WITHOUT the retail client. RESULT: PASS - server opens ch209 (verbatim 1137-bit ROPawn open, NMT 0x86) + back-refs h52(Controller->ch2)/h32(PRI->ch26) + ch2 possession RPCs h24(PC.Pawn)/h85(ClientRestart) in response to the role RPC, with 0 standalone ack-only datagrams (ack-storm fix holds; acks piggyback). Proves the pawn-spawn flow + ack coalescing are correct server-side; real-client possession still pending user test. Extended encode_packet with bit-level (non-byte-aligned) RPC payloads + sint_bits() helper. react still PASS (no regression). | 1e6a55f

## Game-logic validation pass (continued)
2026-06-26 | Auto-respawn ready-gate (game-logic finding) | FIXED: PlayerManager::Update force-respawned ANY dead+teamed player on the timer, yanking players still sitting in the team/role menu into the world. Source ROGameInfo.PlayerShouldRespawn requires ROPlayerController.IsReadyToSpawn() / SpawnReadyStatus==Ready (only DEPLOYED players spawn). Added Player::m_readyToSpawn (default false) + Is/SetReadyToSpawn; gated the respawn loop on IsReadyToSpawn(); set ready=true on DEPLOY in BOTH the netcode path (ConnectionManager handle-175 SelectRoleByClass) and the game-layer GameServer::HandleRoleSelection. Null-guarded (mock client has no game-layer Player). Touches netcode -> validated with BOTH react PASS and spawn PASS. | 0be2bf8

## Security hardening (OPEN findings from wf_fff418dc-46f)
2026-06-26 | Pre-auth gameplay gate (OPEN finding #1) | FIXED: the legacy string-tag dispatch (GameServer::Run) ran the state-changing gameplay handlers (ROLE_SELECT/SPAWN_REQUEST/COMMANDER_ABILITY/SQUAD_ACTION/VEHICLE_ACTION/WEAPON_FIRE + the GameMode action fallthrough) with NO auth/connection-state check, so any client that never logged in could drive gameplay pre-auth (this path is reachable by any non-UE3 datagram). Added a gate: every tag except CHAT_MESSAGE now requires conn->IsHandshakeComplete(). Real UE3 clients never use this legacy path (their gameplay rides the UE3 control channel via DecodeInboundActorBunch), so no impact; CHAT stays allowed (already length-capped). Validated react PASS + spawn PASS. | (this commit)
