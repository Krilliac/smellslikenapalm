# ARCHITECTURE — System Overview

A from-scratch C++23 server emulator for **Rising Storm 2: Vietnam** (Unreal
Engine 3, `EngineVersion 7258`). The goal is to let the *retail* client connect,
complete the UE3 control-channel handshake, bootstrap the world, and reach the
team-select menu. This document is the map for a new engineer: what the
subsystems are, how a packet flows end-to-end, and how the process is threaded.

> For the bit-exact wire formats this code targets, see the RE docs (ground
> truth, not invented): [`docs/re/MASTER_replication_reference.md`](re/MASTER_replication_reference.md),
> [`docs/RS2V_ControlChannel_WireSpec_7258.md`](RS2V_ControlChannel_WireSpec_7258.md),
> [`docs/RS2V_PostJoin_Replication_7258.md`](RS2V_PostJoin_Replication_7258.md),
> [`docs/re/open_bunch_structure.md`](re/open_bunch_structure.md).

---

## 1 · Subsystem map

Source lives under `src/`. Each directory is one subsystem.

| Subsystem | Dir | Responsibility | Key files |
|-----------|-----|----------------|-----------|
| **Network** | `src/Network` | UDP I/O, UE3 packet/bunch **framing**, per-connection handshake + reliability, actor-channel bootstrap. This is where the live client connection lives. | `ConnectionManager`, `PacketCodec`, `BitReader`/`BitWriter`, `HandshakeState`, `ControlChannel`, `PacketAssembler`, `ControlReassembler`, `NetworkManager`, `UDPSocket` |
| **Protocol** | `src/Protocol` | Higher-level message/RPC/replication layer above framing: property replication, RPC dispatch, compression, UE3 protocol glue. | `ReplicationManager`, `RPCHandler`, `PropertyReplication`, `ActorReplication`, `UE3Protocol`, `MessageEncoder`/`Decoder`, `CompressionHandler` |
| **Game** | `src/Game` | Game state and gameplay rules: players, teams, roles, spawns, maps, rounds, modes, scoring. Hosts the **login bridge** that turns a connected socket into a spawned player. | `GameServer`, `ConnectionLoginBridge`, `PlayerManager`, `TeamManager`, `RoleSystem`, `SpawnSystem`, `MapManager`, `GameMode`, `ReplicationInfo.h` (PRI/GRI/TeamInfo) |
| **Security** | `src/Security` | Auth (stubbed Steam), bans, EAC server **emulation** (the retail client expects EAC), token/password handling. | `EACServerEmulator`, `SecurityManager`, `BanManager`, `AuthManager`, `SteamAuth` |
| **Config** | `src/Config` | INI parsing + typed config wrappers loaded at startup. | `ConfigManager`, `INIParser`, `ServerConfig`, `NetworkConfig`, `SecurityConfig`, `GameConfig`, `MapConfig` |
| **Utils** | `src/Utils` | Cross-cutting helpers: logging, crash handler, threadpool, memory pool, crypto, string/file/path. | `Logger`, `CrashHandler`, `ThreadPool`, `MemoryPool`, `CryptoUtils` |

Supporting dirs: **Time** (`GameClock`, the fixed-timestep driver; tick/latency
managers), **Physics** (`PhysicsEngine`, movement/input validators, anti-cheat),
**Math** (`Vector3`), **Scripting** (C# host — **disabled**, does not build; see
`CMakeLists.txt`), and top-level `telemetry/` (metrics sampling + reporters).

---

## 2 · End-to-end pipeline

What happens from a UDP datagram to a player standing in the world. Each arrow
cites the function that does the work.

```
 retail client (UDP :7777)
        │  datagram
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ NETWORK LAYER (src/Network)                                                │
│                                                                            │
│  ConnectionManager::PumpNetwork()        drain ≤256 datagrams/pump,        │
│    └─ CreateOrGetClient()                bandwidth-gate, map addr→clientId │
│    └─ HandleIncomingPacket()                                               │
│         └─ ParseIncomingControl()        PacketCodec::Decode → bunches,    │
│              │                           ack the packet, feed reliable     │
│              │                           bunches to ControlReassembler     │
│              ▼                                                             │
│         HandshakeState::HandleControlMessage()   per-connection state m/c  │
│           StatelessConnect (0x1d→0x20)                                     │
│             → Hello → Challenge → Login → Welcome → Join                   │
│           fires ClientLoggedIn / ClientJoined  (observer callbacks)        │
└───────────────────────────────┬────────────────────────────────────────────┘
            ClientLoggedIn / ClientJoined (no Network→Game compile dependency)
                                ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ GAME LAYER (src/Game)                                                       │
│                                                                            │
│  ConnectionLoginBridge::OnClientLoggedIn()                                 │
│     PreLogin gate → Login → make PlayerReplicationInfo, ensure GRI         │
│  ConnectionLoginBridge::OnClientJoined()                                   │
│     PostLogin → PickTeam → SpawnSystem spawn → mark active                 │
└───────────────────────────────┬────────────────────────────────────────────┘
                                ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ WORLD BOOTSTRAP + MENU (back in ConnectionManager, post-Join)             │
│                                                                            │
│  SendReplicationBootstrap()  PackageMap export (NetGUID/package list)      │
│  SendActorBootstrap()        open actor channels: ROGameReplicationInfo,   │
│                              ROTeamInfo, local ROPlayerController, PRIs     │
│  SendCh2Rpc(ClientShowTeamSelect)   → client renders team-select menu      │
│                                                                            │
│  inbound: DecodeInboundActorBunch() → SelectTeam RPC                       │
│            → ClientShowRoleSelect → role → SPAWN_REQUEST                   │
└──────────────────────────────────────────────────────────────────────────┘
```

