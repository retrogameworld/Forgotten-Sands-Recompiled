#pragma once

// printf-style formatting of guest strings with guest varargs, for the kernel's
// sprintf/_vsnprintf/vswprintf/DbgPrint exports. Xbox 360 varargs are 8-byte slots:
// r3..r10 first, then the caller's parameter area at r1+0x50; a va_list points at slots.
// Doubles travel in the integer slots too.

struct GuestArgs
{
    const PPCContext* ctx = nullptr;
    uint32_t index = 0;      // next argument number (register/stack mode)
    uint32_t vaPointer = 0;  // next slot address (va_list mode)
    bool isVaList = false;

    static GuestArgs FromRegisters(const PPCContext& ctx, uint32_t firstArg)
    {
        GuestArgs a;
        a.ctx = &ctx;
        a.index = firstArg;
        return a;
    }

    static GuestArgs FromVaList(uint32_t va)
    {
        GuestArgs a;
        a.vaPointer = va;
        a.isVaList = true;
        return a;
    }

    uint64_t Next();
};

// format is read as narrow (wide == false) or big-endian UTF-16 (wide == true).
std::u16string GuestFormatGeneric(const void* format, bool wide, GuestArgs args);
std::string GuestFormat(const char* format, GuestArgs args);
