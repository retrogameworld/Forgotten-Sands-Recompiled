#include <deque>
#include <stdafx.h>
#include "function.h"
#include "import_log.h"
#include "heap.h"
#include "memory.h"
#include "xstatus.h"
#include <os/logger.h>
#include <user/config.h>

// Guest virtual memory (NtAllocateVirtualMemory & co), physical memory (Mm*) and pool.
// New for Torchlight: UnleashedRecomp stubbed these because it hooked Sonic's own heap.
// The whole 4 GB guest space is committed on the host up front (kernel/memory.cpp), so this
// file only does address-space bookkeeping and zeroing; no host pages are mapped here.

namespace
{
    struct Region
    {
        uint32_t size;
        uint32_t allocProtect;
        uint32_t protect;
        std::vector<bool> committed; // per 4 KB page
    };

    std::mutex g_vmMutex;
    std::map<uint32_t, Region> g_regions; // key = base address
    constexpr uint32_t PAGE_4K = 0x1000;
    constexpr uint32_t ALLOC_GRANULARITY = 0x10000;

    std::map<uint32_t, Region>::iterator FindRegion(uint32_t address)
    {
        auto it = g_regions.upper_bound(address);
        if (it == g_regions.begin())
            return g_regions.end();
        --it;
        if (address >= it->first && address < it->first + it->second.size)
            return it;
        return g_regions.end();
    }

    uint32_t FindFree(uint32_t size, bool topDown)
    {
        if (topDown)
        {
            uint32_t end = VIRTUAL_END;
            for (auto it = g_regions.rbegin(); ; ++it)
            {
                uint32_t gapStart = (it == g_regions.rend()) ? VIRTUAL_BEGIN : it->first + it->second.size;
                gapStart = RoundUp(gapStart, ALLOC_GRANULARITY);
                if (end > gapStart && end - gapStart >= size)
                    return RoundDown(end - size, ALLOC_GRANULARITY);
                if (it == g_regions.rend())
                    break;
                end = it->first;
            }
            return 0;
        }

        uint32_t cursor = VIRTUAL_BEGIN;
        for (auto& [base, region] : g_regions)
        {
            if (base >= cursor && base - cursor >= size)
                return cursor;
            cursor = std::max(cursor, RoundUp(base + region.size, ALLOC_GRANULARITY));
        }
        if (VIRTUAL_END > cursor && VIRTUAL_END - cursor >= size)
            return cursor;
        return 0;
    }

    bool RangeFree(uint32_t base, uint32_t size)
    {
        if (base < VIRTUAL_BEGIN || uint64_t(base) + size > VIRTUAL_END)
            return false;
        auto it = g_regions.lower_bound(base);
        if (it != g_regions.end() && it->first < base + size)
            return false;
        if (it != g_regions.begin())
        {
            --it;
            if (it->first + it->second.size > base)
                return false;
        }
        return true;
    }
}

uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t allocationType, uint32_t protect, uint32_t debugMemory)
{
    if (!baseAddress || !regionSize || *regionSize == 0)
        return X_STATUS_INVALID_PARAMETER;

    const uint32_t pageSize = (allocationType & X_MEM_LARGE_PAGES) ? 0x10000 : PAGE_4K;
    uint32_t requested = *baseAddress;
    uint32_t base = RoundDown(requested, pageSize);
    uint32_t size = RoundUp<uint64_t>(uint64_t(requested - base) + *regionSize, pageSize);

    std::lock_guard lock(g_vmMutex);

    // MEM_COMMIT on an existing reservation
    if (base != 0 && (allocationType & X_MEM_RESERVE) == 0)
    {
        auto it = FindRegion(base);
        if (it == g_regions.end() || base + size > it->first + it->second.size)
        {
            LOGFN_WARNING("NtAllocateVirtualMemory: commit outside a reservation at 0x{:08X} size 0x{:X}", base, size);
            return X_STATUS_MEMORY_NOT_ALLOCATED;
        }

        auto& region = it->second;
        for (uint32_t a = base; a < base + size; a += PAGE_4K)
            region.committed[(a - it->first) / PAGE_4K] = true;
        region.protect = protect;

        *baseAddress = base;
        *regionSize = size;
        return X_STATUS_SUCCESS;
    }

    // MEM_RESERVE (optionally with MEM_COMMIT)
    if (base != 0)
    {
        base = RoundDown(base, ALLOC_GRANULARITY);
        size = RoundUp<uint64_t>(uint64_t(requested - base) + *regionSize, pageSize);
        if (!RangeFree(base, size))
        {
            LOGFN_WARNING("NtAllocateVirtualMemory: requested range 0x{:08X} size 0x{:X} is not free", base, size);
            return X_STATUS_NO_MEMORY;
        }
    }
    else
    {
        base = FindFree(RoundUp(size, ALLOC_GRANULARITY), (allocationType & X_MEM_TOP_DOWN) != 0);
        if (base == 0)
        {
            LOGFN_ERROR("NtAllocateVirtualMemory: out of guest address space (size 0x{:X})", size);
            return X_STATUS_NO_MEMORY;
        }
    }

    Region region;
    region.size = size;
    region.allocProtect = protect;
    region.protect = protect;
    region.committed.assign(size / PAGE_4K, (allocationType & X_MEM_COMMIT) != 0);
    g_regions.emplace(base, std::move(region));

    if (Config::Verbose)
        LOGFN("NtAllocateVirtualMemory: 0x{:08X} size 0x{:X} type 0x{:X}", base, size, allocationType);

    *baseAddress = base;
    *regionSize = size;
    return X_STATUS_SUCCESS;
}

uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t debugMemory)
{
    if (!baseAddress)
        return X_STATUS_INVALID_PARAMETER;

    uint32_t base = RoundDown<uint32_t>(*baseAddress, PAGE_4K);

    std::lock_guard lock(g_vmMutex);
    auto it = FindRegion(base);
    if (it == g_regions.end())
        return X_STATUS_MEMORY_NOT_ALLOCATED;

    auto& region = it->second;

    if (freeType & X_MEM_RELEASE)
    {
        uint32_t regionBase = it->first;
        uint32_t size = region.size;
        memset(g_memory.Translate(regionBase), 0, size);
        g_regions.erase(it);

        *baseAddress = regionBase;
        if (regionSize)
            *regionSize = size;
        return X_STATUS_SUCCESS;
    }

    if (freeType & X_MEM_DECOMMIT)
    {
        uint32_t size = (regionSize && *regionSize) ? RoundUp<uint32_t>(*regionSize, PAGE_4K) : region.size - (base - it->first);
        size = std::min(size, region.size - (base - it->first));
        memset(g_memory.Translate(base), 0, size);
        for (uint32_t a = base; a < base + size; a += PAGE_4K)
            region.committed[(a - it->first) / PAGE_4K] = false;

        *baseAddress = base;
        if (regionSize)
            *regionSize = size;
        return X_STATUS_SUCCESS;
    }

    return X_STATUS_INVALID_PARAMETER;
}

struct X_MEMORY_BASIC_INFORMATION
{
    be<uint32_t> BaseAddress;
    be<uint32_t> AllocationBase;
    be<uint32_t> AllocationProtect;
    be<uint32_t> RegionSize;
    be<uint32_t> State;
    be<uint32_t> Protect;
    be<uint32_t> Type;
};

