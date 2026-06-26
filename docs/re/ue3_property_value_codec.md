# UE3 Property-VALUE Wire Codec (authoritative)

Reverse-engineered from UE3 C++ source at `D:\RE-Tools\UE3-src\Development\Src`.
This is the ground-truth codec for replicating property VALUES inside an actor-channel
bunch (RS2V / UE3 EngineVersion 7258). Every byte/bit layout below is taken directly
from the engine source; file:line citations are given so it can be re-verified.

All multi-bit values are written into an `FBitWriter` (LSB-first within a byte, bytes in
order). The reader is `FBitReader`. There is NO byte alignment between fields inside a
bunch — everything is bit-packed.

---

## 0. Primitive bit operations (the foundation)

Source: `Core/Src/UnBits.cpp`.

### 0.1 SerializeBits(void* V, INT n) — raw n bits
- `Core/Src/UnBits.cpp:148` (writer), `:286` (reader).
- Copies `n` bits from `V` starting at bit 0 of `V`, LSB-first, into the stream.
- 1-bit case special-cased: writes `V[0] & 0x01`.
- A whole BYTE via `Ar << byte` = `Serialize(1)` = `SerializeBits(&b, 8)` = 8 bits, LSB-first.

### 0.2 SerializeInt(DWORD& Value, DWORD ValueMax) — RANGED int (the critical one)
Writer `Core/Src/UnBits.cpp:180`, Reader `:308`.

```
// WRITE (FBitWriter::SerializeInt)
NewValue = 0;
for (Mask = 1; NewValue + Mask < ValueMax && Mask != 0; Mask <<= 1, Num++) {
    if (Value & Mask) { Buffer(Num>>3) += GShift[Num&7]; NewValue += Mask; }
}
// READ (FBitReader::SerializeInt)
Value = 0;
for (Mask = 1; Value + Mask < ValueMax && Mask != 0; Mask <<= 1, Pos++) {
    if (Buffer(Pos>>3) & GShift[Pos&7]) Value |= Mask;
}
```

Key facts:
- Bits are emitted **LSB-first**.
- The loop STOPS as soon as `NewValue + Mask >= ValueMax`. The terminating
  (high) bit is **never written** — it is implicit. So the count of bits is
  **data-dependent**, but bounded by `ceil(log2(ValueMax))`.
- `GShift[i] = (1 << i)`, i.e. bit `i` within the current byte.
- Number of bits actually emitted = number of loop iterations until
  `accumulated + Mask >= ValueMax`. For `Value == 0` you still emit one bit per
  iteration (all zero) until `Mask >= ValueMax`, i.e. `ceil(log2(ValueMax))` zero bits.

Worked examples:
- `SerializeInt(0, 1024)` -> Mask runs 1,2,...,512 (0+512<1024 true), then Mask=1024
  fails 0+1024<1024 -> stop. = **10 zero bits**.
- `SerializeInt(0, 2048)` -> 11 zero bits. (See MAX_CHANNELS note in §3.)
- `SerializeInt(5, 8)`  (binary 101): Mask=1:0+1<8,bit0=1->NV=1; Mask=2:1+2<8,bit1=0;
  Mask=4:1+4<8,bit4=1->NV=5; Mask=8:5+8<8 false stop. = 3 bits "1 0 1" (LSB first).

### 0.3 WriteIntWrapped(DWORD Value, DWORD ValueMax)
`Core/Src/UnBits.cpp:206`. **Bit-identical to SerializeInt** except it does NOT
clamp/error when `Value > ValueMax` (it just wraps via the mask loop). Used to write
the property handle (FieldNetIndex). For decoding it is the same as `ReadInt(Max)`.

### 0.4 ReadInt(Max) = SerializeInt wrapper returning the value (`:324`).

