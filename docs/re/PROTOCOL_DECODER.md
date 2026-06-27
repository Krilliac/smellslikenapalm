# Runtime Protocol Decoder (self reverse-engineering system)

`src/Protocol/ReverseEngineering/` — the live half of the RE effort. It captures
the server's own client traffic and decodes the UE3/RS2V wire protocol in real
time, feeding the offline knowledge base in `docs/re/`.

## Components

| File | Role |
|------|------|
| `ProtocolDecoder.{h,cpp}` | Capture hooks, per-client sessions, byte-level field heuristics, UE3 bunch analysis, reports/exports. Global singleton `GetProtocolDecoder()`. |
| `NetFieldTable.{h,cpp}` | Loads the per-class handle tables (`data/re/netfields/netfields_u_<Class>.txt`): handle → property name + wire value type, plus `GetMaxIndex()`. |
| `BunchPropertyDecoder.{h,cpp}` | Decodes the **bit-packed** property record stream inside an actor-channel bunch using `BitReader`, per `docs/re/ue3_property_value_codec.md`. |

Wired into the network layer via `ConnectionManager` (`OnRawUDPReceived`,
`OnPacketReceived`, connect/disconnect) and `ClientConnection` (`OnPacketSent`);
configured in `GameServer` from the `[ReverseEngineering]` config section.

## How it decodes

Two independent paths run per connection:

1. **Byte-level structure heuristics** (`AnalyzePacketPayload`). For each packet
   *tag* it buffers the earliest samples, infers a field layout from the **modal
   payload size** (not the first, possibly-atypical packet), then accumulates
   per-field statistics (min/max/mean/variance, observed enum values) and a
   confidence score. Good for tagged application packets that are byte-aligned.

2. **Bit-packed bunch property decode** (`AnalyzeUE3Bunch` → `DecodeBunchProperties`).
   UE3 actor replication is **not** byte-aligned — property records are
   `ReadInt(maxHandle)` handles followed by type-directed bit-packed values. This
   path parses the real bunch with `UE3Protocol::ParseBunch`, then decodes the
   property stream against the known class tables, recovering actual UE3 property
   **names and values** (e.g. `Location=(x,y,z)`, `bNetOwner=true`).
   For an **open** bunch it first parses the `SerializeNewActor` header
   (`BunchPropertyDecoder::ParseOpenHeader`, per `open_bunch_structure.md`): the
   32-bit static class ref, compressed `Location`, optional `Rotation`, and the
   PlayerController `NetPlayerIndex` byte. The class is identified **exactly** by
   its verified static export index (PC=57520, PRI=86701, TeamInfo=90245,
   GRI=70887), the channel is bound to it, and the property block is decoded from
   the correct post-header bit offset. A best-fit scan remains only as a fallback
   for bunches seen before their open.

Value codecs implemented (per the codec spec): bool (1 bit), byte (8 bits),
int/float (32 bits LE), FString, compressed `FVector`/`FRotator`, `Quat`,
`Plane`, `UniqueNetId`, and object/class refs (`selector + SerializeInt`,
`MAX_CHANNELS=1024` for this build). Undecodable widths (enum byte, arbitrary
structs, RPC params) are recorded and the stream stops cleanly rather than
guessing past an unknown width and desyncing the bit cursor.

## Operational notes

- **Safe by default.** `log_raw_packets` and `export_json` default **off** —
  retaining attacker payloads / writing files is opt-in for an active RE session.
- **Off the hot path.** `async_analysis` (default on) copies bytes on the socket
  thread and runs all analysis on a single worker thread (FIFO; bounded
  drop-oldest queue), so capture never blocks the network loop.
- **Cumulative.** `persist_state` merges per-tag totals across runs via
  `protocol_analysis/protocol_state.tsv`.
- **Reports.** `GenerateProtocolReport()` includes decoded byte structures,
  client sessions, and the decoded bit-packed channel properties.

## Tests

`tests/ProtocolDecoderTests.cpp` — self-contained (own `main`, no GoogleTest, no
network). Covers type classification, handle-table parsing (rich + lean),
registry loading, a bit-packed property round-trip built with `BitWriter`, the
clean-stop-on-enum behaviour, sync layout inference, and async worker drain.

Build/run offline:

```
cmake -S . -B build -DBUILD_RE_SELFTEST=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target ProtocolDecoderSelfTest
ctest --test-dir build -R ProtocolDecoderSelfTest --output-on-failure
```

## Known limitations / next steps

- Channel→class binding is **exact** for the menu actors (PC/PRI/TeamInfo/GRI,
  bit-exact indices) and identifies ROPawn (export-index range 285994–286464) and
  ROWeapon by index for the channel map. Pawn/Weapon tables are lean, so those
  channels are named but their values aren't decoded; adding their typed tables
  would unlock pawn/weapon value decode.
- `ROPlayerReplicationInfo` carries the verified scalar property types from
  `open_bunch_structure.md` §4.3 (Score/PlayerName/UniqueId/Kills/… and the
  byte/int static arrays), so a PRI open block decodes; other PRI handles not in
  that block remain untyped and stop the decode if they appear.
- Static C arrays (`Type Name[N]`) decode element-by-element via the 8-bit element
  index. Dynamic `array<...>` members are a no-op on the wire (handle only).
- Enum-byte widths need each enum's `NumEnums`; not present in the handle tables,
  so enum values currently stop the decode. Capturing enum sizes would extend
  coverage past the first enum property.