uint32_t NtQueryVirtualMemory(uint32_t address, X_MEMORY_BASIC_INFORMATION* info, uint32_t debugMemory)
{
    if (!info)
        return X_STATUS_INVALID_PARAMETER;

    memset(info, 0, sizeof(*info));
    uint32_t page = RoundDown(address, PAGE_4K);

    std::lock_guard lock(g_vmMutex);
    auto it = FindRegion(page);
    if (it != g_regions.end())
    {
        auto& region = it->second;
        size_t index = (page - it->first) / PAGE_4K;
        bool state = region.committed[index];
        size_t end = index;
        while (end < region.committed.size() && region.committed[end] == state)
            end++;

        info->BaseAddress = page;
        info->AllocationBase = it->first;
        info->AllocationProtect = region.allocProtect;
        info->RegionSize = uint32_t((end - index) * PAGE_4K);
        info->State = state ? X_MEM_COMMIT : X_MEM_RESERVE;
        info->Protect = state ? region.protect : 0;
        info->Type = X_MEM_PRIVATE;
        return X_STATUS_SUCCESS;
    }

    // Image and physical heap: report committed read/write memory.
    if ((page >= PPC_IMAGE_BASE && page < PPC_IMAGE_BASE + PPC_IMAGE_SIZE) || (page >= PHYSICAL_BEGIN && page < PHYSICAL_END))
    {
        uint32_t end = page < PHYSICAL_BEGIN ? uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE) : PHYSICAL_END;
        info->BaseAddress = page;
        info->AllocationBase = page;
        info->AllocationProtect = X_PAGE_READWRITE;
        info->RegionSize = end - page;
        info->State = X_MEM_COMMIT;
        info->Protect = X_PAGE_READWRITE;
        info->Type = 0x01000000; // MEM_IMAGE
        return X_STATUS_SUCCESS;
    }

    // Free range: size up to the next region.
    uint32_t next = VIRTUAL_END;
    auto upper = g_regions.upper_bound(page);
    if (upper != g_regions.end())
        next = upper->first;
    info->BaseAddress = page;
    info->RegionSize = next > page ? next - page : PAGE_4K;
    info->State = X_MEM_FREE;
    info->Protect = X_PAGE_NOACCESS;
    return X_STATUS_SUCCESS;
}

uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect, uint32_t minAddress, uint32_t maxAddress, uint32_t alignment)
{
    // test FS_PHYS_PAGE=1: sizes and alignment in whole pages (4 KB, 64 KB with X_MEM_LARGE_PAGES), like the console
    if (getenv("FS_PHYS_PAGE"))
    {
        const uint32_t page = (protect & X_MEM_LARGE_PAGES) ? 0x10000u : 0x1000u;
        size = (size + page - 1) & ~(page - 1);
        alignment = std::max(alignment, page);
    }
    static const bool s_logAllocs = getenv("FS_LOG_PHYS_ALLOCS") != nullptr; // test
    if (s_logAllocs) LOGFN("[mem] MmAllocatePhysicalMemoryEx flags 0x{:X} size 0x{:X} protect 0x{:X} min 0x{:X} max 0x{:X} align 0x{:X}", flags, size, protect, minAddress, maxAddress, alignment);
    // Step 38e (GitHub #2 follow-up, 1.1: "the enemies do not attack, and the jars cannot be broken"; reproduced on the
    // PC in the first fight: the Prince took no damage): with the Step 38 allocator the game's blocks (8 MB / 2 MB / 64 KB
    // large-page blocks) came back directly one after another. With unused room after each block (the old o1heap left
    // plenty) the enemies attack again; no write past a block was ever seen (FS_PHYS_GUARD), so the game depends on its
    // blocks not being contiguous. 4 KB of room after every block of the game (about 0.3 MB in all).
    // FS_PHYS_GAP=<bytes> changes it (0 = none).
    static const uint32_t s_gap = [] { const char* e = getenv("FS_PHYS_GAP"); return e ? uint32_t(strtoul(e, nullptr, 0)) : 0x1000u; }();
    void* ptr = g_userHeap.AllocPhysical(size + s_gap, alignment);
    if (ptr == nullptr)
    {
        LOGFN_ERROR("MmAllocatePhysicalMemoryEx: out of physical memory (size 0x{:X})", size);
        return 0;
    }

    memset(ptr, 0, size);
    uint32_t address = g_memory.MapVirtual(ptr);
    static const bool ioTrace = getenv("FS_IO_TRACE") != nullptr;
    if (Config::Verbose || ioTrace)
        LOGFN("MmAllocatePhysicalMemoryEx(size 0x{:X}, protect 0x{:X}, align 0x{:X}) -> 0x{:08X}", size, protect, alignment, address);
    return address;
}

