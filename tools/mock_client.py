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
  reconnect - complete two handshakes on the same UDP socket, resetting client
               PacketId/ChSequence between them. Validates same-endpoint teardown.
  spawn    - drive menu-to-spawn replication and require at least one empty UE3
             transport keepalive while collecting the server's responses.

The bit codec mirrors src/Network/{BitReader,BitWriter,PacketCodec}: LSB-first
bits, UE3 value-dependent SerializeInt(Max), terminator = high bit of the packet's
last byte, and direction-specific live bounds (C2S=10240, S2C=12000).

Usage:
  python tools/mock_client.py replay [--host 127.0.0.1] [--port 7777]
  python tools/mock_client.py drive  [--host 127.0.0.1] [--port 7777]
  python tools/mock_client.py reconnect [--host 127.0.0.1] [--port 7777]
  python tools/mock_client.py spawn [--host 127.0.0.1] [--port 7777] [--linger 30]
  python tools/mock_client.py spawn --team 2
  python tools/mock_client.py spawn --profile cu-chi  # canonical artifact only
  python tools/mock_client.py spawn --profile compound --artifact installed
"""
import argparse
import math
import socket
import subprocess
import sys
import time
from dataclasses import dataclass

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
        newval = 0
        mask = 1
        # UE3 FBitWriter::SerializeInt is value-dependent, matching FBitReader:
        # once val+mask reaches Max, that bit cannot affect a valid value and is
        # omitted. A fixed ceil(log2(Max)) writer silently inserts padding for
        # low handles (the exact bug that turned ClientRestart(NewPawn) into None).
        while (newval + mask) < maxv:
            is_set = bool(val & mask)
            self.bit(is_set)
            if is_set:
                newval |= mask
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
def decode_packet(data, bd_max=12000):
    """Return dict {pid, acks:[], bunches:[{flags, chIndex, chSeq, chType, bits, nmt, payloadHex}], ok}.

    bd_max is the BunchDataBits SerializeInt bound = MaxPacket*8 for the wire
    direction. Retail RS2 uses 10240 (1280-byte MaxPacket) C2S and 12000
    (1500-byte MaxPacket) S2C. Pass the direction-appropriate value or payloads
    misalign; the default is S2C because this harness mostly decodes server replies.
    """
    t = terminator_bit(data)
    if t < 0:
        return {"ok": False, "reason": "no terminator (all-zero)"}
    r = BitReader(data, t)
    pid = r.rint(MAX_PACKETID)
    acks = []
    bunches = []
    guard = 0
    while r.p < t and not r.error and guard < 256:
        guard += 1
        if r.bit():  # IsAck
            acks.append(r.rint(MAX_PACKETID))
            continue
        bC = r.bit()
        bO = r.bit() if bC else 0
        bCl = r.bit() if bC else 0
        bR = r.bit()
        ci = r.rint(1024)
        sq = r.rint(1024) if bR else 0
        ct = r.rint(8) if (bR or bO) else 0
        bd = r.rint(bd_max)
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

# ----------------------------------------------------------------------------
# Packet encode (mirror of src/Network/PacketCodec::Encode) - for the reactive
# client. A bunch dict: {bControl,bOpen,bClose,bReliable,chIndex,chType,chSeq,
# payload(bytes)}.
# ----------------------------------------------------------------------------
def encode_packet(pid, bunches, max_packet_bytes, acks=None):
    w = BitWriter()
    w.wint(pid, MAX_PACKETID)
    for a in (acks or []):
        w.bit(1)           # IsAck
        w.wint(a, MAX_PACKETID)
    bd_max = max_packet_bytes * 8
    for b in bunches:
        w.bit(0)           # not an ack
        bC = b.get("bControl", 0)
        w.bit(bC)
        if bC:
            w.bit(b.get("bOpen", 0))
            w.bit(b.get("bClose", 0))
        bR = b.get("bReliable", 1)
        w.bit(bR)
        w.wint(b["chIndex"], 1024)
        if bR:
            w.wint(b["chSeq"], 1024)
        if bR or b.get("bOpen", 0):
            w.wint(b["chType"], 8)
        if "bits_payload" in b:
            # Bit-level payload (e.g. a UE3 function-call RPC: SerializeInt(handle) +
            # per-param Send bit + value), which is NOT byte-aligned. A list of 0/1 bits.
            pbits = b["bits_payload"]
            w.wint(len(pbits), bd_max)
            for bit in pbits:
                w.bit(bit)
        else:
            payload = b["payload"]
            bits = len(payload) * 8
            w.wint(bits, bd_max)
            for byte in payload:
                for i in range(8):
                    w.bit((byte >> i) & 1)
    w.bit(1)               # terminator (high set bit of the last byte)
    return w.to_bytes()


def make_control_close(sequence):
    """Build UE3's canonical empty reliable close for control channel zero."""
    return {
        "bControl": 1,
        "bOpen": 0,
        "bClose": 1,
        "bReliable": 1,
        "chIndex": 0,
        "chType": 1,
        "chSeq": sequence,
        "payload": b"",
    }


def acknowledge_server_bunch_packet(sock, server, packet_id, decoded,
                                     max_packet_bytes=1280):
    """ACK one decoded server packet iff it carried bunch data.

    UE3 does not ACK pure ACK or empty keepalive packets.  Keeping this rule in
    one helper prevents the live mock modes from either starting an ACK-of-ACK
    loop or abandoning a server reliable ledger when their socket exits.
    Returns ``(next_packet_id, acknowledged)``.
    """
    if (not decoded.get("ok") or not decoded.get("bunches") or
            "pid" not in decoded):
        return packet_id, False

    wire = encode_packet(
        packet_id % MAX_PACKETID, [], max_packet_bytes,
        acks=[decoded["pid"]])
    sock.sendto(wire, server)
    return (packet_id + 1) % MAX_PACKETID, True


def send_graceful_control_close(sock, server, packet_id, sequence,
                                max_packet_bytes=1280):
    """Send UE3's empty reliable ch0 close and return wrapped cursors."""
    close = make_control_close(sequence % 1024)
    wire = encode_packet(
        packet_id % MAX_PACKETID, [close], max_packet_bytes)
    sock.sendto(wire, server)
    return ((packet_id + 1) % MAX_PACKETID, (sequence + 1) % 1024)


def sint_bits(val, maxv):
    """Return SerializeInt(val, maxv) as a list of LSB-first bits (UE3 FBitWriter::WriteInt)."""
    w = BitWriter()
    w.wint(val, maxv)
    return list(w.bits)

def packed_bits(hex_payload, nbits):
    """Expand an LSB-first packed payload to the exact number of wire bits."""
    raw = bytes.fromhex(hex_payload)
    return [(raw[i >> 3] >> (i & 7)) & 1 for i in range(nbits)]

# ----------------------------------------------------------------------------
# Reactive mode: act as a real UE3 client - drive the StatelessConnect handshake
# and the NMT login LIVE against our server, decoding/validating each response
# direction-aware (C2S bound 10240, S2C bound 12000). This
# is the ONLY harness that validates our SEND path (replay only tests receive).
# ----------------------------------------------------------------------------
def react(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.5)
    seq = 1
    pid = 0
    ok = True

    def step(name, payload, max_pkt, expect_prefix, bd_decode, want_open=False):
        # expect_prefix: bytes the server's response bunch payload must START with
        # (e.g. b"\x00\x1e" for HandshakeChallenge, b"\x01" for Welcome), or None.
        nonlocal seq, pid, ok
        b = {"bControl": 1 if want_open else 0, "bOpen": 1 if want_open else 0,
             "bClose": 0, "bReliable": 1, "chIndex": 0, "chType": 1,
             "chSeq": seq, "payload": payload}
        dg = encode_packet(pid, [b], max_pkt)
        sock.sendto(dg, (host, port))
        print(f"  -> {name}: sent pid={pid} seq={seq} payload={payload.hex()} ({max_pkt}B)")
        seq += 1
        pid += 1
        got = []
        payloads = []
        # A healthy server sends an empty UE3 transport keepalive every second,
        # so waiting for socket silence makes this validation hang forever. Use
        # a hard response window and retire reliable server packets as they are
        # observed; otherwise the mock itself creates a retransmission storm
        # that can obscure the next handshake step.
        deadline = time.monotonic() + 2.0
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                resp, _ = sock.recvfrom(4096)
            except socket.timeout:
                break
            dec = decode_packet(resp, bd_max=bd_decode)
            got.append(dec)
            for b2 in dec.get("bunches", []):
                if b2["payloadHex"]:
                    payloads.append(b2["payloadHex"])
            print(f"     <- {fmt_packet(dec)}")
            pid, _ = acknowledge_server_bunch_packet(
                sock, (host, port), pid, dec, max_pkt)
        sock.settimeout(1.5)
        if expect_prefix is not None:
            pref = expect_prefix.hex()
            if any(ph.startswith(pref) for ph in payloads):
                print(f"     OK: server response payload starts with {pref}")
            else:
                print(f"     FAIL: expected payload prefix {pref}, got payloads {payloads}")
                ok = False
        return got

    print(f"react: live handshake against {host}:{port}\n")
    # The whole connection uses the established bound from packet 1 (NO small-bound
    # phase): the client (us) sends C2S at MaxPacket 1280; the server sends S2C at
    # ~1500 (bound 12000). The handshake NMT byte (0x1d/0x1f) is the FIRST payload
    # byte - there is NO 0x00 family prefix.
    SERVER_BD = 1500 * 8
    # 1. StatelessConnect: HandshakeStart [1d 01] -> HandshakeChallenge [1e + nonce].
    step("HandshakeStart 0x1d", bytes([0x1d, 0x01]), 1280, bytes([0x1e]), SERVER_BD, want_open=True)
    # 2. HandshakeResponse [1f ...] -> HandshakeComplete [20].
    step("HandshakeResponse 0x1f", bytes([0x1f, 0x00, 0x00, 0x00, 0x00]), 1280, bytes([0x20]), SERVER_BD)
    # 3. NMT phase: Steam login (0x10) -> the server sends NMT 0x11 (then 0x03 then
    #    PackageMap) - NOT NMT_Welcome(0x01).
    login_got = step("SteamLogin 0x10", bytes([0x10, 0x00, 0x00, 0x00]), 1280, bytes([0x11]), SERVER_BD)
    pkgmap = sum(1 for d in login_got for b2 in d.get("bunches", [])
                 if b2["chIndex"] == 0 and b2["nmt"] == 0x07)
    if pkgmap:
        print(f"     REPLICATION: server sent {pkgmap} PackageMap (NMT 0x07) bunch(es) after login")
    else:
        print("     (no PackageMap after login - replication bootstrap not loaded)")
    # 4. Join (0x09) -> server reaches Joined; post-Join the server should send the
    #    world-replication bootstrap (PackageMap export = NMT 0x07 bunches). If the
    #    server has no bootstrap data loaded, this is just an ack (no 0x07) - which
    #    is still a PASS for the handshake, with a note.
    join_got = step("Join 0x09", bytes([0x09]), 1280, None, SERVER_BD)
    actor_chans = sorted({b2["chIndex"] for d in join_got for b2 in d.get("bunches", [])
                          if b2["chIndex"] >= 2 and b2["bOpen"]})
    if actor_chans:
        print(f"     REPLICATION: server opened bootstrap actor channels {actor_chans} after Join")
    else:
        print("     (no actor channels opened after Join - actor bootstrap not loaded)")

    close_pid = pid
    close_seq = seq
    pid, seq = send_graceful_control_close(
        sock, (host, port), pid, seq)
    print(f"  -> graceful control close: pid={close_pid} seq={close_seq}")
    sock.close()
    print("\n=== react: " + ("PASS - handshake sends are well-formed" if ok else "FAIL - see above") + " ===")
    return 0 if ok else 1

