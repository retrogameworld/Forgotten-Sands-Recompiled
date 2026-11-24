"""Finds Forgotten Sands functions that match already-identified Torchlight functions.

Both games link the Xbox 360 D3D library (different XDK versions), so the same D3D function often has
the same or a very similar instruction sequence. Torchlight's D3D functions are mapped in
E:/TorchlightRecomp/work/d3d_research/d3d_functions.md; this tool reads every sub_XXXXXXXX named in its
table rows, fingerprints those functions and every function in Forgotten Sands, and lists the best matches.

Fingerprint = the instruction sequence with registers, branch targets and immediates masked out
(primary + extended opcode only). Exact matches are tried first, then a similarity score.

Usage: python -I fnmatch.py <fs_image.bin> <tl_image.bin> <tl_d3d_functions.md> <out.md>
Only reads the inputs and writes the one output file. Progress every 5%.
"""
import bisect
import difflib
import re
import struct
import sys
from collections import defaultdict

BASE = 0x82000000
BLR = 0x4E800020


class Img:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        pdata_addr, pdata_size = self.section(".pdata")
        self.text_start, text_size = self.section(".text")
        self.text_end = self.text_start + text_size
        starts = set()
        self.pdata_len = {}
        for i in range(0, pdata_size, 8):
            begin, packed = struct.unpack_from(">II", self.d, pdata_addr - BASE + i)
            if begin == 0:
                continue
            starts.add(begin)
            self.pdata_len[begin] = ((packed >> 8) & 0x3FFFFF) * 4
        # bl targets are function starts too (many small functions have no .pdata entry)
        for a in range(self.text_start, self.text_end, 4):
            x = self.w(a)
            if (x >> 26) == 18 and (x & 3) == 1:
                off = x & 0x03FFFFFC
                if off & 0x02000000:
                    off -= 0x04000000
                t = a + off
                if self.text_start <= t < self.text_end:
                    starts.add(t)
        self.starts = sorted(starts)

    def section(self, name):
        pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        count = struct.unpack_from("<H", self.d, pe + 6)[0]
        opt = struct.unpack_from("<H", self.d, pe + 20)[0]
        sh = pe + 24 + opt
        for i in range(count):
            n = self.d[sh + i * 40:sh + i * 40 + 8].rstrip(b"\0").decode(errors="replace")
            vsize, vaddr = struct.unpack_from("<II", self.d, sh + i * 40 + 8)
            if n == name:
                return BASE + vaddr, vsize
        raise KeyError(name)

    def w(self, a):
        return struct.unpack_from(">I", self.d, a - BASE)[0]

    def extent(self, start):
        """Function end: .pdata length, else up to the next known start (max 4 KB)."""
        if start in self.pdata_len:
            return start + self.pdata_len[start]
        i = bisect.bisect_right(self.starts, start)
        nxt = self.starts[i] if i < len(self.starts) else self.text_end
        return min(nxt, start + 0x1000)


def norm(x):
    op = x >> 26
    if op in (31, 63, 59, 19, 30):
        return (op << 10) | ((x >> 1) & 0x3FF)
    if op == 4:  # VMX: keep the low 11 bits that select the operation
        return (op << 11) | (x & 0x7FF)
    if op in (16, 18):  # branches: keep link/absolute bits, not the target
        return (op << 2) | (x & 3)
    return op << 10


def raw(x):
    """The instruction word with branch targets masked (they move with the code)."""
    op = x >> 26
    if op == 18:
        return x & 0xFC000003
    if op == 16:
        return x & 0xFFFF0003
    return x


def fingerprint(img, start):
    end = img.extent(start)
    return tuple(norm(img.w(a)) for a in range(start, end, 4))


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1
    fs = Img(sys.argv[1])
    tl = Img(sys.argv[2])
    md = open(sys.argv[3], encoding="utf-8", errors="replace").read()

    # Every Torchlight function named in a table row, with the row's first two columns as its name.
    targets = {}
    for line in md.splitlines():
        if not line.startswith("|"):
            continue
        cols = [c.strip() for c in line.strip("|").split("|")]
        if len(cols) < 3:
            continue
        for m in re.finditer(r"sub_([0-9A-F]{8})", cols[2]):
            addr = int(m.group(1), 16)
            targets.setdefault(addr, (cols[0] + " / " + cols[1])[:90])
    print("%d Torchlight functions to look for, %d Forgotten Sands functions" % (len(targets), len(fs.starts)))

    fs_prints = {}
    by_print = defaultdict(list)
    by_len = defaultdict(list)
    last = -1
    for i, s in enumerate(fs.starts):
        pct = i * 100 // len(fs.starts)
        if pct // 5 != last // 5:
            last = pct
            print("fingerprinting %d%%" % (pct // 5 * 5), flush=True)
        fp = fingerprint(fs, s)
        fs_prints[s] = fp
        by_print[fp].append(s)
        by_len[len(fp)].append(s)

    rows = []
    last = -1
    for n, (addr, name) in enumerate(sorted(targets.items())):
        pct = n * 100 // max(1, len(targets))
        if pct // 5 != last // 5:
            last = pct
            print("matching %d%%" % (pct // 5 * 5), flush=True)
        fp = fingerprint(tl, addr)
        exact = by_print.get(fp, [])
        if len(exact) == 1:
            # "same" = also the same registers, offsets and constants (only branch targets differ), so
            # struct offsets the hook code relies on are unchanged between the XDK versions.
            same = all(raw(tl.w(addr + i * 4)) == raw(fs.w(exact[0] + i * 4)) for i in range(len(fp)))
            rows.append((name, addr, len(fp), "exact" + (" same" if same else " offsets differ"), ["sub_%08X" % exact[0]]))
            continue
        if exact:
            rows.append((name, addr, len(fp), "exact x%d" % len(exact), ["sub_%08X" % a for a in exact[:6]]))
            continue
        # similarity among functions of similar size
        cands = []
        lo, hi = int(len(fp) * 0.7), int(len(fp) * 1.3) + 2
        for L in range(lo, hi + 1):
            for s in by_len.get(L, ()):
                sm = difflib.SequenceMatcher(None, fp, fs_prints[s], autojunk=False)
                if sm.real_quick_ratio() < 0.75 or sm.quick_ratio() < 0.75:
                    continue
                r = sm.ratio()
                if r >= 0.75:
                    cands.append((r, s))
        cands.sort(reverse=True)
        if cands:
            rows.append((name, addr, len(fp), "similar", ["sub_%08X (%.2f)" % (s, r) for r, s in cands[:4]]))
        else:
            rows.append((name, addr, len(fp), "none", []))

    with open(sys.argv[4], "w", encoding="utf-8") as out:
        out.write("# Forgotten Sands candidates for Torchlight's mapped D3D functions (fnmatch.py)\n\n")
        out.write("| Torchlight hook / function | TL address | insns | match | Forgotten Sands candidates |\n|---|---|---|---|---|\n")
        for name, addr, n, kind, c in rows:
            out.write("| %s | sub_%08X | %d | %s | %s |\n" % (name.replace("|", "/"), addr, n, kind, ", ".join(c)))
    kinds = defaultdict(int)
    for r in rows:
        kinds[r[3].split()[0]] += 1
    print("done: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())) + " -> " + sys.argv[4])
    return 0


if __name__ == "__main__":
    sys.exit(main())
