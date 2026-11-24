#include <stdafx.h>
#include "file_system.h"
#include <cpu/guest_thread.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <kernel/function.h>
#include <kernel/import_log.h>
#include <kernel/kernel_objects.h>
#include <kernel/xstatus.h>
#include <os/logger.h>

// Step 35: reads into guest memory go through a bounce buffer. The renderer write-protects pages of stable
// vertex/index buffers to notice changes (gpu/video.cpp WriteWatch); a CRT fread that hands the destination
// straight to ReadFile would fail on such a page (kernel-mode writes raise no exception), while a user-mode
// copy faults, gets the page unprotected and continues.
static size_t GuestFread(void* buffer, size_t length, FILE* fp)
{
    const auto* p = static_cast<uint8_t*>(buffer);
    if (p < g_memory.base || p >= g_memory.base + 0x100000000ull)
        return fread(buffer, 1, length, fp);
    static thread_local std::vector<uint8_t> s_bounce;
    if (s_bounce.size() < 0x100000)
        s_bounce.resize(0x100000);
    size_t done = 0;
    while (done < length)
    {
        const size_t want = std::min(length - done, s_bounce.size());
        const size_t n = fread(s_bounce.data(), 1, want, fp);
        memcpy(static_cast<uint8_t*>(buffer) + done, s_bounce.data(), n);
        done += n;
        if (n < want)
            break;
    }
    return done;
}
#include <os/perf.h>
#include "chacha20.h"
#include <random>
#include <user/config.h>
#include <user/paths.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// Xbox kernel file I/O (Nt*File) on top of the host file system.
// UnleashedRecomp hooked Sonic's XCreateFileA/XReadFile wrappers at fixed addresses; for
// Torchlight the kernel calls themselves are implemented so no game addresses are needed.
// Portable C stdio + std::filesystem only (Android later). game:\ and d:\ are read only.

// Opens a host file. On Windows the file is opened with FILE_SHARE_DELETE so it can be renamed or
// replaced while the game still holds another handle to it (the console allows that; the save code
// renames 0.TSV -> backup.tmp and save.tmp -> 0.TSV while 0.TSV is still open for reading).
static FILE* HostOpen(const std::filesystem::path& path, const char* mode)
{
#ifdef _WIN32
    const bool plus = strchr(mode, '+') != nullptr;
    DWORD access = GENERIC_READ | ((plus || mode[0] == 'w') ? GENERIC_WRITE : 0);
    DWORD disposition = mode[0] == 'w' ? CREATE_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;
    int fd = _open_osfhandle(intptr_t(h), (access & GENERIC_WRITE) ? 0 : _O_RDONLY);
    if (fd < 0)
    {
        CloseHandle(h);
        return nullptr;
    }
    FILE* fp = _fdopen(fd, mode);
    if (!fp)
        _close(fd);
    return fp;
#else
    return fopen(path.c_str(), mode);
#endif
}

// ---------------------------------------------------------------------------------------
// Virtual tails (Step 7): a host file whose bytes from `cut` on are replaced by an in-memory buffer.

static std::string ToLowerAscii(const std::u8string& s)
{
    std::string r(s.begin(), s.end());
    for (auto& c : r) c = char(tolower(uint8_t(c)));
    return r;
}

struct VirtualTail
{
    std::filesystem::path path;
    uint64_t cut = 0;
    std::vector<uint8_t> tail;
};
static std::vector<VirtualTail> g_virtualTails; // set before the guest starts, read-only afterwards

void FileSystem::SetVirtualTail(const std::filesystem::path& hostFile, uint64_t cut, std::vector<uint8_t> tail)
{
    for (auto& v : g_virtualTails)
        if (v.path == hostFile) { v.cut = cut; v.tail = std::move(tail); return; }
    g_virtualTails.push_back({ hostFile, cut, std::move(tail) });
}

static const VirtualTail* FindVirtualTail(const std::filesystem::path& p)
{
    if (g_virtualTails.empty() || p.empty())
        return nullptr;
    std::error_code ec;
    for (auto& v : g_virtualTails)
        if (ToLowerAscii(p.filename().u8string()) == ToLowerAscii(v.path.filename().u8string()) &&
            (ToLowerAscii(p.u8string()) == ToLowerAscii(v.path.u8string()) || std::filesystem::equivalent(p, v.path, ec)))
            return &v; // (paths inside the game pak are not real files: compared as text, Step 26)
    return nullptr;
}

static uint64_t VirtualSize(const std::filesystem::path& p, uint64_t realSize)
{
    if (auto v = FindVirtualTail(p))
        return v->cut + v->tail.size();
    return realSize;
}

// ---------------------------------------------------------------------------------------
// Path resolution

static std::mutex g_linkMutex;
static std::map<std::string, std::string> g_devices;  // lower-case device prefix -> host path (utf8)
static std::map<std::string, std::string> g_symlinks; // lower-case "name:" -> guest target

static std::string ToLower(std::string_view s)
{
    std::string r(s);
    for (auto& c : r) c = char(tolower(uint8_t(c)));
    return r;
}

static std::filesystem::path U8Path(const std::string& s)
{
    return std::filesystem::path(std::u8string_view((const char8_t*)s.data(), s.size()));
}

static std::string PathToU8(const std::filesystem::path& p)
{
    auto u8 = p.u8string();
    return std::string((const char*)u8.data(), u8.size());
}

// ---------------------------------------------------------------------------------------
// Step 26: the game pak (ForgottenSands.pak, made with ForgottenSandsRecomp.exe --pack, see FileSystem::BuildPak).
// Everything the game reads from game:\ (default.xex, pak.zip, music, translations, ... and our AI-upscaled
// hires\ images) in one file, so the release folder has no loose game files. Layout (little endian):
//   "TLPK", u32 version (2), u64 index offset, u32 index size, u32 0, 12-byte nonce, then the file data,
//   then the index: u32 count, count x { u16 name length, name (utf-8, '/' separators), u64 offset, u64 size }.
// Step 27: data and index are encrypted with ChaCha20 (key derived in PakKey, never stored as such; the
// key stream position is the byte position in the pak), so nothing in it can be read without this exe.
// Paths inside the pak are "<pak file>/<relative path>" (std::filesystem::path), never real files.

struct PakEntry
{
    std::string name;     // relative path, original case, '/' separators
    uint64_t offset = 0;
    uint64_t size = 0;
    bool isDirectory = false;
};

static struct
{
    bool mounted = false;
    uint64_t base = 0;                                    // Step 33: where the pak starts in its file (inside Setup.exe)
    uint32_t nonce[3] = {};
    std::filesystem::path file;
    std::string fileU8;                                   // PathToU8 of the pak file, prefix of all paths inside it
    std::unordered_map<std::string, PakEntry> entries;    // lower-case relative path -> entry (files and folders)
    std::unordered_map<std::string, std::vector<const PakEntry*>> children; // lower-case folder ("" = root)
} g_pak;

// The 256-bit pak key, computed (not stored as bytes in the exe).
static const uint32_t* PakKey()
{
    static uint32_t key[8];
    static std::once_flag once;
    std::call_once(once, []
    {
        uint32_t a = 0x3C6EF372u, b = 0xA54FF53Au, c = 0x510E527Fu, d = 0x9B05688Cu;
        for (int i = 0; i < 8; i++)
        {
            for (int r = 0; r < 7 + i; r++)
            {
                a += b; d ^= a; d = (d << 16) | (d >> 16);
                c += d; b ^= c; b = (b << 12) | (b >> 20);
                a += b ^ 0x5BE0CD19u; d ^= a; d = (d << 8) | (d >> 24);
                c += d; b ^= c; b = (b << 7) | (b >> 25);
            }
            key[i] = a ^ c ^ uint32_t(0x1F83D9ABu * (i + 3));
        }
    });
    return key;
}

static void PakCodeWith(const uint32_t nonce[3], uint8_t* data, size_t n, uint64_t pos)
{
    ChaCha20::Xor(PakKey(), nonce, pos, data, n);
}

static void PakCode(uint8_t* data, size_t n, uint64_t pos)
{
    PakCodeWith(g_pak.nonce, data, n, pos);
}

static std::string LowerAscii(std::string_view s)
{
    std::string r(s);
    for (auto& c : r) c = char(tolower(uint8_t(c)));
    return r;
}

static bool HostSeek(FILE* fp, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(fp, int64_t(offset), SEEK_SET) == 0;
#else
    return fseeko(fp, off_t(offset), SEEK_SET) == 0;
#endif
}

