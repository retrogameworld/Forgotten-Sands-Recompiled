#include <stdafx.h>
#include <kernel/io/file_system.h>
#include "xex_loader.h"
#include "function.h"
#include "import_log.h"
#include "heap.h"
#include "memory.h"
#include "xstatus.h"
#include <xex.h>
#include <image.h>
#include <os/logger.h>

XexInfo g_xex;

// ---------------------------------------------------------------------------------------
// Export tables (names of every xboxkrnl/xam ordinal, from XenonUtils / Xenia)

namespace
{
    enum ExportType { kFunction = 0, kVariable = 1 };
    struct ExportName { const char* name; int type; };

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), { #NAME, TYPE } }
    const std::unordered_map<uint32_t, ExportName> g_kernelExports =
    {
#include "xbox/xboxkrnl_table.inc"
    };
    const std::unordered_map<uint32_t, ExportName> g_xamExports =
    {
#include "xbox/xam_table.inc"
    };
#undef XE_EXPORT
}

// ---------------------------------------------------------------------------------------
// Kernel variables in guest memory

namespace
{
    std::unordered_map<std::string, uint32_t> g_kernelVariables;
    uint32_t g_timeStampBundle = 0;

    uint32_t AllocVariable(const char* name, size_t size)
    {
        void* p = g_userHeap.AllocPhysical(size, 16);
        memset(p, 0, size);
        uint32_t address = g_memory.MapVirtual(p);
        g_kernelVariables[name] = address;
        return address;
    }

    template<typename T>
    T* Var(uint32_t address) { return reinterpret_cast<T*>(g_memory.Translate(address)); }

    void CreateKernelVariables()
    {
        // XboxKrnlVersion: Major, Minor, Build, ApprovalType, QFE
        uint32_t v = AllocVariable("XboxKrnlVersion", 8);
        *Var<be<uint16_t>>(v + 0) = 2;
        *Var<be<uint16_t>>(v + 2) = 0;
        *Var<be<uint16_t>>(v + 4) = 17559;
        *Var<uint8_t>(v + 6) = 0x80;
        *Var<uint8_t>(v + 7) = 0;

        // XboxHardwareInfo: Flags, NumberOfProcessors, PCIBridgeRevisionID, Reserved[6], BldrMagic, BldrFlags
        v = AllocVariable("XboxHardwareInfo", 16);
        *Var<be<uint32_t>>(v + 0) = 0x00000020;
        *Var<uint8_t>(v + 4) = 6;
        *Var<be<uint16_t>>(v + 12) = 0x4E4E;

        // KeTimeStampBundle: InterruptTime (u64), SystemTime (u64), TickCount (u32); updated every ms
        g_timeStampBundle = AllocVariable("KeTimeStampBundle", 0x18);

        // ExLoadedCommandLine: the command line string
        v = AllocVariable("ExLoadedCommandLine", 1024);
        strcpy(Var<char>(v), "\"default.xex\"");

        // Object types used with ObReferenceObjectByHandle
        v = AllocVariable("ExEventObjectType", 0x40);
        *Var<be<uint32_t>>(v + 0x10) = 'Even';
        v = AllocVariable("ExThreadObjectType", 0x40);
        *Var<be<uint32_t>>(v + 0x10) = 'Thre';

        AllocVariable("KeDebugMonitorData", 0x100);
        AllocVariable("KeCertMonitorData", 0x100);

        // Graphics globals (the real D3D setup comes in step 6e)
        AllocVariable("VdGlobalDevice", 4);
        AllocVariable("VdGlobalXamDevice", 4);
        v = AllocVariable("VdGpuClockInMHz", 4);
        *Var<be<uint32_t>>(v) = 500;
        AllocVariable("VdHSIOCalibrationLock", 0x1C);

        // XexExecutableModuleHandle: pointer to the executable's loader entry (filled at load)
        AllocVariable("XexExecutableModuleHandle", 4);
    }
}

