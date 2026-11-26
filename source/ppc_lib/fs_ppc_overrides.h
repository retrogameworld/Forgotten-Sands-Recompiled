// Force-included into every generated ppc_recomp.*.cpp (see CMakeLists.txt).
// An indirect call to an address the recompiler never found as a function start used to jump to a null
// pointer with no clue where it went; now the runtime reports the guest address (PPCMissingFunction).
#pragma once
#include <cstdint>

void PPCMissingFunction(uint32_t guestAddress);

#define PPC_CALL_INDIRECT_FUNC(x) \
    do { auto* fsTarget_ = PPC_LOOKUP_FUNC(base, x); if (fsTarget_ == nullptr) PPCMissingFunction(uint32_t(x)); else fsTarget_(ctx, base); } while (0)

// Step 37 (crashes on the Odin at random places in the game's job / sound code, null pointers read from objects that
// another thread had just set up): the Xbox 360's in-order cores make every store visible in program order in
// practice, and the game relies on that without barriers (x86 does it too); ARM cores do not. On ARM every guest
// store is a store-release (STLR): nothing stored before it can become visible after it. Unaligned stores (rare)
// stay plain. Build with -DFS_PLAIN_STORES to turn it off.
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(FS_PLAIN_STORES)
#define PPC_STORE_U8(x, y) __atomic_store_n((uint8_t*)(base + (x)), uint8_t(y), __ATOMIC_RELEASE)
#define PPC_STORE_U16(x, y) do { uint8_t* fsA_ = base + (x); const uint16_t fsV_ = __builtin_bswap16(uint16_t(y)); \
    if ((uintptr_t(fsA_) & 1) == 0) __atomic_store_n((uint16_t*)fsA_, fsV_, __ATOMIC_RELEASE); else *(volatile uint16_t*)fsA_ = fsV_; } while (0)
#define PPC_STORE_U32(x, y) do { uint8_t* fsA_ = base + (x); const uint32_t fsV_ = __builtin_bswap32(uint32_t(y)); \
    if ((uintptr_t(fsA_) & 3) == 0) __atomic_store_n((uint32_t*)fsA_, fsV_, __ATOMIC_RELEASE); else *(volatile uint32_t*)fsA_ = fsV_; } while (0)
#define PPC_STORE_U64(x, y) do { uint8_t* fsA_ = base + (x); const uint64_t fsV_ = __builtin_bswap64(uint64_t(y)); \
    if ((uintptr_t(fsA_) & 7) == 0) __atomic_store_n((uint64_t*)fsA_, fsV_, __ATOMIC_RELEASE); else *(volatile uint64_t*)fsA_ = fsV_; } while (0)
#endif

// Step 37: and every guest load a load-acquire (LDAPR with +rcpc): nothing loaded after it is read from before it.
// Store-release alone cut the crashes from ~2 in 5 level runs to 1 in 8. -DFS_PLAIN_LOADS turns it off.
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(FS_PLAIN_LOADS)
#define PPC_LOAD_U8(x) __atomic_load_n((uint8_t*)(base + (x)), __ATOMIC_ACQUIRE)
#define PPC_LOAD_U16(x) ({ uint8_t* fsA_ = base + (x); __builtin_bswap16((uintptr_t(fsA_) & 1) == 0 ? \
    __atomic_load_n((uint16_t*)fsA_, __ATOMIC_ACQUIRE) : *(volatile uint16_t*)fsA_); })
#define PPC_LOAD_U32(x) ({ uint8_t* fsA_ = base + (x); __builtin_bswap32((uintptr_t(fsA_) & 3) == 0 ? \
    __atomic_load_n((uint32_t*)fsA_, __ATOMIC_ACQUIRE) : *(volatile uint32_t*)fsA_); })
#define PPC_LOAD_U64(x) ({ uint8_t* fsA_ = base + (x); __builtin_bswap64((uintptr_t(fsA_) & 7) == 0 ? \
    __atomic_load_n((uint64_t*)fsA_, __ATOMIC_ACQUIRE) : *(volatile uint64_t*)fsA_); })
#endif