// Step 37 (Odin: a rare hang - a sound voice's lock was looked for in memory that by then held another object): physical
// memory the game frees is handed out again only 2 s later (at most 64 MB held back), like the console, where freed
// memory was not reused within a few milliseconds by the next allocation. FS_FREE_NOW=1: freed at once.
void MmFreePhysicalMemory(uint32_t type, uint32_t guestAddress)
{
    if (guestAddress == 0)
        return;
    static const bool s_now = getenv("FS_HOLD_FREES") == nullptr; // Step 37: OFF by default - measured: with it on, 5 of 8 PC runs crashed; off (as before), 0 of 20. Opt-in for tests.
    if (s_now)
    {
        g_userHeap.Free(g_memory.Translate(guestAddress));
        return;
    }
    struct Held { uint32_t address; size_t size; std::chrono::steady_clock::time_point at; };
    static std::mutex s_m;
    static std::deque<Held> s_held;
    static size_t s_heldBytes = 0;
    static uint64_t s_frees = 0;
    const size_t size = g_userHeap.Size(g_memory.Translate(guestAddress));
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> due;
    {
        std::lock_guard l(s_m);
        s_held.push_back({ guestAddress, size, now });
        s_heldBytes += size;
        s_frees++;
        while (!s_held.empty() && (now - s_held.front().at > std::chrono::seconds(2) || s_heldBytes > (64u << 20)))
        {
            due.push_back(s_held.front().address);
            s_heldBytes -= s_held.front().size;
            s_held.pop_front();
        }
        static auto s_log = now;
        if (now - s_log > std::chrono::seconds(10))
        {
            s_log = now;
            LOGFN("[mem] physical frees so far {}, held back now {} ({:.1f} MB)", s_frees, s_held.size(), s_heldBytes / 1048576.0);
        }
    }
    for (uint32_t a : due)
        g_userHeap.Free(g_memory.Translate(a));
}

uint32_t MmGetPhysicalAddress(uint32_t address)
{
    // The physical heap is 0xA0000000-0xBFFFFFFF, so this is a unique 512 MB physical address.
    return address & 0x1FFFFFFF;
}

uint32_t MmQueryAddressProtect(uint32_t guestAddress)
{
    return X_PAGE_READWRITE;
}

uint32_t MmQueryAllocationSize(uint32_t guestAddress)
{
    if (guestAddress >= PHYSICAL_BEGIN && guestAddress < PHYSICAL_END)
        return uint32_t(g_userHeap.Size(g_memory.Translate(guestAddress)));

    std::lock_guard lock(g_vmMutex);
    auto it = FindRegion(guestAddress);
    return it != g_regions.end() ? it->second.size : 0;
}

struct X_MM_QUERY_STATISTICS_SECTION
{
    be<uint32_t> AvailablePages;
    be<uint32_t> TotalVirtualMemoryBytes;
    be<uint32_t> ReservedVirtualMemoryBytes;
    be<uint32_t> PhysicalPages;
    be<uint32_t> PoolPages;
    be<uint32_t> StackPages;
    be<uint32_t> ImagePages;
    be<uint32_t> HeapPages;
    be<uint32_t> VirtualPages;
    be<uint32_t> PageTablePages;
    be<uint32_t> CachePages;
};

struct X_MM_QUERY_STATISTICS_RESULT
{
    be<uint32_t> Size;
    be<uint32_t> TotalPhysicalPages;
    be<uint32_t> KernelPages;
    X_MM_QUERY_STATISTICS_SECTION Title;
    X_MM_QUERY_STATISTICS_SECTION System;
    be<uint32_t> HighestPhysicalPage;
};
static_assert(sizeof(X_MM_QUERY_STATISTICS_RESULT) == 0x68);

