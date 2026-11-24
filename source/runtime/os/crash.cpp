#include <stdafx.h>
#include <os/logger.h>
#include <kernel/memory.h>
#include <cpu/ppc_context.h>
#include <csignal>

// Crash reporter: logs the host fault address (as an offset into the exe, look it up in
// TorchlightRecomp.map), the guest address being accessed and the guest registers we have.

std::string SymbolizeHostAddress(uint64_t address);

static void LogGuestState()
{
    if (auto* ctx = GetPPCContext())
        LOGFN_ERROR("  guest r1(sp)=0x{:08X} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r13=0x{:08X}",
            ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r13.u32);
}

#ifdef _WIN32

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    auto* rec = info->ExceptionRecord;
    uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
    uintptr_t rip = (uintptr_t)info->ContextRecord->Rip;

    LOGFN_ERROR("CRASH at {}", SymbolizeHostAddress(rip));
    LOGFN_ERROR("CRASH: exception 0x{:08X} at host address 0x{:X} (exe+0x{:X}) thread {}",
        rec->ExceptionCode, rip, rip - exeBase, GetCurrentThreadId());

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        uintptr_t target = rec->ExceptionInformation[1];
        const char* kind = rec->ExceptionInformation[0] == 0 ? "read" : (rec->ExceptionInformation[0] == 1 ? "write" : "execute");
        if (target >= (uintptr_t)g_memory.base && target < (uintptr_t)g_memory.base + PPC_MEMORY_SIZE)
            LOGFN_ERROR("  {} of guest address 0x{:08X}", kind, uint32_t(target - (uintptr_t)g_memory.base));
        else if (target == 0 || rip == 0)
            LOGFN_ERROR("  {} of host address 0x{:X} (null: probably an indirect call to a guest function that was not recompiled)", kind, target);
        else
            LOGFN_ERROR("  {} of host address 0x{:X}", kind, target);
    }

    // Return address on the host stack helps when rip == 0 (call through a null table entry).
    uintptr_t* rsp = (uintptr_t*)info->ContextRecord->Rsp;
    if (rsp && !IsBadReadPtr(rsp, sizeof(uintptr_t) * 4))
        LOGFN_ERROR("  host stack top: exe+0x{:X} exe+0x{:X}", rsp[0] - exeBase, rsp[1] - exeBase);

    // Heuristic backtrace: values on the host stack that point into the exe's code (return
    // addresses of the recompiled guest functions). Look them up with maplookup.py.
    {
        auto dos = (const IMAGE_DOS_HEADER*)exeBase;
        auto nt = (const IMAGE_NT_HEADERS*)(exeBase + dos->e_lfanew);
        uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        std::string trace;
        int found = 0;
        for (uintptr_t* p = rsp; p && (uintptr_t)(p + 1) <= high && found < 32; p++)
        {
            if (*p > exeBase + 0x1000 && *p < exeEnd)
            {
                trace += fmt::format(" {}", SymbolizeHostAddress(*p));
                found++;
            }
        }
        LOGFN_ERROR("  stack scan:{}", trace);
    }

    LogGuestState();
    os::logger::Flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Forgotten Sands: uncaught C++ exceptions / abort / invalid CRT parameters end in __fastfail
// (exit 0xC0000409), which never reaches the filter above. Log them first.
static void LogFatal(const char* what)
{
    LOGFN_ERROR("FATAL: {} (thread {})", what, GetCurrentThreadId());
    uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    std::string trace;
    for (USHORT i = 0; i < n; i++)
        trace += fmt::format(" exe+0x{:X}", (uintptr_t)frames[i] - exeBase);
    LOGFN_ERROR("  host stack:{}", trace);
    LogGuestState();
    os::logger::Flush();
}

// Called by the generated code (src/ppc_lib/fs_ppc_overrides.h) for an indirect call whose target the
// recompiler never found: add the address to the `functions` list of the recompiler config.
void PPCMissingFunction(uint32_t guestAddress)
{
    LogFatal(fmt::format("indirect call to guest 0x{:08X}, which was not recompiled as a function", guestAddress).c_str());
    TerminateProcess(GetCurrentProcess(), 7);
}

void InstallCrashHandler()
{
    SetUnhandledExceptionFilter(CrashFilter);
    // Also report crashes the C runtime would otherwise swallow.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    std::set_terminate([]
    {
        std::string what = "std::terminate (uncaught C++ exception)";
        if (auto e = std::current_exception())
        {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) { what += fmt::format(": {}", ex.what()); }
            catch (...) {}
        }
        LogFatal(what.c_str());
        TerminateProcess(GetCurrentProcess(), 4);
    });
    signal(SIGABRT, [](int) { LogFatal("abort()"); TerminateProcess(GetCurrentProcess(), 5); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
    {
        LogFatal("invalid parameter passed to a C runtime function");
        TerminateProcess(GetCurrentProcess(), 6);
    });
}

#else

#include <signal.h>

static void CrashSignal(int sig, siginfo_t* si, void*)
{
    uintptr_t target = (uintptr_t)si->si_addr;
    LOGFN_ERROR("CRASH: signal {} address 0x{:X}", sig, target);
    if (target >= (uintptr_t)g_memory.base && target < (uintptr_t)g_memory.base + PPC_MEMORY_SIZE)
        LOGFN_ERROR("  guest address 0x{:08X}", uint32_t(target - (uintptr_t)g_memory.base));
    LogGuestState();
    os::logger::Flush();
    _exit(3);
}

void InstallCrashHandler()
{
    struct sigaction sa{};
    sa.sa_sigaction = CrashSignal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

#endif
