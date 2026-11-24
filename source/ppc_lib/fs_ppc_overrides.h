// Force-included into every generated ppc_recomp.*.cpp (see CMakeLists.txt).
// An indirect call to an address the recompiler never found as a function start used to jump to a null
// pointer with no clue where it went; now the runtime reports the guest address (PPCMissingFunction).
#pragma once
#include <cstdint>

void PPCMissingFunction(uint32_t guestAddress);

#define PPC_CALL_INDIRECT_FUNC(x) \
    do { auto* fsTarget_ = PPC_LOOKUP_FUNC(base, x); if (fsTarget_ == nullptr) PPCMissingFunction(uint32_t(x)); else fsTarget_(ctx, base); } while (0)
