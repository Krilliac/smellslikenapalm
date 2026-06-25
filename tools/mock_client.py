#!/usr/bin/env python3
"""RS2/UE3 mock client + handshake validation harness for the smellslikenapalm
emulator. Lets us iterate the control-channel handshake WITHOUT the retail client.

Modes:
  replay   - replay the ORIGINAL capture's client->server packets at our server
             (default 127.0.0.1:7777), capture our server's responses, decode
             both, and diff our responses against the official server's responses.
             This is ground-truth validation: it uses the real client's recorded
             packets, so it does not require us to fully understand the protocol.
  drive    - act as a UE3 client: send Hello, decode the server's Challenge, and
             (best-effort) proceed. Useful once the protocol is understood.

The bit codec mirrors src/Network/{BitReader,BitWriter,PacketCodec}: LSB-first
bits, UE3 variable-length ReadInt(Max), terminator = high bit of the packet's
last byte, BunchDataBits = ReadInt(16384) (MaxPacket=2048 for the live connection).

Usage:
  python tools/mock_client.py replay [--host 127.0.0.1] [--port 7777]
  python tools/mock_client.py drive  [--host 127.0.0.1] [--port 7777]
"""
import argparse
import socket
import subprocess
import sys
import time

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
ORIG_PCAP = r"D:\RE-Tools\rs2_handshake_capture.pcapng"
MAX_PACKETID = 16384

# ----------------------------------------------------------------------------
# Bit IO (UE3 FBitReader/FBitWriter compatible, LSB-first)
# ----------------------------------------------------------------------------
class BitReader:
    def __init__(self, data, nbits=None):
        self.d = data
        self.n = nbits if nbits is not None else len(data) * 8
        self.p = 0
        self.error = False

    def bit(self):
        if self.p >= self.n:
            self.error = True
            return 0
        v = (self.d[self.p >> 3] >> (self.p & 7)) & 1
        self.p += 1
        return v

    def rint(self, maxv):
        val = 0
        mask = 1
        while (val + mask) < maxv and not self.error:
            if self.bit():
                val |= mask
            mask <<= 1
        return val

    def ru(self, k):
        v = 0
        for i in range(k):
            if self.bit():
                v |= 1 << i
        return v

