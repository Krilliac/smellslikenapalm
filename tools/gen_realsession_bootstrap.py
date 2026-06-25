#!/usr/bin/env python3
"""EXPERIMENT 1: build a NetGUID-consistent bootstrap from ONE real game session
(capture session A) - its post-login control stream (0x11,0x03,PackageMap) AND its
actor-open burst (ch2 PlayerController first, GRI/Team/PRIs/pawns) + the ch2 RPCs
(ClientRestart). Because everything comes from the SAME session, the actor class
NetGUIDs resolve against the PackageMap and the local PC is a real AutonomousProxy.

Writes:
  data/replication_bootstrap.bin : [u32 len][payload] records (ch0 control msgs)
  data/actor_bootstrap.bin       : [u16 ch][u8 ct][u8 flags][u16 seq][u32 bits][payload]
"""
import sys, struct, subprocess
sys.path.insert(0, r"D:\smellslikenapalm\tools")
import mock_client as mc

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
PCAP = r"D:\RE-Tools\rs2_realserver_capture.pcapng"
FLT = "udp.port==57867 && udp.port==7777"
ROOT = r"D:\smellslikenapalm"

JOIN_FRAME = 1477          # C->S NMT 0x09
BURST_START, BURST_END = 1484, 1540   # S->C actor burst + ClientRestart window

out = subprocess.run([TSHARK, "-r", PCAP, "-Y", FLT, "-T", "fields",
    "-e", "frame.number", "-e", "udp.srcport", "-e", "data.data"],
    capture_output=True, text=True)
frames = []
for line in out.stdout.splitlines():
    c = line.split("\t")
    if len(c) >= 3 and c[0].strip() and c[2].strip():
        try: frames.append((int(c[0]), int(c[1]), bytes.fromhex(c[2].strip())))
        except ValueError: pass

# ---- replication_bootstrap.bin: S->C ch0 reliable msgs after HandshakeComplete,
#      before Join (the 0x11, 0x03, and PackageMap 0x07 chunks), in ChSequence order.
ctrl = {}  # chSeq -> payload
for (fn,sp,data) in frames:
    if sp != 7777 or fn >= JOIN_FRAME: continue
    d = mc.decode_packet(data, bd_max=12000)
    if not d.get("ok"): continue
    for b in d["bunches"]:
        if b["chIndex"]==0 and b["bReliable"] and b["nmt"] is not None and b["chSeq"] not in ctrl:
            # skip the handshake msgs 0x1e/0x20 (chSeq 1,2) - our server generates those
            if b["nmt"] in (0x1e, 0x20): continue
            ctrl[b["chSeq"]] = bytes.fromhex(b["payloadHex"])
rec = bytearray()
nmts=[]
for sq in sorted(ctrl):
    p = ctrl[sq]; rec += struct.pack("<I", len(p)) + p; nmts.append(p[0])
open(ROOT+r"\data\replication_bootstrap.bin","wb").write(rec)
print(f"replication_bootstrap.bin: {len(ctrl)} ctrl msgs (nmts {[hex(x) for x in nmts[:8]]}...), {len(rec)} bytes")

# ---- actor_bootstrap.bin: S->C actor bunches in the burst window (all channels,
#      in frame order, full descriptors with EXACT bits).
arec = bytearray(); n=0; chans=set()
for (fn,sp,data) in frames:
    if sp != 7777 or fn < BURST_START or fn > BURST_END: continue
    d = mc.decode_packet(data, bd_max=12000)
    if not d.get("ok"): continue
    for b in d["bunches"]:
        if b["chIndex"] < 2: continue   # ch0 handled above
        flags = (b["bOpen"]&1)|((b["bClose"]&1)<<1)|((b["bReliable"]&1)<<2)|((b["bControl"]&1)<<3)
        pay = bytes.fromhex(b["payloadHex"])
        arec += struct.pack("<HBBHI", b["chIndex"], b["chType"], flags, b["chSeq"], b["bits"]) + pay
        n+=1; chans.add(b["chIndex"])
open(ROOT+r"\data\actor_bootstrap.bin","wb").write(arec)
print(f"actor_bootstrap.bin: {n} bunches across {len(chans)} channels (ch {min(chans)}..{max(chans)}), {len(arec)} bytes")