uint32_t MmQueryStatistics(X_MM_QUERY_STATISTICS_RESULT* stats)
{
    if (!stats)
        return X_STATUS_INVALID_PARAMETER;

    uint32_t size = stats->Size;
    if (size != sizeof(X_MM_QUERY_STATISTICS_RESULT))
        return X_STATUS_INVALID_PARAMETER;

    memset(stats, 0, sizeof(*stats));
    stats->Size = size;
    stats->TotalPhysicalPages = 0x20000; // 512 MB in 4 KB pages
    stats->KernelPages = 0x100;
    stats->Title.AvailablePages = 0x10000;
    stats->Title.TotalVirtualMemoryBytes = 0x2FFE0000;
    stats->Title.ReservedVirtualMemoryBytes = 0x00160000;
    stats->Title.PhysicalPages = 0x1000;
    stats->Title.PoolPages = 0x10;
    stats->Title.StackPages = 0x100;
    stats->Title.ImagePages = PPC_IMAGE_SIZE / 0x1000;
    stats->Title.HeapPages = 0x100;
    stats->Title.VirtualPages = 0x100;
    stats->Title.PageTablePages = 0x10;
    stats->Title.CachePages = 0x10;
    stats->System.AvailablePages = 0;
    stats->System.TotalVirtualMemoryBytes = 0x2FFF0000;
    stats->System.PhysicalPages = 0x1000;
    stats->HighestPhysicalPage = 0x1FFFF;
    return X_STATUS_SUCCESS;
}

// Xbox signature: MmMapIoSpace(unknown (always 2), PhysicalAddress, Size, Protect) - used for XMA contexts.
// Physical addresses come from MmGetPhysicalAddress (address & 0x1FFFFFFF of the 0xA0000000 heap): map back.
uint32_t MmMapIoSpace(uint32_t unknown, uint32_t physicalAddress, uint32_t size, uint32_t protect)
{
    const uint32_t address = physicalAddress < 0x20000000 ? (physicalAddress | 0xA0000000) : physicalAddress;
    static int s_logged = 0;
    if (s_logged++ < 5)
        LOGFN("MmMapIoSpace({}, 0x{:08X}, 0x{:X}, 0x{:X}) -> 0x{:08X}", unknown, physicalAddress, size, protect, address);
    return address;
}

uint32_t ExAllocatePoolTypeWithTag(uint32_t size, uint32_t tag, uint32_t type)
{
    void* ptr = g_userHeap.Alloc(size);
    if (!ptr)
        return 0;
    memset(ptr, 0, size);
    return g_memory.MapVirtual(ptr);
}

uint32_t ExAllocatePoolWithTag(uint32_t size, uint32_t tag)
{
    return ExAllocatePoolTypeWithTag(size, tag, 0);
}

uint32_t ExAllocatePool(uint32_t size)
{
    return ExAllocatePoolTypeWithTag(size, 'enoN', 0);
}

void ExFreePool(uint32_t address)
{
    if (address)
        g_userHeap.Free(g_memory.Translate(address));
}

GUEST_IMPORT(NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_IMPORT(NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_IMPORT(NtQueryVirtualMemory, NtQueryVirtualMemory);
GUEST_IMPORT(MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx);
GUEST_IMPORT(MmFreePhysicalMemory, MmFreePhysicalMemory);
GUEST_IMPORT(MmGetPhysicalAddress, MmGetPhysicalAddress);
GUEST_IMPORT(MmQueryAddressProtect, MmQueryAddressProtect);
GUEST_IMPORT(MmQueryAllocationSize, MmQueryAllocationSize);
GUEST_IMPORT(MmQueryStatistics, MmQueryStatistics);
GUEST_IMPORT(MmMapIoSpace, MmMapIoSpace);
GUEST_IMPORT(ExAllocatePoolWithTag, ExAllocatePoolWithTag);
GUEST_IMPORT(ExAllocatePool, ExAllocatePool);
GUEST_IMPORT(ExFreePool, ExFreePool);
