# UE3 `UActorChannel` Open / Close + `SerializeNewActor` — exact algorithm (EngineVersion 7258 context)

Goal of this doc: explain, field-by-field, the **send** and **receive** algorithm for opening an
actor channel that the **retail RS2:Vietnam client accepts**, and the **exact conditions** under
which that client rejects / closes / disconnects an actor channel — so our emulator can avoid them.

Primary source: the leaked UE3 engine tree now checked out locally at
`D:\RE-Tools\UE3-src` (CodeRedModding/UnrealEngine3). All file/line citations below are from that
tree unless prefixed `VNGame.exe`. Cross-checked against our own RE docs
(`docs/RS2V_ActorReplication_7258.md`, `docs/RS2V_ControlChannel_WireSpec_7258.md`) and the real
capture `D:\RE-Tools\rs2_realserver_capture.pcapng`.

Confidence: **[H]** = read directly from engine source AND consistent with our capture/disasm;
**[M]** = source-clear but a known build delta exists between this generic UE3 and VNGame.exe 7258;
**[L]** = inference.

---

## 0. TL;DR — why the client closes our channels

The client runs **two** independent gates on every actor-open bunch we send. Failing either is
fatal:

1. **Channel-accept gate** — `UWorld::NotifyAcceptingChannel`
   (`UnWorld.cpp:3813`). For a client (`Driver->ServerConnection != NULL`) this **accepts any
   `CHTYPE_Actor` (== 2) channel and refuses every other type** (`UnWorld.cpp:3824-3835`). If we
   ever send an open with `ChType != 2`, the client immediately **sends a `bClose` bunch back and
   tears the channel down** (`UnConn.cpp:1169-1185`). **[H]**

2. **SerializeNewActor gate** — `UActorChannel::ReceivedBunch`
   (`UnChan.cpp:1359`). The very first thing read from an opening actor bunch is an **object
   reference (the class/archetype) via `UPackageMap::SerializeObject`**. If that reference does
   **not resolve to a `UClass`/archetype that `Cast<AActor>` accepts**, the client does
   `Broken = 1; FNetControlMessage<NMT_ActorChannelFailure>::Send(Connection, ChIndex); return;`
   (`UnChan.cpp:1381-1390`). **[H]**

The `NMT_ActorChannelFailure` then comes back to **us (the server)**, and our handling decides the
disconnect: `UnChan.cpp:782-805` — **if the failed channel's actor is the connection's
PlayerController (`Connection->Actor`), the server closes the entire connection**
(`Connection->Close()`, line 797). That is precisely the observed symptom: *every replayed actor
channel except the client's own PlayerController is rejected, then the connection drops.* It means
**our class NetGUIDs are not resolving on the client** — the archetype reference we serialize does
not map, through the client's PackageMap, to a real `AActor` subclass CDO/archetype. The PC channel
"survives" only because it is the connection's own actor and is handled specially; the failures on
the other channels feed back as `NMT_ActorChannelFailure` and, because at least one of them is (or
maps onto) the PC, the connection is closed.

So the fix target is **the object/class reference codec and the index it points at**, not the
bunch framing (framing is already correct per the WireSpec).

---

## 1. Receive-side algorithm — `UActorChannel::ReceivedBunch` (`UnChan.cpp:1359-1689`) [H]

This is what the retail client executes on each actor bunch we send. Reproduced as the exact
control flow (not verbatim, but every branch and order preserved):