### 0.5 Multi-byte scalars via `Ar << x`
`Ar << INT/FLOAT/DWORD/WORD` routes through `FArchive` byte-order serialize ->
`Serialize(&x, sizeof)` -> `SerializeBits(&x, sizeof*8)`. Result: the native
little-endian bytes are emitted as a raw bit run.
- `INT`   = 32 bits (LE)
- `FLOAT` = 32 bits (IEEE-754, LE byte order)
- `WORD`  = 16 bits, `BYTE` = 8 bits, `QWORD` = 64 bits.

---

## 1. Per-property framing inside an actor bunch

Source: `Engine/Src/UnChan.cpp` — `UActorChannel::ReplicateActor()` (write, :1926-1960)
and `UActorChannel::ReceivedBunch()` (read, :1477-1577).

A bunch carries a sequence of property records, then RPC records. Each record:

```
[handle]  = WriteIntWrapped(FieldNetIndex, ClassCache->GetMaxIndex())   // ranged int
[element] = BYTE, 8 bits  -- ONLY present if property->ArrayDim != 1 (STATIC C array)
[value]   = property->NetSerializeItem(...)                              // see §2
```

- **handle** is read back with `Bunch.ReadInt(ClassCache->GetMaxIndex())`
  (`UnChan.cpp:1477`, `:1576`). `GetMaxIndex()` = `FieldsBase + Fields.Num()`
  (`Core/Inc/UnCoreNet.h:31`). These are the per-class maxHandle values already
  catalogued: ROPlayerController=531, GRI=184, TeamInfo=78.
- The receive loop continues reading `handle/value` pairs until a handle maps to no
  field (`GetFromIndex` returns NULL) or the bunch errors/ends
  (`UnChan.cpp:1485,1500,1576`). There is **no explicit count and no terminator
  value** — running out of bits / an out-of-range handle ends the property stream.
- **element index byte** (`Bunch << Element`, 8 bits) is emitted **only for fixed-size
  C arrays** (`UProperty::ArrayDim != 1`, e.g. `int Foo[4]`), `UnChan.cpp:1953-1957`
  (write) / `:1514-1516` (read). Element must be `< ArrayDim` or it is clamped &
  discarded. Max replicable element index is 255 (write guards at `:1934`).

---

## 2. NetSerializeItem per UProperty subclass

Source: `Core/Src/UnProp.cpp`. Each subclass overrides `NetSerializeItem`.
Base `UProperty::NetSerializeItem` (`:427`) just does plain SerializeItem (not used on
the net path for the typed subclasses below).

### 2.1 UByteProperty (`:656`)
```
Ar.SerializeBits(Data, Enum ? appCeilLogTwo(Enum->NumEnums() - 1) : 8);
```
- Plain BYTE (no enum): **8 bits**, LSB-first.
- ENUM byte: **`ceil(log2(NumEnums - 1))` bits** (a raw SerializeBits run, NOT ranged
  SerializeInt). `NumEnums` INCLUDES the autogenerated trailing `_MAX` entry.
  - e.g. enum with 4 real values has `NumEnums == 5` (4 + `_MAX`) ->
    `ceil(log2(5-1)) = ceil(log2(4)) = 2 bits`.
  - enum with 3 real values: NumEnums=4 -> ceil(log2(3)) = 2 bits.
  - enum with 5 real values: NumEnums=6 -> ceil(log2(5)) = 3 bits.
- `appCeilLogTwo(n)` returns the smallest k with `(1<<k) >= n`; `appCeilLogTwo(0)=0`,
  `appCeilLogTwo(1)=0` (`Core/Inc/UnFile.h:2196`).

### 2.2 UIntProperty (`:818`)
```
Ar << *(INT*)Data;   // 32 bits, little-endian
```

### 2.3 UFloatProperty (`:1532`)
```
Ar << *(FLOAT*)Data; // 32 bits IEEE-754, little-endian
```

### 2.4 UBoolProperty (`:1432`)
```
BYTE v = (bitfield & BitMask) != 0;
Ar.SerializeBits(&v, 1);   // exactly 1 bit
```
One bit. No handle-per-bit packing; each replicated bool is its own property record
(handle + 1 value bit).

