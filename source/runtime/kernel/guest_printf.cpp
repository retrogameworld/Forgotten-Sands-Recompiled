#include <stdafx.h>
#include "guest_printf.h"
#include "function.h"
#include "import_log.h"
#include "memory.h"
#include <cpu/guest_thread.h>
#include <os/logger.h>

uint64_t GuestArgs::Next()
{
    if (isVaList)
    {
        uint64_t v = ByteSwap(*(uint64_t*)g_memory.Translate(vaPointer));
        vaPointer += 8;
        return v;
    }

    uint32_t i = index++;
    switch (i)
    {
    case 0: return ctx->r3.u64;
    case 1: return ctx->r4.u64;
    case 2: return ctx->r5.u64;
    case 3: return ctx->r6.u64;
    case 4: return ctx->r7.u64;
    case 5: return ctx->r8.u64;
    case 6: return ctx->r9.u64;
    case 7: return ctx->r10.u64;
    default: break;
    }

    return ByteSwap(*(uint64_t*)g_memory.Translate(ctx->r1.u32 + 0x50 + (i - 8) * 8));
}

static std::u16string ReadNarrow(uint32_t addr)
{
    std::u16string s;
    if (addr == 0)
        return u"(null)";
    const char* p = (const char*)g_memory.Translate(addr);
    while (*p) s.push_back(uint8_t(*p++));
    return s;
}

static std::u16string ReadWide(uint32_t addr)
{
    std::u16string s;
    if (addr == 0)
        return u"(null)";
    const be<uint16_t>* p = (const be<uint16_t>*)g_memory.Translate(addr);
    while (p->get()) s.push_back(char16_t((p++)->get()));
    return s;
}

static void AppendAscii(std::u16string& out, const char* s)
{
    while (*s) out.push_back(uint8_t(*s++));
}

std::u16string GuestFormatGeneric(const void* format, bool wide, GuestArgs args)
{
    auto fmtChar = [&](size_t i) -> char16_t
    {
        if (wide)
            return char16_t(((const be<uint16_t>*)format)[i].get());
        return char16_t(uint8_t(((const char*)format)[i]));
    };

    std::u16string out;
    size_t i = 0;

    while (true)
    {
        char16_t c = fmtChar(i++);
        if (c == 0)
            break;

        if (c != '%')
        {
            out.push_back(c);
            continue;
        }

        if (fmtChar(i) == '%')
        {
            out.push_back('%');
            i++;
            continue;
        }

        std::string spec = "%";
        // flags
        while (true)
        {
            char16_t f = fmtChar(i);
            if (f == '-' || f == '+' || f == ' ' || f == '#' || f == '0') { spec.push_back(char(f)); i++; }
            else break;
        }
        // width
        if (fmtChar(i) == '*') { spec += std::to_string(int32_t(args.Next())); i++; }
        else while (fmtChar(i) >= '0' && fmtChar(i) <= '9') spec.push_back(char(fmtChar(i++)));
        // precision
        if (fmtChar(i) == '.')
        {
            spec.push_back('.');
            i++;
            if (fmtChar(i) == '*') { spec += std::to_string(int32_t(args.Next())); i++; }
            else while (fmtChar(i) >= '0' && fmtChar(i) <= '9') spec.push_back(char(fmtChar(i++)));
        }
        // length
        enum { LEN_NONE, LEN_H, LEN_L, LEN_LL, LEN_W } len = LEN_NONE;
        while (true)
        {
            char16_t l = fmtChar(i);
            if (l == 'h') { len = LEN_H; i++; }
            else if (l == 'l') { len = (len == LEN_L) ? LEN_LL : LEN_L; i++; }
            else if (l == 'w') { len = LEN_W; i++; }
            else if (l == 'L' || l == 'q' || l == 'j') { len = LEN_LL; i++; }
            else if (l == 'z' || l == 't') { i++; }
            else if (l == 'I')
            {
                if (fmtChar(i + 1) == '6' && fmtChar(i + 2) == '4') { len = LEN_LL; i += 3; }
                else if (fmtChar(i + 1) == '3' && fmtChar(i + 2) == '2') { i += 3; }
                else i++;
            }
            else break;
        }

        char16_t conv = fmtChar(i++);
        if (conv == 0)
            break;

        char buffer[512];
        switch (conv)
        {
        case 'd': case 'i':
        {
            uint64_t v = args.Next();
            if (len == LEN_LL) snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)int64_t(v));
            else if (len == LEN_H) snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int(int16_t(v)));
            else snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int(int32_t(v)));
            AppendAscii(out, buffer);
            break;
        }
        case 'u': case 'x': case 'X': case 'o':
        {
            uint64_t v = args.Next();
            std::string s = spec + (len == LEN_LL ? "ll" : "") + char(conv);
            if (len == LEN_LL) snprintf(buffer, sizeof(buffer), s.c_str(), (unsigned long long)v);
            else if (len == LEN_H) snprintf(buffer, sizeof(buffer), s.c_str(), unsigned(uint16_t(v)));
            else snprintf(buffer, sizeof(buffer), s.c_str(), unsigned(uint32_t(v)));
            AppendAscii(out, buffer);
            break;
        }
        case 'p':
        {
            snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
            AppendAscii(out, buffer);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        {
            uint64_t bits = args.Next();
            double d;
            memcpy(&d, &bits, sizeof(d));
            snprintf(buffer, sizeof(buffer), (spec + char(conv)).c_str(), d);
            AppendAscii(out, buffer);
            break;
        }
        case 'c': case 'C':
        {
            char16_t ch = char16_t(uint16_t(args.Next()));
            if (!wide && conv == 'c' && len != LEN_L && len != LEN_W) ch = uint8_t(ch);
            out.push_back(ch);
            break;
        }
        case 's': case 'S': case 'Z':
        {
            uint32_t addr = uint32_t(args.Next());
            bool wideArg;
            if (len == LEN_H) wideArg = false;
            else if (len == LEN_L || len == LEN_W) wideArg = true;
            else wideArg = (conv == 's') ? wide : !wide;

            std::u16string str = wideArg ? ReadWide(addr) : ReadNarrow(addr);

            // precision / width handling
            size_t dot = spec.find('.');
            if (dot != std::string::npos)
            {
                size_t precision = strtoul(spec.c_str() + dot + 1, nullptr, 10);
                if (str.size() > precision) str.resize(precision);
            }
            bool leftAlign = spec.find('-') != std::string::npos;
            size_t width = 0;
            {
                size_t p = 1;
                while (p < spec.size() && strchr("-+ #0", spec[p])) p++;
                width = strtoul(spec.c_str() + p, nullptr, 10);
            }
            if (str.size() < width && !leftAlign) out.append(width - str.size(), ' ');
            out += str;
            if (str.size() < width && leftAlign) out.append(width - str.size(), ' ');
            break;
        }
        case 'n':
            args.Next();
            break;
        default:
            out.push_back('%');
            out.push_back(conv);
            break;
        }
    }

    return out;
}

