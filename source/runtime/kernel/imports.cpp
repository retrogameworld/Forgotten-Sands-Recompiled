#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <cpu/guest_thread.h>
#include <apu/audio.h>
#include "function.h"
#include "import_log.h"
#include "kernel_objects.h"
#include "xstatus.h"
#include "xbox.h"
#include "heap.h"
#include "memory.h"
#include "xam.h"
#include "xdm.h"
#include "guest_printf.h"
#include <user/config.h>
#include <os/logger.h>
#include <os/perf.h>

// xboxkrnl: threads, synchronisation, Rtl*, Ke*, Ob*, misc.
// Started from UnleashedRecomp kernel/imports.cpp; only Torchlight's imports are kept,
// every "!!! STUB !!!" body was either implemented or turned into a logged stub.
// File I/O lives in io/file_system.cpp, memory in nt_memory.cpp, XEX/module functions and
// kernel variables in xex_loader.cpp, XeCrypt in xecrypt.cpp, Vd* in video_stubs.cpp.

std::atomic<uint32_t> g_signalGeneration;
std::atomic<uint32_t> g_multiWaiters;

// Step 37 (Android): wait on / wake a 32-bit word the way WaitOnAddress / WakeByAddress do on Windows. The first
// Android build slept 1 ms per critical-section wait and per finite wait here: the game ran at 34 fps instead of 60.
#ifndef _WIN32
#include <linux/futex.h>
#include <sys/syscall.h>
// Step 37 (Odin profile: the game's job threads spent ~16% of their time in NtSetEvent, every set / every critical
// section leave was a futex system call even with nobody waiting): waiters are counted in a small table hashed by
// address, and a wake only goes to the kernel when someone may be waiting there. Dekker pattern: the waiter counts
// itself in, full fence, then the kernel compares the word; the waker changes the word, full fence, then reads the
// count - one of the two always sees the other. FS_ALWAYS_WAKE=1 = a system call for every wake (as before).
static std::atomic<uint32_t> g_addressWaiters[1024];
static std::atomic<uint32_t>& AddressWaiters(const void* addr)
{
    return g_addressWaiters[(uint32_t(uintptr_t(addr) >> 2) * 2654435761u) >> 22];
}
static bool AddressMayHaveWaiters(const void* addr)
{
    static const bool s_always = getenv("FS_ALWAYS_WAKE") != nullptr;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return s_always || AddressWaiters(addr).load(std::memory_order_relaxed) != 0;
}
static void AddressWait(const void* addr, uint32_t expected, uint32_t ms)
{
    timespec ts{ time_t(ms / 1000), long(ms % 1000) * 1000000L };
    auto& waiters = AddressWaiters(addr);
    waiters.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, ms == 0xFFFFFFFFu ? nullptr : &ts, nullptr, 0);
    waiters.fetch_sub(1, std::memory_order_relaxed);
}
static void AddressWakeAll(const void* addr)
{
    if (AddressMayHaveWaiters(addr))
        syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
}
static void AddressWakeOne(const void* addr)
{
    if (AddressMayHaveWaiters(addr))
        syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}
#endif

// ---------------------------------------------------------------------------------------
// Step 37: the console's one-core priority rule (crashes on PC and Odin in the game's sound objects, right after
// sounds start; the PC rate was ~1 in 8 level runs since the first builds).
// The game puts its XAudio processing thread (sub_82D29FD8, priority 16) on hardware thread 5 together with other
// threads, and those raise their own priority to 16 for a moment around work on the same sound data
// (KeSetBasePriorityThread(current, 16) ... (current, 0), hundreds of times). On the console two threads of
// priority 16 on one hardware thread never run at the same time; here every thread has its own host thread, so
// those sections ran right in the middle of the processing pass. Now threads of priority >= 16 on the same
// hardware thread take that core's gate while they run and give it back whenever they block (waits, sleeps,
// critical sections, yields). FS_NO_CORE_GATE=1: off.
static std::mutex g_coreGates[8];
static thread_local int t_coreGroup = -1;
static thread_local bool t_coreHigh = false, t_coreHolding = false;
static std::atomic<uint64_t> g_coreGateTakes[8];
static bool CoreGateOn() { static const bool s_on = getenv("FS_CORE_GATE") != nullptr; return s_on; } // opt-in: did not help (3 crashes in 8 PC runs)
static int CoreGroupOf(uint32_t affinity)
{
    if (affinity == 0 || (affinity & (affinity - 1)) != 0) return -1; // none or several hardware threads
    int g = 0; while (!(affinity & 1)) { affinity >>= 1; g++; }
    return g < 8 ? g : -1;
}
static void CoreGateAcquire()
{
    if (!CoreGateOn() || !t_coreHigh || t_coreGroup < 0 || t_coreHolding) return;
    if (!g_coreGates[t_coreGroup].try_lock())
    {
        static std::atomic<uint32_t> s_waits{0};
        const uint32_t n = ++s_waits;
        if (n <= 5 || (n & (n - 1)) == 0)
            LOGFN("[coregate] wait #{} on core {} (r13 0x{:08X})", n, t_coreGroup, g_ppcContext ? g_ppcContext->r13.u32 : 0);
        g_coreGates[t_coreGroup].lock();
    }
    t_coreHolding = true;
    g_coreGateTakes[t_coreGroup]++;
}
static void CoreGateRelease()
{
    if (!t_coreHolding) return;
    t_coreHolding = false;
    g_coreGates[t_coreGroup].unlock();
}
// Around anything that blocks: the gate goes to the core's other high-priority thread meanwhile.
struct CoreGateYield
{
    bool held;
    CoreGateYield() : held(t_coreHolding) { if (held) CoreGateRelease(); }
    ~CoreGateYield() { if (held) CoreGateAcquire(); }
};
static thread_local int t_corePriority = 0;
void CoreGateThreadStart(uint32_t affinity, int priority)
{
    t_coreGroup = CoreGroupOf(affinity);
    t_corePriority = priority;
    t_coreHigh = priority >= 16;
    CoreGateAcquire();
}
void CoreGateThreadEnd() { CoreGateRelease(); }
int CoreGateCurrentPriority(int set)
{
    const int previous = t_corePriority;
    t_corePriority = set;
    t_coreHigh = set >= 16;
    if (t_coreHigh) CoreGateAcquire(); else CoreGateRelease();
    return previous;
}
void CoreGateCurrentAffinity(uint32_t affinity)
{
    const int g = CoreGroupOf(affinity);
    if (g == t_coreGroup) return;
    CoreGateRelease();
    t_coreGroup = g;
    CoreGateAcquire();
}

void NotifySignal()
{
    g_signalGeneration.fetch_add(1);
#ifdef _WIN32
    g_signalGeneration.notify_all();
#else
    AddressWakeAll(&g_signalGeneration); // (nothing waits with std::atomic::wait off Windows)
#endif
}

void NotifySignal(std::atomic<uint32_t>& objectGeneration)
{
    objectGeneration.fetch_add(1);
#ifdef _WIN32
    objectGeneration.notify_all();
#else
    AddressWakeAll(&objectGeneration);
#endif
    if (g_multiWaiters.load() != 0) // seq_cst: pairs with the increment in WaitForObjects/NtWaitForMultipleObjectsEx
        NotifySignal();
}

// Sleeps until `notifier` moves off `generation` or `ms` pass (finite waits no longer poll every 1 ms).
static void WaitGenerationFor(std::atomic<uint32_t>& notifier, uint32_t generation, std::chrono::steady_clock::duration left)
{
#ifdef _WIN32
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    // debug FS_WAIT_POLL_MS=<n>: re-check the waited objects every n ms (finds signals that never notify)
    static const long long s_poll = [] { const char* e = getenv("FS_WAIT_POLL_MS"); return e ? std::max(1ll, atoll(e)) : 1000ll; }();
    WaitOnAddress(&notifier, &generation, sizeof(generation), DWORD(std::clamp<long long>(ms, 1, s_poll)));
#else
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    AddressWait(&notifier, generation, uint32_t(std::clamp<long long>(ms, 1, 1000)));
#endif
}