static bool MountPak(const std::filesystem::path& file, uint64_t base = 0)
{
    FILE* fp = HostOpen(file, "rb");
    if (!fp)
        return false;
    auto readAt = [&](uint64_t off, void* buf, size_t n) { return HostSeek(fp, base + off) && fread(buf, 1, n, fp) == n; };
    uint8_t header[36];
    uint32_t version = 0;
    bool ok = readAt(0, header, sizeof(header)) && memcmp(header, "TLPK", 4) == 0;
    if (ok)
        memcpy(&version, header + 4, 4);
    ok = ok && version == 2;
    uint64_t indexOffset = 0;
    uint32_t indexSize = 0;
    std::vector<uint8_t> index;
    if (ok)
    {
        memcpy(&indexOffset, header + 8, 8);
        memcpy(&indexSize, header + 16, 4);
        memcpy(g_pak.nonce, header + 24, 12);
        index.resize(indexSize);
        ok = indexSize >= 4 && readAt(indexOffset, index.data(), indexSize);
    }
    fclose(fp);
    if (!ok)
    {
        LOGFN_ERROR("[file] {} is not a valid game pak", PathToU8(file));
        return false;
    }
    PakCode(index.data(), index.size(), indexOffset);
    size_t p = 0;
    auto take = [&](void* dst, size_t n) { if (p + n > index.size()) return false; memcpy(dst, &index[p], n); p += n; return true; };
    uint32_t count = 0;
    take(&count, 4);
    g_pak.entries.clear();
    g_pak.children.clear();
    g_pak.entries[""] = PakEntry{ "", 0, 0, true };
    for (uint32_t i = 0; i < count; i++)
    {
        uint16_t len = 0;
        PakEntry e;
        if (!take(&len, 2) || p + len > index.size())
            break;
        e.name.assign(reinterpret_cast<const char*>(&index[p]), len);
        p += len;
        if (!take(&e.offset, 8) || !take(&e.size, 8))
            break;
        const std::string lower = LowerAscii(e.name);
        for (size_t slash = lower.find('/'); slash != std::string::npos; slash = lower.find('/', slash + 1))
            if (!g_pak.entries.count(lower.substr(0, slash)))
                g_pak.entries[lower.substr(0, slash)] = PakEntry{ e.name.substr(0, slash), 0, 0, true };
        g_pak.entries[lower] = e;
    }
    for (auto& [lower, e] : g_pak.entries)
    {
        if (lower.empty())
            continue;
        const size_t slash = lower.rfind('/');
        g_pak.children[slash == std::string::npos ? std::string() : lower.substr(0, slash)].push_back(&e);
    }
    g_pak.base = base;
    g_pak.file = file;
    g_pak.fileU8 = PathToU8(file);
    g_pak.mounted = true;
    LOGFN("[file] game pak {} mounted: {} files", PathToU8(file.filename()), count);
    return true;
}

static bool HostIsInPak(const std::filesystem::path& p)
{
    if (!g_pak.mounted)
        return false;
    const std::string u8 = PathToU8(p);
    return u8.starts_with(g_pak.fileU8) && (u8.size() == g_pak.fileU8.size() || u8[g_pak.fileU8.size()] == '/' || u8[g_pak.fileU8.size()] == '\\');
}

// The pak entry for a path inside the pak ("<pak>/music/x.ogg"), or nullptr.
static const PakEntry* PakLookup(const std::filesystem::path& p, std::string* relLower = nullptr)
{
    if (!HostIsInPak(p))
        return nullptr;
    const std::string u8 = PathToU8(p);
    std::string rel;
    for (size_t i = g_pak.fileU8.size(); i < u8.size(); i++)
    {
        const char c = u8[i] == '\\' ? '/' : char(tolower(uint8_t(u8[i])));
        if (c == '/' && (rel.empty() || rel.back() == '/'))
            continue;
        rel.push_back(c);
    }
    while (!rel.empty() && rel.back() == '/')
        rel.pop_back();
    if (relLower)
        *relLower = rel;
    auto it = g_pak.entries.find(rel);
    return it != g_pak.entries.end() ? &it->second : nullptr;
}

static uint64_t VirtualSizeOf(const std::filesystem::path& p, uint64_t realSize);

// exists / is_directory / size for a real file or a path inside the pak
static bool HostStat(const std::filesystem::path& p, bool* isDir, uint64_t* size)
{
    if (HostIsInPak(p))
    {
        const PakEntry* e = PakLookup(p);
        if (isDir) *isDir = e && e->isDirectory;
        if (size) *size = e && !e->isDirectory ? VirtualSizeOf(p, e->size) : 0;
        return e != nullptr;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(p, ec);
    const bool dir = exists && std::filesystem::is_directory(p, ec);
    if (isDir) *isDir = dir;
    if (size) *size = exists && !dir ? VirtualSizeOf(p, std::filesystem::file_size(p, ec)) : 0;
    return exists;
}

FileSystem::Reader::~Reader()
{
    if (m_fp)
        fclose(m_fp);
}

bool FileSystem::Reader::Open(const std::filesystem::path& path)
{
    if (m_fp) { fclose(m_fp); m_fp = nullptr; }
    if (HostIsInPak(path))
    {
        const PakEntry* e = PakLookup(path);
        if (!e || e->isDirectory)
            return false;
        m_fp = HostOpen(g_pak.file, "rb");
        m_base = e->offset;
        m_size = e->size;
        m_pak = true;
        return m_fp != nullptr;
    }
    m_fp = HostOpen(path, "rb");
    m_pak = false;
    m_base = 0;
    if (!m_fp)
        return false;
    std::error_code ec;
    m_size = std::filesystem::file_size(path, ec);
    return true;
}

size_t FileSystem::Reader::Read(uint64_t offset, void* buffer, size_t length)
{
    if (!m_fp || offset >= m_size)
        return 0;
    length = size_t(std::min<uint64_t>(length, m_size - offset));
    if (!HostSeek(m_fp, (m_pak ? g_pak.base : 0) + m_base + offset))
        return 0;
    const size_t n = GuestFread(buffer, length, m_fp);
    if (m_pak)
        PakCode(static_cast<uint8_t*>(buffer), n, m_base + offset);
    return n;
}

bool FileSystem::ReadHostFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    Reader r;
    if (!r.Open(path))
        return false;
    out.resize(size_t(r.Size()));
    return r.Read(0, out.data(), out.size()) == out.size();
}

std::vector<std::string> FileSystem::ListPakFolder(std::string_view folder)
{
    std::vector<std::string> names;
    if (!g_pak.mounted)
        return names;
    auto it = g_pak.children.find(LowerAscii(folder));
    if (it != g_pak.children.end())
        for (const PakEntry* e : it->second)
            if (!e->isDirectory)
                names.push_back(e->name.substr(e->name.rfind('/') + 1));
    return names;
}

bool FileSystem::PakMounted()
{
    return g_pak.mounted;
}

bool FileSystem::MountPakAt(const std::filesystem::path& file, uint64_t base)
{
    return MountPak(file, base);
}

// Step 27: writes a game pak (ForgottenSandsRecomp.exe --pack <out> <folder> <prefix> ...). Each folder's files
// are stored under <prefix>/ ("." = the pak root). Progress every 5 % on stdout and in the log.
bool FileSystem::BuildPak(const std::filesystem::path& out, const std::vector<std::pair<std::filesystem::path, std::string>>& sources)
{
    std::error_code ec;
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (const auto& [folder, prefix] : sources)
    {
        std::vector<std::filesystem::path> list;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec))
                list.push_back(it->path());
        std::sort(list.begin(), list.end());
        for (const auto& f : list)
        {
            std::string rel = PathToU8(std::filesystem::relative(f, folder, ec));
            std::replace(rel.begin(), rel.end(), '\\', '/');
            files.emplace_back(prefix == "." || prefix.empty() ? rel : prefix + "/" + rel, f);
        }
    }
    uint64_t total = 0;
    for (auto& [name, f] : files)
        total += std::filesystem::file_size(f, ec);
    auto say = [](const std::string& line) { printf("%s\n", line.c_str()); fflush(stdout); LOGFN("{}", line); };
    say(fmt::format("[pack] {} files, {:.1f} MB -> {}", files.size(), total / 1e6, PathToU8(out)));

    uint32_t nonce[3];
    {
        std::random_device rd;
        for (auto& n : nonce) n = rd();
    }
    const auto tmp = std::filesystem::path(out).concat(".tmp");
    FILE* o = HostOpen(tmp, "wb");
    if (!o)
    {
        say("[pack] cannot write " + PathToU8(tmp));
        return false;
    }
    std::vector<uint8_t> header(36, 0);
    fwrite(header.data(), 1, header.size(), o);
    uint64_t pos = header.size(), done = 0;
    int nextPct = 5;
    std::vector<uint8_t> index(4, 0);
    const uint32_t count = uint32_t(files.size());
    memcpy(index.data(), &count, 4);
    std::vector<uint8_t> buf(8 << 20);
    for (auto& [name, f] : files)
    {
        const uint64_t aligned = (pos + 15) & ~uint64_t(15);
        if (aligned != pos)
        {
            const uint8_t zero[16] = {};
            fwrite(zero, 1, size_t(aligned - pos), o);
            pos = aligned;
        }
        FILE* in = HostOpen(f, "rb");
        uint64_t size = 0;
        while (in)
        {
            const size_t n = fread(buf.data(), 1, buf.size(), in);
            if (n == 0)
                break;
            PakCodeWith(nonce, buf.data(), n, pos + size);
            fwrite(buf.data(), 1, n, o);
            size += n;
            done += n;
            while (total && done * 100 >= uint64_t(nextPct) * total && nextPct <= 100)
            {
                say(fmt::format("[pack] {:3}%  {:.0f} MB", nextPct, done / 1e6));
                nextPct += 5;
            }
        }
        if (in)
            fclose(in);
        const uint16_t len = uint16_t(name.size());
        index.insert(index.end(), reinterpret_cast<const uint8_t*>(&len), reinterpret_cast<const uint8_t*>(&len) + 2);
        index.insert(index.end(), name.begin(), name.end());
        index.insert(index.end(), reinterpret_cast<const uint8_t*>(&pos), reinterpret_cast<const uint8_t*>(&pos) + 8);
        index.insert(index.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + 8);
        pos += size;
    }
    const uint64_t indexOffset = (pos + 15) & ~uint64_t(15);
    if (indexOffset != pos)
    {
        const uint8_t zero[16] = {};
        fwrite(zero, 1, size_t(indexOffset - pos), o);
    }
    PakCodeWith(nonce, index.data(), index.size(), indexOffset);
    fwrite(index.data(), 1, index.size(), o);
    const uint32_t version = 2, indexSize = uint32_t(index.size());
    memcpy(header.data(), "TLPK", 4);
    memcpy(header.data() + 4, &version, 4);
    memcpy(header.data() + 8, &indexOffset, 8);
    memcpy(header.data() + 16, &indexSize, 4);
    memcpy(header.data() + 24, nonce, 12);
    HostSeek(o, 0);
    fwrite(header.data(), 1, header.size(), o);
    fclose(o);
    if (std::filesystem::exists(out, ec))
        std::filesystem::rename(out, std::filesystem::path(out).concat(".previous"), ec); // keep the old pak (never delete)
    std::filesystem::rename(tmp, out, ec);
    say(fmt::format("[pack] done: {} files, {:.1f} MB{}", count, std::filesystem::file_size(out, ec) / 1e6, ec ? " (rename failed: " + ec.message() + ")" : ""));
    return !ec;
}

