#include <stdafx.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include <os/logger.h>
#include "ppc_context.h"
#include <csetjmp>
#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// Forgotten Sands: full host call stack of a stopped thread with symbol names. The recompiled guest
// functions are named sub_XXXXXXXX in the PDB, so this shows the guest call chain of a stall.
static std::string HostStack(HANDLE thread)
{
    static bool s_symInit = false;
    if (!s_symInit)
    {
        s_symInit = true;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }

    uint64_t pcs[48];
    int n = 0;
    if (SuspendThread(thread) == DWORD(-1))
        return " (suspend failed)";
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(thread, &ctx))
    {
        for (; n < 48 && ctx.Rip != 0; n++)
        {
            pcs[n] = ctx.Rip;
            DWORD64 imageBase = 0;
            auto* fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (fe == nullptr)
            {
                if (ctx.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) { n++; break; }
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
            else
            {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
            }
        }
    }
    ResumeThread(thread);

    std::string out;
    for (int i = 0; i < n; i++)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), pcs[i], &disp, sym))
            out += fmt::format("\n      {}+0x{:X}", sym->Name, disp);
        else
            out += fmt::format("\n      0x{:X}", pcs[i]);
    }
    return out;
}
#endif

#ifdef _WIN32
// Debug helper: the current thread's guest call chain (recompiled sub_XXXXXXXX frames only), innermost first.
std::string CurrentGuestCallChain(int maxFrames)
{
    static std::mutex s_mutex;
    std::lock_guard lock(s_mutex);
    static bool s_symInit = false;
    if (!s_symInit)
    {
        s_symInit = true;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }
    void* frames[64];
    USHORT n = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
    std::string out;
    int found = 0;
    for (USHORT i = 0; i < n && found < maxFrames; i++)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), DWORD64(frames[i]), &disp, sym) && strstr(sym->Name, "sub_8") != nullptr)
        {
            out += fmt::format(" {}", sym->Name);
            found++;
        }
    }
    return out;
}
#endif
#ifdef _WIN32
std::string SymbolizeHostAddress(uint64_t address)
{
    static std::mutex s_mutex;
    std::lock_guard lock(s_mutex);
    static bool s_symInit = false;
    if (!s_symInit) { s_symInit = true; SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), nullptr, TRUE); }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &disp, sym))
        return fmt::format("{}+0x{:X}", sym->Name, disp);
    return fmt::format("0x{:X}", address);
}
#endif
constexpr size_t PCR_SIZE = 0xAB0;
constexpr size_t TLS_SIZE = 0x100;
constexpr size_t TEB_SIZE = 0x2E0;
constexpr size_t MIN_STACK_SIZE = 0x40000;
constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize)
{
    assert(thread == nullptr);

    const size_t stack = RoundUp<size_t>(std::max<size_t>(MIN_STACK_SIZE, stackSize), 0x1000);
    totalSize = PCR_SIZE + TLS_SIZE + TEB_SIZE + stack;

    thread = (uint8_t*)g_userHeap.Alloc(totalSize);
    memset(thread, 0, totalSize);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    // Stack grows down from the end of the block; 16-byte aligned with a small gap.
    ppcContext.r1.u64 = (g_memory.MapVirtual(thread + totalSize) - 0x100) & ~0xFull;
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
}

GuestThreadContext::~GuestThreadContext()
{
    g_ppcContext = nullptr;
    g_userHeap.Free(thread);
}

static thread_local std::jmp_buf* t_exitJump;
static thread_local uint32_t t_exitCode;

static void GuestThreadFunc(GuestThreadHandle* hThread)
{
#ifdef _WIN32
    // Keep 64 KB for the crash handler, so a stack overflow is reported instead of a silent exit.
    ULONG guarantee = 0x10000;
    SetThreadStackGuarantee(&guarantee);
#endif
    hThread->suspended.wait(true);
    hThread->exitCode = GuestThread::Start(hThread->params);
    hThread->finished = true;
    hThread->finished.notify_all();
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params), suspended((params.flags & 0x1) != 0)
{
    thread = std::thread(GuestThreadFunc, this);
}

GuestThreadHandle::~GuestThreadHandle()
{
    // Closing a thread handle on the console does not wait for or kill the thread.
    if (thread.joinable())
        thread.detach();
}

template <typename ThreadType>
static uint32_t CalcThreadId(const ThreadType& id)
{
    if constexpr (sizeof(id) == 4)
        return *reinterpret_cast<const uint32_t*>(&id);
    else
        return XXH32(&id, sizeof(id), 0);
}