# ----------------------------------------------------------------------------
# Reconnect mode: finish a session, then reuse the exact same UDP endpoint for a
# fresh ch0 open whose PacketId/ChSequence restart at 0/1. This reproduces the
# retail client's "disconnect/reopen still stuck until hard server restart" case.
# The second HandshakeStart must receive a new 0x1e challenge, and the replacement
# session must still reach Join (proving stale player/capacity state was removed).
# ----------------------------------------------------------------------------
def reconnect(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.1)
    SERVER_BD = 1500 * 8
    ok = True

    def exchange(state, label, payload, expect_prefix=None, want_open=False):
        nonlocal ok
        bunch = {
            "bControl": 1 if want_open else 0,
            "bOpen": 1 if want_open else 0,
            "bClose": 0,
            "bReliable": 1,
            "chIndex": 0,
            "chType": 1,
            "chSeq": state["seq"],
            "payload": payload,
        }
        dg = encode_packet(state["pid"], [bunch], 1280)
        sock.sendto(dg, (host, port))
        print(f"  -> {label}: pid={state['pid']} seq={state['seq']} payload={payload.hex()}")
        state["pid"] += 1
        state["seq"] += 1

        payloads = []
        decoded = []
        packets = 0
        deadline = time.time() + 1.0
        quiet_since = None
        while time.time() < deadline:
            try:
                resp, _ = sock.recvfrom(4096)
            except socket.timeout:
                if quiet_since is None:
                    quiet_since = time.time()
                if time.time() - quiet_since >= 0.15:
                    break
                continue
            quiet_since = None
            dec = decode_packet(resp, bd_max=SERVER_BD)
            decoded.append(dec)
            packets += 1
            for b2 in dec.get("bunches", []):
                if b2["payloadHex"]:
                    payloads.append(b2["payloadHex"])
            state["pid"], acknowledged = acknowledge_server_bunch_packet(
                sock, (host, port), state["pid"], dec)
            if acknowledged:
                state["acked_server_packets"] += 1

        if expect_prefix is not None:
            wanted = expect_prefix.hex()
            found = any(p.startswith(wanted) for p in payloads)
            print(f"     {'OK' if found else 'FAIL'}: expected {wanted}, "
                  f"received {packets} packet(s)")
            if not found:
                ok = False
        else:
            print(f"     received {packets} packet(s)")
        return decoded

    def complete_session(number):
        state = {"pid": 0, "seq": 1, "acked_server_packets": 0}
        print(f"\n  session {number}")
        start_packets = exchange(
            state, "HandshakeStart", bytes([0x1d, 0x01]), bytes([0x1e]), True)
        exchange(state, "HandshakeResponse", bytes([0x1f, 0, 0, 0, 0]), bytes([0x20]))
        exchange(state, "SteamLogin", bytes([0x10, 0, 0, 0]), bytes([0x11]))
        join_packets = exchange(state, "Join", bytes([0x09]))

        if number == 2:
            # A new challenge alone could be a false-green if the old outbound
            # assembler survived. Require the replacement connection's first
            # response to restart both PacketId and reliable ch0 ChSequence.
            reset_challenge = any(
                d.get("pid") == 0 and
                any(b["chIndex"] == 0 and b["chSeq"] == 1 and
                    b["payloadHex"].startswith("1e")
                    for b in d.get("bunches", []))
                for d in start_packets)
            opened = {
                b["chIndex"] for d in join_packets for b in d.get("bunches", [])
                if b["bOpen"] and b["chIndex"] >= 2
            }
            joined_again = 2 in opened
            print(f"     replacement reset pid/ch0 sequence: "
                  f"{'yes' if reset_challenge else 'NO'}")
            print(f"     replacement Join opened owning PC ch2: "
                  f"{'yes' if joined_again else 'NO'}")
            if not reset_challenge or not joined_again:
                ok = False
        print(f"     acknowledged {state['acked_server_packets']} "
              "server bunch packet(s)")
        return state

    print(f"reconnect: two sessions on one UDP endpoint against {host}:{port}")
    complete_session(1)
    # Do not close/rebind: keeping this socket is the key regression condition.
    replacement = complete_session(2)
    close_pid = replacement["pid"]
    close_seq = replacement["seq"]
    replacement["pid"], replacement["seq"] = send_graceful_control_close(
        sock, (host, port), close_pid, close_seq)
    print(f"  -> graceful replacement-session close: pid={close_pid} "
          f"seq={close_seq}")
    sock.close()

    print("\n=== reconnect: " +
          ("PASS - fresh same-endpoint session replaced Joined state"
           if ok else "FAIL - second HandshakeStart was swallowed by stale state") + " ===")
    return 0 if ok else 1

# ----------------------------------------------------------------------------
# Spawn mode: drive the FULL menu->spawn path as a UE3 client - handshake, Join,
# SelectTeam(170), SelectRoleByClass(175), ServerSetSpawnSelect(261), and
# ServerSetReadyToSpawn(434) - then verify the server reacts by opening the pawn
# channel and its loadout contract. This validates our SEND path server-side
# WITHOUT the retail client, including capture-exact object references, camera,
# and attachment deltas. It canNOT prove the real client renders the first-person
# weapon mesh; that still needs a real-client test. Reliable sequence numbers are
# channel-local in UE3, so control channel 0, actor channel 2, and the inventory
# manager actor channel each start at sequence 1. Menu RPCs include their complete
# parameter bodies; a bare SerializeInt(handle, 531) is not a valid role selection.
# See docs/re/pawn_spawn_replication.md.
# ----------------------------------------------------------------------------
ROPC_MAXHANDLE = 531        # kRoPcMaxHandle - PlayerController ClassNetCache max handle
SELECT_TEAM = 170           # ROPlayerController.SelectTeam(byte TeamID)
SELECT_ROLE = 175           # ROPlayerController.SelectRoleByClass(...)
SET_SPAWN = 261             # ROPlayerController.ServerSetSpawnSelect(byte)
SET_READY = 434             # ROPlayerController.ServerSetReadyToSpawn(enum)
LOCAL_PAWN_CH = 209         # ConnectionManager::kLocalPawnChannel
REMOTE_PARTICIPANT_FIRST_CH = 512
REMOTE_PARTICIPANT_LAST_CH = 767


@dataclass(frozen=True)
class SpawnTeamContract:
    """Capture-pinned wire expectations for one owning faction graph."""

    team_id: int
    retail_team_id: int
    label: str
    role_payload_hex: str
    pawn_class_ref: int
    loadout_classes: tuple
    weapon_chain: tuple
    weapon_max_handles: tuple
    attachments: tuple
    attachment_payload_bits: int
    attachment_payload_hex: str
    current_attachment_class_ref: int
    current_attachment_payload_hex: str
    final_tail_bits: int
    final_tail_hex: str
    minimum_switch_best_weapon_count: int
    north_graph: bool = False

    @property
    def loadout_class_map(self):
        return dict(self.loadout_classes)

    @property
    def weapon_max_handle_map(self):
        return dict(self.weapon_max_handles)

    @property
    def weapon_next_map(self):
        return {
            channel: (self.weapon_chain[index + 1]
                      if index + 1 < len(self.weapon_chain) else 0)
            for index, channel in enumerate(self.weapon_chain)
        }