static uint64_t VirtualSizeOf(const std::filesystem::path& p, uint64_t realSize) { return VirtualSize(p, realSize); }

void FileSystem::Init()
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(GetGamePath(), ec))
        MountPak(GetGamePath()); // Step 26
    std::filesystem::create_directories(GetCachePath(), ec);
    std::filesystem::create_directories(GetSavePath(), ec);
    std::filesystem::create_directories(GetUserPath() / "hdd", ec);

    std::string game = PathToU8(GetGamePath());
    g_devices["game:"] = game;
    g_devices["d:"] = game;
    g_devices["media:"] = game;
    g_devices["\\device\\cdrom0"] = game;
    g_devices["\\device\\harddisk0\\sysext"] = game;
    g_devices["cache:"] = PathToU8(GetCachePath());
    g_devices["\\device\\harddisk0\\cache0"] = PathToU8(GetCachePath());
    g_devices["\\device\\harddisk0\\cache1"] = PathToU8(GetCachePath());
    g_devices["\\device\\harddisk0\\partition1"] = PathToU8(GetUserPath() / "hdd");
    g_devices["hdd:"] = PathToU8(GetUserPath() / "hdd");
    // Before the game mounts its save container with XamContentCreateEx, SAVE:\ does not exist
    // on the console (Xenia: "SAVE: device not found"). Torchlight then keeps its built-in
    // defaults; when we used to give it a folder here it wrote PC defaults (800x600) to
    // local_settings.txt and read them back. TL_SAVE_UNMOUNTED=1 restores the old behaviour.
    if (getenv("TL_SAVE_UNMOUNTED"))
    {
        g_devices["save:"] = PathToU8(GetUserPath() / "save_unmounted");
        std::filesystem::create_directories(GetUserPath() / "save_unmounted", ec);
    }
}

void FileSystem::CreateSymbolicLink(std::string_view link, std::string_view target)
{
    std::string name = ToLower(link);
    if (name.starts_with("\\??\\"))
        name = name.substr(4);
    std::lock_guard lock(g_linkMutex);
    g_symlinks[name] = std::string(target);
}

void FileSystem::DeleteSymbolicLink(std::string_view link)
{
    std::string name = ToLower(link);
    if (name.starts_with("\\??\\"))
        name = name.substr(4);
    std::lock_guard lock(g_linkMutex);
    g_symlinks.erase(name);
}

// ---------------------------------------------------------------------------------------
// Step 9a: cached directory trees. Ogre looks for every resource folder in every resource location
// (game:\music\MEDIA\..., game:\programs\MEDIA\..., game:\RTShaderLib\MEDIA\..., GAME:\MEDIA\...), so a
// zone load did ~5000 NtOpenFile calls per second for paths that do not exist, each costing several
// host file-system calls (exists, directory scans for the case fix, overlay checks). The game folder
// never changes (read only) and the overlay only changes through our own writes, so both trees are
// listed once and lookups are answered from memory. The overlay listing is rebuilt after any write.

struct TreeEntry
{
    std::filesystem::path real; // host path with the real case
    bool isDirectory = false;
};

struct TreeIndex
{
    std::filesystem::path root;
    std::string rootU8;          // PathToU8(root), prefix for StatPath
    bool rootExists = false;
    uint64_t generation = 0;     // overlay: g_overlayGeneration when built
    bool built = false;
    std::unordered_map<std::string, TreeEntry> entries; // lower-case relative path with '/' -> entry
};

// Step 11: host paths the game asked to delete (we never delete; see NtSetInformationFile)
static std::mutex g_deleteRequestedMutex;
static std::unordered_set<std::string> g_deleteRequested;

static std::mutex g_treeMutex;
static TreeIndex g_gameTree, g_overlayTree;
static std::atomic<uint64_t> g_overlayGeneration{ 1 };
static bool TreeCacheEnabled()
{
    static const bool enabled = getenv("TL_NO_PATH_CACHE") == nullptr;
    return enabled;
}

static void InvalidateOverlayTree() { g_overlayGeneration.fetch_add(1); }

// "MEDIA/Sounds//x/" -> "media/sounds/x" (lower case, '/' separators, no empty or "." parts)
static std::string NormalizeRelative(std::string_view rel)
{
    std::string out;
    out.reserve(rel.size());
    size_t start = 0;
    while (start <= rel.size())
    {
        size_t end = rel.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = rel.size();
        std::string_view part = rel.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".")
        {
            if (end == rel.size()) break;
            continue;
        }
        if (!out.empty()) out.push_back('/');
        for (char c : part) out.push_back(char(tolower(uint8_t(c))));
        if (end == rel.size()) break;
    }
    return out;
}

static void BuildTree(TreeIndex& tree, const std::filesystem::path& root)
{
    tree.root = root;
    tree.rootU8 = PathToU8(root);
    tree.entries.clear();
    std::error_code ec;
    if (g_pak.mounted && root == g_pak.file)
    {
        // Step 26: the game pak's index is the tree
        tree.rootExists = true;
        tree.built = true;
        for (auto& [lower, e] : g_pak.entries)
            if (!lower.empty())
                tree.entries[lower] = TreeEntry{ root / U8Path(e.name), e.isDirectory };
        return;
    }
    tree.rootExists = std::filesystem::is_directory(root, ec);
    tree.built = true;
    if (!tree.rootExists)
        return;
    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
        !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code ec2;
        auto rel = PathToU8(std::filesystem::relative(it->path(), root, ec2));
        if (ec2)
            continue;
        tree.entries[NormalizeRelative(rel)] = TreeEntry{ it->path(), it->is_directory(ec2) };
    }
}

// Returns the tree for this root (built / refreshed as needed) or nullptr if the root is not cached.
// Caller holds g_treeMutex.
static TreeIndex* TreeFor(const std::filesystem::path& root)
{
    if (!TreeCacheEnabled())
        return nullptr;
    const auto gameRoot = GetGamePath();
    if (root == gameRoot)
    {
        if (!g_gameTree.built)
        {
            BuildTree(g_gameTree, gameRoot);
            LOGFN("[file] game folder listing cached: {} entries", g_gameTree.entries.size());
        }
        return &g_gameTree;
    }
    const auto overlayRoot = GetUserPath() / "game_overlay";
    if (root == overlayRoot)
    {
        const uint64_t gen = g_overlayGeneration.load();
        if (!g_overlayTree.built || g_overlayTree.generation != gen)
        {
            BuildTree(g_overlayTree, overlayRoot);
            g_overlayTree.generation = gen;
        }
        return &g_overlayTree;
    }
    return nullptr;
}

// exists / is_directory for a host path, answered from the cached trees when the path is inside one.
static bool StatPath(const std::filesystem::path& p, bool* isDir)
{
    if (isDir) *isDir = false;
    if (p.empty())
        return false;
    if (TreeCacheEnabled())
    {
        const std::string u8 = PathToU8(p);
        std::lock_guard lock(g_treeMutex);
        for (const std::filesystem::path& root : { GetGamePath(), GetUserPath() / "game_overlay" })
        {
            TreeIndex* tree = TreeFor(root);
            if (!tree || tree->rootU8.empty() || !u8.starts_with(tree->rootU8))
                continue;
            if (u8.size() != tree->rootU8.size() && u8[tree->rootU8.size()] != '/' && u8[tree->rootU8.size()] != '\\')
                continue;
            const std::string rel = NormalizeRelative(std::string_view(u8).substr(tree->rootU8.size()));
            if (rel.empty())
            {
                if (isDir) *isDir = tree->rootExists;
                return tree->rootExists;
            }
            auto it = tree->entries.find(rel);
            if (it == tree->entries.end())
                return false;
            if (isDir) *isDir = it->second.isDirectory;
            return true;
        }
    }
    return HostStat(p, isDir, nullptr);
}