uint32_t GuestThreadHandle::GetThreadId() const
{
    return CalcThreadId(thread.get_id());
}

uint32_t GuestThreadHandle::Wait(uint32_t timeout)
{
    if (timeout == INFINITE)
    {
        while (!finished)
            finished.wait(false);
        return STATUS_WAIT_0;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (!finished)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return STATUS_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return STATUS_WAIT_0;
}

// Registry of running guest threads, for the stall report (GuestThread::DumpAll).
struct ThreadRecord
{
    uint32_t entry;
    PPCContext* ctx;
#ifdef _WIN32
    HANDLE handle;
#endif
};
static std::mutex g_threadRecordMutex;
static std::list<ThreadRecord> g_threadRecords;

struct ThreadRecordScope
{
    std::list<ThreadRecord>::iterator it;
    ThreadRecordScope(uint32_t entry, PPCContext* ctx)
    {
        ThreadRecord rec{ entry, ctx };
#ifdef _WIN32
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &rec.handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
#endif
        std::lock_guard lock(g_threadRecordMutex);
        it = g_threadRecords.insert(g_threadRecords.end(), rec);
    }
    ~ThreadRecordScope()
    {
        std::lock_guard lock(g_threadRecordMutex);
#ifdef _WIN32
        CloseHandle(it->handle);
#endif
        g_threadRecords.erase(it);
    }
};

void GuestThread::DumpAll()
{
    std::lock_guard lock(g_threadRecordMutex);
    LOGFN("Guest threads running: {}", g_threadRecords.size());
    for (auto& rec : g_threadRecords)
    {
        std::string where;
#ifdef _WIN32
        uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
        for (int sample = 0; sample < 4; sample++)
        {
            if (SuspendThread(rec.handle) != DWORD(-1))
            {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(rec.handle, &c))
                    where += fmt::format(" exe+0x{:X}", c.Rip - exeBase);
                ResumeThread(rec.handle);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#endif
        // r13 = this thread's id as critical sections store it; [r3+24] = OwningThread when r3 is a critical section.
        const uint32_t r3 = rec.ctx->r3.u32;
        const uint32_t owner = (r3 >= 0x10000 && r3 < 0xF0000000) ? uint32_t(*reinterpret_cast<be<uint32_t>*>(g_memory.Translate(r3 + 24))) : 0;
        LOGFN("  thread entry 0x{:08X}: guest sp=0x{:08X} r3=0x{:08X} r13=0x{:08X} [r3+24]=0x{:08X} host pc samples:{}", rec.entry, rec.ctx->r1.u32, r3, rec.ctx->r13.u32, owner, where);
#ifdef _WIN32
        LOGFN("    stack:{}", HostStack(rec.handle));
#endif
    }
}

uint32_t GuestThread::Start(const GuestThreadParams& params)
{
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    GuestThreadContext ctx(cpuNumber, params.stackSize);
    ThreadRecordScope record(params.function, &ctx.ppcContext);

    std::jmp_buf exitJump;
    t_exitJump = &exitJump;

    if (setjmp(exitJump) == 0)
    {
        if (params.startup != 0)
        {
            ctx.ppcContext.r3.u64 = params.function;
            ctx.ppcContext.r4.u64 = params.value;
            g_memory.FindFunction(params.startup)(ctx.ppcContext, g_memory.base);
        }
        else
        {
            ctx.ppcContext.r3.u64 = params.value;
            g_memory.FindFunction(params.function)(ctx.ppcContext, g_memory.base);
        }
        t_exitJump = nullptr;
        return ctx.ppcContext.r3.u32;
    }

    t_exitJump = nullptr;
    LOGFN("Guest thread 0x{:08X} exited via ExTerminateThread({})", params.function, t_exitCode);
    return t_exitCode;
}

void GuestThread::Exit(uint32_t exitCode)
{
    t_exitCode = exitCode;
    if (t_exitJump)
        std::longjmp(*t_exitJump, 1);

    LOGFN_ERROR("ExTerminateThread called outside a guest thread, exiting process");
    os::logger::Flush();
    std::_Exit(int(exitCode));
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto hThread = CreateKernelObject<GuestThreadHandle>(params);

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

uint32_t GuestThread::GetCurrentThreadId()
{
    return CalcThreadId(std::this_thread::get_id());
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}