```
ReceivedBunch(Bunch):
  check(!Closing)                                   # line 1361
  if (Broken || bTornOff) return                    # 1363 — dead channel, silently drop

  if (Actor == NULL):                               # 1371 — first bunch on this channel
      if (!Bunch.bOpen):                            # 1373
          # "New actor channel received non-open packet" -> just return (no close)
          return                                    # 1376
      # ---- READ THE CLASS / ARCHETYPE REFERENCE ----
      UObject* Object;
      Bunch << Object                               # 1381 -> UPackageMap::SerializeObject (§3)
      AActor* InActor = Cast<AActor>(Object)         # 1382
      if (InActor == NULL):                          # 1383  <=== THE REJECT
          debugf("Received invalid actor class on channel %i", ChIndex)
          Broken = 1                                 # 1387
          NMT_ActorChannelFailure::Send(Conn,ChIndex)# 1388  <=== client tells server
          return                                     # 1389
      if (InActor->HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject)):  # 1391 transient actor
          FVector Location; FRotator Rotation(0,0,0);
          SerializeCompressedInitial(Bunch, Location, Rotation,
                                     InActor->bNetInitialRotation, NULL)     # 1396 (§4)
          InActor = GWorld->SpawnActor(InActor->GetClass(), NAME_None,
                                       Location, Rotation, InActor, ...)     # 1398
          # if spawn fails -> only a debugf; then check(InActor) (assert in debug)  1400-1408
      SetChannelActor(InActor)                       # 1411
      if (Actor is a PlayerController):              # 1414
          Bunch << PC->NetPlayerIndex                # 1417  (read AFTER Location/Rotation)
          if (this is the client's ServerConnection):
              if (NetPlayerIndex == 0) Connection->HandleClientPlayer(PC)   # 1423 main local PC
              else  ... split-screen child connection ...                    # 1427-1439
  else:
      # subsequent bunch on an already-open channel — no header, straight to property/RPC loop

  ClassCache = PackageMap->GetClassNetCache(ActorClass)   # 1448 — per-class field table
  # set bNetOwner from whether the top PlayerController's Player is our LocalPlayer   1452-1474

  # ---- PROPERTY / RPC STREAM ----                       1477+
  RepIndex   = Bunch.ReadInt( ClassCache->GetMaxIndex() ) # handle, max = FieldsBase+Fields.Num()
  FieldCache = ClassCache->GetFromIndex(RepIndex)
  while (FieldCache || bJustSpawned):
      while FieldCache is a UProperty:                    # 1500 replicated property
          if (ArrayDim != 1) Bunch << Element             # 1516 (only for static arrays)
          prop->NetSerializeItem(Bunch, PackageMap, dest) # 1557 type-specific value
          RepIndex = Bunch.ReadInt(ClassCache->GetMaxIndex()); FieldCache = GetFromIndex()
      ... PostNetReceive + RepNotify ...                  # 1581-1614
      if FieldCache is a UFunction:                       # 1617 RPC
          for each param: if (bool || Bunch.ReadBit()) NetSerializeItem(...)  # 1628-1640
          if bCanExecute: Actor->ProcessEvent(Function, Parms)               # 1643-1650
          RepIndex = Bunch.ReadInt(ClassCache->GetMaxIndex()); FieldCache=GetFromIndex()
      else if FieldCache:                                  # 1673
          appErrorfDebug("Invalid replicated field %i")    # malformed handle -> debug assert
          return
  if (Actor->bTearOff && NM_Client): ... bTornOff=true; Actor=NULL          # 1680-1688
```

### Wire order of an OPENING actor bunch payload (definitive, from the receive code) [H]

```
[ object ref  ]   # the CLASS/ARCHETYPE, via SerializeObject (§3). NOT a separate actor NetGUID.
[ Location    ]   # SerializeCompressed (§4) — ONLY present for transient (spawned) actors
[ Rotation    ]   # SerializeCompressed, ONLY if InActor->bNetInitialRotation (§4)
[ NetPlayerIndex] # int32-ish, ONLY if the actor is a PlayerController (line 1417)
[ property/RPC stream ]   # RepIndex(SerializeInt(GetMaxIndex())) + value, repeated (§5)
```

This **resolves the open item in `RS2V_ActorReplication_7258.md` §2.2/§6.0**: Location/Rotation are
read **inside the SerializeNewActor header** (right after the class ref, before the property loop),
**not** as ordinary `Actor` properties — and there is **no second "actor NetGUID"** in the open
bunch. The actor's identity on the wire **is the channel index** (see §6). The recurring constant
per-class leading bytes our capture saw (`5aa50200`, `86bb0800`, …) are the **class/archetype
reference**; the bytes our doc called the "per-actor NetGUID" are actually the **compressed
Location** (`SerializeCompressed` leads with a small `SerializeInt(Bits,20)` then three ranged
ints — variable width, which is exactly why a constant `maxHandle` never consumed them cleanly).

---

## 2. Send-side algorithm — `UActorChannel::ReplicateActor` (`UnChan.cpp:1695-2020`) [H]

What our emulator must emit. The initial-open block (`UnChan.cpp:1769-1799`):

