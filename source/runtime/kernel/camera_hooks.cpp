// Step 35p: kill close-ups at frame rates above 30 (the tester: in a kill close-up - most clearly when an enemy is thrown
// over the balcony railing - the camera points at the floor or at bystanders at 60 / 120 fps). Found and fixed
// first in the PC version (E:\FSPCFix\src\camera.cpp); the game logic is the same here.
//
// In a finishing move the camera plays an imported camera animation (class ImportedAnimationModifier, play modes
// 5 / 6 = PM_SynchronizeWithActions / PM_SynchronizeWithCurrentAction) relative to a target tracker that holds
// the attacker's position and direction. When the move starts, the attacker is snapped into place next to his
// victim at a fixed speed. At 30 frames per second, the rate the game was made for, the snap fits inside one
// frame and the tracker is filled after it. The tracker is filled only once and then stays frozen for the
// close-up. At 60 fps and above the snap takes 2 or more frames; the tracker is frozen on the first of them,
// before the snap, and the whole camera animation plays from the old position and direction.
//
// Fix: in the first frames of such a camera animation the attacker's own position and rotation are followed
// until they stop changing (the snap is over, at most 0.12 s) and kept as the anchor the camera should have had.
// The game refills its tracker with the stale values on every frame, so the tracker is left alone and the result
// of the camera animation is moved instead, rigidly, from the stale anchor to the right one. When both anchors
// are equal (30 fps, or no snap needed) nothing changes.
//
// 360 layout (from the update function sub_827BBD28, vtable 0x82112928 slot 21):
//   modifier: +16 clip count in the top 14 bits, +20 play mode, +24 tracker, +37 "attached to the tracker" flag
//   tracker:  +32 position (float4), +48 rotation (quaternion x y z w), +148 object whose virtual function +76
//             returns the tracked entity; sub_827C0638(tracker, pos*, rot*) samples that entity the way the
//             tracker itself does
//   output:   +16 position, +32 rotation, +48 field of view
// FS_NO_CLOSEUP_FIX=1: off. FS_LOG_CLOSEUP=1: logs anchor, attacker and camera in the first frames of a close-up.
//
// What the 360 game does (logged at 120 fps): in the aerial vault kill and in the ground finisher after a kick the
// tracker is NOT frozen - it equals the attacker on every update of the whole close-up, and those close-ups are
// framed correctly. The correction therefore only applies while the tracker stays at the value it had on the
// close-up's first update; as soon as it moves, the close-up is left alone for good (the PC version adopted the
// same rule). FS_TEST_FREEZE_TRACKER=1 (tests): holds the tracker at its first-update value in every such
// close-up, which recreates the fault in a close-up that can be scripted.
//
// Found with that test: when the camera animation's clips are over, the update function no longer writes
// "tracker x animation pose" but hands back the CURRENT camera (sub_8279A508) for the rest of the close-up -
// which already contains the correction of the frame before. Correcting it again turned the camera by the same
// angle on every frame (a spinning camera for about 0.4 s). The correction is therefore only applied on updates
// whose output really is "tracker x the modifier's pose" (modifier +96 = the pose's position in the tracker's
// frame); on the other updates the output is left as it is (the held, already corrected camera).
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>

namespace
{
    struct Vec { float v[4]; };

    Vec ReadVec(uint8_t* base, uint32_t guest)
    {
        Vec r;
        auto p = reinterpret_cast<be<float>*>(base + guest);
        for (int i = 0; i < 4; i++) r.v[i] = p[i];
        return r;
    }
    void WriteVec3(uint8_t* base, uint32_t guest, const float* v, int n)
    {
        auto p = reinterpret_cast<be<float>*>(base + guest);
        for (int i = 0; i < n; i++) p[i] = v[i];
    }
    void QMul(const float* a, const float* b, float* o) // Hamilton product a * b, components x y z w
    {
        o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    }
    // Angle of the rotation in degrees (for the log; which axis is "up" does not matter for comparing).
    float AngleBetween(const float* a, const float* b)
    {
        const float dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return 2.0f * std::acos(std::min(1.0f, dot)) * 57.29578f;
    }

