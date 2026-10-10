"""Analyze a cdmp sniffer capture (cdmp_packets_*.bin).

Usage:
  python analyze_capture.py <capture.bin> summary            # totals + timeline
  python analyze_capture.py <capture.bin> show <name|op> [N]   # first N packets of a type (hex)
  python analyze_capture.py <capture.bin> floats <name|op>     # float32 view of payloads
  python analyze_capture.py <capture.bin> phases               # activity per 10 s window

Message names come from docs/research/data/troc_messages_<build>.csv.
Packet: u16 opcode | u8 slot | u16 payload_len | payload.
"""
import csv
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REC = struct.Struct("<QBBHHHII")  # t_us dir pad size copied pad session thread


def load_names():
    names = {}
    for p in sorted((ROOT / "docs/research/data").glob("troc_messages_*.csv")):
        with open(p, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                names[int(row["id"])] = row["name"]
    return names


def read_capture(path):
    data = Path(path).read_bytes()
    if data[:4] != b"CDPK":
        raise SystemExit("not a cdmp capture")
    pos = 12
    out = []
    while pos + REC.size <= len(data):
        t, d, _, size, copied, _, sess, thread = REC.unpack_from(data, pos)
        pos += REC.size
        pkt = data[pos:pos + copied]
        pos += copied
        if len(pkt) < 5:
            continue
        op, slot, plen = struct.unpack_from("<HbH", pkt, 0)
        out.append((t, d, op, slot, plen, size, pkt[5:], thread))
    return out


def resolve(arg, names):
    if arg.isdigit():
        return int(arg)
    for op, n in names.items():
        if n.lower() == arg.lower():
            return op
    raise SystemExit(f"unknown message {arg}")


def summary(pkts, names):
    t0, t1 = pkts[0][0], pkts[-1][0]
    print(f"{len(pkts)} packets over {(t1 - t0) / 1e6:.0f} s")
    for d, label in ((1, "C>S"), (2, "S>C")):
        c = Counter(p[2] for p in pkts if p[1] == d)
        print(f"\n== {label}: {sum(c.values())} packets, {len(c)} message types")
        for op, n in c.most_common():
            sizes = [p[4] for p in pkts if p[1] == d and p[2] == op]
            print(f"  {names.get(op, '?'):45} {op:5} x{n:<7} len {min(sizes)}..{max(sizes)}")


def phases(pkts, names):
    t0 = pkts[0][0]
    windows = defaultdict(lambda: (Counter(), Counter()))
    for p in pkts:
        w = int((p[0] - t0) / 10e6)
        windows[w][0 if p[1] == 1 else 1][p[2]] += 1
    seen = set()
    for w in sorted(windows):
        c2s, s2c = windows[w]
        new = [names.get(op, str(op)) for op in list(c2s) + list(s2c) if op not in seen]
        seen.update(c2s)
        seen.update(s2c)
        top = ", ".join(f"{names.get(o, o)} x{n}" for o, n in c2s.most_common(4))
        print(f"{w * 10:5}s C>S {sum(c2s.values()):5} S>C {sum(s2c.values()):6} | {top}")
        if new:
            print(f"        new: {', '.join(new[:12])}{' …' if len(new) > 12 else ''}")


def show(pkts, names, op, n):
    rows = [p for p in pkts if p[2] == op][:n]
    t0 = pkts[0][0]
    for t, d, _, slot, plen, size, payload, thread in rows:
        print(f"{(t - t0) / 1e6:9.3f} {'C>S' if d == 1 else 'S>C'} slot {slot} len {plen} | "
              + payload[:96].hex(" "))


def floats(pkts, names, op):
    rows = [p for p in pkts if p[2] == op][:20]
    t0 = pkts[0][0]
    for t, d, _, slot, plen, size, payload, thread in rows:
        vals = []
        for off in range(0, min(len(payload), 96) - 3, 4):
            f = struct.unpack_from("<f", payload, off)[0]
            vals.append(f"{off:02X}:{f:.2f}" if 1e-3 < abs(f) < 1e7 else f"{off:02X}:-")
        print(f"{(t - t0) / 1e6:9.3f} " + " ".join(vals))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    names = load_names()
    pkts = read_capture(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "summary":
        summary(pkts, names)
    elif cmd == "phases":
        phases(pkts, names)
    elif cmd == "show":
        show(pkts, names, resolve(sys.argv[3], names), int(sys.argv[4]) if len(sys.argv) > 4 else 10)
    elif cmd == "floats":
        floats(pkts, names, resolve(sys.argv[3], names))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
