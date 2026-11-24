#include <stdafx.h>
#include <os/perf.h>
#include <os/logger.h>
#include <os/process.h>
#include <user/config.h>

#ifdef _WIN32
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

// Step 9a - see perf.h.

namespace
{
    struct Slot
    {
        std::atomic<uint64_t> count{ 0 };
        std::atomic<uint64_t> ns{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
    };

    Slot g_slots[Perf::Count_];
    const auto g_start = std::chrono::steady_clock::now();

    std::mutex g_frameMutex;
    uint64_t g_lastFrameNs = 0;
    uint64_t g_framesInSecond = 0;
    uint64_t g_maxFrameNs = 0;

    std::atomic<bool> g_running{ false };
    std::thread g_reporter;
#ifdef _WIN32
    std::thread g_profiler;
#endif
}

uint64_t Perf::NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count());
}

void Perf::Add(Counter c, uint64_t ns, uint64_t bytes)
{
    auto& s = g_slots[c];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.ns.fetch_add(ns, std::memory_order_relaxed);
    if (bytes)
        s.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

uint64_t Perf::Count(Counter c)
{
    return g_slots[c].count.load(std::memory_order_relaxed);
}

bool Perf::LoadingActivitySinceLastCheck()
{
    static uint64_t last[4];
    const uint64_t now[4] = { Count(FileOpen), Count(FileRead), Count(TextureCreate), Count(ShaderTranslate) + Count(ShaderCacheLoad) + Count(Pipeline) };
    // music streaming reads a few blocks per second during normal play: ignore single reads
    const bool active = now[0] != last[0] || now[1] > last[1] + 1 || now[2] != last[2] || now[3] != last[3];
    for (int i = 0; i < 4; i++) last[i] = now[i];
    return active;
}

// Load bursts: a run of frames that are long (> 25 ms) or come with loading work, logged once it is over.
static struct { bool active = false; uint64_t start = 0, end = 0, frames = 0, longest = 0; uint64_t last[4] = {}; } g_burst;
static std::atomic<uint64_t> g_lastBusyNs{ 0 };

bool Perf::RecentlyBusy(uint64_t windowNs)
{
    const uint64_t last = g_lastBusyNs.load(std::memory_order_relaxed);
    return last != 0 && NowNs() - last < windowNs;
}

void Perf::FramePresented()
{
    const uint64_t now = NowNs();
    std::lock_guard lock(g_frameMutex);
    const uint64_t frameNs = g_lastFrameNs ? now - g_lastFrameNs : 0;
    if (g_lastFrameNs != 0)
        g_maxFrameNs = std::max(g_maxFrameNs, frameNs);

    const uint64_t c[4] = { Count(FileOpen), Count(FileRead), Count(TextureCreate), Count(GuestCompile) + Count(Pipeline) };
    const bool work = c[0] != g_burst.last[0] || c[1] > g_burst.last[1] + 1 || c[2] != g_burst.last[2] || c[3] != g_burst.last[3];
    for (int i = 0; i < 4; i++) g_burst.last[i] = c[i];
    const bool busy = frameNs > 25'000'000ull || work;
    // Step 11 debug: TL_FRAME_TRACE=1 logs every long / loading frame (load structure analysis)
    static const bool frameTrace = getenv("TL_FRAME_TRACE") != nullptr;
    if (frameTrace && (busy || g_burst.active))
        LOGFN("[frame] {:.3f} s: {:.1f} ms{}", double(now) / 1e9, double(frameNs) / 1e6, work ? " (load work)" : "");
    if (busy)
    {
        g_lastBusyNs.store(now, std::memory_order_relaxed);
        if (!g_burst.active)
        {
            g_burst = { true, g_lastFrameNs ? g_lastFrameNs : now, now, 0, 0, { c[0], c[1], c[2], c[3] } };
        }
        g_burst.end = now;
        g_burst.longest = std::max(g_burst.longest, frameNs);
    }
    if (g_burst.active)
    {
        g_burst.frames++;
        if (now - g_burst.end > 1'000'000'000ull)
        {
            g_burst.active = false;
            const double dur = double(g_burst.end - g_burst.start) / 1e9;
            if (dur > 0.3 && g_burst.longest > 25'000'000ull) // gameplay with a little streaming is not a load
                LOGFN("[perf] LOAD {:.2f}-{:.2f} s: {:.2f} s, longest frame {:.0f} ms", double(g_burst.start) / 1e9, double(g_burst.end) / 1e9, dur,
                    double(g_burst.longest) / 1e6);
        }
    }
    g_lastFrameNs = now;
    g_framesInSecond++;
}

static void ReporterThread()
{
    uint64_t prev[Perf::Count_][3] = {};
    uint64_t lastNs = Perf::NowNs();
    uint64_t quietSeconds = 0;
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const uint64_t now = Perf::NowNs();

        uint64_t frames, maxFrame, lastFrame;
        {
            std::lock_guard lock(g_frameMutex);
            frames = g_framesInSecond;
            maxFrame = g_maxFrameNs;
            lastFrame = g_lastFrameNs;
            g_framesInSecond = 0;
            g_maxFrameNs = 0;
        }
        // a frame still running right now counts as long too
        if (lastFrame && now > lastFrame)
            maxFrame = std::max(maxFrame, now - lastFrame);

        uint64_t d[Perf::Count_][3];
        for (uint32_t i = 0; i < Perf::Count_; i++)
        {
            const uint64_t c = g_slots[i].count.load(), n = g_slots[i].ns.load(), b = g_slots[i].bytes.load();
            d[i][0] = c - prev[i][0];
            d[i][1] = n - prev[i][1];
            d[i][2] = b - prev[i][2];
            prev[i][0] = c; prev[i][1] = n; prev[i][2] = b;
        }

        const double secs = double(now - lastNs) / 1e9;
        lastNs = now;
        const double fps = frames / std::max(secs, 0.001);
        const double expected = Config::FpsLimit > 0 ? double(Config::FpsLimit) : 60.0;
        const bool busy = maxFrame > 40'000'000ull || fps < expected * 0.85 || d[Perf::FileOpen][0] > 20 ||
            d[Perf::FileRead][2] > (1u << 20) || d[Perf::ShaderTranslate][0] || d[Perf::Pipeline][0] || d[Perf::GuestCompile][0];
        if (!busy || frames == 0 && lastFrame == 0)
        {
            quietSeconds++;
            continue;
        }
        quietSeconds = 0;

        auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
        LOGFN("[perf] {:.1f}s: {} frames ({:.0f} fps, longest {:.0f} ms) | open {} ({} missing) {:.0f} ms | read {} x {:.1f} MB {:.0f} ms | "
            "query {} {:.0f} ms | shaders new {} {:.0f} ms, cached {} {:.0f} ms | guest compile {} {:.0f} ms | pipelines {} {:.0f} ms | "
            "tex create {} {:.0f} ms, upload {} {:.1f} MB {:.0f} ms | sleep {} x {:.0f} ms | limiter {:.0f} ms | shader wait {} {:.0f} ms",
            double(now) / 1e9, frames, fps, ms(maxFrame),
            d[Perf::FileOpen][0], d[Perf::FileOpenMissing][0], ms(d[Perf::FileOpen][1]),
            d[Perf::FileRead][0], double(d[Perf::FileRead][2]) / 1048576.0, ms(d[Perf::FileRead][1]),
            d[Perf::FileQuery][0], ms(d[Perf::FileQuery][1]),
            d[Perf::ShaderTranslate][0], ms(d[Perf::ShaderTranslate][1]), d[Perf::ShaderCacheLoad][0], ms(d[Perf::ShaderCacheLoad][1]),
            d[Perf::GuestCompile][0], ms(d[Perf::GuestCompile][1]),
            d[Perf::Pipeline][0], ms(d[Perf::Pipeline][1]),
            d[Perf::TextureCreate][0], ms(d[Perf::TextureCreate][1]),
            d[Perf::TextureUpload][0], double(d[Perf::TextureUpload][2]) / 1048576.0, ms(d[Perf::TextureUpload][1]),
            d[Perf::GuestSleep][0], ms(d[Perf::GuestSleep][1]), ms(d[Perf::FrameWait][1]),
            d[Perf::ShaderWait][0], ms(d[Perf::ShaderWait][1]));
    }
}

// =============================================================================================
// Sampling profiler (Windows, TL_PROFILE)
// =============================================================================================
#ifdef _WIN32
namespace
{
    constexpr int MAX_DEPTH = 96;

    struct ThreadProfile
    {
        uint64_t samples = 0;
        std::unordered_map<uint64_t, uint64_t> inclusive; // function start -> samples
        std::unordered_map<uint64_t, uint64_t> exclusive;
        std::unordered_map<uint64_t, uint64_t> callers;   // (leaf function, caller) pairs hashed: leaf ^ caller<<1 -> samples
        std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> callerKey;
        struct Stack { uint64_t count = 0; std::vector<uint64_t> frames; };
        std::unordered_map<uint64_t, Stack> stacks;          // whole call stacks (function starts, leaf first)
        std::unordered_map<uint64_t, uint64_t> exePc;        // first pc inside the exe (leaf or nearest caller) -> samples
    };

    bool InExe(uint64_t pc)
    {
        static const auto range = [] {
            auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            return std::pair<uint64_t, uint64_t>(base, base + nt->OptionalHeader.SizeOfImage);
        }();
        return pc >= range.first && pc < range.second;
    }

    std::string LineName(uint64_t addr)
    {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD disp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &disp, &line))
            return fmt::format("{}:{}", std::filesystem::path(line.FileName).filename().string(), line.LineNumber);
        return fmt::format("?+0x{:X}", addr);
    }

    uint64_t FunctionStart(uint64_t pc)
    {
        DWORD64 imageBase = 0;
        if (auto* fe = RtlLookupFunctionEntry(pc, &imageBase, nullptr))
            return imageBase + fe->BeginAddress;
        return pc; // leaf code without unwind info (or JIT): keep the pc
    }

    int CaptureStack(HANDLE thread, uint64_t* out)
    {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(thread) == DWORD(-1))
            return 0;
        int n = 0;
        if (GetThreadContext(thread, &ctx))
        {
            // Unwind while the thread is stopped (stack memory is stable). No allocations here.
            for (; n < MAX_DEPTH; n++)
            {
                out[n] = ctx.Rip;
                DWORD64 imageBase = 0;
                auto* fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (fe == nullptr)
                {
                    // leaf function: return address is at [rsp]
                    if (ctx.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8))
                    {
                        n++;
                        break;
                    }
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                }
                else
                {
                    void* handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
                }
                if (ctx.Rip == 0)
                {
                    n++;
                    break;
                }
            }
        }
        ResumeThread(thread);
        return n;
    }

    std::string SymbolName(uint64_t addr)
    {
        static bool init = false;
        if (!init)
        {
            init = true;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        }
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string module;
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &mod))
        {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mod, path, MAX_PATH))
                module = std::filesystem::path(path).filename().string();
        }
        if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym))
            return fmt::format("{}!{}{}", module, sym->Name, disp ? fmt::format("+0x{:X}", disp) : "");
        return fmt::format("{}+0x{:X}", module.empty() ? "?" : module, mod ? addr - uint64_t(mod) : addr);
    }

    void WriteReport(const std::map<DWORD, ThreadProfile>& threads, double from, double to, uint64_t rounds)
    {
        const auto path = os::process::GetExecutableRoot() / fmt::format("profile_{:.0f}_{:.0f}.txt", from, to);
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"w");
        if (!f)
            return;
        fmt::print(f, "TL_PROFILE {:.1f}-{:.1f} s, {} sampling rounds (about 1 ms apart)\n", from, to, rounds);
        fmt::print(f, "Percentages are of this thread's samples. Inclusive = function anywhere on the stack.\n\n");

        std::vector<std::pair<DWORD, const ThreadProfile*>> order;
        for (auto& [id, p] : threads)
            order.push_back({ id, &p });
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second->samples > b.second->samples; });

        for (auto& [id, p] : order)
        {
            if (p->samples < rounds / 50 + 5)
                continue;
            // A thread that is mostly blocked shows ntdll wait functions as its top exclusive entries.
            std::vector<std::pair<uint64_t, uint64_t>> inc(p->inclusive.begin(), p->inclusive.end());
            std::vector<std::pair<uint64_t, uint64_t>> exc(p->exclusive.begin(), p->exclusive.end());
            std::sort(inc.begin(), inc.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::sort(exc.begin(), exc.end(), [](auto& a, auto& b) { return a.second > b.second; });
            fmt::print(f, "==== thread {} : {} samples ====\n-- exclusive (where the time is spent)\n", id, p->samples);
            for (size_t i = 0; i < std::min<size_t>(exc.size(), 30); i++)
                fmt::print(f, "  {:5.1f}%  {}\n", 100.0 * exc[i].second / p->samples, SymbolName(exc[i].first));
            fmt::print(f, "-- inclusive\n");
            for (size_t i = 0; i < std::min<size_t>(inc.size(), 70); i++)
                fmt::print(f, "  {:5.1f}%  {}\n", 100.0 * inc[i].second / p->samples, SymbolName(inc[i].first));
            std::vector<std::pair<uint64_t, uint64_t>> cal;
            for (auto& [k, v] : p->callers)
                cal.push_back({ k, v });
            std::sort(cal.begin(), cal.end(), [](auto& a, auto& b) { return a.second > b.second; });
            fmt::print(f, "-- hottest exclusive function <- caller\n");
            for (size_t i = 0; i < std::min<size_t>(cal.size(), 25); i++)
            {
                auto it = p->callerKey.find(cal[i].first);
                if (it == p->callerKey.end())
                    continue;
                fmt::print(f, "  {:5.1f}%  {} <- {}\n", 100.0 * cal[i].second / p->samples, SymbolName(it->second.first), SymbolName(it->second.second));
            }
            {
                // Source lines (first frame inside the exe: inlined code shows as its own line).
                std::unordered_map<std::string, uint64_t> lines;
                for (auto& [pc, c] : p->exePc)
                    lines[LineName(pc)] += c;
                std::vector<std::pair<std::string, uint64_t>> ls(lines.begin(), lines.end());
                std::sort(ls.begin(), ls.end(), [](auto& a, auto& b) { return a.second > b.second; });
                // Per source file (ppc_recomp.*.cpp = the game's own code).
                std::unordered_map<std::string, uint64_t> files;
                for (auto& [ln, c] : lines)
                {
                    std::string fn = ln.substr(0, ln.rfind(':'));
                    if (fn.rfind("ppc_recomp.", 0) == 0 || fn[0] == '?') fn = "game code (ppc_recomp, no line info)";
                    files[fn] += c;
                }
                std::vector<std::pair<std::string, uint64_t>> fs(files.begin(), files.end());
                std::sort(fs.begin(), fs.end(), [](auto& a, auto& b) { return a.second > b.second; });
                fmt::print(f, "-- source files (exe; samples whose first exe frame is there)\n");
                for (size_t i = 0; i < std::min<size_t>(fs.size(), 15); i++)
                    fmt::print(f, "  {:5.1f}%  {}\n", 100.0 * fs[i].second / p->samples, fs[i].first);
                fmt::print(f, "-- hottest source lines (exe)\n");
                for (size_t i = 0; i < std::min<size_t>(ls.size(), 40); i++)
                    fmt::print(f, "  {:5.1f}%  {}\n", 100.0 * ls[i].second / p->samples, ls[i].first);
            }
            std::vector<const ThreadProfile::Stack*> st;
            for (auto& [k, v] : p->stacks)
                st.push_back(&v);
            std::sort(st.begin(), st.end(), [](auto a, auto b) { return a->count > b->count; });
            for (size_t i = 0; i < std::min<size_t>(st.size(), 6); i++)
            {
                if (st[i]->count * 50 < p->samples)
                    break;
                fmt::print(f, "-- stack {:.1f}% (leaf first)\n", 100.0 * st[i]->count / p->samples);
                for (auto fn : st[i]->frames)
                    fmt::print(f, "      {}\n", SymbolName(fn));
            }
            fmt::print(f, "\n");
        }
        fclose(f);
        LOGFN("[perf] profile written: {}", path.string());
    }

    void ProfilerThread(std::vector<std::pair<double, double>> windows)
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        for (auto [from, to] : windows)
        {
            while (g_running.load() && double(Perf::NowNs()) / 1e9 < from)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!g_running.load())
                return;
            LOGFN("[perf] profiling {:.1f}-{:.1f} s", from, to);

            std::map<DWORD, ThreadProfile> profiles;
            std::map<DWORD, HANDLE> handles;
            uint64_t rounds = 0;
            auto lastEnum = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            static uint64_t stack[MAX_DEPTH];
            while (g_running.load() && double(Perf::NowNs()) / 1e9 < to)
            {
                if (std::chrono::steady_clock::now() - lastEnum > std::chrono::milliseconds(500))
                {
                    lastEnum = std::chrono::steady_clock::now();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    THREADENTRY32 te{ sizeof(te) };
                    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
                    {
                        do
                        {
                            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || handles.count(te.th32ThreadID))
                                continue;
                            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                            if (h)
                                handles[te.th32ThreadID] = h;
                        } while (Thread32Next(snap, &te));
                    }
                    if (snap != INVALID_HANDLE_VALUE)
                        CloseHandle(snap);
                }

                for (auto& [id, h] : handles)
                {
                    const int n = CaptureStack(h, stack);
                    if (n <= 0)
                        continue;
                    auto& p = profiles[id];
                    p.samples++;
                    for (int i = 0; i < n; i++)
                        if (InExe(stack[i])) { p.exePc[i == 0 ? stack[i] : stack[i] - 1]++; break; }
                    const uint64_t leaf = FunctionStart(stack[0]);
                    p.exclusive[leaf]++;
                    if (n > 1)
                    {
                        const uint64_t caller = FunctionStart(stack[1]);
                        const uint64_t key = leaf * 31 + caller;
                        p.callers[key]++;
                        p.callerKey[key] = { leaf, caller };
                    }
                    {
                        uint64_t h = 1469598103934665603ull;
                        for (int i = 0; i < n; i++)
                            h = (h ^ FunctionStart(stack[i])) * 1099511628211ull;
                        auto& st = p.stacks[h];
                        if (st.count++ == 0)
                            for (int i = 0; i < n; i++)
                                st.frames.push_back(FunctionStart(stack[i]));
                    }
                    // inclusive: each function once per sample
                    uint64_t seen[MAX_DEPTH];
                    int seenCount = 0;
                    for (int i = 0; i < n; i++)
                    {
                        const uint64_t fn = FunctionStart(stack[i]);
                        bool dup = false;
                        for (int j = 0; j < seenCount; j++)
                            if (seen[j] == fn) { dup = true; break; }
                        if (dup)
                            continue;
                        seen[seenCount++] = fn;
                        p.inclusive[fn]++;
                    }
                }
                rounds++;
                std::this_thread::sleep_for(std::chrono::microseconds(900));
            }
            for (auto& [id, h] : handles)
                CloseHandle(h);
            WriteReport(profiles, from, to, rounds);
        }
    }
}
#endif

void Perf::Init()
{
    if (g_running.exchange(true))
        return;
    g_reporter = std::thread(ReporterThread);

#ifdef _WIN32
    if (const char* spec = getenv("TL_PROFILE"); spec && *spec)
    {
        std::vector<std::pair<double, double>> windows;
        std::string s(spec);
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t comma = s.find(',', pos);
            std::string part = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            double a = 0, b = 0;
            if (sscanf(part.c_str(), "%lf-%lf", &a, &b) == 2 && b > a)
                windows.push_back({ a, b });
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        if (!windows.empty())
            g_profiler = std::thread(ProfilerThread, windows);
    }
#endif
}

void Perf::Shutdown()
{
    if (!g_running.exchange(false))
        return;
    if (g_reporter.joinable())
        g_reporter.detach();
#ifdef _WIN32
    if (g_profiler.joinable())
        g_profiler.detach();
#endif
}