    // The tracked entity's position and rotation, sampled by the game's own function.
    bool SampleEntity(PPCContext& live, uint8_t* base, uint32_t tracker, Vec& pos, Vec& rot)
    {
        const uint32_t getter = *reinterpret_cast<be<uint32_t>*>(base + tracker + 148);
        if (getter == 0)
            return false;
        static uint32_t s_scratch = 0;
        if (s_scratch == 0)
            s_scratch = g_memory.MapVirtual(g_userHeap.AllocPhysical(64, 16));
        memset(base + s_scratch, 0, 64);
        PPCContext ctx;
        ctx.r1 = live.r1;
        ctx.r13 = live.r13;
        ctx.fpscr = live.fpscr;
        ctx.r3.u64 = tracker;
        ctx.r4.u64 = s_scratch;
        ctx.r5.u64 = s_scratch + 32;
        // The sampler ends by passing the result through the tracker's filter object (+152, sub_827D6FF0), which
        // can replace the entity's values by processed ones (possibly the very values that keep the tracker
        // frozen). The attacker himself is wanted here, so the filter is taken out for this one call.
        auto filterSlot = reinterpret_cast<be<uint32_t>*>(base + tracker + 152);
        const uint32_t filter = *filterSlot;
        *filterSlot = 0;
        SetPPCContext(ctx);
        g_memory.FindFunction(0x827C0638)(ctx, base);
        SetPPCContext(live);
        *filterSlot = filter;
        pos = ReadVec(base, s_scratch);
        rot = ReadVec(base, s_scratch + 32);
        const float n = std::sqrt(rot.v[0] * rot.v[0] + rot.v[1] * rot.v[1] + rot.v[2] * rot.v[2] + rot.v[3] * rot.v[3]);
        if (!(n > 0.9f && n < 1.1f))
            return false; // not a plain rotation: leave the camera alone
        for (float& x : rot.v) x /= n;
        return true;
    }

    struct Follow
    {
        uint32_t modifier = 0;
        std::chrono::steady_clock::time_point startedAt{}, lastUpdate{}, lastSample{};
        int samples = 0, updates = 0;
        bool done = false;
        Vec pos{}, rot{}, firstRot{};
        Vec firstTp{}, firstTq{};   // the tracker on the close-up's first update
        bool standDown = false;     // the tracker moved since: it follows the attacker itself, nothing to correct
    };
    Follow g_follow;

    // FS_TEST_FREEZE_TRACKER: the tracker held at these values (see the tracker update hook)
    uint32_t g_testFreezeTracker = 0;
    Vec g_testFreezePos{}, g_testFreezeRot{};
}

