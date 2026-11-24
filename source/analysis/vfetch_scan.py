# Scans translated-shader blobs (builds/test_user/shader_cache/*.bin) and reports vertex shaders whose
# vfetch instructions use an index register other than r0 (computed index = instancing / manual fetch).
import struct, sys, glob, os
d = sys.argv[1]
files = glob.glob(os.path.join(d, '*.bin'))
total = computed = 0
for i, f in enumerate(files):
    if len(files) >= 20 and i % max(1, len(files) // 20) == 0:
        print(f'  {100 * i // len(files)}%', flush=True)
    b = open(f, 'rb').read()
    if len(b) < 0x24: continue
    w = lambda o: struct.unpack(">I", b[o:o+4])[0] if o + 4 <= len(b) else 0
    flags, vsize, psize = w(0), w(4), w(8)
    if (flags & 0xFFFFFF00) != 0x102A1100: continue
    so = w(24)
    if so + 36 > len(b): continue
    phys, field18, count = w(so), w(so + 24), w(so + 28)
    if count == 0 or count > 32: continue  # pixel shaders / no inputs
    code = vsize + phys
    total += 1
    srcs = []
    for k in range(count):
        v = w(so + 36 + (field18 + k) * 4)
        addr, usage, idx = v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF
        o = code + addr * 12
        if o + 12 > len(b): continue
        w0, w1 = w(o), w(o + 4)
        if w0 & 0x1F: continue
        src = (w0 >> 5) & 0x3F; slot = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3); mini = (w1 >> 30) & 1
        srcs.append((usage, idx, src, (w0 >> 30) & 3, 95 - slot, mini))
    if any(s[2] != 0 for s in srcs):
        computed += 1
        if computed <= 12:
            print(os.path.basename(f), ' '.join(f'u{u}i{x}:r{s}.{"xyzw"[sw]}/s{st}{"m" if m else ""}' for u, x, s, sw, st, m in srcs))
print(f'vertex shaders {total}, with computed fetch index {computed}')
