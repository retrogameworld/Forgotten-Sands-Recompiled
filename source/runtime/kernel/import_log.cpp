#include <stdafx.h>
#include "import_log.h"
#include <os/logger.h>
#include <user/config.h>

void ImportTrace(const char* name)
{
    if (Config::TraceImports)
        LOGFN("[import] {}", name);
}

void ImportStub(const char* name, uint32_t ret)
{
    LOGFN_WARNING("[STUB] {} (returns 0x{:X})", name, ret);
}

void ImportUnimplemented(const char* name)
{
    LOGFN_ERROR("[UNIMPLEMENTED IMPORT] {} (returns 0)", name);
}