### 2.5 UStrProperty (FString) — uses base path -> `FArchive operator<<(FString&)`
`UStrProperty` has **no** NetSerializeItem override, so it falls to
`UProperty::NetSerializeItem` -> `SerializeItem` -> `Ar << FString`.
Source `Core/Src/UnMisc.cpp:835`.

Wire layout:
```
[INT  SaveNum]                       // 32 bits, LE, SIGNED
[chars ...]                          // |SaveNum| code units
```
- `SaveNum = +Length` if the string is pure-ASCII -> chars are **1 byte each (ANSICHAR)**.
- `SaveNum = -Length` otherwise -> chars are **2 bytes each (UNICHAR / UTF-16LE)**.
- **CRITICAL: Length INCLUDES the trailing NUL terminator.** UE3 `FString` is a
  `TArray<TCHAR>` whose `Num()` counts the `\0`. So:
  - "" (empty)  -> `SaveNum = 0`, zero chars.
  - "A"         -> `SaveNum = 2` (ansi), bytes `'A' 0x00`.
  - "AB"        -> `SaveNum = 3` (ansi), bytes `'A' 'B' 0x00`.
- On load, a string whose Num()==1 (just the NUL) is emptied (`UnMisc.cpp:881`).
- The 32-bit length is itself emitted as raw 32 bits via the bit writer (it is NOT a
  ranged int), so it is bit-packed like everything else.

### 2.6 UObjectProperty (`:1922`) and UClassProperty
```
return Map->SerializeObject(Ar, PropertyClass, *(UObject**)Data);
```
`UClassProperty` is a subclass of `UObjectProperty` and uses the same path (its
PropertyClass is `UClass`). Both go through `UPackageMapLevel::SerializeObject`. See §3.

### 2.7 UStructProperty (`:4192`)
Dispatches on the struct's FName:
- **Vector** -> `FVector::SerializeCompressed(Ar)` — see §4.1.
- **Rotator** -> `FRotator::SerializeCompressed(Ar)` — see §4.2.
- **Quat** -> writes `Ar << X << Y << Z` (3 x 32-bit float); W is reconstructed on load
  from unit-length assumption (`:4204`). 96 bits.
- **Plane** -> 4 x `SWORD` (X,Y,Z,W rounded to 16-bit signed) = 64 bits (`:4253`).
- **UniqueNetId** -> single `QWORD` = 64 bits (`:4261`).
- **Any other struct** -> iterate `TFieldIterator<UProperty>` over the struct's members
  in declaration order; for each member that `Map->SupportsObject`, loop its
  `ArrayDim` and recursively `NetSerializeItem` each element (`:4267-4279`). NO handle,
  NO length — members are written back-to-back in field order. Returns
  `bMapped || !(CPF_RepRetry)`.

### 2.8 UArrayProperty (dynamic TArray) (`:3365`) — **NO-OP**
```
UBOOL UArrayProperty::NetSerializeItem(...) const { return 1; }
```
- **In this engine build, NetSerializeItem for a dynamic array writes NOTHING.**
- The actor-channel replication path (`UnChan.cpp:1960`) calls `NetSerializeItem`
  directly for the property and does NOT special-case `UArrayProperty`. There is no
  separate per-element handle expansion for dynamic arrays in `ReplicateActor`
  (the `ArrayDim != 1` element byte in §1 is for fixed C arrays, NOT TArray).
- **Consequence / gotcha:** replicated dynamic-array (`array<...>`) member VALUES are
  effectively NOT delta-replicated through the standard property path here. A handle
  for an array property would consume the handle bits and then emit zero value bits.
  Do not attempt to "replicate" a dynamic array as a normal property — it transmits no
  contents and will desync your bit cursor expectations only by the handle width.
  (Arrays inside actors are normally populated client-side via spawn/RPC, or are
  fixed-size C arrays that DO use the §1 element byte.)
- Static fixed arrays (`Type Name[N]`, `ArrayDim==N>1`) ARE supported: each touched
  element is a separate property record `handle + BYTE element + value` (§1).