// Case-insensitive lookup of each component (needed on Linux/Android; free on Windows).
static std::filesystem::path FixCase(const std::filesystem::path& root, const std::string& relative)
{
    std::filesystem::path result = root;
    std::error_code ec;

    size_t start = 0;
    while (start <= relative.size())
    {
        size_t end = relative.find('/', start);
        if (end == std::string::npos) end = relative.size();
        std::string part = relative.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".")
            continue;

        std::filesystem::path candidate = result / U8Path(part);
        if (!std::filesystem::exists(candidate, ec))
        {
            std::string lower = ToLower(part);
            for (auto& entry : std::filesystem::directory_iterator(result, ec))
            {
                if (ToLower(PathToU8(entry.path().filename())) == lower)
                {
                    candidate = entry.path();
                    break;
                }
            }
        }
        result = candidate;
        if (end == relative.size())
            break;
    }
    return result;
}

std::filesystem::path FileSystem::ResolvePath(std::string_view guestPath, bool* readOnly, std::filesystem::path* overlay)
{
    std::string path(guestPath);
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.starts_with("\\??\\"))
        path = path.substr(4);

    if (readOnly)
        *readOnly = false;

    // Relative paths ("TRANSLATIONS/TRANSLATION.DAT") are relative to the title's folder.
    if (path.find(':') == std::string::npos && !path.starts_with("\\"))
        path = "game:\\" + path;

    std::string lower = ToLower(path);
    std::string hostRoot;
    std::string rest;

    {
        std::lock_guard lock(g_linkMutex);

        // Symbolic links created by the game (ObCreateSymbolicLink), resolved once.
        for (int depth = 0; depth < 4; depth++)
        {
            bool replaced = false;
            for (auto& [link, target] : g_symlinks)
            {
                if (lower.starts_with(link) && (lower.size() == link.size() || lower[link.size()] == '\\'))
                {
                    path = target + path.substr(link.size());
                    lower = ToLower(path);
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                break;
        }

        // Content roots created through XamContentCreateEx ("save:" ...) win over the defaults.
        size_t colon = path.find(':');
        if (colon != std::string::npos && path.find('\\') > colon)
        {
            std::string_view contentRoot = XamGetRootPath(std::string_view(path).substr(0, colon));
            if (!contentRoot.empty())
            {
                hostRoot = std::string(contentRoot);
                rest = path.substr(colon + 1);
            }
        }

        // Longest matching device prefix.
        size_t best = 0;
        std::string bestRoot;
        for (auto& [device, host] : g_devices)
        {
            if (hostRoot.empty() && device.size() > best && lower.starts_with(device) &&
                (lower.size() == device.size() || lower[device.size()] == '\\' || device.back() == ':'))
            {
                best = device.size();
                bestRoot = host;
            }
        }

        if (best != 0)
        {
            rest = path.substr(best);
            hostRoot = bestRoot;
        }
    }

    if (hostRoot.empty())
    {
        // Content roots created through XamContentCreateEx ("save:", "dlc0:" ...)
        size_t colon = path.find(':');
        if (colon == std::string::npos)
            return {};

        std::string_view root = std::string_view(path).substr(0, colon);
        std::string_view contentRoot = XamGetRootPath(root);
        if (contentRoot.empty())
            return {};

        hostRoot = std::string(contentRoot);
        rest = path.substr(colon + 1);
    }

    const bool isGame = (hostRoot == PathToU8(GetGamePath()));
    if (readOnly)
        *readOnly = isGame;

    std::replace(rest.begin(), rest.end(), '\\', '/');
    while (!rest.empty() && rest.front() == '/')
        rest.erase(rest.begin());

    auto resolveUnder = [&](const std::filesystem::path& root) -> std::filesystem::path
    {
        if (rest.empty())
            return root;
        {
            // Step 9a: cached trees (game folder, overlay) answer without touching the disk.
            std::lock_guard lock(g_treeMutex);
            if (TreeIndex* tree = TreeFor(root))
            {
                auto it = tree->entries.find(NormalizeRelative(rest));
                return it != tree->entries.end() ? it->second.real : root / U8Path(rest);
            }
        }
        std::error_code ec;
        std::filesystem::path direct = root / U8Path(rest);
        if (std::filesystem::exists(direct, ec))
            return direct;
        return FixCase(root, rest);
    };

    // The game folder is never written to; writes go to <user>/game_overlay with the same layout.
    if (overlay)
        *overlay = isGame ? resolveUnder(GetUserPath() / "game_overlay") : std::filesystem::path();

    return resolveUnder(U8Path(hostRoot));
}

// ---------------------------------------------------------------------------------------
// File objects

struct DirEntry
{
    std::string name;
    uint64_t size;
    bool isDirectory;
    uint64_t writeTime;
};

struct FileHandle : KernelObject
{
    std::mutex mutex;
    FILE* fp = nullptr;
    std::filesystem::path path;
    std::string guestPath;
    std::filesystem::path mergeDir; // second directory listed by NtQueryDirectoryFile (game + overlay)
    bool isDirectory = false;
    bool writable = false;
    uint64_t position = 0;
    const VirtualTail* vtail = nullptr; // Step 7: reads past vtail->cut come from memory
    bool inPak = false;                 // Step 26: fp is the game pak, the file is [pakBase, pakBase + pakSize)
    uint64_t pakBase = 0, pakSize = 0;

    // NtQueryDirectoryFile state
    bool enumerated = false;
    std::vector<DirEntry> entries;
    size_t entryIndex = 0;

    ~FileHandle() override
    {
        if (fp)
            fclose(fp);
    }

    uint32_t Wait(uint32_t timeout) override
    {
        return X_STATUS_SUCCESS; // all I/O completes synchronously
    }
};

static int Seek(FILE* fp, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(fp, int64_t(offset), SEEK_SET);
#else
    return fseeko(fp, off_t(offset), SEEK_SET);
#endif
}

static uint64_t FileTimeOf(const std::filesystem::path& p)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec)
        return 0;
#ifdef _WIN32
    // MSVC file_clock is FILETIME (100ns since 1601) already.
    return uint64_t(t.time_since_epoch().count());
#else
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return uint64_t(std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(sys.time_since_epoch()).count()
        + 116444736000000000LL);
#endif
}

static FileHandle* GetFile(uint32_t handle)
{
    if (!IsKernelObject(handle))
        return nullptr;
    return dynamic_cast<FileHandle*>(GetKernelObject(handle));
}

static std::string GetGuestName(XOBJECT_ATTRIBUTES* attributes)
{
    if (!attributes || !attributes->Name)
        return {};

    XANSI_STRING* name = attributes->Name.get();
    std::string result(name->Buffer.get(), name->Length.get());

    // Relative to a directory handle
    if (attributes->RootDirectory != 0 && attributes->RootDirectory != 0xFFFFFFFD)
    {
        if (auto* dir = GetFile(attributes->RootDirectory))
        {
            std::string base = dir->guestPath;
            if (!base.empty() && base.back() != '\\')
                base.push_back('\\');
            result = base + result;
        }
    }

    return result;
}

static void SetIoStatus(XIO_STATUS_BLOCK* iosb, uint32_t status, uint32_t information)
{
    if (iosb)
    {
        iosb->Status = status;
        iosb->Information = information;
    }
}

constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
constexpr uint32_t FILE_DIRECTORY_FILE = 0x1, FILE_NON_DIRECTORY_FILE = 0x40;
constexpr uint32_t X_FILE_SUPERSEDED = 0, X_FILE_OPENED = 1, X_FILE_CREATED = 2, X_FILE_OVERWRITTEN = 3, X_FILE_EXISTS = 4, X_FILE_DOES_NOT_EXIST = 5;
constexpr uint32_t ACCESS_WRITE_MASK = 0x40000000 | 0x2 | 0x4 | 0x10000 | 0x100 | 0x10; // GENERIC_WRITE, WRITE_DATA, APPEND, DELETE, WRITE_ATTRIBUTES, WRITE_EA

