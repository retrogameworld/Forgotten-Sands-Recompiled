// Step 35: frame-time fixes for frame rates above 60.
//
// sub_826D3D28 (the world / physics update: it steps the physics world, sub_82B92B50) clamps its time step to
// [1/60 s, 1/15 s] with constants at 0x820AEFC4 / 0x820AEFC8. Above 60 fps every frame still advanced the
// physics by 1/60 s: at 120 fps debris, ragdolls and cloth ran at double speed. While that function runs, its
// lower bound is 1/240 s (the constant is shared with three debug-text conversions in sub_823007xx, so it is
// only changed for the duration of the call). FS_NO_PHYSICS_DT_FIX=1: off.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>

PPC_FUNC_IMPL(__imp__sub_826D3D28);
PPC_FUNC(sub_826D3D28)
{
    static const bool s_off = getenv("FS_NO_PHYSICS_DT_FIX") != nullptr;
    if (getenv("FS_LOG_WORLD_DT")) // debug: calls per second and the time step it is given (f1)
    {
        static int s_calls = 0; static double s_sum = 0; static auto s_t = std::chrono::steady_clock::now();
        s_calls++; s_sum += ctx.f1.f64;
        if (std::chrono::steady_clock::now() - s_t > std::chrono::seconds(2))
        {
            LOGFN("[world] update {:.1f} calls/s, mean f1 {:.4f}", s_calls / 2.0, s_calls ? s_sum / s_calls : 0.0);
            s_calls = 0; s_sum = 0; s_t = std::chrono::steady_clock::now();
        }
    }
    auto& minStep = *reinterpret_cast<be<float>*>(base + 0x820AEFC4);
    if (s_off || float(minStep) != 1.0f / 60.0f)
    {
        __imp__sub_826D3D28(ctx, base);
        return;
    }
    minStep = 1.0f / 240.0f;
    __imp__sub_826D3D28(ctx, base);
    minStep = 1.0f / 60.0f;
}