PPC_FUNC_IMPL(__imp__sub_827BBD28);
PPC_FUNC(sub_827BBD28)
{
    static const bool s_off = getenv("FS_NO_CLOSEUP_FIX") != nullptr;
    static const bool s_log = getenv("FS_LOG_CLOSEUP") != nullptr;
    const uint32_t self = ctx.r3.u32, out = ctx.r4.u32;
    __imp__sub_827BBD28(ctx, base);
    if (self == 0 || out == 0)
        return;
    const uint32_t clips = uint32_t(*reinterpret_cast<be<uint32_t>*>(base + self + 16)) >> 18;
    const int32_t mode = int32_t(uint32_t(*reinterpret_cast<be<uint32_t>*>(base + self + 20)));
    if (clips < 1 || (mode != 5 && mode != 6))
        return;
    const uint32_t tracker = *reinterpret_cast<be<uint32_t>*>(base + self + 24);
    const bool attached = base[self + 37] != 0;
    const auto now = std::chrono::steady_clock::now();
    Follow& f = g_follow;
    // a new close-up: another modifier, or none was updated for a while
    if (f.modifier != self || now - f.lastUpdate > std::chrono::milliseconds(120))
    {
        f = Follow{};
        f.modifier = self;
        f.startedAt = now;
        g_testFreezeTracker = 0;
    }
    f.lastUpdate = now;
    if (tracker == 0 || !attached)
        return;

    // Follow the attacker until the snap is over.
    if (!f.done && (f.samples == 0 || now - f.lastSample >= std::chrono::milliseconds(3)))
    {
        Vec pos, rot;
        if (!SampleEntity(ctx, base, tracker, pos, rot))
            f.done = true;
        else
        {
            if (f.samples == 0)
                f.firstRot = rot;
            else
            {
                const float dp = std::fabs(pos.v[0] - f.pos.v[0]) + std::fabs(pos.v[1] - f.pos.v[1]) + std::fabs(pos.v[2] - f.pos.v[2]);
                const float dot = std::fabs(rot.v[0] * f.rot.v[0] + rot.v[1] * f.rot.v[1] + rot.v[2] * f.rot.v[2] + rot.v[3] * f.rot.v[3]);
                if ((dp < 0.002f && dot > 0.999999f) || now - f.startedAt > std::chrono::milliseconds(120))
                {
                    f.done = true;
                    if (f.samples > 1)
                        LOGFN("[camera] close-up: the anchor followed the attacker's snap for {} updates (turned {:.0f} degrees)", f.samples - 1, AngleBetween(f.firstRot.v, f.rot.v));
                }
            }
            if (!f.done)
            {
                f.pos = pos; f.rot = rot;
                f.samples++;
                f.lastSample = now;
            }
        }
    }

    const Vec tp = ReadVec(base, tracker + 32), tq = ReadVec(base, tracker + 48);
    Vec op = ReadVec(base, out + 16), oq = ReadVec(base, out + 32);
    if (f.updates == 0)
    {
        f.firstTp = tp; f.firstTq = tq;
        static const bool s_testFreeze = getenv("FS_TEST_FREEZE_TRACKER") != nullptr;
        if (s_testFreeze) { g_testFreezePos = tp; g_testFreezeRot = tq; g_testFreezeTracker = tracker; }
    }
    else if (!f.standDown)
    {
        const float moved = std::fabs(tp.v[0] - f.firstTp.v[0]) + std::fabs(tp.v[1] - f.firstTp.v[1]) + std::fabs(tp.v[2] - f.firstTp.v[2]);
        const float dot = std::fabs(tq.v[0] * f.firstTq.v[0] + tq.v[1] * f.firstTq.v[1] + tq.v[2] * f.firstTq.v[2] + tq.v[3] * f.firstTq.v[3]);
        if (moved > 0.01f || dot < 0.99999f)
        {
            f.standDown = true;
            if (s_log) LOGFN("[camera] close-up update {}: the tracker moves with the attacker - this close-up is left alone", f.updates);
        }
    }
    // Is this output "tracker x animation pose" (a clip was played on this update)? Otherwise it is the current
    // camera handed back, which must not be corrected again.
    bool fresh;
    {
        const Vec c = ReadVec(base, self + 96);
        const float cv[4] = { c.v[0], c.v[1], c.v[2], 0.0f }, tinv[4] = { -tq.v[0], -tq.v[1], -tq.v[2], tq.v[3] };
        float a[4], b[4];
        QMul(tq.v, cv, a);
        QMul(a, tinv, b);
        const float miss = std::fabs(tp.v[0] + b[0] - op.v[0]) + std::fabs(tp.v[1] + b[1] - op.v[1]) + std::fabs(tp.v[2] + b[2] - op.v[2]);
        fresh = miss < 0.02f;
        if (s_log && f.updates < 40)
            LOGFN("[camera] close-up update {}: pose in the tracker's frame {:.2f} {:.2f} {:.2f}, output misses it by {:.3f} -> {}", f.updates, c.v[0], c.v[1], c.v[2], miss, fresh ? "animation" : "current camera handed back");
    }

    // Move the camera animation's result from the game's (stale) anchor to the followed one.
    bool correct = !s_off && f.samples >= 1 && !f.standDown && fresh;
    float newPos[3] = {}, q[4] = {};
    if (correct)
    {
        const float inv[4] = { -tq.v[0], -tq.v[1], -tq.v[2], tq.v[3] };
        float d[4];
        QMul(f.rot.v, inv, d);
        const float shift = std::fabs(f.pos.v[0] - tp.v[0]) + std::fabs(f.pos.v[1] - tp.v[1]) + std::fabs(f.pos.v[2] - tp.v[2]);
        // Anchors closer than 1 degree and 0.02 units count as the same. A live tracker that has not yet moved far
        // enough to be recognised as live (vault kill: it stands still for 0.3 s, then starts to turn slowly) is
        // within that distance of the followed anchor, so a live close-up is never touched at all.
        if (std::fabs(d[3]) > 0.99996f && shift < 0.02f)
            correct = false; // the same anchor
        else
        {
            const float v[4] = { op.v[0] - tp.v[0], op.v[1] - tp.v[1], op.v[2] - tp.v[2], 0.0f }, dinv[4] = { -d[0], -d[1], -d[2], d[3] };
            float t[4], r[4];
            QMul(d, v, t);
            QMul(t, dinv, r);
            newPos[0] = f.pos.v[0] + r[0]; newPos[1] = f.pos.v[1] + r[1]; newPos[2] = f.pos.v[2] + r[2];
            QMul(d, oq.v, q);
        }
    }

    if (s_log)
    {
        const int s_n = f.updates;
        if (s_n < 40 || s_n % 30 == 0)
        {
            // the attacker as he is NOW (the followed anchor stops when the snap is over)
            Vec lp{}, lr{};
            const bool live = SampleEntity(ctx, base, tracker, lp, lr);
            if (s_n == 0)
                LOGFN("[camera] close-up starts: modifier 0x{:08X} tracker 0x{:08X} vtable 0x{:08X} flags +188 {} +189 {} getter 0x{:08X} filter 0x{:08X}", self, tracker,
                    uint32_t(*reinterpret_cast<be<uint32_t>*>(base + tracker)), base[tracker + 188], base[tracker + 189],
                    uint32_t(*reinterpret_cast<be<uint32_t>*>(base + tracker + 148)), uint32_t(*reinterpret_cast<be<uint32_t>*>(base + tracker + 152)));
            LOGFN("[camera] close-up update {} mode {}: tracker {:.2f} {:.2f} {:.2f} rot {:.3f} {:.3f} {:.3f} {:.3f} | attacker now {:.2f} {:.2f} {:.2f} rot {:.3f} {:.3f} {:.3f} {:.3f} ({:.0f} deg from tracker) | followed anchor {:.2f} {:.2f} {:.2f} ({:.0f} deg from tracker, {} samples, done {}, tracker {}) | camera {:.2f} {:.2f} {:.2f} rot {:.3f} {:.3f} {:.3f} {:.3f}{}",
                s_n, mode, tp.v[0], tp.v[1], tp.v[2], tq.v[0], tq.v[1], tq.v[2], tq.v[3], lp.v[0], lp.v[1], lp.v[2], lr.v[0], lr.v[1], lr.v[2], lr.v[3], live ? AngleBetween(tq.v, lr.v) : -1.0f,
                f.pos.v[0], f.pos.v[1], f.pos.v[2], f.samples > 0 ? AngleBetween(tq.v, f.rot.v) : 0.0f, f.samples, f.done, f.standDown ? "live" : "frozen", op.v[0], op.v[1], op.v[2], oq.v[0], oq.v[1], oq.v[2], oq.v[3],
                correct ? fmt::format(" | corrected {:.2f} {:.2f} {:.2f} rot {:.3f} {:.3f} {:.3f} {:.3f}", newPos[0], newPos[1], newPos[2], q[0], q[1], q[2], q[3]) : std::string());
        }
    }
    f.updates++;
    if (!correct)
        return;
    WriteVec3(base, out + 16, newPos, 3);
    WriteVec3(base, out + 32, q, 4);
}