**Framing detail (Network).** `PacketCodec` decodes/encodes the
`<PacketId><acks><bunches><terminator>` UE3 wire structure using LSB-first
`BitReader`/`BitWriter` (FBitReader/FBitWriter-compatible). `MaxPacket` is
direction-dependent from the first packet: 1280 for inbound C2S decode and 1500
for our outbound S2C encode — the exact bounds matter bit-for-bit
(`src/Network/PacketCodec.h` documents why). Outbound framing (monotonic internal
PacketId assignment with 14-bit wire projection and ch0's 127-record reliable
issuance window) is `PacketAssembler`; every outbound builder performs full-packet
size validation and queued-ACK prefix packing, while its transactional ch0 path
also reserves and commits the reliable sequence atomically. Inbound
ordering/dedup of reliable control bunches is `ControlReassembler`, which mirrors
UE3's strict per-channel modulo-1024 `InReliable[ChIndex]` cursor and never skips a
missing reliable based on later arrivals. Reliable
bunches are retransmitted until acked — `ConnectionManager::SendReliableBunches`
records them, `OnClientAck` clears them, `RetransmitTick` (run every pump) resends.
Outbound ch0 and explicit reliable ch2 actor sequences each own an
`OutboundReliableSequencer` per connection/channel. It reserves modulo-1024 batches
(including sequence 0), caps the ordinary forward issuance window at UE3's 127
reliable records, and commits a
tokenized reservation only after `pendingReliable` owns the retry. Out-of-order
ACKed successors remain window tombstones until the oldest gap closes. ACK
processing first unwraps each raw PacketId against a monotonic reference, then
releases exact full-serial ledger matches; the actual peer-ACK high-water remains
separate. Bunch-less packet identities are locally retired because UE3 does not ACK
them, but only while every live retry ledger retains a fresh attempt inside the
14-bit half-range. Bunch-bearing allocation stops before the exact ambiguous half.
Retransmission reuses the original ChSequence with a new packet serial and records
that attempt before UDP handoff.

**Decoupling.** The Network layer never `#include`s anything from Game. The
handshake notifies Game purely through `std::function` observers
(`ClientLoggedInEvent` / `ClientJoinedEvent` in `HandshakeState.h`), wired in
`GameServer::Initialize` to the `ConnectionLoginBridge`. The bridge reaches back
into Network only through a connection-resolver callback, so it is unit-testable
without a socket.

---

## 3 · Threading model

The server is, in practice, **single-threaded for game + network**. There is no
separate network thread in the live path.

```
main() ─ src/main.cpp
  │  InstallCrashHandler, InitializeLogging, SocketFactory::Initialize (WSAStartup)
  │  GameServer::Initialize()  → ConfigManager, NetworkManager(→ConnectionManager,
  │                              binds UDP), all Game subsystems, ProtocolHandler,
  │                              ReplicationManager, ConnectionLoginBridge
  │  EACServerEmulator::Initialize(port 7957)
  │  TelemetryManager::Initialize + StartSampling
  │
  └─ GameClock::RunLoop()   fixed timestep @ tickRate (default 60 Hz)
        every tick → callback:
          GameServer::Run()                       ← all game + network work here
            ├ NetworkManager::PollNetwork()
            │    └ ConnectionManager::PumpNetwork()  recv, decode, handshake,
            │                                        RetransmitTick
            │    └ RemoveStaleConnections, BandwidthManager::Update
            ├ FetchPendingPackets() → dispatch by tag (CHAT/ROLE_SELECT/SPAWN_…)
            ├ tick subsystems (GameMode, PlayerManager, ReplicationManager,
            │   TicketSystem, ObjectiveSystem, SpawnSystem, DamageSystem,
            │   ProjectileManager, HelicopterPhysics, active GameMode)
            └ NetworkManager::Flush()
          EACServerEmulator::ProcessRequests()
```

