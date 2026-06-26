# UE3 Object-Reference / PackageMap Serialization (EngineVersion 7258, RS2:Vietnam)

Source-anchored spec for how the UE3 (RS2V, EngineVersion 7258) server serializes
**object references** ("NetGUIDs") and **PackageMap exports**, and what our emulator
must implement to make per-session actors the retail client will accept.

Primary source = the leaked UE3 engine source now at `D:\RE-Tools\UE3-src`
(`Development\Src\...`), cross-checked against our prior `VNGame.exe` disassembly
(`docs/RS2V_ActorReplication_7258.md` §2.1, `docs/RS2V_ControlChannel_WireSpec_7258.md`,
`docs/RS2V_PostJoin_Replication_7258.md`).

**Confidence tags:** `[H]` = exact UE3 source + matches capture/disasm; `[M]` = source
clear but one mapping inferred; `[L]` = inferred, not yet bit-verified.

---

## 0. TL;DR — the single most important finding

> **This UE3 build has NO "NetGUID" system.** There is no `AssignNetGUID`,
> `GetNetGUID`, `FNetGUIDCache`, `IsExportingNetGUIDBunch`, or `SerializeNewActor`
> anywhere in the engine source (`grep -rn` over `Development\Src` returns only
> *package-level* `FGuid`s, never per-actor GUIDs). Those are **UE4** concepts.
> **[H]**

In UE3/7258 an object reference on the wire is encoded by **`UPackageMap::SerializeObject`**
as a **1-bit selector + one bounded `SerializeInt`**, and the index it carries is one of
exactly two kinds:

1. **Dynamic actor** → identified by its **open actor-channel index** (`UActorChannel::ChIndex`),
   bounded by `MAX_CHANNELS = 2048`. There is **no persistent id** — the actor *is* its channel.
2. **Static / known object** (classes, archetypes, CDOs, static level actors) → identified by a
   **PackageMap object index** = `package.ObjectBase + Object->NetIndex`, bounded by
   `MAX_OBJECT_INDEX = 0x80000000`. Deterministic from package load order × linker export order.

**Why verbatim replay fails (the crux):** the captured actor-open bunches reference dynamic
actors *by the captured session's channel indices* and reference classes/archetypes *by the
captured session's PackageMap object indices*. When we replay them verbatim into OUR session,
(a) those channel indices don't correspond to channels WE opened in the order the client expects,
and (b) the static class indices only resolve if OUR PackageMap is byte-identical (same package
list, same order, same per-package `ObjectCount`s, same GUIDs) to the one we exported. The client
`bClose`s every channel whose contents it can't reconcile. **The fix is to GENERATE actor opens
where the channel index, the archetype reference, and any cross-actor references are all
self-consistent within our own session's channel space and our own exported PackageMap.** **[H]**

The actor's own identity is therefore **the channel** we choose to open it on — we assign it,
the client mirrors it. We never write an actor id; we write the actor's *archetype* (static
ref) + Location/Rotation, and the **channel index** is the identity.

---

## 1. `UPackageMap::SerializeObject` — the object-reference codec

### 1.1 Class hierarchy and which one RS2 uses

`Development\Src\Core\Inc\UnCoreNet.h` and `Development\Src\Engine\Inc\UnNetDrv.h`:

```
UObject
  └ UPackageMap            (Core/UnCoreNet.{h,cpp})     base; List<FPackageInfo>, Object<->Index
      └ UPackageMapLevel   (Engine/UnNetDrv.{h,cpp})    ** the live game uses THIS **
          └ UPackageMapSeekFree (Engine/UnNetDrv.cpp)   path-string fallback; NOT used on the wire
```

