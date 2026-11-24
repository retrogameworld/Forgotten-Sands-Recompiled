#include <stdafx.h>
#include "function.h"
#include "import_log.h"
#include "memory.h"
#include "xstatus.h"
#include <os/logger.h>

// Vd* (video driver) imports. Graphics come in step 6e (hooking Torchlight's D3D layer to the
// plume Vulkan backend, like UnleashedRecomp does for Sonic); until then these return what the
// console would and log their first calls with arguments so the D3D init sequence is visible.

static void LogVd(const char* name, const PPCContext& ctx, int maxCalls = 3)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, int> counts;
    std::lock_guard lock(mutex);
    int& n = counts[name];
    if (n++ < maxCalls)
        LOGFN("[Vd] {}(0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}){}", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            n == maxCalls ? " (further calls not logged)" : "");
}

#define VD_STUB(name, ret) \
    PPC_FUNC(__imp__##name) { LogVd(#name, ctx); ctx.r3.u64 = uint32_t(ret); }

void VdQueryVideoMode(XVIDEO_MODE* vm)
{
    memset(vm, 0, sizeof(XVIDEO_MODE));
    vm->DisplayWidth = 1280;
    vm->DisplayHeight = 720;
    vm->IsInterlaced = false;
    vm->IsWidescreen = true;
    vm->IsHighDefinition = true;
    vm->RefreshRate = 0x42700000; // 60.0f
    vm->VideoStandard = 1;
    vm->Unknown4A = 0x4A;
    vm->Unknown01 = 0x01;
}

uint32_t VdPersistDisplay(uint32_t a1, be<uint32_t>* a2)
{
    if (a2)
        *a2 = 0;
    return 0;
}

void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
{
    if (type) *type = 2;
    if (power) *power = 2.22222233f;
}

void VdGetCurrentDisplayInformation(uint8_t* info)
{
    memset(info, 0, 0x58);
    *(be<uint16_t>*)(info + 0x00) = 1280; // front buffer width
    *(be<uint16_t>*)(info + 0x02) = 720;  // front buffer height
    *(be<uint32_t>*)(info + 0x0C) = 1280; // display width
    *(be<uint32_t>*)(info + 0x10) = 720;  // display height
    *(be<uint32_t>*)(info + 0x40) = 60;   // refresh rate
    *(be<uint32_t>*)(info + 0x48) = 1280; // actual display width
}

void VdGetSystemCommandBuffer(be<uint32_t>* p0, be<uint32_t>* p1)
{
    if (p0) *p0 = 0xBEEF0000;
    if (p1) *p1 = 0xBEEF0001;
}

static uint32_t g_graphicsInterruptCallback, g_graphicsInterruptData;

void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t data)
{
    LOGFN("[Vd] VdSetGraphicsInterruptCallback(0x{:08X}, 0x{:08X}) - interrupts are not delivered yet", callback, data);
    g_graphicsInterruptCallback = callback;
    g_graphicsInterruptData = data;
}

PPC_FUNC(__imp__VdQueryVideoMode) { LogVd("VdQueryVideoMode", ctx, 1); HostToGuestFunction<VdQueryVideoMode>(ctx, base); }
PPC_FUNC(__imp__XGetVideoMode) { LogVd("XGetVideoMode", ctx, 1); HostToGuestFunction<VdQueryVideoMode>(ctx, base); }
PPC_FUNC(__imp__VdPersistDisplay) { LogVd("VdPersistDisplay", ctx); HostToGuestFunction<VdPersistDisplay>(ctx, base); }
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) { LogVd("VdGetCurrentDisplayGamma", ctx); HostToGuestFunction<VdGetCurrentDisplayGamma>(ctx, base); }
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) { LogVd("VdGetCurrentDisplayInformation", ctx); HostToGuestFunction<VdGetCurrentDisplayInformation>(ctx, base); }
PPC_FUNC(__imp__VdGetSystemCommandBuffer) { LogVd("VdGetSystemCommandBuffer", ctx); HostToGuestFunction<VdGetSystemCommandBuffer>(ctx, base); }
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) { HostToGuestFunction<VdSetGraphicsInterruptCallback>(ctx, base); }

VD_STUB(VdInitializeEngines, 1);
VD_STUB(VdShutdownEngines, 0);
VD_STUB(VdIsHSIOTrainingSucceeded, 1);
VD_STUB(VdRetrainEDRAM, 0);
VD_STUB(VdRetrainEDRAMWorker, 0);
VD_STUB(VdSetDisplayMode, 0);
VD_STUB(VdQueryVideoFlags, 7);
VD_STUB(VdCallGraphicsNotificationRoutines, 0);
VD_STUB(VdInitializeScalerCommandBuffer, 0);
VD_STUB(VdEnableRingBufferRPtrWriteBack, 0);
VD_STUB(VdInitializeRingBuffer, 0);
VD_STUB(VdSetSystemCommandBufferGpuIdentifierAddress, 0);
VD_STUB(VdEnableDisableClockGating, 0);
VD_STUB(VdSwap, 0);