void KernelStartTimeStampUpdater()
{
    std::thread([]
    {
        const auto start = std::chrono::steady_clock::now();
        while (true)
        {
            auto elapsed = std::chrono::steady_clock::now() - start;
            uint64_t interrupt100ns = std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10000000>>>(elapsed).count();
            uint64_t system100ns = std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10000000>>>(
                std::chrono::system_clock::now().time_since_epoch()).count() + 116444736000000000ull;
            uint32_t tick = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

            *Var<be<uint64_t>>(g_timeStampBundle + 0) = interrupt100ns;
            *Var<be<uint64_t>>(g_timeStampBundle + 8) = system100ns;
            *Var<be<uint32_t>>(g_timeStampBundle + 16) = tick;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

// ---------------------------------------------------------------------------------------
// Loader

static const char* HeaderKeyName(uint32_t key)
{
    switch (key)
    {
    case XEX_HEADER_RESOURCE_INFO: return "RESOURCE_INFO";
    case XEX_HEADER_FILE_FORMAT_INFO: return "FILE_FORMAT_INFO";
    case XEX_HEADER_BASE_REFERENCE: return "BASE_REFERENCE";
    case XEX_HEADER_ORIGINAL_BASE_ADDRESS: return "ORIGINAL_BASE_ADDRESS";
    case XEX_HEADER_ENTRY_POINT: return "ENTRY_POINT";
    case XEX_HEADER_IMAGE_BASE_ADDRESS: return "IMAGE_BASE_ADDRESS";
    case XEX_HEADER_IMPORT_LIBRARIES: return "IMPORT_LIBRARIES";
    case XEX_HEADER_CHECKSUM_TIMESTAMP: return "CHECKSUM_TIMESTAMP";
    case XEX_HEADER_ORIGINAL_PE_NAME: return "ORIGINAL_PE_NAME";
    case XEX_HEADER_STATIC_LIBRARIES: return "STATIC_LIBRARIES";
    case XEX_HEADER_TLS_INFO: return "TLS_INFO";
    case XEX_HEADER_DEFAULT_STACK_SIZE: return "DEFAULT_STACK_SIZE";
    case XEX_HEADER_DEFAULT_FILESYSTEM_CACHE_SIZE: return "DEFAULT_FILESYSTEM_CACHE_SIZE";
    case XEX_HEADER_DEFAULT_HEAP_SIZE: return "DEFAULT_HEAP_SIZE";
    case XEX_HEADER_SYSTEM_FLAGS: return "SYSTEM_FLAGS";
    case XEX_HEADER_EXECUTION_INFO: return "EXECUTION_INFO";
    case XEX_HEADER_TITLE_WORKSPACE_SIZE: return "TITLE_WORKSPACE_SIZE";
    case XEX_HEADER_GAME_RATINGS: return "GAME_RATINGS";
    case XEX_HEADER_LAN_KEY: return "LAN_KEY";
    case XEX_HEADER_XBOX360_LOGO: return "XBOX360_LOGO";
    case XEX_HEADER_MULTIDISC_MEDIA_IDS: return "MULTIDISC_MEDIA_IDS";
    case XEX_HEADER_ALTERNATE_TITLE_IDS: return "ALTERNATE_TITLE_IDS";
    case XEX_HEADER_ADDITIONAL_TITLE_MEMORY: return "ADDITIONAL_TITLE_MEMORY";
    case XEX_HEADER_EXPORTS_BY_NAME: return "EXPORTS_BY_NAME";
    default: return "?";
    }
}

static void PatchVariableImports(const uint8_t* xex)
{
    auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
    if (imports == nullptr)
        return;

    std::vector<std::string_view> stringTable;
    auto* pStrTable = reinterpret_cast<const char*>(imports + 1);
    size_t paddedStringOffset = 0;
    for (size_t i = 0; i < imports->numImports; i++)
    {
        stringTable.emplace_back(pStrTable + paddedStringOffset);
        paddedStringOffset += ((stringTable.back().length() + 1) + 3) & ~3;
    }

    uint32_t variables = 0, missing = 0;
    auto* library = (const Xex2ImportLibrary*)(((const char*)imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
    for (size_t i = 0; i < stringTable.size(); i++)
    {
        const bool isXam = stringTable[i] == "xam.xex";
        const auto& names = isXam ? g_xamExports : g_kernelExports;
        auto* descriptors = (const Xex2ImportDescriptor*)(library + 1);

        LOGFN("Import library {} v{:08X}: {} records", stringTable[i], library->version.get(), library->numberOfImports.get());

        for (size_t im = 0; im < library->numberOfImports; im++)
        {
            uint32_t slotAddress = descriptors[im].firstThunk;
            auto* slot = reinterpret_cast<uint32_t*>(g_memory.Translate(slotAddress));
            // Xex2LoadImage byte-swapped every record to host order and replaced function
            // thunks (type 1) with code. Type 0 records still hold (type << 24 | ordinal).
            uint32_t value = *slot;
            uint32_t type = value >> 24;
            uint32_t ordinal = value & 0xFFFF;
            if (type != 0)
                continue;

            auto it = names.find(ordinal);
            if (it == names.end() || it->second.type != kVariable)
            {
                *slot = ByteSwap(value); // function IAT slot: leave as it was in the file
                continue;
            }

            auto var = g_kernelVariables.find(it->second.name);
            if (var == g_kernelVariables.end())
            {
                LOGFN_ERROR("Kernel variable {} ({}) is not provided", it->second.name, ordinal);
                *slot = 0;
                missing++;
                continue;
            }

            *slot = ByteSwap(var->second);
            variables++;
            LOGFN("  variable {} -> 0x{:08X} (slot 0x{:08X})", it->second.name, var->second, slotAddress);
        }

        library = (const Xex2ImportLibrary*)((const char*)(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }

    LOGFN("Kernel variables patched: {}, missing: {}", variables, missing);
}

bool XexLoadExecutable(const std::filesystem::path& path)
{
    std::vector<uint8_t> file; // Step 26: default.xex may be inside the game pak
    FileSystem::ReadHostFile(path, file);
    if (file.empty())
    {
        LOGFN_ERROR("Could not read {}", (const char*)path.u8string().c_str());
        return false;
    }

    LOGFN("Loading {} ({} bytes)", (const char*)path.u8string().c_str(), file.size());

    auto image = Xex2LoadImage(file.data(), file.size());
    if (!image.data)
    {
        LOGFN_ERROR("Xex2LoadImage failed (decryption/decompression error)");
        return false;
    }

    LOGFN("XEX image: base 0x{:08X} size 0x{:X} entry 0x{:08X}", image.base, image.size, image.entry_point);
    if (image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
    {
        LOGFN_ERROR("Image does not match the recompiled code (expected base 0x{:08X} size 0x{:X})", PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        return false;
    }

    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);

    g_xex.entryPoint = uint32_t(image.entry_point);
    g_xex.imageBase = uint32_t(image.base);
    g_xex.imageSize = image.size;
    for (auto& s : image.sections)
    {
        g_xex.sections.push_back({ s.name.c_str(), uint32_t(s.base), s.size });
        LOGFN("  section {:8} 0x{:08X} size 0x{:08X}", s.name.c_str(), uint32_t(s.base), s.size);
    }

    for (auto& sym : image.symbols)
        if (sym.name.starts_with("__imp__"))
            g_xex.importThunks[sym.name.substr(7)] = uint32_t(sym.address);
    LOGFN("Import thunks: {}", g_xex.importThunks.size());

    // Guest copy of the XEX header.
    auto* header = reinterpret_cast<const Xex2Header*>(file.data());
    const uint32_t headerSize = header->headerSize;
    void* headerCopy = g_userHeap.AllocPhysical(headerSize, 0x1000);
    memcpy(headerCopy, file.data(), headerSize);
    g_xex.headerGuest = g_memory.MapVirtual(headerCopy);

    for (uint32_t i = 0; i < header->headerCount; i++)
    {
        const auto& opt = ((const Xex2OptHeader*)(header + 1))[i];
        uint32_t key = opt.key;
        LOGFN("  opt header 0x{:08X} {:28} value/offset 0x{:08X}", key, HeaderKeyName(key), opt.value.get());

        if (key == XEX_HEADER_DEFAULT_STACK_SIZE)
            g_xex.defaultStackSize = opt.value;
        else if (key == XEX_HEADER_SYSTEM_FLAGS)
            g_xex.systemFlags = opt.value;
        else if (key == XEX_HEADER_TLS_INFO)
        {
            auto* tls = (const be<uint32_t>*)(file.data() + opt.offset);
            LOGFN("  TLS info: slots {} raw data 0x{:08X} data size 0x{:X} raw size 0x{:X}",
                tls[0].get(), tls[1].get(), tls[2].get(), tls[3].get());
        }
        else if (key == XEX_HEADER_EXECUTION_INFO)
        {
            auto* exec = (const be<uint32_t>*)(file.data() + opt.offset);
            g_xex.titleId = exec[3];
            LOGFN("  title id {:08X}", g_xex.titleId);
        }
    }

    CreateKernelVariables();
    PatchVariableImports(file.data());

    // Loader entry (X_LDR_DATA_TABLE_ENTRY) for XexExecutableModuleHandle / XexGetModuleHandle.
    auto* ldr = (uint8_t*)g_userHeap.AllocPhysical(0x80, 16);
    memset(ldr, 0, 0x80);
    uint32_t ldrGuest = g_memory.MapVirtual(ldr);
    for (int i = 0; i < 3; i++) // empty list heads pointing at themselves
    {
        *(be<uint32_t>*)(ldr + i * 8) = ldrGuest + i * 8;
        *(be<uint32_t>*)(ldr + i * 8 + 4) = ldrGuest + i * 8;
    }
    *(be<uint32_t>*)(ldr + 0x18) = g_xex.imageBase;  // DllBase
    *(be<uint32_t>*)(ldr + 0x1C) = g_xex.imageBase;  // ImageBase
    *(be<uint32_t>*)(ldr + 0x20) = g_xex.imageSize;  // SizeOfImage
    *(be<uint32_t>*)(ldr + 0x34) = 0x00000000;       // Flags
    *(be<uint32_t>*)(ldr + 0x38) = g_xex.imageSize;  // FullImageSize
    *(be<uint32_t>*)(ldr + 0x3C) = g_xex.entryPoint; // EntryPoint
    *(be<uint16_t>*)(ldr + 0x40) = 1;                // LoadCount
    *(be<uint32_t>*)(ldr + 0x58) = g_xex.headerGuest;// XexHeaderBase
    g_xex.ldrEntry = ldrGuest;

    *Var<be<uint32_t>>(g_kernelVariables["XexExecutableModuleHandle"]) = ldrGuest;

    LOGFN("XEX loaded: entry 0x{:08X}, header copy 0x{:08X}, module entry 0x{:08X}, default stack 0x{:X}",
        g_xex.entryPoint, g_xex.headerGuest, g_xex.ldrEntry, g_xex.defaultStackSize);
    return true;
}

// ---------------------------------------------------------------------------------------
// Xex* / RtlImageXexHeaderField

uint32_t RtlImageXexHeaderField(uint32_t headerBase, uint32_t key)
{
    if (headerBase == 0)
        headerBase = g_xex.headerGuest;

    auto* header = reinterpret_cast<const Xex2Header*>(g_memory.Translate(headerBase));
    auto* opts = reinterpret_cast<const Xex2OptHeader*>(header + 1);
    for (uint32_t i = 0; i < header->headerCount; i++)
    {
        if (opts[i].key != key)
            continue;

        switch (key & 0xFF)
        {
        case 0x00: return opts[i].value;
        case 0x01: return g_memory.MapVirtual(&opts[i].value);
        default:   return headerBase + opts[i].offset;
        }
    }
    return 0;
}

static uint32_t g_fakeModules[2]; // xboxkrnl.exe, xam.xex

uint32_t XexGetModuleHandle(const char* moduleName, be<uint32_t>* handle)
{
    std::string name = moduleName ? moduleName : "";
    for (auto& c : name) c = char(tolower(uint8_t(c)));

    if (name.empty() || name == "default.xex")
    {
        *handle = g_xex.ldrEntry;
        return X_STATUS_SUCCESS;
    }

    int index = name == "xboxkrnl.exe" ? 0 : (name == "xam.xex" ? 1 : -1);
    if (index < 0)
    {
        LOGFN_WARNING("XexGetModuleHandle(\"{}\") - unknown module", name);
        *handle = 0;
        return 0xC0000225; // STATUS_NOT_FOUND
    }

    if (g_fakeModules[index] == 0)
    {
        void* p = g_userHeap.AllocPhysical(0x80, 16);
        memset(p, 0, 0x80);
        g_fakeModules[index] = g_memory.MapVirtual(p);
    }

    LOGFN("XexGetModuleHandle(\"{}\") -> 0x{:08X}", name, g_fakeModules[index]);
    *handle = g_fakeModules[index];
    return X_STATUS_SUCCESS;
}

uint32_t XexGetProcedureAddress(uint32_t module, uint32_t ordinal, be<uint32_t>* address)
{
    const char* name = "?";
    const char* moduleName = module == g_fakeModules[1] ? "xam.xex" : (module == g_fakeModules[0] ? "xboxkrnl.exe" : "default.xex");
    const auto& table = module == g_fakeModules[1] ? g_xamExports : g_kernelExports;
    if (auto it = table.find(ordinal); it != table.end())
        name = it->second.name;

    if (address)
        *address = 0;

    // Functions the XEX also imports statically have a recompiled thunk we can hand out.
    if (auto it = g_xex.importThunks.find(name); it != g_xex.importThunks.end())
    {
        if (address)
            *address = it->second;
        LOGFN("XexGetProcedureAddress({}, {}) -> thunk 0x{:08X}", moduleName, name, it->second);
        return X_STATUS_SUCCESS;
    }

    LOGFN_WARNING("XexGetProcedureAddress({}, ordinal 0x{:X} = {}) - not statically imported, not available", moduleName, ordinal, name);
    return 0xC0000225; // STATUS_NOT_FOUND
}

bool XexWriteTitleImage(const std::filesystem::path& out)
{
    if (g_xex.headerGuest == 0)
        return false;
    const uint8_t* header = static_cast<const uint8_t*>(g_memory.Translate(g_xex.headerGuest));
    auto* info = reinterpret_cast<const be<uint32_t>*>(getOptHeaderPtr(header, XEX_HEADER_RESOURCE_INFO));
    if (info == nullptr)
        return false;
    const uint32_t count = (uint32_t(info[0]) - 4) / 16;
    char want[9];
    snprintf(want, sizeof(want), "%08X", g_xex.titleId);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* entry = reinterpret_cast<const uint8_t*>(info) + 4 + i * 16;
        const std::string name(reinterpret_cast<const char*>(entry), strnlen(reinterpret_cast<const char*>(entry), 8));
        const uint32_t address = uint32_t(*reinterpret_cast<const be<uint32_t>*>(entry + 8));
        const uint32_t size = uint32_t(*reinterpret_cast<const be<uint32_t>*>(entry + 12));
        std::string upper = name;
        for (auto& ch : upper) ch = char(toupper(uint8_t(ch)));
        if (upper != want || size < 24)
            continue;
        const uint8_t* x = static_cast<const uint8_t*>(g_memory.Translate(address));
        auto u16 = [&](uint32_t o) { return uint32_t(x[o]) << 8 | x[o + 1]; };
        auto u32 = [&](uint32_t o) { return uint32_t(x[o]) << 24 | uint32_t(x[o + 1]) << 16 | uint32_t(x[o + 2]) << 8 | x[o + 3]; };
        if (memcmp(x, "XDBF", 4) != 0)
            return false;
        const uint32_t entryTable = u32(8), entries = u32(12), freeTable = u32(16);
        const uint32_t dataStart = 24 + entryTable * 18 + freeTable * 8;
        for (uint32_t k = 0; k < entries && 24 + (k + 1) * 18 <= size; k++)
        {
            const uint32_t e = 24 + k * 18;
            const uint32_t ns = u16(e);
            const uint64_t id = uint64_t(u32(e + 2)) << 32 | u32(e + 6);
            const uint32_t off = u32(e + 10), len = u32(e + 14);
            if (ns != 2 || id != 0x8000 || dataStart + off + len > size || len < 8)
                continue;
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(x + dataStart + off), len);
            LOGFN("[xex] title image ({} bytes) written to {}", len, (const char*)out.u8string().c_str());
            return bool(f);
        }
        return false;
    }
    return false;
}

uint32_t XexGetModuleSection(uint32_t module, const char* name, be<uint32_t>* data, be<uint32_t>* size)
{
    for (auto& s : g_xex.sections)
    {
        if (s.name == name)
        {
            if (data) *data = s.base;
            if (size) *size = s.size;
            return X_STATUS_SUCCESS;
        }
    }

    LOGFN_WARNING("XexGetModuleSection(\"{}\") - not found", name);
    return 0xC0000225;
}

uint32_t XexCheckExecutablePrivilege(uint32_t privilege)
{
    if (privilege >= 32)
        return 0;
    return (g_xex.systemFlags >> privilege) & 1;
}

uint32_t XexUnloadImage(uint32_t handle)
{
    return X_STATUS_SUCCESS;
}

GUEST_IMPORT(RtlImageXexHeaderField, RtlImageXexHeaderField);
GUEST_IMPORT(XexGetModuleHandle, XexGetModuleHandle);
GUEST_IMPORT(XexGetProcedureAddress, XexGetProcedureAddress);
GUEST_IMPORT(XexGetModuleSection, XexGetModuleSection);
GUEST_IMPORT(XexCheckExecutablePrivilege, XexCheckExecutablePrivilege);
GUEST_IMPORT(XexUnloadImage, XexUnloadImage);