class BitWriter:
    def __init__(self):
        self.bits = []

    def bit(self, b):
        self.bits.append(1 if b else 0)

    def wint(self, val, maxv):
        mask = 1
        while mask < maxv:
            self.bit(val & mask)
            mask <<= 1

    def wu(self, val, k):
        for i in range(k):
            self.bit((val >> i) & 1)

    def to_bytes(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, b in enumerate(self.bits):
            if b:
                out[i >> 3] |= 1 << (i & 7)
        return bytes(out)

def terminator_bit(data):
    """High set bit of the LAST byte (UE3 one-packet-per-datagram rule)."""
    if not data:
        return -1
    last = data[-1]
    for i in range(7, -1, -1):
        if (last >> i) & 1:
            return (len(data) - 1) * 8 + i
    return -1

# ----------------------------------------------------------------------------
# Packet decode (PacketId, acks, bunches)
# ----------------------------------------------------------------------------
def decode_packet(data):
    """Return dict {pid, acks:[], bunches:[{flags, chIndex, chSeq, chType, bits, nmt, payloadHex}], ok}."""
    t = terminator_bit(data)
    if t < 0:
        return {"ok": False, "reason": "no terminator (all-zero)"}
    r = BitReader(data, t)
    pid = r.rint(MAX_PACKETID)
    acks = []
    bunches = []
    guard = 0
    while r.p < t and not r.error and guard < 64:
        guard += 1
        if r.bit():  # IsAck
            acks.append(r.rint(MAX_PACKETID))
            continue
        bC = r.bit()
        bO = r.bit() if bC else 0
        bCl = r.bit() if bC else 0
        bR = r.bit()
        ci = r.rint(1023)
        sq = r.rint(1024) if bR else 0
        ct = r.rint(8) if (bR or bO) else 0
        # BunchDataBits = SerializeInt(MaxPacket*8). MaxPacket is 2048 for the live
        # connection (RE'd from VNGame.exe [conn+0x10c]) => bound 16384, a 14-bit
        # field, from the very first packet. (The old rint(64) was wrong and caused
        # large bunches - e.g. the PackageMap export - to mis-decode as "partial".)
        bd = r.rint(16384)
        ps = r.p
        # payload bits
        pay = BitWriter()
        for _ in range(bd):
            pay.bit(r.bit())
        payb = pay.to_bytes()
        nmt = payb[0] if payb else None
        bunches.append({
            "bControl": bC, "bOpen": bO, "bClose": bCl, "bReliable": bR,
            "chIndex": ci, "chSeq": sq, "chType": ct, "bits": bd,
            "nmt": nmt, "payloadHex": payb.hex(),
        })
    return {"ok": not r.error, "pid": pid, "acks": acks, "bunches": bunches,
            "endpos": r.p, "termbit": t}

def fmt_packet(dec):
    if not dec.get("ok"):
        return f"<undecodable: {dec.get('reason','overflow')}>"
    parts = [f"pid={dec['pid']}"]
    if dec["acks"]:
        parts.append("ack" + ",".join(str(a) for a in dec["acks"]))
    for b in dec["bunches"]:
        nmt = f"0x{b['nmt']:02x}" if b["nmt"] is not None else "--"
        parts.append(f"B[ch{b['chIndex']} sq{b['chSeq']} O{b['bOpen']} {b['bits']}b NMT={nmt}]")
    return " ".join(parts)

# ----------------------------------------------------------------------------
# Load capture frames via tshark (subprocess: stdout pipe, no file handoff)
# ----------------------------------------------------------------------------
def load_frames(direction):
    """direction 'c2s' (dstport 7777) or 's2c' (srcport 7777). Returns [(frame, t, bytes)]."""
    flt = "udp.dstport==7777" if direction == "c2s" else "udp.srcport==7777"
    out = subprocess.run(
        [TSHARK, "-r", ORIG_PCAP, "-Y", flt, "-T", "fields",
         "-e", "frame.number", "-e", "frame.time_relative", "-e", "data.data"],
        capture_output=True, text=True)
    frames = []
    for line in out.stdout.splitlines():
        cols = line.split("\t")
        if len(cols) >= 3 and cols[0].strip() and cols[2].strip():
            try:
                frames.append((int(cols[0]), float(cols[1]), bytes.fromhex(cols[2].strip())))
            except ValueError:
                pass
    return frames

# ----------------------------------------------------------------------------
# Replay mode
# ----------------------------------------------------------------------------
def replay(host, port):
    c2s = load_frames("c2s")
    s2c = load_frames("s2c")
    if not c2s:
        print("ERROR: no C2S frames loaded from capture (tshark/pcap problem)")
        return 2
    print(f"Loaded {len(c2s)} client->server and {len(s2c)} server->client frames from capture.")
    print(f"Replaying client packets at {host}:{port} and recording OUR server's responses.\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    our_responses = []  # (after_frame, bytes)

    sent = 0
    for (frame, t, data) in c2s:
        try:
            sock.sendto(data, (host, port))
            sent += 1
        except OSError as e:
            print(f"send error: {e}")
            break
        # quick non-blocking drain (server responds per ~16ms tick; the final
        # drain below catches anything still in flight). ~5ms/packet keeps a full
        # 968-frame replay to ~5s instead of ~60s.
        deadline = time.time() + 0.005
        while time.time() < deadline:
            try:
                resp, _ = sock.recvfrom(2048)
                our_responses.append((frame, resp))
            except (BlockingIOError, socket.timeout):
                pass
            except OSError:
                break
    # final drain: keep collecting for the full window (non-blocking reads raise
    # BlockingIOError when momentarily empty - that's not end-of-data).
    deadline = time.time() + 1.5
    while time.time() < deadline:
        try:
            resp, _ = sock.recvfrom(2048)
            our_responses.append((c2s[-1][0], resp))
        except (BlockingIOError, socket.timeout):
            continue
        except OSError:
            break
    sock.close()

    print(f"=== Sent {sent} client packets; received {len(our_responses)} responses from OUR server ===\n")
    print("--- OUR server responses (decoded) ---")
    our_nmts = []
    for (after, resp) in our_responses[:60]:
        dec = decode_packet(resp)
        for b in dec.get("bunches", []):
            if b["nmt"] is not None:
                our_nmts.append(b["nmt"])
        print(f"  after C2S f{after}: {resp.hex()}  {fmt_packet(dec)}")

    print("\n--- OFFICIAL server responses (from capture, decoded) ---")
    off_nmts = []
    for (frame, t, data) in s2c[:30]:
        dec = decode_packet(data)
        for b in dec.get("bunches", []):
            if b["nmt"] is not None:
                off_nmts.append(b["nmt"])
        print(f"  f{frame}: {data.hex()[:60]}{'...' if len(data.hex())>60 else ''}  {fmt_packet(dec)}")

    print("\n=== SUMMARY ===")
    print(f"  OUR server bunch NMTs:      {[hex(x) for x in our_nmts]}")
    print(f"  OFFICIAL server bunch NMTs: {[hex(x) for x in off_nmts]}")
    return 0

# ----------------------------------------------------------------------------
# Drive mode (act as a UE3 client) - minimal, extended as protocol is learned
# ----------------------------------------------------------------------------
def drive(host, port):
    HELLO = bytes.fromhex("008005208040001d0101")  # captured opening Hello bunch
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    print(f"drive: sending Hello to {host}:{port}: {HELLO.hex()}")
    sock.sendto(HELLO, (host, port))
    for _ in range(10):
        try:
            resp, _ = sock.recvfrom(2048)
        except socket.timeout:
            print("  (timeout waiting for server)")
            break
        dec = decode_packet(resp)
        print(f"  server -> {resp.hex()}  {fmt_packet(dec)}")
    sock.close()
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["replay", "drive"])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    args = ap.parse_args()
    if args.mode == "replay":
        return replay(args.host, args.port)
    return drive(args.host, args.port)

if __name__ == "__main__":
    sys.exit(main())
