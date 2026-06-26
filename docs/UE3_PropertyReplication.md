# UE3 Property-Replication Wire Format (EngineVersion 7258)

Source-of-truth mined from the **leaked UE3 engine source** at
`D:\RE-Tools\UE3-src` (CodeRedModding/UnrealEngine3, "Copyright 1998-2013 Epic
Games"). This pins the exact bit layout our emulator must emit so the **retail
RS2:Vietnam client** accepts our server-generated actors. Every claim cites a
file:line. This supersedes the open items in `RS2V_ActorReplication_7258.md` §2.2
/ §3.1 ("maxHandle could not be pinned").

Confidence: **[H]** = exact engine source, byte/bit-deterministic; **[M]** =
source-derived but depends on a runtime value (class field count, MaxPacket);
**[L]** = inference.

Primary sources:
- `Development/Src/Engine/Src/UnChan.cpp` — `UActorChannel::ReplicateActor` (send,
  L1695/L1926-1985), `UActorChannel::ReceivedBunch` (recv, L1359).
- `Development/Src/Core/Inc/UnCoreNet.h` — `FClassNetCache`, `FFieldNetCache`,
  `UPackageMap` (L9-178).
- `Development/Src/Core/Src/UnCoreNet.cpp` — `UPackageMap::GetClassNetCache`
  (L137-195) — the handle-index assignment.
- `Development/Src/Core/Src/UnClass.cpp` — `UClass::Link()` NetFields build +
  sort comparator (`IMPLEMENT_COMPARE_POINTER(UField,UnClass,…)` L1508).
- `Development/Src/Core/Src/UnProp.cpp` — every `UProperty::NetSerializeItem`.
- `Development/Src/Core/Src/UnMath.cpp` — `FVector/FRotator::SerializeCompressed`
  (L51, L84).
- `Development/Src/Core/Src/UnBits.cpp` — `FBitWriter/FBitReader::SerializeInt`,
  `SerializeBits`, `WriteIntWrapped` (L148-329).
- `Development/Src/Core/Src/UnMisc.cpp` — `operator<<(FArchive&, FString&)` (L835).
- `Development/Src/Engine/Src/UnNetDrv.cpp` — `UPackageMapLevel::SerializeObject`
  (NetGUID/object-ref codec, L97-231).

---

## 0. TL;DR — the three answers

1. **`maxHandle` (the SerializeInt Max for a property/function handle) =
   `ClassCache->GetMaxIndex()` = `FieldsBase + Fields.Num()`** — a **per-class
   runtime value** equal to *(the class's own net-field count) + (the cumulative
   net-field count of every super class)*. It is **NOT** a constant, **NOT**
   `MAX_CHANNELS`, **NOT** a packet size. (`UnCoreNet.h:31-34`,
   `UnChan.cpp:1952`/`1359`-region.) **[H mechanism / M exact integer per class]**

2. **The HANDLE of a property** = its index in the class's sorted `NetFields`
   list, offset by `FieldsBase`. The list is **all `CPF_Net` properties + all
   `FUNC_Net` functions (that don't override a super function)**, gathered per
   class in `UStruct::Link` field order, then **sorted ascending by
   `UField::GetNetIndex()`** (`UnClass.cpp:1508`, `UClass::Link`). Indices are
   assigned **base-class-first**: a subclass's first net field gets index =
   parent's `GetMaxIndex()`. (`UnCoreNet.cpp:151-168`.) **[H]**

3. **Per-property value serialization** = the property's virtual
   `NetSerializeItem` (`UnProp.cpp`): bool=1 bit, byte=8 bits (or
   `ceilLog2(enum)` bits for enums), int/float=32 bits LE, FString=`INT len +
   chars`, object/NetGUID=`SerializeObject` (1 selector bit + ranged int),
   Vector/Rotator=`SerializeCompressed`. Full table in §3. **[H]**

There is **no end-of-properties sentinel byte**. The property loop simply runs
until the bunch's bits are exhausted; the receiver's `ReadInt` then errors and
`GetFromIndex` returns NULL, ending the loop. (`UnChan.cpp:1359` recv loop.) **[H]**

---

## 1. Building the net field list — `UClass::Link` (the HANDLE namespace)

`Development/Src/Core/Src/UnClass.cpp`, `UClass::Link()`, verbatim:

```cpp
if( !GIsEditor )
{
    NetFields.Empty();
    ClassReps = (SuperStruct != NULL) ? GetSuperClass()->ClassReps : TArray<FRepRecord>();
    for( TFieldIterator<UField> It(this,FALSE); It; ++It )   // FALSE = THIS class only, not supers
    {
        UProperty* P;
        UFunction* F;
        if( (P=Cast<UProperty>(*It))!=NULL )
        {
            if( P->PropertyFlags&CPF_Net )                  // only replicated properties
            {
                NetFields.AddItem( *It );
                if( P->GetOuter()==this )
                {
                    P->RepIndex = ClassReps.Num();
                    for( INT i=0; i<P->ArrayDim; i++ )
                        new(ClassReps)FRepRecord(P,i);
                }
            }
        }
        else if( (F=Cast<UFunction>(*It))!=NULL )
        {
            if( (F->FunctionFlags&FUNC_Net) && !F->GetSuperFunction() )  // replicated, non-override RPCs
                NetFields.AddItem( *It );
        }
    }
    NetFields.Shrink();
    Sort<USE_COMPARE_POINTER(UField,UnClass)>( &NetFields(0), NetFields.Num() );
}
```

The sort comparator (`UnClass.cpp:1508`):

```cpp
IMPLEMENT_COMPARE_POINTER( UField, UnClass, { return A->GetNetIndex() - B->GetNetIndex(); } )
```

So `NetFields` for a class is the set **{CPF_Net properties} ∪ {FUNC_Net
functions with no super-function}**, **declared in this class only** (the iterator
is `(this,FALSE)`), **sorted ascending by `UField::GetNetIndex()`**. **[H]**

> **`GetNetIndex()` semantics:** UE3 assigns each `UField` a stable per-outer net
> index at load/link time (it is the field's serialization order within its
> owning struct — effectively **UnrealScript declaration order**). This is why our
> RE doc's "handle = var declaration order" (§3.2) is correct: properties replicate
> in declaration order **within each class**, and inheritance stacks parent fields
> before child fields (see §2). Both properties and replicated functions share this
> one index space — which is why the `ServerMove` RPC handle (664) and property
> handles (23/51/64) come from the same `SerializeInt(maxHandle)` namespace. **[H
> for the shared namespace + declaration-order rule; M that GetNetIndex == raw decl
> order for every field — RS2's native classes may inject native fields.]**

`ClassReps` (the flat send-list, one entry per array element) is built in the
same pass: `P->RepIndex = ClassReps.Num()`. `ReplicateActor` iterates `ClassReps`,
not `NetFields`, but maps each property back through `GetFromField` to recover its
`FieldNetIndex` (§3). **[H]**

---

## 2. `maxHandle` and the handle→property map — `FClassNetCache`

### 2.1 The struct (`Core/Inc/UnCoreNet.h:9-70`) **[H]**

```cpp
class FFieldNetCache {
public:
    UField* Field;
    INT FieldNetIndex;      // <-- THE HANDLE written on the wire
    INT ConditionIndex;
};

class FClassNetCache {
public:
    INT GetMaxIndex() { return FieldsBase+Fields.Num(); }      // <-- THE maxHandle
    FFieldNetCache* GetFromField( UObject* Field );            // property -> cache (send)
    FFieldNetCache* GetFromIndex( INT Index );                 // handle -> cache (recv)
private:
    INT FieldsBase;                 // = Super->GetMaxIndex(); 0 for the root
    FClassNetCache* Super;
    UClass* Class;
    TArray<FFieldNetCache> Fields;  // this class's own net fields, in NetFields order
    TMap<UObject*,FFieldNetCache*> FieldMap;
};
```

`GetFromIndex` walks the super chain to find which class owns a handle
(`UnCoreNet.h:51-61`):

```cpp
FFieldNetCache* GetFromIndex( INT Index ) {
    for( FClassNetCache* C=this; C; C=C->Super )
        if( Index>=C->FieldsBase && Index<C->FieldsBase+C->Fields.Num() )
            return &C->Fields(Index-C->FieldsBase);
    return NULL;
}
```

### 2.2 How the index is assigned — `UPackageMap::GetClassNetCache` (`Core/Src/UnCoreNet.cpp:137-195`) **[H]**

```cpp
FClassNetCache* UPackageMap::GetClassNetCache( UClass* Class )
{
    FClassNetCache* Result = ClassFieldIndices.FindRef(Class);
    if( !Result && SupportsObject(Class) )
    {
        Result = ClassFieldIndices.Set( Class, new FClassNetCache(Class) );
        Result->Super = NULL;
        Result->RepConditionCount = 0;
        Result->FieldsBase = 0;
        if( Class->GetSuperClass() )
        {
            Result->Super              = GetClassNetCache(Class->GetSuperClass());
            Result->RepProperties      = Result->Super->RepProperties;
            Result->RepConditionCount  = Result->Super->RepConditionCount;
            Result->FieldsBase         = Result->Super->GetMaxIndex();   // <-- stack on parent
        }
        Result->Fields.Empty( Class->NetFields.Num() );
        for( INT i=0; i<Class->NetFields.Num(); i++ )
        {
            UField* Field = Class->NetFields(i);
            if( SupportsObject(Field) )
            {
                INT ConditionIndex = INDEX_NONE;
                INT ThisIndex      = Result->GetMaxIndex();             // = FieldsBase + Fields.Num()
                UProperty* ItP     = Cast<UProperty>(Field,CLASS_IsAUProperty);
                if( ItP )
                    ConditionIndex = Result->RepConditionCount++;
                new(Result->Fields)FFieldNetCache( Field, ThisIndex, ConditionIndex );
            }
        }
        ...
    }
    return Result;
}
```

**The handle assignment rule, exactly:**

```
FieldsBase(Class)        = Super ? GetMaxIndex(Super) : 0
FieldNetIndex(field i)   = FieldsBase + i'          (i' = field's position among the
                                                     SupportsObject-passing NetFields)
GetMaxIndex(Class)       = FieldsBase + count(this class's net fields)
```

So indices are a contiguous range per class, stacked base-class-first up the
inheritance chain. For an actor of class `C` whose chain is
`Object < Actor < … < C`, the handle space is
`[0 .. GetMaxIndex(Object)) ∪ [.. Actor ..) ∪ … ∪ [FieldsBase(C) .. GetMaxIndex(C))`.

**`maxHandle` written/read on the wire is always the LEAF class's
`GetMaxIndex()`** (the actor's own class), because `ClassCache` in
`ReplicateActor`/`ReceivedBunch` is `GetClassNetCache(Actor->GetClass())`. **[H]**

> **Implication for our emulator:** to compute `maxHandle` for, e.g.,
> `ROPlayerReplicationInfo`, sum the replicated-field counts of
> `Object`+`Actor`+`Info`+`ReplicationInfo`+`PlayerReplicationInfo`+
> `ROPlayerReplicationInfo` (only `CPF_Net` properties + `FUNC_Net` non-override
> functions per class). We must reproduce that exact count from the decompiled
> `.uc` sources at `D:\RE-Tools\rs2-source`. The count is what `SerializeInt`'s
> minimal-bit width keys off (§4). **[M — needs the per-class net-field tally.]**

---

## 3. The send/receive loop (per-property layout)

### 3.1 Send — `UActorChannel::ReplicateActor` (`Engine/Src/UnChan.cpp:1926-1985`) **[H]**

```cpp
for( INT* iPtr=Reps; iPtr<LastRep; iPtr++ )
{
    FRepRecord* Rep    = &ActorClass->ClassReps(*iPtr);
    UProperty*  It     = Rep->Property;
    INT         Index  = Rep->Index;                       // array element
    INT         Offset = It->Offset + Index*It->ElementSize;

    // ROLE SWAP on the wire:
    FFieldNetCache* FieldCache =
          It->GetFName()==NAME_Role       ? ClassCache->GetFromField(Connection->Driver->RemoteRoleProperty)
        : It->GetFName()==NAME_RemoteRole ? ClassCache->GetFromField(Connection->Driver->RoleProperty)
        :                                   ClassCache->GetFromField(It);

    Bunch.WriteIntWrapped(FieldCache->FieldNetIndex, ClassCache->GetMaxIndex());  // <-- HANDLE
    if( It->ArrayDim != 1 )
    {
        BYTE Element = Index;
        Bunch << Element;                                  // 8-bit array index, only if ArrayDim>1
    }
    UBOOL Mapped = It->NetSerializeItem( Bunch, Connection->PackageMap, (BYTE*)Actor + Offset ); // VALUE
    ...
}
```

Three load-bearing facts:

1. **Handle** = `WriteIntWrapped(FieldNetIndex, GetMaxIndex())` — a minimal-bit
   ranged int (§4 algorithm). **[H]**
2. **Role swap** — when the property is `Role`, the wire handle is
   `RemoteRole`'s, and vice versa (L1942-1946). The client receives the server's
   `Role` as its `RemoteRole` and the server's `RemoteRole` as its `Role`. This is
   how the owning client sees its own pawn/PC as `Role=ROLE_AutonomousProxy`.
   Confirms `RS2V_ActorReplication_7258.md` §2.3. **[H]**
3. **Array index** — only emitted when `ArrayDim != 1`, as a raw `BYTE` (8 bits),
   *after* the handle and *before* the value. Static-size-1 properties emit no
   index. **[H]**

### 3.2 Receive — `UActorChannel::ReceivedBunch` (`UnChan.cpp:1359` region) **[H]**

```cpp
INT             RepIndex   = Bunch.ReadInt( ClassCache->GetMaxIndex() );
FFieldNetCache* FieldCache = Bunch.IsError() ? NULL : ClassCache->GetFromIndex( RepIndex );
while (FieldCache || bJustSpawned)
{
    UProperty* ReplicatedProp = Cast<UProperty>(FieldCache->Field, CLASS_IsAUProperty);
    BYTE Element = 0;
    if( ReplicatedProp->ArrayDim != 1 )
        Bunch << Element;                                   // 8-bit array index
    INT   Offset = ReplicatedProp->Offset + Element*ReplicatedProp->ElementSize;
    BYTE* Data   = DestActor ? (DestActor+Offset) : NewZeroed<BYTE>(...);
    ReplicatedProp->NetSerializeItem( Bunch, Connection->PackageMap, Data );  // VALUE

    RepIndex   = Bunch.ReadInt( ClassCache->GetMaxIndex() );  // next handle
    FieldCache = Bunch.IsError() ? NULL : ClassCache->GetFromIndex( RepIndex );
}
```

**Terminator:** none explicit. The loop reads handle→value→handle→value… until
`ReadInt` runs past the bunch end (`Bunch.IsError()`), nulling `FieldCache`. So
our emitter writes exactly N (handle,value) pairs and stops — **no zero handle,
no length field**. The bunch's own `BunchDataBits` (set in the bunch header)
bounds it. **[H]**

> One subtlety: `ReadInt(Max)` for a small `Max` can return a value `< Max` even
> from trailing zero padding, so an actor whose `GetMaxIndex()` is small could
> mis-read pad bits as a spurious handle. UE3 avoids this because a bunch is
> bit-tight (the bunch length is exact) and any over-read sets `IsError`. Our
> emitter must likewise size the bunch's `BunchDataBits` to exactly the bits
> written. **[H]**

---

## 4. The ranged-int primitive (handle, NetGUID index, compressed-vector fields)

`FBitWriter::WriteIntWrapped` / `SerializeInt` and `FBitReader::SerializeInt` are
**the same minimal-bit algorithm** used for handles, NetGUID indices, and inside
`SerializeCompressed`. `Core/Src/UnBits.cpp`:

**Write (`UnBits.cpp:206-221`, `WriteIntWrapped`; identical core in `SerializeInt`
L180-200):**
```cpp
DWORD NewValue=0;
for( DWORD Mask=1; NewValue+Mask<ValueMax && Mask; Mask*=2, Num++ )
    if( Value&Mask )
    {
        Buffer(Num>>3) += GShift[Num&7];   // set bit Num (LSB-first within each byte)
        NewValue += Mask;
    }
```

**Read (`UnBits.cpp:308-323`):**
```cpp
Value=0;
for( DWORD Mask=1; Value+Mask<ValueMax && Mask; Mask*=2, Pos++ )
    if( Buffer(Pos>>3) & GShift[Pos&7] )
        Value |= Mask;
```

Properties of this codec (all **[H]**):
- **Bit count is value-dependent**, not fixed: it emits bits LSB-first, stopping as
  soon as `NewValue+Mask >= ValueMax`. The maximum width is
  `ceilLogTwo(ValueMax)` bits (used for the overflow check at L188). A value far
  below `ValueMax` can terminate early (fewer bits) — this is the "minimal-bit
  ranged int" our RE doc (§2.1) observed.
- **Bit order: LSB-first within each byte** (`GShift[Num&7]`, `Num` increasing).
  Matches the bunch-header decode in `RS2V_ActorReplication_7258.md`.
- `SerializeInt` **clamps** out-of-range values and logs; `WriteIntWrapped`
  wraps. Both produce identical bits for in-range values. The handle path uses
  `WriteIntWrapped` (send) / `ReadInt`→`SerializeInt` (recv).
- `appCeilLogTwo(1)==0` ⇒ a `ValueMax` of 0 or 1 writes **zero bits**. So a class
  with one net field (maxHandle==1) writes a 0-bit handle. **[H]**

`SerializeBits` (`UnBits.cpp:148`) writes raw bits LSB-first; the 1-bit fast path
checks `Src[0] & 0x01`. `Serialize(bytes)` = `SerializeBits(bytes*8)`, i.e.
byte values are written little-endian, 8 bits each, bit-packed (NOT
byte-aligned). **[H]**

---

## 5. Per-type VALUE serialization table (`UProperty::NetSerializeItem`)

All in `Core/Src/UnProp.cpp`. On the shipping **little-endian** client/server,
`Ar << x` ⇒ `ByteOrderSerialize` ⇒ `Serialize(&x, sizeof)` ⇒ `sizeof*8` bits,
**little-endian, bit-packed** (`Core/Inc/UnArc.h:25` `#define ByteOrderSerialize
Serialize`; operators L372-431).

| Property type | NetSerializeItem | Wire bits | Notes | Conf |
|---|---|---|---|---|
| **bool** (`UBoolProperty`, L1432) | `SerializeBits(&Value,1)` | **1 bit** | LSB-first; the property's `BitMask` selects the bitfield | **[H]** |
| **byte / enum** (`UByteProperty`, L656) | `SerializeBits(Data, Enum ? appCeilLogTwo(Enum->NumEnums()-1) : 8)` | **8 bits** plain byte; **`ceilLog2(NumEnums-1)` bits** for an enum byte | enum width excludes the autogen `_MAX`; e.g. `ENetRole`(4 enums+_MAX→ `ceilLog2(4)=2` bits) | **[H]** |
| **int** (`UIntProperty`, L818) | `Ar << *(INT*)Data` | **32 bits LE** | full width | **[H]** |
| **float** (`UFloatProperty`, L1532) | `Ar << *(FLOAT*)Data` | **32 bits LE** (IEEE-754) | full width, no compression | **[H]** |
| **FString** (`UStrProperty`) | no override → base `UProperty::NetSerializeItem` (L427) → `SerializeItem` → `Ar << FString` (`UnMisc.cpp:835`) | **`INT SaveNum` (32 bits LE)** then the chars | `SaveNum = +Num` for pure-ANSI (1 byte/char), `-Num` for Unicode (2 bytes/char). **`Num` INCLUDES the trailing NUL** (so "Krill"→`SaveNum=6`, then `K r i l l \0`). Empty string ⇒ `SaveNum=0`, no chars | **[H]** |
| **object / actor ref / NetGUID** (`UObjectProperty`, L1922) | `Map->SerializeObject(Ar, PropertyClass, obj)` | **1 selector bit + ranged int** — see §6 | this is the Team ref, Owner, Instigator, Controller, PlayerReplicationInfo, ViewTarget, etc. | **[H]** |
| **Vector** (`UStructProperty` NAME_Vector, L4194) | `FVector::SerializeCompressed(Ar)` | compressed — see §7.1 | used for `Velocity`, replicated locations that go through a Vector prop | **[H]** |
| **Rotator** (`UStructProperty` NAME_Rotator, L4199) | `FRotator::SerializeCompressed(Ar)` | compressed — see §7.2 | `Rotation`, `RemoteViewPitch/Yaw` are bytes (separate), full rotators use this | **[H]** |
| **Quat** (`UStructProperty` NAME_Quat, L4204) | X,Y,Z as 3×32-bit float; W reconstructed | **96 bits** | normalized, W≥0 implied | **[H]** |
| other struct | recursive `SerializeItem` of members | byte-serialized | non-net-special structs fall through to full serialize | **[M]** |
| array (`UArrayProperty`, L3365) | count + per-element NetSerializeItem | — | dynamic-array net path (rare in initial blocks) | **[M]** |

Notes:
- **Properties are written `Ar << x` are bit-packed, not byte-aligned.** An int
  after a 1-bit bool starts mid-byte. The whole bunch is one continuous bitstream.
- `RemoteViewPitch`/`RemoteViewYaw` in `Pawn` are `byte` properties (8-bit each),
  **not** a Rotator — they replicate the compressed view angle. (Pawn.uc decl;
  byte path above.) **[M]**

---

## 6. Object / NetGUID codec — `UPackageMapLevel::SerializeObject` (`Engine/Src/UnNetDrv.cpp:97-231`) **[H]**

This is the **source-of-truth** for every object reference (Owner, Team,
Instigator, Controller, the class ref + actor ref in `SerializeNewActor`, RPC
object params). **It corrects the flag-bit polarity stated in
`RS2V_ActorReplication_7258.md` §2.1.**

```cpp
// SAVE (server side):
if (Actor && dynamic && has a channel) {
    BYTE B=1; Ar.SerializeBits(&B,1);                 // selector = 1  -> DYNAMIC
    Ar.SerializeInt(Index, MAX_CHANNELS);             //   value = the actor's CHANNEL INDEX
}
else if (Object==NULL || not-yet-init) {
    BYTE B=1; Ar.SerializeBits(&B,1);                 // selector = 1, Index=0  -> None
    Ar.SerializeInt(Index/*=0*/, MAX_CHANNELS);
}
else {                                                 // static / package-map object (e.g. a CLASS)
    BYTE B=0; Ar.SerializeBits(&B,1);                 // selector = 0  -> STATIC
    Index = ObjectToIndex(Object);
    Ar.SerializeInt(Index, MAX_OBJECT_INDEX);          //   value = PackageMap net index
}
```

LOAD mirrors it (L100-167):
```cpp
BYTE B; Ar.SerializeBits(&B,1);
if (B)  { Ar.SerializeInt(Index, MAX_CHANNELS);     /* Index<=0 => None; else channel->actor */ }
else    { Ar.SerializeInt(Index, MAX_OBJECT_INDEX); Object = IndexToObject(Index,1); /* package map */ }
```

Constants:
- `MAX_OBJECT_INDEX = DWORD(1)<<31` (`UnCoreNet.h:100`) → static-object index field
  is up to **31 bits** (minimal-bit, value-dependent per §4). Matches our disasm's
  `0x80000000` branch. **[H]**
- `MAX_CHANNELS = MAX_NET_CHANNELS` = **2048** in stock UE3 (`UnConn.h:143,153`).
  **RS2 ships a customized value: our disasm pinned the dynamic branch at
  `SerializeInt(.,1023)`**, i.e. RS2's `MAX_NET_CHANNELS = 1024` (consistent with
  the bunch-header `ChIndex` max of 1023 in `RS2V_ActorReplication_7258.md` §"Bunch
  header field maxes"). **Use 1024 for RS2-7258, not 2048.** **[H that source uses
  MAX_CHANNELS; H from disasm that RS2's value yields max 1023/1024.]**

> **Polarity correction [H]:** the selector bit is **`1` = dynamic (channel
> index)**, **`0` = static (package-map index)**. `RS2V_ActorReplication_7258.md`
> §2.1 had it inverted ("flag==1 static / flag==0 dynamic"). The capture's first
> NetGUID bit = **0** on the PlayerController open is therefore the **STATIC**
> branch = a **class reference into the PackageMap** (`SerializeInt(.,
> MAX_OBJECT_INDEX)`), which is exactly right for the class ref that opens a
> channel. The per-actor channel ref (when one actor references another open
> actor, e.g. PRI→Team) uses bit **1** + the target's channel index. This also
> explains §6.0 of the RE doc: `[classNetGUID]` (static, bit 0) then
> `[actorNetGUID]`.

---

## 7. Compressed Vector & Rotator (`Core/Src/UnMath.cpp`)

### 7.1 `FVector::SerializeCompressed` (L51-77) **[H]**

```cpp
INT IntX=appRound(X), IntY=appRound(Y), IntZ=appRound(Z);            // rounded to integers!
DWORD Bits = Clamp<DWORD>( appCeilLogTwo(1 + Max3(Abs(IntX),Abs(IntY),Abs(IntZ))), 1, 20 ) - 1;
Ar.SerializeInt( Bits, 20 );                  // Bits in [0..19], written ranged-to-20
INT   Bias = 1<<(Bits+1);
DWORD Max  = 1<<(Bits+2);
Ar.SerializeInt( DX=IntX+Bias, Max );         // each component: ranged int, width ~Bits+2
Ar.SerializeInt( DY=IntY+Bias, Max );
Ar.SerializeInt( DZ=IntZ+Bias, Max );
```

Wire layout: `SerializeInt(Bits,20)` then three `SerializeInt(component+Bias,
1<<(Bits+2))`. **Components are rounded to integers — sub-unit precision is lost.**
This is the compressed form used by `Velocity` and any Vector-typed replicated
prop. **[H]**

> Location for the **owning** pawn/PC is normally client-predicted, not sent; when
> a full position is sent it goes via this Vector codec or inside `SerializeNewActor`
> (open item in `RS2V_ActorReplication_7258.md` §2.2 — whether the channel-open
> header reads Location/Rotation before the property loop; that header is in native
> `UActorChannel::ReceivedBunch`/`SerializeNewActor`, see §8). **[M]**

### 7.2 `FRotator::SerializeCompressed` (L84-128) **[H]**

```cpp
BYTE BytePitch=Pitch>>8, ByteYaw=Yaw>>8, ByteRoll=Roll>>8;          // 16-bit angle -> top byte
BYTE B;
B=(BytePitch!=0); Ar.SerializeBits(&B,1); if(B) Ar<<BytePitch;      // 1 presence bit + optional byte
B=(ByteYaw  !=0); Ar.SerializeBits(&B,1); if(B) Ar<<ByteYaw;
B=(ByteRoll !=0); Ar.SerializeBits(&B,1); if(B) Ar<<ByteRoll;
```

Wire layout: **3 presence bits** (pitch, yaw, roll), each followed by an **8-bit
byte only if non-zero**. A zero component costs 1 bit; a non-zero one costs 9 bits.
Angle precision = top 8 bits of the 16-bit UE rotator (`>>8`), i.e. 256
steps/revolution. So a standing actor facing yaw only ⇒ `0` (pitch) `1`+byte
(yaw) `0` (roll) = 11 bits. **[H]**

---

## 8. What is still NATIVE (not in this source tree) — open items

- **`SerializeNewActor` / `UActorChannel::ReceivedBunch` actor-open header.** The
  channel-open codec (actor NetGUID, class NetGUID, and whether Location/Rotation
  are read in the header vs. as ordinary `Actor` initial properties) is in the
  **native** `UActorChannel`. In THIS leaked tree, `ReceivedBunch` (`UnChan.cpp`)
  contains the property loop (§3.2) but the new-actor-spawn sub-block
  (`bJustSpawned` path) calls into spawn code. Cross-reference with the RS2 disasm
  in `RS2V_ActorReplication_7258.md` §2 (`SerializeObject @0x140696070`,
  `SerializeNewActor`). The property-block format (§3-§7) is fully pinned here and
  is correct regardless of how the open header is split. **[H for §3-§7 / M for the
  open-header field split — unchanged from the RE doc's open item.]**
- **The exact per-class net-field COUNT** (to compute `maxHandle` integers) must be
  tallied from the decompiled `.uc` at `D:\RE-Tools\rs2-source` (count `CPF_Net`
  vars + `FUNC_Net` non-override functions per class, up the chain). The mechanism
  is pinned (§2); the integers are a counting exercise against the RS2 scripts.
  **[M]**

---

## 9. Emitter recipe (byte-exact initial property block)

To serialize an initial property block for an actor of class `C` (e.g.
`ROPlayerReplicationInfo`):

1. Build the net-field list per §1: walk `C` and every super class; for each class
   collect its `CPF_Net` properties + `FUNC_Net` (non-override) functions **in
   UnrealScript declaration order** (= `GetNetIndex` order). Concatenate
   **base-class-first**. Assign `FieldNetIndex` = position in that concatenation.
   `maxHandle = total count`. (§1, §2.) **[H mechanism]**
2. For each property you are sending (only the ones whose replication condition is
   true for this initial bunch — `bNetInitial`, `bNetOwner`, `Role==Authority`,
   etc., per `RS2V_ActorReplication_7258.md` §3.2), in **ascending FieldNetIndex
   order**:
   a. Write the handle: `WriteIntWrapped(FieldNetIndex, maxHandle)` (§4 LSB-first
      minimal-bit). **Apply the Role↔RemoteRole swap** (§3.1). 
   b. If `ArrayDim>1`: write the array index as an 8-bit byte.
   c. Write the value by type (§5 table): bool=1 bit, byte=8 bits (enum=ceilLog2
      bits), int/float=32 bits LE, FString=`INT len(incl NUL)` + chars,
      object=`SerializeObject` (§6: bit 0 + 31-bit packagemap index for a static
      class/object, or bit 1 + channel index for an open actor), Vector/Rotator =
      §7 compressed.
3. **Do not write any terminator.** Set the bunch's `BunchDataBits` to the exact
   number of bits written. The client loops until its `ReadInt` over-reads (§3.2).
4. For the **role fields** specifically: send `RemoteRole=ROLE_AutonomousProxy(2)`,
   `Role=ROLE_Authority(3)` (wire-swapped so client sees `Role=AutonomousProxy`),
   `bNetOwner=true` for the owning client's PC/Pawn. `ENetRole` is a `byte`
   property whose enum (`ROLE_None..ROLE_MAX`) ⇒ `ceilLog2(NumEnums-1)` bits ≈
   **2 bits** each, NOT 8. (§5 byte/enum row.) **[H]**

---

## Appendix: confidence summary

| Claim | Conf |
|---|---|
| `maxHandle = ClassCache->GetMaxIndex() = FieldsBase + Fields.Num()` | **[H]** |
| Handle = sorted-NetFields position + FieldsBase, base-class-first | **[H]** |
| NetFields = CPF_Net props + FUNC_Net non-override funcs, sorted by GetNetIndex | **[H]** |
| Role↔RemoteRole wire swap | **[H]** |
| No terminator; loop ends on bunch-bit exhaustion | **[H]** |
| bool=1bit, int/float=32 LE, byte=8 (enum=ceilLog2) | **[H]** |
| FString = INT len (incl NUL, ±for ANSI/Unicode) + chars | **[H]** |
| Object ref = 1 selector bit (1=dynamic/channel, 0=static/packagemap) + ranged int | **[H]** |
| MAX_OBJECT_INDEX = 1<<31; RS2 MAX_CHANNELS = 1024 (stock 2048) | **[H]** |
| Vector SerializeCompressed = SerializeInt(Bits,20)+3×ranged(component+Bias) | **[H]** |
| Rotator SerializeCompressed = 3×(1 presence bit + optional 8-bit top-byte) | **[H]** |
| Ranged-int = LSB-first minimal-bit, value-dependent width, max ceilLog2(Max) | **[H]** |
| Exact integer maxHandle per RS2 class (the field tally) | **[M]** |
| SerializeNewActor open-header field split (Location/Rotation placement) | **[M]** |
