#include <stdafx.h>
#include "memory.h"

#ifdef _WIN32
// Forgotten Sands: on the console 0xA0000000, 0xC0000000 (and 0xE0000000) are views of the same physical
// memory, and the game's resource mover reads through the 0xC0000000 view what it wrote through 0xA0000000.
// Map one 512 MB section at both addresses so the two views alias. (0xE0000000 has a 4 KB physical offset
// that a 64 KB-granular view cannot express; it stays separate for now.)
static uint8_t* MapAliasedGuestMemory(uint8_t* want)
{
    constexpr uint64_t PHYS_SIZE = 0x20000000;
    void* probe = VirtualAlloc(want, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (probe != want)
    {
        if (probe) VirtualFree(probe, 0, MEM_RELEASE);
        return nullptr;
    }
    VirtualFree(probe, 0, MEM_RELEASE);

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, DWORD(PHYS_SIZE), nullptr);
    if (section == nullptr)
        return nullptr;
    void* low = VirtualAlloc(want, 0xA0000000ull, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* viewA = low == want ? MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, PHYS_SIZE, want + 0xA0000000ull) : nullptr;
    void* viewC = viewA ? MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, PHYS_SIZE, want + 0xC0000000ull) : nullptr;
    void* high = viewC ? VirtualAlloc(want + 0xE0000000ull, PPC_MEMORY_SIZE - 0xE0000000ull, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
    if (high != want + 0xE0000000ull)
    {
        if (high) VirtualFree(high, 0, MEM_RELEASE);
        if (viewC) UnmapViewOfFile(viewC);
        if (viewA) UnmapViewOfFile(viewA);
        if (low) VirtualFree(low, 0, MEM_RELEASE);
        CloseHandle(section);
        return nullptr;
    }
    return want; // the section handle stays open for the life of the process
}
#endif
Memory::Memory()
{
#ifdef _WIN32
    base = MapAliasedGuestMemory(reinterpret_cast<uint8_t*>(0x100000000ull));
    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == nullptr)
        return;

    mprotect(base, 4096, PROT_NONE);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
