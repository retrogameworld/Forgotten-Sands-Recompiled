// Step 35: XAudio2 lock-order deadlock (rare freeze when a level starts).
//
// The game's statically linked XAudio2 GetVoiceState (sub_82D05EC8) takes the engine API lock and then the
// voice lock (sub_82D058B8). The audio processing thread (entry sub_82D29FD8) holds the voice lock while it
// runs voice callbacks, and the game's callback (sub_829D2388) calls GetVoiceState - so engine lock <- voice
// lock on one thread and voice lock <- engine lock on the other: a deadlock seen in 3 of ~80 level starts
// (dumps: game worker in sub_82D058B8 waiting for the voice lock, audio thread in sub_82D05EC8 waiting for the
// engine lock). On the audio thread GetVoiceState now runs without the engine lock (the voice lock still
// guards the queue state it reads). FS_NO_XAUDIO_LOCK_FIX=1: off.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>

static thread_local bool t_xaudioThread = false;

PPC_FUNC_IMPL(__imp__sub_82D29FD8);
PPC_FUNC(sub_82D29FD8)
{
    t_xaudioThread = true;
    __imp__sub_82D29FD8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82D058B8);
PPC_FUNC_IMPL(__imp__sub_82D05EC8);
// diag FS_LOG_VOICE_STATE=1: per voice, once a second: queued buffers and samples played (how far ahead the
// game's mixer has queued audio).
static void LogVoiceState(uint8_t* base, uint32_t self, uint32_t out)
{
    static const bool s_on = getenv("FS_LOG_VOICE_STATE") != nullptr;
    if (!s_on) return;
    struct Seen { std::chrono::steady_clock::time_point at{}; uint64_t samples = 0; uint32_t calls = 0; };
    static std::mutex s_m; static std::unordered_map<uint32_t, Seen> s_seen;
    const uint32_t queued = *reinterpret_cast<be<uint32_t>*>(base + out);
    const uint64_t played = *reinterpret_cast<be<uint64_t>*>(base + out + 8);
    std::lock_guard l(s_m);
    auto& v = s_seen[self];
    v.calls++;
    const auto now = std::chrono::steady_clock::now();
    if (now - v.at > std::chrono::seconds(1))
    {
        const double dt = std::chrono::duration<double>(now - v.at).count();
        if (false) LOGFN("[voice] 0x{:08X}: buffers queued {} samples played {} (+{:.0f}/s) rate field {} calls {}", self, queued, played,
            v.at.time_since_epoch().count() ? double(played - v.samples) / dt : 0.0, uint32_t(*reinterpret_cast<be<uint32_t>*>(base + self + 64)), v.calls);
        v.at = now; v.samples = played; v.calls = 0;
    }
}

PPC_FUNC(sub_82D05EC8)
{
    static const bool s_off = getenv("FS_NO_XAUDIO_LOCK_FIX") != nullptr;
    if (s_off || !t_xaudioThread)
    {
        const uint32_t self0 = ctx.r3.u32, out0 = ctx.r4.u32;
        __imp__sub_82D05EC8(ctx, base);
        LogVoiceState(base, self0, out0);
        return;
    }
    static std::atomic<int> s_logged{0};
    if (s_logged++ == 0)
        LOGFN("[audio] GetVoiceState from the audio thread: engine lock skipped (deadlock fix)");

    // Same as the original (sub_82D05EC8) minus the lock/unlock virtual calls on (voice+52)+44.
    auto load32 = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
    auto store32 = [&](uint32_t a, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(base + a) = v; };
    const uint32_t self = ctx.r3.u32, out = ctx.r4.u32;
    const PPCRegister savedR1 = ctx.r1;
    ctx.r1.u64 = ctx.r1.u32 - 128; // our frame (the original's size), the position query writes at r1+80

    ctx.r3.u64 = self;
    __imp__sub_82D058B8(ctx, base); // queued buffer count (takes the voice lock itself)
    store32(out + 0, ctx.r3.u32);
    store32(out + 4, load32(self + 148));

    const uint32_t positionObject = load32(self + 144);
    ctx.r3.u64 = positionObject;
    ctx.r4.u64 = ctx.r1.u32 + 80;
    const uint32_t target = load32(load32(positionObject) + 52);
    PPC_CALL_INDIRECT_FUNC(target);

    const uint64_t samples = *reinterpret_cast<be<uint64_t>*>(base + ctx.r1.u32 + 88);
    const uint64_t divisor = uint64_t(load32(self + 64)) << 1 & 0xFFFFFFFE;
    const uint64_t played = divisor ? uint32_t(samples / divisor) : 0;
    *reinterpret_cast<be<uint64_t>*>(base + out + 8) = played;
    ctx.r1 = savedR1;
    LogVoiceState(base, self, out);
}

