#include <stdafx.h>
#include <os/logger.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static const auto g_logStart = std::chrono::steady_clock::now();

void os::logger::Init(const std::filesystem::path& logFile)
{
    std::lock_guard lock(g_logMutex);
    if (!logFile.empty() && g_logFile == nullptr)
    {
#ifdef _WIN32
        g_logFile = _wfopen(logFile.c_str(), L"w");
#else
        g_logFile = fopen(logFile.c_str(), "w");
#endif
    }
}

// Step 31: the Settings window moves its log when the save location changes
void os::logger::Reopen(const std::filesystem::path& logFile)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile)
        fclose(g_logFile);
#ifdef _WIN32
    g_logFile = _wfopen(logFile.c_str(), L"w");
#else
    g_logFile = fopen(logFile.c_str(), "w");
#endif
}

void os::logger::Log(const std::string_view str, ELogType type, const char* func)
{
    const char* prefix = "";
    switch (type)
    {
    case ELogType::Utility: prefix = "[util] "; break;
    case ELogType::Warning: prefix = "[warn] "; break;
    case ELogType::Error:   prefix = "[ERROR] "; break;
    default: break;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_logStart).count();
    std::string line = func ? fmt::format("{:8}ms {}[{}] {}\n", ms, prefix, func, str) : fmt::format("{:8}ms {}{}\n", ms, prefix, str);

    std::lock_guard lock(g_logMutex);
#ifdef __ANDROID__
    __android_log_write(type == ELogType::Error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "Forgotten Sands", line.c_str());
#else
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
#endif
    if (g_logFile)
    {
        fwrite(line.data(), 1, line.size(), g_logFile);
        fflush(g_logFile);
    }
}

void os::logger::Flush()
{
    std::lock_guard lock(g_logMutex);
    fflush(stdout);
    if (g_logFile)
        fflush(g_logFile);
}
