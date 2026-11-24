"""Lists the D3D-library functions the engine's render backend calls, with caller counts.

Usage: python backend_calls.py <image.bin> [lo hi] [d3d ranges...]
Only reads the image.
"""
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from fnmatch import Img  # noqa: E402
from funcinfo import func_of  # noqa: E402

KNOWN = {
    0x821AB5A8: "SetRenderTarget", 0x821AB280: "SetScissorRect", 0x821AB378: "SetViewport", 0x821AC628: "SetPixelShader",
    0x821AC9D0: "SetVertexShader", 0x821AC1F0: "SetVSConstF?", 0x821AC2C8: "SetPSConstF?", 0x821AC3A0: "SetConstB/I",
    0x821AC400: "SetConstB/I", 0x821AB518: "SetIndices", 0x821AB3F8: "SetStreamSource", 0x821AFFE8: "SetTexture",
    0x821AB8F8: "SetDepthStencilSurface", 0x821C3918: "ClearF", 0x821C0BC0: "BeginTiling", 0x821A6058: "Release",
    0x821A58A8: "DestroyResource", 0x821AFC80: "CreateTexture", 0x821AFDA0: "CreateSurface", 0x821AFF08: "GetSurfaceDesc",
    0x821AFBA0: "GetSurfaceLevel", 0x821AEE58: "GetLevelDesc", 0x821AEF58: "VolGetLevelDesc", 0x821AFB90: "GetLevelCount",
    0x821AFFA0: "VolGetDesc", 0x821AC538: "CreatePixelShader", 0x821AC8E0: "CreateVertexShader", 0x82995A30: "DrawVertices",
    0x82995E20: "DrawIndexedVertices", 0x82995598: "BeginVertices", 0x821B2048: "Swap", 0x821ACD78: "SetStateDefaults",
    0x821B1B10: "FrameEndPM4", 0x821A8D20: "SetViewportF internal", 0x821A7DE8: "CmdMakeSpace", 0x821A8410: "SetRingBufferParams",
    0x821A5DC8: "CacheFlush", 0x821B2C70: "QueryIssue", 0x821B2EF8: "QueryGetData", 0x821B02E0: "Cmd2MakeSpace",
    0x821AF4D8: "TextureLevelAddress", 0x821AD4B0: "DeviceInit",
}


def main():
    img = Img(sys.argv[1])
    lo, hi = (int(sys.argv[2], 16), int(sys.argv[3], 16)) if len(sys.argv) > 3 else (0x823C0000, 0x823F8000)
    ranges = [(0x821A0000, 0x821C8000), (0x82990000, 0x829A0000)]
    calls = defaultdict(set)
    for a in range(lo, hi, 4):
        x = img.w(a)
        if (x >> 26) == 18 and (x & 3) == 1:
            off = x & 0x03FFFFFC
            if off & 0x02000000:
                off -= 0x04000000
            t = a + off
            if any(r0 <= t < r1 for r0, r1 in ranges):
                calls[t].add(func_of(img, a))
    for t in sorted(calls):
        print("sub_%08X  %3d callers  %s" % (t, len(calls[t]), KNOWN.get(t, "")))


if __name__ == "__main__":
    main()