```
if (Actor->bNetInitial && OpenedLocally):
    if (Actor->IsStatic() || Actor->bNoDelete):
        Bunch << Actor                 # 1774 persistent actor: a STATIC object ref (level actor)
    else:
        # transient (server-spawned) actor — the normal gameplay case
        UObject* Archetype = Actor->GetArchetype()      # 1788  (== the class CDO for a plain class)
        Bunch << Archetype                              # 1789  <-- the class/archetype ref the
                                                        #            client reads at ReceivedBunch:1381
        SerializeCompressedInitial(Bunch, Actor->Location, Actor->Rotation,
                                   Actor->bNetInitialRotation, this)   # 1790 (§4)
        if (Actor is PlayerController):
            Bunch << PC->NetPlayerIndex                 # 1796
```

Then (`1801-1806`) the **RemoteRole downgrade**:

```
ActualRemoteRole = Actor->RemoteRole
if (Actor->RemoteRole == ROLE_AutonomousProxy
    && ( ((Instigator==NULL || !Instigator->bNetOwner) && !Actor->bNetOwner)
         || (bDemoRecording && !bDemoOwner) )):
    Actor->RemoteRole = ROLE_SimulatedProxy            # 1805
```

i.e. an actor is only sent as **`AutonomousProxy` to the client that owns it** (`bNetOwner`); to
everyone else its RemoteRole is downgraded to `SimulatedProxy`. UE3 also swaps Role<->RemoteRole on
the wire, so the owning client receives **its** pawn/PC with `Role = AutonomousProxy`. This matches
`RS2V_ActorReplication_7258.md §2.3`.

### Open-bunch flag setup [H]

- Reliable open (the normal case): `bOpen` is set by `SendBunch` when `OpenPacketId==INDEX_NONE`
  (`UnChan.cpp:373-377`); the channel is reliable so `bReliable=1`.
- **One-shot `bNetTemporary` actors** (projectiles, transient FX): `ReplicateActor` line **1735**
  sets `Bunch.bClose = Actor->bNetTemporary` and `Bunch.bReliable = !Actor->bNetTemporary`, i.e. the
  open bunch is **open+close in a single UNRELIABLE bunch**. The client opens, spawns, applies the
  initial block, and closes the channel in one shot. **Do not** use this for PRI/Pawn/GRI/TeamInfo —
  those are reliable opens that stay open.

So our `MakeOpeningActorBunch` (currently in `src/Network/ActorReplication.cpp`) is structurally
right (`bOpen=1, bReliable=1, ChType=2`) **except** that it writes `[classGuid][actorGuid]` — see
§7 for the correction (it must be `[classRef][compressed Location][compressed Rotation?]
[NetPlayerIndex?]`, with no second actor GUID).

---

## 3. The object/class reference codec — `UPackageMap::SerializeObject` (`UnNetDrv.cpp:97-231`) [H]

This is the function our class reference must satisfy. READ side (client), exact wire layout:

```
SerializeObject(Ar, Class, Object&):          # Ar.IsLoading() == client receive
    Object = NULL
    B = SerializeBits(1)                       # 1 selector bit
    if (B == 1):                               # DYNAMIC: a live actor, referenced by channel index
        SerializeInt(Index, MAX_CHANNELS)      # MAX_CHANNELS (1023 wire bound in 7258)
        if (Index <= 0): Object = NULL
        elif (Channels[Index] is an open CHTYPE_Actor channel):
            Object = ((UActorChannel*)Channels[Index])->GetActor()   # line 120
    else:                                      # B == 0  STATIC: a known package-map object
        SerializeInt(Index, MAX_OBJECT_INDEX)  # MAX_OBJECT_INDEX = 1<<31 in THIS build (line 100/125)
        Object = IndexToObject(Index, 1)       # resolve via the package-map export tables
        # (skips objects whose streaming level isn't visible yet) 130-160
    if (Object && !Object->IsA(Class)):        # 162 — "Forged object" guard
        debugf("Forged object: got X expecting Y"); Object = NULL    # 165
    return 1
```

Decisive facts for us:

- **A class/archetype reference is the STATIC path (`B == 0`)** — a class object lives in a verified
  package, so the client resolves it via `IndexToObject(Index)` against the **PackageMap export
  tables we sent during the export phase**. If our `Index` does not address the intended `UClass`
  object in the client's reconstructed table, `IndexToObject` returns the wrong object (or NULL),
  `Cast<AActor>` fails, and the channel is rejected (§1). **This is the most likely root cause.**
  **[H]**
- **The forged-object guard (line 162):** even if the index resolves to *some* object, if it is not
  `IsA(Class)` the client nulls it. At `ReceivedBunch:1381` the call is `Bunch << Object` with
  `Class == UObject::StaticClass()` (the generic `<<` operator passes the broadest class), so the
  forged guard there is permissive — the real filter is `Cast<AActor>(Object)` at line 1382. The
  net effect is identical: **the index must resolve to an actor-class/archetype object.** **[H]**
