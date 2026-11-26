#pragma once

#include "mutex.h"

// Guest memory map used by the runtime (from Torchlight) (guest addresses):
//   0x00000000-0x0001FFFF  unmapped (null page guard)
//   0x00020000-0x3FFFFFFF  user heap (o1heap): host-side allocations, guest stacks, pool, XamAlloc
//   0x40000000-0x7FE9FFFF  NtAllocateVirtualMemory region allocator (kernel/nt_memory.cpp)
//   0x7FEA0000-0x7FFFFFFF  reserved (XMA / GPU register area on the real console)
//   0x82000000-0x82F9FFFF  default.xex image (Forgotten Sands)
//   0x82FA0000-...         recompiled function lookup table (PPC_IMAGE_BASE+PPC_IMAGE_SIZE)
//   0xA0000000-0xBFFFFFFF  physical heap (MmAllocatePhysicalMemoryEx, kernel objects), 512 MB
//                          like the console, so (addr & 0x1FFFFFFF) is a unique physical address
//   0xC0000000-0xFFFFFFFF  unused for now (physical mirrors on the console; graphics step)
constexpr uint32_t USER_HEAP_BEGIN = 0x00020000;
constexpr uint32_t USER_HEAP_END = 0x40000000;
constexpr uint32_t VIRTUAL_BEGIN = 0x40000000;
constexpr uint32_t VIRTUAL_END = 0x7FEA0000;
constexpr uint32_t PHYSICAL_BEGIN = 0xA0000000;
constexpr uint32_t PHYSICAL_END = 0xC0000000;

// Step 38 (Android 1.1, GitHub issue "invisible textures"): the physical heap was an o1heap, which rounds every
// block up to a power of two after adding the alignment - the game's 8 MB requests (4 KB aligned) took 16 MB each,
// so the 512 MB physical memory ran out as the game streamed more ("MmAllocatePhysicalMemoryEx: out of physical
// memory", then missing level parts / a crash). PhysicalArena = best fit over exact sizes (16-byte granules).
// FS_O1HEAP_PHYSICAL=1: the old o1heap.
struct PhysicalArena
{
    struct Block { uint32_t start, size, requested; };
    uint32_t begin = 0, end = 0;
    std::map<uint32_t, uint32_t> freeByAddr;            // start -> size
    std::set<std::pair<uint32_t, uint32_t>> freeBySize; // (size, start)
    ankerl::unordered_dense::map<uint32_t, Block> live; // user address -> block
    uint64_t used = 0, requested = 0, peakUsed = 0, oomCount = 0, allocs = 0;

    void Init(uint32_t b, uint32_t e);
    uint32_t Allocate(uint32_t size, uint32_t alignment); // 0 = no room
    bool Free(uint32_t address);
    uint32_t Size(uint32_t address) const;
    uint32_t LargestFree() const { return freeBySize.empty() ? 0 : freeBySize.rbegin()->first; }
private:
    void InsertFree(uint32_t start, uint32_t size);
    void RemoveFree(std::map<uint32_t, uint32_t>::iterator it);
};

struct Heap
{
    Mutex mutex;
    O1HeapInstance* heap;

    Mutex physicalMutex;
    O1HeapInstance* physicalHeap = nullptr; // only with FS_O1HEAP_PHYSICAL=1
    PhysicalArena physicalArena;

    void Init();
    void LogPhysicalStats(const char* why);

    void* Alloc(size_t size);
    void* AllocPhysical(size_t size, size_t alignment);
    void Free(void* ptr);

    size_t Size(void* ptr);
    bool IsPhysical(const void* ptr) const;

    template<typename T, typename... Args>
    T* Alloc(Args&&... args)
    {
        T* obj = (T*)Alloc(sizeof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }

    template<typename T, typename... Args>
    T* AllocPhysical(Args&&... args)
    {
        T* obj = (T*)AllocPhysical(sizeof(T), alignof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }
};

extern Heap g_userHeap;
