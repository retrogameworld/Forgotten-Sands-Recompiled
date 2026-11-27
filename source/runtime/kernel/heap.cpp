#include <stdafx.h>
#include "heap.h"
#include "memory.h"
#include "function.h"
#include <os/logger.h>

// From UnleashedRecomp. The Sonic-specific RtlAllocateHeap/XAllocMem hooks
// (sub_82BD7D30 etc.) were removed: Torchlight's CRT heap sits on top of
// NtAllocateVirtualMemory, which is implemented in nt_memory.cpp.

// Debug: FS_TRACK_ADDR=<hex guest address> logs every physical alloc/free whose block contains it.
static void TrackHeapEvent(const char* what, void* ptr, size_t size)
{
    static const uint32_t tracked = [] { const char* e = getenv("FS_TRACK_ADDR"); return e ? uint32_t(strtoul(e, nullptr, 16)) : 0u; }();
    if (tracked == 0 || ptr == nullptr)
        return;
    const uint32_t a = g_memory.MapVirtual(ptr);
    if (tracked >= a && tracked < a + size)
    {
        extern std::string CurrentGuestCallChain(int maxFrames);
        LOGFN("[track] {} 0x{:08X} size 0x{:X} contains 0x{:08X} |{}", what, a, size, tracked, CurrentGuestCallChain(8));
    }
}
// ---- PhysicalArena (Step 38) ----
static constexpr uint32_t PHYS_GRANULE = 16;
// Debug FS_PHYS_GUARD=1: 64 bytes after every block are filled with 0xA5 and checked on free and every 10 s - a block
// written past its requested size is logged (the old o1heap had slack after most blocks; this arena has none).
static uint32_t PhysGuard() { static const uint32_t s_g = [] { const char* e = getenv("FS_PHYS_GUARD"); return e ? std::max(64u, uint32_t(atoi(e)) & ~15u) : 0u; }(); return s_g; }
static void CheckGuard(uint32_t address, uint32_t requested, const char* when)
{
    const uint32_t g = PhysGuard();
    if (g == 0) return;
    const uint8_t* p = static_cast<const uint8_t*>(g_memory.Translate(address + requested));
    for (uint32_t i = 0; i < g; i++)
        if (p[i] != 0xA5)
        {
            static std::atomic<int> s_n{0};
            if (s_n++ < 60)
                LOGFN_WARNING("[mem] OVERRUN ({}): physical block 0x{:08X} size 0x{:X} written at +0x{:X} (byte 0x{:02X})", when, address, requested, requested + i, p[i]);
            return;
        }
}

void PhysicalArena::Init(uint32_t b, uint32_t e)
{
    begin = b; end = e;
    freeByAddr.clear(); freeBySize.clear(); live.clear();
    InsertFree(b, e - b);
}

void PhysicalArena::RemoveFree(std::map<uint32_t, uint32_t>::iterator it)
{
    freeBySize.erase({ it->second, it->first });
    freeByAddr.erase(it);
}

void PhysicalArena::InsertFree(uint32_t start, uint32_t size)
{
    if (size == 0)
        return;
    // merge with the free neighbours
    auto next = freeByAddr.lower_bound(start);
    if (next != freeByAddr.end() && next->first == start + size)
    {
        size += next->second;
        auto n = next++;
        RemoveFree(n);
    }
    if (next != freeByAddr.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start)
        {
            start = prev->first;
            size += prev->second;
            RemoveFree(prev);
        }
    }
    freeByAddr.emplace(start, size);
    freeBySize.insert({ size, start });
}

uint32_t PhysicalArena::Allocate(uint32_t size, uint32_t alignment)
{
    const uint32_t need = (std::max<uint32_t>(size, 1) + PhysGuard() + PHYS_GRANULE - 1) & ~(PHYS_GRANULE - 1);
    alignment = std::max<uint32_t>(alignment, PHYS_GRANULE);
    // Best fit: the smallest free block that holds the request at the asked alignment (lowest address on ties).
    for (auto it = freeBySize.lower_bound({ need, 0 }); it != freeBySize.end(); ++it)
    {
        const uint32_t start = it->second, bsize = it->first;
        const uint64_t a = (uint64_t(start) + alignment - 1) & ~uint64_t(alignment - 1);
        if (a + need > uint64_t(start) + bsize)
            continue;
        RemoveFree(freeByAddr.find(start));
        InsertFree(start, uint32_t(a - start));                                    // the part before the alignment
        InsertFree(uint32_t(a + need), uint32_t(uint64_t(start) + bsize - (a + need))); // the rest
        live[uint32_t(a)] = { uint32_t(a), need, size };
        if (PhysGuard()) memset(g_memory.Translate(uint32_t(a) + size), 0xA5, PhysGuard());
        used += need;
        requested += size;
        peakUsed = std::max(peakUsed, used);
        allocs++;
        return uint32_t(a);
    }
    oomCount++;
    return 0;
}

// 0xC0000000 is a second view of the same memory (kernel/memory.cpp)
static uint32_t PhysCanon(uint32_t address) { return address >= 0xC0000000u && address < 0xE0000000u ? address - 0x20000000u : address; }

bool PhysicalArena::Free(uint32_t address)
{
    auto it = live.find(PhysCanon(address));
    if (it == live.end())
        return false;
    const Block b = it->second;
    CheckGuard(b.start, b.requested, "free");
    live.erase(it);
    used -= b.size;
    requested -= b.requested;
    InsertFree(b.start, b.size);
    return true;
}