struct MultiWaitScope
{
    MultiWaitScope() { g_multiWaiters.fetch_add(1); }
    ~MultiWaitScope() { g_multiWaiters.fetch_sub(1); }
};

uint32_t WaitWithTimeout(const std::function<bool()>& tryAcquire, std::atomic<uint32_t>& notifier, uint32_t timeoutMs)
{
    if (tryAcquire())
        return X_STATUS_SUCCESS;

    if (timeoutMs == 0)
        return X_STATUS_TIMEOUT;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (true)
    {
        uint32_t generation = notifier.load();
        if (tryAcquire())
            return X_STATUS_SUCCESS;

        if (timeoutMs == X_INFINITE)
        {
            CoreGateYield gate;
            static const bool s_pollInf = getenv("FS_WAIT_POLL_MS") != nullptr;
            if (s_pollInf) WaitGenerationFor(notifier, generation, std::chrono::seconds(1));
#ifdef _WIN32
            else notifier.wait(generation);
#else
            else AddressWait(&notifier, generation, 1000); // (re-checked after a second, like the Windows poll)
#endif
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            // diag FS_LOG_WAIT_TIMEOUTS=1: which threads wake up by timeout (a missed signal shows up as a thread
            // that keeps timing out on a wait with a long timeout)
            static const bool s_log = getenv("FS_LOG_WAIT_TIMEOUTS") != nullptr;
            if (s_log && timeoutMs >= 5)
            {
                static std::mutex s_m; static std::map<std::pair<uint32_t, uint32_t>, int> s_counts; static auto s_at = std::chrono::steady_clock::now();
                std::lock_guard l(s_m);
                if (s_counts[{ g_ppcContext ? g_ppcContext->r13.u32 : 0, timeoutMs }]++ == 0)
                {
                    extern std::string CurrentGuestCallChain(int maxFrames);
                    LOGFN("[wait] first timeout: r13 0x{:08X} timeout {} ms |{}", g_ppcContext ? g_ppcContext->r13.u32 : 0, timeoutMs, CurrentGuestCallChain(8));
                }
                if (std::chrono::steady_clock::now() - s_at > std::chrono::seconds(5))
                {
                    std::string t; for (auto& [k, v] : s_counts) t += fmt::format(" [r13 {:08X} {} ms: {}]", k.first, k.second, v);
                    LOGFN("[wait] timeouts in the last 5 s:{}", t);
                    s_counts.clear(); s_at = std::chrono::steady_clock::now();
                }
            }
            return X_STATUS_TIMEOUT;
        }

        CoreGateYield gate;
        WaitGenerationFor(notifier, generation, deadline - now);
    }
}

uint32_t GuestTimeoutToMilliseconds(be<int64_t>* timeout)
{
    if (timeout == nullptr)
        return X_INFINITE;

    int64_t value = *timeout;
    if (value == 0)
        return 0;

    if (value < 0)
    {
        int64_t ms = (-value + 9999) / 10000;
        return ms >= X_INFINITE ? X_INFINITE - 1 : uint32_t(ms);
    }

    // Absolute time (100ns since 1601).
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    int64_t now = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        std::chrono::system_clock::now().time_since_epoch()).count() + FILETIME_EPOCH_DIFFERENCE;
    if (value <= now)
        return 0;
    return uint32_t(std::min<int64_t>((value - now) / 10000, X_INFINITE - 1));
}

KernelObject* ResolveWaitable(uint32_t handleOrAddress)
{
    if (handleOrAddress == 0 || handleOrAddress == GUEST_INVALID_HANDLE_VALUE || handleOrAddress == CURRENT_THREAD_HANDLE)
        return nullptr;

    if (IsKernelObject(handleOrAddress))
        return GetKernelObject(handleOrAddress);

    if (handleOrAddress >= PPC_MEMORY_SIZE)
        return nullptr;

    auto* header = reinterpret_cast<XDISPATCHER_HEADER*>(g_memory.Translate(handleOrAddress));
    switch (header->Type)
    {
    case 0: // NotificationEvent
    case 1: // SynchronizationEvent
        return QueryKernelObject<Event>(*header);
    case 2: // Mutant
        return QueryKernelObject<Mutant>(*header);
    case 5: // Semaphore
        return QueryKernelObject<Semaphore>(*reinterpret_cast<XDISPATCHER_HEADER*>(header));
    default:
        LOGFN_WARNING("ResolveWaitable: unknown dispatcher type {} at 0x{:08X}", header->Type, handleOrAddress);
        return nullptr;
    }
}

// Handle duplication: handles are host object addresses, so a duplicate is the same
// value with an extra reference that NtClose consumes before destroying the object.
static std::mutex g_handleRefMutex;
static std::unordered_map<uint32_t, uint32_t> g_handleExtraRefs;

// ---------------------------------------------------------------------------------------
// Ob* / handles

uint32_t NtClose(uint32_t handle)
{
    if (handle == GUEST_INVALID_HANDLE_VALUE || handle == 0)
        return X_STATUS_INVALID_HANDLE;

    if (!IsKernelObject(handle))
    {
        LOG_ONCE("NtClose: unknown handle 0x{:08X} (further ones not logged)", handle);
        return X_STATUS_INVALID_HANDLE;
    }

    {
        std::lock_guard lock(g_handleRefMutex);
        auto it = g_handleExtraRefs.find(handle);
        if (it != g_handleExtraRefs.end())
        {
            if (--it->second == 0)
                g_handleExtraRefs.erase(it);
            return X_STATUS_SUCCESS;
        }
    }

    DestroyKernelObject(handle);
    return X_STATUS_SUCCESS;
}

uint32_t NtDuplicateObject(uint32_t handle, be<uint32_t>* newHandle, uint32_t options)
{
    if (newHandle)
        *newHandle = handle;

    if ((options & 1) == 0 && IsKernelObject(handle)) // not DUPLICATE_CLOSE_SOURCE
    {
        std::lock_guard lock(g_handleRefMutex);
        g_handleExtraRefs[handle]++;
    }

    return X_STATUS_SUCCESS;
}

uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t objectType, be<uint32_t>* object)
{
    if (object)
        *object = handle;
    return X_STATUS_SUCCESS;
}

uint32_t ObReferenceObject(uint32_t object) { return 0; }
void ObDereferenceObject(uint32_t object) { }

// ---------------------------------------------------------------------------------------
// Events / semaphores / mutants / waits

uint32_t NtCreateEvent(be<uint32_t>* handle, void* objAttributes, uint32_t eventType, uint32_t initialState)
{
    *handle = GetKernelHandle(CreateKernelObject<Event>(!eventType, !!initialState));
    return X_STATUS_SUCCESS;
}

template<typename T>
static T* ResolveAs(uint32_t handle)
{
    return dynamic_cast<T*>(ResolveWaitable(handle));
}

uint32_t NtSetEvent(uint32_t handle, be<uint32_t>* previousState)
{
    auto* ev = ResolveAs<Event>(handle);
    if (!ev)
        return X_STATUS_INVALID_HANDLE;

    bool prev = ev->Set();
    if (previousState)
        *previousState = prev;
    return X_STATUS_SUCCESS;
}

uint32_t NtClearEvent(uint32_t handle)
{
    auto* ev = ResolveAs<Event>(handle);
    if (!ev)
        return X_STATUS_INVALID_HANDLE;

    ev->Reset();
    return X_STATUS_SUCCESS;
}