// The Prince's position as the gameplay camera's tracker has it (for the test scripts' "goto").
static std::mutex g_playerMutex;
static float g_playerPos[3];
static std::chrono::steady_clock::time_point g_playerAt{};
bool CameraPlayerPosition(float* xyz)
{
    std::lock_guard l(g_playerMutex);
    if (std::chrono::steady_clock::now() - g_playerAt > std::chrono::milliseconds(500))
        return false;
    xyz[0] = g_playerPos[0]; xyz[1] = g_playerPos[1]; xyz[2] = g_playerPos[2];
    return true;
}

// Test: FS_LOG_TRACKER=1: where the camera's target trackers are, once a second each (to navigate scripted runs).
PPC_FUNC_IMPL(__imp__sub_827C03F8);
PPC_FUNC(sub_827C03F8)
{
    const uint32_t tracker = ctx.r3.u32;
    __imp__sub_827C03F8(ctx, base);
    if (tracker == 0)
        return;
    if (tracker == g_testFreezeTracker) // test: the tracker stays where it was on the close-up's first update
    {
        if (std::chrono::steady_clock::now() - g_follow.lastUpdate > std::chrono::milliseconds(120))
            g_testFreezeTracker = 0; // the close-up is over
        else
        {
            WriteVec3(base, tracker + 32, g_testFreezePos.v, 4);
            WriteVec3(base, tracker + 48, g_testFreezeRot.v, 4);
        }
    }
    {
        // the Prince's own tracker stands upright (rotation about the vertical axis only)
        const Vec p = ReadVec(base, tracker + 32), q = ReadVec(base, tracker + 48);
        if (std::fabs(q.v[0]) < 0.01f && std::fabs(q.v[1]) < 0.01f)
        {
            std::lock_guard l(g_playerMutex);
            g_playerPos[0] = p.v[0]; g_playerPos[1] = p.v[1]; g_playerPos[2] = p.v[2];
            g_playerAt = std::chrono::steady_clock::now();
        }
    }
    static const bool s_log = getenv("FS_LOG_TRACKER") != nullptr;
    if (!s_log)
        return;
    static std::mutex s_m; static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_last;
    std::lock_guard l(s_m);
    const auto now = std::chrono::steady_clock::now();
    auto& t = s_last[tracker];
    if (now - t < std::chrono::milliseconds(500))
        return;
    t = now;
    const Vec p = ReadVec(base, tracker + 32), q = ReadVec(base, tracker + 48);
    LOGFN("[tracker] 0x{:08X} at {:.2f} {:.2f} {:.2f} rot {:.3f} {:.3f} {:.3f} {:.3f}", tracker, p.v[0], p.v[1], p.v[2], q.v[0], q.v[1], q.v[2], q.v[3]);
}