@dataclass(frozen=True)
class SpawnWireContract:
    """PackageMap-specific static refs and their exact pawn deltas."""

    pawn_class_ref: int
    loadout_classes: tuple
    attachments: tuple
    attachment_payload_bits: int
    attachment_payload_hex: str
    current_attachment_class_ref: int
    current_attachment_payload_bits: int
    current_attachment_payload_hex: str

    @property
    def loadout_class_map(self):
        return dict(self.loadout_classes)


SOUTH_SPAWN_TEAM = SpawnTeamContract(
    team_id=1,
    retail_team_id=1,
    label="South/US",
    role_payload_hex="af465c150080c301",
    pawn_class_ref=286151,
    loadout_classes=((210, 286374), (211, 286391), (212, 286464),
                     (213, 286109), (214, 286389), (219, 82735)),
    weapon_chain=(210, 211, 212, 213, 214),
    weapon_max_handles=((210, 99), (211, 99), (212, 101),
                        (213, 99), (214, 101)),
    attachments=((0, 286936), (1, 286946), (2, 287063),
                 (3, 286944), (5, 286126)),
    attachment_payload_bits=245,
    attachment_payload_hex=(
        "a700608311004e03100723009c0a70154600381d001c8c00705a806b170100"),
    current_attachment_class_ref=286936,
    current_attachment_payload_hex="93b0c10800",
    final_tail_bits=119,
    final_tail_hex="1c7080eaca02040000000800000010",
    minimum_switch_best_weapon_count=6,
)

NORTH_SPAWN_TEAM = SpawnTeamContract(
    team_id=2,
    retail_team_id=0,
    label="North/NVA",
    role_payload_hex="af4456150080c301",
    pawn_class_ref=286147,
    loadout_classes=((210, 286271), (212, 286804),
                     (214, 286758), (219, 82735)),
    weapon_chain=(210, 212, 214),
    weapon_max_handles=((210, 99), (212, 101), (214, 101)),
    attachments=((0, 286845), (2, 287188), (4, 287147)),
    # Three 49-bit h167 records followed by h148 Encumbrance=9.92.
    attachment_payload_bits=187,
    attachment_payload_hex="a700f48111004e05a00e23009c12b01a4600a094c2f50802",
    current_attachment_class_ref=286845,
    current_attachment_payload_hex="93fac00800",
    # Exact f63525 h28(false), h28(false), h390, h226, h101(false,true,false,false).
    final_tail_bits=51,
    final_tail_hex="1c7060585c1901",
    minimum_switch_best_weapon_count=4,
    north_graph=True,
)

SPAWN_TEAM_CONTRACTS = (SOUTH_SPAWN_TEAM, NORTH_SPAWN_TEAM)

# Installed VNSK-Compound live requests pinned by
# tests/RoleSelectionReplicationTests.cpp. South's final request is followed by
# h451 only; North's captured final request also carries the exact h89 default
# spectator-location tail.
COMPOUND_ROLE_REQUESTS = {
    1: ("af565c150080c301", 57),
    2: ("af94561500180000000080c3b300", 107),
}

# Source-grounded VNTE-CuChi Territories class-0 requests under the historical
# role-registry base token 39479. The canonical ROGame package's actual
# ObjectBase is 39478; do not conflate the two. These preserve the captured
# 57-bit h175+h451 shape, but use Cu Chi's force-substituted US Grunt / NLF
# Guerilla CDOs instead of Resort's Skirmish role objects.
CU_CHI_ROLE_REQUESTS = {
    1: ("af265c150080c301", 57),
    2: ("af6456150080c301", 57),
}

# Source-grounded installed PackageMap layout. RetailBootstrapTests pins the
# ROGameContent +5 export shift for every actor/attachment below and pins the
# inventory-manager CDO separately at 82737. The payloads are those exact refs
# serialized into the capture-matched h167 list and h147 current-attachment
# property shapes.
COMPOUND_SOUTH_WIRE = SpawnWireContract(
    pawn_class_ref=286156,
    loadout_classes=((210, 286379), (211, 286396), (212, 286469),
                     (213, 286114), (214, 286394), (219, 82737)),
    attachments=((0, 286941), (1, 286951), (2, 287068),
                 (3, 286949), (5, 286131)),
    attachment_payload_bits=245,
    attachment_payload_hex=(
        "a700748311004e03380723009c0ac0154600381da01c8c00705ac06c170100"),
    current_attachment_class_ref=286941,
    current_attachment_payload_bits=40,
    current_attachment_payload_hex="93bac10800",
)

COMPOUND_NORTH_WIRE = SpawnWireContract(
    pawn_class_ref=286152,
    loadout_classes=((210, 286276), (212, 286809),
                     (214, 286763), (219, 82737)),
    attachments=((0, 286850), (2, 287193), (4, 287152)),
    attachment_payload_bits=187,
    attachment_payload_hex="a700088211004e05c80e23009c12001b4600a094c2f50802",
    current_attachment_class_ref=286850,
    current_attachment_payload_bits=40,
    current_attachment_payload_hex="9304c10800",
)

COMPOUND_WIRE_CONTRACTS = {
    1: COMPOUND_SOUTH_WIRE,
    2: COMPOUND_NORTH_WIRE,
}


def resolve_spawn_team(team_id=1):
    """Return the immutable faction contract; direct callers fail closed."""
    if isinstance(team_id, bool) or not isinstance(team_id, int):
        raise ValueError("team must be integer 1 (South) or 2 (North)")
    for contract in SPAWN_TEAM_CONTRACTS:
        if contract.team_id == team_id:
            return contract
    raise ValueError("team must be 1 (South) or 2 (North)")


def resolve_spawn_wire_contract(team_id=1, profile="resort",
                                artifact_variant="canonical"):
    """Resolve PackageMap-specific owning-pawn validation expectations."""
    contract = resolve_spawn_team(team_id)
    validate_role_spawn_support(profile, artifact_variant)
    if profile == "compound":
        return COMPOUND_WIRE_CONTRACTS[contract.team_id]
    return SpawnWireContract(
        pawn_class_ref=contract.pawn_class_ref,
        loadout_classes=contract.loadout_classes,
        attachments=contract.attachments,
        attachment_payload_bits=contract.attachment_payload_bits,
        attachment_payload_hex=contract.attachment_payload_hex,
        current_attachment_class_ref=contract.current_attachment_class_ref,
        current_attachment_payload_bits=40,
        current_attachment_payload_hex=contract.current_attachment_payload_hex,
    )


def resolve_north_role_transition_contract(profile="resort",
                                           artifact_variant="canonical"):
    """Return the exact h210+h211 transition for a clean North spawn."""
    validate_role_spawn_support(profile, artifact_variant)
    if profile in ("compound", "cu-chi"):
        # Compound and Cu Chi allocate the first free runtime squad/role slot.
        # On a clean server that is 0/0, so both h211 byte parameters use their
        # defaults. Do not import Resort capture occupancy (2/3) into either
        # live map profile.
        return 32, "d2fe731a"
    # Resort frame 61989 captured h210(255,0,false,true)+h211(2,3).
    return 48, "d2fe735a8103"


def build_team_selection_bits(team_id=1):
    """Build SelectTeam(h170), converting server 1/2 to retail US/NVA 1/0."""
    contract = resolve_spawn_team(team_id)
    bits = sint_bits(SELECT_TEAM, ROPC_MAXHANDLE)
    if contract.retail_team_id == 0:
        return bits + [0]  # default byte is omitted by UE3's presence bit
    return bits + [1] + [(contract.retail_team_id >> i) & 1 for i in range(8)]


def build_role_selection_bits(team_id=1, profile="resort",
                              artifact_variant="canonical"):
    """Return the profile- and faction-exact captured final role request."""
    contract = resolve_spawn_team(team_id)
    validate_role_spawn_support(profile, artifact_variant)
    if profile == "compound":
        payload_hex, payload_bits = COMPOUND_ROLE_REQUESTS[contract.team_id]
    elif profile == "cu-chi":
        payload_hex, payload_bits = CU_CHI_ROLE_REQUESTS[contract.team_id]
    else:
        payload_hex, payload_bits = contract.role_payload_hex, 57
    return packed_bits(payload_hex, payload_bits)

def is_remote_participant_pawn_channel(channel):
    """True for the odd pawn half of the connection-local PRI/pawn pool."""
    return (isinstance(channel, int) and
            REMOTE_PARTICIPANT_FIRST_CH <= channel <= REMOTE_PARTICIPANT_LAST_CH and
            (channel - REMOTE_PARTICIPANT_FIRST_CH) % 2 == 1)


def packet_starts_with_actor_rpc(packet, channel, handle, max_handle):
    """Return whether any non-open bunch starts with the requested RPC.

    This is intentionally a leading-handle probe, not a general UnrealScript
    payload decoder.  It is sufficient for detecting the server's reliable
    ChangedRole spawn-selection reopen without guessing at later parameters.
    """
    if not packet.get("ok"):
        return False
    for bunch in packet.get("bunches", []):
        if (bunch.get("chIndex") != channel or bunch.get("bOpen") or
                not bunch.get("payloadHex") or not bunch.get("bits")):
            continue
        reader = BitReader(bytes.fromhex(bunch["payloadHex"]), bunch["bits"])
        decoded_handle = reader.rint(max_handle)
        if not reader.error and decoded_handle == handle:
            return True
    return False

