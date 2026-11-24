#pragma once

// Loads default.xex (encrypted + LZX, via XenonUtils' Xex2LoadImage) into guest memory at
// its base address, fills in the kernel variable imports, and keeps the XEX header in guest
// memory for RtlImageXexHeaderField / XexExecutableModuleHandle.

struct XexInfo
{
    uint32_t entryPoint = 0;
    uint32_t imageBase = 0;
    uint32_t imageSize = 0;
    uint32_t headerGuest = 0;     // guest copy of the XEX header
    uint32_t ldrEntry = 0;        // X_LDR_DATA_TABLE_ENTRY for the executable
    uint32_t defaultStackSize = 0;
    uint32_t systemFlags = 0;
    uint32_t titleId = 0;
    struct SectionInfo { std::string name; uint32_t base; uint32_t size; };
    std::vector<SectionInfo> sections;
    std::unordered_map<std::string, uint32_t> importThunks; // "NtCreateFile" -> thunk address
};

extern XexInfo g_xex;

bool XexLoadExecutable(const std::filesystem::path& path);
void KernelStartTimeStampUpdater();
