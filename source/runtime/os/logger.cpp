#include <stdafx.h>
#include <os/logger.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
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

// Step 38 (Android): an app's stderr goes nowhere, and plume (the Vulkan layer) reports every failure there - why a
// device could not be made, why a pipeline failed. A pipe in place of stderr feeds those lines into the log.
void os::logger::CaptureStderr()
{
#ifdef __ANDROID__
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;
    int fds[2];
    if (pipe(fds) != 0)
        return;
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (dup2(fds[1], 2) < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    close(fds[1]);
    std::thread([fd = fds[0]] {
        std::string pending;
        char buf[1024];
        for (;;)
        {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            pending.append(buf, size_t(n));
            for (size_t at; (at = pending.find('\n')) != std::string::npos;)
            {
                if (at > 0)
                    Log(std::string_view(pending.data(), at), ELogType::None, "stderr");
                pending.erase(0, at + 1);
            }
        }
    }).detach();
#endif
}
