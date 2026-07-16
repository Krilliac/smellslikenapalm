# Retail vehicle replication evidence (RS2/7258)

This note separates exact evidence from the still-opaque parts of helicopter
replication.  The reproducible extractor is
`tools/extract_vehicle_capture_evidence.py`; its focused fixtures are in
`tools/tests/test_vehicle_capture_evidence.py`.

## Provenance

- real-server pcap: `D:\RE-Tools\rs2_realserver_capture.pcapng`
  - bytes: `20,878,200`
  - SHA-256: `08B9128409B8269100AA21B3DD2D596DE1F6BE50AABE53A3C7B53BE9E48A411F`
- capture-compatible server `ROGameContent.u`
  - bytes: `23,934,851`
  - SHA-256: `2D6433144F00EB130D15193C414A4F401FCA40806AEFC9A6342B7670E376F992`
  - package GUID: `FE4B4F2F4B3128C42FE5098ACAB560C8`
  - engine/package/licensee: `7258 / 765 / 771`
  - the helicopter UClasses are exports `50`, `52`, and `94`; their actor
    archetype CDOs are exports/serialized `UObject.NetIndex` values `51`, `53`,
    and `95`.
- installed `VNTE-Resort.roe`
  - bytes: `1,162,107,869`
  - SHA-256: `58193C282D27B3D8E2A47B16C919618BBAE6EE2F43B64CCC738DA6FFEA7055AF`
  - package GUID: `C75E786345B77AA5243259ABAF16C294`

## Exact archetype refs in actor opens

The capture's transient vehicle opens begin with a static PackageMap reference
to an actor archetype CDO, not to its UClass. `UActorChannel::ReceivedBunch`
loads that object and requires it to cast to `AActor`; a UClass reference cannot
satisfy that contract. The three CDO refs independently produce the same
`ROGameContent` ObjectBase:

| actor archetype CDO | CDO NetIndex | ObjectBase | captured static ref | class maxHandle |
|---|---:|---:|---:|---:|
| `ROGameContent.Default__ROHeli_AH1G_Content` | 51 | 285943 | **285994** | 130 |
| `ROGameContent.Default__ROHeli_OH6_Content` | 53 | 285943 | **285996** | 129 |
| `ROGameContent.Default__ROHeli_UH1H_Content` | 95 | 285943 | **286038** | 150 |

The compiled factory CDOs resolve through the same ObjectBase, but are not
claimed as capture-observed actor opens:

| factory archetype CDO | CDO NetIndex | compiled static ref | class maxHandle |
|---|---:|---:|---:|
| `ROGameContent.Default__ROVehicleFactory_AH1G` | 301 | **286244** | 25 |
| `ROGameContent.Default__ROVehicleFactory_OH6` | 309 | **286252** | 25 |
| `ROGameContent.Default__ROVehicleFactory_UH1H` | 316 | **286259** | 25 |

This corrects both the earlier generic-`ROPawn` classification and the later
off-by-one derivation that paired UClass exports `50/52/94` with ObjectBase
`285944`. The summed wire refs happened to remain correct, hiding the mistake;
the exact derivation is CDO NetIndex plus ObjectBase `285943`. Representative
bit-exact opens used by the tests are:

| class | frame | channel / sequence | payload bits | decoded location |
|---|---:|---:|---:|---:|
| AH-1G | 37165 | 97 / 43 | 919 | `(-92829, 9010, -412)` |
| OH-6 | 19887 | 201 / 1 | 973 | `(-91681, 9016, -428)` |
| UH-1H | 19718 | 36 / 24 | 1206 | `(-91709, 7934, -416)` |

The full pcap scan recognizes 115 opens (AH-1G 24, OH-6 24, UH-1H 67) with
zero malformed recognized prefixes.  Fifteen other datagrams do not consume
cleanly in the current generic packet decoder, so the extractor returns partial
exit code `67`; it does not claim that 115 is a complete capture-wide count.

## Resort factory placement

The cooked map contains exactly six matching world actors with no extraction
errors or truncated properties:

| cooked export | factory class | location | serialized rotation (P/Y/R) |
|---:|---|---|---|
| 12856 | `ROVehicleFactory_AH1G` | `-92829.33 9010.422 -408` | `0 -65552 0` |
| 12857 | `ROVehicleFactory_OH6` | `-91680.8 9015.586 -433` | `0 -131220 0` |
| 12858 | `ROVehicleFactory_UH1H` | `-91684.66 6822.016 -408` | `0 -131040 0` |
| 12859 | `ROVehicleFactory_UH1H_1` | `-92808.88 7904.438 -408` | `0 -131132 0` |
| 12860 | `ROVehicleFactory_UH1H_2` | `-92793.24 6804.031 -408` | `0 -131004 0` |
| 12861 | `ROVehicleFactory_UH1H_3` | `-91709.16 7933.789 -408` | `0 -131140 0` |

The recurring capture locations agree with these factories after the class
spawn offset and integer wire quantization.  The OH-6 actor's serialized `Tag`
is `VNVehicleFactory_UH1H`; the class, not that mismatched map tag, is identity.

These `12856..12861` values are local cooked export-table indices.  They are
**not** proven PackageMap static object refs and must never be emitted as such.

## Exact net-field handles now extracted

Generated typed tables:

- `tools/netfields_u_ROVehicleFactory.txt` (`maxHandle=25`)
- `tools/netfields_u_ROVehicleHelicopter.txt` (`maxHandle=122`)
- `tools/netfields_u_ROHeli_AH1G_Content.txt` (`maxHandle=130`)
- `tools/netfields_u_ROHeli_OH6_Content.txt` (`maxHandle=129`)
- `tools/netfields_u_ROHeli_UH1H_Content.txt` (`maxHandle=150`)

