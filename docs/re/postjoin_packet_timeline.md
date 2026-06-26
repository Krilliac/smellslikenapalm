# Post-Join packet/bunch timeline (real server ground truth)

Source capture: `D:\RE-Tools\rs2_realserver_capture.pcapng`
Wire filters: S2C = `udp.srcport==7777 && udp.dstport==57867`, C2S = `udp.srcport==57867 && udp.dstport==7777`.
Decoder: `tools/mock_client.py` `decode_packet(bytes, bd_max=...)` — S2C `bd_max=12000`, C2S `bd_max=16384`.

This documents the exact packet-level shape of the server's response from the client's
**Join** (NMT 0x09) through the first ~80 frames, so our actor-bootstrap send path can match it.

---

## 1. The Join trigger and the very first response packet

| Frame | Dir | t (s) | Bytes | Contents |
|------|-----|-------|-------|----------|
| f1477 | C2S | 103.5452 | 16 | `pid=14` **ch0 sq45 R1 NMT=0x09 (Join)** — payload `09 bbdbe803 0100 1001` |
| f1479 | C2S | 103.5525 | 85 | pid=15 pure-ack (acks pid 28..71) |
| f1480 | S2C | 103.5656 | 2 | pid=74 (empty keepalive, the last of the pre-join 2-byte idle packets) |
| f1483 | C2S | 103.7817 | 79 | pid=16 pure-ack (32..74) |
| **f1484** | **S2C** | **103.7822** | **1237** | **pid=75, first actor-bootstrap packet** |

The server does **not** answer Join with a tiny control packet. ~230 ms after Join it begins
the actor dump. The **first** S2C packet (f1484, pid=75) already carries 11 bunches:

```
f1484 pid=75 ack14,15
  B[ch2  sq1 O1 R1  925b]   <- PlayerController channel OPEN (first byte 0x60)
  B[ch0  sq34 O0 R1  40b NMT=0x24]  <- control message 0x24  (payload 24 01000000)
  B[ch2  sq0 O0 R0 270b]   <- ch2 unreliable property data (first byte 0x33)
  B[ch3  sq1 O1 R1 995b]   <- actor OPEN
  B[ch4  sq1 O1 R1 1236b]  <- actor OPEN
  B[ch5  sq1 O1 R1 1013b]  <- actor OPEN
  B[ch6  sq1 O1 R1 1250b]
  B[ch7  sq1 O1 R1 1187b]
  B[ch8  sq1 O1 R1 213b]
  B[ch9  sq1 O1 R1 1144b]
  B[ch10 sq1 O1 R1 1132b]
```

**Key ordering fact:** NMT 0x24 is **not** sent in its own pre-actor packet. It is the
**2nd bunch of the first actor packet**, emitted right after the PlayerController channel
(ch2) open bunch. So the real pre-actor flow is: `[ch2 open][ch0 0x24][ch2 data][actor opens...]`.
Our prior "send 0x24 before actor bootstrap" should instead interleave 0x24 immediately
after the PC channel open in the same packet.

---

## 2. Control-channel (ch0) bunches across the whole window

Only ch0 carries real NMT control messages. (On actor channels the reported "NMT" byte is
just the first payload byte of the replicated actor data — meaningless as a message id.)

| Frame | Dir | pid | ch0 seq | bits | NMT | Payload (hex) |
|------|-----|-----|---------|------|-----|---------------|
| f1477 | C2S | 14 | sq45 | 72 | 0x09 Join | `09 bbdbe803 0100 1001` |
| f1484 | S2C | 75 | sq34 | 40 | **0x24** | `24 01000000` |
| f1501 | S2C | 91 | sq35 | 232 | **0x23** | `23 18000000 0200 0b00 1360 ...` |
| f1526 | C2S | 18 | sq46 | 232 | 0x23 | `23 18000000 0200 1300 1360 ...` |
| f1549 | S2C | 122 | sq36 | 232 | 0x23 | `23 18000000 0200 2300 1360 ...` |
| f1551 | C2S | 47 | sq47 | 232 | 0x23 | `23 18000000 0200 4700 1360 ...` |

Decoded control-message shapes:

- **NMT 0x24** = `[byte 0x24][int32 = 0x00000001]` → 5 bytes / 40 bits. One-shot, server→client,
  sent once at the start of the actor dump. (Single int payload = 1.)
- **NMT 0x23** = `[byte 0x23][int32 length = 0x18 = 24][24 opaque bytes]` → 29 bytes / 232 bits.
  Length-prefixed blob, exchanged **bidirectionally** and periodically (server ch0 seq
  34→35→36, client ch0 seq 45→46→47). The leading fixed bytes (`0200 NN00 1360 0000...`) are
  constant; the trailing 4–8 bytes differ every time (look like a rolling hash/nonce —
  consistent with an RS2 anti-cheat / signed heartbeat challenge). It is **reliable** (R1) and
  rides on ch0 with its own incrementing reliable sequence, independent of the Join/idle flow.

These are RS2 game-custom control messages (stock UE3 NMT ids only go up to ~0x11); they are
above the stock range and are not in the available partial UE3 C++ source.

---

## 3. The actor-open burst: packing and pacing

Burst window = S2C pid 75..126 (frames f1484..f1559), **52 packets**, **34,491 bytes total**.
It opens **152 actor channels** (ch2 … ch153). Packet size min 29 / max 1279 / avg 663 bytes.
Average 25 bunches per packet (min 2, max 40). The server coalesces as many bunches as fit
under the ~1280-byte MTU per packet, then flushes.

### Pacing — the burst is front-loaded, not one flat batch

First 27 S2C packets (cumOpen = cumulative distinct actor channels opened):

