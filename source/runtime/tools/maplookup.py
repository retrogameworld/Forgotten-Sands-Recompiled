"""Find which function contains an exe offset reported by the crash handler.
Usage: python maplookup.py <TorchlightRecomp.map> <exe+offset hex> [...]
Reads the lld-link /MAP file only."""
import re
import sys
import bisect

map_path = sys.argv[1]
symbols = []
# link.exe-style map: " 0001:00001234  symbol  0000000140002234 f obj"
pattern = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})")
with open(map_path, errors="replace") as f:
    for line in f:
        m = pattern.match(line)
        if not m:
            continue
        symbols.append((int(m.group(2), 16), m.group(1)))

symbols.sort()
addrs = [a for a, _ in symbols]
base = 0x140000000
for arg in sys.argv[2:]:
    off = int(arg.replace("exe+", ""), 16)
    target = base + off
    i = bisect.bisect_right(addrs, target) - 1
    if i < 0:
        print(arg, "-> ?")
    else:
        print(f"{arg} -> {symbols[i][1]} +0x{target - symbols[i][0]:X}")