// diag FS_LOG_VOICE_STATE=1: source voice Start / Stop / first SubmitSourceBuffer (vtable around GetState:
// Start 0x82D02928, Stop 0x82D02A90, SubmitSourceBuffer 0x82D064F8, FlushSourceBuffers 0x82D05D88).
static bool VoiceLogOn() { static const bool s = getenv("FS_LOG_VOICE_STATE") != nullptr; return s; }
static std::mutex g_voiceLogMutex;
static std::unordered_map<uint32_t, int> g_voiceSubmits;
PPC_FUNC_IMPL(__imp__sub_82D02928);
PPC_FUNC(sub_82D02928)
{
    if (VoiceLogOn()) { std::lock_guard l(g_voiceLogMutex); LOGFN("[voice] Start 0x{:08X} (submits so far {}) r13 0x{:08X}", ctx.r3.u32, g_voiceSubmits[ctx.r3.u32], ctx.r13.u32); }
    __imp__sub_82D02928(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82D02A90);
PPC_FUNC(sub_82D02A90)
{
    if (VoiceLogOn()) { std::lock_guard l(g_voiceLogMutex); LOGFN("[voice] Stop 0x{:08X}", ctx.r3.u32); g_voiceSubmits[ctx.r3.u32] = 0; }
    __imp__sub_82D02A90(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82D064F8);
PPC_FUNC(sub_82D064F8)
{
    if (VoiceLogOn())
    {
        std::lock_guard l(g_voiceLogMutex);
        auto b = reinterpret_cast<be<uint32_t>*>(base + ctx.r4.u32); // XAUDIO2_BUFFER: Flags, AudioBytes, pAudioData, ...
        if (g_voiceSubmits[ctx.r3.u32]++ < 3)
            LOGFN("[voice] Submit #{} 0x{:08X}: {} bytes, flags {} r13 0x{:08X}", g_voiceSubmits[ctx.r3.u32], ctx.r3.u32, uint32_t(b[1]), uint32_t(b[0]), ctx.r13.u32);
        // leading silence of this voice's sound: 16-bit samples before the first one louder than ~-50 dB
        static std::unordered_map<uint32_t, int64_t> s_lead; // voice -> samples so far while still silent (-1: done)
        if (g_voiceSubmits[ctx.r3.u32] == 1) s_lead[ctx.r3.u32] = 0;
        auto it = s_lead.find(ctx.r3.u32);
        if (it != s_lead.end() && it->second >= 0)
        {
            const uint32_t bytes = b[1], data = b[2];
            auto pcm = reinterpret_cast<be<int16_t>*>(base + data);
            const uint32_t count = bytes / 2;
            uint32_t k = 0;
            while (k < count && std::abs(int(int16_t(pcm[k]))) < 100) k++;
            if (k < count)
            {
                LOGFN("[voice] 0x{:08X}: first audible sample after {} silent samples (submit #{}, peak there {})", ctx.r3.u32, it->second + k, g_voiceSubmits[ctx.r3.u32], int(int16_t(pcm[k])));
                it->second = -1;
            }
            else
                it->second += count;
        }
    }
    __imp__sub_82D064F8(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_82D05D88);
PPC_FUNC(sub_82D05D88)
{
    if (VoiceLogOn()) { std::lock_guard l(g_voiceLogMutex); LOGFN("[voice] Flush 0x{:08X}", ctx.r3.u32); g_voiceSubmits[ctx.r3.u32] = 0; }
    __imp__sub_82D05D88(ctx, base);
}

// diag FS_LOG_VOICE_STATE=1: the sound engine's voice thread (sub_82A10030: wait for the event at obj+272, then
// process): how often it is woken.
PPC_FUNC_IMPL(__imp__sub_82A10030);
PPC_FUNC(sub_82A10030)
{
    if (!VoiceLogOn()) { __imp__sub_82A10030(ctx, base); return; }
    static thread_local auto t_last = std::chrono::steady_clock::now();
    static thread_local int t_n = 0; static thread_local double t_max = 0, t_sum = 0; static thread_local auto t_logAt = std::chrono::steady_clock::now();
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_82A10030(ctx, base);
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - t_last).count();
    t_last = now; t_n++; t_sum += ms; t_max = std::max(t_max, ms);
    if (now - t_logAt > std::chrono::seconds(1))
    {
        LOGFN("[voice] thread obj 0x{:08X} event 0x{:08X}: {} passes in the last second, mean {:.1f} ms, longest gap {:.1f} ms", obj, uint32_t(*reinterpret_cast<be<uint32_t>*>(base + obj + 272)), t_n, t_sum / t_n, t_max);
        t_n = 0; t_sum = 0; t_max = 0; t_logAt = now;
    }
}

// Step 37 (the tester's Odin crash, and mine: the game's audio processing thread read a null XMA context in
// sub_82D01748, "output samples ready in this voice's XMA ring"): the voice's entry no longer had a context (the
// voice was being torn down by another thread while this pass still had it in its list). On the console that
// window practically never opens; on the phone it did. With no context the ring is simply reported empty
// (0 samples ready) - the voice gets no more data and goes on to its end. FS_NO_XMA_NULL_GUARD=1: off.
PPC_FUNC_IMPL(__imp__sub_82D01748);
PPC_FUNC(sub_82D01748)
{
    static const bool s_off = getenv("FS_NO_XMA_NULL_GUARD") != nullptr;
    if (!s_off)
    {
        auto load32 = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        const uint32_t obj = ctx.r3.u32;
        const bool localCopy = obj != 0 && (load32(obj + 4) & 0x20000) != 0; // rlwinm. r11,r11,0,14,14
        const uint32_t entries = obj ? load32(obj + 8) : 0;
        if (obj == 0 || entries == 0 || (!localCopy && load32(entries + ctx.r4.u32 * 96 + 64) == 0))
        {
            static std::atomic<int> s_n{0};
            if (s_n++ < 20)
                LOGFN("[audio] XMA voice 0x{:08X} entry {} has no context any more (being torn down): 0 samples ready", obj, ctx.r4.u32);
            ctx.r3.u64 = 0;
            return;
        }
    }
    __imp__sub_82D01748(ctx, base);
}

// Step 37 diag: who tears a voice's XMA contexts down (sub_82D01688: releases each entry's context and zeroes it),
// on which thread, and how soon after the voice was started. FS_LOG_XMA_TEARDOWN=1.
PPC_FUNC_IMPL(__imp__sub_82D01688);
PPC_FUNC(sub_82D01688)
{
    static const bool s_log = getenv("FS_LOG_XMA_TEARDOWN") != nullptr;
    if (s_log)
    {
        extern std::string CurrentGuestCallChain(int maxFrames);
        static std::atomic<int> s_n{0};
        if (s_n++ < 200)
            LOGFN("[audio] XMA teardown of 0x{:08X} on r13 0x{:08X} |{}", ctx.r3.u32, ctx.r13.u32, CurrentGuestCallChain(10));
    }
    __imp__sub_82D01688(ctx, base);
}

// Step 37: the same guard for sub_82D01700 ("is this entry's XMA context idle?", 1 = idle), which read the context
// of the same torn-down voice right after sub_82D01748 (Odin crash, run odin73_6). No context = idle.
PPC_FUNC_IMPL(__imp__sub_82D01700);
PPC_FUNC(sub_82D01700)
{
    static const bool s_off = getenv("FS_NO_XMA_NULL_GUARD") != nullptr;
    if (!s_off)
    {
        auto load32 = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        const uint32_t obj = ctx.r3.u32;
        const bool localCopy = obj != 0 && (load32(obj + 4) & 0x20000) != 0;
        const uint32_t entries = obj ? load32(obj + 8) : 0;
        if (obj == 0 || entries == 0 || (!localCopy && load32(entries + ctx.r4.u32 * 96 + 64) == 0))
        {
            static std::atomic<int> s_n{0};
            if (s_n++ < 20)
                LOGFN("[audio] XMA voice 0x{:08X} entry {} has no context any more (being torn down): reported idle", obj, ctx.r4.u32);
            ctx.r3.u64 = 1;
            return;
        }
    }
    __imp__sub_82D01700(ctx, base);
}

// Step 37 diag (PC + Odin crash in sub_8249F180, a write to guest 0x5 while putting a finished request back on its
// manager's free list - the list's links were broken): is a request released twice? The release sets the state
// word (+40) to 0; a request whose state is already 0 when it is released again is logged with the caller chain.
// FS_LOG_REQ_RELEASE=1.
PPC_FUNC_IMPL(__imp__sub_8249F180);
PPC_FUNC(sub_8249F180)
{
    static const bool s_log = getenv("FS_LOG_REQ_RELEASE") != nullptr;
    if (s_log)
    {
        auto load32 = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        const uint32_t item = ctx.r4.u32, mgr = ctx.r3.u32;
        static std::atomic<uint64_t> s_calls{0};
        static std::atomic<int> s_logged{0};
        s_calls++;
        const uint32_t state = item ? load32(item + 40) : 0xFFFFFFFF;
        if (item && state == 0 && s_logged++ < 40)
        {
            extern std::string CurrentGuestCallChain(int maxFrames);
            LOGFN("[req] request 0x{:08X} of 0x{:08X} released again (state 0, links {:08X} {:08X}, release #{}) r13 0x{:08X} |{}", item, mgr,
                load32(item), load32(item + 4), s_calls.load(), ctx.r13.u32, CurrentGuestCallChain(12));
        }
    }
    __imp__sub_8249F180(ctx, base);
}
