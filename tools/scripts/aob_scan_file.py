#!/usr/bin/env python3
"""Scan a PE file ON DISK for AOB signatures (read-only, standard library only).

Usage:
    python aob_scan_file.py <exe> <sigs_file> [--max N] [--bytes] [--only NAME]

Sigs file: one signature per line, ``name | pattern | description | source``.
Blank lines and lines starting with ``#`` are ignored. If the file contains any
```` ```sigs ```` fenced block (e.g. a Markdown research note), only the lines
inside those blocks are read -- the same rule the in-game probe uses.

Pattern tokens (space separated):
    AA        literal byte
    ? / ??    any byte
    4? / ?8   nibble wildcard (DetourModKit style)
    [n-m]     bounded gap of n..m arbitrary bytes ([n] = exactly n)
    @         result marker: the reported "result" offset is this position
              (DetourModKit writes it as ``|``; ``|`` is the field separator here)
Only ``AA`` and ``??`` are understood by the live-memory probe; the other tokens
exist so upstream DetourModKit ladders can be checked verbatim.

For every match the script prints the file offset, RVA, VA, the section it falls
in (with its R/W/X flags -- Denuvo renames sections, so trust the flags, not the
name) and, when the instruction at the result position is RIP-relative
(``[rip+disp32]`` mov/lea/cmp/... or ``E8``/``E9`` rel32), the decoded target.

A pattern with 0 matches is UNVERIFIED on disk (the code may be absent in this
build, or encrypted/virtualised by Denuvo and only present in live memory).
Exactly 1 match is the goal; more than 1 means the pattern needs more context.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass

SCN_EXEC = 0x20000000
SCN_READ = 0x40000000
SCN_WRITE = 0x80000000
COUNT_CAP = 1000  # stop counting after this many matches per pattern


# ----------------------------------------------------------------------------- PE


@dataclass
class Section:
    name: str
    va: int
    vsize: int
    raw_ptr: int
    raw_size: int
    flags: int

    @property
    def perms(self) -> str:
        return (
            ("R" if self.flags & SCN_READ else "-")
            + ("W" if self.flags & SCN_WRITE else "-")
            + ("X" if self.flags & SCN_EXEC else "-")
        )

    @property
    def label(self) -> str:
        return f"{self.name}({self.perms})"


@dataclass
class PeImage:
    image_base: int
    size_of_image: int
    size_of_headers: int
    timestamp: int
    sections: list[Section]

    def off_to_rva(self, off: int) -> tuple[int | None, Section | None]:
        if off < self.size_of_headers:
            return off, None
        for s in self.sections:
            if s.raw_ptr <= off < s.raw_ptr + s.raw_size:
                return s.va + (off - s.raw_ptr), s
        return None, None

    def rva_to_section(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.raw_size):
                return s
        return None


def parse_pe(data: bytes) -> PeImage:
    if data[:2] != b"MZ":
        raise ValueError("not an MZ file")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError("PE signature not found")
    _machine, nsec, timestamp = struct.unpack_from("<HHI", data, e_lfanew + 4)
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        raise ValueError("only PE32+ (x64) images are supported")
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    size_of_image, size_of_headers = struct.unpack_from("<II", data, opt + 56)
    sections = []
    for i in range(nsec):
        o = opt + opt_size + 40 * i
        name, vsize, va, raw_size, raw_ptr = struct.unpack_from("<8sIIII", data, o)
        flags = struct.unpack_from("<I", data, o + 36)[0]
        sections.append(
            Section(name.rstrip(b"\0").decode("latin-1"), va, vsize, raw_ptr, raw_size, flags)
        )
    return PeImage(image_base, size_of_image, size_of_headers, timestamp, sections)


# ----------------------------------------------------------------------- patterns

_GAP = re.compile(r"^\[(\d+)(?:-(\d+))?\]$")
_HEX = "0123456789abcdefABCDEF"


def compile_pattern(text: str) -> re.Pattern[bytes]:
    """Translate the pattern syntax into a bytes regex. The marker is group 'm'."""
    parts: list[bytes] = []
    marker = False
    for tok in text.split():
        if tok in ("?", "??"):
            parts.append(b".")
        elif tok == "@":
            if marker:
                raise ValueError("more than one @ marker")
            parts.append(b"(?P<m>)")
            marker = True
        elif m := _GAP.match(tok):
            lo = int(m.group(1))
            hi = int(m.group(2)) if m.group(2) else lo
            if hi < lo:
                raise ValueError(f"bad gap {tok}")
            parts.append(b".{%d,%d}?" % (lo, hi))
        elif len(tok) == 2 and tok[0] == "?" and tok[1] in _HEX:
            lo_nib = int(tok[1], 16)
            parts.append(b"[" + b"".join(re.escape(bytes([h << 4 | lo_nib])) for h in range(16)) + b"]")
        elif len(tok) == 2 and tok[1] == "?" and tok[0] in _HEX:
            base = int(tok[0], 16) << 4
            parts.append(b"[" + re.escape(bytes([base])) + b"-" + re.escape(bytes([base | 0xF])) + b"]")
        elif len(tok) == 2 and tok[0] in _HEX and tok[1] in _HEX:
            parts.append(re.escape(bytes([int(tok, 16)])))
        else:
            raise ValueError(f"bad token {tok!r}")
    if not parts:
        raise ValueError("empty pattern")
    return re.compile(b"".join(parts), re.DOTALL)


def find_all(data: bytes, rx: re.Pattern[bytes], cap: int) -> list[re.Match[bytes]]:
    """All (overlapping) matches, at most one per start offset."""
    out = []
    pos = 0
    while len(out) < cap:
        m = rx.search(data, pos)
        if m is None:
            break
        out.append(m)
        pos = m.start() + 1
    return out


# -------------------------------------------------------------- RIP-rel decoding


def decode_rip_target(pe: PeImage, data: bytes, off: int, rva: int) -> tuple[int, str] | None:
    """Return (target_rva, mnemonic-ish) if the instruction at `off` uses rip+disp32."""
    b = data[off : off + 16]
    if len(b) < 6:
        return None
    if b[0] in (0xE8, 0xE9):
        disp = struct.unpack_from("<i", b, 1)[0]
        return rva + 5 + disp, "call" if b[0] == 0xE8 else "jmp"
    i = 0
    if b[i] in (0x66, 0xF2, 0xF3):  # operand-size / SSE prefix
        i += 1
    if 0x40 <= b[i] <= 0x4F:  # REX
        i += 1
    if b[i] == 0x0F:  # two-byte opcode (movss/movups/...)
        i += 2
    elif b[i] == 0xFF:  # call/jmp [rip+disp32]
        i += 1
    else:
        i += 1
    if i >= len(b) - 4:
        return None
    modrm = b[i]
    if (modrm & 0xC7) != 0x05:  # mod=00, rm=101 -> [rip+disp32]
        return None
    disp = struct.unpack_from("<i", b, i + 1)[0]
    length = i + 5
    # Instructions with an immediate after the displacement (e.g. 80 3D ... imm8,
    # C7 05 ... imm32) would need the immediate size; flag them as approximate.
    opcode = b[i - 1]
    if opcode in (0x80, 0x83, 0xC6):
        length += 1
    elif opcode in (0x81, 0xC7):
        length += 4
    return rva + length + disp, f"rip-rel (len {length})"


# --------------------------------------------------------------------- sigs file


@dataclass
class Sig:
    name: str
    pattern: str
    desc: str
    source: str
    line: int


def load_sigs(path: str) -> list[Sig]:
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    has_blocks = any(l.strip().startswith("```sigs") for l in lines)
    inside = not has_blocks
    sigs = []
    for n, raw in enumerate(lines, 1):
        l = raw.strip()
        if has_blocks:
            if l.startswith("```sigs"):
                inside = True
                continue
            if l.startswith("```"):
                inside = False
                continue
        if not inside or not l or l.startswith("#"):
            continue
        fields = [f.strip() for f in l.split("|")]
        if len(fields) < 2:
            print(f"warning: line {n}: expected 'name | pattern | desc | source'", file=sys.stderr)
            continue
        fields += [""] * (4 - len(fields))
        sigs.append(Sig(fields[0], fields[1], fields[2], " | ".join(fields[3:]).strip(" |"), n))
    return sigs


# --------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe")
    ap.add_argument("sigs_file")
    ap.add_argument("--max", type=int, default=8, help="matches to list per pattern (default 8)")
    ap.add_argument("--bytes", action="store_true", help="print the matched bytes")
    ap.add_argument("--only", help="scan only signatures whose name contains this text")
    args = ap.parse_args()

    with open(args.exe, "rb") as fh:
        data = fh.read()
    pe = parse_pe(data)
    print(f"file      : {args.exe}")
    print(f"size      : {len(data):,} bytes   timestamp 0x{pe.timestamp:08X}")
    print(f"image     : base 0x{pe.image_base:X}  SizeOfImage 0x{pe.size_of_image:X}")
    print("sections  :")
    for s in pe.sections:
        print(
            f"  {s.name:<8} {s.perms}  rva 0x{s.va:08X}+0x{s.vsize:08X}  "
            f"raw 0x{s.raw_ptr:08X}+0x{s.raw_size:08X}"
        )
    print()

    sigs = load_sigs(args.sigs_file)
    if args.only:
        sigs = [s for s in sigs if args.only.lower() in s.name.lower()]
    summary = []
    for sig in sigs:
        try:
            rx = compile_pattern(sig.pattern)
        except ValueError as exc:
            print(f"[ERROR    ] {sig.name}: {exc}\n")
            summary.append((sig.name, "ERROR"))
            continue
        matches = find_all(data, rx, COUNT_CAP)
        n = len(matches)
        count = f"{n}+" if n >= COUNT_CAP else str(n)
        status = "UNIQUE" if n == 1 else ("NOT FOUND" if n == 0 else f"MULTI({count})")
        print(f"[{status:<9}] {sig.name}   ({sig.source})")
        if sig.desc:
            print(f"             {sig.desc}")
        for m in matches[: args.max]:
            off = m.start()
            rva, sec = pe.off_to_rva(off)
            res_off = m.start("m") if "m" in rx.groupindex else off
            res_rva, _ = pe.off_to_rva(res_off)
            where = sec.label if sec else "(headers/overlay)"
            line = f"    file 0x{off:08X}"
            if rva is not None:
                line += f"  rva 0x{rva:08X}  va 0x{pe.image_base + rva:X}  {where}"
            if res_off != off and res_rva is not None:
                line += f"  result rva 0x{res_rva:08X} (+0x{res_off - off:X})"
            if res_rva is not None:
                tgt = decode_rip_target(pe, data, res_off, res_rva)
                if tgt is not None:
                    tsec = pe.rva_to_section(tgt[0])
                    line += f"  -> 0x{tgt[0]:08X} {tsec.label if tsec else '?'} [{tgt[1]}]"
            print(line)
            if args.bytes:
                print("      " + m.group(0).hex(" ").upper())
        if n > args.max:
            print(f"    ... {count} matches total")
        print()
        summary.append((sig.name, status))

    print("summary:")
    for name, status in summary:
        print(f"  {status:<10} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