def parse_channel_set(value):
    try:
        channels = {int(piece, 10) for piece in value.split(",") if piece.strip()}
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "channels must be comma-separated integers") from exc
    if not channels or any(channel < 1 or channel >= 1024 for channel in channels):
        raise argparse.ArgumentTypeError(
            "channels must be a non-empty set of values from 1 through 1023")
    return channels


def parse_objective_mapping(value):
    try:
        mapping = [int(piece, 10) for piece in value.split(",")
                   if piece.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "objective mapping must be comma-separated integers") from exc
    if (not mapping or len(mapping) > 16 or
            any(index < 0 or index > 255 for index in mapping) or
            len(set(mapping)) != len(mapping)):
        raise argparse.ArgumentTypeError(
            "objective mapping must contain 1-16 unique byte values")
    return mapping


SPAWN_PROFILES = {
    # Every supported profile uses the per-session PC/GRI/TeamInfo/PRI cohort.
    # The populated Resort capture is available only through the explicit
    # RS2V_REPLAY_CAPTURE_WORLD=1 reverse-engineering diagnostic.
    "resort": ((0, 1, 2, 3, 4), 3, frozenset({2, 3, 4, 5, 26})),
    # Exact cooked ROObjective replication indices from
    # data/maps/VNTE-CuChi/objectives.txt, ordered by client slot 0..6.
    "cu-chi": ((2, 3, 4, 7, 5, 10, 9), 3,
                frozenset({2, 3, 4, 5, 26})),
    "hue-city": ((1, 2, 3, 4, 5, 6), 3, frozenset({2, 3, 4, 5, 26})),
    "compound": ((1, 2, 3), 3, frozenset({2, 3, 4, 5, 26})),
}

REPLICATION_ARTIFACT_VARIANTS = ("canonical", "installed")

# Live PC/GRI/TeamInfo/PRI actor opens are grounded for the full 4x2 matrix.
# h175 and the downstream pawn graph are intentionally narrower: every pair
# not listed in ROLE_SPAWN_SUPPORT must fail before a socket is opened.
ACTOR_BOOTSTRAP_SUPPORT = frozenset(
    (profile, artifact)
    for profile in SPAWN_PROFILES
    for artifact in REPLICATION_ARTIFACT_VARIANTS
)
ROLE_SPAWN_SUPPORT = frozenset({
    ("resort", "canonical"),
    ("cu-chi", "canonical"),
    ("compound", "installed"),
})


def validate_actor_bootstrap_support(profile, artifact_variant):
    """Require one of the eight grounded live actor-bootstrap combinations."""
    if profile not in SPAWN_PROFILES:
        raise ValueError(f"unknown spawn profile: {profile}")
    if artifact_variant not in REPLICATION_ARTIFACT_VARIANTS:
        raise ValueError(f"unknown replication artifact: {artifact_variant}")
    if (profile, artifact_variant) not in ACTOR_BOOTSTRAP_SUPPORT:
        raise ValueError(
            f"actor bootstrap is not grounded for {profile}/{artifact_variant}")


def validate_role_spawn_support(profile, artifact_variant):
    """Fail closed unless h175 and its pawn graph are grounded for this pair."""
    validate_actor_bootstrap_support(profile, artifact_variant)
    if (profile, artifact_variant) in ROLE_SPAWN_SUPPORT:
        return
    supported = ", ".join(
        f"{supported_profile}/{supported_artifact}"
        for supported_profile, supported_artifact in sorted(ROLE_SPAWN_SUPPORT)
    )
    raise ValueError(
        "role/spawn validation is not grounded for "
        f"{profile}/{artifact_variant}; supported pairs: {supported}")


OBJECTIVE_GRI_MAX_HANDLE = 184
OBJECTIVE_GRI_INT_ARRAY_HANDLES = frozenset({37, 38, 46})
OBJECTIVE_GRI_INT32_HANDLES = frozenset({25, 27, 28, 29, 39, 40, 41, 47, 48, 67})
OBJECTIVE_GRI_BOOL_HANDLES = frozenset({31, 32, 100, 114, 116, 117})
OBJECTIVE_GRI_BYTE_HANDLES = frozenset({126, 143})
OBJECTIVE_GRI_BYTE_ARRAY_HANDLES = frozenset({129, 174, 175, 176, 177, 178, 179})


def decode_objective_gri_payload(data, nbits):
    """Decode the objective-related fields authored on ROGameReplicationInfo.

    Unknown handles stop decoding without guessing their value layout. Callers
    must require ``complete`` before merging the result so a truncated or newer
    payload cannot make the spawn validator claim a partial baseline is valid.
    """
    empty = {
        "ok": False,
        "complete": False,
        "fields": {},
        "scalars": {},
        "unknown_handle": None,
    }
    if (not isinstance(data, (bytes, bytearray)) or
            isinstance(nbits, bool) or not isinstance(nbits, int) or
            nbits < 0 or nbits > len(data) * 8):
        return empty

    reader = BitReader(bytes(data), nbits)
    fields = {}
    scalars = {}
    unknown_handle = None

    def read_int32():
        raw = reader.ru(32)
        if raw & 0x80000000:
            return raw - 0x100000000
        return raw

    while reader.p < reader.n and not reader.error:
        handle = reader.rint(OBJECTIVE_GRI_MAX_HANDLE)
        if reader.error:
            break

        if handle in OBJECTIVE_GRI_INT_ARRAY_HANDLES:
            slot = reader.ru(8)
            value = read_int32()
            if not reader.error:
                fields[(handle, slot)] = value
        elif handle in OBJECTIVE_GRI_INT32_HANDLES:
            value = read_int32()
            if not reader.error:
                scalars[handle] = value
        elif handle in OBJECTIVE_GRI_BOOL_HANDLES:
            value = bool(reader.bit())
            if not reader.error:
                scalars[handle] = value
        elif handle == 124:
            slot = reader.ru(8)
            value = tuple(reader.ru(8) for _ in range(4))
            if not reader.error:
                fields[(handle, slot)] = value
        elif handle in OBJECTIVE_GRI_BYTE_HANDLES:
            value = reader.ru(8)
            if not reader.error:
                scalars[handle] = value
        elif handle in OBJECTIVE_GRI_BYTE_ARRAY_HANDLES:
            slot = reader.ru(8)
            value = reader.ru(8)
            if not reader.error:
                fields[(handle, slot)] = value
        else:
            unknown_handle = handle
            break

    ok = not reader.error
    complete = ok and unknown_handle is None and reader.p == reader.n
    return {
        "ok": ok,
        "complete": complete,
        "fields": fields,
        "scalars": scalars,
        "unknown_handle": unknown_handle,
    }


def resolve_spawn_profile(profile="resort", expected_objectives=None,
                          objective_mapping=None, gri_channel=None,
                          menu_channels=None, artifact_variant=None):
    """Resolve immutable profile defaults plus explicit CLI overrides."""
    if artifact_variant is None:
        if profile not in SPAWN_PROFILES:
            raise ValueError(f"unknown spawn profile: {profile}")
    else:
        validate_actor_bootstrap_support(profile, artifact_variant)
    profile_mapping, profile_gri, profile_menu = SPAWN_PROFILES[profile]

    if objective_mapping is not None:
        mapping = list(objective_mapping)
    elif expected_objectives is not None:
        if expected_objectives < 1 or expected_objectives > 16:
            raise ValueError("--expected-objectives must be between 1 and 16")
        mapping = list(range(expected_objectives))
    else:
        mapping = list(profile_mapping)

    if (not mapping or len(mapping) > 16 or
            any(index < 0 or index > 255 for index in mapping) or
            len(set(mapping)) != len(mapping)):
        raise ValueError(
            "objective mapping must contain 1-16 unique byte values")

    resolved_gri = profile_gri if gri_channel is None else gri_channel
    resolved_menu = set(profile_menu if menu_channels is None else menu_channels)
    if resolved_gri < 1 or resolved_gri >= 1024:
        raise ValueError("--gri-channel must be between 1 and 1023")
    if (not resolved_menu or
            any(channel < 1 or channel >= 1024 for channel in resolved_menu)):
        raise ValueError(
            "--menu-channels must contain values from 1 through 1023")
    if resolved_gri not in resolved_menu:
        raise ValueError("--gri-channel must be included in --menu-channels")
    return mapping, resolved_gri, resolved_menu