---

## 3. Object / Class reference codec — UPackageMapLevel::SerializeObject

Source: `Engine/Src/UnNetDrv.cpp:97`. This is the codec for every UObjectProperty /
UClassProperty value, and also for the actor open-header class ref (`Bunch << Class`).

Constants:
- `MAX_OBJECT_INDEX = (DWORD(1) << 31) = 0x80000000` (`Core/Inc/UnCoreNet.h:100`).
- `MAX_CHANNELS = MAX_NET_CHANNELS = 2048` per generic UE3 source
  (`Engine/Inc/UnConn.h:143,153`). **See WARNING below — prior RS2 RE used 1024.**

### Wire layout (LOAD / decode side, `:100-167`)
```
[selector] = 1 bit (SerializeBits 1)
if (selector == 1) {                       // "dynamic actor or None"
    Index = SerializeInt(Index, MAX_CHANNELS);   // ranged int, ~11 bits (max 2048)
    if (Index <= 0)  Object = None/NULL;
    else             Object = channel[Index]'s actor (if that channel is an open ActorChannel)
} else {                                   // selector == 0 -> "static object"
    Index = SerializeInt(Index, MAX_OBJECT_INDEX);  // ranged int over 0..2^31, ~31 bits
    Object = IndexToObject(Index);          // package-map net index -> static object
}
```

### Wire layout (SAVE / encode side, `:169-229`) — exact branch the server emits
- **Dynamic actor** (non-static, has an ActorChannel): `selector=1`, then
  `SerializeInt(Ch->ChIndex, MAX_CHANNELS)`. Return value `Mapped = Ch->OpenAcked`.
- **NULL / None**: `selector=1`, then `SerializeInt(0, MAX_CHANNELS)`
  -> selector bit `1` followed by `ceil(log2(MAX_CHANNELS))` zero bits
  (11 zero bits if MAX_CHANNELS=2048; 10 if 1024). (`:203-209`)
- **Static object** (resolvable package-map index): `selector=0`, then
  `SerializeInt(ObjectIndex, MAX_OBJECT_INDEX)`. (`:210-228`)
- Dynamic-but-not-yet-initialized-on-client, or un-indexable object: server sends NULL
  form (`selector=1`, SerializeInt(0,MAX_CHANNELS)) and returns 0 (unmapped). (`:188-202,213-219`)

### Summary table
| Object kind        | selector bit | following ranged int                       |
|--------------------|:------------:|--------------------------------------------|
| None / NULL        | 1            | SerializeInt(0, MAX_CHANNELS)              |
| Dynamic actor      | 1            | SerializeInt(channelIndex, MAX_CHANNELS)   |
| Static object      | 0            | SerializeInt(netIndex, 0x80000000)         |

