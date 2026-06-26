#!/usr/bin/env python3
"""Extract the official server's bootstrap ACTOR-channel opens (capture frame f231
burst: ch2-ch8 ChType2 bOpen bunches = ROGameReplicationInfo/TeamInfo/
ROPlayerController/PRIs, + the ch0 control bunches in the same packet) and write
them to data/actor_bootstrap.bin as full bunch descriptors for the emulator's
actor-channel send path.

Decode at the SERVER S2C bound (MaxPacket 1500 => 12000), matching how the server
framed these S2C bunches.

Record format (little-endian): per bunch
  u16 chIndex | u8 chType | u8 flags(bit0 bOpen,1 bClose,2 bReliable,3 bControl)
  | u16 chSequence | u32 bunchDataBits | payload bytes (ceil(bunchDataBits/8))
The EXACT bit count is stored (not byte length) so the emulator re-emits the bunch
with the same BunchDataBits - byte-padding would feed the client junk trailing bits
and corrupt the actor-property parse.
"""
import sys, struct, os
sys.path.insert(0, r"D:\smellslikenapalm\tools")
import mock_client as mc

SERVER_BD = 1500 * 8  # 12000
ROOT = r"D:\smellslikenapalm"

# Decode the whole packet at the server bound, returning full bunch dicts.
def decode_full(data, bd_max):
    term = mc.terminator_bit(data)
    if term < 0:
        return None
    r = mc.BitReader(data, term)
    pid = r.rint(mc.MAX_PACKETID)
    bunches = []
    guard = 0
    while r.p < term and not r.error and guard < 64:
        guard += 1
        if r.bit():  # ack
            r.rint(mc.MAX_PACKETID); continue
        bC = r.bit(); bO = r.bit() if bC else 0; bCl = r.bit() if bC else 0
        bR = r.bit(); ci = r.rint(1023); sq = r.rint(1024) if bR else 0
        ct = r.rint(8) if (bR or bO) else 0
        bd = r.rint(bd_max)
        if r.p + bd > term:
            bunches.append({"_overflow": True, "ci": ci, "ct": ct, "bd": bd}); break
        payb = bytearray((bd + 7)//8)
        for i in range(bd):
            if r.bit(): payb[i>>3] |= 1 << (i & 7)
        bunches.append({"bC":bC,"bO":bO,"bCl":bCl,"bR":bR,"ci":ci,"sq":sq,"ct":ct,
                        "bd":bd,"payload":bytes(payb)})
    return {"pid": pid, "bunches": bunches}

s2c = mc.load_frames("s2c")
target = None
for (frame, t, data) in s2c:
    if frame == 231:
        target = (frame, data); break
if not target:
    print("f231 not found"); sys.exit(1)

frame, data = target
dec = decode_full(data, SERVER_BD)
print(f"f{frame} pid={dec['pid']} rawlen={len(data)}: {len(dec['bunches'])} bunches")
records = bytearray()
n = 0
for b in dec["bunches"]:
    if b.get("_overflow"):
        print(f"  OVERFLOW ci{b['ci']} ct{b['ct']} bd={b['bd']} - stopping"); break
    nmt = b["payload"][0] if b["payload"] else None
    print(f"  ch{b['ci']}/ct{b['ct']} O{b['bO']}Cl{b['bCl']}R{b['bR']} sq{b['sq']} bd={b['bd']} "
          f"({len(b['payload'])}B) nmt={'0x%02x'%nmt if nmt is not None else '--'}")
    flags = (b["bO"] & 1) | ((b["bCl"] & 1) << 1) | ((b["bR"] & 1) << 2) | ((b["bC"] & 1) << 3)
    # store the EXACT BunchDataBits (b["bd"]); payload is ceil(bd/8) bytes
    records += struct.pack("<HBBHI", b["ci"], b["ct"], flags, b["sq"], b["bd"]) + b["payload"]
    n += 1

out = os.path.join(ROOT, "data", "actor_bootstrap.bin")
with open(out, "wb") as f:
    f.write(records)
print(f"wrote {out}: {n} bunch descriptors, {len(records)} bytes")
