#pragma once

// Logging (from UnleashedRecomp, made portable: one implementation in os/logger.cpp
// that writes to stdout and to a log file, thread safe, no console colour APIs).

#include <source_location>

#define LOG_IMPL(type, func, str)       os::logger::Log(str, os::logger::ELogType::type, func)
#define LOGF_IMPL(type, func, str, ...) os::logger::Log(fmt::format(str __VA_OPT__(,) __VA_ARGS__), os::logger::ELogType::type, func)

#define LOG(str)               LOG_IMPL(None, __func__, str)
#define LOG_WARNING(str)       LOG_IMPL(Warning, __func__, str)
#define LOG_ERROR(str)         LOG_IMPL(Error, __func__, str)
#define LOG_UTILITY(str)       do { if (os::logger::g_verbose) LOG_IMPL(Utility, __func__, str); } while (0)

#define LOGF(str, ...)         LOGF_IMPL(None, __func__, str, __VA_ARGS__)
#define LOGF_WARNING(str, ...) LOGF_IMPL(Warning, __func__, str, __VA_ARGS__)
#define LOGF_ERROR(str, ...)   LOGF_IMPL(Error, __func__, str, __VA_ARGS__)
#define LOGF_UTILITY(str, ...) do { if (os::logger::g_verbose) LOGF_IMPL(Utility, __func__, str, __VA_ARGS__); } while (0)

#define LOGN(str)               LOG_IMPL(None, "*", str)
#define LOGN_WARNING(str)       LOG_IMPL(Warning, "*", str)
#define LOGN_ERROR(str)         LOG_IMPL(Error, "*", str)
#define LOGN_UTILITY(str)       do { if (os::logger::g_verbose) LOG_IMPL(Utility, "*", str); } while (0)

#define LOGFN(str, ...)         LOGF_IMPL(None, "*", str, __VA_ARGS__)
#define LOGFN_WARNING(str, ...) LOGF_IMPL(Warning, "*", str, __VA_ARGS__)
#define LOGFN_ERROR(str, ...)   LOGF_IMPL(Error, "*", str, __VA_ARGS__)
#define LOGFN_UTILITY(str, ...) do { if (os::logger::g_verbose) LOGF_IMPL(Utility, "*", str, __VA_ARGS__); } while (0)

namespace os::logger
{
    enum class ELogType
    {
        None,
        Utility,
        Warning,
        Error
    };

    inline bool g_verbose = false;

    void Init(const std::filesystem::path& logFile = {});
    void Log(const std::string_view str, ELogType type = ELogType::None, const char* func = nullptr);
    void Flush();
    void CaptureStderr(); // Step 38 (Android): lines written to stderr (plume's Vulkan errors) go into the log
    void Reopen(const std::filesystem::path& logFile); // closes the current log file, starts a new one
}