uint32_t KeSetEvent(XKEVENT* pEvent, uint32_t Increment, uint32_t Wait)
{
    auto* ev = QueryKernelObject<Event>(*pEvent);
    bool prev = ev->Set();
    pEvent->SignalState = 1;
    return prev;
}

uint32_t KeResetEvent(XKEVENT* pEvent)
{
    auto* ev = QueryKernelObject<Event>(*pEvent);
    bool prev = ev->Reset();
    pEvent->SignalState = 0;
    return prev;
}

uint32_t NtCreateSemaphore(be<uint32_t>* Handle, XOBJECT_ATTRIBUTES* ObjectAttributes, uint32_t InitialCount, uint32_t MaximumCount)
{
    *Handle = GetKernelHandle(CreateKernelObject<Semaphore>(InitialCount, MaximumCount));
    return X_STATUS_SUCCESS;
}

uint32_t NtReleaseSemaphore(uint32_t handle, uint32_t ReleaseCount, be<int32_t>* PreviousCount)
{
    auto* sem = ResolveAs<Semaphore>(handle);
    if (!sem)
        return X_STATUS_INVALID_HANDLE;

    uint32_t previousCount;
    uint32_t status = sem->Release(ReleaseCount, &previousCount);

    if (PreviousCount != nullptr)
        *PreviousCount = int32_t(previousCount);

    return status;
}

uint32_t NtReleaseMutant(uint32_t handle, be<int32_t>* previousCount)
{
    auto* mutant = ResolveAs<Mutant>(handle);
    if (!mutant)
    {
        LOG_ONCE("NtReleaseMutant: unknown handle 0x{:08X}", handle);
        return X_STATUS_INVALID_HANDLE;
    }

    if (previousCount)
        *previousCount = 0;
    return mutant->Release();
}

// Forgotten Sands: named/unnamed mutexes (the names are ignored, nothing else shares them).
uint32_t NtCreateMutant(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* objAttributes, uint32_t initialOwner)
{
    *handle = GetKernelHandle(CreateKernelObject<Mutant>(!!initialOwner));
    return X_STATUS_SUCCESS;
}

static uint32_t WaitForObjects(std::vector<KernelObject*>& objects, uint32_t waitType, uint32_t timeout)
{
    if (waitType == 0) // WaitAll (approximation: one after another)
    {
        for (auto* obj : objects)
        {
            uint32_t status = obj->Wait(timeout);
            if (status != X_STATUS_SUCCESS)
                return status;
        }
        return X_STATUS_SUCCESS;
    }

    uint32_t signaledIndex = 0;
    MultiWaitScope multiWait;
    uint32_t status = WaitWithTimeout([&]
    {
        for (uint32_t i = 0; i < objects.size(); i++)
        {
            if (objects[i]->Wait(0) == X_STATUS_SUCCESS)
            {
                signaledIndex = i;
                return true;
            }
        }
        return false;
    }, g_signalGeneration, timeout);

    return status == X_STATUS_SUCCESS ? X_STATUS_WAIT_0 + signaledIndex : status;
}

// Forgotten Sands: like KeWaitForMultipleObjects, but with handles.
uint32_t NtWaitForMultipleObjectsEx(uint32_t Count, be<uint32_t>* Handles, uint32_t WaitType, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    const uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);

    thread_local std::vector<KernelObject*> s_objects;
    s_objects.resize(Count);
    for (uint32_t i = 0; i < Count; i++)
    {
        s_objects[i] = ResolveWaitable(Handles[i]);
        if (!s_objects[i])
        {
            LOG_ONCE("NtWaitForMultipleObjectsEx: unknown handle 0x{:08X}", uint32_t(Handles[i]));
            return X_STATUS_INVALID_HANDLE;
        }
    }
    return WaitForObjects(s_objects, WaitType, timeout);
}

uint32_t NtYieldExecution()
{
    CoreGateYield gate;
    std::this_thread::yield();
    return X_STATUS_SUCCESS;
}

uint32_t KeSetDisableBoostThread(uint32_t thread, uint32_t disable)
{
    return 0; // previous value; thread boosting does not exist on the PC
}

uint32_t NtWaitForSingleObjectEx(uint32_t Handle, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);

    if (auto* obj = ResolveWaitable(Handle))
        return obj->Wait(timeout);

    LOG_ONCE("NtWaitForSingleObjectEx: unknown handle 0x{:08X}", Handle);
    return X_STATUS_INVALID_HANDLE;
}

uint32_t KeWaitForSingleObject(XDISPATCHER_HEADER* Object, uint32_t WaitReason, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    const uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);

    if (auto* obj = ResolveWaitable(g_memory.MapVirtual(Object)))
        return obj->Wait(timeout);

    return X_STATUS_INVALID_HANDLE;
}

uint32_t KeWaitForMultipleObjects(uint32_t Count, xpointer<XDISPATCHER_HEADER>* Objects, uint32_t WaitType, uint32_t WaitReason, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout, void* WaitBlockArray)
{
    const uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);

    thread_local std::vector<KernelObject*> s_objects;
    s_objects.resize(Count);
    for (size_t i = 0; i < Count; i++)
    {
        s_objects[i] = ResolveWaitable(Objects[i].ptr.get());
        if (!s_objects[i])
            return X_STATUS_INVALID_PARAMETER;
    }

    if (WaitType == 0) // WaitAll (approximation: one after another)
    {
        for (size_t i = 0; i < Count; i++)
        {
            uint32_t status = s_objects[i]->Wait(timeout);
            if (status != X_STATUS_SUCCESS)
                return status;
        }
        return X_STATUS_SUCCESS;
    }

    // WaitAny
    uint32_t signaledIndex = 0;
    MultiWaitScope multiWait;
    uint32_t status = WaitWithTimeout([&]
    {
        for (uint32_t i = 0; i < Count; i++)
        {
            if (s_objects[i]->Wait(0) == X_STATUS_SUCCESS)
            {
                signaledIndex = i;
                return true;
            }
        }
        return false;
    }, g_signalGeneration, timeout);

    return status == X_STATUS_SUCCESS ? X_STATUS_WAIT_0 + signaledIndex : status;
}

uint32_t KeDelayExecutionThread(uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);
    Perf::Add(Perf::GuestSleep, uint64_t(timeout == X_INFINITE ? 1000 : timeout) * 1000000ull);

    CoreGateYield gate;
    if (timeout == 0)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout == X_INFINITE ? 1000 : timeout));

    return Alertable ? X_STATUS_USER_APC : X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Threads

uint32_t ExCreateThread(be<uint32_t>* handle, uint32_t stackSize, be<uint32_t>* threadId, uint32_t xApiThreadStartup, uint32_t startAddress, uint32_t startContext, uint32_t creationFlags)
{
    LOGFN("ExCreateThread(start=0x{:08X}, ctx=0x{:08X}, startup=0x{:08X}, stack=0x{:X}, flags=0x{:X})",
        startAddress, startContext, xApiThreadStartup, stackSize, creationFlags);

    uint32_t hostThreadId;
    GuestThreadParams params{ startAddress, startContext, creationFlags, xApiThreadStartup, stackSize };

    auto* thread = GuestThread::Start(params, &hostThreadId);
    if (handle)
        *handle = GetKernelHandle(thread);

    if (threadId != nullptr)
        *threadId = hostThreadId;

    return X_STATUS_SUCCESS;
}

void ExTerminateThread(uint32_t exitCode)
{
    GuestThread::Exit(exitCode);
}

uint32_t NtResumeThread(uint32_t handle, be<uint32_t>* suspendCount)
{
    if (!IsKernelObject(handle))
        return X_STATUS_INVALID_HANDLE;

    auto* hThread = dynamic_cast<GuestThreadHandle*>(GetKernelObject(handle));
    if (!hThread)
        return X_STATUS_INVALID_HANDLE;

    if (suspendCount)
        *suspendCount = hThread->suspended ? 1 : 0;

    hThread->suspended = false;
    hThread->suspended.notify_all();
    return X_STATUS_SUCCESS;
}