- **Build delta [M]:** this generic UE3 uses `MAX_OBJECT_INDEX = 1<<31` for the static index, while
  our disasm of VNGame.exe (`RS2V_ActorReplication_7258.md §2.1`) found the static branch bounded by
  `SerializeInt(.,1023)` and the export branch by `SerializeInt(.,0x80000000)`. RS2-7258 is the
  EOS/"Leech" build and uses a **1-bit flag + the 1023/0x80000000 ranged ints** — trust the disasm
  numbers (`docs` §2.1) for the on-wire bit widths, and this source for the *semantics* (selector
  bit meaning, channel-index vs export-table resolution, forged-object guard). Our
  `ActorRepl::WriteNetGUID` (`kStaticGuidMax`/`kExportGuidMax`) already encodes the disasm widths.

---

## 4. `SerializeCompressedInitial` — Location/Rotation header (`UnChan.cpp:15-29`, `UnMath.cpp:51-129`) [H]

Called inline in both the open send (`1790`) and open receive (`1396`). Order: **Location always,
Rotation only if `bNetInitialRotation`.**

`FVector::SerializeCompressed` (`UnMath.cpp:51`):
```
Bits = clamp(ceilLog2(1 + max(|X|,|Y|,|Z|)), 1, 20) - 1
SerializeInt(Bits, 20)                 # 0..19, the magnitude class
Bias = 1<<(Bits+1);  Max = 1<<(Bits+2)
SerializeInt(X+Bias, Max)              # each axis as a ranged int, width = Bits+2
SerializeInt(Y+Bias, Max)
SerializeInt(Z+Bias, Max)
# values are appRound()'d to integers — sub-unit precision is lost
```

`FRotator::SerializeCompressed` (`UnMath.cpp:84`): for each of Pitch/Yaw/Roll, a **1 presence bit**;
if set, **1 byte** (`angle>>8`). Zero components send only the bit. So a zero rotation = 3 bits.

This is the variable-width region our capture analysis (`§6.0` note) could not consume with a
constant `maxHandle` — because it is **not** a property handle at all; it is this compressed vector
in the SerializeNewActor header.

---

## 5. Property / RPC handle codec [H]

Inside the actor bunch, after the open header, the stream is `RepIndex` + value, repeated:

```
RepIndex   = Bunch.ReadInt( ClassCache->GetMaxIndex() )    # UnChan.cpp:1477,1576,1670
FieldCache = ClassCache->GetFromIndex(RepIndex)
```