The live connection's `Connection->PackageMap` is a **`UPackageMapLevel`** (per-connection,
holds a back-pointer to `UNetConnection`). `UNetConnection::InitConnection`
(`Engine/UnConn.cpp:275-282`) picks the class by `GUseSeekFreePackageMap`: `TRUE` →
`UPackageMapSeekFree` (path-string fallback), `FALSE` → `UPackageMapLevel` (index-based). The
RS2 capture shows **constant per-class integer refs** (`5aa50200`, `ce290200`, …), not path
strings, so RS2 runs with `GUseSeekFreePackageMap == FALSE` → **`UPackageMapLevel` is the live
codec**. **[H]** The base `UPackageMap::SerializeObject` is a stub
that `appErrorf`s (`UnCoreNet.cpp:128`). `UPackageMapSeekFree::SerializeObject` serializes
full path strings and is **not** the wire format we see (it's a seekfree/no-linker fallback).
**Every `Bunch << Object`** routes here:

`Development\Src\Engine\Src\UnBunch.cpp:30-34, 96-100`:
```cpp
FArchive& FInBunch::operator<<( UObject*& Object )
{ Connection->PackageMap->SerializeObject( *this, UObject::StaticClass(), Object ); return *this; }
FArchive& FOutBunch::operator<<( UObject*& Object )
{ Channel->Connection->PackageMap->SerializeObject( *this, UObject::StaticClass(), Object ); return *this; }
```

### 1.2 The exact algorithm — `UPackageMapLevel::SerializeObject`

`Development\Src\Engine\Src\UnNetDrv.cpp:97-231`. Read (load) side, verbatim logic:

```cpp
UBOOL UPackageMapLevel::SerializeObject( FArchive& Ar, UClass* Class, UObject*& Object ) {
  DWORD Index = 0;
  if (Ar.IsLoading()) {
    Object = NULL;
    BYTE B = 0; Ar.SerializeBits(&B, 1);          // (1) the selector bit
    if (B) {                                       //   B==1 => DYNAMIC actor (or None)
      Ar.SerializeInt(Index, UNetConnection::MAX_CHANNELS);   // (2a) bound = 2048
      if (Index <= 0) Object = NULL;               //   index 0 => None
      else if (Index < MAX_CHANNELS && Channels[Index]
               && Channels[Index]->ChType==CHTYPE_Actor && !Closing)
        Object = ((UActorChannel*)Channels[Index])->GetActor();  // resolve via channel
    } else {                                       //   B==0 => STATIC / known object
      Ar.SerializeInt(Index, MAX_OBJECT_INDEX);    // (2b) bound = 0x80000000
      Object = IndexToObject(Index, 1);            // resolve via PackageMap (§2)
      /* ... level-visibility gating elided ... */
    }
    if (Object && !Object->IsA(Class)) Object = NULL;   // "Forged object" guard
    return 1;
  } else { /* save side, exact mirror, see below */ }
}
```

Save (write) side (`UnNetDrv.cpp:169-230`), the mirror:
- **Dynamic actor** (`Cast<AActor>` && not CDO/archetype && `!IsStatic()` && `!bNoDelete`):
  write `B=1`, look up `Connection->ActorChannels.FindRef(Actor)`; `Index = Ch->ChIndex`
  (0 if no channel yet), `SerializeInt(Index, MAX_CHANNELS)`. **Returns `Ch->OpenAcked`** as
  the "mapped" flag (an unacked-open actor serializes as its channel index but reports
  *unmapped*, so the caller knows the ref may not resolve yet).
- **NULL** or **object the client hasn't initialized the level for**: write `B=1`,
  `SerializeInt(0, MAX_CHANNELS)` (a dynamic index of 0 == None).
- **Static / regular object** with a valid `ObjectToIndex`: write `B=0`,
  `Index = ObjectToIndex(Object)`, `SerializeInt(Index, MAX_OBJECT_INDEX)`.
- Object that exists but `ObjectToIndex==INDEX_NONE` (not in PackageMap): write `B=1` +
  `SerializeInt(0,...)` (sends None — it *cannot* be serialized).

### 1.3 Wire bit layout (definitive)

```
SerializeObject(obj):
    bit  selector B           ; 1 bit, LSB-first (Ar.SerializeBits(&B,1))
    if B == 1:   (DYNAMIC actor, or None)
        Index = SerializeInt(MAX_CHANNELS = 2048)      ; ranged minimal-bit int
        ; Index 0 == None; otherwise Index == the actor's open ChIndex
    if B == 0:   (STATIC / known object: class, archetype, CDO, static actor)
        Index = SerializeInt(MAX_OBJECT_INDEX = 0x80000000) ; ranged minimal-bit int
        ; Index resolves through the PackageMap (package.ObjectBase + Object->NetIndex)
```

`SerializeInt(value, Max)` = the standard UE3 ranged minimal-bit int (LSB-first, reads bits
while `(value+mask) < Max`), the same primitive documented in
`RS2V_ControlChannel_WireSpec_7258.md §1`. For `Max=2048` that is up to 11 bits; for
`Max=0x80000000` it is up to 31 bits.

### 1.4 Reconciliation with our `VNGame.exe` disassembly **[H — corrected]**

`RS2V_ActorReplication_7258.md §2.1` recorded, from `VNGame.exe @ 0x140696070`:
*"flag==1 ⇒ static, `SerializeInt(1023)`; flag==0 ⇒ dynamic/export, `SerializeInt(0x80000000)`."*
Matching against this UE3 source, two corrections (the high-level shape — **1 flag bit + ranged
SerializeInt** — was right):

| Aspect | Disasm doc said | UE3 source says | Resolution |
|--------|-----------------|-----------------|------------|
| selector polarity | flag=1 ⇒ static | **B=1 ⇒ DYNAMIC actor**, B=0 ⇒ static | **source is authoritative**; the doc's flag semantics are inverted |
| dynamic-branch bound | 0x80000000 | **`MAX_CHANNELS = 2048`** | the 0x80000000 belongs to the **static** branch, not dynamic |
| static-branch bound | 1023 | **`MAX_OBJECT_INDEX = 0x80000000`** | `1023` is the *bunch-header ChIndex* bound (`SerializeInt(1023)` for the ChIndex field, doc §3), **not** the object-ref index. The two were conflated. |

So the corrected, source-true codec is: **B=0 → `SerializeInt(0x80000000)` (static/PackageMap);
B=1 → `SerializeInt(2048)` (dynamic/channel).** The `1023` from the old note is the separate
ChIndex header field. The capture evidence still holds: the ch2 PlayerController open payload
begins `60 c1 01 00 …` → byte0 `0x60` LSB-first = bits `0,0,0,0,0,1,1,0`, **first bit = 0 = the
STATIC branch** — which is exactly right, because the first object written in an actor-open is the
**archetype** (a static CDO), not the dynamic actor itself (§3). Earlier we read this 0-bit as
"dynamic/export"; per source it is the **static** archetype reference. The subsequent
`SerializeInt(0x80000000)` then reads the archetype's PackageMap index. **[H]**

> ACTION for our codec: if our emulator currently writes object refs as
> "flag=1→static(1023)/flag=0→dynamic(0x80000000)", **flip it** to
> "flag=0→static(0x80000000)/flag=1→dynamic(2048)". Re-verify against the `60 c1 01 00` open.

### 1.5 `SerializeName` (for completeness) **[H]**

Two implementations exist:
- `UPackageMap::SerializeName` (`UnCoreNet.cpp:79-122`) — the **real** one: `1 bit bHardcoded`;
  if hardcoded, `SerializeInt(NameIndex, MAX_NETWORKED_HARDCODED_NAME+1)` where
  `MAX_NETWORKED_HARDCODED_NAME = 1250` (`Core/Inc/UnNames.h:16`) → indices ≤1250 are the
  engine's hardcoded `EName` table; else `FString InString; INT InNumber` (name + number).
- `UPackageMapSeekFree::SerializeName` (`UnNetDrv.cpp:314`) — always string+number (fallback only).
The live wire uses the hardcoded-index form for engine names. Not on the critical path for actor
opens (names appear in a few RPCs/property values), but pin it if a name-bearing property misdecodes.

---

## 2. How static indices ("class NetGUIDs") are ASSIGNED — `UPackageMap` + linker

This is the deterministic algorithm that produces the **constant-per-class** leading field we see
in every actor-open (`5aa50200` for PRI ×65, `86bb0800` for Pawn, `ce290200` for GRI, etc.).
Source: `Development\Src\Core\Src\UnCoreNet.cpp`.

### 2.1 The PackageMap object-index space

A `UPackageMap` holds `TArray<FPackageInfo> List` (one entry per replicated package). Each
`FPackageInfo` (`UnCoreNet.h:75-95`) carries `PackageName`, `Guid`, `ObjectBase` (net index of its
first object), `ObjectCount`, generations, flags, extension.

`UPackageMap::Compute()` (`UnCoreNet.cpp:210-249`) lays out the **global object-index space** by
walking `List` in order and assigning each package a contiguous block:
```cpp
DWORD MaxObjectIndex = 0;
for (i in List) {
    Info.ObjectBase  = MaxObjectIndex;          // this package's block starts here
    Info.ObjectCount = <net-object count for the agreed generation>;  // from the UPackage
    PackageListMap.Set(Info.Parent->GetFName(), i);
    MaxObjectIndex  += Info.ObjectCount;        // next package starts after this block
}   // appErrorf if MaxObjectIndex > MAX_OBJECT_INDEX (0x80000000)
```

So the **global index of a static object** is `package.ObjectBase + object.NetIndex`
(`ObjectToIndex`, `UnCoreNet.cpp:445-460`):
```cpp
INT UPackageMap::ObjectToIndex(UObject* Object) {
    if (Object && Object->NetIndex != INDEX_NONE) {
        INT* Found = PackageListMap.Find(Object->GetOutermost()->GetFName());
        if (Found) { FPackageInfo& Info = List(*Found);
            if (Object->NetIndex < Info.ObjectCount) return Info.ObjectBase + Object->NetIndex; }
    }
    return INDEX_NONE;
}
```
and the reverse `IndexToObject(InIndex)` (`UnCoreNet.cpp:474-540`) walks `List`, subtracting each
`Info.ObjectCount` until `InIndex < Info.ObjectCount`, then returns
`Info.Parent->GetNetObjectAtIndex(InIndex)` (loading the export on demand).

### 2.2 `Object->NetIndex` = the linker export index **[H]**

`UObject::NetIndex` is set from the object's **linker export index** when the package is loaded
(`Core/Src/UnObj.cpp:1570-1587`: `SetNetIndex(_LinkerIndex)`), and `SetNetIndex`
(`UnObj.cpp:1058-1078`) registers the object in its package's net-object table
(`Package->AddNetObject(this)`), unless the package is `PKG_ServerSideOnly`. So:

> **A static object's wire index is fully determined by: (PackageMap List order) × (each
> package's net-object/linker-export order).** It is NOT random and NOT per-session — it is the
> same on server and client **iff both built their PackageMap from the same package set, in the
> same order, with the same per-package object counts and GUIDs.** That is the entire purpose of
> the NMT_Uses PackageMap export (§4). **[H]**

This is why the class field is constant across both captured sessions (`5aa50200` etc.): the
package list and the engine/content linker layout are identical between runs. Our emulator gets
the **same** class indices for free **as long as we export the identical PackageMap** and the
client resolves classes the same way — which it does, because the indices are computed from the
packages the client itself loaded.

### 2.3 Dynamic actors get NO assigned index — they ARE their channel **[H]**

There is no `AssignNetGUID`. A dynamic (spawned, non-static, `!bNoDelete`) actor is referenced
purely by `UActorChannel::ChIndex` (§1.2 save side). The server "assigns" a dynamic actor's
identity by the simple act of **opening an actor channel for it** (`UActorChannel`, picked from the
first free channel slot in `UNetConnection`'s `Channels[2048]`). `UPackageMapLevel::CanSerializeObject`
(`UnNetDrv.cpp:68-95`) confirms: a dynamic actor "can be serialized if it has a channel"
(`Connection->ActorChannels.FindRef(Actor) != NULL`); a static actor can always be serialized once
the client has initialized its level.

> **Consequence for us:** to make actor A referenceable (e.g. a PRI's `Team` pointing at a
> TeamInfo, or a Pawn's `PlayerReplicationInfo`), we must have **already opened a channel for the
> target**, and we encode the reference as `B=1 + SerializeInt(targetChIndex, 2048)`. Cross-actor
> references therefore force an **open ordering**: open referenced actors before (or in the same
> burst as) the referrers, and make every channel index we cite real and consistent. The capture's
> open order (GRI, TeamInfos, local PRI, then PC/pawns) is exactly this dependency order. **[H]**

---

## 3. The actor-open header (UE3's equivalent of "SerializeNewActor")

There is no `SerializeNewActor` function. The new-actor header is written **inline** in
`UActorChannel::ReplicateActor` and read inline in `UActorChannel::ReceivedBunch`.

### 3.1 Write side — `UActorChannel::ReplicateActor` (`UnChan.cpp:1695, header at 1769-1799`) **[H]**

For the **initial** open of a dynamic (transient) actor:
```cpp
if (Actor->bNetInitial && OpenedLocally) {
  if (Actor->IsStatic() || Actor->bNoDelete) {
      Bunch << Actor;                       // persistent actor: ONE object ref (static path)
  } else {
      // transient (spawned) actor:
      UObject* Archetype = Actor->GetArchetype();
      Bunch << Archetype;                   // (1) ARCHETYPE ref -> STATIC path (B=0, §1.3/§2)
      SerializeCompressedInitial(Bunch, Actor->Location, Actor->Rotation,
                                 Actor->bNetInitialRotation, this);   // (2) compressed pos/rot
      APlayerController* PC = Actor->GetAPlayerController();
      if (PC != NULL) Bunch << PC->NetPlayerIndex;   // (3) PlayerController only: BYTE-ish index
  }
}
// ... then the normal replicated-property block follows (handle + value, §3.4) ...
```

So a dynamic actor-open bunch payload is, in order:
```
[ archetype object-ref ]   ; SerializeObject, B=0 static, SerializeInt(0x80000000)
                           ;   -> resolves to the actor's class-default/archetype, which is
                           ;      what tells the client WHICH CLASS to SpawnActor (§3.2)
[ compressed Location ]    ; FVector::SerializeCompressed
[ compressed Rotation ]    ; FRotator::SerializeCompressed  (only if bNetInitialRotation)
[ NetPlayerIndex ]         ; ONLY if the actor is a PlayerController
[ initial replicated-property block ]   ; handle + value pairs (§3.4)
```
The **actor's own identity is the channel** (`Bunch.ChIndex` from the bunch header) — it is never
written in the payload. **[H]** This matches the capture: the leading constant `60c10100`/`5aa50200`/
`ce290200` is the **archetype** (class) static ref; there is no separate per-actor id field after it
(what `RS2V_ActorReplication_7258.md` §2.2 labeled "actor NetGUID" is actually the start of the
compressed Location). **[H — source-confirmed correction to the earlier two-NetGUID reading.]**

### 3.2 Read side — `UActorChannel::ReceivedBunch` (`UnChan.cpp:1371-1419`) **[H]**

The client mirror, on a fresh channel with `Bunch.bOpen`:
```cpp
UObject* Object; Bunch << Object;            // read the ARCHETYPE static ref
AActor* InActor = Cast<AActor>(Object);
if (InActor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) {  // transient actor
    FVector Location; FRotator Rotation(0,0,0);
    SerializeCompressedInitial(Bunch, Location, Rotation, InActor->bNetInitialRotation, NULL);
    InActor = GWorld->SpawnActor(InActor->GetClass(), NAME_None, Location, Rotation,
                                 InActor /*template*/, 1,1, NULL,NULL,1);   // <-- SPAWNS from archetype
}
SetChannelActor(InActor);                     // binds this channel <-> the spawned actor
APlayerController* PC = Actor->GetAPlayerController();
if (PC) { Bunch << PC->NetPlayerIndex; ... }   // match to local viewport
```
So the client **spawns the actor from the archetype's class** and binds it to the channel. If the
archetype static-ref doesn't resolve (wrong PackageMap index → wrong/absent class), it logs
"Received invalid actor class", sets `Broken=1`, sends **`NMT_ActorChannelFailure`** and the
channel is dead — which is exactly the "client bCloses the channel" failure we observe on verbatim
replay. **[H]**

### 3.3 `SerializeCompressedInitial` (`UnChan.cpp:15-29`) **[H]**

```cpp
Location.SerializeCompressed(Bunch);                       // FVector compressed
if (bSerializeRotation) Rotation.SerializeCompressed(Bunch);   // FRotator compressed
```
`FVector::SerializeCompressed` / `FRotator::SerializeCompressed` are the UE3 compressed vector/
rotator codecs (in `Core/UnMath` / vector classes). Bit-exact widths are **[M]** here (variable,
value-dependent); this is the "native compressed Location/Rotation" the capture analysis already
flagged as not single-constant-width. For generation, our spec uses the verbatim-template approach
(§5) for the bytes inside the initial block and patches only per-session fields.

### 3.4 Property block + handle max **[H]**

After the new-actor header, replicated properties are written (`ReplicateActor`, ~`UnChan.cpp:1952`):
```cpp
Bunch.WriteIntWrapped(FieldCache->FieldNetIndex, ClassCache->GetMaxIndex());   // the HANDLE
It->NetSerializeItem(Bunch, Connection->PackageMap, (BYTE*)Actor + Offset);     // the value
```
The handle is `SerializeInt(FieldNetIndex, ClassCache->GetMaxIndex())`, where
`GetMaxIndex() = FieldsBase + Fields.Num()` (`UnCoreNet.h:31-34`) — the **per-class runtime field
count**, accumulated across the super-class chain (`GetClassNetCache`, `UnCoreNet.cpp:137-195`).
This is exactly the "max = ClassNetCache replicated-field count, a per-class runtime value" finding
in `RS2V_ActorReplication_7258.md §3.1`. Each property's `FieldNetIndex` is assigned in
`GetClassNetCache` by walking `Class->NetFields` and giving each field
`ThisIndex = Result->GetMaxIndex()` at the time it's added (so indices = declaration order across
the inheritance chain, base class first). **[H]** Object-typed property *values* (e.g. `Owner`,
`PlayerReplicationInfo`, `Team`) serialize via the **same `SerializeObject`** (§1) — dynamic refs
become channel indices, so the open ordering in §2.3 applies to property values too.

---

## 4. The PackageMap export (NMT 0x07 / `NMT_Uses`)

Already reversed bit-exactly in `RS2V_PostJoin_Replication_7258.md §4` (321 packages, 20 reliable
ch0 bunches, per-record `0x07 + FGuid(16) + name + ext + flags(u32) + generation(u32) + "None" +
8 zero bytes`). The UE3 source confirms the **purpose and the field set**:

- The server's `MasterMap` is a `UPackageMap` built by `AddNetPackages()` (`UnCoreNet.cpp:264-277`)
  from `UPackage::GetNetPackages()`; per-connection maps are `Copy()`d from it. **[H]**
- Each exported record corresponds to one `FPackageInfo` (`UnCoreNet.h:75-95`): `PackageName`,
  `Guid`, `PackageFlags`, generation/`ObjectCount`, `Extension`. The wire record in
  `RS2V_PostJoin_Replication_7258.md §4.2` is exactly these fields. **[H]**
- The client builds **its own** `UPackageMap` from this list and runs the same `Compute()`
  (`UnCoreNet.cpp:210`) → both sides derive **identical `ObjectBase` blocks** → static object
  indices (§2.1) match. The `RemoteGeneration` handshake in `Compute()` (only count objects both
  sides have) is why generation/`ObjectCount` must match. **[H]**
- Client mirrors its PackageMap upstream (capture C2S f193-f200) so the server can do the same
  reconciliation for client→server refs. **[H]**

> **Reconciliation requirement for us:** export the **same 321 packages in the same order with the
> same GUIDs, flags, and object counts** as the capture (`data/packagemap_export_7258.bin` /
> `packagemap_chunks.json`). Then `package.ObjectBase` values match the client's, and every static
> class/archetype ref we write with `ObjectToIndex` resolves on the client. Do **not** reorder,
> add, or drop packages, or every static index downstream shifts. **[H]**

---

## 5. What our emulator must implement (concrete spec)

### 5.1 Object-reference codec (`SerializeObject`) — REQUIRED, fix polarity

Implement encode/decode exactly as §1.3 (note the **corrected** polarity vs. our old codec):

```
writeObjectRef(obj):
  if obj is a dynamic actor (spawned, !static, !bNoDelete):
      writeBit(1)
      ch = channelIndexFor(obj)            # 0 if no open channel yet (=> sends None/unmapped)
      writeInt(ch, 2048)
      return openAcked(ch)                 # "mapped" flag for caller
  elif obj is NULL or unserializable:
      writeBit(1); writeInt(0, 2048)
  else:  # static / class / archetype / CDO / static actor
      writeBit(0); writeInt(objectToIndex(obj), 0x80000000)

readObjectRef():
  B = readBit()
  if B: idx = readInt(2048);       return (idx==0) ? None : actorOnChannel(idx)
  else: idx = readInt(0x80000000); return indexToObject(idx)
```

`objectToIndex(obj) = packageMap.objectBaseFor(obj.package) + obj.netIndex` (§2.1). For our
generated bootstrap actors we only ever need to **encode**: dynamic actors → their channel index;
classes/archetypes → their PackageMap static index (which we can read straight out of our exported
PackageMap, since we control it).

### 5.2 Channel-index assignment = our "NetGUID assignment" — REQUIRED

- Maintain a per-connection `Channels[2048]` array; a dynamic actor's identity **is** the index we
  open it on. Assign ascending from 2 (ch0 = control, ch1 reserved), exactly as the capture
  (`RS2V_ActorReplication_7258.md` opens ch2 PC first, then ascending). **[H]**
- Keep a map `actor -> channelIndex` so cross-actor references (property values, §3.4) encode the
  right index. **Open a referenced actor before any actor that references it** (§2.3): GRI →
  TeamInfos → local PRI → PlayerController → Pawn. **[H]**
- `OpenAcked`: until the client acks the open, `SerializeObject` reports the ref *unmapped*. For a
  single-shot bootstrap burst this is usually fine (the refs resolve once the opens are acked); if
  a property value ref must be mapped immediately, gate that property until the target's open is
  acked (matches UE3's `Mapped = Ch->OpenAcked` return). **[M]**

### 5.3 Static class/archetype indices — REQUIRED, derived from OUR PackageMap

- Build our connection PackageMap from the **same 321-package export** we send (§4). Run the
  `Compute()` cumulative-`ObjectBase` algorithm (§2.1) so our `objectToIndex` matches the client's.
- For each bootstrap class we open (ROGameReplicationInfo, ROTeamInfo, ROPlayerReplicationInfo,
  ROPlayerController, ROPawn, ...), its **archetype/CDO static index** is constant and equals the
  value already captured (`ce290200`, `0ac10200`, `5aa50200`, `60c10100`, `86bb0800`, ...). Since
  these are stable across sessions (§2.2) we can **emit the captured class refs verbatim** — they
  resolve as long as our PackageMap export matches. **[H]**

### 5.4 Actor-open bunch generator (`MakeOpeningActorBunch`) — REQUIRED

Per §3.1, for each bootstrap actor emit a bunch with header `bOpen=1, bReliable=1, ChType=2(Actor)`,
`ChIndex = our assigned index`, `ChSequence` from the channel, then payload:
```
1. archetype static ref      = writeObjectRef(archetypeCDO)  -> B=0 + SerializeInt(idx, 0x80000000)
2. SerializeCompressed(Location) [+ Rotation if bNetInitialRotation]
3. if PlayerController: NetPlayerIndex
4. initial replicated-property block: for each initial-condition replicated field in class
   declaration order (RS2V_ActorReplication_7258.md §3.2): writeInt(FieldNetIndex, ClassCache.MaxIndex)
   then the typed value (object values via writeObjectRef from step §5.1).
```
Pragmatic path (matches `RS2V_ActorReplication_7258.md §6`): for the initial property block, **emit
the captured per-class template bytes verbatim** and patch only per-session fields (actor's channel
index is in the header not the payload; FStrings ServerName/PlayerName; cross-actor `Team`/`Owner`/
`PlayerReplicationInfo` refs rewritten to OUR channel indices via §5.1). The compressed Location/
Rotation widths are value-dependent and not worth re-deriving for a static spawn — reuse the
template's bytes (or send a known spawn transform). **[H bytes / M field-level patching]**

### 5.5 Minimal bootstrap set + order (unchanged from prior docs, now source-justified)

`PC(ch2) → GRI → TeamInfo ×2-3 → local PRI`, each a verbatim §5.4 template with our channel indices
and PackageMap-consistent class refs; then (for control) spawn+possess Pawn, open its channel,
send `ClientRestart(Pawn)` on ch2. The open **order is forced by §2.3** (referenced-before-referrer).

### 5.6 Things to delete/avoid from any UE4-derived assumptions

- No NetGUID bunch, no `bHasMustBeMappedGUIDs`, no `bExportGUIDs`, no inline-export GUID stream, no
  GuidCache. None exist in 7258. Do not emit or parse any of these. **[H]**
- The "is this object exported inline vs separate bunch" question (from the task) is **moot**:
  static objects are referenced by a pre-agreed PackageMap index (no inline export needed), and
  dynamic objects are referenced by channel index (the channel-open IS the "export"). **[H]**

---

## 6. Source citations (file : symbol : line)

| Claim | UE3 source | VNGame.exe |
|-------|-----------|------------|
| Object-ref codec, 1 bit + ranged int | `Core/UnCoreNet.cpp` stub; `Engine/UnNetDrv.cpp:97` `UPackageMapLevel::SerializeObject` | `0x140696070` (net override) |
| Dynamic bound = MAX_CHANNELS=2048 | `Engine/UnConn.h:143` `#define MAX_NET_CHANNELS 2048`; used `UnNetDrv.cpp:107,185` | (disasm doc had 1023, = ChIndex field, not obj-ref) |
| Static bound = MAX_OBJECT_INDEX=0x80000000 | `Core/UnCoreNet.h:100` `#define MAX_OBJECT_INDEX (DWORD(1)<<31)`; `UnNetDrv.cpp:226` | `0x14069613a`/disasm `0x80000000` ✓ |
| `Bunch << Object` → SerializeObject | `Engine/UnBunch.cpp:30-34, 96-100` | — |
| Static index = ObjectBase + NetIndex | `Core/UnCoreNet.cpp:445` `ObjectToIndex`; `:210` `Compute` | — |
| NetIndex = linker export index | `Core/UnObj.cpp:1058` `SetNetIndex`, `:1570-1587` | — |
| Dynamic actor = its channel, no GUID | `Engine/UnNetDrv.cpp:68` `CanSerializeObject`, `:177-186` | — |
| New-actor header (archetype + pos/rot + NetPlayerIndex) | `Engine/UnChan.cpp:1769-1799` (write), `:1371-1419` (read) | `UActorChannel::ReceivedBunch` (vtable, calls `0x140696070`) |
| Compressed initial transform | `Engine/UnChan.cpp:15-29` `SerializeCompressedInitial` | — |
| Property handle max = ClassNetCache field count | `Core/UnCoreNet.h:31` `GetMaxIndex`; `:137` `GetClassNetCache`; `Engine/UnChan.cpp:1952` | per-class runtime (disasm doc §3.1) ✓ |
| PackageMap export = FPackageInfo list | `Core/UnCoreNet.h:75`; `UnCoreNet.cpp:264` `AddNetPackages` | `UnNetDrv.cpp` send loop `0x1404a7680` (post-doc §4.6) |
| SerializeName hardcoded-name path | `Core/UnCoreNet.cpp:79`; `MAX_NETWORKED_HARDCODED_NAME=1250` `Core/UnNames.h:16` | — |
| `NMT_ActorChannelFailure` on bad class | `Engine/UnChan.cpp:1388` | — (explains observed channel bClose) |

UE3 source root: `D:\RE-Tools\UE3-src\Development\Src`. All line numbers from that clone
(EngineVersion 7258 lineage, CodeRedModding/UnrealEngine3 @ main).
