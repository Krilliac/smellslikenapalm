#!/usr/bin/env python3
"""Minimal SPECTATOR/TEAM-SELECT-MENU bootstrap from real session A:
  - replication_bootstrap.bin : the real session's post-login control stream
    (0x11, 0x03, PackageMap 0x07) - same as gen_realsession_bootstrap.
  - actor_bootstrap.bin        : ONLY the minimal menu actor opens (NO pawn):
      ch2  = PlayerController, ch54 = GRI, ch21/56/76 = TeamInfos, ch26 = local PRI.
Small (~6 actor bunches) + paced => avoids the 1147-bunch "Not responding" flood,
and pre-spawn (no pawn/possession) => matches our just-joined client's state so it
should show the team-select menu rather than freezing on mismatched spawn data.
"""
import sys, struct, subprocess
sys.path.insert(0, r"D:\smellslikenapalm\tools")
import mock_client as mc

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
PCAP = r"D:\RE-Tools\rs2_realserver_capture.pcapng"
FLT = "udp.port==57867 && udp.port==7777"
ROOT = r"D:\smellslikenapalm"
JOIN_FRAME = 1477
MENU_CHANNELS = {2, 54, 21, 56, 76, 26}   # PC, GRI, TeamInfos, local PRI

out = subprocess.run([TSHARK, "-r", PCAP, "-Y", FLT, "-T", "fields",
    "-e", "frame.number", "-e", "udp.srcport", "-e", "data.data"],
    capture_output=True, text=True)
frames = []
for line in out.stdout.splitlines():
    c = line.split("\t")
    if len(c) >= 3 and c[0].strip() and c[2].strip():
        try: frames.append((int(c[0]), int(c[1]), bytes.fromhex(c[2].strip())))
        except ValueError: pass

# control stream (unchanged: 0x11/0x03/PackageMap, skip handshake 0x1e/0x20)
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

# minimal menu actor OPENS only (one bOpen bunch per target channel), frame order
arec = bytearray(); seen=set(); rows=[]
for (fn,sp,data) in frames:
    if sp != 7777 or fn < JOIN_FRAME: continue
    d = mc.decode_packet(data, bd_max=12000)
    if not d.get("ok"): continue
    for b in d["bunches"]:
        ci=b["chIndex"]
        if ci in MENU_CHANNELS and b["bOpen"] and ci not in seen:
            seen.add(ci)
            flags = (b["bOpen"]&1)|((b["bClose"]&1)<<1)|((b["bReliable"]&1)<<2)|((b["bControl"]&1)<<3)
            pay = bytes.fromhex(b["payloadHex"])
            arec += struct.pack("<HBBHI", ci, b["chType"], flags, b["chSeq"], b["bits"]) + pay
            rows.append((fn,ci,b["chType"],b["bits"],pay[:6].hex()))
    if seen >= MENU_CHANNELS: break
open(ROOT+r"\data\actor_bootstrap.bin","wb").write(arec)
print(f"actor_bootstrap.bin: {len(seen)} menu opens (channels {sorted(seen)}), {len(arec)} bytes")
for r in rows: print("  f%d ch%d ct%d %db %s"%r)