static GuestThreadHandle* ThreadFromHandle(uint32_t handle)
{
    if (handle == CURRENT_THREAD_HANDLE || !IsKernelObject(handle))
        return nullptr;
    return dynamic_cast<GuestThreadHandle*>(GetKernelObject(handle));
}

uint32_t KeSetBasePriorityThread(uint32_t thread, int32_t priority)
{
    if (thread == CURRENT_THREAD_HANDLE)
    {
        extern int CoreGateCurrentPriority(int set);
        return uint32_t(CoreGateCurrentPriority(priority));
    }
    {
        static std::atomic<int> s_n{0};
        if (s_n++ < 60) { auto* t = ThreadFromHandle(thread); LOGFN("[thread] KeSetBasePriorityThread(handle 0x{:08X}, {}) start 0x{:08X} ctx 0x{:08X}", thread, priority, t ? t->params.function : 0, t ? t->params.value : 0); }
    }
    auto* hThread = ThreadFromHandle(thread);
    int previous = hThread ? hThread->priority : 0;
    if (hThread)
        hThread->priority = priority;
    return uint32_t(previous);
}

uint32_t KeQueryBasePriorityThread(uint32_t thread)
{
    auto* hThread = ThreadFromHandle(thread);
    return hThread ? uint32_t(hThread->priority) : 0;
}

uint32_t KeSetAffinityThread(uint32_t Thread, uint32_t Affinity, be<uint32_t>* lpPreviousAffinity)
{
    extern bool PcrAffinityOn();
    extern int ProcessorOfAffinity(uint32_t affinity);
    const int processor = PcrAffinityOn() ? ProcessorOfAffinity(Affinity) : -1;
    if (Thread == CURRENT_THREAD_HANDLE)
    {
        extern void CoreGateCurrentAffinity(uint32_t affinity);
        CoreGateCurrentAffinity(Affinity);
        if (processor >= 0)
            *((uint8_t*)g_memory.Translate(g_ppcContext->r13.u32) + 0x10C) = uint8_t(processor);
    }
    else if (auto* t = ThreadFromHandle(Thread))
    {
        t->affinity = Affinity;
        if (const uint32_t pcr = t->pcr; pcr != 0 && processor >= 0)
            *((uint8_t*)g_memory.Translate(pcr) + 0x10C) = uint8_t(processor);
    }
    {
        static std::atomic<int> s_n{0};
        if (s_n++ < 60) { auto* t = ThreadFromHandle(Thread); LOGFN("[thread] KeSetAffinityThread(handle 0x{:08X}, affinity 0x{:X}) start 0x{:08X} ctx 0x{:08X}", Thread, Affinity, t ? t->params.function : 0, t ? t->params.value : 0); }
    }
    if (lpPreviousAffinity)
        *lpPreviousAffinity = 2;

    return X_STATUS_SUCCESS;
}

static uint32_t g_processType = 1;

uint32_t KeGetCurrentProcessType() { return g_processType; }
void KeSetCurrentProcessType(uint32_t type) { g_processType = type; }

void KeEnterCriticalRegion() { }
void KeLeaveCriticalRegion() { }
void KeEnableFpuExceptions(uint32_t enabled) { }
void KeLockL2() { }
void KeUnlockL2() { }
uint32_t KiApcNormalRoutineNop() { return 0; }

// APC / DPC: there is no interrupt or APC delivery in the runtime. Queued routines are run
// immediately on the calling thread, which is the closest synchronous equivalent.
struct XKDPC
{
    be<uint16_t> Type;
    uint8_t InsertedNumber;
    uint8_t TargetNumber;
    XLIST_ENTRY DpcListEntry;
    be<uint32_t> DeferredRoutine;
    be<uint32_t> DeferredContext;
    be<uint32_t> SystemArgument1;
    be<uint32_t> SystemArgument2;
};

void KeInitializeDpc(XKDPC* dpc, uint32_t routine, uint32_t context)
{
    memset(dpc, 0, sizeof(*dpc));
    dpc->Type = 19;
    dpc->DeferredRoutine = routine;
    dpc->DeferredContext = context;
}

uint32_t KeInsertQueueDpc(XKDPC* dpc, uint32_t arg1, uint32_t arg2)
{
    LOG_ONCE("KeInsertQueueDpc: running DPC routine 0x{:08X} synchronously", dpc->DeferredRoutine.get());
    dpc->SystemArgument1 = arg1;
    dpc->SystemArgument2 = arg2;
    if (dpc->DeferredRoutine)
        GuestToHostFunction<void>(dpc->DeferredRoutine.get(), dpc, dpc->DeferredContext.get(), arg1, arg2);
    return 1;
}

struct XKAPC
{
    be<uint16_t> Type;
    uint8_t ApcMode;
    uint8_t Inserted;
    be<uint32_t> Thread;
    XLIST_ENTRY ApcListEntry;
    be<uint32_t> KernelRoutine;
    be<uint32_t> RundownRoutine;
    be<uint32_t> NormalRoutine;
    be<uint32_t> NormalContext;
    be<uint32_t> SystemArgument1;
    be<uint32_t> SystemArgument2;
};

void KeInitializeApc(XKAPC* apc, uint32_t thread, uint32_t kernelRoutine, uint32_t rundownRoutine, uint32_t normalRoutine, uint32_t processorMode, uint32_t normalContext)
{
    memset(apc, 0, sizeof(*apc));
    apc->Type = 18;
    apc->ApcMode = uint8_t(processorMode);
    apc->Thread = thread;
    apc->KernelRoutine = kernelRoutine;
    apc->RundownRoutine = rundownRoutine;
    apc->NormalRoutine = normalRoutine;
    apc->NormalContext = normalContext;
}

uint32_t KeInsertQueueApc(XKAPC* apc, uint32_t arg1, uint32_t arg2, uint32_t increment)
{
    LOG_ONCE("KeInsertQueueApc: running APC normal routine 0x{:08X} synchronously", apc->NormalRoutine.get());
    apc->SystemArgument1 = arg1;
    apc->SystemArgument2 = arg2;
    uint32_t routine = apc->NormalRoutine;
    if (routine != 0)
        GuestToHostFunction<void>(routine, apc->NormalContext.get(), arg1, arg2);
    return 1;
}

// ---------------------------------------------------------------------------------------
// TLS (host thread_local, as in UnleashedRecomp)

static std::vector<size_t> g_tlsFreeIndices;
static size_t g_tlsNextIndex = 0;
static Mutex g_tlsAllocationMutex;

static uint32_t& KeTlsGetValueRef(size_t index)
{
    // Step 11: fixed slots for the usual small indices (KeTlsGetValue is called very often by the
    // guest CRT / Ogre; ~2% of a zone load went into the vector path)
    thread_local uint32_t s_fastValues[64]{};
    if (index < 64)
        return s_fastValues[index];
    thread_local std::vector<uint32_t> s_tlsValues;

    if (s_tlsValues.size() <= index)
        s_tlsValues.resize(index + 1, 0);

    return s_tlsValues[index];
}

uint32_t KeTlsGetValue(uint32_t dwTlsIndex)
{
    return KeTlsGetValueRef(dwTlsIndex);
}

uint32_t KeTlsSetValue(uint32_t dwTlsIndex, uint32_t lpTlsValue)
{
    KeTlsGetValueRef(dwTlsIndex) = lpTlsValue;
    return TRUE;
}

