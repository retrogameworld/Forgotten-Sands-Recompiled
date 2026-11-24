#include <stdafx.h>
#ifdef __x86_64__
#include <cpuid.h>
#endif
#include <cpu/guest_thread.h>
#include <gpu/video.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/xam.h>
#include <kernel/xex_loader.h>
#include <kernel/io/file_system.h>
#include <kernel/io/iso_extract.h>
#include <apu/audio.h>
#include <hid/hid.h>
#include <hid/input_script.h>
#include <user/config.h>
#include <user/paths.h>
#include <os/logger.h>
#include <os/process.h>
#include <os/perf.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

// Torchlight runtime entry point. Based on UnleashedRecomp's main.cpp with the installer,
// mod loader, update checker, achievements, persistent storage and Sonic-specific startup
// removed. Loads default.xex, mounts game:\ and runs _xstart on a guest thread.

Memory g_memory;
Heap g_userHeap;

void InstallCrashHandler();
bool XeCryptSelfTest();

#ifdef __x86_64__
__attribute__((constructor(101), target("no-avx,no-avx2"), noinline))
void init()
{
    uint32_t eax, ebx, ecx, edx;
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    if ((ecx & (1 << 28)) == 0)
    {
        printf("[*] CPU does not support the AVX instruction set.\n");
        std::_Exit(1);
    }
}
#endif

// Step 36 (public release): the recompiled code belongs to one exact game executable. XXH3 of the default.xex
// it was made from (Prince of Persia: The Forgotten Sands, Xbox 360 disc, title 5553083E). 0 = not checked.
static constexpr uint64_t SUPPORTED_XEX_HASH = 0xC7AA0665D38C2FFEull;
static std::string U8Path(const std::filesystem::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
static void ShowMessage(bool error, const std::string& text)
{
    LOGFN("{}", text);
    if (Config::Window) // never a window in headless test runs
        SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, "Forgotten Sands Recompiled", text.c_str(), nullptr);
}

// The game folder: there already, or unpacked from the player's disc image (.iso) next to the exe.
static bool PrepareGameFolder(const std::filesystem::path& exeRoot, bool interactive)
{
    std::error_code ec;
    if (std::filesystem::exists(Config::XexPath, ec))
        return true;
    std::vector<std::filesystem::path> isos;
    for (const auto& dir : { exeRoot, exeRoot / "game" })
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (e.is_regular_file(ec) && ext == ".iso" && IsoExtract::IsGameDisc(e.path()))
                isos.push_back(e.path());
        }
    if (isos.empty())
    {
        ShowMessage(true, "The game files are missing.\n\nPut your own disc image of Prince of Persia: The Forgotten Sands "
            "(Xbox 360, a .iso file) into this folder:\n\n" + U8Path(exeRoot) + "\n\nand start the game again. "
            "It is unpacked into the 'game' folder at the first start. Or copy the files of an already unpacked "
            "disc into the 'game' folder (default.xex must be in it).");
        return false;
    }
    const auto& iso = isos.front();
    if (interactive)
        ShowMessage(false, "First start: your disc image\n\n" + U8Path(iso.filename()) + "\n\nis now unpacked into the "
            "'game' folder (about 5 GB, a few minutes). The game starts when it is done.\n\nPress OK to begin.");
    std::string error;
    int lastPercent = -1;
    const bool ok = IsoExtract::ExtractAll(iso, Config::GamePath, error, [&](uint64_t done, uint64_t total)
    {
        const int pct = total ? int(done * 100 / total) : 0;
        if (pct / 5 != lastPercent / 5) { lastPercent = pct; LOGFN("[iso] unpacking {}%", pct); printf("Unpacking the game: %d%%\n", pct); }
    });
    if (!ok || !std::filesystem::exists(Config::XexPath, ec))
    {
        ShowMessage(true, "The disc image could not be unpacked: " + (ok ? std::string("it has no default.xex") : error));
        return false;
    }
    LOGFN("[iso] done: {}", (const char*)Config::GamePath.u8string().c_str());
    return true;
}

