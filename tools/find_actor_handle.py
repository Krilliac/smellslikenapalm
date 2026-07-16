#!/usr/bin/env python3
"""Stream a UE3 pcap and report actor bunches whose first net-field handle matches.

This is intentionally a narrow reverse-engineering probe: it does not guess RPC
parameter layouts or walk combined bunches. It finds capture examples where the
requested property/function is the first field, preserving the exact payload and
bit count for follow-up semantic decoding.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mock_client as mc  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pcap", required=True)
    parser.add_argument("--handle", type=int, required=True)
    parser.add_argument("--max-handle", type=int, required=True)
    parser.add_argument("--channel", type=int, default=2)
    parser.add_argument("--server-port", type=int, default=7777)
    parser.add_argument("--frame-start", type=int)
    parser.add_argument("--frame-end", type=int)
    parser.add_argument("--any-offset", action="store_true",
                        help="scan every payload bit offset instead of only the first field")
    parser.add_argument("--tshark", default=r"C:\Program Files\Wireshark\tshark.exe")
    args = parser.parse_args()

    display_filter = f"udp.srcport=={args.server_port}"
    if args.frame_start is not None:
        display_filter += f" && frame.number>={args.frame_start}"
    if args.frame_end is not None:
        display_filter += f" && frame.number<={args.frame_end}"
    cmd = [
        args.tshark,
        "-r", args.pcap,
        "-Y", display_filter,
        "-T", "fields",
        "-e", "frame.number",
        "-e", "frame.time_relative",
        "-e", "data.data",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace")
    assert proc.stdout is not None

    matches = 0
    decoded = 0
    for line in proc.stdout:
        cols = line.rstrip("\r\n").split("\t")
        if len(cols) < 3 or not cols[2]:
            continue
        try:
            frame = int(cols[0])
            timestamp = float(cols[1])
            datagram = bytes.fromhex(cols[2])
        except ValueError:
            continue

        packet = mc.decode_packet(datagram, bd_max=12000)
        if not packet.get("ok"):
            continue
        decoded += 1
        for bunch in packet.get("bunches", []):
            if (bunch.get("chIndex") != args.channel or bunch.get("bOpen") or
                    not bunch.get("payloadHex") or bunch.get("bits", 0) <= 0):
                continue
            payload = bytes.fromhex(bunch["payloadHex"])
            offsets = [0]
            if args.any_offset:
                needle = mc.sint_bits(args.handle, args.max_handle)
                haystack = [
                    (payload[bit >> 3] >> (bit & 7)) & 1
                    for bit in range(bunch["bits"])
                ]
                offsets = [
                    bit for bit in range(0, len(haystack) - len(needle) + 1)
                    if haystack[bit:bit + len(needle)] == needle
                ]
            for offset in offsets:
                reader = mc.BitReader(payload, bunch["bits"])
                reader.p = offset
                first_handle = reader.rint(args.max_handle)
                if reader.error or first_handle != args.handle:
                    continue
                matches += 1
                print(json.dumps({
                    "frame": frame,
                    "time": timestamp,
                    "packetId": packet.get("pid"),
                    "channel": bunch["chIndex"],
                    "sequence": bunch.get("chSeq"),
                    "reliable": bool(bunch.get("bReliable")),
                    "bits": bunch["bits"],
                    "bitOffset": offset,
                    "firstHandle": first_handle,
                    "payloadHex": bunch["payloadHex"],
                }, separators=(",", ":")))

    stderr = proc.stderr.read() if proc.stderr is not None else ""
    rc = proc.wait()
    if rc != 0:
        if stderr:
            sys.stderr.write(stderr)
        return rc
    print(f"find_actor_handle: decoded={decoded} matches={matches}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