def spawn(host, port, deployment_wait, expected_objective_values,
          gri_channel, menu_bootstrap_channels, linger=0.0, team=1,
          profile="resort", artifact_variant="canonical"):
    validate_role_spawn_support(profile, artifact_variant)
    print(f"Role/spawn validation uses grounded pair "
          f"{profile}/{artifact_variant}.")
    team_contract = resolve_spawn_team(team)
    wire_contract = resolve_spawn_wire_contract(
        team, profile, artifact_variant)
    north_role_transition_bits, north_role_transition_hex = (
        resolve_north_role_transition_contract(profile, artifact_variant))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.5)
    SERVER_BD = 1500 * 8
    state = {"control_seq": 1, "actor_seq": 1, "pid": 0}
    seen_empty_keepalives = 0
    acked_server_packets = 0

    def receive_until(seconds, stop_predicate=None, quiet=False):
        nonlocal seen_empty_keepalives, acked_server_packets
        got = []
        empty_keepalives = 0
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                resp, _ = sock.recvfrom(4096)
                dec = decode_packet(resp, bd_max=SERVER_BD)
                if not quiet:
                    got.append(dec)
                if (dec.get("ok") and not dec.get("acks") and
                        not dec.get("bunches")):
                    empty_keepalives += 1
                # Retire every server packet that delivered bunch data. Pure ACK
                # and empty keepalive packets are intentionally not ACKed, matching
                # UE3 and avoiding an ACK-of-ACK ping-pong. Without these ACKs the
                # mock itself creates a reliable retransmission storm, so the server
                # never reaches the idle interval this gate is meant to validate.
                state["pid"], acknowledged = acknowledge_server_bunch_packet(
                    sock, (host, port), state["pid"], dec)
                if acknowledged:
                    acked_server_packets += 1
                if stop_predicate is not None and stop_predicate(dec):
                    break
            except socket.timeout:
                break
        sock.settimeout(1.5)
        seen_empty_keepalives += empty_keepalives
        if not quiet:
            for d in got:
                print(f"     <- {fmt_packet(d)}")
        return got

    def send_recv(name, bunch):
        dg = encode_packet(state["pid"], [bunch], 1280)
        sock.sendto(dg, (host, port))
        print(f"  -> {name}: pid={state['pid']} seq={bunch.get('chSeq')}")
        state["pid"] += 1
        # A healthy server now sends an empty UE3 transport packet every second.
        # Bound collection by elapsed time instead of waiting for socket silence,
        # otherwise that keepalive makes this validation harness wait forever.
        return receive_until(1.75)

    def hs(name, payload, want_open=False):
        b = {"bControl": 1 if want_open else 0, "bOpen": 1 if want_open else 0,
             "bClose": 0, "bReliable": 1, "chIndex": 0, "chType": 1,
             "chSeq": state["control_seq"], "payload": payload}
        got = send_recv(name, b)
        state["control_seq"] += 1
        return got

    def ch2_rpc(name, bits_payload):
        b = {"bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
             "chIndex": 2, "chType": 2, "chSeq": state["actor_seq"],
             "bits_payload": bits_payload}
        got = send_recv(name, b)
        state["actor_seq"] += 1
        return got

    def actor_rpc(name, channel, sequence, bits_payload):
        b = {"bControl": 0, "bOpen": 0, "bClose": 0, "bReliable": 1,
             "chIndex": channel, "chType": 2, "chSeq": sequence,
             "bits_payload": bits_payload}
        return send_recv(name, b)

    print(f"spawn: drive {team_contract.label} menu->spawn against {host}:{port}\n")
    # 1-4. Handshake -> Join (same sequence react validates).
    hs("HandshakeStart 0x1d", bytes([0x1d, 0x01]), want_open=True)
    hs("HandshakeResponse 0x1f", bytes([0x1f, 0x00, 0x00, 0x00, 0x00]))
    hs("SteamLogin 0x10", bytes([0x10, 0x00, 0x00, 0x00]))
    join_got = hs("Join 0x09", bytes([0x09]))
    boot = sorted({b2["chIndex"] for d in join_got for b2 in d.get("bunches", [])
                   if b2["chIndex"] >= 2 and b2["bOpen"]})
    print(f"     bootstrap opened {len(boot)} actor channels (ch{boot[0] if boot else '-'}..{boot[-1] if boot else '-'})\n")
    opened_bootstrap_channels = set(boot)
    missing_menu_chans = sorted(
        menu_bootstrap_channels - opened_bootstrap_channels)
    unexpected_bootstrap_chans = sorted(
        opened_bootstrap_channels - menu_bootstrap_channels)

    # The retail objective HUD is populated by ROGameReplicationInfo array
    # properties (live GRI ch3), not the emulator's
    # legacy OBJECTIVE_UPDATE packet. Decode the reliable post-open baseline and
    # require the requested identity slot map.
    objective_fields = {}
    objective_overview_enabled = False
    match_has_begun = None
    stop_countdown = None
    objective_payloads_complete = True
    for d in join_got:
        for b2 in d.get("bunches", []):
            if (b2["chIndex"] != gri_channel or b2["bOpen"] or
                    not b2["payloadHex"]):
                continue
            decoded = decode_objective_gri_payload(
                bytes.fromhex(b2["payloadHex"]), b2["bits"])
            if not decoded["complete"]:
                objective_payloads_complete = False
                continue
            objective_fields.update(decoded["fields"])
            scalars = decoded["scalars"]
            if 31 in scalars:
                match_has_begun = scalars[31]
            if 32 in scalars:
                stop_countdown = scalars[32]
            if 100 in scalars:
                objective_overview_enabled = not scalars[100]
    expected_objective_mapping = {
        slot: value for slot, value in enumerate(expected_objective_values)
    }
    objective_mapping = {
        slot: objective_fields.get((179, slot))
        for slot in range(len(expected_objective_values))
    }
    has_objective_baseline = (objective_payloads_complete and
                              objective_overview_enabled and
                              objective_mapping == expected_objective_mapping and all(
        (178, slot) in objective_fields
        for slot in range(len(expected_objective_values))))
    print(f"     objective GRI baseline ch{gri_channel}: mapping={objective_mapping}, "
          f"overview={'enabled' if objective_overview_enabled else 'DISABLED'}, "
          f"status={'yes' if has_objective_baseline else 'NO'}, "
          f"active-match={'yes' if match_has_begun is True and stop_countdown is False else 'not-published'}\n")

    # 5. SelectTeam(byte TeamID): SerializeInt(170,531) + Send presence bit(1) + TeamID byte.
    team_bits = build_team_selection_bits(team_contract.team_id)
    team_got = ch2_rpc(
        f"SelectTeam(170, retail={team_contract.retail_team_id}, "
        f"server={team_contract.team_id})", team_bits)

    # 6. Pick a role and request deployment. This is a retail-captured,
    # semantically complete h175 payload whose final optional bCloseMenu bit is
    # true. A bare handle is not a valid SelectRoleByClass call and must not
    # authorize an early spawn. Resort preserves the frame-2533 h175+h451
    # request; Compound uses its installed live profile's exact role object.
    final_role_payload = build_role_selection_bits(
        team_contract.team_id, profile, artifact_variant)
    role_got = ch2_rpc(
        f"SelectRoleByClass(175,{team_contract.label},bCloseMenu=true)",
        final_role_payload)

    # 7. Select the first normal spawn-list slot. The UI encodes normal TeamInfo
    # array slots as 128+slot, and RPC byte parameters carry a presence bit.
    spawn_bits = (sint_bits(SET_SPAWN, ROPC_MAXHANDLE) + [1] +
                  [(128 >> i) & 1 for i in range(8)])
    select_got = ch2_rpc("ServerSetSpawnSelect(261, slot=0)", spawn_bits)

    # 8. Ready is enum value zero, so UE3 emits only the false parameter-presence
    # bit and the UnrealScript default supplies Ready. Once the round is active
    # (or the preparation countdown reaches its final deployment window), this
    # authorizes the selected slot exactly once.
    ready_bits = sint_bits(SET_READY, ROPC_MAXHANDLE) + [0]
    ready_got = ch2_rpc("ServerSetReadyToSpawn(434, Ready)", ready_bits)

    # A fresh Territories server intentionally queues Ready until the stock
    # preparation clock enters its final eight-second window. Keep the mock
    # connected and ACKing server traffic so that the asynchronous pawn/loadout
    # transition can be validated instead of reporting a false failure merely
    # because the test reached h434 before the deployment threshold.
    def packet_opens_local_pawn(packet):
        return any(b2.get("chIndex") == LOCAL_PAWN_CH and b2.get("bOpen")
                   for b2 in packet.get("bunches", []))

    def packet_reopens_spawn_selection(packet):
        return packet_starts_with_actor_rpc(
            packet, 2, 210, ROPC_MAXHANDLE)  # ChangedRole(..., showSpawnSelect=true)

    deferred_got = []
    latest_deployment_got = ready_got
    deployment_deadline = time.monotonic() + deployment_wait
    reselection_attempts = 0
    announced_wait = False
    while not any(packet_opens_local_pawn(d) for d in latest_deployment_got):
        # Territory can advance after h261 but before h434.  The server then
        # reopens the retail selection scene with a fresh, phase-correct list.
        # Follow that explicit ChangedRole transition instead of timing out on
        # the stale slot; bound retries so a broken server cannot spin the tool.
        if any(packet_reopens_spawn_selection(d)
               for d in latest_deployment_got):
            if reselection_attempts >= 3:
                print("  !! spawn selection reopened more than three times; stopping retries")
                break
            reselection_attempts += 1
            print(f"  .. spawn list changed; reselecting slot 0 "
                  f"(attempt {reselection_attempts}/3)")
            retry_select = ch2_rpc(
                "ServerSetSpawnSelect(261, slot=0, refreshed)", spawn_bits)
            retry_ready = ch2_rpc(
                "ServerSetReadyToSpawn(434, Ready, refreshed)", ready_bits)
            deferred_got += retry_select + retry_ready
            latest_deployment_got = retry_select + retry_ready
            continue

        remaining = deployment_deadline - time.monotonic()
        if remaining <= 0.0:
            break
        if not announced_wait:
            print(f"  .. awaiting preparation deployment window "
                  f"(up to {deployment_wait:.1f}s)")
            announced_wait = True
        latest_deployment_got = receive_until(
            remaining,
            lambda packet: (packet_opens_local_pawn(packet) or
                            packet_reopens_spawn_selection(packet)))
        deferred_got += latest_deployment_got
        if not latest_deployment_got:
            break

    if any(packet_opens_local_pawn(d)
           for d in ready_got + deferred_got):
        # Drain the rest of the burst; pawn, inventory and camera state can
        # span several datagrams after the first pawn-channel open.
        deferred_got += receive_until(1.0)

    # 9. Exercise UE3's recovery request too. A correct server answers AskForPawn with
    # GivePawn(NewPawn), using the same presence bit + dynamic actor ref as ClientRestart.
    ask_got = ch2_rpc("AskForPawn(42)", sint_bits(42, ROPC_MAXHANDLE))

    # Confirm the weapon selected by ClientSwitchToBestWeapon. This is the
    # capture-pinned ROInventoryManager h25 shape from f27397. Then clear it on
    # the next reliable manager sequence so the mock verifies both h147 forms.
    # Never target an unopened actor channel after a failed deployment: the
    # server correctly rejects it, but the resulting warning obscures the actual
    # role/spawn failure this harness is meant to report.
    pawn_graph_opened = any(
        packet_opens_local_pawn(packet)
        for packet in ready_got + deferred_got + ask_got)
    weapon_select_got = []
    weapon_clear_got = []
    if pawn_graph_opened:
        select_weapon_bits = (sint_bits(25, 34) + [1, 1] +
                              sint_bits(210, 1024))
        weapon_select_got = actor_rpc(
            "ServerSetCurrentWeapon(219,h25,ch210)", 219, 1,
            select_weapon_bits)
        weapon_clear_got = actor_rpc(
            "ServerSetCurrentWeapon(219,h25,None)", 219, 2,
            sint_bits(25, 34) + [0])
    else:
        print("  .. pawn graph did not open; skipping ch219 weapon RPC probes")

    # The owning pawn graph starts at the fixed local pawn channel (kPawnCh=209).
    after = (team_got + role_got + select_got + ready_got + deferred_got + ask_got +
             weapon_select_got + weapon_clear_got)
    pawn_opens = sorted({b2["chIndex"] for d in after for b2 in d.get("bunches", [])
                         if b2["chIndex"] >= LOCAL_PAWN_CH and b2["bOpen"]})
    # Participant actor pairs are allocated as PRI=512+2N, pawn=513+2N. PRI
    # opens are independently grounded and expected; odd pawn channels must
    # remain absent until the full role/class templates are capture-backed.
    remote_pawn_opens = [
        channel for channel in pawn_opens
        if is_remote_participant_pawn_channel(channel)
    ]
    expected_owning_channels = {LOCAL_PAWN_CH} | set(
        wire_contract.loadout_class_map)
    owning_graph_opens = {
        channel for channel in pawn_opens if LOCAL_PAWN_CH <= channel <= 219
    }
    unexpected_owning_opens = sorted(
        owning_graph_opens - expected_owning_channels)
    # Count standalone ack-only datagrams (no bunches) - should be coalesced (few), not a storm.
    ack_only = sum(1 for d in after if d.get("ok") and not d.get("bunches") and d.get("acks"))
    bunch_pkts = sum(1 for d in after if d.get("bunches"))

    def fields_on(channel, max_handle):
        for d in after:
            for b2 in d.get("bunches", []):
                if b2["chIndex"] != channel or b2["bOpen"] or not b2["payloadHex"]:
                    continue
                br = BitReader(bytes.fromhex(b2["payloadHex"]), b2["bits"])
                handle = br.rint(max_handle)
                yield handle, br, b2

    def read_dynamic_ref(br):
        dynamic = bool(br.bit())
        index = br.rint(1024 if dynamic else 0x80000000)
        return dynamic, index

    expected_loadout_classes = wire_contract.loadout_class_map
    loadout_open_classes = {}
    pawn_open_class = None
    for d in after:
        for b2 in d.get("bunches", []):
            if (not b2["bOpen"] or
                    b2["chIndex"] not in
                    (set(expected_loadout_classes) | {LOCAL_PAWN_CH}) or
                    not b2["payloadHex"]):
                continue
            br = BitReader(bytes.fromhex(b2["payloadHex"]), b2["bits"])
            dynamic, class_ref = read_dynamic_ref(br)
            if not br.error and not dynamic:
                if b2["chIndex"] == LOCAL_PAWN_CH:
                    pawn_open_class = class_ref
                else:
                    loadout_open_classes[b2["chIndex"]] = class_ref
    has_exact_pawn_class = pawn_open_class == wire_contract.pawn_class_ref
    has_exact_loadout_classes = loadout_open_classes == expected_loadout_classes

    # Decode the actual values, not just each payload's first byte. The old gate gave
    # a false PASS to fixed-width literals whose handle looked right but whose presence/
    # selector bit decoded NewPawn as None or a bogus static object.
    has_pc_pawn = False
    has_client_restart = False
    has_give_pawn = False
    has_client_on_possess = False
    has_changed_role = False
    has_view_target = False
    has_controller_view_target = False
    has_camera_reset = False
    has_exact_camera_tail = False
    has_first_person = False
    has_zero_spawn_penalty = False
    has_alive_state = False
    has_switch_best_weapon = False
    switch_best_weapon_count = 0
    saw_hud_h392 = False
    has_north_rotation = False
    has_north_collide_world_false = False
    has_north_respawn_sentinel = False
    has_north_spawned_rpc = False
    has_north_hide_round_start_rpc = False
    has_north_cinematic_tail = False
    has_exact_north_role_transition = any(
        b2["chIndex"] == 2 and not b2["bOpen"] and
        b2.get("bReliable") and
        b2["bits"] == north_role_transition_bits and
        b2["payloadHex"].lower() == north_role_transition_hex
        for d in after for b2 in d.get("bunches", [])
        if b2.get("payloadHex"))
    has_exact_north_post_delta = any(
        b2["chIndex"] == 2 and not b2["bOpen"] and
        not b2.get("bReliable") and b2["bits"] == 72 and
        b2["payloadHex"].lower() == "11c0301a9e7f969800"
        for d in after for b2 in d.get("bunches", [])
        if b2.get("payloadHex"))
    # Walk every field in each ch2 bunch. Retail combines h85/h87/h61/h265/h150/h151
    # into ONE reliable payload, so checking only byte zero/first handle would miss most
    # of the transition and recreate the old false-green spawn gate.
    for d in after:
        for b2 in d.get("bunches", []):
            if b2["chIndex"] != 2 or b2["bOpen"] or not b2["payloadHex"]:
                continue
            has_exact_camera_tail |= (
                bool(b2.get("bReliable")) and
                b2["bits"] == team_contract.final_tail_bits and
                b2["payloadHex"].lower() == team_contract.final_tail_hex)
            br = BitReader(bytes.fromhex(b2["payloadHex"]), b2["bits"])
            while br.p < br.n and not br.error:
                handle = br.rint(ROPC_MAXHANDLE)
                if handle == 24:
                    dyn, idx = read_dynamic_ref(br)
                    has_pc_pawn |= dyn and idx == LOCAL_PAWN_CH and not br.error
                elif handle == 28:  # ClientSwitchToBestWeapon(optional bool)
                    valid_switch = br.bit() == 0 and not br.error
                    has_switch_best_weapon |= valid_switch
                    switch_best_weapon_count += int(valid_switch)
                elif handle in (43, 85, 150):
                    present = bool(br.bit())
                    dyn, idx = read_dynamic_ref(br) if present else (False, 0)
                    valid = present and dyn and idx == LOCAL_PAWN_CH and not br.error
                    has_give_pawn |= handle == 43 and valid
                    has_client_restart |= handle == 85 and valid
                    has_client_on_possess |= handle == 150 and valid
                elif handle == 87:  # ClientSetViewTarget(Pawn, TransitionParams)
                    present = bool(br.bit())
                    dyn, idx = read_dynamic_ref(br) if present else (False, 0)
                    transition_present = bool(br.bit())
                    if transition_present:
                        blend_time = br.ru(32)
                        blend_function = br.rint(6)
                        blend_exp = br.ru(32)
                        lock_outgoing = br.bit()
                        valid_transition = (present and dyn and blend_time == 0 and
                                            blend_function == 1 and
                                            blend_exp == 0x40000000 and
                                            lock_outgoing == 0 and not br.error)
                        has_view_target |= valid_transition and idx == LOCAL_PAWN_CH
                        has_controller_view_target |= valid_transition and idx == 2
                elif handle == 61:  # ClientSetCameraMode(FName)
                    present = bool(br.bit())
                    hardcoded = bool(br.bit()) if present else False
                    name = ""
                    number = -1
                    if present and not hardcoded:
                        length = br.ru(32)
                        raw = bytes(br.ru(8) for _ in range(length)) if length <= 256 else b""
                        number = br.ru(32)
                        name = raw.rstrip(b"\x00").decode("ascii", errors="replace")
                    has_first_person |= (present and not hardcoded and name == "FirstPerson" and
                                         number == 0 and not br.error)
                elif handle == 265:  # SetHUDSpawnPenalty(int); zero is omitted/default
                    present = bool(br.bit())
                    penalty = br.ru(32) if present else 0
                    has_zero_spawn_penalty |= penalty == 0 and not br.error
                elif handle == 151:  # ClientOnDead(bool), no presence bit for bool params
                    has_alive_state |= br.bit() == 0 and not br.error
                elif handle == 17:  # Actor.bCollideWorld property
                    has_north_collide_world_false |= br.bit() == 0 and not br.error
                elif handle == 26:  # ClientSetRotation(rotator, bResetCamera)
                    present = bool(br.bit())
                    if present:
                        pitch = br.ru(8) << 8 if br.bit() else 0
                        yaw = br.ru(8) << 8 if br.bit() else 0
                        roll = br.ru(8) << 8 if br.bit() else 0
                    else:
                        pitch = yaw = roll = 0
                    reset_camera = bool(br.bit())
                    has_north_rotation |= (present and pitch == 0 and yaw == 40960 and
                                           roll == 0 and reset_camera and not br.error)
                elif handle == 168:  # ClientCameraReset(), no params
                    has_camera_reset = not br.error
                elif handle == 226:  # ClientHideRoundStartScreen(), no params
                    has_north_hide_round_start_rpc = not br.error
                elif handle == 316:  # NextRespawnTime property
                    raw = br.ru(32)
                    respawn_time = raw - 0x100000000 if raw & 0x80000000 else raw
                    has_north_respawn_sentinel |= respawn_time == 9999999 and not br.error
                elif handle == 390:  # ClientSpawned(), no params
                    has_north_spawned_rpc = not br.error
                elif handle == 392:  # ShowInitialWorldWidget(), no params
                    saw_hud_h392 = not br.error
                elif handle == 101:  # ClientSetCinematicMode(bool,bool,bool,bool)
                    cinematic = bool(br.bit())
                    movement = bool(br.bit())
                    turning = bool(br.bit())
                    hud = bool(br.bit())
                    has_north_cinematic_tail |= (
                        not cinematic and movement and not turning and not hud and
                        not br.error)
                elif handle == 210:  # ChangedRole(byte, byte, bool, bool)
                    squad_present = bool(br.bit())
                    squad = br.ru(8) if squad_present else 0
                    class_present = bool(br.bit())
                    class_index = br.ru(8) if class_present else 0
                    show_lobby = bool(br.bit())
                    show_spawn = bool(br.bit())
                    has_changed_role |= (squad == 255 and class_index == 0 and
                                         not show_lobby and show_spawn and not br.error)
                else:
                    # This gate only authors/understands the spawn transition fields.
                    # Stop this bunch rather than guessing an unknown parameter layout.
                    break

    has_pawn_controller = False
    has_pawn_pri = False
    has_pawn_inv_manager = False
    has_client_possessed = False
    for handle, br, _ in fields_on(LOCAL_PAWN_CH, 168):
        has_client_possessed |= handle == 57 and not br.error
        if handle in (27, 32, 52):
            dyn, idx = read_dynamic_ref(br)
            has_pawn_inv_manager |= handle == 27 and dyn and idx == 219 and not br.error
            has_pawn_pri |= handle == 32 and dyn and idx == 26 and not br.error
            has_pawn_controller |= handle == 52 and dyn and idx == 2 and not br.error

    has_attachment_list = any(
        b2["chIndex"] == LOCAL_PAWN_CH and not b2["bOpen"] and
        not b2.get("bReliable") and
        b2["bits"] == wire_contract.attachment_payload_bits and
        b2["payloadHex"].lower() == wire_contract.attachment_payload_hex
        for d in after for b2 in d.get("bunches", [])
        if b2.get("payloadHex"))
    current_attachment_refs = set()
    has_exact_current_attachment = False
    for handle, br, b2 in fields_on(LOCAL_PAWN_CH, 168):
        if handle != 147:
            continue
        dynamic, attachment_ref = read_dynamic_ref(br)
        if not br.error:
            current_attachment_refs.add((dynamic, attachment_ref))
        has_exact_current_attachment |= (
            not b2.get("bReliable") and
            b2["bits"] == wire_contract.current_attachment_payload_bits and
            b2["payloadHex"].lower() ==
            wire_contract.current_attachment_payload_hex)
    has_attachment_clear = (True, 0) in current_attachment_refs

    expected_weapon_next = team_contract.weapon_next_map
    weapon_max_handle = team_contract.weapon_max_handle_map
    linked_weapons = set()
    for weapon_ch, next_ch in expected_weapon_next.items():
        max_handle = weapon_max_handle[weapon_ch]
        for handle, br, _ in fields_on(weapon_ch, max_handle):
            if handle != 23:
                continue
            dyn_manager, manager_ch = read_dynamic_ref(br)
            dyn_next, decoded_next = True, 0
            next_handle = br.rint(max_handle)
            if next_ch != 0:
                if br.error or next_handle != 24:
                    continue
                dyn_next, decoded_next = read_dynamic_ref(br)
                next_handle = br.rint(max_handle)
            if br.error or next_handle != 25 or not br.bit():
                continue
            dyn_owner, owner_ch = read_dynamic_ref(br)
            do_not_activate = br.bit()
            if (not br.error and dyn_manager and manager_ch == 219 and dyn_next and
                    decoded_next == next_ch and dyn_owner and owner_ch == LOCAL_PAWN_CH and
                    do_not_activate == 0):
                linked_weapons.add(weapon_ch)
            break

    print()
    print(f"     pawn channel opens (ch{LOCAL_PAWN_CH}+) after role-select: {pawn_opens}")
    print(f"     ungrounded remote pawn opens: {remote_pawn_opens or 'none'}")
    print(f"     owning graph: {team_contract.label} pawn class="
          f"{pawn_open_class if pawn_open_class is not None else 'missing'} "
          f"({'exact' if has_exact_pawn_class else 'WRONG'}), "
          f"unexpected stable channels={unexpected_owning_opens or 'none'}")
    print(f"     pawn back-refs: Controller(h52)->ch2={'yes' if has_pawn_controller else 'NO'}, "
          f"PRI(h32)->ch26={'yes' if has_pawn_pri else 'NO'}, "
          f"InvManager(h27)->ch219={'yes' if has_pawn_inv_manager else 'NO'}, "
          f"ClientPossessed(h57)={'yes' if has_client_possessed else 'NO'}")
    print(f"     possession fields: PC.Pawn(h24)->ch{LOCAL_PAWN_CH}={'yes' if has_pc_pawn else 'NO'}, "
          f"ClientRestart(h85)={'yes' if has_client_restart else 'NO'}, "
          f"ClientOnPossess(h150)={'yes' if has_client_on_possess else 'NO'}, "
          f"GivePawn(h43 recovery)={'yes' if has_give_pawn else 'NO'}")
    print(f"     UI/camera transition: ChangedRole(h210)={'yes' if has_changed_role else 'NO'}, "
          f"InitialViewTarget(h87)->pawn={'yes' if has_view_target else 'NO'}, "
          f"FactionTail({team_contract.final_tail_bits}b)={'yes' if has_exact_camera_tail else 'NO'}, "
          f"FirstPerson(h61)={'yes' if has_first_person else 'NO'}, "
          f"SpawnPenalty0(h265)={'yes' if has_zero_spawn_penalty else 'NO'}, "
          f"Alive(h151)={'yes' if has_alive_state else 'NO'}, "
          f"ForbiddenHUD(h392)={'PRESENT' if saw_hud_h392 else 'absent'}")
    loadout_channels = set(expected_loadout_classes)
    loadout_opens = sorted(set(pawn_opens) & loadout_channels)
    print(f"     loadout actors: {loadout_opens}; "
          f"classes={'exact' if has_exact_loadout_classes else loadout_open_classes}; "
          f"linked={sorted(linked_weapons)}; "
          f"ClientSwitchToBestWeapon(h28)={switch_best_weapon_count}x")
    print(f"     attachment state: h167-list={'yes' if has_attachment_list else 'NO'}, "
          f"h147-current({wire_contract.current_attachment_class_ref})="
          f"{'yes' if has_exact_current_attachment else 'NO'}, "
          f"h147-clear={'yes' if has_attachment_clear else 'NO'}")
    if team_contract.north_graph:
        print(f"     North capture state: h210+h211="
              f"{'exact' if has_exact_north_role_transition else 'NO'}, "
              f"rotation={'exact' if has_north_rotation else 'NO'}, "
              f"post-delta={'exact' if has_exact_north_post_delta else 'NO'}, "
              f"spawn/hide/cinematic="
              f"{'yes' if (has_north_spawned_rpc and has_north_hide_round_start_rpc and has_north_cinematic_tail) else 'NO'}")
    print(f"     server data packets: {bunch_pkts}; standalone ack-only datagrams: {ack_only}")
    print(f"     server bunch packets acknowledged by mock: {acked_server_packets}")
    print(f"     empty UE3 transport keepalives: {seen_empty_keepalives}")
    camera_semantics_ok = (
        (has_north_spawned_rpc and has_north_hide_round_start_rpc and
         has_north_cinematic_tail)
        if team_contract.north_graph
        else (has_controller_view_target and has_camera_reset))
    north_capture_ok = (
        not team_contract.north_graph or
        (has_exact_north_role_transition and has_north_rotation and
         has_exact_north_post_delta and has_north_collide_world_false and
         has_north_respawn_sentinel))
    ok = (not missing_menu_chans and not unexpected_bootstrap_chans and
          not remote_pawn_opens and
          not unexpected_owning_opens and
          has_objective_baseline and
          LOCAL_PAWN_CH in pawn_opens and
          has_exact_pawn_class and
          has_pawn_controller and has_pawn_pri and has_pawn_inv_manager and
          has_client_possessed and has_pc_pawn and has_client_restart and
          has_client_on_possess and has_give_pawn and has_changed_role and
          has_view_target and camera_semantics_ok and north_capture_ok and
          has_exact_camera_tail and has_first_person and has_zero_spawn_penalty and
          has_alive_state and not saw_hud_h392 and
          loadout_channels.issubset(set(pawn_opens)) and
          has_exact_loadout_classes and
          set(expected_weapon_next).issubset(linked_weapons) and
          has_attachment_list and has_exact_current_attachment and
          has_attachment_clear and has_switch_best_weapon and
          switch_best_weapon_count >= team_contract.minimum_switch_best_weapon_count and
          seen_empty_keepalives >= 1)
    if ok:
        print(f"\n=== spawn: PASS - {team_contract.label} pawn refs, loadout graph, "
              f"ClientRestart, and GivePawn recovery decode to ch{LOCAL_PAWN_CH} ===")
    else:
        miss = []
        if missing_menu_chans:
            miss.append(
                f"missing live menu bootstrap channels: {missing_menu_chans}")
        if unexpected_bootstrap_chans:
            miss.append(
                "unexpected actors opened at live bootstrap: "
                f"{unexpected_bootstrap_chans}")
        if remote_pawn_opens:
            miss.append(f"ungrounded remote pawn actors opened: {remote_pawn_opens}")
        if unexpected_owning_opens:
            miss.append(f"unexpected faction graph channels: {unexpected_owning_opens}")
        if not has_objective_baseline:
            miss.append(
                f"missing ch{gri_channel} objective mapping/status baseline")
        if LOCAL_PAWN_CH not in pawn_opens: miss.append(f"no pawn open on ch{LOCAL_PAWN_CH}")
        if not has_exact_pawn_class:
            miss.append(f"pawn class differs: {pawn_open_class} != {wire_contract.pawn_class_ref}")
        if not has_pawn_controller: miss.append("Pawn.Controller is not dynamic ch2")
        if not has_pawn_pri: miss.append("Pawn.PRI is not dynamic ch26")
        if not has_pawn_inv_manager: miss.append("Pawn.InvManager is not dynamic ch219")
        if not has_client_possessed: miss.append("no pawn ClientPossessed(h57)")
        if not has_pc_pawn: miss.append(f"PC.Pawn is not dynamic ch{LOCAL_PAWN_CH}")
        if not has_client_restart: miss.append("ClientRestart NewPawn is absent/wrong")
        if not has_client_on_possess: miss.append("ClientOnPossess Pawn is absent/wrong")
        if not has_give_pawn: miss.append("AskForPawn recovery did not send valid GivePawn")
        if not has_changed_role: miss.append("ChangedRole did not close role/unit select")
        if not has_view_target: miss.append("initial ClientSetViewTarget is absent/wrong")
        if not team_contract.north_graph and not has_controller_view_target:
            miss.append("final ClientSetViewTarget is not dynamic ch2")
        if not team_contract.north_graph and not has_camera_reset:
            miss.append("final ClientCameraReset(h168) is absent")
        if not has_exact_camera_tail:
            miss.append(f"final {team_contract.final_tail_bits}-bit faction tail differs")
        if team_contract.north_graph and not has_exact_north_role_transition:
            miss.append("North h210+h211 role transition differs")
        if team_contract.north_graph and not has_north_rotation:
            miss.append("North h26 ClientSetRotation differs")
        if team_contract.north_graph and not has_exact_north_post_delta:
            miss.append("North 72-bit h17/h24/h316 post-open delta differs")
        if team_contract.north_graph and not has_north_collide_world_false:
            miss.append("North h17 bCollideWorld=false absent")
        if team_contract.north_graph and not has_north_respawn_sentinel:
            miss.append("North h316 NextRespawnTime sentinel absent")
        if team_contract.north_graph and not has_north_spawned_rpc:
            miss.append("North ClientSpawned(h390) absent")
        if team_contract.north_graph and not has_north_hide_round_start_rpc:
            miss.append("North ClientHideRoundStartScreen(h226) absent")
        if team_contract.north_graph and not has_north_cinematic_tail:
            miss.append("North ClientSetCinematicMode(h101) differs")
        if not has_first_person: miss.append("ClientSetCameraMode is not FirstPerson")
        if not has_zero_spawn_penalty: miss.append("SetHUDSpawnPenalty is not zero")
        if not has_alive_state: miss.append("ClientOnDead did not set alive=false")
        if saw_hud_h392: miss.append("forbidden ShowInitialWorldWidget(h392) was authored")
        missing_loadout = sorted(loadout_channels - set(pawn_opens))
        if missing_loadout: miss.append(f"missing loadout actor opens: {missing_loadout}")
        if not has_exact_loadout_classes:
            miss.append(f"loadout class refs differ: {loadout_open_classes}")
        missing_graph = sorted(set(expected_weapon_next) - linked_weapons)
        if missing_graph: miss.append(f"unlinked weapon channels: {missing_graph}")
        if not has_attachment_list:
            miss.append(f"missing exact {len(wire_contract.attachments)}-record "
                        "pawn h167 attachment state")
        if not has_exact_current_attachment:
            miss.append("missing exact faction-primary pawn h147 current attachment")
        if not has_attachment_clear: miss.append("missing pawn h147 dynamic-None clear")
        if switch_best_weapon_count < team_contract.minimum_switch_best_weapon_count:
            miss.append(f"ClientSwitchToBestWeapon(false) repeated only "
                        f"{switch_best_weapon_count}x")
        if seen_empty_keepalives < 1: miss.append("no empty UE3 transport keepalive")
        print(f"\n=== spawn: FAIL - {'; '.join(miss)} (check team->role advance / pawn-spawn) ===")
    # Keep the fully joined session alive for live-server observation when
    # requested.  The quiet receive loop still retires every data-bearing server
    # PacketId, so linger never creates the reliable retry storm this harness is
    # meant to detect.
    if boot and linger > 0.0:
        acked_before_linger = acked_server_packets
        print(f"\n     lingering for {linger:g}s (quietly ACKing server bunch packets)")
        receive_until(linger, quiet=True)
        print(f"     linger complete; acknowledged "
              f"{acked_server_packets - acked_before_linger} server bunch packets")

    # A joined mock must leave like a UE3 peer, not merely abandon its UDP
    # socket.  The server intentionally retires this packet without replying;
    # retransmitted duplicates therefore cannot recreate a phantom session.
    close_pid = state["pid"]
    close_seq = state["control_seq"]
    state["pid"], state["control_seq"] = send_graceful_control_close(
        sock, (host, port), close_pid, close_seq)
    print(f"     -> graceful control close: pid={close_pid} seq={close_seq}")

    sock.close()
    return 0 if ok else 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["replay", "drive", "react", "spawn", "reconnect"])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument(
        "--profile", choices=tuple(SPAWN_PROFILES), default="resort",
        help="map profile used by actor and role/spawn validation")
    ap.add_argument(
        "--artifact", choices=REPLICATION_ARTIFACT_VARIANTS,
        default="canonical",
        help=("replication artifact selected by the server (default: canonical; "
              "must match RS2V_REPLICATION_BOOTSTRAP_VARIANT)"))
    ap.add_argument(
        "--team", type=int, choices=(1, 2), default=1,
        help="spawn faction: 1=South/US (default), 2=North/NVA")
    ap.add_argument("--deployment-wait", type=float, default=35.0,
                    help="seconds spawn mode waits for the preparation deployment window")
    ap.add_argument(
        "--linger", type=float, default=0.0,
        help="seconds spawn mode stays connected and quietly ACKs before close (0-600)")
    objective_expectation = ap.add_mutually_exclusive_group()
    objective_expectation.add_argument(
        "--expected-objectives", type=int,
        help="number of identity-mapped objective slots expected in spawn mode")
    objective_expectation.add_argument(
        "--objective-mapping", type=parse_objective_mapping,
        help="comma-separated cooked objective indices expected for slots 0..N")
    ap.add_argument("--gri-channel", type=int,
                    help="map profile's GameReplicationInfo actor channel")
    ap.add_argument("--menu-channels", type=parse_channel_set,
                    help="comma-separated actor channels expected during menu bootstrap")
    args = ap.parse_args()
    if (not math.isfinite(args.deployment_wait) or
            args.deployment_wait < 0.0 or args.deployment_wait > 120.0):
        ap.error("--deployment-wait must be finite and between 0 and 120 seconds")
    if (not math.isfinite(args.linger) or
            args.linger < 0.0 or args.linger > 600.0):
        ap.error("--linger must be finite and between 0 and 600 seconds")
    try:
        expected_objective_values, gri_channel, menu_channels = resolve_spawn_profile(
            args.profile, args.expected_objectives, args.objective_mapping,
            args.gri_channel, args.menu_channels, args.artifact)
    except ValueError as exc:
        ap.error(str(exc))
    if args.mode == "replay":
        return replay(args.host, args.port)
    if args.mode == "react":
        return react(args.host, args.port)
    if args.mode == "spawn":
        try:
            validate_role_spawn_support(args.profile, args.artifact)
        except ValueError as exc:
            ap.error(str(exc))
        return spawn(args.host, args.port, args.deployment_wait,
                     expected_objective_values, gri_channel, menu_channels,
                     args.linger, args.team, args.profile, args.artifact)
    if args.mode == "reconnect":
        return reconnect(args.host, args.port)
    return drive(args.host, args.port)

if __name__ == "__main__":
    sys.exit(main())