uint32_t KeTlsAlloc()
{
    std::lock_guard<Mutex> lock(g_tlsAllocationMutex);
    if (!g_tlsFreeIndices.empty())
    {
        size_t index = g_tlsFreeIndices.back();
        g_tlsFreeIndices.pop_back();
        return index;
    }

    return g_tlsNextIndex++;
}

uint32_t KeTlsFree(uint32_t dwTlsIndex)
{
    std::lock_guard<Mutex> lock(g_tlsAllocationMutex);
    g_tlsFreeIndices.push_back(dwTlsIndex);
    return TRUE;
}

// ---------------------------------------------------------------------------------------
// Critical sections and spinlocks (UnleashedRecomp)

void RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    // Step 35 diag: a leave by a thread that does not own the section (or one too many) corrupts it for good.
    {
        const uint32_t owner = std::atomic_ref(cs->OwningThread).load();
        if (owner != g_ppcContext->r13.u32 || cs->RecursionCount <= 0)
        {
            static std::atomic<int> s_n{0};
            if (s_n++ < 20)
            {
                extern std::string CurrentGuestCallChain(int maxFrames);
                LOGFN("[cs] leave of 0x{:08X} by r13 0x{:08X}: owner 0x{:08X} recursion {} |{}", uint32_t((uint8_t*)cs - g_memory.base), g_ppcContext->r13.u32, owner, cs->RecursionCount, CurrentGuestCallChain(12));
            }
            if (owner != g_ppcContext->r13.u32)
                return;
        }
    }
    cs->RecursionCount--;

    if (cs->RecursionCount != 0)
        return;

    std::atomic_ref owningThread(cs->OwningThread);
    owningThread.store(0);
#ifdef _WIN32
    owningThread.notify_one();
#else
    AddressWakeOne(&cs->OwningThread);
#endif
}

void RtlEnterCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    uint32_t thisThread = g_ppcContext->r13.u32;
    assert(thisThread != NULL);

    std::atomic_ref owningThread(cs->OwningThread);

    auto waitStart = std::chrono::steady_clock::time_point{};
    bool logged = false;
    while (true)
    {
        uint32_t previousOwner = 0;

        if (owningThread.compare_exchange_weak(previousOwner, thisThread) || previousOwner == thisThread)
        {
            cs->RecursionCount++;
            return;
        }

        CoreGateYield gate; // Step 37: give the core's gate away while blocked
        // Step 35 diag: report a wait of more than 3 s (deadlock hunting) once, then keep waiting.
        if (!logged)
        {
            if (waitStart == std::chrono::steady_clock::time_point{})
                waitStart = std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(3))
            {
                logged = true;
                extern std::string CurrentGuestCallChain(int maxFrames);
                LOGFN("[cs] r13 0x{:08X} waits >3 s for 0x{:08X} owned by 0x{:08X} (recursion {}) |{}", thisThread, uint32_t((uint8_t*)cs - g_memory.base), previousOwner, cs->RecursionCount, CurrentGuestCallChain(40));
            }
#ifdef _WIN32
            WaitOnAddress(&cs->OwningThread, &previousOwner, sizeof(previousOwner), 1000);
#else
            AddressWait(&cs->OwningThread, previousOwner, 1000);
#endif
        }
        else
#ifdef _WIN32
            owningThread.wait(previousOwner);
#else
            AddressWait(&cs->OwningThread, previousOwner, 1000);
#endif
    }
}

uint32_t RtlTryEnterCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    uint32_t thisThread = g_ppcContext->r13.u32;
    std::atomic_ref owningThread(cs->OwningThread);

    uint32_t previousOwner = 0;
    if (owningThread.compare_exchange_weak(previousOwner, thisThread) || previousOwner == thisThread)
    {
        cs->RecursionCount++;
        return true;
    }

    return false;
}

uint32_t RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    cs->Header.Absolute = 0;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    return 0;
}

uint32_t RtlInitializeCriticalSectionAndSpinCount(XRTL_CRITICAL_SECTION* cs, uint32_t spinCount)
{
    cs->Header.Absolute = (spinCount + 255) >> 8;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    return 0;
}

static void SpinAcquire(uint32_t* spinLock)
{
    std::atomic_ref spinLockRef(*spinLock);
    while (true)
    {
        uint32_t expected = 0;
        if (spinLockRef.compare_exchange_weak(expected, g_ppcContext->r13.u32))
            break;
        CoreGateYield gate;
        std::this_thread::yield();
    }
}

static void SpinRelease(uint32_t* spinLock)
{
    std::atomic_ref spinLockRef(*spinLock);
    spinLockRef = 0;
}

uint32_t KfAcquireSpinLock(uint32_t* spinLock) { SpinAcquire(spinLock); return 0; }
void KfReleaseSpinLock(uint32_t* spinLock, uint32_t oldIrql) { SpinRelease(spinLock); }
void KeAcquireSpinLockAtRaisedIrql(uint32_t* spinLock) { SpinAcquire(spinLock); }
void KeReleaseSpinLockFromRaisedIrql(uint32_t* spinLock) { SpinRelease(spinLock); }

// ---------------------------------------------------------------------------------------
// Time

// The console's timebase (mftb) runs at 49.875 MHz and KeQueryPerformanceFrequency reports that.
// Recompiled mftb calls PPCQueryTimeBase() (see the XenonRecomp patch), which scales the host's
// high-resolution clock to the same 49.875 MHz so guest timers run at real speed.
static constexpr uint64_t GUEST_TIMEBASE_FREQUENCY = 49875000;

uint64_t KeQueryPerformanceFrequency()
{
    return GUEST_TIMEBASE_FREQUENCY;
}

extern "C" uint64_t PPCQueryTimeBase() // C linkage: declared at block scope inside the extern "C" recompiled functions
{
    static const uint64_t s_hostFrequency = SDL_GetPerformanceFrequency();
    static const uint64_t s_hostStart = SDL_GetPerformanceCounter();
    const uint64_t ticks = SDL_GetPerformanceCounter() - s_hostStart;
    return (ticks / s_hostFrequency) * GUEST_TIMEBASE_FREQUENCY + ((ticks % s_hostFrequency) * GUEST_TIMEBASE_FREQUENCY) / s_hostFrequency;
}

static int64_t HostSystemTime100ns()
{
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    auto timeSinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timeSinceEpoch).count() + FILETIME_EPOCH_DIFFERENCE;
}

void KeQuerySystemTime(be<uint64_t>* time)
{
    *time = uint64_t(HostSystemTime100ns());
}

// days from civil, Howard Hinnant's algorithm
static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

static void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

static const int64_t DAYS_1601_TO_1970 = 134774;

void RtlTimeToTimeFields(be<int64_t>* time, XTIME_FIELDS* fields)
{
    int64_t t = *time;
    int64_t ms = t / 10000;
    int64_t days = ms / 86400000;
    int64_t msOfDay = ms % 86400000;

    int64_t y; unsigned m, d;
    CivilFromDays(days - DAYS_1601_TO_1970, y, m, d);

    fields->Year = uint16_t(y);
    fields->Month = uint16_t(m);
    fields->Day = uint16_t(d);
    fields->Hour = uint16_t(msOfDay / 3600000);
    fields->Minute = uint16_t((msOfDay / 60000) % 60);
    fields->Second = uint16_t((msOfDay / 1000) % 60);
    fields->Milliseconds = uint16_t(msOfDay % 1000);
    fields->Weekday = uint16_t((days + 1) % 7); // 1601-01-01 was a Monday
}

uint32_t RtlTimeFieldsToTime(XTIME_FIELDS* fields, be<int64_t>* time)
{
    if (fields->Month < 1 || fields->Month > 12 || fields->Day < 1 || fields->Day > 31)
        return false;

    int64_t days = DaysFromCivil(fields->Year, fields->Month, fields->Day) + DAYS_1601_TO_1970;
    int64_t ms = days * 86400000 + int64_t(fields->Hour) * 3600000 + int64_t(fields->Minute) * 60000 +
        int64_t(fields->Second) * 1000 + fields->Milliseconds;
    *time = ms * 10000;
    return true;
}

