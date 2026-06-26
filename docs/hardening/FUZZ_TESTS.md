# RS2V Inbound-Parse Fuzz & Malformed-Input Tests

Consolidated index of the fuzz / hostile-input test suites that prove the
defensively-hardened inbound parse path (BitReader/BitWriter, PacketCodec,
ControlReassembler, HandshakeState/ControlChannel) survives arbitrary, malformed
and adversarially-mutated wire input **without** crashing, reading/writing out of
bounds, hanging (infinite loop), or allocating without bound — and that valid
input still round-trips.

All suites are written in the existing **GoogleTest** framework using the
repo's one-executable-per-file convention (each file has its own `main()` that
runs `InitGoogleTest` + `RUN_ALL_TESTS`, links `rs2v_core`, and is registered via
`rs2v_add_test(...)` in `tests/CMakeLists.txt`). They build with the same test
target machinery as the pre-existing ~11 suites — no new framework, no production
source changes.

Each suite embeds a **watchdog** (background thread / `std::async` with a
wall-clock budget) that turns a genuine hang into a hard, visible failure
(`std::abort` or a gtest timeout assertion) rather than wedging CI forever, and
uses a **fixed deterministic seed** so any failure reproduces exactly.

## Test files

| File | Suite / target | # tests | Under test | Approx. iterations |
|------|----------------|--------:|-----------|--------------------|
| `tests/BitReaderFuzzTests.cpp` | `BitReaderFuzzTests` | 20 | `Network/BitReader.h`, `Network/BitWriter.h` | ~9k decode loops + per-method sweeps |
| `tests/PacketCodecFuzzTests.cpp` | `PacketCodecFuzzTests` | 6 | `Network/PacketCodec.{h,cpp}` (packet+bunch framing) | ~170k Decode/Encode calls |
| `tests/ControlReassemblerFuzzTests.cpp` | `ControlReassemblerFuzzTests` | 7 | `Network/ControlReassembler` | ~400k `OnBunch` calls |
| `tests/HandshakeStateFuzzTests.cpp` | `HandshakeStateFuzzTests` | 8 | `Network/HandshakeState`, `Network/ControlChannel` | ~700k `HandleControlMessage` calls |

All four are registered in `tests/CMakeLists.txt` (allow-list, lines 56–61).

## Build + run — exact command sequence

CMake (VS 2022 generator) is pre-configured at `build-tests/`. The parallel build
has a pre-existing PDB-lock bug, so build single-process (`-- /m:1`). From the
repo root `D:\smellslikenapalm`:

```
cmake --build build-tests --target BitReaderFuzzTests          --config Debug -- /m:1
cmake --build build-tests --target PacketCodecFuzzTests        --config Debug -- /m:1
cmake --build build-tests --target ControlReassemblerFuzzTests --config Debug -- /m:1
cmake --build build-tests --target HandshakeStateFuzzTests     --config Debug -- /m:1

build-tests\tests\Debug\BitReaderFuzzTests.exe
build-tests\tests\Debug\PacketCodecFuzzTests.exe
build-tests\tests\Debug\ControlReassemblerFuzzTests.exe
build-tests\tests\Debug\HandshakeStateFuzzTests.exe
```

Or via ctest (runs all four by name regex):

```
ctest --test-dir build-tests -C Debug -R "FuzzTests" -V
```

If `build-tests/` does not exist yet, configure first:

```
cmake -S . -B build-tests
```

### Last verified run (2026-06-26, Debug)

```
BitReaderFuzzTests           20 tests PASSED   (262 ms)
PacketCodecFuzzTests          6 tests PASSED   (11163 ms)
ControlReassemblerFuzzTests   7 tests PASSED   (3216 ms)
HandshakeStateFuzzTests       8 tests PASSED   (9569 ms)
```

Total: **41 fuzz tests, all PASSED, exit 0** — no crash, no over-read, no hang.

## What each suite covers

### BitReaderFuzzTests (20 tests, 5 suites)
The foundational bounds-checking contract: every read is checked against the
backing buffer's bit length; an over-read must set the sticky `IsOverflowed()`
flag, return a zero/default value, and never read OOB / throw / hang.
- **Malformed/over-read:** empty buffer (every read overflows, flag sticky);
  1-byte `ReadInt32`, partial-then-overflow split; `ReadString` with a
  huge-positive (INT32_MAX), huge-negative (INT32_MIN, UCS-2 path), and
  truncated length prefix — all bounded, no multi-GB alloc.
- **Adversarial `SerializeInt` maxValue:** 0 and 1 (degenerate, consume nothing);
  `UINT32_MAX` on empty and short buffers (terminates, overflows, no hang);
  round-trip across a spread of maxValues/values.
- **Round-trip:** random typed sequences (byte/int32/uint32/uint64/float/bits)
  and ANSI + UCS-2 strings survive `BitWriter -> BitReader` exactly.
- **Mutation fuzz:** a canonical record, then bit-flips (4000×), every truncation
  prefix, extensions, and **FString length-prefix overwrite** with every
  evil/huge/negative/zero pattern — never crash, never hang.
- **Random + boundary sweep:** arbitrary buffers (len 0..4096, sub-byte
  validBits) hammered through every read method (1500×); boundary buffers
  (0-len, single 0x00/0xFF, 4096× all-0xFF/all-zero, alternating). Buffers are
  heap-allocated to exact length so an OOB read is a genuine fault (run under
  ASan to harden further). Core invariant asserted: `BitPos() <= NumBits()`
  after every op.