`GetMaxIndex() = FieldsBase + Fields.Num()` (`UnCoreNet.h:31-34`) — the **total replicated
field count of the class, summed up the inheritance chain** (each class's `FieldsBase` is its
super's cumulative count). `GetFromIndex` walks `this->Super->...` and returns the field whose
`[FieldsBase, FieldsBase+Fields.Num())` range contains the index (`UnCoreNet.h:51-61`). This
**confirms** `RS2V_ActorReplication_7258.md §3.1`: the handle max is a **per-class runtime value =
the class net-cache field count**, and **properties and replicated functions share one index
namespace** (RPCs are just `FieldCache` entries that are `UFunction` instead of `UProperty`). The
`ServerMove` function handle 664 and the property handles 23/51/64 from the capture are indices into
this same table. **[H]**

A malformed/out-of-range handle that maps to neither a property nor a function triggers
`appErrorfDebug("Invalid replicated field")` and `return` (`UnChan.cpp:1673-1677`) — in a shipping
build that is a silent drop of the rest of the bunch, not a channel close, but it desyncs the
channel.

---

## 6. Channel index assignment & open acceptance — `UNetConnection::ReceivedPacket` (`UnConn.cpp:907,1101-1196`) [H]

- **The channel index is chosen by the SERVER and is authoritative.** The client creates the
  channel on first receipt at exactly the index in the bunch header:
  `Channel = CreateChannel((EChannelType)Bunch.ChType, FALSE, Bunch.ChIndex)` (`UnConn.cpp:1166`).
  Actor channels are **bidirectional at the same index** — the client's `ServerMove` goes back out
  on the same ChIndex (this is why ch2 = PC in both directions). So our ascending-from-2 assignment
  is correct; the client mirrors it. **[H]**
- **Pre-open guards that close/drop (server-side, less relevant to us as server but informative):**
  a non-control bunch before the control channel exists → `Close()` (`1101-1110`); during login a
  bunch for a non-existent channel → `Close()` (`1116-1122`); unknown `ChType` →
  `appErrorfDebug` + ack + return (`1154-1162`).
- **The accept gate** (`1169-1185`): on creating the channel the client calls
  `NotifyAcceptingChannel`. If it returns 0 the client **constructs a `FOutBunch CloseBunch(Channel,
  1)` (which sets `bClose`), `bReliable=1`, `SendBunch`s it, flushes, and `ConditionalCleanUp`s** —
  i.e. an immediate **`bClose` reply on the wire**. For the client this only fires for **non-actor**
  channel types (`UnWorld.cpp:3824-3835`), so as long as we send `ChType==2` we pass this gate. **[H]**
- `Bunch.bOpen` ⇒ `Channel->OpenAcked = 1; OpenPacketId = PacketId` (`1188-1192`).

---

## 7. Close / teardown semantics [H]

- **`UChannel::Close()`** (`UnChan.cpp:72-88`): if not already closing and the connection is
  Open/Pending, it sends a **reliable `bClose` bunch** (`FOutBunch CloseBunch(this,1)` →
  `CloseBunch.bClose` true) and waits for the ack.
- **Receiving a `bClose`** — `UChannel::ReceivedSequencedBunch` (`UnChan.cpp:272-282`): after
  processing the bunch's payload, if `Bunch.bClose` it logs "got close-notify" and
  `ConditionalCleanUp()`s the channel.
- **Close-on-ack** — `UChannel::ReceivedAcks` (`UnChan.cpp:156-178`): when an outgoing bunch that
  had `bClose` is acked (`DoClose |= OutRec->bClose`), or for an `OpenTemporary` channel once the
  open is acked, the channel is cleaned up. This is the lifecycle of a `bNetTemporary` one-shot.
- **`UActorChannel::Close()`** (`UnChan.cpp:1108-1142`): removes the actor from
  `Connection->ActorChannels`, and for transient actors nulls out `Recent` references to this actor
  in **all other channels** (so a later re-open resends them), then `Actor = NULL`.
- **`UActorChannel::CleanUp()`** (`UnChan.cpp:1158+`): on the **client**, unless the actor is
  `bTearOff` or `bNetTemporary`, it **destroys the actor** (`GWorld->DestroyActor`). So a spurious
  bClose from us = the client deletes the corresponding actor. `bNoDelete` actors get
  `eventReplicationEnded()` instead.
- **`NMT_ActorChannelFailure` handling (server side = us)** (`UnChan.cpp:782-805`): the client sends
  this when *it* can't open a channel; on receipt we look up `Channels[ChannelIndex]`, and **if that
  channel's actor is `Connection->Actor` (the PlayerController) we `Connection->Close()` the whole
  connection**; otherwise `Connection->Actor->NotifyActorChannelFailure(ActorChan)`.

---

## 8. What our emulator MUST emit (and avoid)

**Emit, per opening actor channel (reliable, `bControl=1,bOpen=1,bClose=0,bReliable=1,ChType=2`,
ascending ChIndex from 2, ChSeq starting 1):**

1. **One object reference = the actor's CLASS/ARCHETYPE**, via the §3 codec: selector bit + ranged
   `SerializeInt`. For a class it is the **static** path — the index MUST resolve, through the
   client's reconstructed PackageMap export tables, to the intended `UClass`/CDO that
   `Cast<AActor>` accepts. **No second per-actor NetGUID is written** — channel index is identity.
2. **Compressed Location** (§4) — always. Use `appRound`ed integer coords.
3. **Compressed Rotation** (§4) — **only if** the class's `bNetInitialRotation` is true (for most
   gameplay actors it is; a zero rotation is just 3 bits).
4. **`NetPlayerIndex`** — **only if** the actor is the PlayerController (0 for the main local PC).
5. Then the **initial replicated-property block**: `SerializeInt(handle, ClassCache.GetMaxIndex())`
   + typed value, in field-declaration order, initial-condition properties only (§5, and the
   per-class field lists in `RS2V_ActorReplication_7258.md §3.2`). Set the role flags so the owning
   PC/Pawn carries `RemoteRole=AutonomousProxy(2)` + `bNetOwner`, others `SimulatedProxy(1)` (§2).

**Avoid (each = a reject/close):**