// ---------------------------------------------------------------------------------------
// Strings

struct XUNICODE_STRING
{
    be<uint16_t> Length;
    be<uint16_t> MaximumLength;
    xpointer<be<uint16_t>> Buffer;
};

void RtlInitAnsiString(XANSI_STRING* destination, char* source)
{
    const uint16_t length = source ? (uint16_t)strlen(source) : 0;
    destination->Length = length;
    destination->MaximumLength = source ? length + 1 : 0;
    destination->Buffer = source;
}

void RtlInitUnicodeString(XUNICODE_STRING* destination, be<uint16_t>* source)
{
    uint16_t length = 0;
    if (source)
        while (source[length] != 0) length++;

    destination->Length = length * 2;
    destination->MaximumLength = source ? (length + 1) * 2 : 0;
    destination->Buffer = source;
}

void RtlFreeAnsiString(XANSI_STRING* string)
{
    if (string->Buffer.ptr.get() != 0)
        g_userHeap.Free(string->Buffer.get());

    string->Buffer.ptr = 0;
    string->Length = 0;
    string->MaximumLength = 0;
}

uint32_t RtlUnicodeStringToAnsiString(XANSI_STRING* destination, XUNICODE_STRING* source, uint32_t allocate)
{
    const uint16_t chars = source->Length / 2;

    if (allocate)
    {
        auto* buffer = (char*)g_userHeap.Alloc(chars + 1);
        destination->Buffer = buffer;
        destination->MaximumLength = chars + 1;
    }
    else if (destination->MaximumLength < chars + 1)
    {
        return X_STATUS_BUFFER_OVERFLOW;
    }

    char* out = destination->Buffer.get();
    const be<uint16_t>* in = source->Buffer.get();
    for (uint16_t i = 0; i < chars; i++)
    {
        uint16_t c = in[i];
        out[i] = c < 256 ? char(c) : '?';
    }
    out[chars] = 0;
    destination->Length = chars;
    return X_STATUS_SUCCESS;
}

uint32_t RtlUnicodeToMultiByteN(char* MultiByteString, uint32_t MaxBytesInMultiByteString, be<uint32_t>* BytesInMultiByteString, const be<uint16_t>* UnicodeString, uint32_t BytesInUnicodeString)
{
    const auto reqSize = BytesInUnicodeString / sizeof(uint16_t);
    const auto count = std::min<uint32_t>(reqSize, MaxBytesInMultiByteString);

    for (size_t i = 0; i < count; i++)
    {
        const auto c = UnicodeString[i].get();
        MultiByteString[i] = c < 256 ? c : '?';
    }

    if (BytesInMultiByteString)
        *BytesInMultiByteString = count;

    return X_STATUS_SUCCESS;
}

uint32_t RtlMultiByteToUnicodeN(be<uint16_t>* UnicodeString, uint32_t MaxBytesInUnicodeString, be<uint32_t>* BytesInUnicodeString, const char* MultiByteString, uint32_t BytesInMultiByteString)
{
    uint32_t length = std::min(MaxBytesInUnicodeString / 2, BytesInMultiByteString);

    for (size_t i = 0; i < length; i++)
        UnicodeString[i] = uint8_t(MultiByteString[i]);

    if (BytesInUnicodeString != nullptr)
        *BytesInUnicodeString = length * 2;

    return X_STATUS_SUCCESS;
}

uint32_t RtlCompareMemoryUlong(be<uint32_t>* source, uint32_t length, uint32_t pattern)
{
    uint32_t matched = 0;
    for (uint32_t i = 0; i < length / 4; i++)
    {
        if (source[i].get() != pattern)
            break;
        matched += 4;
    }
    return matched;
}

void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
{
    for (uint32_t i = 0; i < length / 4; i++)
        destination[i] = pattern;
}

uint32_t RtlNtStatusToDosError(uint32_t status)
{
    switch (status)
    {
    case X_STATUS_SUCCESS: return 0;
    case X_STATUS_PENDING: return 997;                   // ERROR_IO_PENDING
    case X_STATUS_TIMEOUT: return 1460;                  // ERROR_TIMEOUT
    case X_STATUS_BUFFER_OVERFLOW: return 234;           // ERROR_MORE_DATA
    case X_STATUS_NO_MORE_FILES: return 18;              // ERROR_NO_MORE_FILES
    case X_STATUS_UNSUCCESSFUL: return 31;               // ERROR_GEN_FAILURE
    case X_STATUS_NOT_IMPLEMENTED: return 1;             // ERROR_INVALID_FUNCTION
    case X_STATUS_INVALID_HANDLE: return 6;              // ERROR_INVALID_HANDLE
    case X_STATUS_INVALID_PARAMETER: return 87;          // ERROR_INVALID_PARAMETER
    case X_STATUS_NO_SUCH_FILE: return 2;                // ERROR_FILE_NOT_FOUND
    case X_STATUS_END_OF_FILE: return 38;                // ERROR_HANDLE_EOF
    case X_STATUS_NO_MEMORY: return 8;                   // ERROR_NOT_ENOUGH_MEMORY
    case X_STATUS_ACCESS_DENIED: return 5;               // ERROR_ACCESS_DENIED
    case X_STATUS_BUFFER_TOO_SMALL: return 122;          // ERROR_INSUFFICIENT_BUFFER
    case X_STATUS_OBJECT_NAME_INVALID: return 123;       // ERROR_INVALID_NAME
    case X_STATUS_OBJECT_NAME_NOT_FOUND: return 2;       // ERROR_FILE_NOT_FOUND
    case X_STATUS_OBJECT_NAME_COLLISION: return 183;     // ERROR_ALREADY_EXISTS
    case X_STATUS_OBJECT_PATH_NOT_FOUND: return 3;       // ERROR_PATH_NOT_FOUND
    case X_STATUS_FILE_IS_A_DIRECTORY: return 5;
    case X_STATUS_NOT_A_DIRECTORY: return 267;           // ERROR_DIRECTORY
    case X_STATUS_DEVICE_NOT_CONNECTED: return 1167;     // ERROR_DEVICE_NOT_CONNECTED
    default:
        if ((status & 0xFFFF0000) == 0x80070000)
            return status & 0xFFFF;
        return 317; // ERROR_MR_MID_NOT_FOUND
    }
}

// ---------------------------------------------------------------------------------------
// Exceptions / debug / bugcheck

struct XEXCEPTION_RECORD
{
    be<uint32_t> ExceptionCode;
    be<uint32_t> ExceptionFlags;
    be<uint32_t> ExceptionRecord;
    be<uint32_t> ExceptionAddress;
    be<uint32_t> NumberParameters;
    be<uint32_t> ExceptionInformation[15];
};

void RtlRaiseException_x(XEXCEPTION_RECORD* record)
{
    const uint32_t code = record->ExceptionCode;

    if (code == 0x406D1388) // SetThreadName
    {
        if (record->NumberParameters >= 2 && record->ExceptionInformation[1] != 0)
            LOGFN("Guest thread name: {}", (const char*)g_memory.Translate(record->ExceptionInformation[1]));
        return;
    }

    if (code == 0xE06D7363) // C++ throw
    {
        LOGFN_ERROR("RtlRaiseException: guest C++ exception thrown (object 0x{:08X}, type info 0x{:08X}). "
            "SEH unwinding is not supported by the recompiled code; continuing after the throw.",
            record->NumberParameters >= 2 ? record->ExceptionInformation[1].get() : 0,
            record->NumberParameters >= 3 ? record->ExceptionInformation[2].get() : 0);
        return;
    }

    LOGFN_ERROR("RtlRaiseException: code 0x{:08X} flags 0x{:X} address 0x{:08X}", code, record->ExceptionFlags.get(), record->ExceptionAddress.get());
}