```
frame  pid  size  dt_ms  nbunch nopen cumOpen
f1484  75   1237   0.0    11    9      9     <- PC(ch2), 0x24, ch3..10
f1485  76   1238   0.0    11   11     20
f1486  77   1271   0.0    11   11     31
f1487  78   438    7.0    15   14     45     <- 4 packets within 7ms -> 45 channels open
f1488  79   875   42.3     7    7     52
f1489  80   1131   0.0     6    6     58     <- GRI opens here: ch54 sq1 O1 4662b
f1490  81   614    2.7    21    4     62
f1491  82   1185  49.1    23    7     69
f1493  83   1030  45.2    27    5     74
f1494  84   1222  71.7     5    4     78     <- GRI ch54 partial continues: 7096b bunch
f1495  85   432    0.0    21    1     79
f1496  86   561   41.7    24    2     81
f1497  87   347   40.9    24    1     82
f1498  88   703   55.0    27    3     85
f1499  89   845   59.2    32    5     90
f1500  90   645   37.5    28    3     93
f1501  91   713   60.8    29    2     95     <- ch0 0x23 (#35) rides along here
f1502  92   569   46.2    24    3     98
f1503  93   1099  63.4    31    6    104
f1504  94   673   61.3    36    6    110
f1505  95   986   47.8    34    6    116
f1506  96   1261  31.9    35    7    123
f1507  97   29    7.2      2    0    123     <- overflow tail of f1506 (2 bunches)
f1508  98   972   67.2    28    5    128
f1509  99   362   47.1    21    1    129
f1510  100  744   46.6    32    4    133
...                                          (last actor channel opened at f1549, pid=122)
```

Pacing model:
- **Tick burst of up to ~4 packets back-to-back** (f1484–1487 share t≈103.7822 with dt 0/0/7 ms),
  draining the largest backlog first. Pairs of packets at identical timestamps recur
  (f1488/f1489, etc.) — the server emits multiple datagrams per network tick when the send
  queue is deep.
- After the initial drain, steady state is **~2 packets per ~40–70 ms tick** (the
  `NetServerMaxTickRate` send cadence), each ~300–1280 B.
- Channels open **progressively over ~2.48 s** (f1484 t=103.782 → f1549 t=106.063), NOT in a
  single instantaneous batch. The first 4 packets (≈50 ms) open 45 of 152 channels; the
  heavy actors (PlayerController ch2, GameReplicationInfo ch54, the long PRI chain) are all
  inside the first ~8 packets.

### Interleaving: opens up front, property-resends behind

- A given packet contains a **few O1 (open) bunches first, then many O0 (non-open) bunches**
  that re-send/continue property data for channels already opened in earlier packets
  (the long tails of `B[chN sq0 O0 .. NMT=0x0d]` etc.). Each later packet re-touches the
  whole live channel set with delta/property bunches — this is normal UE3 per-tick
  replication layered on top of the open burst.
- ch0 control bunches (0x24, 0x23) and ch2 (PC) bunches are interleaved **inside** the actor
  packets, never on their own dedicated packet.
- The single biggest actor is **ch54 = GameReplicationInfo**: opened f1489 with a 4662-bit
  bunch, continued f1494 (7096 bits) and f1527 (7190 bits) — it spans multiple packets via
  partial bunches. ch21/56/76 = TeamInfo; ch2 = PlayerController; ch13/14/16/17… = the
  PlayerReplicationInfo chain.

---

## 4. Ack behaviour during the burst

- The **client acks every server packet** almost immediately (C2S pure-ack packets f1479,
  f1483, f1530–f1556 each ack the latest server pid; e.g. f1530 acks 105..116).
- The **server acks the client sparsely**, piggybacked on actor packets: `ack14,15` on f1484,
  `ack16` on f1494, `ack17` on f1525, `ack18` on f1537, `ack18,19` on f1541, etc. The server
  does not send standalone ack packets in this window — acks ride the actor dump.
- Reliable actor opens use `sq1` (first reliable bunch on a freshly opened channel) then `sq0`
  for subsequent unreliable property bunches on that same channel.

---

## 5. Implications for our bootstrap (action items)

1. **Do not emit NMT 0x24 as a standalone pre-actor packet.** Put the PlayerController (ch2)
   open bunch first, then the ch0 0x24 (`24 01000000`) bunch, then the rest of the actor
   opens — all coalesced into the first packet (pid after the Join ack).
2. **Match the packing, not a fixed 12.** The real server uses **52 packets** for the full
   open burst, but front-loads: ~4 packets emitted back-to-back in the first tick opening
   ~45 channels, then ~2 packets/tick. Fill each datagram up to ~1280 B with as many bunches
   as fit, flush, repeat; allow multiple datagrams per tick when backlog is deep. Our current
   "~12 packet" batch is too few and likely too uniform — we should keep coalescing until the
   send queue drains and spread it across ticks.
3. **GameReplicationInfo (ch54) and the big actors split across packets** via partial bunches;
   we must support multi-packet partial bunches for the large GRI/TeamInfo property blobs
   rather than assuming one bunch = one channel's full state.
4. **NMT 0x23** is a periodic bidirectional reliable ch0 heartbeat/challenge
   (`23` + int32 len 24 + 24 opaque bytes, trailing nonce changes each time). The client
   sends it back (ch0 sq45→47); if our server never sends 0x23 the client may still proceed,
   but to be faithful we should emit it on ch0 ~every few seconds with its own reliable seq.
5. The client tolerates the burst being spread over ~2.5 s; the earlier hang was almost
   certainly malformed bunch content, not pacing. Matching the per-packet bunch shape above
   (open header + interleaved control + property tails) is the priority.
