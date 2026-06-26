# UE3 / RS2:Vietnam Spawn → Possess → Control Flow (EngineVersion 7258)

The exact ordered server→client + client→server sequence that turns a frozen,
camera-locked retail client (loaded into the map, no usable PlayerController) into a
**controllable autonomous PlayerController**, and the separable **spectator / team-select
menu** state.

This is the *control-flow* companion to the bit-level wire specs:
- `docs/RS2V_ControlChannel_WireSpec_7258.md` — packet/bunch framing, NMT enum.
- `docs/RS2V_PostJoin_Replication_7258.md` — NMT_Welcome → PackageMap export → actor-open burst.
- `docs/RS2V_ActorReplication_7258.md` — bit-decoded actor opens, the ch2 PlayerController,
  ServerMove handle 664, the verbatim open-bunch templates.

This doc adds the **native (C++) + script (UnrealScript) mechanics** behind those wire bytes:
*why* the client adopts one replicated actor as its own autonomous PC, and the precise
ordered login→spawn→possess→ClientRestart→ServerMove chain.

**Sources (all primary, cross-checked):**
- **Native UE3 C++** — local clone `D:\RE-Tools\UE3-src\Development\Src\Engine\Src\` (a
  generic UE3 build; mechanics identical to 7258). Independently corroborated against the
  CodeRedModding/UnrealEngine3 mirror (build 10897) — **both agree line-for-line**.
- **Script (RS2 overrides + Engine base)** — decompiled UnrealScript at
  `D:\RE-Tools\rs2-source\` (`Engine\*.uc`, `ROGame\*.uc`).
- **Wire corroboration** — the two real-server captures decoded in the docs above
  (ch2 = local PC; first `ServerMove` byte-identical across two sessions).

Confidence tags: **[H]** = exact source excerpt (native or script) and/or bit-exact in
capture; **[M]** = strong inference from source + capture; **[L]** = plausible, not pinned.

---

## 0. TL;DR — the single fact that unfreezes the client

> The wire **never carries a literal "Role/RemoteRole swap" step, and `bNetOwner` is not
> what makes the client adopt its PC.** The owning client is identified **purely by
> `NetPlayerIndex == 0`** carried inside the PlayerController's *initial open bunch*. When
> the client opens an actor channel whose actor `IsA(APlayerController)`, it reads that
> `NetPlayerIndex`; if it is 0 it calls `UNetConnection::HandleClientPlayer(PC)`, which
> **sets `PC->Role = ROLE_AutonomousProxy`, binds it to the local viewport via
> `PC->SetPlayer(LocalPlayer)`, and stores `Connection->Actor = PC`.** That is the moment a
> replicated actor becomes "this client's own controllable PC." **[H]**

Everything else (the Role↔RemoteRole field swap, `bNetOwner`, `RemoteRole=AutonomousProxy`,
`ClientRestart`, `SetViewTarget`) is necessary supporting machinery, but the *adoption
selector* is `NetPlayerIndex==0` in the PC open bunch — **not** the channel index and **not**
a role bit alone.

Two separable milestones:
1. **Spectator / team-select menu** = PC adopted (above) + GRI + TeamInfo(s) + local PRI
   replicated. **No Pawn needed.** Client shows the menu; camera is a spectator cam.
2. **Controllable deployed player** = server `RestartPlayer` → `SpawnDefaultPawnFor` →
   `Possess` (sets `Pawn.RemoteRole=ROLE_AutonomousProxy`) → reliable **`ClientRestart(Pawn)`**
   on the PC channel → client `SetViewTarget(Pawn)` (camera unlocks) +
   `AcknowledgePossession` → client begins emitting `ServerMove`.

---

## 1. The ordered message / actor sequence (native + script, end to end)

`C→S` and `S→C` are control-channel NMT messages or actor-channel bunches. Native function
in `()`, script event/function in `[]`. The PackageMap export phase (between Welcome and
Join) is covered in `RS2V_PostJoin_Replication_7258.md` and elided as "…PackageMap…".

```
PHASE A — handshake & welcome  (server: UnWorld.cpp::NotifyControlMessage @4448)
 1. C→S NMT_Hello
 2. C→S NMT_Login        (URL + UniqueId)
        server: case NMT_Login @4917 → eventPreLogin → WelcomePlayer(Connection) @4960
 3. S→C NMT_Welcome      (WelcomePlayer @4145: sends Map + GameClass; bWelcomed=TRUE @4172)
 4. S→C …PackageMap export (NMT_Uses 0x07, the 321-package list)…
        client: UnPenLev.cpp::NotifyControlMessage — on NMT_Welcome marks connected,
        mirrors its own PackageMap, then SENDS NMT_Join when packages verified.