### PacketCodecFuzzTests (6 tests)
UE3 packet+bunch framing codec. Decoded against all three MaxPacket phase bounds
(handshake 8, S2C 1500, C2S/NMT 2048). A 5-second hard watchdog dumps the
offending bytes and aborts on any hung call.
- **RandomBytesNeverCrashOrOOB** — 60k random buffers (len 0..300).
- **MutatedValidPacketsNeverCrashOrOOB** — 40k: encode a valid packet then
  bit-flip / overwrite / truncate / extend / zero-terminator, decode under a
  (possibly different) phase bound.
- **BoundaryBuffers** — 0-length, every single byte 0x00..0xFF, all-zero,
  all-0xFF, alternating, terminator-only, lengths up to 4096.
- **ValidPacketsRoundTrip** — 20k: `Encode -> Decode -> Encode` is field- and
  byte-exact.
- **PerBunchPayloadFuzz** — 30k: drives the bunch-payload path directly,
  including **inconsistent bunches** (payloadBits ≫ actual buffer) to exercise
  the clamp/guard paths.
- **DecodeReencodeDecodeStable** — 20k: decode→re-encode→decode is stable
  (catches parser non-determinism / state bleed).

Safety invariants asserted on every decode: entry count ≤ bit budget (loop
consumes ≥1 bit/entry → bounded loop & allocation); all bounded fields within
their `SerializeInt` range; **`payload.size()*8 >= payloadBits`** (the critical
OOB invariant — a consumer copying `payloadBits` can't run past the buffer);
`payloadBits` ≤ MaxPacket bound.

> **Documented intentional semantic** (not a bug): `PacketCodec::Decode` is
> UE3-lenient — it returns `ok == true` for any datagram whose 14-bit PacketId is
> readable, and `ok == false` only for empty input, a zero last byte (no
> terminator), or a PacketId that overflows. "Garbage" does not generally yield
> `ok == false`; the engine tolerates trailing/benign bits via `AtEnd()`. The
> tests therefore assert the safety invariants above on every decode and assert
> `ok == false` only for the inputs the contract actually rejects. A stricter
> reject policy would be a production decision, not a decoder-safety bug.

### ControlReassemblerFuzzTests (7 tests)
Inbound control-channel reassembler. Asserts the documented DoS caps
(128 pending bunches / 256 KiB pending bytes / 64-sequence-ahead window) hold
under hostile flooding; 30-second watchdog per loop.
- **RandomBunchesStayBounded** — 200k fully-random bunches (tiny/near/far/zero/
  uint32-top sequences; honest *and* lying `payloadBits` incl. 0xFFFFFFFF);
  pending count stays ≤ cap.
- **MutatedValidStreamSurvives** — 100k mutated seq-2 bunches (bitflip / truncate
  / extend / bad length field / bad seq / wrong flag / wrong channel).
- **RejectsPayloadBitsOverrun** — a bunch claiming 4096 bits with 32 real bits is
  dropped (not delivered, not buffered, stream doesn't advance); honest version
  still accepted afterward.
- **HugeSequenceFloodNeverBuffers** — far-future + near-uint32-max sequences
  (wrap bait) never buffer.
- **ScatteredOversizedStormStaysCapped** — 50k 8 KiB bunches scattered in-window
  with a permanent gap; pending count and `count*8KiB` byte ceiling stay under
  the 256 KiB cap.
- **BoundaryBunches** — 0-length/zero-payloadBits (advances stream, no empty
  delivery), 1-byte, 16384-byte all-0xFF, alternating.
- **ValidStreamStillRoundTripsAfterFuzz** — out-of-order + duplicate valid stream
  still delivers in order (proves no shared-state corruption).

### HandshakeStateFuzzTests (8 tests)
Per-connection control-channel handshake state machine. Proves
`HandleControlMessage` "never throws; malformed input is logged + ignored", the
phase enum stays valid, and the machine never advances past `Joined` on garbage;
30-second watchdog per loop.
- **RandomNmtPayloads** — 200k random payloads to a machine in the NMT phase.
- **RandomPreHandshakePayloads** — 200k random payloads in the pre-NMT
  StatelessConnect phase.
- **EveryTypeByteTruncatedAndOversized** — every leading byte 0x00..0xFF with a
  truncated body, a short bogus body, and a 2048-byte all-0xFF body.
- **OutOfRangeTypeBytesIgnored** — type bytes > `kNMTMaxCase` never dispatch /
  never advance the machine.
- **FStringLengthOverflowRejected** — body-bearing NMTs (Hello/Login/Netspeed/
  SteamLogin/SteamAuth) with a 4-byte length claiming a huge string but no
  backing bytes are rejected by the bounded BitReader; never reach `Joined`.
- **MalformedStatelessHandshakeSubtypes** — every pre-NMT subtype 0x00..0xFF;
  only `kStart`/`kResponse` are meaningful, all else leaves handshake incomplete.
- **MutatedValidStreamSurvives** — 100k runs corrupting one message of a valid
  Hello→Netspeed→Login→Join stream (bitflip / truncate / extend / type-swap).
- **ValidHandshakeStillReachesJoined** — regression: a clean handshake still
  drives AwaitingHello→…→Joined and fires the loggedIn/joined events.

## Real bugs found needing a production fix

**None found.** All 41 fuzz tests pass with no crash, no out-of-bounds access, no
hang, and no unbounded allocation across ~1.3M decode/parse calls. The existing
non-fatal hardening (BitReader sticky-overflow guard, PacketCodec bounded
SerializeInt + payload-bit invariants, ControlReassembler DoS caps,
HandshakeState log-and-ignore dispatch) held against every random / mutated /
boundary input exercised. Valid input still round-trips byte- and field-exactly.

> Note: the PacketCodec UE3-lenient `ok==true` behavior described above is
> intentional, not a defect. If a stricter inbound reject policy is later desired,
> that is a production-code design decision, not a decoder-safety bug surfaced by
> these tests.
