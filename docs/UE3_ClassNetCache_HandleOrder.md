# UE3 7258 — Property/Function Handle Ordering (ClassNetCache) & the Team-Select Menu RPC

Source-exact derivation of how the replication **handle** (the `SerializeInt(handle, maxHandle)`
that prefixes every replicated property AND every client/server function on an actor channel) is
assigned. From the leaked UE3 source at `D:\RE-Tools\UE3-src`. This is what we need to *interpret*
or *generate* the function bunches the server sends on the PlayerController channel (ch2) — in
particular the RPC that opens the team-select menu.

## The handle algorithm (definitive)

`UPackageMap::GetClassNetCache` — `Development/Src/Core/Src/UnCoreNet.cpp:137`:
- `FieldsBase = Super ? Super->GetMaxIndex() : 0` (base classes occupy the low handles).
- For each field in `Class->NetFields` (in array order), assign `FieldNetIndex = current
  GetMaxIndex()` — i.e. sequential starting at `FieldsBase`.
- `GetMaxIndex() = FieldsBase + Fields.Num()`  → this is the **maxHandle** passed to
  `Bunch.ReadInt(ClassCache->GetMaxIndex())` (`UnChan.cpp:1477`) and
  `Bunch.WriteIntWrapped(FieldCache->FieldNetIndex, GetMaxIndex())` (`UnChan.cpp:1952`).

So: **handle = (sum of net-field counts of all super classes) + (this field's position in this
class's NetFields)**. Handles are cumulative, base-class-first.

## What's in NetFields, and its order

`UClass::Link` — `Development/Src/Core/Src/UnClass.cpp:2126-2152`:
- A property is in NetFields iff `PropertyFlags & CPF_Net` (it appears in the class's
  `replication{}` block).
- A function is in NetFields iff `FunctionFlags & FUNC_Net` **and** `!GetSuperFunction()` (it is
  FIRST declared in this class — overrides do NOT get their own handle; they reuse the base
  class's).
- Then: `Sort<USE_COMPARE_POINTER(UField,UnClass)>(...)`. The comparator (`UnClass.cpp:2086`) is
  `{ return A->GetNetIndex() - B->GetNetIndex(); }` → **NetFields is sorted by `GetNetIndex()`**,
  which is the field's declaration index within the class (low = declared earlier).

Net: within one class, replicated properties and `reliable client/server` functions are
interleaved and ordered by their `.uc` declaration order; across classes, super first.

## Wire format of a replicated function call (RPC) on an actor channel

After the actor-open header (or on a later bunch), the data stream is a sequence of:
`SerializeInt(handle, maxHandle)` then, if the handle is a FUNCTION, the function's parameters
serialized in declaration order (each param by its UProperty NetSerializeItem — byte=8b, bool=1b,
int/float=32b, FString, object=SerializeObject/static-or-dynamic ref, etc.). Properties and RPCs
share the same handle space and can be merged into one bunch. The block is length-delimited (runs
until the bunch's bits are exhausted; no terminator). See `UnChan.cpp:1477-1672`.

## THE TEAM-SELECT MENU RPC (the current goal)

`ROGame/ROPlayerController.uc:3533`:
```
reliable client simulated function ChangedTeams(byte TeamIndex, bool bShowRoleSelection,
    optional Class<GameInfo> GameTypeClass, optional bool bTeamBalancing, optional bool bShowLobby)
```
This is the client RPC that opens the team / role-selection UI. The server (GameInfo /
ROPlayerController) calls it on the owning client's PC after Join to show team-select. Related
nearby RPCs: `SelectTeamFailed` (3511), `ClientCloseUnitSelectScene` (3770), `ClientSetHUD`
(1578), `ClientRestart` (1642). The menu is therefore a **PlayerController client function on
ch2**, not pure actor replication — even once the GRI/TeamInfo/PRI actors persist, the client
likely needs this RPC (or the GameInfo state that triggers it) to actually display the menu.

## How to pin ChangedTeams' exact handle / bytes (next step)

Two routes (do the empirical one first — it self-validates):
1. EMPIRICAL: get ROPlayerController's `maxHandle` (decode any ch2 bunch so the length-delimited
   block consumes exactly), then scan the early ch2 S2C bunches (capture f1484+, the post-open
   state-setup RPCs) for one whose decoded params match ChangedTeams' signature
   (byte + bool [+ static Class<GameInfo> ref + bool + bool]). Reuse those bytes verbatim to replay
   the call. The frequent `176a…`-prefixed ch2 bunches are a different high-rate RPC (looks like
   per-frame spectator camera/view updates); ChangedTeams is a rare early one.
2. COMPUTE: enumerate the net fields of the whole PlayerController→ROPlayerController hierarchy in
   declaration order (per the algorithm above) to get FieldsBase + ChangedTeams' position. Large
   mechanical pass over the `.uc` files; validate against route 1.

Reference: `scratchpad/ch2funcs.py` extracts the ch2 stream; `tools/mock_client.py` decode_packet
(bd_max=12000 for S2C) decodes bunches.
