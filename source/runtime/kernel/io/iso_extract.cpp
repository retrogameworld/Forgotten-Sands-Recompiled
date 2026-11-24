// Step 36 (public release): the player's own Xbox 360 disc image (.iso) -> the game folder, at first start.
// An Xbox 360 game disc (XGD2 / XGD3) holds an XDVDFS file system at a fixed offset; a "trimmed" XISO has it at 0.
// Volume descriptor at partition + 0x10000: "MICROSOFT*XBOX*MEDIA", u32 root directory sector, u32 root size.
// A directory is a binary tree of entries: u16 left, u16 right (offsets in dwords), u32 sector, u32 size,
// u8 attributes (0x10 = directory), u8 name length, name; 0xFF bytes pad the rest of a sector. Sectors are 2048
// bytes. All little-endian. Everything is read; nothing on the disc image is changed.
#include <stdafx.h>
#include "iso_extract.h"
#include <os/logger.h>

namespace
{
    constexpr uint64_t SECTOR = 2048;
    constexpr uint64_t PARTITIONS[] = { 0, 0x2080000, 0xFD90000, 0x18300000 };

    struct Iso
    {
        std::ifstream f;
        uint64_t base = 0;
        uint64_t totalBytes = 0, doneBytes = 0;
        std::function<void(uint64_t, uint64_t)> progress;
        std::vector<char> buffer = std::vector<char>(8 << 20);

        bool Read(uint64_t offset, void* out, size_t n)
        {
            f.seekg(std::streamoff(offset));
            f.read(reinterpret_cast<char*>(out), std::streamsize(n));
            return size_t(f.gcount()) == n;
        }
    };

    struct Entry { std::string name; uint32_t sector, size; bool dir; };
    std::string U8(const std::filesystem::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

    // All entries of one directory (the tree is walked as a flat list: every entry inside the directory's size
    // is visited once, 0xFF padding skips to the next sector).
    bool ReadDirectory(Iso& iso, uint32_t sector, uint32_t size, std::vector<Entry>& out)
    {
        std::vector<uint8_t> d(size);
        if (size == 0) return true;
        if (!iso.Read(iso.base + sector * SECTOR, d.data(), size)) return false;
        size_t p = 0;
        while (p + 14 <= size)
        {
            if (d[p] == 0xFF && d[p + 1] == 0xFF) // padding: rest of this sector
            {
                p = (p / SECTOR + 1) * SECTOR;
                continue;
            }
            const uint32_t s = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | uint32_t(d[p + 7]) << 24;
            const uint32_t n = d[p + 8] | d[p + 9] << 8 | d[p + 10] << 16 | uint32_t(d[p + 11]) << 24;
            const uint8_t attr = d[p + 12], len = d[p + 13];
            if (len == 0 || p + 14 + len > size)
                break;
            out.push_back({ std::string(reinterpret_cast<const char*>(&d[p + 14]), len), s, n, (attr & 0x10) != 0 });
            p = (p + 14 + len + 3) & ~size_t(3);
        }
        return true;
    }

    bool SafeName(const std::string& n)
    {
        return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:*?\"<>|") == std::string::npos;
    }

    uint64_t Measure(Iso& iso, uint32_t sector, uint32_t size, int depth)
    {
        std::vector<Entry> e;
        if (depth > 32 || !ReadDirectory(iso, sector, size, e)) return 0;
        uint64_t t = 0;
        for (auto& x : e)
            t += x.dir ? Measure(iso, x.sector, x.size, depth + 1) : x.size;
        return t;
    }

    bool Extract(Iso& iso, uint32_t sector, uint32_t size, const std::filesystem::path& to, int depth, std::string& error)
    {
        std::vector<Entry> entries;
        if (depth > 32 || !ReadDirectory(iso, sector, size, entries)) { error = "the disc image is damaged (a folder could not be read)"; return false; }
        std::error_code ec;
        std::filesystem::create_directories(to, ec);
        for (auto& e : entries)
        {
            if (!SafeName(e.name)) continue;
            const auto path = to / std::filesystem::u8path(e.name);
            if (e.dir)
            {
                if (!Extract(iso, e.sector, e.size, path, depth + 1, error)) return false;
                continue;
            }
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            if (!o) { error = "could not write " + U8(path); return false; }
            uint64_t left = e.size, at = iso.base + e.sector * SECTOR;
            while (left > 0)
            {
                const size_t n = size_t(std::min<uint64_t>(left, iso.buffer.size()));
                if (!iso.Read(at, iso.buffer.data(), n)) { error = "the disc image ends early (incomplete dump?)"; return false; }
                o.write(iso.buffer.data(), std::streamsize(n));
                if (!o) { error = "could not write " + U8(path) + " (disk full?)"; return false; }
                left -= n; at += n; iso.doneBytes += n;
                if (iso.progress) iso.progress(iso.doneBytes, iso.totalBytes);
            }
        }
        return true;
    }
}

namespace IsoExtract
{
    bool IsGameDisc(const std::filesystem::path& iso)
    {
        std::ifstream f(iso, std::ios::binary);
        char magic[20];
        for (uint64_t base : PARTITIONS)
        {
            f.clear(); f.seekg(std::streamoff(base + 0x10000));
            if (f.read(magic, 20) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
                return true;
        }
        return false;
    }

    bool ExtractAll(const std::filesystem::path& isoPath, const std::filesystem::path& to, std::string& error,
        std::function<void(uint64_t, uint64_t)> progress)
    {
        Iso iso;
        iso.f.open(isoPath, std::ios::binary);
        if (!iso.f) { error = "could not open " + U8(isoPath); return false; }
        uint8_t v[28] = {};
        bool found = false;
        for (uint64_t base : PARTITIONS)
        {
            iso.f.clear();
            if (iso.Read(base + 0x10000, v, 28) && memcmp(v, "MICROSOFT*XBOX*MEDIA", 20) == 0) { iso.base = base; found = true; break; }
        }
        if (!found) { error = "this is not an Xbox 360 game disc image"; return false; }
        const uint32_t rootSector = v[20] | v[21] << 8 | v[22] << 16 | uint32_t(v[23]) << 24;
        const uint32_t rootSize = v[24] | v[25] << 8 | v[26] << 16 | uint32_t(v[27]) << 24;
        iso.totalBytes = Measure(iso, rootSector, rootSize, 0);
        iso.progress = std::move(progress);
        LOGFN("[iso] {}: XDVDFS at 0x{:X}, {} MB of files", (const char*)U8(isoPath).c_str(), iso.base, iso.totalBytes >> 20);
        return Extract(iso, rootSector, rootSize, to, 0, error);
    }
}
