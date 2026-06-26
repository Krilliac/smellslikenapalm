#!/usr/bin/env python3
"""
pktlog_decode.py — read a PacketRecorder sniff (packetlog/session_*.jsonl) and print
a human-readable C2S/S2C transcript, decoding each datagram's UE3 bunches via the
mock_client decoder. The packetlog analog of a WoW packet-parser.

Usage:
  python tools/pktlog_decode.py                      # latest session, all datagrams
  python tools/pktlog_decode.py <file.jsonl>         # a specific sniff
  python tools/pktlog_decode.py --dir C2S            # only client->server
  python tools/pktlog_decode.py --ch 2               # only datagrams touching channel 2
  python tools/pktlog_decode.py --grep ChangedTeams  # raw substring filter on the decode
  python tools/pktlog_decode.py --nulls              # show the matching nulls_*.jsonl instead

Each datagram line:  [seq] DIR client peer  len=N  pid=.. <bunch summary>
"""
import sys, os, json, glob

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
try:
    from mock_client import decode_packet, fmt_packet
except Exception as e:
    print(f"could not import mock_client decoder: {e}", file=sys.stderr)
    decode_packet = fmt_packet = None


def latest(pattern):
    files = sorted(glob.glob(pattern), key=os.path.getmtime)
    return files[-1] if files else None


def main():
    args = sys.argv[1:]
    want_dir = None
    want_ch = None
    grep = None
    show_nulls = False
    path = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--dir":     want_dir = args[i + 1].upper(); i += 2; continue
        if a == "--ch":      want_ch = int(args[i + 1]); i += 2; continue
        if a == "--grep":    grep = args[i + 1]; i += 2; continue
        if a == "--nulls":   show_nulls = True; i += 1; continue
        path = a; i += 1

    base = os.path.join(os.path.dirname(__file__), "..", "packetlog")
    if show_nulls:
        f = path or latest(os.path.join(base, "nulls_*.jsonl"))
        if not f:
            print("no nulls_*.jsonl found"); return
        print(f"# nulls: {f}\n")
        for line in open(f, encoding="utf-8"):
            try:
                o = json.loads(line)
            except Exception:
                continue
            print(f"  t={o.get('t')} client={o.get('client')} ctx={o.get('ctx')}  {o.get('detail')}")
        return

    f = path or latest(os.path.join(base, "session_*.jsonl"))
    if not f:
        print("no session_*.jsonl found (run the server with RS2V_PKTLOG on, then connect)")
        return
    print(f"# sniff: {f}")
    print(f"# filters: dir={want_dir or 'all'} ch={want_ch if want_ch is not None else 'all'} grep={grep or '-'}\n")

    n_c2s = n_s2c = shown = 0
    for line in open(f, encoding="utf-8"):
        try:
            o = json.loads(line)
        except Exception:
            continue
        d = o.get("dir")
        if d == "C2S": n_c2s += 1
        else:          n_s2c += 1
        if want_dir and d != want_dir:
            continue
        data = bytes.fromhex(o.get("hex", ""))
        summary = ""
        if decode_packet and data:
            try:
                dec = decode_packet(data, bd_max=16384)
                summary = fmt_packet(dec)
            except Exception as e:
                summary = f"<decode err: {e}>"
        if want_ch is not None and (f"ch{want_ch} " not in summary and f"ch{want_ch}]" not in summary):
            continue
        if grep and grep not in summary:
            continue
        arrow = "->" if d == "S2C" else "<-"
        print(f"[{o.get('seq'):>4}] {d} {arrow} client {o.get('client')} {o.get('peer')}  len={o.get('len')}  {summary}")
        shown += 1

    print(f"\n# total: {n_c2s} C2S, {n_s2c} S2C  ({shown} shown after filters)")


if __name__ == "__main__":
    main()