Auxiliary threads (not the game loop):
- **Telemetry sampling thread** — `TelemetryManager::StartSampling` (default 1 Hz).
- **Optional handler auto-regen thread** — `GameServer::m_regenThread`, off by
  default (`StartAutoRegen`).
- **EAC emulator** — may service requests on its own listener; the loop also calls
  `ProcessRequests()` each tick for the non-threaded path.

> **Note — `NetworkThread` is currently dead code.** `src/Network/NetworkThread.{h,cpp}`
> implements an alternative model (its own thread calling `PumpNetwork` + a tick
> callback at a set rate), but nothing instantiates it — the live server drives
> `PumpNetwork` synchronously from `GameServer::Run` via `GameClock`. It is a
> candidate to either wire up (move network I/O off the game tick) or remove.

Concurrency is therefore minimal: the per-connection state (`m_clients`,
`m_handshakes`, `m_controlState` in `ConnectionManager`) is touched only from the
game tick. The cross-thread seams are the packet queue
(`GameServer::m_packetQueue`, mutex-guarded) and telemetry.

---

## 4 · Client-connection lifecycle

One `ClientConnection` per remote address (`ConnectionManager::CreateOrGetClient`
allocates a `clientId`). Its progression, with the owning state:

| Phase | Where the state lives | What happens |
|-------|----------------------|--------------|
| **StatelessConnect** | `HandshakeState` (`m_controlHandshakeComplete`) | UE3 cookie handshake `0x1d→0x1e→0x1f→0x20`; completion changes message semantics while the direction-specific MaxPacket remains constant. |
| **Hello → Challenge** | `HandshakePhase::ChallengeSent` | Client `NMT_Hello` (version, SteamId, rate, URL); server emits Challenge nonce. Steam auth is **stubbed** (accepted blindly). |
| **Login → Welcome** | `HandshakePhase::WelcomeSent` | `NMT_Login` parsed → `ClientLoggedIn` fires → `ConnectionLoginBridge` runs PreLogin + Login, creates the PRI and (lazily) the single GRI. |
| **Join** | `HandshakePhase::Joined` | `NMT_Join` → `ClientJoined` fires → bridge runs PostLogin (team pick + spawn). |
| **World bootstrap** | `ConnectionManager::ControlState` (per-client) | `SendReplicationBootstrap` (PackageMap) then `SendActorBootstrap` open the bootstrap actor channels; ch2 carries `ClientShowTeamSelect`. |
| **Team / role / spawn** | `ControlState` (`teamSelected`, `ch2Reliable`, owning-pawn generation fields) | Inbound `SelectTeam` (`DecodeInboundActorBunch`) persists the team and advances to `ClientShowRoleSelect`; role selection leads to a spawn request. Possession recovery is eligible only for the exact live generation represented by the open owning-pawn graph. |
| **Teardown** | — | `RemoveStaleConnections` (heartbeat timeout) or explicit disconnect drops the `ClientConnection`, handshake, and control state. |

Reliability spans the whole post-Join phase: every reliable server→client bunch
is retransmitted (same per-channel `ChSequence`, new `PacketId`) until the client
acks the packet it rode in — without this, a single dropped bootstrap bunch
soft-locks the client. A full ch0 or ch2 reliable window is transient backpressure: a
new atomic reservation is deferred without consuming a sequence or a
possession-recovery response. See `ConnectionManager::ControlState::SentReliable`
and `PacketCodec::OutboundReliableSequencer`. Required ch0 messages are retained in
a bounded per-connection FIFO and published in order when contiguous ACK progress
reopens capacity; exhaustion or an oversized complete packet fails closed.
`NMT_Join` completion is also held behind that drain barrier: only then may the
connection become handshake-complete, publish the same-packet ch2 OPEN/NMT 0x24
entry transaction and the actor cohort, or invoke the Game callback. This prevents
deferred ch0 lifecycle work from being overtaken and prevents UDP loss from exposing
NMT 0x24 without the owning PlayerController open.

---

## 5 · Where to start reading

- **The connection** — `src/Network/ConnectionManager.{h,cpp}` (handshake driver,
  bootstrap, reliability) and `src/Network/HandshakeState.{h,cpp}` (state machine).
- **The framing** — `src/Network/PacketCodec.{h,cpp}` + `BitReader`/`BitWriter`.
- **Game entry** — `src/Game/GameServer.cpp` (`Initialize`, `Run`) and
  `src/Game/ConnectionLoginBridge.{h,cpp}` (login → spawn).
- **The loop** — `src/main.cpp` + `src/Time/GameClock`.
- **What the wire must look like** — the RE docs linked at the top.

*This document describes the code as it actually is. When the live path changes
(e.g. if `NetworkThread` is wired in, or Steam/EAC stops being stubbed), update
this file.*
