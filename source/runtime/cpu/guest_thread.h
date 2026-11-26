#pragma once

#include <kernel/xdm.h>

// Guest threads (from UnleashedRecomp). Changes for Torchlight:
//  - ExCreateThread's XapiThreadStartup routine is honoured (CRT per-thread init)
//  - per-thread stack size, minimum 256 KB
//  - ExTerminateThread support (GuestThread::Exit)
//  - Sonic-specific thread hooks (SetThreadName etc. at sub_82xxxxxx) removed

#define CURRENT_THREAD_HANDLE uint32_t(-2)

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* thread = nullptr;
    size_t totalSize = 0;

    GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize = 0);
    ~GuestThreadContext();
};

struct GuestThreadParams
{
    uint32_t function;
    uint32_t value;
    uint32_t flags;
    uint32_t startup = 0;    // XapiThreadStartup (called as startup(function, value)) or 0
    uint32_t stackSize = 0;
};

struct GuestThreadHandle : KernelObject
{
    GuestThreadParams params;
    std::atomic<bool> suspended;
    std::atomic<bool> finished{ false };
    uint32_t exitCode = 0;
    int priority = 0;
    std::atomic<uint32_t> pcr{0}; // Step 37: guest address of the running thread's PCR (processor number at +0x10C)
    uint32_t affinity = 0; // Step 37: KeSetAffinityThread before the thread started (core gate, imports.cpp)
    std::thread thread;

    GuestThreadHandle(const GuestThreadParams& params);
    ~GuestThreadHandle() override;

    uint32_t GetThreadId() const;

    uint32_t Wait(uint32_t timeout) override;
};

struct GuestThread
{
    static uint32_t Start(const GuestThreadParams& params);
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);
    [[noreturn]] static void Exit(uint32_t exitCode);
    static void DumpAll(); // logs every guest thread and where its host code is (stall report)

    static uint32_t GetCurrentThreadId();
    static void SetLastError(uint32_t error);
};