High-value handles shared by the helicopter classes include:

| handle | field / RPC |
|---:|---|
| 58 | `Driver` |
| 60 | `RBState` |
| 63 / 64 | `ServerAdjacentSeat` / `ServerChangeSeat` |
| 70 | `PassengerPRI` |
| 73 / 74 | `ReplicatedSeatProxyHealths2` / `ReplicatedSeatProxyHealths` |
| 75 | `SeatMask` |
| 78 | `BackSeatDriverIndex` |
| 81 | `EngineStatus` |
| 83 | `Team` |
| 103 | `bEngineOn` |
| 107 | `HelicopterArrayIndex` |
| 109 / 110 | `VehHitZoneHealths[55]` / `VehHitZoneHealthsChanged` |
| 111 / 112 | `CurrentRPM` / `DesiredRPM` |

The related already-grounded cross-actor fields are `ROTeamInfo` h48/h49/h50/h72
(pilot names, locations, vehicle refs, change counters) and
`ROPlayerReplicationInfo` h63/h64 (helicopter seat/array indices).

## Exact cross-actor correlations now extracted

Schema `rs2.vehicle-transaction-evidence.v3` records the exact actor-archetype
CDO path/NetIndex derivation and tracks channel generations per
client endpoint.  It assigns semantics only after a source channel is grounded
by its actor open, and only when the selected typed decoder consumes the exact
`BunchDataBits`.  A target dynamic object ref must resolve to the *currently
open generation* of a known helicopter channel.  Reused channel numbers cannot
retroactively change earlier evidence.

The pinned full-capture run produced:

| evidence | exact observations / correlations |
|---|---:|
| whole target-property bunches | 195 |
| `ROTeamInfo.TeamHelicopterArray` h50 elements | 80 |
| h50 state transitions resolving to a live known helicopter | **10** |
| h50-resolved vehicle classes | 7 UH-1H, 2 OH-6, 1 AH-1G |
| `TeamHelicopterPilotNames` h48 elements | 9 |
| h48 changes joined to a current h50 vehicle slot | 3 |
| `TeamHelicopterRep` h72 elements | 9 |
| fully typed `TeamHelicopterLocationArray` h49 elements | **0** |
| PRI `Team` h35 refs | 103 |
| PRI helicopter array index h64 | 14 |
| PRI helicopter seat index h63 | 3 |

One representative direct join is frame 7562: TeamInfo ch56 h50 element 4
points to dynamic ch112 in an exact 50-bit bunch, while ch112's current actor
generation is a capture-grounded UH-1H open from frame 7550.  The evidence
record retains the exact property bit offset/length, payload bit count, and
SHA-256 instead of copying an inferred property tail.

The capture does **not** yet provide a transactionally complete PRI seat join.
The h35/h63/h64 values above are bit-exact, but the fully typed TeamInfo slot
mapping and PRI seat updates do not line up in an order that permits a
generation-safe join.  In particular, the extractor will not carry an old PRI
seat assignment forward across a later helicopter replacement merely because
the same TeamInfo array index is reused.

The scan also finds no actor open using one of the three grounded factory CDO
refs and therefore emits zero typed `ChildVehicle` h24 observations.
That is reported as `factoryEvidenceState=noGroundedFactoryChannelObserved`,
not as proof that the server never replicated a child.  Resort's placed factory
export-table indices remain insufficient to identify their PackageMap refs.

There are 15 undecodable datagrams in the capture, so the summary remains
`complete=false` and the command returns partial exit code `67`.  A failed
packet could hide a channel close/reopen.  At each failure the extractor now
drops every live typed actor identity for that client endpoint.  Each
`channelLifecycleInvalidation` record retains the frame, endpoint, packet id,
datagram size and SHA-256, decoder bit positions, partial-bunch count, and the
exact actor generations invalidated.  Only a later decoded actor open can
ground a new generation.

This fail-closed boundary invalidates 180 live actor identities across those 15
frames.  It discards 37,114 subsequent bunches spanning 178 pre-gap channel
lifetimes; compact `postGapDiscardRange` records preserve their first/last
frames and packet ids while assigning no property or actor semantics.  The 195
typed blocks and 13 vehicle correlations above are the independently decodable
transactions, not a continuation through those gaps.  Within that accepted
set the typed path has zero property errors; another 9,374 bunches on currently
grounded TeamInfo/PRI channels contain unsupported fields and remain opaque.

## Still unknown; do not synthesize

- The extractor stops after the archetype CDO ref plus compressed Location.  Whether and how
  the remaining bits split into Rotation and initial fields is not yet decoded.
- Exact initial values/order for `RBState`, seats, driver/PRI, engine/RPM, hit
  zones, and per-helicopter subclass fields remain opaque.
- Factory `ChildVehicle` h24 and `bHasLockedVehicle` h23 handles plus the three
  factory CDO refs are exact, but no grounded factory channel is observed in
  this capture.  Bit-pattern matches on untyped actor channels are rejected.
- TeamInfo h50 is now correlated to dynamic vehicle channel generations.  A
  fully typed h49 location update and a complete h50 + PRI h35/h63/h64 seat
  transaction have not yet been captured in compatible, exact-consumption
  bunches.

Until those are captured and typed, runtime code should use the exact actor-archetype refs
and handle tables only as decoding/building prerequisites, not fabricate a
vehicle property tail.

## Reproduce

```powershell
python tools\extract_vehicle_capture_evidence.py `
  --pcap D:\RE-Tools\rs2_realserver_capture.pcapng `
  --output $env:TEMP\resort-vehicle-transactions.jsonl

python -m unittest tools.tests.test_vehicle_capture_evidence -v
```