std::string GuestFormat(const char* format, GuestArgs args)
{
    std::u16string w = GuestFormatGeneric(format, false, args);
    std::string s;
    s.reserve(w.size());
    for (char16_t c : w) s.push_back(c < 256 ? char(c) : '?');
    return s;
}

// int sprintf(char* buffer, const char* format, ...)
PPC_FUNC(__imp__sprintf)
{
    static std::atomic<bool> s_logged;
    if (!s_logged.exchange(true)) ImportTrace("sprintf");

    char* buffer = (char*)g_memory.Translate(ctx.r3.u32);
    const char* format = (const char*)g_memory.Translate(ctx.r4.u32);
    std::string s = GuestFormat(format, GuestArgs::FromRegisters(ctx, 2));
    memcpy(buffer, s.c_str(), s.size() + 1);
    ctx.r3.u64 = uint32_t(s.size());
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args)
PPC_FUNC(__imp___vsnprintf)
{
    static std::atomic<bool> s_logged;
    if (!s_logged.exchange(true)) ImportTrace("_vsnprintf");

    char* buffer = (char*)g_memory.Translate(ctx.r3.u32);
    uint32_t count = ctx.r4.u32;
    const char* format = (const char*)g_memory.Translate(ctx.r5.u32);
    std::string s = GuestFormat(format, GuestArgs::FromVaList(ctx.r6.u32));

    if (s.size() < count)
    {
        memcpy(buffer, s.c_str(), s.size() + 1);
        ctx.r3.u64 = uint32_t(s.size());
    }
    else
    {
        memcpy(buffer, s.data(), count);
        ctx.r3.u64 = s.size() == count ? count : uint32_t(-1);
    }
}

// int vswprintf(wchar_t* buffer, const wchar_t* format, va_list args)  (old MS signature, no count)
PPC_FUNC(__imp__vswprintf)
{
    static std::atomic<bool> s_logged;
    if (!s_logged.exchange(true)) ImportTrace("vswprintf");

    be<uint16_t>* buffer = (be<uint16_t>*)g_memory.Translate(ctx.r3.u32);
    const void* format = g_memory.Translate(ctx.r4.u32);
    std::u16string s = GuestFormatGeneric(format, true, GuestArgs::FromVaList(ctx.r5.u32));

    for (size_t i = 0; i < s.size(); i++)
        buffer[i] = uint16_t(s[i]);
    buffer[s.size()] = 0;
    ctx.r3.u64 = uint32_t(s.size());
}
