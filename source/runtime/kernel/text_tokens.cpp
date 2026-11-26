// In-game text tokens such as "{CtlRStick}" (button icons in tutorial / menu text).
// The game's text formatter (sub_8265D1B8) finds "{...}" and looks the name up in a token list with
// sub_8265D0C8(list, name, length). FS_TOKEN_TRACE=1 logs every lookup and, the first time a name is missing,
// the whole list, so we can see which names the game registered.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <user/config.h>
#include <set>
#include <mutex>

namespace
{
    std::string ReadGuestWide(uint32_t p, uint32_t maxLen)
    {
        std::string s;
        if (!p) return "<null>";
        for (uint32_t i = 0; i < maxLen; i++)
        {
            uint16_t c = *(be<uint16_t>*)g_memory.Translate(p + i * 2);
            if (!c) break;
            s.push_back(c < 128 ? char(c) : '?');
        }
        return s;
    }

    std::string EntryName(uint32_t entry)
    {
        if (!entry) return "<null entry>";
        uint32_t ptr = *(be<uint32_t>*)g_memory.Translate(entry);
        uint32_t flags = *(be<uint32_t>*)g_memory.Translate(entry + 4);
        if (!(flags & 2)) return fmt::format("<flags {:08X}>", flags);
        return ReadGuestWide(ptr, 128);
    }
}

PPC_FUNC_IMPL(__imp__sub_8265D0C8);
PPC_FUNC(sub_8265D0C8)
{
    static const bool s_on = getenv("FS_TOKEN_TRACE") != nullptr;
    if (!s_on) { __imp__sub_8265D0C8(ctx, base); return; }

    const uint32_t list = ctx.r3.u32, name = ctx.r4.u32, len = ctx.r5.u32;
    const std::string query = ReadGuestWide(name, len);
    __imp__sub_8265D0C8(ctx, base);
    const uint32_t found = ctx.r3.u32;

    static std::mutex s_m;
    static std::set<std::string> s_seen;
    static std::set<uint32_t> s_dumped;
    std::lock_guard lock(s_m);
    if (s_seen.insert(query + (found ? "+" : "-")).second)
        LOGFN("[token] list 0x{:08X} lookup '{}' -> {}", list, query, found ? fmt::format("0x{:08X}", found) : std::string("MISSING"));
    if (!found && s_dumped.insert(list).second)
    {
        uint32_t arr = *(be<uint32_t>*)g_memory.Translate(list);
        uint32_t count = (*(be<uint32_t>*)g_memory.Translate(list + 4) >> 18) & 0x3FFF;
        LOGFN("[token] list 0x{:08X} has {} names:", list, count);
        for (uint32_t i = 0; i < count && arr; i++)
        {
            uint32_t e = *(be<uint32_t>*)g_memory.Translate(arr + i * 4);
            LOGFN("[token]   {:3}: '{}' (entry 0x{:08X}, id {})", i, EntryName(e), e, e ? int32_t(*(be<uint32_t>*)g_memory.Translate(e + 16)) : 0);
        }
    }
}

// sub_8268C930 picks "the input device used last" (smallest vtable[40] value >= 0) and sets byte +152 of the
// text-token owner to 1 when that device is a pad (vtable[24] == 2 or 3). With 0 the game shows the mouse
// bindings for the right stick (PC code path), so "{CtlRStick}" has no icon. FS_TOKEN_TRACE logs the inputs.
PPC_FUNC_IMPL(__imp__sub_821C3F08);
PPC_FUNC_IMPL(__imp__sub_821C3F78);
PPC_FUNC_IMPL(__imp__sub_8268C930);
PPC_FUNC(sub_8268C930)
{
    static const bool s_on = getenv("FS_TOKEN_TRACE") != nullptr;
    if (s_on)
    {
        const uint32_t mgr = *(be<uint32_t>*)g_memory.Translate(0x82F66448);
        std::string line = fmt::format("mgr 0x{:08X}", mgr);
        if (mgr)
        {
            PPCContext c = ctx;
            auto call = [&](uint32_t fn, uint32_t r3, uint32_t r4) { c.r3.u64 = r3; c.r4.u64 = r4; g_memory.FindFunction(fn)(c, base); };
            call(0x821C3F08, mgr, 0);
            const uint32_t count = c.r3.u32 & 0xFFFF;
            line += fmt::format(" devices {} (drivers {})", count, uint32_t((*(be<uint16_t>*)g_memory.Translate(mgr + 196)).get()));
            for (uint32_t i = 0; i < count && i < 16; i++)
            {
                call(0x821C3F78, mgr, i);
                const uint32_t it = c.r3.u32, drv = it >> 16, local = it & 0xFFFF;
                const uint32_t dev = *(be<uint32_t>*)g_memory.Translate(mgr + (drv + 5) * 4);
                const uint32_t vt = *(be<uint32_t>*)g_memory.Translate(dev);
                call(*(be<uint32_t>*)g_memory.Translate(vt + 16), dev, local); const int32_t t16 = c.r3.s32;
                call(*(be<uint32_t>*)g_memory.Translate(vt + 40), dev, local); const double f1 = c.f1.f64;
                call(*(be<uint32_t>*)g_memory.Translate(vt + 24), dev, local); const int32_t t24 = c.r3.s32;
                line += fmt::format(" | #{} drv {} local {} dev 0x{:08X} vt 0x{:08X} state {} last {:.3f} kind {}", i, drv, local, dev, vt, t16, f1, t24);
            }
        }
        const uint8_t before = *(uint8_t*)g_memory.Translate(ctx.r3.u32 + 152);
        const uint32_t owner = ctx.r3.u32;
        __imp__sub_8268C930(ctx, base);
        const uint8_t after = *(uint8_t*)g_memory.Translate(owner + 152);
        static std::string s_last;
        line += fmt::format(" => pad flag {} -> {}", before, after);
        // Log when anything but the timer values changes, and every 10 s.
        std::string key;
        for (size_t i = 0; i < line.size(); i++) { if (line.compare(i, 5, "last ") == 0) { i += 5; while (i < line.size() && line[i] != ' ') i++; } if (i < line.size()) key.push_back(line[i]); }
        static auto s_t = std::chrono::steady_clock::now() - std::chrono::seconds(20);
        const auto now = std::chrono::steady_clock::now();
        if (key != s_last || before != after || now - s_t > std::chrono::seconds(10)) { LOGFN("[padflag] {}", line); s_last = key; s_t = now; }
        return;
    }
    __imp__sub_8268C930(ctx, base);
}

