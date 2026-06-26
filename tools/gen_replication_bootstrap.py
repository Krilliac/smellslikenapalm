#!/usr/bin/env python3
"""Generate data/replication_bootstrap.bin (the post-Join world-replication send
stream consumed by ConnectionManager::SendReplicationBootstrap) from the canonical
capture-reconstructed PackageMap export.

Inputs (produced by the PackageMap RE):
  data/packagemap_export_7258.bin   - all 20 NMT_Uses(0x07) bunch payloads concatenated
  data/packagemap_chunks.json       - per-bunch {blob_offset, payload_bytes} send plan

Output:
  data/replication_bootstrap.bin    - stream of [uint32 LE len][payload] records, one
                                       per control-channel message to send in order.
"""
import json, struct, os, sys

ROOT = r"D:\smellslikenapalm"
blob_path = os.path.join(ROOT, "data", "packagemap_export_7258.bin")
json_path = os.path.join(ROOT, "data", "packagemap_chunks.json")
out_path  = os.path.join(ROOT, "data", "replication_bootstrap.bin")

with open(blob_path, "rb") as f:
    blob = f.read()
with open(json_path, "r", encoding="utf-8") as f:
    plan = json.load(f)

# The official server's post-HandshakeComplete control messages that PRECEDE the
# PackageMap (reversed from the capture S2C frames f162 sq3, f165 sq4):
#   NMT 0x11 (f162): setup message the server sends right after the Steam login.
#   NMT 0x03 (f165): NMT_Challenge { int32 version=7258, int32, FString challenge }.
# The client needs these BEFORE the PackageMap; our old fake NMT_Welcome(0x01)
# made it close the control channel. Replay them verbatim as the first two records.
PRE_PACKAGEMAP = [
    bytes.fromhex("11010000000000000000"),                    # f162 NMT 0x11
    bytes.fromhex("035a1c000009000000303235343743353800"),    # f165 NMT 0x03 Challenge
]

chunks = sorted(plan["chunks"], key=lambda c: c["chSeq"])
out = bytearray()
total = 0
for p in PRE_PACKAGEMAP:
    out += struct.pack("<I", len(p)) + p
    total += len(p)
for c in chunks:
    off, n = c["blob_offset"], c["payload_bytes"]
    payload = blob[off:off + n]
    if len(payload) != n:
        print(f"ERROR chSeq {c['chSeq']}: wanted {n} bytes at {off}, got {len(payload)}")
        sys.exit(1)
    if payload[0] != 0x07:
        print(f"WARN chSeq {c['chSeq']}: payload[0]=0x{payload[0]:02x} (expected 0x07)")
    out += struct.pack("<I", n) + payload
    total += n

with open(out_path, "wb") as f:
    f.write(out)
print(f"wrote {out_path}: {len(chunks)} records, {total} payload bytes, {len(out)} total bytes")
print(f"chSeqs {chunks[0]['chSeq']}..{chunks[-1]['chSeq']}, sizes {[c['payload_bytes'] for c in chunks]}")