uint32_t PhysicalArena::Size(uint32_t address) const
{
    auto it = live.find(PhysCanon(address));
    return it == live.end() ? 0 : it->second.requested;
}

static bool UseO1HeapPhysical()
{
    static const bool s_on = getenv("FS_O1HEAP_PHYSICAL") != nullptr;
    return s_on;
}

void Heap::Init()
{
    heap = o1heapInit(g_memory.Translate(USER_HEAP_BEGIN), USER_HEAP_END - USER_HEAP_BEGIN);
    if (UseO1HeapPhysical())
        physicalHeap = o1heapInit(g_memory.Translate(PHYSICAL_BEGIN), PHYSICAL_END - PHYSICAL_BEGIN);
    else
        physicalArena.Init(PHYSICAL_BEGIN + 0x10000, PHYSICAL_END); // the first 64 KB stay unused (physical address 0)
}

// Step 38: how full the 512 MB physical memory is (every 10 s with an allocation, and on a failed one).
void Heap::LogPhysicalStats(const char* why)
{
    std::lock_guard lock(physicalMutex);
    if (physicalHeap)
    {
        const O1HeapDiagnostics d = o1heapGetDiagnostics(physicalHeap);
        LOGFN("[mem] physical heap ({}, o1heap): in use {:.1f} MB incl. rounding, peak {:.1f} MB, of {:.1f} MB, failed {}",
            why, d.allocated / 1048576.0, d.peak_allocated / 1048576.0, d.capacity / 1048576.0, d.oom_count);
        return;
    }
    const auto& a = physicalArena;
    if (PhysGuard())
        for (const auto& [addr, blk] : a.live)
            CheckGuard(addr, blk.requested, why);
    LOGFN("[mem] physical heap ({}): in use {:.1f} MB ({} blocks), peak {:.1f} MB, of {:.1f} MB, largest free {:.1f} MB in {} free ranges, failed {}",
        why, a.used / 1048576.0, a.live.size(), a.peakUsed / 1048576.0, (a.end - a.begin) / 1048576.0, a.LargestFree() / 1048576.0, a.freeByAddr.size(), a.oomCount);
}

void* Heap::Alloc(size_t size)
{
    std::lock_guard lock(mutex);

    return o1heapAllocate(heap, std::max<size_t>(1, size));
}

void* Heap::AllocPhysical(size_t size, size_t alignment)
{
    size = std::max<size_t>(1, size);
    alignment = alignment == 0 ? 0x1000 : std::max<size_t>(16, alignment);

    if (physicalHeap == nullptr)
    {
        uint32_t address;
        {
            std::lock_guard lock(physicalMutex);
            address = size < 0x20000000 ? physicalArena.Allocate(uint32_t(size), uint32_t(alignment)) : 0;
        }
        static std::atomic<int64_t> s_lastLog{0};
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = s_lastLog.load();
        if (address == 0 || (now - last >= 10 && s_lastLog.compare_exchange_strong(last, now)))
            LogPhysicalStats(address == 0 ? "allocation failed" : "10 s");
        if (address == 0)
            return nullptr;
        void* ptr = g_memory.Translate(address);
        TrackHeapEvent("alloc", ptr, size);
        return ptr;
    }

    void* ptr;
    {
        std::lock_guard lock(physicalMutex);
        ptr = o1heapAllocate(physicalHeap, size + alignment);
    }
    {
        static std::atomic<int64_t> s_lastLog{0};
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = s_lastLog.load();
        if (ptr == nullptr || (now - last >= 10 && s_lastLog.compare_exchange_strong(last, now)))
            LogPhysicalStats(ptr == nullptr ? "allocation failed" : "10 s");
    }
    if (ptr == nullptr)
        return nullptr;

    size_t aligned = ((size_t)ptr + alignment) & ~(alignment - 1);

    *((void**)aligned - 1) = ptr;
    *((size_t*)aligned - 2) = size + O1HEAP_ALIGNMENT;

    TrackHeapEvent("alloc", (void*)aligned, size);
    return (void*)aligned;
}

bool Heap::IsPhysical(const void* ptr) const
{
    return ptr >= (const void*)g_memory.Translate(PHYSICAL_BEGIN);
}

void Heap::Free(void* ptr)
{
    if (ptr == nullptr)
        return;

    if (IsPhysical(ptr))
    {
        TrackHeapEvent("free", ptr, Size(ptr));
        std::lock_guard lock(physicalMutex);
        if (physicalHeap == nullptr)
        {
            if (!physicalArena.Free(g_memory.MapVirtual(ptr)))
            {
                static std::atomic<int> s_bad{0};
                if (s_bad++ < 20)
                    LOGFN_WARNING("[mem] physical free of 0x{:08X}: not an allocated block (ignored)", g_memory.MapVirtual(ptr));
            }
            return;
        }
        o1heapFree(physicalHeap, *((void**)ptr - 1));
    }
    else
    {
        std::lock_guard lock(mutex);
        o1heapFree(heap, ptr);
    }
}

size_t Heap::Size(void* ptr)
{
    if (ptr && physicalHeap == nullptr && IsPhysical(ptr))
    {
        std::lock_guard lock(physicalMutex);
        return physicalArena.Size(g_memory.MapVirtual(ptr));
    }
    if (ptr)
        return *((size_t*)ptr - 2) - O1HEAP_ALIGNMENT; // relies on fragment header in o1heap.c

    return 0;
}