// Experiment (FS_PAD_KIND=<n>): report the pad device's kind (vtable[24], sub_821CA5A0: 2 = Xbox 360 pad) as n.
// Kind 3 is the PS3 pad in this shared PC/PS3/360 code; the 360 data still has MENU_BTN_PS3_* button pictures.
PPC_FUNC_IMPL(__imp__sub_821CA5A0);
PPC_FUNC(sub_821CA5A0)
{
    static const int s_kind = [] { const char* e = getenv("FS_PAD_KIND"); return e ? atoi(e) : 0; }();
    __imp__sub_821CA5A0(ctx, base);
    if (s_kind && ctx.r3.u32 == 2)
        ctx.r3.u64 = uint32_t(s_kind);
}

// PlayStation button icons (the tester's default). sub_8268CF98 builds a binding's icon name: for a 360 pad binding it
// takes XBOX360_* from the table at 0x82F42F38 (index = binding +256). The PS3 table at 0x82F42FC0 has the same
// order for the first 20 entries, then 4 extra SENSOR_* entries. While sub_8268CF98 runs, the 360 table holds
// the matching PS3_* names; the 360 data still has the MENU_BTN_PS3_* pictures. FS_XBOX_ICONS=1: Xbox icons.
PPC_FUNC_IMPL(__imp__sub_8268CF98);
PPC_FUNC(sub_8268CF98)
{
    // Step 37 (the tester, Odin: "square is Y / x = B / circle = A / triangle = X"): button_style "odin" keeps the PS3
    // pictures for the sticks, d-pad, L1/R1/L2/R2, START and SELECT, and shows the four face buttons with the 360
    // letter pictures of the letters printed on the device: bottom button (360 A) = B, right (360 B) = A, left
    // (360 X) = Y, top (360 Y) = X. FS_BUTTON_STYLE=playstation|xbox|odin overrides the setting (tests).
    static const Config::ButtonStyle s_style = Config::EffectiveButtonStyle();
    if (s_style == Config::ButtonStyle::Xbox) { __imp__sub_8268CF98(ctx, base); return; }

    constexpr uint32_t kXbox = 0x82F42F38, kPs3 = 0x82F42FC0, kCount = 35; // XBOX360_DPAD_UP .. XBOX360_DPAD_Y
    static std::mutex s_m;
    std::lock_guard lock(s_m);
    auto* xbox = reinterpret_cast<be<uint32_t>*>(base + kXbox);
    auto* ps3 = reinterpret_cast<be<uint32_t>*>(base + kPs3);
    uint32_t saved[kCount];
    for (uint32_t i = 0; i < kCount; i++)
    {
        saved[i] = xbox[i];
        xbox[i] = ps3[i < 20 ? i : i + 4];
    }
    if (s_style == Config::ButtonStyle::Odin)
    {
        // entries 10..13 = XBOX360_A, _B, _X, _Y
        xbox[10] = saved[11]; xbox[11] = saved[10]; xbox[12] = saved[13]; xbox[13] = saved[12];
    }
    __imp__sub_8268CF98(ctx, base);
    for (uint32_t i = 0; i < kCount; i++)
        xbox[i] = saved[i];
}