void DbgPrint(const char* format)
{
    std::string text = GuestFormat(format, GuestArgs::FromRegisters(*GetPPCContext(), 1));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    LOGFN("[DbgPrint] {}", text);
}

void DbgBreakPoint()
{
    LOG_ONCE("DbgBreakPoint called by the game (ignored)");
}

[[noreturn]] static void FatalExit(const std::string& message)
{
    LOGFN_ERROR("{}", message);
    os::logger::Flush();
    std::_Exit(1);
}

void KeBugCheck(uint32_t code)
{
    FatalExit(fmt::format("KeBugCheck(0x{:08X}) - the game hit a fatal error", code));
}

void KeBugCheckEx(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4)
{
    FatalExit(fmt::format("KeBugCheckEx(0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})", code, p1, p2, p3, p4));
}

void HalReturnToFirmware(uint32_t routine)
{
    LOGFN("HalReturnToFirmware({}) - the game asked to quit", routine);
    os::logger::Flush();
    std::_Exit(0);
}

void ExRegisterTitleTerminateNotification(void* registration, uint32_t create) { }

// ---------------------------------------------------------------------------------------
// XConfig

uint32_t ExGetXConfigSetting(uint16_t Category, uint16_t Setting, void* Buffer, uint16_t SizeOfBuffer, be<uint32_t>* RequiredSize)
{
    uint32_t data[4]{};
    uint32_t size = 4;

    switch (Category)
    {
        case 0x0002: // XCONFIG_SECURED_CATEGORY
            switch (Setting)
            {
                case 0x0002: data[0] = ByteSwap(0x00001000); break; // AV region: USA/Canada
                default: LOGFN_WARNING("ExGetXConfigSetting: unknown setting {}/{}", Category, Setting); return X_STATUS_INVALID_PARAMETER;
            }
            break;

        case 0x0003: // XCONFIG_USER_CATEGORY
            switch (Setting)
            {
                case 0x0001: case 0x0002: case 0x0003: case 0x0004:
                case 0x0005: case 0x0006: case 0x0007:
                    data[0] = 0; break; // time zone
                case 0x0009: data[0] = ByteSwap(Config::Language); break;  // language
                case 0x000A: data[0] = ByteSwap(0x00040000); break;         // video flags: widescreen
                case 0x000B: data[0] = 0; break;                            // audio flags: stereo
                case 0x000C: data[0] = ByteSwap(1); break;                  // retail flags
                case 0x000E: data[0] = ByteSwap(103); size = 1; data[0] = 103; break; // country (1 byte): US
                case 0x000F: data[0] = ByteSwap(0xFFFFFFFF); break;         // parental control
                default: LOGFN_WARNING("ExGetXConfigSetting: unknown setting {}/{}", Category, Setting); return X_STATUS_INVALID_PARAMETER;
            }
            break;

        default:
            LOGFN_WARNING("ExGetXConfigSetting: unknown category {}/{}", Category, Setting);
            return X_STATUS_INVALID_PARAMETER;
    }

    if (RequiredSize)
        *RequiredSize = size;

    if (Buffer)
    {
        if (SizeOfBuffer < size)
            return X_STATUS_BUFFER_TOO_SMALL;
        memcpy(Buffer, data, std::min<size_t>(SizeOfBuffer, size));
    }

    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Audio (render driver from UnleashedRecomp apu/; XMA decoding comes later)

uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, be<float>* volume)
{
    if (volume)
        *volume = 1.0f;
    return 0;
}

uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
{
    *config = 0x00010001;
    return 0;
}

// XMACreateContext / XMAReleaseContext and the other XMA* imports: apu/xma.cpp

// ---------------------------------------------------------------------------------------
// Unsupported low-level things

uint32_t MicDeviceRequest(uint32_t a1, uint32_t a2, uint32_t a3) { return X_STATUS_DEVICE_NOT_CONNECTED; }
uint32_t RmcDeviceRequest(uint32_t a1, uint32_t a2, uint32_t a3) { return X_STATUS_DEVICE_NOT_CONNECTED; }

GUEST_IMPORT(NtClose, NtClose);
GUEST_IMPORT(NtDuplicateObject, NtDuplicateObject);
GUEST_IMPORT(ObReferenceObjectByHandle, ObReferenceObjectByHandle);
GUEST_IMPORT(ObReferenceObject, ObReferenceObject);
GUEST_IMPORT(ObDereferenceObject, ObDereferenceObject);
GUEST_IMPORT(NtCreateEvent, NtCreateEvent);
GUEST_IMPORT(NtSetEvent, NtSetEvent);
GUEST_IMPORT(NtClearEvent, NtClearEvent);
GUEST_IMPORT(KeSetEvent, KeSetEvent);
GUEST_IMPORT(KeResetEvent, KeResetEvent);
GUEST_IMPORT(NtCreateSemaphore, NtCreateSemaphore);
GUEST_IMPORT(NtReleaseSemaphore, NtReleaseSemaphore);
GUEST_IMPORT(NtReleaseMutant, NtReleaseMutant);
GUEST_IMPORT(NtCreateMutant, NtCreateMutant);
GUEST_IMPORT(NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx);
GUEST_IMPORT(NtYieldExecution, NtYieldExecution);
GUEST_IMPORT(KeSetDisableBoostThread, KeSetDisableBoostThread);
GUEST_IMPORT(NtWaitForSingleObjectEx, NtWaitForSingleObjectEx);
GUEST_IMPORT(KeWaitForSingleObject, KeWaitForSingleObject);
GUEST_IMPORT(KeWaitForMultipleObjects, KeWaitForMultipleObjects);
GUEST_IMPORT(KeDelayExecutionThread, KeDelayExecutionThread);
GUEST_IMPORT(ExCreateThread, ExCreateThread);
GUEST_IMPORT(ExTerminateThread, ExTerminateThread);
GUEST_IMPORT(NtResumeThread, NtResumeThread);
GUEST_IMPORT(KeSetBasePriorityThread, KeSetBasePriorityThread);
GUEST_IMPORT(KeQueryBasePriorityThread, KeQueryBasePriorityThread);
GUEST_IMPORT(KeSetAffinityThread, KeSetAffinityThread);
GUEST_IMPORT(KeGetCurrentProcessType, KeGetCurrentProcessType);
GUEST_IMPORT(KeSetCurrentProcessType, KeSetCurrentProcessType);
GUEST_IMPORT(KeEnterCriticalRegion, KeEnterCriticalRegion);
GUEST_IMPORT(KeLeaveCriticalRegion, KeLeaveCriticalRegion);
GUEST_IMPORT(KeEnableFpuExceptions, KeEnableFpuExceptions);
GUEST_IMPORT(KeLockL2, KeLockL2);
GUEST_IMPORT(KeUnlockL2, KeUnlockL2);
GUEST_IMPORT(KiApcNormalRoutineNop, KiApcNormalRoutineNop);
GUEST_IMPORT(KeInitializeDpc, KeInitializeDpc);
GUEST_IMPORT(KeInsertQueueDpc, KeInsertQueueDpc);
GUEST_IMPORT(KeInitializeApc, KeInitializeApc);
GUEST_IMPORT(KeInsertQueueApc, KeInsertQueueApc);
GUEST_IMPORT(KeTlsGetValue, KeTlsGetValue);
GUEST_IMPORT(KeTlsSetValue, KeTlsSetValue);
GUEST_IMPORT(KeTlsAlloc, KeTlsAlloc);
GUEST_IMPORT(KeTlsFree, KeTlsFree);
GUEST_IMPORT(RtlLeaveCriticalSection, RtlLeaveCriticalSection);
GUEST_IMPORT(RtlEnterCriticalSection, RtlEnterCriticalSection);
GUEST_IMPORT(RtlTryEnterCriticalSection, RtlTryEnterCriticalSection);
GUEST_IMPORT(RtlInitializeCriticalSection, RtlInitializeCriticalSection);
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount, RtlInitializeCriticalSectionAndSpinCount);
GUEST_IMPORT(KfAcquireSpinLock, KfAcquireSpinLock);
GUEST_IMPORT(KfReleaseSpinLock, KfReleaseSpinLock);
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql);
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql);
GUEST_IMPORT(KeQueryPerformanceFrequency, KeQueryPerformanceFrequency);
GUEST_IMPORT(KeQuerySystemTime, KeQuerySystemTime);
GUEST_IMPORT(RtlTimeToTimeFields, RtlTimeToTimeFields);
GUEST_IMPORT(RtlTimeFieldsToTime, RtlTimeFieldsToTime);
GUEST_IMPORT(RtlInitAnsiString, RtlInitAnsiString);
GUEST_IMPORT(RtlInitUnicodeString, RtlInitUnicodeString);
GUEST_IMPORT(RtlFreeAnsiString, RtlFreeAnsiString);
GUEST_IMPORT(RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString);
GUEST_IMPORT(RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN);
GUEST_IMPORT(RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN);
GUEST_IMPORT(RtlCompareMemoryUlong, RtlCompareMemoryUlong);
GUEST_IMPORT(RtlFillMemoryUlong, RtlFillMemoryUlong);
GUEST_IMPORT(RtlNtStatusToDosError, RtlNtStatusToDosError);
GUEST_IMPORT(RtlRaiseException, RtlRaiseException_x);
GUEST_IMPORT(DbgPrint, DbgPrint);
GUEST_IMPORT(DbgBreakPoint, DbgBreakPoint);
GUEST_IMPORT(KeBugCheck, KeBugCheck);
GUEST_IMPORT(KeBugCheckEx, KeBugCheckEx);
GUEST_IMPORT(HalReturnToFirmware, HalReturnToFirmware);
GUEST_IMPORT(ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification);
GUEST_IMPORT(ExGetXConfigSetting, ExGetXConfigSetting);
GUEST_IMPORT(XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume);
GUEST_IMPORT(XAudioGetSpeakerConfig, XAudioGetSpeakerConfig);