static uint32_t OpenFileImpl(be<uint32_t>* handleOut, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* iosb,
    uint32_t disposition, uint32_t createOptions, const char* apiName)
{
    if (handleOut)
        *handleOut = 0;

    std::string guestName = GetGuestName(attributes);
    bool readOnly = false;
    std::filesystem::path overlayPath;
    std::filesystem::path hostPath = FileSystem::ResolvePath(guestName, &readOnly, &overlayPath);
    {
        // Bink movies: remembered so apu/bink_ffmpeg.cpp can decode the same file cleanly with FFmpeg.
        std::string ext = hostPath.extension().string();
        for (auto& ch : ext) ch = char(tolower(ch));
        if (ext == ".bik")
        {
            extern void BinkOverrideFileOpened(const std::filesystem::path& path);
            BinkOverrideFileOpened(hostPath);
        }
    }

    auto logResult = [&](uint32_t status, const char* what)
    {
        // Step 9a: "not found" probes (thousands per zone load) are only logged with verbose = true.
        static std::atomic<uint32_t> s_missesLogged;
        const bool miss = status == X_STATUS_OBJECT_NAME_NOT_FOUND || status == X_STATUS_OBJECT_PATH_NOT_FOUND;
        if (miss && !Config::Verbose && s_missesLogged.fetch_add(1) >= 100)
            return;
        if (Config::LogFileAccess)
            LOGFN("[file] {} \"{}\" -> {} (0x{:08X})", apiName, guestName, what, status);
    };

    if (hostPath.empty())
    {
        SetIoStatus(iosb, X_STATUS_OBJECT_PATH_NOT_FOUND, X_FILE_DOES_NOT_EXIST);
        logResult(X_STATUS_OBJECT_PATH_NOT_FOUND, "unknown device");
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    std::error_code ec;
    bool isDir = false;
    bool exists = StatPath(hostPath, &isDir);
    const bool wantsWrite = (desiredAccess & ACCESS_WRITE_MASK) != 0 ||
        disposition == FILE_SUPERSEDE || disposition == FILE_CREATE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF ||
        (disposition == FILE_OPEN_IF && !exists);

    // game:\ is read only: anything the game writes there lives in <user>/game_overlay.
    std::filesystem::path mergeDir; // game-folder directory merged into an overlay directory listing
    if (readOnly)
    {
        const bool overlayExists = StatPath(overlayPath, nullptr);
        if (overlayExists || (wantsWrite && !(exists && isDir)))
        {
            if (!overlayExists && exists && !isDir && disposition != FILE_SUPERSEDE && disposition != FILE_OVERWRITE && disposition != FILE_OVERWRITE_IF)
            {
                // Copy-on-write of an existing game file (the original is only read).
                std::filesystem::create_directories(overlayPath.parent_path(), ec);
                if (HostIsInPak(hostPath))
                {
                    std::vector<uint8_t> data;
                    if (FileSystem::ReadHostFile(hostPath, data))
                        if (FILE* out = HostOpen(overlayPath, "wb")) { fwrite(data.data(), 1, data.size(), out); fclose(out); }
                }
                else
                    std::filesystem::copy_file(hostPath, overlayPath, ec);
                InvalidateOverlayTree();
            }
            if (exists && isDir)
                mergeDir = hostPath;
            hostPath = overlayPath;
            readOnly = false;
            exists = StatPath(hostPath, &isDir);
        }
        else if (exists && isDir)
        {
            mergeDir = overlayPath; // may not exist; listing skips it then
        }
    }

    uint32_t information = X_FILE_OPENED;

    if (exists)
    {
        if (disposition == FILE_CREATE)
        {
            SetIoStatus(iosb, X_STATUS_OBJECT_NAME_COLLISION, X_FILE_EXISTS);
            logResult(X_STATUS_OBJECT_NAME_COLLISION, "already exists");
            return X_STATUS_OBJECT_NAME_COLLISION;
        }
        if (isDir && (createOptions & FILE_NON_DIRECTORY_FILE))
        {
            SetIoStatus(iosb, X_STATUS_FILE_IS_A_DIRECTORY, 0);
            logResult(X_STATUS_FILE_IS_A_DIRECTORY, "is a directory");
            return X_STATUS_FILE_IS_A_DIRECTORY;
        }
        if (!isDir && (createOptions & FILE_DIRECTORY_FILE))
        {
            SetIoStatus(iosb, X_STATUS_NOT_A_DIRECTORY, 0);
            logResult(X_STATUS_NOT_A_DIRECTORY, "not a directory");
            return X_STATUS_NOT_A_DIRECTORY;
        }
        if (disposition == FILE_SUPERSEDE) information = X_FILE_SUPERSEDED;
        else if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF) information = X_FILE_OVERWRITTEN;
    }
    else
    {
        if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)
        {
            uint32_t status = StatPath(hostPath.parent_path(), nullptr) ? X_STATUS_OBJECT_NAME_NOT_FOUND : X_STATUS_OBJECT_PATH_NOT_FOUND;
            SetIoStatus(iosb, status, X_FILE_DOES_NOT_EXIST);
            logResult(status, "not found");
            return status;
        }
        information = X_FILE_CREATED;
    }

    auto* file = CreateKernelObject<FileHandle>();
    file->path = hostPath;
    file->guestPath = guestName;
    file->mergeDir = mergeDir;
    file->writable = wantsWrite && !readOnly;

    bool createDir = !exists && (createOptions & FILE_DIRECTORY_FILE);
    if (isDir || createDir)
    {
        if (createDir)
            std::filesystem::create_directories(hostPath, ec);
        file->isDirectory = true;
    }
    else
    {
        const char* mode = "rb";
        if (file->writable)
            mode = (information == X_FILE_OPENED) ? "r+b" : "w+b";

        if (information == X_FILE_CREATED)
            std::filesystem::create_directories(hostPath.parent_path(), ec);

        if (const PakEntry* pe = file->writable ? nullptr : PakLookup(hostPath))
        {
            file->fp = HostOpen(g_pak.file, "rb");
            file->inPak = true;
            file->pakBase = pe->offset;
            file->pakSize = pe->size;
        }
        else
            file->fp = HostOpen(hostPath, mode);
        if (!file->writable)
            file->vtail = FindVirtualTail(hostPath);
        if (!file->fp)
        {
            DestroyKernelObject(file);
            SetIoStatus(iosb, X_STATUS_ACCESS_DENIED, 0);
            logResult(X_STATUS_ACCESS_DENIED, "host open failed");
            return X_STATUS_ACCESS_DENIED;
        }
    }

    if (file->writable || createDir)
        InvalidateOverlayTree(); // something may have been created

    *handleOut = GetKernelHandle(file);
    SetIoStatus(iosb, X_STATUS_SUCCESS, information);
    logResult(X_STATUS_SUCCESS, file->isDirectory ? "directory" : (information == X_FILE_CREATED ? "created" : "opened"));
    return X_STATUS_SUCCESS;
}

uint32_t NtCreateFile(be<uint32_t>* FileHandleOut, uint32_t DesiredAccess, XOBJECT_ATTRIBUTES* Attributes, XIO_STATUS_BLOCK* IoStatusBlock,
    be<uint64_t>* AllocationSize, uint32_t FileAttributes, uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions)
{
    const uint64_t t0 = Perf::NowNs();
    const uint32_t status = OpenFileImpl(FileHandleOut, DesiredAccess, Attributes, IoStatusBlock, CreateDisposition, CreateOptions, "NtCreateFile");
    Perf::Add(Perf::FileOpen, Perf::NowNs() - t0);
    if (status == X_STATUS_OBJECT_NAME_NOT_FOUND || status == X_STATUS_OBJECT_PATH_NOT_FOUND)
        Perf::Add(Perf::FileOpenMissing, 0);
    return status;
}

uint32_t NtOpenFile(be<uint32_t>* FileHandleOut, uint32_t DesiredAccess, XOBJECT_ATTRIBUTES* Attributes, XIO_STATUS_BLOCK* IoStatusBlock,
    uint32_t ShareAccess, uint32_t OpenOptions)
{
    const uint64_t t0 = Perf::NowNs();
    const uint32_t status = OpenFileImpl(FileHandleOut, DesiredAccess, Attributes, IoStatusBlock, FILE_OPEN, OpenOptions, "NtOpenFile");
    Perf::Add(Perf::FileOpen, Perf::NowNs() - t0);
    if (status == X_STATUS_OBJECT_NAME_NOT_FOUND || status == X_STATUS_OBJECT_PATH_NOT_FOUND)
        Perf::Add(Perf::FileOpenMissing, 0);
    return status;
}

static void CompleteAsync(uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* iosb)
{
    if (event)
    {
        if (auto* ev = dynamic_cast<Event*>(ResolveWaitable(event)))
            ev->Set();
    }

    if (apcRoutine)
    {
        LOG_ONCE("File I/O completion APC 0x{:08X} called synchronously", apcRoutine);
        GuestToHostFunction<void>(apcRoutine, apcContext, iosb, 0u);
    }
}

static uint64_t ResolveOffset(FileHandle* file, be<uint64_t>* byteOffset)
{
    if (byteOffset)
    {
        uint64_t off = *byteOffset;
        // FILE_USE_FILE_POINTER_POSITION (-2 as LARGE_INTEGER) / FILE_WRITE_TO_END_OF_FILE (-1)
        if (off != 0xFFFFFFFFFFFFFFFEull && off != 0xFFFFFFFFFFFFFFFFull && (off >> 32) != 0xFFFFFFFF)
            return off;
    }
    return file->position;
}

