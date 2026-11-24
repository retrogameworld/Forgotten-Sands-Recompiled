"""Device-offset translation Torchlight -> Forgotten Sands.

For every Torchlight/Forgotten Sands function pair that fnmatch.py matched exactly (same opcode sequence),
compares the instructions one by one and lists the 16-bit displacements / immediates that differ
(D-form loads, stores, addi, cmpwi ...). With the same code and a different number, the field moved.

Usage: python offsetmap.py <fs_image.bin> <tl_image.bin> <d3d_candidates.md>
Only reads the inputs; prints the table.
"""
import re
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from fnmatch import Img, fingerprint  # noqa: E402

DFORM = {14: "addi", 15: "addis", 10: "cmplwi", 11: "cmpwi", 24: "ori", 28: "andi.", 32: "lwz", 33: "lwzu",
         34: "lbz", 36: "stw", 37: "stwu", 38: "stb", 40: "lhz", 42: "lha", 44: "sth", 48: "lfs", 50: "lfd",
         52: "stfs", 54: "stfd", 58: "ld", 62: "std"}


def main():
    fs = Img(sys.argv[1])
    tl = Img(sys.argv[2])
    pairs = []
    for line in open(sys.argv[3], encoding="utf-8"):
        cols = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cols) == 5 and cols[3].startswith("exact") and "x" not in cols[3].split()[-1]:
            m = re.match(r"sub_([0-9A-F]{8})$", cols[4])
            if m:
                pairs.append((int(cols[1][4:], 16), int(m.group(1), 16), cols[0][:40]))

    moves = defaultdict(set)  # (mnemonic, tl imm) -> {(fs imm, function)}
    for tla, fsa, name in pairs:
        n = len(fingerprint(tl, tla))
        for i in range(n):
            a, b = tl.w(tla + i * 4), fs.w(fsa + i * 4)
            if a == b:
                continue
            op = a >> 26
            if op in DFORM and (a & 0xFFFF0000) == (b & 0xFFFF0000):
                ia, ib = a & 0xFFFF, b & 0xFFFF
                if op not in (24, 28, 10):  # signed immediates
                    ia = ia - 0x10000 if ia & 0x8000 else ia
                    ib = ib - 0x10000 if ib & 0x8000 else ib
                moves[(DFORM[op], ia)].add((ib, name))
    print("| insn | Torchlight | Forgotten Sands | delta | seen in |")
    print("|---|---|---|---|---|")
    for (mn, ia), s in sorted(moves.items(), key=lambda kv: kv[0][1]):
        fsvals = sorted({v for v, _ in s})
        names = sorted({n for _, n in s})
        print("| %s | 0x%X (%d) | %s | %s | %s |" % (mn, ia & 0xFFFFFFFF, ia, ", ".join("0x%X (%d)" % (v & 0xFFFFFFFF, v) for v in fsvals),
                                                 ", ".join("%+d" % (v - ia) for v in fsvals), "; ".join(names)[:80]))


if __name__ == "__main__":
    main()
