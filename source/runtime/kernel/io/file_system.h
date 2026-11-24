#pragma once

struct FileSystem
{
    // Mounts game:\ d:\ cache:\ etc. Call once after Config::Load().
    // Step 26: when the game path is a .pak file (ForgottenSands.pak), game:\ is read from inside it.
    static void Init();

    // Guest path ("game:\resources.cfg", "\Device\Cdrom0\x", "\??\cache:\y") -> host path.
    // Returns an empty path if the device is unknown. readOnly is set for the game folder,
    // overlay receives the matching writable path under <user>/game_overlay for game paths.
    // Step 26: paths inside the game pak are "<pak file>/<relative path>" (not real files; use the
    // functions below to read them).
    static std::filesystem::path ResolvePath(std::string_view guestPath, bool* readOnly = nullptr, std::filesystem::path* overlay = nullptr);

    static void CreateSymbolicLink(std::string_view link, std::string_view target);
    static void DeleteSymbolicLink(std::string_view link);

    // Step 7: the game sees hostFile as its first `cut` bytes followed by `tail` (used to append
    // patched entries + a new central directory to pak.zip without touching the real file).
    // Call before the guest starts.
    static void SetVirtualTail(const std::filesystem::path& hostFile, uint64_t cut, std::vector<uint8_t> tail);

    // Step 26: read a host file or a file inside the game pak (paths like Config::GamePath / "pak.zip").
    static bool ReadHostFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
    // Names of the files in a folder of the game pak ("hires"); empty without a pak.
    static std::vector<std::string> ListPakFolder(std::string_view folder);
    static bool PakMounted();
    // Step 33: mounts a pak that starts at byte `base` of `file` (the game pak inside the Setup exe);
    // paths inside it are then "<file>/<relative path>".
    static bool MountPakAt(const std::filesystem::path& file, uint64_t base);
    // Step 27: writes an encrypted game pak; sources = (folder, prefix inside the pak, "." = root).
    static bool BuildPak(const std::filesystem::path& out, const std::vector<std::pair<std::filesystem::path, std::string>>& sources);

    // Random access to a host file or a file inside the game pak.
    class Reader
    {
    public:
        Reader() = default;
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;
        ~Reader();
        bool Open(const std::filesystem::path& path);
        uint64_t Size() const { return m_size; }
        size_t Read(uint64_t offset, void* buffer, size_t length); // returns bytes read
    private:
        FILE* m_fp = nullptr;
        uint64_t m_base = 0, m_size = 0;
        bool m_pak = false;
    };
};