// Reads from the host file, or from its virtual tail past vtail->cut.
static size_t RawReadAt(FileHandle* file, uint64_t offset, void* buffer, size_t length)
{
    if (file->inPak)
    {
        if (offset >= file->pakSize)
            return 0;
        length = size_t(std::min<uint64_t>(length, file->pakSize - offset));
        Seek(file->fp, g_pak.base + file->pakBase + offset);
        const size_t n = length ? GuestFread(buffer, length, file->fp) : 0;
        PakCode(static_cast<uint8_t*>(buffer), n, file->pakBase + offset);
        return n;
    }
    Seek(file->fp, offset);
    return length ? GuestFread(buffer, length, file->fp) : 0;
}

static size_t ReadAt(FileHandle* file, uint64_t offset, void* buffer, size_t length)
{
    if (file->vtail == nullptr)
        return RawReadAt(file, offset, buffer, length);
    const auto* v = file->vtail;
    size_t done = 0;
    auto* out = static_cast<uint8_t*>(buffer);
    if (offset < v->cut)
    {
        const size_t n = size_t(std::min<uint64_t>(length, v->cut - offset));
        done = RawReadAt(file, offset, out, n);
        if (done < n)
            return done;
    }
    const uint64_t pos = offset + done;
    if (done < length && pos >= v->cut && pos - v->cut < v->tail.size())
    {
        const size_t n = size_t(std::min<uint64_t>(length - done, v->tail.size() - (pos - v->cut)));
        memcpy(out + done, v->tail.data() + (pos - v->cut), n);
        done += n;
    }
    return done;
}

