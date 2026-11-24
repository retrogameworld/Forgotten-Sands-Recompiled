#pragma once

// Import logging helpers.
//  - GUEST_IMPORT(name, func): real implementation; the first call is traced once
//    (shown when trace_imports is on in the config, default on).
//  - GUEST_IMPORT_STUB(name, ret): not implemented; logs "[STUB] name" once and returns ret in r3.
//  - import_fallback.cpp defines a weak "[UNIMPLEMENTED]" version of every import the
//    recompiled code references, so anything without a real definition still logs once.

#include "function.h"

void ImportTrace(const char* name);
void ImportStub(const char* name, uint32_t ret);
void ImportUnimplemented(const char* name);

#define GUEST_IMPORT(name, function) \
    PPC_FUNC(__imp__##name) \
    { \
        static std::atomic<bool> s_logged; \
        if (!s_logged.exchange(true, std::memory_order_relaxed)) ImportTrace(#name); \
        HostToGuestFunction<function>(ctx, base); \
    }

#define GUEST_IMPORT_STUB(name, ret) \
    PPC_FUNC(__imp__##name) \
    { \
        static std::atomic<bool> s_logged; \
        if (!s_logged.exchange(true, std::memory_order_relaxed)) ImportStub(#name, uint32_t(ret)); \
        ctx.r3.u64 = uint32_t(ret); \
    }

// Log something only the first time this line runs.
#define LOG_ONCE(...) \
    do { static std::atomic<bool> s_once; if (!s_once.exchange(true)) LOGFN_WARNING(__VA_ARGS__); } while (0)
