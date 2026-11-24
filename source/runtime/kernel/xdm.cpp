#include <stdafx.h>
#include "xdm.h"
#include "freelist.h"

// From UnleashedRecomp; handle registry added (see xdm.h).

Mutex g_kernelLock;

static std::mutex g_handleMutex;
static std::unordered_set<uint32_t> g_handles;

void RegisterKernelObject(KernelObject* obj)
{
    std::lock_guard lock(g_handleMutex);
    g_handles.insert(g_memory.MapVirtual(obj));
}

void DestroyKernelObject(KernelObject* obj)
{
    {
        std::lock_guard lock(g_handleMutex);
        g_handles.erase(g_memory.MapVirtual(obj));
    }
    obj->~KernelObject();
    g_userHeap.Free(obj);
}

uint32_t GetKernelHandle(KernelObject* obj)
{
    assert(obj != GetInvalidKernelObject());
    return g_memory.MapVirtual(obj);
}

void DestroyKernelObject(uint32_t handle)
{
    DestroyKernelObject(GetKernelObject(handle));
}

bool IsKernelObject(uint32_t handle)
{
    std::lock_guard lock(g_handleMutex);
    return g_handles.contains(handle);
}

bool IsKernelObject(void* obj)
{
    return IsKernelObject(g_memory.MapVirtual(obj));
}

bool IsInvalidKernelObject(void* obj)
{
    return obj == GetInvalidKernelObject();
}
