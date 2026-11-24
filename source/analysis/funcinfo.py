"""Small lookup helper on the decrypted image (builds\\analysis\\default_image.bin).

  python -I funcinfo.py <image.bin> func <hex addr>...   function (from .pdata / bl targets) containing each address
  python -I funcinfo.py <image.bin> callers <hex addr>   functions that bl to that address
  python -I funcinfo.py <image.bin> chain <hex addr> [depth]  caller tree upwards
Only reads the image.
"""
import sys
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from fnmatch import Img  # noqa: E402  (our fnmatch.py, not the stdlib module: run with -I and this path)
import bisect


def func_of(img, a):
    i = bisect.bisect_right(img.starts, a) - 1
    return img.starts[i] if i >= 0 else None


def callers(img, target):
    out = []
    for a in range(img.text_start, img.text_end, 4):
        x = img.w(a)
        if (x >> 26) == 18 and (x & 3) == 1:
            off = x & 0x03FFFFFC
            if off & 0x02000000:
                off -= 0x04000000
            if a + off == target:
                out.append((a, func_of(img, a)))
    return out


def main():
    img = Img(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "func":
        for s in sys.argv[3:]:
            a = int(s, 16)
            f = func_of(img, a)
            print("0x%08X in sub_%08X (size 0x%X, pdata %s)" % (a, f, img.extent(f) - f, "yes" if f in img.pdata_len else "no"))
    elif cmd == "callers":
        for a, f in callers(img, int(sys.argv[3], 16)):
            print("0x%08X in sub_%08X" % (a, f))
    elif cmd == "chain":
        depth = int(sys.argv[4]) if len(sys.argv) > 4 else 4
        start = func_of(img, int(sys.argv[3], 16))
        seen = set()

        def walk(f, d):
            print("  " * d + "sub_%08X" % f)
            if d >= depth or f in seen:
                return
            seen.add(f)
            for _, c in callers(img, f)[:8]:
                walk(c, d + 1)
        walk(start, 0)


if __name__ == "__main__":
    main()
