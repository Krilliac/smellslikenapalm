#!/usr/bin/env python3
"""Probe-driven bootstrap: the client accepted the PlayerController (ch2) but
bClosed the GRI/TeamInfo/PRI - their refs dangle because we only opened 6 of the
~143 actors. So open the FULL ref-consistent actor set, but OPENS ONLY (one bOpen
bunch per channel) - NOT the ~1000 property deltas/RPCs that caused "Not responding".

Writes data/actor_bootstrap.bin (all opens) + data/replication_bootstrap.bin
(real session A control/PackageMap, unchanged from gen_menu_bootstrap)."""
import sys, struct, subprocess
sys.path.insert(0, r"D:\smellslikenapalm\tools")
import mock_client as mc

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
PCAP = r"D:\RE-Tools\rs2_realserver_capture.pcapng"
FLT = "udp.port==57867 && udp.port==7777"
ROOT = r"D:\smellslikenapalm"
JOIN_FRAME = 1477
OPEN_END = 1525   # the initial actor-open burst (before steady-state property deltas)

out = subprocess.run([TSHARK, "-r", PCAP, "-Y", FLT, "-T", "fields",
    "-e", "frame.number", "-e", "udp.srcport", "-e", "data.data"],
    capture_output=True, text=True)
frames = []
for line in out.stdout.splitlines():
    c = line.split("\t")
    if len(c) >= 3 and c[0].strip() and c[2].strip():
        try: frames.append((int(c[0]), int(c[1]), bytes.fromhex(c[2].strip())))
        except ValueError: pass

# control stream (unchanged)
ctrl = {}
for (fn,sp,data) in frames:
    if sp != 7777 or fn >= JOIN_FRAME: continue
    d = mc.decode_packet(data, bd_max=12000)
    if not d.get("ok"): continue
    for b in d["bunches"]:
        if b["chIndex"]==0 and b["bReliable"] and b["nmt"] is not None and b["chSeq"] not in ctrl:
            if b["nmt"] in (0x1e, 0x20): continue
            ctrl[b["chSeq"]] = bytes.fromhex(b["payloadHex"])
rec = bytearray()
for sq in sorted(ctrl):
    p = ctrl[sq]; rec += struct.pack("<I", len(p)) + p
open(ROOT+r"\data\replication_bootstrap.bin","wb").write(rec)
print(f"replication_bootstrap.bin: {len(ctrl)} ctrl msgs, {len(rec)} bytes")

# ALL actor OPENS (first bOpen per channel) in the burst window, frame order
arec = bytearray(); seen=set()
for (fn,sp,data) in frames:
    if sp != 7777 or fn < JOIN_FRAME or fn > OPEN_END: continue
    d = mc.decode_packet(data, bd_max=12000)
    if not d.get("ok"): continue
    for b in d["bunches"]:
        ci=b["chIndex"]
        if ci>=2 and b["bOpen"] and ci not in seen:
            seen.add(ci)
            flags = (b["bOpen"]&1)|((b["bClose"]&1)<<1)|((b["bReliable"]&1)<<2)|((b["bControl"]&1)<<3)
            pay = bytes.fromhex(b["payloadHex"])
            arec += struct.pack("<HBBHI", ci, b["chType"], flags, b["chSeq"], b["bits"]) + pay
open(ROOT+r"\data\actor_bootstrap.bin","wb").write(arec)
print(f"actor_bootstrap.bin: {len(seen)} actor opens (ch {min(seen)}..{max(seen)}), {len(arec)} bytes")