// The game executable must be the one the code was recompiled from.
static bool CheckGameVersion(bool allowAny)
{
    std::ifstream f(Config::XexPath, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint64_t h = XXH3_64bits(data.data(), data.size());
    LOGFN("default.xex: {} bytes, XXH3 {:016X}", data.size(), h);
    if (SUPPORTED_XEX_HASH == 0 || h == SUPPORTED_XEX_HASH || allowAny)
        return true;
    ShowMessage(true, "This copy of the game is not supported.\n\nForgotten Sands Recompiled needs the Xbox 360 disc release "
        "of Prince of Persia: The Forgotten Sands (title 5553083E) without changes. The default.xex in the game folder "
        "is a different version (a title update or another release?).");
    return false;
}

static void PrintUsage()
{
    printf("ForgottenSandsRecomp.exe [--game <folder>] [--xex <file>] [--window] [--verbose] [--timeout <seconds>] [--quiet-files] [--shot <seconds> <file.bmp>] [--input-script <file>]\n");
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    os::process::CheckConsole();

    for (int i = 1; i + 1 < argc; i++)
        if (std::string_view(argv[i]) == "--write-default-settings") // Step 36: for the release zip
            return Config::WriteDefaultSettings(argv[i + 1]) ? 0 : 1;

    const auto exeRoot = os::process::GetExecutableRoot();
    // Launch history (before anything that could fail): proves a run started even if it dies before the log.
    if (FILE* h = _wfopen((exeRoot / "launch_history.txt").c_str(), L"a"))
    {
        const auto t = std::time(nullptr);
        char when[64]; std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        fprintf(h, "%s start pid %lu args:", when, GetCurrentProcessId());
        for (int i = 1; i < argc; i++) fprintf(h, " %s", argv[i]);
        fputc('\n', h);
        fclose(h);
    }
    Config::Load(exeRoot);

    // FS_LOG=<file>: Claude's test runs log elsewhere, so they never overwrite the log of the tester's own run.
    std::optional<std::filesystem::path> logFile = exeRoot / "ForgottenSandsRecomp.log";
    if (const char* l = getenv("FS_LOG"); l && *l)
        logFile = std::filesystem::path(l);
    uint32_t timeoutSeconds = 0;
    std::vector<uint32_t> peekAddresses; // --peek <hex guest address>: dumped at --timeout (debugging stalls)
    std::vector<std::pair<uint32_t, uint32_t>> mirrors; // --mirror <src> <dst>: copy a guest word every 1 ms (debug experiments only)
    std::vector<std::pair<uint32_t, std::filesystem::path>> shots; // --shot <seconds> <file.bmp>: save a presented frame
    std::filesystem::path inputScript; // --input-script <file>: scripted pad input (see hid/input_script.h)
    bool anyXex = false; // --any-xex: skip the game version check (developers)

    for (int i = 1; i < argc; i++)
    {
        std::string_view arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };

        if (arg == "--game") { Config::GamePath = next(); Config::XexPath = Config::GamePath / "default.xex"; }
        else if (arg == "--xex") Config::XexPath = next();
        else if (arg == "--window") Config::Window = true;
        else if (arg == "--headless") Config::Window = false;
        else if (arg == "--verbose") Config::Verbose = true;
        else if (arg == "--quiet-files") Config::LogFileAccess = false;
        else if (arg == "--timeout") timeoutSeconds = uint32_t(atoi(next()));
        else if (arg == "--peek") peekAddresses.push_back(uint32_t(strtoul(next(), nullptr, 16)));
        else if (arg == "--mirror") { uint32_t s = uint32_t(strtoul(next(), nullptr, 16)); mirrors.emplace_back(s, uint32_t(strtoul(next(), nullptr, 16))); }
        else if (arg == "--shot") { uint32_t t = uint32_t(atoi(next())); shots.emplace_back(t, std::filesystem::path(next())); }
        else if (arg == "--input-script") inputScript = next();
        else if (arg == "--any-xex") anyXex = true;
        else if (arg == "--help" || arg == "-h") { PrintUsage(); return 0; }
        else printf("Unknown argument: %s\n", argv[i]);
    }

    os::logger::g_verbose = Config::Verbose;
    os::logger::Init(*logFile);
    InstallCrashHandler();
    Perf::Init(); // TL_PROFILE=<from>-<to> seconds: sampling profile of all threads (os/perf.cpp)

    LOGN("Forgotten Sands Recompiled runtime (step 6e build " __DATE__ " " __TIME__ ")");
    LOGFN("Game folder: {}", (const char*)Config::GamePath.u8string().c_str());
    LOGFN("XEX:         {}", (const char*)Config::XexPath.u8string().c_str());
    LOGFN("User folder: {}", (const char*)Config::UserPath.u8string().c_str());

    if (g_memory.base == nullptr)
    {
        LOGN_ERROR("Could not reserve 4 GB of guest memory.");
        return 1;
    }
    LOGFN("Guest memory at host 0x{:X}", (uintptr_t)g_memory.base);

    LOGFN("XeCrypt SHA self test: {}", XeCryptSelfTest() ? "OK" : "FAILED");

    g_userHeap.Init();
    FileSystem::Init();

#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    SDL_SetMainReady();
    hid::Init();
    if (!inputScript.empty())
        InputScript::Load(inputScript);
    XAudioInitializeSystem();

    if (!PrepareGameFolder(exeRoot, Config::Window) || !CheckGameVersion(anyXex))
        return 1;
    if (!XexLoadExecutable(Config::XexPath))
    {
        LOGN_ERROR("Failed to load the XEX. Check game_path / xex_path in ForgottenSandsRecomp.toml.");
        return 1;
    }

    KernelStartTimeStampUpdater();

    if (!mirrors.empty())
    {
        LOGFN("DEBUG: {} --mirror rule(s) active", mirrors.size());
        std::thread([mirrors]
        {
            while (true)
            {
                for (auto [s, d] : mirrors)
                    *(uint32_t*)g_memory.Translate(d) = *(uint32_t*)g_memory.Translate(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }

    // The D3D replacement needs the host device even without --window (the window is then hidden).
    if (!Video::CreateHostDevice())
    {
        LOGN_ERROR("Could not create the Vulkan device; the game cannot render.");
        return 1;
    }

    std::atomic<bool> guestDone = false;
    std::thread guestMain([&]
    {
        LOGFN("Starting guest entry point _xstart at 0x{:08X}", g_xex.entryPoint);
        uint32_t result = GuestThread::Start({ g_xex.entryPoint, 0, 0, 0, std::max<uint32_t>(g_xex.defaultStackSize, 0x100000) });
        LOGFN("Guest main thread returned {}", result);
        guestDone = true;
    });

    const auto start = std::chrono::steady_clock::now();
    while (!guestDone)
    {
        if (Video::IsCreated())
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT) && !event.key.repeat)
                    Video::ToggleFullscreen();
                if (event.type == SDL_QUIT)
                {
                    LOGN("Window closed, exiting.");
                    os::logger::Flush();
                    std::_Exit(0);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));

        if (!InputScript::Update())
        {
            LOGFN("Input script quit ({} frames presented).", Video::GetPresentedFrameCount());
            GuestThread::DumpAll();
            os::logger::Flush();
            std::_Exit(0);
        }

        // Presentation happens on the guest render thread (Swap/Present hooks); here we only
        // pump window events and schedule test screenshots.
        const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        for (auto it = shots.begin(); it != shots.end();)
        {
            if (uint64_t(elapsedSeconds) >= it->first)
            {
                LOGFN("Screenshot requested at {}s ({} frames presented so far): {}", elapsedSeconds, Video::GetPresentedFrameCount(),
                    (const char*)it->second.u8string().c_str());
                Video::RequestScreenshot(it->second);
                it = shots.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (timeoutSeconds && std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds))
        {
            LOGFN("--timeout {}s reached, guest still running ({} frames presented).", timeoutSeconds, Video::GetPresentedFrameCount());
            GuestThread::DumpAll();
            for (uint32_t a : peekAddresses)
            {
                auto* w = (be<uint32_t>*)g_memory.Translate(a & ~3u);
                LOGFN("  peek 0x{:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", a & ~3u,
                    w[0].get(), w[1].get(), w[2].get(), w[3].get(), w[4].get(), w[5].get(), w[6].get(), w[7].get());
            }
            LOGN("Exiting.");
            os::logger::Flush();
            std::_Exit(2);
        }
    }

    guestMain.join();
    os::logger::Flush();
    std::_Exit(0);
}