GUEST_IMPORT(XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient);
GUEST_IMPORT(XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient);
GUEST_IMPORT(XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame);
GUEST_IMPORT(MicDeviceRequest, MicDeviceRequest);
GUEST_IMPORT(RmcDeviceRequest, RmcDeviceRequest);

// SEH runtime support.
//
// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue). The game only reaches it
// through the CRT's _local_unwind (Torchlight 0x8286F530, Forgotten Sands 0x829B1250), used by _fsopen/_wfsopen to leave a __try
// early ("return" inside __try): the __finally handlers of the scopes being left must run
// (they unlock the FILE stream). Recompiled code has no guest PC, so we find the scopes from
// .pdata + the __C_specific_handler scope table of the function that contains TargetIp: every
// __finally scope that does not contain TargetIp but contains a call to _local_unwind.
// After the handlers ran, the caller simply continues (in the recompiled code the call to
// _local_unwind is followed by the jump to TargetIp).
static constexpr uint32_t GUEST_LOCAL_UNWIND = 0x829B1250;       // FS: only RtlUnwind caller with li r5,0 / li r6,0
static constexpr uint32_t GUEST_C_SPECIFIC_HANDLER_THUNK = 0x82DE4DE4; // FS: handler of 64 .pdata functions (xexdiag seh)

static bool FindRuntimeFunction(uint32_t ip, uint32_t& begin, uint32_t& flags)
{
    // PE header of the loaded image (little endian), exception directory = data directory 3.
    const uint8_t* image = (const uint8_t*)g_memory.Translate(0x82000000);
    const uint32_t lfanew = *(const uint32_t*)(image + 0x3C);
    const uint8_t* opt = image + lfanew + 24;
    const uint32_t rva = *(const uint32_t*)(opt + 96 + 3 * 8);
    const uint32_t size = *(const uint32_t*)(opt + 96 + 3 * 8 + 4);
    auto* entries = (const be<uint32_t>*)g_memory.Translate(0x82000000 + rva);
    size_t lo = 0, hi = size / 8;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        uint32_t b = entries[mid * 2];
        uint32_t f = entries[mid * 2 + 1];
        uint32_t len = ((f >> 8) & 0x3FFFFF) * 4;
        if (ip < b) hi = mid;
        else if (ip >= b + len) lo = mid + 1;
        else { begin = b; flags = f; return true; }
    }
    return false;
}

static bool RangeCallsLocalUnwind(uint32_t start, uint32_t end)
{
    for (uint32_t a = start; a < end; a += 4)
    {
        uint32_t ins = *(be<uint32_t>*)g_memory.Translate(a);
        if ((ins >> 26) == 18 && (ins & 3) == 1)
        {
            int32_t off = int32_t(ins & 0x3FFFFFC);
            if (off & 0x2000000) off -= 0x4000000;
            if (a + off == GUEST_LOCAL_UNWIND)
                return true;
        }
    }
    return false;
}

PPC_FUNC(__imp__RtlUnwind)
{
    static std::atomic<bool> s_logged;
    const uint32_t targetFrame = ctx.r3.u32;
    const uint32_t targetIp = ctx.r4.u32;
    const uint32_t exceptionRecord = ctx.r5.u32;
    const bool first = !s_logged.exchange(true);
    if (first)
        ImportTrace("RtlUnwind");

    uint32_t begin = 0, flags = 0;
    if (exceptionRecord != 0 || !FindRuntimeFunction(targetIp, begin, flags) || (flags >> 31) == 0 ||
        *(be<uint32_t>*)g_memory.Translate(begin - 8) != GUEST_C_SPECIFIC_HANDLER_THUNK)
    {
        LOGFN_WARNING("RtlUnwind(frame 0x{:08X}, ip 0x{:08X}, record 0x{:08X}) - not a local __try unwind, ignored",
            targetFrame, targetIp, exceptionRecord);
        ctx.r3.u64 = 0;
        return;
    }

    const uint32_t scopeTable = *(be<uint32_t>*)g_memory.Translate(begin - 4);
    const uint32_t count = *(be<uint32_t>*)g_memory.Translate(scopeTable);
    for (uint32_t i = 0; i < count; i++)
    {
        auto* s = (be<uint32_t>*)g_memory.Translate(scopeTable + 4 + i * 16);
        const uint32_t sBegin = s[0], sEnd = s[1], handler = s[2], jumpTarget = s[3];
        if (jumpTarget != 0 || (targetIp >= sBegin && targetIp < sEnd) || !RangeCallsLocalUnwind(sBegin, sEnd))
            continue;

        PPCFunc* func = g_memory.FindFunction(handler);
        if (first)
            LOGFN("RtlUnwind: local unwind in function 0x{:08X} -> __finally 0x{:08X} (frame 0x{:08X})", begin, handler, targetFrame);
        if (!func)
        {
            LOGFN_ERROR("RtlUnwind: __finally handler 0x{:08X} is not a recompiled function", handler);
            continue;
        }

        PPCContext c = ctx;
        c.r3.u64 = 1; // AbnormalTermination
        c.r4.u64 = targetFrame;
        c.r12.u64 = targetFrame; // MSVC PPC funclets take the establisher frame in r12
        func(c, base);
    }

    ctx.r3.u64 = 0;
}
GUEST_IMPORT_STUB(RtlCaptureContext, 0);
GUEST_IMPORT_STUB(__C_specific_handler, 1); // ExceptionContinueSearch