- Wrong `ChType` (anything but 2) → instant `bClose` from the client (§1.1/§6). 
- A class index that resolves to NULL or a non-`AActor` object → `NMT_ActorChannelFailure` →
  if it's the PC channel, **connection closed** (§1.2/§7). **This is the current failure.**
- Writing a second "actor NetGUID" after the class ref (our current
  `ActorRepl::WriteNewActorHeader` does `[classGuid][actorGuid]`) — the client will misread the
  extra ref as the start of the compressed Location, desync the whole bunch, and fail the
  property loop / produce a forged/NULL object on the next channel. **Drop the actorGuid write.**
- Setting `bClose`/unreliable on a non-temporary actor (would make the client spawn-then-destroy
  the actor, §7).
- A handle ≥ `ClassCache.GetMaxIndex()` for the (wrong) class → desync (§5).

### Concrete change to `src/Network/ActorReplication.cpp` [M]

`WriteNewActorHeader` should become, in order: `WriteNetGUID(classRef)` →
`WriteCompressedVector(Location)` → `if (bNetInitialRotation) WriteCompressedRotator(Rotation)` →
`if (isPlayerController) WriteInt32(NetPlayerIndex)`. Remove the `actorGuid` write entirely. Add
`WriteCompressedVector`/`WriteCompressedRotator` per §4 to `BitWriter`. The class ref must index the
**actual class object** in our sent PackageMap (the same index family the capture shows as the
recurring `5aa50200`/`86bb0800` leading bytes), via the disasm-confirmed 1023/0x80000000 ranged-int
widths in `RS2V_ActorReplication_7258.md §2.1` — **not** this generic build's `1<<31`.

---

## 9. Source citations (local UE3 tree)

| Claim | File:line |
|------|-----------|
| Receive open: read class ref, Cast<AActor>, reject → Broken + NMT_ActorChannelFailure | `UnChan.cpp:1381-1390` |
| Transient actor: SerializeCompressedInitial then SpawnActor | `UnChan.cpp:1391-1409` |
| PlayerController NetPlayerIndex read after Loc/Rot | `UnChan.cpp:1414-1442` |
| Property/RPC stream, max = GetClassNetCache()->GetMaxIndex() | `UnChan.cpp:1448,1477,1576,1670` |
| Send open: `Bunch << Archetype` then SerializeCompressedInitial then NetPlayerIndex | `UnChan.cpp:1769-1799` |
| RemoteRole AutonomousProxy→SimulatedProxy downgrade for non-owners | `UnChan.cpp:1801-1806` |
| bNetTemporary ⇒ open bunch is bClose+unreliable | `UnChan.cpp:1735-1736` |
| SerializeObject: 1 selector bit, dynamic=channel idx / static=export idx, forged-object guard | `UnNetDrv.cpp:97-167` |
| `MAX_OBJECT_INDEX = 1<<31` (this build) / `GetMaxIndex = FieldsBase+Fields.Num()` | `UnCoreNet.h:100,31-34` |
| FVector/FRotator::SerializeCompressed wire format | `UnMath.cpp:51-77,84-129` |
| Client accepts only CHTYPE_Actor, refuses other types | `UnWorld.cpp:3813-3858` |
| Refused channel ⇒ client sends bClose + cleanup | `UnConn.cpp:1166-1185` |
| Channel created at server-chosen ChIndex on first receive | `UnConn.cpp:1166` |
| bClose receive ⇒ ConditionalCleanUp | `UnChan.cpp:272-282` |
| close-on-ack / OpenTemporary cleanup | `UnChan.cpp:156-178` |
| NMT_ActorChannelFailure: close connection if failed chan is the PC | `UnChan.cpp:782-805` |
| UChannel::Close sends reliable bClose bunch | `UnChan.cpp:72-88` |

VNGame.exe addresses for the same logic (from `RS2V_ActorReplication_7258.md §2.1` /
`RS2V_ControlChannel_WireSpec_7258.md §3`): `SerializeObject` net override `0x140696070`
(static branch `SerializeInt(.,1023)` @`0x1406960d5`, export `SerializeInt(.,0x80000000)`
@`0x14069613a`); bunch header serializer `0x1404a79d0`; `ReceivedPacket` `0x1404a4e60`.
`UActorChannel::ReceivedBunch` itself is unpinned in the shipping binary (asserts stripped,
vtable-only) but provably calls `0x140696070` — this source supplies the exact algorithm it runs.