Notes:
- "Static object" = anything resolvable through the package map's static export table
  (classes like ROGameReplicationInfo's class ref, archetypes, content objects). Class
  references (the actor-open class id, and UClassProperty values) use the static form
  with the well-known class static indices (PC=57520, GRI=70887, PRI=86701,
  TeamInfo=90245).
- `IndexToObject(Index, 1)` and `ObjectToIndex` map net index <-> object via the
  package-map's per-package `ObjectBase`/`ObjectCount` ranges (`FPackageInfo`).

### WARNING — MAX_CHANNELS 2048 vs 1024 (must verify against capture)
The generic UE3 tree here defines `MAX_NET_CHANNELS = 2048` (-> 11-bit channel/None
field). Prior RS2-7258 reverse-engineering notes (MEMORY) assumed **1024** (10-bit).
These differ by exactly one bit in EVERY object-ref and every None value, which will
shift the entire downstream bit cursor. **Confirm empirically** against
`rs2_realserver_capture.pcapng`: decode a known None object-ref and count the zero bits
after the selector (10 => 1024, 11 => 2048). RS2 is a UE3 licensee build and may have
overridden this constant; trust the capture over either source value. Until confirmed,
treat the channel/None field width as the single highest-risk unknown in the codec.

---

## 4. Compressed struct codecs

### 4.1 FVector::SerializeCompressed (`Core/Src/UnMath.cpp:51`)
```
IntX/Y/Z = appRound(X/Y/Z);
Bits = Clamp(appCeilLogTwo(1 + Max3(|IntX|,|IntY|,|IntZ|)), 1, 20) - 1;  // 0..19
SerializeInt(Bits, 20);                     // ranged int, the per-component bit budget
Bias = 1 << (Bits+1);
Max  = 1 << (Bits+2);
SerializeInt(IntX + Bias, Max);             // ranged int
SerializeInt(IntY + Bias, Max);             // ranged int
SerializeInt(IntZ + Bias, Max);             // ranged int
// load: comp = (INT)D - Bias;
```
- Components are integer-rounded (sub-unit precision is LOST).
- Field order: `Bits`, then `DX, DY, DZ`.
- `Bits` is in range 0..19 (the `-1` after Clamp(...,1,20)).

### 4.2 FRotator::SerializeCompressed (`Core/Src/UnMath.cpp:84`)
Each of Pitch, Yaw, Roll (UE 16-bit angles) is sent as the **high byte only**
(`angle >> 8`), each gated by a presence bit:
```
for each of {Pitch, Yaw, Roll}:
    BYTE hi = angle >> 8;
    BYTE present = (hi != 0);
    SerializeBits(&present, 1);             // 1 bit
    if (present) Ar << hi;                  // 8 bits
// load: angle = hi << 8;  (low byte always 0)
```
- Order: Pitch, then Yaw, then Roll.
- A zero component costs only its 1 presence bit (no byte). Low 8 bits of each angle are
  discarded (resolution = 256 units = ~1.4 degrees).

---

## 5. Decoder checklist (for tools/mock_client.py-style decoding)

To decode a property record stream for a class with maxHandle `M`:
1. `handle = ReadInt(M)` (ranged, §0.2). If handle is out of the class's field range
   (`GetFromIndex` would return NULL) -> end of property section.
2. Resolve handle -> UProperty via the class's net-field table (see netfields_u_*.txt).
3. If that property is a fixed C array (ArrayDim>1): read `BYTE element` (8 bits).
4. Read value per §2 by property type:
   - byte: 8 bits (or `ceil(log2(NumEnums-1))` if enum)
   - int/float: 32 bits LE
   - bool: 1 bit
   - string: int32 len (incl NUL) + |len| ansi/uni chars
   - object/class: §3 (selector + ranged int) — **verify MAX_CHANNELS width!**
   - vector: §4.1 ; rotator: §4.2 ; other struct: members in field order, no framing
   - dynamic array: NOTHING (no-op) — handle only
5. Loop to 1.

## 6. Source citations index
- Bit codec: `Core/Src/UnBits.cpp` 148 (SerializeBits W), 180 (SerializeInt W),
  206 (WriteIntWrapped), 286 (SerializeBits R), 308 (SerializeInt R), 324 (ReadInt).
- Property NetSerializeItem: `Core/Src/UnProp.cpp` 656 (Byte), 818 (Int), 1432 (Bool),
  1532 (Float), 1922 (Object), 3365 (Array=noop), 4192 (Struct).
- FString: `Core/Src/UnMisc.cpp:835`.
- Object ref: `Engine/Src/UnNetDrv.cpp:97` (UPackageMapLevel::SerializeObject).
- Per-property framing: `Engine/Src/UnChan.cpp` 1477/1576 (read handle),
  1500-1557 (read value + element), 1926-1960 (write).
- Vector/Rotator compress: `Core/Src/UnMath.cpp` 51 / 84.
- Constants: `Core/Inc/UnCoreNet.h:100` (MAX_OBJECT_INDEX=0x80000000),
  `Engine/Inc/UnConn.h:143,153` (MAX_NET_CHANNELS=2048), `Core/Inc/UnCoreNet.h:31`
  (GetMaxIndex = FieldsBase + Fields.Num()).
