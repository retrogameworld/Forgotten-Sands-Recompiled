#pragma once

// Host-side dispatcher objects (Event/Semaphore from UnleashedRecomp's imports.cpp, extended
// with finite timeouts and a Mutant type).

#include "xdm.h"
#include "xstatus.h"

uint32_t WaitWithTimeout(const std::function<bool()>& tryAcquire, std::atomic<uint32_t>& notifier, uint32_t timeoutMs);

// Bumped on every signal of any object while a multi-object wait is running (those sleep on it).
extern std::atomic<uint32_t> g_signalGeneration;
extern std::atomic<uint32_t> g_multiWaiters;
void NotifySignal();
// Step 35 (60 fps): single-object waits sleep on their object's own counter. Every signal used to wake every
// waiting guest thread (one global counter): ~20 threads re-checking on each SetEvent cost the render thread
// ~10% and delayed the job threads.
void NotifySignal(std::atomic<uint32_t>& objectGeneration);

struct Event final : KernelObject, HostObject<XKEVENT>
{
    bool manualReset;
    std::atomic<bool> signaled;

    Event(XKEVENT* header)
        : manualReset(!header->Type), signaled(!!header->SignalState)
    {
    }

    Event(bool manualReset, bool initialState)
        : manualReset(manualReset), signaled(initialState)
    {
    }

    bool TryAcquire()
    {
        if (manualReset)
            return signaled.load();

        bool expected = true;
        return signaled.compare_exchange_strong(expected, false);
    }

    std::atomic<uint32_t> generation{0};

    uint32_t Wait(uint32_t timeout) override
    {
        return WaitWithTimeout([this] { return TryAcquire(); }, generation, timeout);
    }

    bool Set()
    {
        bool previous = signaled.exchange(true);
        NotifySignal(generation);
        return previous;
    }

    bool Reset()
    {
        return signaled.exchange(false);
    }
};

struct Semaphore final : KernelObject, HostObject<XKSEMAPHORE>
{
    std::atomic<uint32_t> count;
    uint32_t maximumCount;

    Semaphore(XKSEMAPHORE* semaphore)
        : count(semaphore->Header.SignalState), maximumCount(semaphore->Limit)
    {
    }

    Semaphore(uint32_t count, uint32_t maximumCount)
        : count(count), maximumCount(maximumCount)
    {
    }

    bool TryAcquire()
    {
        uint32_t current = count.load();
        while (current != 0)
        {
            if (count.compare_exchange_weak(current, current - 1))
                return true;
        }
        return false;
    }

    std::atomic<uint32_t> generation{0};

    uint32_t Wait(uint32_t timeout) override
    {
        return WaitWithTimeout([this] { return TryAcquire(); }, generation, timeout);
    }

    uint32_t Release(uint32_t releaseCount, uint32_t* previousCount)
    {
        uint32_t previous = count.load();
        if (previousCount != nullptr)
            *previousCount = previous;

        if (previous + releaseCount > maximumCount)
            return X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;

        count += releaseCount;
        NotifySignal(generation);
        return X_STATUS_SUCCESS;
    }
};

struct Mutant final : KernelObject, HostObject<XDISPATCHER_HEADER>
{
    std::mutex lock;
    std::thread::id owner{};
    uint32_t recursion = 0;

    Mutant(XDISPATCHER_HEADER* header)
    {
        // SignalState 1 = free, <= 0 = owned
    }

    Mutant(bool initialOwner)
    {
        if (initialOwner)
        {
            owner = std::this_thread::get_id();
            recursion = 1;
        }
    }

    bool TryAcquire()
    {
        std::lock_guard guard(lock);
        if (recursion == 0 || owner == std::this_thread::get_id())
        {
            owner = std::this_thread::get_id();
            recursion++;
            return true;
        }
        return false;
    }

    std::atomic<uint32_t> generation{0};

    uint32_t Wait(uint32_t timeout) override
    {
        return WaitWithTimeout([this] { return TryAcquire(); }, generation, timeout);
    }

    uint32_t Release()
    {
        std::lock_guard guard(lock);
        if (recursion == 0 || owner != std::this_thread::get_id())
            return X_STATUS_MUTANT_NOT_OWNED;

        if (--recursion == 0)
            owner = {};

        NotifySignal(generation);
        return X_STATUS_SUCCESS;
    }
};

// Returns the host object for a handle (NtCreate*/ExCreateThread) or for a guest
// dispatcher header (KEVENT/KSEMAPHORE/KMUTANT embedded in guest memory). nullptr if unknown.
KernelObject* ResolveWaitable(uint32_t handleOrAddress);

// Milliseconds from an NT timeout (negative = relative 100ns units, positive = absolute
// system time, nullptr = infinite).
uint32_t GuestTimeoutToMilliseconds(be<int64_t>* timeout);
