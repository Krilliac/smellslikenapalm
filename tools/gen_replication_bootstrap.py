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

chunks = sorted(plan["chunks"], key=lambda c: c["chSeq"])
out = bytearray()
total = 0
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
