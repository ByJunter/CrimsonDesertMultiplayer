#!/usr/bin/env python3
"""Dump the MSVC x64 RTTI of a PE file ON DISK (read-only, standard library only).

Usage:
    python rtti_dump.py [<exe>] [-o OUT.json] [--filter TEXT]

For every RTTICompleteObjectLocator (signature 1, image-relative RVAs) the script
records:
  * the class name (mangled + best-effort demangled),
  * every vtable that points at that COL (the qword right after the COL pointer),
    with the COL ``offset``/``cdOffset`` and an estimated vtable length (number of
    consecutive qwords that point into an executable section),
  * the ClassHierarchyDescriptor and its BaseClassArray (all bases with
    numContainedBases, PMD {mdisp, pdisp, vdisp} and attributes).

The RVA <-> file offset mapping is done with the PE section headers. Denuvo renames
the sections of CrimsonDesert.exe (code lives in ``.rsrc`` R-X, data in ``.rdata``
RW-, mutated code in ``.xtls`` RWX), so sections are classified by their flags,
never by their names.

Output: JSON (default ``build/research/rtti_hierarchy.json``):
    {"exe": ..., "image_base": ..., "timestamp": ..., "sections": [...],
     "classes": {"<mangled>": {"name": "<demangled>", "td_rva": ...,
                               "chd": {...}, "bases": [...], "vtables": [...]}}}
All RVAs are hex strings ("0x55B62C8"); add the image base for a VA.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import time
from array import array
from dataclasses import dataclass
from pathlib import Path

SCN_EXEC = 0x20000000
SCN_READ = 0x40000000
SCN_WRITE = 0x80000000
DEFAULT_EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
MAX_VTABLE = 4096  # hard cap for the length estimate

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
    def exec(self) -> bool:
        return bool(self.flags & SCN_EXEC)


class Pe:
    def __init__(self, data: bytes):
        if data[:2] != b"MZ":
            raise ValueError("not an MZ file")
        e = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e : e + 4] != b"PE\0\0":
            raise ValueError("PE signature not found")
        _m, nsec, self.timestamp = struct.unpack_from("<HHI", data, e + 4)
        opt_size = struct.unpack_from("<H", data, e + 20)[0]
        opt = e + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x20B:
            raise ValueError("only PE32+ (x64) is supported")
        self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
        self.size_of_image, self.size_of_headers = struct.unpack_from("<II", data, opt + 56)
        self.sections: list[Section] = []
        for i in range(nsec):
            o = opt + opt_size + 40 * i
            name, vsize, va, raw_size, raw_ptr = struct.unpack_from("<8sIIII", data, o)
            flags = struct.unpack_from("<I", data, o + 36)[0]
            self.sections.append(
                Section(name.rstrip(b"\0").decode("latin-1"), va, vsize, raw_ptr, raw_size, flags)
            )
        self.data = data
        # sorted exec ranges (RVA) for the vtable length estimate
        self.exec_ranges = sorted((s.va, s.va + max(s.vsize, s.raw_size)) for s in self.sections if s.exec)

    def rva_to_off(self, rva: int) -> int | None:
        if rva < self.size_of_headers:
            return rva
        for s in self.sections:
            if s.va <= rva < s.va + s.raw_size:
                return s.raw_ptr + (rva - s.va)
        return None

    def section_of(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.raw_size):
                return s
        return None

    def is_exec_rva(self, rva: int) -> bool:
        for lo, hi in self.exec_ranges:
            if lo <= rva < hi:
                return True
        return False

    def u32(self, rva: int) -> int | None:
        off = self.rva_to_off(rva)
        if off is None or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]

    def i32(self, rva: int) -> int | None:
        off = self.rva_to_off(rva)
        if off is None or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<i", self.data, off)[0]

    def u64(self, rva: int) -> int | None:
        off = self.rva_to_off(rva)
        if off is None or off + 8 > len(self.data):
            return None
        return struct.unpack_from("<Q", self.data, off)[0]

    def cstr(self, rva: int, limit: int = 4096) -> str | None:
        off = self.rva_to_off(rva)
        if off is None:
            return None
        end = self.data.find(b"\0", off, off + limit)
        if end < 0:
            return None
        return self.data[off:end].decode("latin-1")


# ------------------------------------------------------------------- demangling


class _Demangler:
    """Best-effort MSVC demangler for RTTI type names (``.?AV...``)."""

    PRIM = {
        "C": "signed char", "D": "char", "E": "unsigned char", "F": "short",
        "G": "unsigned short", "H": "int", "I": "unsigned int", "J": "long",
        "K": "unsigned long", "M": "float", "N": "double", "O": "long double",
        "X": "void", "Z": "...",
    }
    PRIM_ = {
        "_N": "bool", "_J": "__int64", "_K": "unsigned __int64", "_W": "wchar_t",
        "_S": "char16_t", "_U": "char32_t", "_Q": "char8_t", "_D": "__int8",
        "_E": "unsigned __int8", "_F": "__int16", "_G": "unsigned __int16",
        "_H": "__int32", "_I": "unsigned __int32",
    }

    def __init__(self, s: str):
        self.s = s
        self.i = 0

    def peek(self, n: int = 1) -> str:
        return self.s[self.i : self.i + n]

    def take(self, n: int = 1) -> str:
        r = self.s[self.i : self.i + n]
        if len(r) < n:
            raise ValueError("eof")
        self.i += n
        return r

    def number(self) -> int:
        neg = False
        if self.peek() == "?":
            neg = True
            self.i += 1
        c = self.take()
        if c.isdigit():
            v = int(c) + 1
        else:
            v = 0
            while c != "@":
                if not ("A" <= c <= "P"):
                    raise ValueError("bad number")
                v = v * 16 + (ord(c) - ord("A"))
                c = self.take()
        return -v if neg else v

    def qualified(self, names: list[str]) -> str:
        """Parse fragments until the terminating '@'. Returns 'a::b::c'."""
        parts: list[str] = []
        while True:
            c = self.peek()
            if c == "":
                raise ValueError("eof in name")
            if c == "@":
                self.i += 1
                break
            parts.append(self.fragment(names))
        return "::".join(reversed(parts))

    def fragment(self, names: list[str]) -> str:
        c = self.peek()
        if c.isdigit():
            self.i += 1
            return names[int(c)]
        if self.peek(2) == "?$":
            self.i += 2
            r = self.template()
            if len(names) < 10:
                names.append(r)
            return r
        if self.peek(2) == "?A" and self.s.startswith("?A0x", self.i):
            end = self.s.index("@", self.i)
            self.i = end + 1
            r = "`anonymous namespace'"
            if len(names) < 10:
                names.append(r)
            return r
        end = self.s.index("@", self.i)
        r = self.s[self.i : end]
        self.i = end + 1
        if len(names) < 10:
            names.append(r)
        return r

    def template(self) -> str:
        tnames: list[str] = []
        end = self.s.index("@", self.i)
        name = self.s[self.i : end]
        self.i = end + 1
        tnames.append(name)
        args: list[str] = []
        targs: list[str] = []
        while self.peek() != "@":
            if self.peek() == "":
                raise ValueError("eof in template")
            args.append(self.type_(tnames, targs))
        self.i += 1
        return f"{name}<{','.join(args)}>"

    def type_(self, names: list[str], targs: list[str]) -> str:
        c = self.peek()
        if c.isdigit():  # type back-reference
            self.i += 1
            return targs[int(c)]
        start_len = len(targs)
        r = self._type(names, targs)
        if len(targs) == start_len and len(targs) < 10 and len(r) > 1:
            targs.append(r)
        return r

    def _type(self, names: list[str], targs: list[str]) -> str:
        c = self.take()
        if c in ("V", "U", "T"):
            return self.qualified(names)
        if c == "W":
            self.take()  # enum underlying size
            return self.qualified(names)
        if c in self.PRIM:
            return self.PRIM[c]
        if c == "_":
            k = "_" + self.take()
            if k in self.PRIM_:
                return self.PRIM_[k]
            raise ValueError("unknown _ type")
        if c in ("P", "Q", "A", "R", "S"):
            ptr = "&" if c == "A" else "*"
            if self.peek() == "E":
                self.i += 1
            q = self.take()  # cv qualifier A/B/C/D
            if q == "6":  # function pointer: give up gracefully
                raise ValueError("fnptr")
            inner = self.type_(names, targs)
            return ("const " if q in "BD" else "") + inner + ptr
        if c == "$":
            k = self.take()
            if k == "0":
                return str(self.number())
            if k == "$":
                k2 = self.take()
                if k2 == "C":
                    self.take()
                    return self.type_(names, targs)
                if k2 in ("V", "Z"):
                    return ""
                if k2 == "Q":  # rvalue ref
                    self.take()
                    return self.type_(names, targs) + "&&"
                raise ValueError("$$" + k2)
            if k == "1":  # pointer to entity (mangled symbol) - skip to matching '@'
                raise ValueError("$1")
            raise ValueError("$" + k)
        raise ValueError("unknown type " + c)


def demangle(mangled: str) -> str:
    if not mangled.startswith(".?A") or len(mangled) < 5:
        return mangled
    try:
        d = _Demangler(mangled[4:])
        r = d.qualified([])
        return r
    except (ValueError, IndexError):
        return mangled


# ------------------------------------------------------------------------- RTTI


def find_type_descriptors(pe: Pe) -> dict[int, str]:
    """TypeDescriptor RVA -> mangled name. The name lives at TD+0x10."""
    tds: dict[int, str] = {}
    pat = re.compile(rb"\.\?A[VU][\x21-\x7e]{1,4000}?@@\x00")
    for s in pe.sections:
        if s.exec and not (s.flags & SCN_WRITE):
            continue
        blob = pe.data[s.raw_ptr : s.raw_ptr + s.raw_size]
        for m in pat.finditer(blob):
            name_rva = s.va + m.start()
            tds[name_rva - 0x10] = m.group()[:-1].decode("latin-1")
    return tds


def find_cols(pe: Pe, tds: dict[int, str]) -> dict[int, tuple[int, int, int, int]]:
    """COL RVA -> (offset, cdOffset, td_rva, chd_rva). Signature 1, pSelf == own RVA."""
    cols: dict[int, tuple[int, int, int, int]] = {}
    for s in pe.sections:
        if s.exec and not (s.flags & SCN_WRITE):
            continue
        n = s.raw_size // 4 * 4
        a = array("I")
        a.frombytes(pe.data[s.raw_ptr : s.raw_ptr + n])
        if sys.byteorder != "little":
            a.byteswap()
        va = s.va
        # candidates: 4-aligned dword == 1; pSelf (dword i+5) must equal the COL RVA
        ln = len(a) - 6
        blob = pe.data[s.raw_ptr : s.raw_ptr + n]
        pos = blob.find(b"\x01\x00\x00\x00")
        while pos >= 0:
            if pos % 4 == 0:
                i = pos // 4
                if i < ln and a[i + 5] == va + pos:
                    td = a[i + 3]
                    if td in tds:
                        cols[va + pos] = (a[i + 1], a[i + 2], td, a[i + 4])
            pos = blob.find(b"\x01\x00\x00\x00", pos + 1)
    return cols


def find_vtables(pe: Pe, cols: dict[int, tuple]) -> dict[int, list[int]]:
    """COL RVA -> list of vtable RVAs (qword after a qword == image_base + COL RVA)."""
    want = {pe.image_base + rva: rva for rva in cols}
    out: dict[int, list[int]] = {}
    for s in pe.sections:
        if s.exec and not (s.flags & SCN_WRITE):
            continue
        n = s.raw_size // 8 * 8
        q = array("Q")
        q.frombytes(pe.data[s.raw_ptr : s.raw_ptr + n])
        if sys.byteorder != "little":
            q.byteswap()
        # set intersection to avoid a Python-level loop over every qword
        hits = want.keys() & set(q)
        if not hits:
            continue
        for j, v in enumerate(q):
            if v in hits:
                out.setdefault(want[v], []).append(s.va + 8 * (j + 1))
    return out


def vtable_len(pe: Pe, vt_rva: int, col_vas: set[int]) -> int:
    n = 0
    off = pe.rva_to_off(vt_rva)
    if off is None:
        return 0
    base = pe.image_base
    while n < MAX_VTABLE:
        if off + 8 * (n + 1) > len(pe.data):
            break
        v = struct.unpack_from("<Q", pe.data, off + 8 * n)[0]
        if v in col_vas or v < base:
            break
        if not pe.is_exec_rva(v - base):
            break
        n += 1
    return n


def parse_chd(pe: Pe, chd_rva: int, tds: dict[int, str]) -> tuple[dict, list[dict]]:
    sig, attrs, nbases, bca = (pe.u32(chd_rva + k) for k in (0, 4, 8, 12))
    chd = {"rva": hex(chd_rva), "signature": sig, "attributes": attrs,
           "num_bases": nbases, "bca_rva": hex(bca or 0)}
    bases: list[dict] = []
    if nbases is None or bca is None or nbases > 512:
        return chd, bases
    for k in range(nbases):
        bcd = pe.u32(bca + 4 * k)
        if bcd is None:
            break
        td = pe.u32(bcd)
        ncont = pe.u32(bcd + 4)
        mdisp, pdisp, vdisp = pe.i32(bcd + 8), pe.i32(bcd + 12), pe.i32(bcd + 16)
        battr = pe.u32(bcd + 20)
        mangled = tds.get(td) if td is not None else None
        if mangled is None and td is not None:
            mangled = pe.cstr(td + 0x10) or f"<td {td:#x}>"
        bases.append({
            "mangled": mangled, "name": demangle(mangled), "num_contained": ncont,
            "mdisp": mdisp, "pdisp": pdisp, "vdisp": vdisp, "attributes": battr,
        })
    return chd, bases


# ------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", nargs="?", default=DEFAULT_EXE)
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("-o", "--out", default=str(root / "build" / "research" / "rtti_hierarchy.json"))
    ap.add_argument("--filter", help="only print classes whose demangled name contains TEXT")
    args = ap.parse_args()

    t0 = time.time()
    data = Path(args.exe).read_bytes()  # read-only
    pe = Pe(data)
    print(f"[+] {args.exe}: {len(data)/2**20:.1f} MiB, base {pe.image_base:#x}, "
          f"timestamp {pe.timestamp:#x}", file=sys.stderr)
    for s in pe.sections:
        print(f"    {s.name:<8} {s.perms} va {s.va:#010x} vsize {s.vsize:#010x} "
              f"raw {s.raw_ptr:#010x}+{s.raw_size:#x}", file=sys.stderr)

    tds = find_type_descriptors(pe)
    print(f"[+] {len(tds)} TypeDescriptors ({time.time()-t0:.1f}s)", file=sys.stderr)
    cols = find_cols(pe, tds)
    print(f"[+] {len(cols)} CompleteObjectLocators ({time.time()-t0:.1f}s)", file=sys.stderr)
    vts = find_vtables(pe, cols)
    print(f"[+] {sum(map(len, vts.values()))} vtables for {len(vts)} COLs ({time.time()-t0:.1f}s)",
          file=sys.stderr)

    col_vas = {pe.image_base + r for r in cols}
    classes: dict[str, dict] = {}
    chd_cache: dict[int, tuple[dict, list[dict]]] = {}
    for col_rva, (offset, cd_off, td, chd_rva) in sorted(cols.items()):
        mangled = tds[td]
        c = classes.get(mangled)
        if c is None:
            if chd_rva not in chd_cache:
                chd_cache[chd_rva] = parse_chd(pe, chd_rva, tds)
            chd, bases = chd_cache[chd_rva]
            c = classes[mangled] = {
                "name": demangle(mangled), "td_rva": hex(td), "chd": chd,
                "bases": bases, "vtables": [],
            }
        for vt in vts.get(col_rva, []):
            sec = pe.section_of(vt)
            c["vtables"].append({
                "rva": hex(vt), "col_rva": hex(col_rva), "offset": offset,
                "cd_offset": cd_off, "length": vtable_len(pe, vt, col_vas),
                "section": f"{sec.name}({sec.perms})" if sec else None,
            })
        if not vts.get(col_rva):
            c["vtables"].append({"rva": None, "col_rva": hex(col_rva), "offset": offset,
                                 "cd_offset": cd_off, "length": 0, "section": None})
    for c in classes.values():
        c["vtables"].sort(key=lambda v: v["offset"])

    out = {
        "exe": args.exe, "file_size": len(data), "image_base": hex(pe.image_base),
        "timestamp": hex(pe.timestamp),
        "sections": [{"name": s.name, "perms": s.perms, "va": hex(s.va), "vsize": hex(s.vsize),
                      "raw_ptr": hex(s.raw_ptr), "raw_size": hex(s.raw_size)} for s in pe.sections],
        "stats": {"type_descriptors": len(tds), "cols": len(cols), "classes_with_col": len(classes),
                  "vtables": sum(len(v) for v in vts.values())},
        "classes": classes,
    }
    p = Path(args.out)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(out, indent=1), encoding="utf-8")
    print(f"[+] wrote {p} ({len(classes)} classes, {time.time()-t0:.1f}s)", file=sys.stderr)

    if args.filter:
        for m, c in sorted(classes.items(), key=lambda kv: kv[1]["name"]):
            if args.filter in c["name"]:
                vt = c["vtables"][0] if c["vtables"] else {}
                print(f"{c['name']}  vt={vt.get('rva')} len={vt.get('length')}  "
                      f"bases=[{', '.join(b['name'] for b in c['bases'][1:])}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