PHASE B — JOIN → server spawns the owning PlayerController
 5. C→S NMT_Join         (== the capture's "NMT 0x09" ready signal)
        server: case NMT_Join @4964 — iff (Connection->Actor==NULL && bWelcomed):
            PackageMap->Compute();
            Connection->Actor = SpawnPlayActor(Connection, ROLE_AutonomousProxy, …) @4992
        SpawnPlayActor (UnLevAct.cpp @926):
            APlayerController* PC = GetGameInfo()->eventLogin(Portal,Options,UniqueId,Error)
                                                        // script [GameInfo.Login], §2
            PC->NetPlayerIndex = InNetPlayerIndex;      // 0 for the primary connection
            PC->SetPlayer(Connection);                  // PC->Player = the UNetConnection
            PC->Role       = ROLE_Authority;
            PC->RemoteRole = ROLE_AutonomousProxy;      // <-- owning client's PC role
            GetGameInfo()->eventPostLogin(PC);          // script [GameInfo.PostLogin]
        → server now has a PC owned by this connection, in state 'PlayerWaiting' or
          'Spectating' (script Login chooses; §2). bDelayedStart ⇒ 'PlayerWaiting'.

PHASE C — actor-open burst (server replication tick, UnChan.cpp::ReplicateActor)
 6. S→C OPEN actor channel for the PlayerController FIRST (ch2 in capture).
        Initial open bunch (§3) carries: archetype/class NetGUID, compressed
        Location/Rotation, and **PC->NetPlayerIndex** (=0). The base-Actor initial
        property block carries RemoteRole/Role (WIRE-SWAPPED, §4) + bNetOwner + Owner.
 7. S→C OPEN GameReplicationInfo, TeamInfo(s), the local PlayerReplicationInfo, …
        (the menu-bootstrap set; see RS2V_ActorReplication_7258.md §6.5).

        client: UnChan.cpp::ReceivedBunch @1359 — on the PC open:
            Bunch << PC->NetPlayerIndex;                          // reads the 0
            if (NetPlayerIndex==0) Connection->HandleClientPlayer(PC);  // @1423
        HandleClientPlayer (UnConn.cpp @1650):
            PC->Role = ROLE_AutonomousProxy;  PC->SetPlayer(LocalPlayer);
            Connection->Actor = PC;            // <== CLIENT NOW OWNS THIS PC  [H]
        → MILESTONE 1: client has its local autonomous PC. Team-select / spectator
          menu can now bind to WorldInfo.GRI + GRI.Teams[] + this PC's PRI.

PHASE D — deploy: server spawns + possesses the Pawn
 8. C→S reliable RPC: player picks a role / presses fire to spawn:
        Engine path:  ServerRestartPlayer()   [PlayerController.uc state PlayerWaiting @7735]
        RS2 path:     SelectRoleByClass(...)   [ROPlayerController.uc @3790] → RestartPlayer()
 9. server: GameInfo.RestartPlayer(NewPlayer)  [GameInfo.uc @1262]:
            NewPlayer.Pawn = SpawnDefaultPawnFor(NewPlayer, StartSpot);   @1295
            NewPlayer.Possess(NewPlayer.Pawn, false);                     @1319
            NewPlayer.ClientSetRotation(Pawn.Rotation, true);             @1321
            AddDefaultInventory / SetPlayerDefaults                       @1325-1327
        Controller.Possess [Controller.uc @323]:
            inPawn.PossessedBy(self, false);  Pawn = inPawn;  Restart(...)
        Pawn.PossessedBy [Pawn.uc @1029] (RS2: ROPawn.PossessedBy @3537 → super(Pawn)):
            Controller = C;  NetPriority=3; bForceNetUpdate=true;
            if (C is PlayerController && NetMode != NM_Standalone)
                RemoteRole = ROLE_AutonomousProxy;       // Pawn.uc @1049  <-- the Pawn flip
10. S→C OPEN the Pawn's actor channel (ChType 2): initial bunch carries class NetGUID,
        compressed Location/Rotation, RemoteRole(=AutonomousProxy, wire-swapped),
        bNetOwner=true, PlayerReplicationInfo, Health, Controller (§3/§4).
11. S→C reliable **ClientRestart(Pawn)** RPC on the PC channel (ch2).
        ClientRestart [PlayerController.uc @4059; RS2 ROPlayerController.uc @1642]:
            Pawn = NewPawn;  AcknowledgePossession(Pawn);
            Pawn.ClientRestart();
            if (Role < ROLE_Authority) {           // i.e. on the client
                SetViewTarget(Pawn);   // <== CAMERA UNLOCKS
                ResetCameraMode();  EnterStartState();   // → PlayerWalking state
            }
        AcknowledgePossession [PlayerController.uc @1555] (client):
            AcknowledgedPawn = P;  ServerAcknowledgePossession(P);  // reliable C→S

PHASE E — control loop
12. C→S ServerMove RPC stream on the PC channel (ch2), function handle 664, one bunch
        per saved move at the client tick rate.   <== MILESTONE 2: CONTROLLABLE  [H]
13. S→C steady-state: pawn property deltas (SimulatedProxy pawns) + occasional reliable
        corrections (ClientAdjustPosition-class) on ch2.
```

---

## 2. Script side: `GameInfo.Login` decides spectator vs. waiting-to-deploy

`event PlayerController Login(...)` — `Engine/GameInfo.uc:1006`. Called natively by
`SpawnPlayActor` (`UnLevAct.cpp:939`, `eventLogin`). It **spawns the PlayerController**
(`SpawnPlayerController`, `:1081`), assigns PRI/PlayerID/UniqueId, then chooses the PC's
starting state — this is the menu-vs-deploy fork: **[H]**

- **Spectator** (`GameInfo.uc:1107-1115`): if `bSpectator` (URL `?SpectatorOnly=1`) or
  `PRI.bOnlySpectator` or `ChangeTeam` fails →
  `NewPlayer.GotoState('Spectating')`; sets `bOnlySpectator/bIsSpectator/bOutOfLives/
  bJoinedAsSpectator`. Returns. The client gets a free-look spectator PC, no pawn.
- **Delayed start** (`GameInfo.uc:1122-1126`): if `bDelayedStart` →
  `NewPlayer.GotoState('PlayerWaiting')`. This is the normal "team-select menu / waiting to
  deploy" state for a live match.
- Otherwise returns the PC in its default state (deploys via the match flow).

`SpawnPlayActor` finishes by forcing `Role=ROLE_Authority, RemoteRole=ROLE_AutonomousProxy`
on the returned PC (`UnLevAct.cpp:950-951`) regardless of state — the spectator PC is still
the connection's autonomous-proxy actor; it simply has no Pawn yet. **[H]**

### The state machine that gates deploy (script)

`Engine/PlayerController.uc`:
- `auto state PlayerWaiting extends BaseSpectating` (`:7724`) — the team-select/waiting
  state. `reliable server function ServerRestartPlayer()` (`:7735`): if
  `bWaitingToStartMatch` sets `PRI.bReadyToPlay=true`, else calls
  `WorldInfo.Game.RestartPlayer(self)`. `IsSpectating()` returns true here (`BaseSpectating`,
  `:7565`) → spectator camera. **[H]**
- `state Spectating extends BaseSpectating` (`:7677`) — pure spectator. **[H]**
- `state WaitingForPawn extends BaseSpectating` (`:7789`) — transient: the client has been
  told it will get a pawn but the Pawn actor channel/possession hasn't arrived yet.
  `ClientRestart` enters this state when `Pawn==None` (`:4074`). **[H]**
- `state Dead` (`:8014`) — `ServerRestartPlayer()` gated on `Game.PlayerCanRestart(self)`
  (respawn). **[H]**

RS2 deploy entry point: `ROPlayerController.SelectRoleByClass(...)` (reliable server,
`ROGame/ROPlayerController.uc:3790`) commits the chosen role on the PRI and, when in spawn
and the match is active, calls `RestartPlayer()` (`:3879`) — i.e. RS2's "deploy" button
routes to the same `GameInfo.RestartPlayer` → `Possess` → `ClientRestart` chain. Readiness is
the replicated `ROPlayerReplicationInfo.bReadyToPlay`. **[M]**

---

## 3. The PlayerController/​Pawn open bunch ("SerializeNewActor") — exact native layout

There is **no function literally named `SerializeNewActor`** in this UE3 build; the
initial-actor header is inlined in `UActorChannel::ReplicateActor`, in the
`if (Actor->bNetInitial && OpenedLocally)` block (`UnChan.cpp:1769-1798`). Wire order for a
transient (spawned) actor: **[H]**

```
1. Archetype/class object ref :  Bunch << Archetype;   // PackageMap NetGUID (§ wire spec)
2. Compressed Location, optional compressed Rotation:
       SerializeCompressedInitial(Bunch, Actor->Location, Actor->Rotation,
                                  Actor->bNetInitialRotation, this);   // UnChan.cpp:1790
3. (PlayerControllers ONLY)   Bunch << PC->NetPlayerIndex;             // UnChan.cpp:1796
4. then the bNetInitial replicated-property block (handles + values), §4.
```

`SerializeCompressedInitial` (`UnChan.cpp:15`): `Location.SerializeCompressed(Bunch); if
(bSerializeRotation) Rotation.SerializeCompressed(Bunch);`. **This resolves the open item in
`RS2V_ActorReplication_7258.md` §2.2/§6 — Location/Rotation are written INSIDE the open header
(compressed), NOT as ordinary `Location/Rotation` replicated properties.** That is why the PC
and Pawn initial blocks decode as "native-serialized compressed vectors" of variable,
non-byte-aligned width before the handle-indexed property region. **[H]**

The client mirror (`UnChan.cpp:1391-1417`): reads the class object, if it is a CDO/archetype
calls `SerializeCompressedInitial` + `GWorld->SpawnActor(...)` to instantiate the actor,
`SetChannelActor(InActor)`, then if `IsA(APlayerController)` reads `NetPlayerIndex`. **[H]**

> **Emulator note:** for the PC and Pawn opens, emit `[classGUID][actorGUID]` then the
> **compressed Location+Rotation**, then (PC only) a `NetPlayerIndex` byte/int = **0**, then
> the initial property block. The verbatim per-class templates in
> `RS2V_ActorReplication_7258.md §6` already encode these bits correctly; this section
> explains *what* those leading bits are.

---

## 4. Role / RemoteRole on the wire — the implicit field-index swap

The server does **not** byte-swap two values; instead, when `ReplicateActor` serializes the
`Role` and `RemoteRole` properties it **substitutes the opposite property's field-net-cache
index**, so on the wire the byte tagged "Role" actually carries the server's `RemoteRole`
value and vice-versa (`UnChan.cpp:1940-1946`): **[H]**

```cpp
FFieldNetCache* FieldCache
=   It->GetFName()==NAME_Role        ? ClassCache->GetFromField(Connection->Driver->RemoteRoleProperty)
:   It->GetFName()==NAME_RemoteRole  ? ClassCache->GetFromField(Connection->Driver->RoleProperty)
:                                      ClassCache->GetFromField(It);
```

Net effect for the owning client's PC/Pawn: the server holds `Role=ROLE_Authority(3),
RemoteRole=ROLE_AutonomousProxy(2)`; the client receives `Role=ROLE_AutonomousProxy(2),
RemoteRole=ROLE_Authority(3)`. That is why `ClientRestart`'s `if (Role < ROLE_Authority)`
(`PlayerController.uc:4079`) is true on the client and false on the server. **[H]**

### `bNetOwner` (server) — who owns the actor

`UnChan.cpp:1743-1756`. `bNetOwner` is true for this connection iff the actor's top
PlayerController's `Player` *is this connection*: **[H]**

```cpp
Actor->bNetOwner = 0;
APlayerController* Top = Actor->GetTopPlayerController();
UPlayer* Player = Top ? Top->Player : NULL;
...
Actor->bNetOwner = Connection->Driver->ServerConnection
                 ? Cast<ULocalPlayer>(Player)!=NULL          // client side
                 : Player==Connection;                       // SERVER side: own this conn?
```

`SpawnPlayActor` set `PC->SetPlayer(Connection)` (`UnLevAct.cpp:948`), so `Top->Player ==
Connection` ⇒ `bNetOwner=true` for the PC and everything it owns (its Pawn, its PRI) on its
own connection, and `false` on every other connection. **[H]**

### RemoteRole downgrade for non-owners

`UnChan.cpp:1801-1806` — an `ROLE_AutonomousProxy` actor is **downgraded to
`ROLE_SimulatedProxy`** before sending to any connection that is *not* its owner (so only the
owning client sees its pawn as autonomous; everyone else sees it as a simulated proxy).
Restored after the send at `:2020`. **[H]**

```cpp
BYTE ActualRemoteRole=Actor->RemoteRole;
if (Actor->RemoteRole==ROLE_AutonomousProxy &&
    (((Actor->Instigator==NULL || !Actor->Instigator->bNetOwner) && !Actor->bNetOwner) || ...))
    Actor->RemoteRole=ROLE_SimulatedProxy;
```

### Base-Actor replication conditions (script) — where the role bits live

`Engine/Actor.uc:549-552` (decompiler `// Pos:0x2BA`), condition `bNetInitial &&
Role==ROLE_Authority`: **[H]**

```unrealscript
RemoteRole, Role, bNetOwner, bTearOff;   // initial-only role block
// Pos:0x37A — Owner, gated additionally on bNetOwner:
Owner;
```

`ENetRole` values (`Actor.uc:54-60`): `ROLE_None=0, ROLE_SimulatedProxy=1,
ROLE_AutonomousProxy=2, ROLE_Authority=3`. **[H]**

---

## 5. Client adoption — the definitive native chain (the "frozen → mine" flip)

`UActorChannel::ReceivedBunch` (`UnChan.cpp:1359`), on a freshly-opened actor channel whose
actor is a PlayerController (`:1413-1424`): **[H]**

```cpp
APlayerController* PC = Actor->GetAPlayerController();
if (PC != NULL) {
    Bunch << PC->NetPlayerIndex;                       // read from the open bunch (§3)
    if (Connection == Connection->Driver->ServerConnection) {   // we are the client
        if (PC->NetPlayerIndex == 0)
            Connection->HandleClientPlayer(PC);         // <-- adopt as OUR PC
        else { /* split-screen child connection */ }
    }
}
```

`UNetConnection::HandleClientPlayer` (`UnConn.cpp:1650-1690`): **[H]**

```cpp
ULocalPlayer* LocalPlayer = /* first local player of the GameEngine */;
if (LocalPlayer->Actor) {                              // detach the placeholder PC
    if (LocalPlayer->Actor->Role == ROLE_Authority)    //   the temp local PC
        GWorld->DestroyActor(LocalPlayer->Actor);
    else
        FNetControlMessage<NMT_PCSwap>::Send(this, Index);  // tell server swap done
    LocalPlayer->Actor->Player = NULL;  LocalPlayer->Actor = NULL;
}
LocalPlayer->CurrentNetSpeed = CurrentNetSpeed;
PC->Role = ROLE_AutonomousProxy;          // force autonomous on the client
PC->SetPlayer(LocalPlayer);               // LocalPlayer->Actor = PC; PC->Player = LocalPlayer
State = USOCK_Open;
Actor = PC;                               // Connection->Actor = the adopted PC
```

`SetPlayer` is UE3's "switch controller" wiring: it cross-links `LocalPlayer->Actor = PC` and
`PC->Player = LocalPlayer`, so the local viewport/camera/input is now driven by this PC. After
this, the client's input produces `ServerMove` RPCs on the PC's channel. **[H]**

> **This is the answer to "what makes our client adopt one actor as its local autonomous
> PC":** the client only adopts the PlayerController whose open bunch carries
> `NetPlayerIndex==0`. Our emulator therefore MUST write a `NetPlayerIndex` of 0 into the PC
> open bunch (after compressed Location/Rotation, before the property block, §3). bNetOwner
> and the swapped Role are still required for correct downstream behavior, but they are *not*
> the selector — `NetPlayerIndex` is. **[H]**

---

## 6. Channel index for the PlayerController

The **control channel is index 0** (opened in the connection ctor). Actor channels are
dynamic and assigned the **first free index** by the connection's channel allocator
(`UNetConnection::CreateChannel`); the PC is simply the first actor channel opened for the
connection, so it lands at a low index (the captures show **ch2** in one session, **ch6** in
another — the index is *not* fixed). Channels are **bidirectional**: the same `ChIndex`
carries S→C property/RPC bunches and C→S `ServerMove`/`ServerAcknowledgePossession` RPCs.
The client matches its PC by `NetPlayerIndex==0`, **not** by channel number. **[H native
mechanism; H bidirectional/low-index, from captures + ReceivedBunch]**

> Capture note: `RS2V_ActorReplication_7258.md` (session :57867) shows the PC at **ch2** and
> the first `ServerMove` on ch2; `RS2V_PostJoin_Replication_7258.md` (the loopback capture)
> shows it at **ch6**. Both are correct — the index is whatever the allocator hands out;
> emulator should not hardcode it, only ensure the PC channel is opened (early) and the
> client's `NetPlayerIndex==0` selector is present. **[H]**

---

## 7. Spectator / menu state vs. deployed state — the wire difference

| | Spectator / team-select menu (MILESTONE 1) | Deployed / controllable (MILESTONE 2) |
|---|---|---|
| Server PC state | `Spectating` / `PlayerWaiting` (`GameInfo.Login` §2) | `PlayerWalking` (via `EnterStartState`) |
| Pawn | none | spawned + possessed (`RemoteRole=AutonomousProxy`) |
| Actor channels needed | PC + GRI + TeamInfo(s) + local PRI | + the Pawn channel |
| Camera | spectator cam (`SetViewTarget(self)` / PRI) | `SetViewTarget(Pawn)` via `ClientRestart` |
| Client→server | menu RPCs (`ServerChangeTeam`, `SelectRoleByClass`, `ServerRestartPlayer`) | `ServerMove` stream (handle 664) |
| Trigger to advance | client picks role / fires → `RestartPlayer` | — |

Key: the **menu does not need a Pawn**. As soon as the PC is adopted (NetPlayerIndex==0) and
GRI + TeamInfos + the local PRI are replicated, `WorldInfo.GRI` / `GRI.Teams[]` resolve and
the team-select UI binds. The frozen-camera "stuck in map" symptom = PC adopted but **no
`ClientRestart(Pawn)`** yet (or no Pawn channel), so `SetViewTarget` was never called on a
pawn. **[M from script + capture]**

---

## 8. Emulator implementation checklist (control-handshake subsystem)

Priority order; each item cites the mechanism above. Phases A and the PackageMap export are
already covered by the other docs.

1. **On `NMT_Login`** → run login, send `NMT_Welcome` + PackageMap, set `bWelcomed`.
   (`UnWorld.cpp:4917,4145,4172`.) **[H]**
2. **On `NMT_Join`** (capture's NMT 0x09), iff no PC yet and welcomed:
   spawn the connection's PlayerController, set `Role=ROLE_Authority`,
   `RemoteRole=ROLE_AutonomousProxy`, `Player=connection`, `NetPlayerIndex=0`,
   `Connection->Actor=PC`. (`UnWorld.cpp:4964`, `UnLevAct.cpp:926`.) **[H]**
3. **Open the PC actor channel FIRST.** Open bunch (§3): classGUID, actorGUID, compressed
   Location/Rotation, **`NetPlayerIndex=0`**, then the bNetInitial property block with the
   base-Actor role group **wire-swapped** (so client reads `Role=AutonomousProxy(2),
   RemoteRole=Authority(3)`) and `bNetOwner=true`. Without the `NetPlayerIndex==0` the client
   will NOT adopt it. (`UnChan.cpp:1769,1796,1940`; client `:1413`/`UnConn.cpp:1650`.) **[H]**
4. **Open GRI + TeamInfo(s) + the local PRI** → MILESTONE 1 (menu). (See
   `RS2V_ActorReplication_7258.md §6.5` for the verbatim templates.) **[H ordering]**
5. **On the deploy RPC** (`ServerRestartPlayer` / RS2 `SelectRoleByClass` → `RestartPlayer`):
   server-side spawn a Pawn, `Possess` it → `Pawn.RemoteRole=ROLE_AutonomousProxy`
   (`GameInfo.uc:1262`, `Pawn.uc:1029`). **[H]**
6. **Open the Pawn actor channel** (compressed Location/Rotation in the header; role group
   wire-swapped to AutonomousProxy; `bNetOwner`, `PlayerReplicationInfo`, `Health`,
   `Controller`). **[H mechanism]**
7. **Send reliable `ClientRestart(Pawn)` on the PC channel** → client `SetViewTarget(Pawn)`
   (camera unlocks) + `AcknowledgePossession` → MILESTONE 2. (`PlayerController.uc:4059`.)
   **[M wire / H script]**
8. **Accept the `ServerMove` stream** on the PC channel (function handle 664) and run movement
   server-side; send `ClientAdjustPosition`-class corrections only on error.
   (See `RS2V_ActorReplication_7258.md §4`.) **[H handle / M correction path]**

---

## 9. Open items / lower confidence

- **[RESOLVED H]** Location/Rotation are in the open header (compressed), not as ordinary
  replicated properties — `SerializeCompressedInitial` (`UnChan.cpp:15,1790`). Closes the §2.2
  open item in `RS2V_ActorReplication_7258.md`.
- **[RESOLVED H]** Client adoption selector = `NetPlayerIndex==0` in the PC open bunch, not
  channel index / not bNetOwner alone (`UnChan.cpp:1413` → `UnConn.cpp:1689`).
- **[M]** Exact wire bit position of `NetPlayerIndex` within the captured PC open bunch (it is
  written after compressed Location/Rotation; the capture decode treats that whole region as
  "native compressed vectors" — `NetPlayerIndex` is the trailing int of that region). The
  verbatim PC template already carries the correct value (0), so this does not block
  emulation; pin it bit-exact when authoring novel PC opens.
- **[M]** Whether RS2 ever sends `ClientRestart` for the *spectator* PC with a non-pawn view
  target before deploy (some flows call `ClientSetViewTarget` directly). The deployed-pawn
  path above is the one proven by the `ServerMove`-on-ch2 milestone.
- **[L]** Native float/compressed encoding inside `ServerMove` params — see
  `RS2V_ActorReplication_7258.md §4.2` (structure settled, exact widths pending
  `execServerMove` disasm).
```
