# Pawn Spawn & Possession — Replication Ground Truth (RS2V 7258)

Reverse-engineered from the real client↔real server capture
`D:\RE-Tools\rs2_realserver_capture.pcapng` (S2C filter `udp.srcport==7777 && udp.dstport==57867`,
C2S `udp.srcport==57867 && udp.dstport==7777`). Bunches decoded with
`tools/mock_client.py decode_packet(..., bd_max=12000)` for S2C / `16384` for C2S. Open class refs
decoded as `UPackageMap::SerializeObject` = `[1 selector bit][static: SerializeInt(0x80000000) |
dynamic: SerializeInt(1023)]` (see `docs/UE3_ActorChannel.md §3`).

This is groundwork for emulating the **player-pawn spawn + possess** step (the step after
team/role selection).

---

## 0. TL;DR

- The **initial world burst** (existing players, GRI, teams, their pawns) opens on **ch2..ch153 at
  frames f1484..f1549** (t103.8..t106.0), right after Join (C2S NMT 0x09 at f1477).
- **Every later channel open is a spawn/respawn event or a transient** (projectile/effect). A
  **pawn spawn = one ROPawn-family channel + a small burst of Inventory/Weapon item channels, all
  opened in the SAME frame and all sharing the SAME compressed Location** (the items are attached to
  the pawn so they replicate the pawn's spawn point). Examples below: f18972, f19885, f22367, f27394.
- The **local player's first pawn possession is ≈ t146 (f≈4266)**: that is when the client's C2S
  ch2 traffic switches from tiny menu/team-select RPCs (~52 bits) to steady ~31/s ServerMove
  movement bunches (~205 bits). Team/role selection therefore completed between Join (t103) and t146.
- **Pawn class** = `ROPawn` (chain `Object→Actor→Pawn→GamePawn→ROPawn`), **net handle space
  maxHandle = 170**. **Weapon class** = `ROWeapon` (`Object→Actor→Inventory→Weapon→ROWeapon`),
  **maxHandle = 99**.
- **CORRECTION to MEMORY note**: on `ROPlayerController` (ch2) the Pawn property is **handle 24**,
  not 23. **Handle 23 = `PlayerReplicationInfo`, handle 24 = `Pawn`** (`Controller.Pawn`, ni=1861).

---

## 1. Open timeline — first-open frame per channel (S2C)

4111 total open bunches across the capture; 219 distinct channel indices ever opened (channels are
**reused** as actors die/respawn, so 219 ≠ live actor count). Initial world burst then sparse
respawns:

| Frames | t (s) | Channels | What |
|--------|-------|----------|------|
| f1484–f1489 | 103.78 | ch2..ch58 | PC (ch2), other-player pawns (286147/286151…), PRIs (86701), GRI (ch54=70887), TeamInfo (ch21/56=90245) |
| f1489–f1549 | 103.8–106.0 | ch59..ch153 | rest of the world burst (more PRIs/pawns/inventory/static actors) |
| f1579–f1684 | 106.5–108.0 | ch154..ch167 | trickle (more actors becoming relevant) |
| f3110–f3449 | 131.8–136.0 | ch168..ch176 | first respawns (pawns 286147/286151 + items) |
| **f4266** | **146.6** | (reused chans) | **local player's first possession begins (C2S ServerMove onset)** |
| f6705, f18972, f19885, f22367, f27394, … | 195–553+ | bursts of 7–11 chans | **respawn clusters** (pawn + inventory, shared Location) |

The **post-burst NEW opens are the pawn/weapon spawns** the task asked to find. They are NOT a
single contiguous block — they are scattered respawn clusters spread across the whole 21-minute
match, one cluster per (re)spawn of any player.

---

## 2. The spawn-cluster pattern (the key finding)

Decoding the open headers (`classref` + `SerializeCompressedInitial` Location) for a single frame
shows the pawn and its inventory share one Location. Two real clusters at **f27394 (t553)**:

```
 ch209  cls=286151  loc=(-8981, 7936,-517)   <- PAWN (ROPawn-family, 763 bits)
 ch210  cls=286374  loc=(-8981, 7936,-517)   <- inventory/ammo item
 ch211  cls=286391  loc=(-8981, 7936,-517)   <- inventory item
 ch212  cls=286464  loc=(-8981, 7936,-517)   <- inventory item
 ch213  cls=286109  loc=(-8981, 7936,-517)   <- inventory item
 ch214  cls=286389  loc=(-8981, 7936,-517)   <- inventory item
 ch219  cls=82735   loc=(-8981, 7936,-517)   <- weapon (ROWeapon-family)
 --- a SECOND pawn in the same packet, different spawn point ---
 ch218  cls=286038  loc=(-92793,6804,-416)   <- PAWN (1383 bits)
 ch215  cls=286095  loc=(-92793,6804,-416)
 ch216  cls=75939   loc=(-92793,6804,-416)
 ch217  cls=82735   loc=(-92793,6804,-408)   <- weapon
```

`f19885 (t439)` and `f18972 (t427)` show the same shape: a ~1000-bit ROPawn channel plus 5–7
~100–400-bit Inventory/Weapon channels at one Location. **Reproduce with**
`scratchpad/decode_cluster.py` (loads a frame window, prints `classidx`, compressed Location).

So to **emulate a player spawn** the server must, in one tick, open:
1. one **ROPawn** actor channel (class ref = the pawn class, Location = spawn point), then
2. one **ROWeapon** + N **Inventory** actor channels at the **same Location**, with the pawn's
   `InvManager`/inventory linked-list and each item's `Owner`/`Instigator`/`Inventory` refs pointing
   back (dynamic channel-index refs) at the pawn channel.

---

## 3. Class-index inventory (static PackageMap object indices)

Static index = `package.ObjectBase + Object->NetIndex` (deterministic; see
`docs/UE3_NetGUID_PackageMap.md §0`). Resolving an index → exact class **name** needs the cooked
package linker export tables (not in `data/packagemap_export_7258.bin`, which only exports *package*
names via NMT_Uses). Classes are therefore identified by the **known four** plus **empirical family
classification** (open size + co-location + decode against a class's maxHandle):

| idx | opens | avg bits | class (confidence) |
|-----|------:|---------:|--------------------|
| 57520 | 1 | 925 | `ROPlayerController` (PC, ch2) — **known** |
| 70887 | 1 | 4662 | `ROGameReplicationInfo` (GRI, ch54) — **known** |
| 86701 | 92 | 729 | `ROPlayerReplicationInfo` (PRI) — **known** |
| 90245 | 3 | 317 | `ROTeamInfo` (ch21/56/76) — **known** |
| **286147** | **841** | **1186** | **ROPawn family — dominant soldier pawn** (most common respawn) |
| **286151** | 516 | 1086 | **ROPawn family** (pawn) |
| **286186** | 52 | 964 | **ROPawn family** (pawn) |
| **286038** | 36 | 1409 | **ROPawn family** (heavier pawn) |
| **285996 / 285994 / 286184** | 13/11/13 | ~1100–1260 | **ROPawn family** (pawns) |
| 82735 | 26 | 209 | **ROWeapon/Inventory family** (co-located weapon) |
| 75939 | 22 | 198 | Inventory/Weapon family |
| 286374 / 286391 / 286464 / 286109 / 286389 / 286097 / 286095 | 4–15 each | ~100–280 | **Inventory/ammo items** (co-located with pawns) |
| 60168 | 1383 | 179 | high-rate transient (projectile/tracer/FX), first seen f20874 |
| 68586 | 274 | 378 | transient (grenade/projectile) |
| 60852 / 60303 / 60059 / 59648 / 60264 … | many | 150–230 | transients / effects / minor actors |

The pawn-class indices all live in the tight band **285994..286464** (cooked ROGame export
neighborhood) — consistent with ROPawn subclasses (per-faction / per-role soldier classes) and their
inventory living next to each other in the cooked package. Pinning each exact name is a separate
linker-export task; not required to generate the spawn.

---

## 4. ROPawn replicated-field handle layout (maxHandle = 170)

From `tools/netfields_from_u.ps1 -Class ROPawn` (UELib over the compiled `.u`; chain
`Object→Actor(23)→Pawn(33)→GamePawn(3)→ROPawn(111)`, FieldsBase Actor=0/Pawn=23/GamePawn=56/ROPawn=59).
Full dump: `tools/netfields_u_ROPawn.txt`. **Handles needed to wire a spawned pawn:**

| handle | property | type | role in spawn |
|-------:|----------|------|---------------|
| 4 | `Instigator` | object ref | usually = self/owner |
| 6 | `Owner` | object ref | actor owner |
| 8 | `Role` | byte (enum) | set on wire (swapped: owner sees AutonomousProxy) |
| 9 | `RemoteRole` | byte (enum) | **AutonomousProxy(2) to the owning client, SimulatedProxy(1) to others** |
| 12 / 13 | `Rotation` / `Location` | (initial sent in the open header, not here) | |
| 14 | `bNetOwner` | bool | true on the owner's copy |
| 27 | `InvManager` | object ref | → the pawn's InventoryManager channel |
| 32 | `PlayerReplicationInfo` | object ref | → the owning player's **PRI channel** |
| 52 | `Controller` | object ref | → the possessing **PC channel (ch2 for the local player)** |

ROPawn-specific replicated state (handles 97–167) covers stance/cover/lean/health-zones/weapon
attachment (e.g. 147 `CurrentWeaponAttachmentClass`, 137 `HitZoneHealths`, 33 `Health`,
40 `FiringMode`, 41 `FlashCount`). These are why a pawn open is ~1000+ bits.

**Identifying the LOCAL player's pawn** (next step, not yet pinned to a channel): it is the ROPawn
channel whose **handle 52 (`Controller`) = a dynamic ref to ch2** and **handle 32
(`PlayerReplicationInfo`) = the local PRI**, replicated with `RemoteRole=AutonomousProxy` +
`bNetOwner=1`. Equivalently it is the channel ch2's **handle 24 (`Pawn`)** points at.

## 4b. ROWeapon handle layout (maxHandle = 99)

From `tools/netfields_from_u.ps1 -Class ROWeapon` (`Object→Actor(23)→Inventory(2)→Weapon→ROWeapon`;
`tools/netfields_u_ROWeapon.txt`). Key: **23 `InvManager`** (obj ref → pawn's inv manager),
**24 `Inventory`** (obj ref → next item in the pawn's inventory linked list), and ammo state
89 `TotalStoredAmmoCount` / 91 `CurrentMagCount` / 92 `AmmoCount`.

---

## 5. Open-bunch wire layout (recap, validated against capture)

Each opening actor channel (`bControl=1,bOpen=1,bClose=0,bReliable=1,ChType=2`, ascending ChIndex,
ChSeq from 1) carries, in order (`docs/UE3_ActorChannel.md §1`):

```
[ class ref     ]  SerializeObject static path: 1 sel bit(=0) + SerializeInt(idx, 0x80000000)
[ Location      ]  FVector::SerializeCompressed: SerializeInt(Bits,20); X,Y,Z = SerializeInt(1<<(Bits+2))-(1<<(Bits+1))
[ Rotation      ]  FRotator::SerializeCompressed (only if class bNetInitialRotation): per axis 1 bit + (byte<<8)
[ NetPlayerIndex]  ONLY if the actor is a PlayerController (ch2: =0 for the main local PC)
[ property stream] SerializeInt(handle, maxHandle) + typed value, repeated until bits exhausted
```

Verified: ch2 open (f1484) decodes `classidx=57520`, Location `(-831,4085,292)`. The exact
Rotation/NetPlayerIndex/first-property bit boundary is still slightly fuzzy in our decoder (the
property-stream value sizes per handle aren't yet typed) — full ch2 property-stream typing is the
follow-up that will pin handle 24 (`Pawn`) to the local pawn channel.

---

## 6. The possess control flow (server-side, source-anchored)

From `D:\RE-Tools\rs2-source` + `docs/UE3_SpawnControlFlow.md`. RPC handles below are
`ROPlayerController` net handles (from `tools/netfields_u_global.txt`, maxHandle = 531):

1. Player finishes team+role select → server `GameInfo` calls **`ServerRestartPlayer`** (handle 27)
   path → `RestartPlayer(PC)` → `SpawnDefaultPawnFor` spawns the **ROPawn** at a PlayerStart →
   `PC.Possess(pawn)`. This is when the **pawn actor channel opens** (the cluster in §2).
2. `Controller.Possess` sets `Controller.Pawn = pawn` and `pawn.Controller = PC`; these replicate as
   ch2 **handle 24 (`Pawn`)** and pawn **handle 52 (`Controller`)** (cross-refs by channel index).
3. Server sends **`ClientRestart`** (handle **85**, `ClientRestart(Pawn NewPawn)`) on **ch2** — the
   client RPC that makes the client accept the pawn, set its ViewTarget, and leave the menu/spawn
   state. Related client RPCs in the flow: **`ClientOnPossess`** (handle 150),
   **`ClientSetHUD`** (45), **`AskForPawn`/`GivePawn`** (42/43, the request/answer if the client's
   pawn ref is stale), **`ServerAcknowledgePossession`** (44, C2S ack).
4. Once possessed, the client streams **ServerMove** (a PlayerController C2S function on ch2) every
   tick — this is the t146+ traffic in §0/§7 and the definitive "pawn is possessed" signal.

---

## 7. Local-possession timeline (C2S evidence)

`scratchpad/c2s_ch2.py` bins ch2 C2S bunches by 30 s:

```
t 90 : 633 bunches, avg  52 bits   <- pre-spawn: tiny menu/team-select/handshake RPCs
t120 :1432 bunches, avg  69 bits   <- still mostly menu (team/role selection window)
t150 : 932 bunches, avg 205 bits   <- ServerMove movement stream begins => PAWN POSSESSED
t180+:  ~950/30s,   avg ~190 bits  <- steady ~31 Hz ServerMove for the rest of the life
```

First ch2 C2S bunch at f1522/t105.4 (164 bits, a menu RPC). First **>250-bit** ch2 C2S bunch at
**f4266 / t146.6** (`payload 400a13a108b558…`) — the first ServerMove-sized move = the moment the
local player's pawn is possessed and controllable. So **team/role selection occupied t103→~t146**,
and the **first local pawn spawn cluster is in that window** (the f3110–f3449 respawn opens at
t131–136 are the nearest NEW pawn opens; the local pawn may also reuse an existing low channel
index — confirm via ch2 handle 24).

---

## 8. Reproduce / tooling

- `scratchpad/scan_opens.py` — every actor-channel first-open (frame, ChIndex, decoded class idx).
- `scratchpad/classidx.py` — class-index histogram (count / avg bits / first frame / sample chans).
- `scratchpad/decode_cluster.py` — decode a frame window's open headers (class ref + compressed
  Location) to see spawn clusters.
- `scratchpad/c2s_ch2.py` — C2S ch2 timeline (possession onset).
- `tools/netfields_from_u.ps1 -Class ROPawn|ROWeapon` — handle layouts (now wired into the script;
  outputs `tools/netfields_u_ROPawn.txt`, `tools/netfields_u_ROWeapon.txt`).

## 9. Open follow-ups

1. **Pin the local pawn channel**: type the ROPlayerController property stream and read ch2 handle
   24 (`Pawn`) over time → exact local-pawn channel index at each life.
2. **Name the pawn/inventory class indices** (285994–286464, 82735, 75939, 286097/109/374/389/391/464)
   via the cooked ROGame linker export table → exact ROPawn subclass + weapon/ammo class names.
3. **Decode one full pawn open** property-by-property (typed) to get the exact initial-property set +
   role-flag bytes the client expects, for the generator.
