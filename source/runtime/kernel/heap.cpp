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
void Heap::Init()
{
    heap = o1heapInit(g_memory.Translate(USER_HEAP_BEGIN), USER_HEAP_END - USER_HEAP_BEGIN);
    physicalHeap = o1heapInit(g_memory.Translate(PHYSICAL_BEGIN), PHYSICAL_END - PHYSICAL_BEGIN);
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

    std::lock_guard lock(physicalMutex);

    void* ptr = o1heapAllocate(physicalHeap, size + alignment);
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
    return ptr >= (const void*)physicalHeap;
}

void Heap::Free(void* ptr)
{
    if (ptr == nullptr)
        return;

    if (IsPhysical(ptr))
    {
        TrackHeapEvent("free", ptr, Size(ptr));
        std::lock_guard lock(physicalMutex);
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
    if (ptr)
        return *((size_t*)ptr - 2) - O1HEAP_ALIGNMENT; // relies on fragment header in o1heap.c

    return 0;
}