uint32_t NtReadFile(uint32_t FileHandleIn, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, XIO_STATUS_BLOCK* IoStatusBlock,
    void* Buffer, uint32_t Length, be<uint64_t>* ByteOffset)
{
    auto* file = GetFile(FileHandleIn);
    if (!file || !file->fp)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    uint32_t status;
    size_t read = 0;
    {
        Perf::Scope perf(Perf::FileRead);
        std::lock_guard lock(file->mutex);
        uint64_t offset = ResolveOffset(file, ByteOffset);
        read = ReadAt(file, offset, Buffer, Length);
        perf.bytes = read;
        file->position = offset + read;
        status = (read == 0 && Length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    }

    if (Config::Verbose)
        LOGFN("[file] read {} bytes from \"{}\"", read, file->guestPath);
    static const bool ioTrace = getenv("FS_IO_TRACE") != nullptr; // debug: every read with its guest buffer
    if (ioTrace)
    {
        extern std::string CurrentGuestCallChain(int maxFrames);
        LOGFN("[io] read 0x{:08X} +0x{:X} at 0x{:X} \"{}\" thread {} |{}", g_memory.MapVirtual(Buffer), read, file->position - read, file->guestPath, GetCurrentThreadId(), CurrentGuestCallChain(8));
    }
    // Step 10 debug: TL_PAK_TRACE=1 logs every read of pak.zip (offset, length) - map offsets to zip entries
    // with work\playthrough\pakmap.py to see which game files a load reads.
    static const bool pakTrace = getenv("TL_PAK_TRACE") != nullptr;
    if (pakTrace && file->guestPath.find("pak.zip") != std::string::npos)
        LOGFN("[pak] read 0x{:X} +{}", file->position - read, read);

    SetIoStatus(IoStatusBlock, status, uint32_t(read));
    CompleteAsync(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return status;
}

uint32_t NtReadFileScatter(uint32_t FileHandleIn, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, XIO_STATUS_BLOCK* IoStatusBlock,
    be<uint32_t>* SegmentArray, uint32_t Length, be<uint64_t>* ByteOffset)
{
    auto* file = GetFile(FileHandleIn);
    if (!file || !file->fp)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    size_t total = 0;
    {
        std::lock_guard lock(file->mutex);
        uint64_t offset = ResolveOffset(file, ByteOffset);
        for (uint32_t i = 0; total < Length; i++)
        {
            uint32_t chunk = std::min<uint32_t>(0x1000, Length - uint32_t(total));
            size_t got = ReadAt(file, offset + total, g_memory.Translate(SegmentArray[i]), chunk);
            total += got;
            if (got < chunk)
                break;
        }
        file->position = offset + total;
    }

    static const bool ioTrace = getenv("FS_IO_TRACE") != nullptr;
    if (ioTrace)
        LOGFN("[io] scatter read first page 0x{:08X} +0x{:X} at 0x{:X} \"{}\"", uint32_t(SegmentArray[0]), total, file->position - total, file->guestPath);
    uint32_t status = (total == 0 && Length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    SetIoStatus(IoStatusBlock, status, uint32_t(total));
    CompleteAsync(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return status;
}

uint32_t NtWriteFile(uint32_t FileHandleIn, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, XIO_STATUS_BLOCK* IoStatusBlock,
    const void* Buffer, uint32_t Length, be<uint64_t>* ByteOffset)
{
    auto* file = GetFile(FileHandleIn);
    if (!file || !file->fp)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    if (!file->writable)
    {
        LOG_ONCE("NtWriteFile denied on read-only file \"{}\"", file->guestPath);
        SetIoStatus(IoStatusBlock, X_STATUS_ACCESS_DENIED, 0);
        return X_STATUS_ACCESS_DENIED;
    }

    size_t written;
    {
        std::lock_guard lock(file->mutex);
        uint64_t offset = ResolveOffset(file, ByteOffset);
        if (ByteOffset && *ByteOffset == 0xFFFFFFFFFFFFFFFFull) // append
        {
            std::error_code ec;
            offset = std::filesystem::file_size(file->path, ec);
        }
        Seek(file->fp, offset);
        written = fwrite(Buffer, 1, Length, file->fp);
        fflush(file->fp);
        file->position = offset + written;
    }

    SetIoStatus(IoStatusBlock, X_STATUS_SUCCESS, uint32_t(written));
    CompleteAsync(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return X_STATUS_SUCCESS;
}

uint32_t NtFlushBuffersFile(uint32_t FileHandleIn, XIO_STATUS_BLOCK* IoStatusBlock)
{
    if (auto* file = GetFile(FileHandleIn); file && file->fp)
        fflush(file->fp);
    SetIoStatus(IoStatusBlock, X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Information classes

struct X_FILE_NETWORK_OPEN_INFORMATION
{
    be<uint64_t> CreationTime;
    be<uint64_t> LastAccessTime;
    be<uint64_t> LastWriteTime;
    be<uint64_t> ChangeTime;
    be<uint64_t> AllocationSize;
    be<uint64_t> EndOfFile;
    be<uint32_t> FileAttributes;
    be<uint32_t> Pad;
};
static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 0x38);

struct X_FILE_BASIC_INFORMATION
{
    be<uint64_t> CreationTime;
    be<uint64_t> LastAccessTime;
    be<uint64_t> LastWriteTime;
    be<uint64_t> ChangeTime;
    be<uint32_t> FileAttributes;
    be<uint32_t> Pad;
};

struct X_FILE_STANDARD_INFORMATION
{
    be<uint64_t> AllocationSize;
    be<uint64_t> EndOfFile;
    be<uint32_t> NumberOfLinks;
    uint8_t DeletePending;
    uint8_t Directory;
    uint8_t Pad[2];
};
static_assert(sizeof(X_FILE_STANDARD_INFORMATION) == 0x18);

struct X_FILE_DIRECTORY_INFORMATION
{
    be<uint32_t> NextEntryOffset;
    be<uint32_t> FileIndex;
    be<uint64_t> CreationTime;
    be<uint64_t> LastAccessTime;
    be<uint64_t> LastWriteTime;
    be<uint64_t> ChangeTime;
    be<uint64_t> EndOfFile;
    be<uint64_t> AllocationSize;
    be<uint32_t> FileAttributes;
    be<uint32_t> FileNameLength;
    char FileName[1];
};
static_assert(offsetof(X_FILE_DIRECTORY_INFORMATION, FileName) == 0x40);

constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x01;
constexpr uint32_t X_FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t X_FILE_ATTRIBUTE_NORMAL = 0x80;

static void FillNetworkInfo(X_FILE_NETWORK_OPEN_INFORMATION* info, const std::filesystem::path& p)
{
    bool dir = false;
    uint64_t size = 0;
    HostStat(p, &dir, &size);
    uint64_t t = FileTimeOf(HostIsInPak(p) ? g_pak.file : p);
    info->CreationTime = t;
    info->LastAccessTime = t;
    info->LastWriteTime = t;
    info->ChangeTime = t;
    info->AllocationSize = RoundUp<uint64_t>(size, 0x1000);
    info->EndOfFile = size;
    info->FileAttributes = dir ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
    info->Pad = 0;
}

uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* Attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    Perf::Scope perf(Perf::FileQuery);
    std::string guestName = GetGuestName(Attributes);
    std::filesystem::path overlayPath;
    std::filesystem::path hostPath = FileSystem::ResolvePath(guestName, nullptr, &overlayPath);
    std::error_code ec;

    if (!overlayPath.empty() && std::filesystem::exists(overlayPath, ec))
        hostPath = overlayPath;

    if (hostPath.empty() || !HostStat(hostPath, nullptr, nullptr))
    {
        if (Config::LogFileAccess)
            LOGFN("[file] NtQueryFullAttributesFile \"{}\" -> not found", guestName);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }

    FillNetworkInfo(info, hostPath);
    if (Config::LogFileAccess)
        LOGFN("[file] NtQueryFullAttributesFile \"{}\" -> size {}", guestName, info->EndOfFile.get());
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryInformationFile(uint32_t FileHandleIn, XIO_STATUS_BLOCK* IoStatusBlock, void* FileInformation, uint32_t Length, uint32_t FileInformationClass)
{
    auto* file = GetFile(FileHandleIn);
    if (!file)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    std::error_code ec;
    uint64_t size = file->isDirectory ? 0 : file->inPak ? file->pakSize : std::filesystem::file_size(file->path, ec);
    if (file->vtail)
        size = file->vtail->cut + file->vtail->tail.size();
    uint32_t written = 0;

    switch (FileInformationClass)
    {
    case 4: // FileBasicInformation
    {
        auto* info = (X_FILE_BASIC_INFORMATION*)FileInformation;
        uint64_t t = FileTimeOf(file->inPak || HostIsInPak(file->path) ? g_pak.file : file->path);
        info->CreationTime = t; info->LastAccessTime = t; info->LastWriteTime = t; info->ChangeTime = t;
        info->FileAttributes = file->isDirectory ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
        info->Pad = 0;
        written = sizeof(X_FILE_BASIC_INFORMATION);
        break;
    }
    case 5: // FileStandardInformation
    {
        auto* info = (X_FILE_STANDARD_INFORMATION*)FileInformation;
        memset(info, 0, sizeof(*info));
        info->AllocationSize = RoundUp<uint64_t>(size, 0x1000);
        info->EndOfFile = size;
        info->NumberOfLinks = 1;
        info->Directory = file->isDirectory;
        written = sizeof(X_FILE_STANDARD_INFORMATION);
        break;
    }
    case 6: // FileInternalInformation
        *(be<uint64_t>*)FileInformation = XXH64(file->guestPath.data(), file->guestPath.size(), 0);
        written = 8;
        break;
    case 14: // FilePositionInformation
        *(be<uint64_t>*)FileInformation = file->position;
        written = 8;
        break;
    case 17: // FileAlignmentInformation
        *(be<uint32_t>*)FileInformation = 0;
        written = 4;
        break;
    case 20: // FileEndOfFileInformation
        *(be<uint64_t>*)FileInformation = size;
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        FillNetworkInfo((X_FILE_NETWORK_OPEN_INFORMATION*)FileInformation, file->path);
        written = sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
        break;
    case 26: // XFileSectorInformation (Forgotten Sands asks for it on every .forge): the disc sector
             // the file starts at. There is no disc, so give each file a stable fake sector number.
    {
        uint64_t sector = XXH64(file->guestPath.data(), file->guestPath.size(), 0) & 0xFFFFF;
        memset(FileInformation, 0, Length);
        if (Length >= 8) *(be<uint64_t>*)FileInformation = sector;
        else if (Length >= 4) *(be<uint32_t>*)FileInformation = uint32_t(sector);
        written = std::min<uint32_t>(Length, 8);
        LOG_ONCE("NtQueryInformationFile: XFileSectorInformation (length {}) answered with a fake sector", Length);
        break;
    }
    default:
        LOGFN_WARNING("NtQueryInformationFile: unsupported class {} on \"{}\"", FileInformationClass, file->guestPath);
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_PARAMETER, 0);
        return X_STATUS_INVALID_PARAMETER;
    }

    SetIoStatus(IoStatusBlock, X_STATUS_SUCCESS, written);
    return X_STATUS_SUCCESS;
}

uint32_t NtSetInformationFile(uint32_t FileHandleIn, XIO_STATUS_BLOCK* IoStatusBlock, void* FileInformation, uint32_t Length, uint32_t FileInformationClass)
{
    auto* file = GetFile(FileHandleIn);
    if (!file)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    switch (FileInformationClass)
    {
    case 4: // FileBasicInformation (times/attributes) - ignored
        break;
    case 13: // FileDispositionInformation (delete on close) - never delete anything
        LOGFN_WARNING("NtSetInformationFile: game asked to delete \"{}\" (ignored)", file->guestPath);
        {
            // Step 11: remembered so a following rename onto this name may replace it (see case 10)
            std::lock_guard lock(g_deleteRequestedMutex);
            g_deleteRequested.insert(ToLower(PathToU8(file->path)));
        }
        break;
    case 14: // FilePositionInformation
        file->position = *(be<uint64_t>*)FileInformation;
        break;
    case 10: // FileRenameInformation {ReplaceIfExists, RootDirectory, ANSI_STRING FileName} - used for saves (save.tmp -> real name)
    {
        // Step 11: ReplaceIfExists is a BOOLEAN (one byte, then 3 padding bytes). It was read as a 32-bit
        // value, so the game's CRT rename() (MoveFileA, which only writes that byte = FALSE) "replaced"
        // depending on stack garbage in the padding - saves only worked by luck.
        struct XFILE_RENAME_INFORMATION { uint8_t ReplaceIfExists; uint8_t pad[3]; be<uint32_t> RootDirectory; XANSI_STRING FileName; };
        auto* info = (XFILE_RENAME_INFORMATION*)FileInformation;
        std::string target(info->FileName.Buffer.get(), info->FileName.Length.get());
        if (info->RootDirectory != 0 && info->RootDirectory != 0xFFFFFFFD)
            if (auto* dir = GetFile(info->RootDirectory))
                target = dir->guestPath + (dir->guestPath.empty() || dir->guestPath.back() == '\\' ? "" : "\\") + target;
        // A bare name renames inside the same folder.
        if (target.find(':') == std::string::npos && target.find('\\') == std::string::npos)
        {
            auto slash = file->guestPath.find_last_of("\\/");
            target = (slash == std::string::npos ? std::string() : file->guestPath.substr(0, slash + 1)) + target;
        }

        bool readOnly = false;
        std::filesystem::path overlay;
        std::filesystem::path newPath = FileSystem::ResolvePath(target, &readOnly, &overlay);
        if (readOnly)
            newPath = overlay; // game:\ writes go to the overlay
        std::error_code ec;
        // The game's save code deletes backup.tmp and then renames 0.TSV onto it (and save.tmp onto 0.TSV).
        // We never delete files, so the target still exists: replacing a file the game asked to delete is
        // what the game intended (the same end result as on the console).
        bool replace = info->ReplaceIfExists != 0;
        if (!replace && !newPath.empty())
        {
            std::lock_guard lock(g_deleteRequestedMutex);
            replace = g_deleteRequested.erase(ToLower(PathToU8(newPath))) != 0;
        }
        if (newPath.empty() || (!replace && std::filesystem::exists(newPath, ec)))
        {
            LOGFN_WARNING("NtSetInformationFile: rename \"{}\" -> \"{}\" refused", file->guestPath, target);
            SetIoStatus(IoStatusBlock, X_STATUS_OBJECT_NAME_COLLISION, 0);
            return X_STATUS_OBJECT_NAME_COLLISION;
        }

        std::lock_guard lock(file->mutex);
        const bool wasOpen = file->fp != nullptr;
        if (wasOpen)
        {
            fclose(file->fp); // Windows cannot rename an open file
            file->fp = nullptr;
        }
        std::filesystem::create_directories(newPath.parent_path(), ec);
        std::filesystem::rename(file->path, newPath, ec);
        InvalidateOverlayTree();
        if (ec && replace)
        {
            // The game believes this file has been moved away (second save of a session: 0.TSV -> backup.tmp
            // fails while backup.tmp is still open), so its next rename onto this name may replace it.
            std::lock_guard lock(g_deleteRequestedMutex);
            g_deleteRequested.insert(ToLower(PathToU8(file->path)));
        }
        if (ec)
            LOGFN_ERROR("NtSetInformationFile: rename {} -> {} failed: {}", (const char*)file->path.u8string().c_str(), (const char*)newPath.u8string().c_str(), ec.message());
        else
        {
            LOGFN("[file] renamed \"{}\" -> \"{}\"", file->guestPath, target);
            file->path = newPath;
            file->guestPath = target;
        }
        if (wasOpen)
        {
            file->fp = HostOpen(file->path, file->writable ? "r+b" : "rb");
        }
        if (ec)
        {
            SetIoStatus(IoStatusBlock, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }
        break;
    }
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
        if (file->writable && file->fp)
        {
            std::lock_guard lock(file->mutex);
            fflush(file->fp);
            std::error_code ec;
            std::filesystem::resize_file(file->path, *(be<uint64_t>*)FileInformation, ec);
        }
        break;
    default:
        LOGFN_WARNING("NtSetInformationFile: unsupported class {} on \"{}\"", FileInformationClass, file->guestPath);
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_PARAMETER, 0);
        return X_STATUS_INVALID_PARAMETER;
    }

    SetIoStatus(IoStatusBlock, X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryVolumeInformationFile(uint32_t FileHandleIn, XIO_STATUS_BLOCK* IoStatusBlock, void* FsInformation, uint32_t Length, uint32_t FsInformationClass)
{
    auto* file = GetFile(FileHandleIn);
    if (!file)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    uint8_t* out = (uint8_t*)FsInformation;
    uint32_t written = 0;
    memset(out, 0, Length);

    switch (FsInformationClass)
    {
    case 1: // FileFsVolumeInformation
    {
        const char label[] = "POPFS";
        *(be<uint64_t>*)(out + 0) = 0;
        *(be<uint32_t>*)(out + 8) = 0x12345678;
        *(be<uint32_t>*)(out + 12) = sizeof(label) - 1;
        out[16] = 0;
        memcpy(out + 17, label, std::min<size_t>(sizeof(label) - 1, Length > 17 ? Length - 17 : 0));
        written = 17 + sizeof(label) - 1;
        break;
    }
    case 3: // FileFsSizeInformation
        *(be<uint64_t>*)(out + 0) = 0x100000;  // total allocation units
        *(be<uint64_t>*)(out + 8) = 0x80000;   // available
        *(be<uint32_t>*)(out + 16) = 32;       // sectors per unit
        *(be<uint32_t>*)(out + 20) = 512;      // bytes per sector
        written = 24;
        break;
    case 4: // FileFsDeviceInformation
        *(be<uint32_t>*)(out + 0) = 0x24;      // FILE_DEVICE_DISK? (any value works)
        *(be<uint32_t>*)(out + 4) = 0;
        written = 8;
        break;
    case 5: // FileFsAttributeInformation
    {
        const char name[] = "FATX";
        *(be<uint32_t>*)(out + 0) = 0x2;       // FILE_CASE_PRESERVED_NAMES
        *(be<uint32_t>*)(out + 4) = 255;
        *(be<uint32_t>*)(out + 8) = sizeof(name) - 1;
        memcpy(out + 12, name, sizeof(name) - 1);
        written = 12 + sizeof(name) - 1;
        break;
    }
    case 7: // FileFsFullSizeInformation
        *(be<uint64_t>*)(out + 0) = 0x100000;
        *(be<uint64_t>*)(out + 8) = 0x80000;
        *(be<uint64_t>*)(out + 16) = 0x80000;
        *(be<uint32_t>*)(out + 24) = 32;
        *(be<uint32_t>*)(out + 28) = 512;
        written = 32;
        break;
    default:
        LOGFN_WARNING("NtQueryVolumeInformationFile: unsupported class {}", FsInformationClass);
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_PARAMETER, 0);
        return X_STATUS_INVALID_PARAMETER;
    }

    SetIoStatus(IoStatusBlock, X_STATUS_SUCCESS, written);
    return X_STATUS_SUCCESS;
}

static bool WildcardMatch(const char* pattern, const char* name)
{
    while (*pattern)
    {
        if (*pattern == '*')
        {
            pattern++;
            if (!*pattern) return true;
            for (; *name; name++)
                if (WildcardMatch(pattern, name)) return true;
            return WildcardMatch(pattern, name);
        }
        if (!*name) return false;
        if (*pattern != '?' && tolower(uint8_t(*pattern)) != tolower(uint8_t(*name)))
            return false;
        pattern++;
        name++;
    }
    return *name == 0;
}

uint32_t NtQueryDirectoryFile(uint32_t FileHandleIn, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, XIO_STATUS_BLOCK* IoStatusBlock,
    X_FILE_DIRECTORY_INFORMATION* FileInformation, uint32_t Length, XANSI_STRING* FileMask, uint32_t RestartScan)
{
    Perf::Scope perf(Perf::FileQuery);
    auto* file = GetFile(FileHandleIn);
    if (!file || !file->isDirectory)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_INVALID_HANDLE, 0);
        return X_STATUS_INVALID_HANDLE;
    }

    std::lock_guard lock(file->mutex);

    if (!file->enumerated || RestartScan)
    {
        std::string mask = "*";
        if (FileMask && FileMask->Buffer && FileMask->Length != 0)
            mask.assign(FileMask->Buffer.get(), FileMask->Length.get());
        if (mask == "*.*")
            mask = "*";

        file->entries.clear();
        file->entryIndex = 0;
        std::error_code ec;
        std::unordered_set<std::string> seen;
        for (const auto& dirPath : { file->path, file->mergeDir })
        {
            if (dirPath.empty())
                continue;
            if (HostIsInPak(dirPath))
            {
                // Step 26: a folder inside the game pak
                std::string relLower;
                const PakEntry* d = PakLookup(dirPath, &relLower);
                auto it = d && d->isDirectory ? g_pak.children.find(relLower) : g_pak.children.end();
                if (it == g_pak.children.end())
                    continue;
                const uint64_t t = FileTimeOf(g_pak.file);
                for (const PakEntry* e : it->second)
                {
                    const std::string name = e->name.substr(e->name.rfind('/') + 1);
                    if (!WildcardMatch(mask.c_str(), name.c_str()) || !seen.insert(ToLower(name)).second)
                        continue;
                    file->entries.push_back({ name, e->isDirectory ? 0 : VirtualSize(dirPath / U8Path(name), e->size), e->isDirectory, t });
                }
                continue;
            }
            for (auto& entry : std::filesystem::directory_iterator(dirPath, ec))
            {
                std::string name = PathToU8(entry.path().filename());
                if (!WildcardMatch(mask.c_str(), name.c_str()) || !seen.insert(ToLower(name)).second)
                    continue;
                bool dir = entry.is_directory(ec);
                file->entries.push_back({ name, dir ? 0 : VirtualSize(entry.path(), entry.file_size(ec)), dir, FileTimeOf(entry.path()) });
            }
        }
        file->enumerated = true;

        if (Config::LogFileAccess)
            LOGFN("[file] NtQueryDirectoryFile \"{}\" mask \"{}\" -> {} entries", file->guestPath, mask, file->entries.size());

        if (file->entries.empty())
        {
            SetIoStatus(IoStatusBlock, X_STATUS_NO_SUCH_FILE, 0);
            return X_STATUS_NO_SUCH_FILE;
        }
    }

    if (file->entryIndex >= file->entries.size())
    {
        SetIoStatus(IoStatusBlock, X_STATUS_NO_MORE_FILES, 0);
        return X_STATUS_NO_MORE_FILES;
    }

    const auto& e = file->entries[file->entryIndex];
    uint32_t needed = uint32_t(offsetof(X_FILE_DIRECTORY_INFORMATION, FileName) + e.name.size());
    if (Length < needed)
    {
        SetIoStatus(IoStatusBlock, X_STATUS_BUFFER_OVERFLOW, 0);
        return X_STATUS_BUFFER_OVERFLOW;
    }

    memset(FileInformation, 0, offsetof(X_FILE_DIRECTORY_INFORMATION, FileName));
    FileInformation->NextEntryOffset = 0;
    FileInformation->FileIndex = uint32_t(file->entryIndex);
    FileInformation->CreationTime = e.writeTime;
    FileInformation->LastAccessTime = e.writeTime;
    FileInformation->LastWriteTime = e.writeTime;
    FileInformation->ChangeTime = e.writeTime;
    FileInformation->EndOfFile = e.size;
    FileInformation->AllocationSize = RoundUp<uint64_t>(e.size, 0x1000);
    FileInformation->FileAttributes = e.isDirectory ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
    FileInformation->FileNameLength = uint32_t(e.name.size());
    memcpy(FileInformation->FileName, e.name.data(), e.name.size());
    file->entryIndex++;

    SetIoStatus(IoStatusBlock, X_STATUS_SUCCESS, needed);
    CompleteAsync(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Symbolic links

uint32_t ObCreateSymbolicLink(XANSI_STRING* link, XANSI_STRING* device)
{
    std::string linkName(link->Buffer.get(), link->Length.get());
    std::string target(device->Buffer.get(), device->Length.get());
    LOGFN("ObCreateSymbolicLink(\"{}\" -> \"{}\")", linkName, target);
    FileSystem::CreateSymbolicLink(linkName, target);
    return X_STATUS_SUCCESS;
}

uint32_t ObDeleteSymbolicLink(XANSI_STRING* link)
{
    std::string linkName(link->Buffer.get(), link->Length.get());
    LOGFN("ObDeleteSymbolicLink(\"{}\")", linkName);
    FileSystem::DeleteSymbolicLink(linkName);
    return X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtCreateFile, NtCreateFile);
GUEST_IMPORT(NtOpenFile, NtOpenFile);
GUEST_IMPORT(NtReadFile, NtReadFile);
GUEST_IMPORT(NtReadFileScatter, NtReadFileScatter);
GUEST_IMPORT(NtWriteFile, NtWriteFile);
GUEST_IMPORT(NtFlushBuffersFile, NtFlushBuffersFile);
GUEST_IMPORT(NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_IMPORT(NtQueryInformationFile, NtQueryInformationFile);
GUEST_IMPORT(NtSetInformationFile, NtSetInformationFile);
GUEST_IMPORT(NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
GUEST_IMPORT(NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_IMPORT(ObCreateSymbolicLink, ObCreateSymbolicLink);
GUEST_IMPORT(ObDeleteSymbolicLink, ObDeleteSymbolicLink);
