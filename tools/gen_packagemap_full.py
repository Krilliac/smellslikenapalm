#!/usr/bin/env python3
"""Re-extract the COMPLETE initial PackageMap from the real-server capture and rebuild
data/replication_bootstrap.bin.

ROOT CAUSE this fixes: the previous export (data/packagemap_export_7258.bin, 20 chunks /
22852 bytes) was TRUNCATED. The real server sends the initial PackageMap as ~29 NMT_Uses
(0x07) control bunches (capture frames f1336..f1363, ~34KB) BEFORE the first actor open at
f1484. Sending only 20 left the client's static-object index space short: low-index class
refs (the PlayerController class @57520) resolved so ch2 survived, but higher-index classes
(GRI @70887, PRI @86701, TeamInfo @90245) fell in the missing tail packages and did NOT
resolve -> the client closed all 138 non-PC actor channels (verified in server_live.log:
139 empty bClose bunches on ch3..ch140, ch2 kept).

This script pulls EVERY ch0 0x07 bunch payload before the first actor open, in capture
order, and writes them as the PackageMap records (preceded by the NMT 0x11 + 0x03 messages
the client needs first). Output stream format unchanged: [uint32 LE len][payload] records.
"""
import sys, struct, subprocess, os
sys.path.insert(0, r"D:\smellslikenapalm\tools")
import mock_client as mc

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
PCAP   = r"D:\RE-Tools\rs2_realserver_capture.pcapng"
ROOT   = r"D:\smellslikenapalm"
FIRST_ACTOR_FRAME = 1484  # first S2C actor open (ch2) in the capture

# Pre-PackageMap control messages (verbatim from capture f1331/f1334): the client needs
# these before the map or it tears down ch0. (Same bytes the old generator used.)
PRE_PACKAGEMAP = [
    bytes.fromhex("11010000000000000000"),                  # NMT 0x11 (post-login setup)
    bytes.fromhex("035a1c000009000000303235343743353800"),  # NMT 0x03 Challenge (ver 7258)
]

o = subprocess.run(
    [TSHARK, "-r", PCAP, "-Y", "udp.srcport==7777 && udp.dstport==57867",
     "-T", "fields", "-e", "frame.number", "-e", "data.data"],
    capture_output=True, text=True)

records = []
for line in o.stdout.splitlines():
    c = line.split("\t")
    if len(c) < 2 or not c[1].strip():
        continue
    fn = int(c[0])
    if fn >= FIRST_ACTOR_FRAME:
        break  # only the INITIAL PackageMap (everything before the first actor)
    d = mc.decode_packet(bytes.fromhex(c[1].strip()), bd_max=12000)
    if not d.get("ok"):
        continue
    for b in d["bunches"]:
        if b["chIndex"] == 0 and b["bits"] > 0 and b["payloadHex"][:2] == "07":
            records.append(bytes.fromhex(b["payloadHex"]))

if not records:
    print("ERROR: no PackageMap bunches extracted")
    sys.exit(1)

# Rewrite the full export blob too (for provenance / other tooling).
blob = b"".join(records)
with open(os.path.join(ROOT, "data", "packagemap_export_7258.bin"), "wb") as f:
    f.write(blob)

out = bytearray()
for p in PRE_PACKAGEMAP:
    out += struct.pack("<I", len(p)) + p
for p in records:
    out += struct.pack("<I", len(p)) + p

out_path = os.path.join(ROOT, "data", "replication_bootstrap.bin")
with open(out_path, "wb") as f:
    f.write(out)

print(f"wrote {out_path}")
print(f"  PackageMap 0x07 bunches: {len(records)} (was 20)")
print(f"  PackageMap payload bytes: {len(blob)} (was 22852)")
print(f"  total records: {len(PRE_PACKAGEMAP)+len(records)}  total file bytes: {len(out)}")
print(f"  bunch sizes: {[len(p) for p in records]}")
