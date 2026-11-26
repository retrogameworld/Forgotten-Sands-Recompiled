#include <os/process.h>

#include <signal.h>

std::filesystem::path os::process::GetExecutablePath()
{
    char exePath[PATH_MAX] = {};
    if (readlink("/proc/self/exe", exePath, PATH_MAX) > 0)
    {
        return std::filesystem::path(std::u8string_view((const char8_t*)(exePath)));
    }
    else
    {
        return std::filesystem::path();
    }
}


std::filesystem::path os::process::GetExecutableRoot()
{
#ifdef __ANDROID__
    // Step 37: on Android "next to the exe" is the app's own folder on the shared storage
    // (Android/data/<package>/files): the disc image, game folder, settings, saves and log live there.
    // (also called by static constructors at library load, before SDL's Java side is ready: built from the
    // package name instead of SDL_AndroidGetExternalStoragePath)
    static const std::filesystem::path s_root = []
    {
        char cmd[256] = {};
        if (FILE* f = fopen("/proc/self/cmdline", "rb")) { fread(cmd, 1, sizeof(cmd) - 1, f); fclose(f); }
        std::string pkg = cmd;
        if (auto c = pkg.find(':'); c != std::string::npos) pkg.resize(c);
        auto dir = std::filesystem::path("/storage/emulated/0/Android/data") / pkg / "files";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / "";
    }();
    return s_root;
#endif
    return GetExecutablePath().remove_filename();
}

std::filesystem::path os::process::GetWorkingDirectory()
{
    char cwd[PATH_MAX] = {};
    char *res = getcwd(cwd, sizeof(cwd));
    if (res != nullptr)
    {
        return std::filesystem::path(std::u8string_view((const char8_t*)(cwd)));
    }
    else
    {
        return std::filesystem::path();
    }
}

bool os::process::SetWorkingDirectory(const std::filesystem::path& path)
{
    return chdir(path.c_str()) == 0;
}

bool os::process::StartProcess(const std::filesystem::path& path, const std::vector<std::string>& args, std::filesystem::path work)
{
    pid_t pid = fork();
    if (pid < 0)
        return false;

    if (pid == 0)
    {
        setsid();
        
        std::u8string workU8 = work.u8string();
        chdir((const char*)(workU8.c_str())); 
        
        std::u8string pathU8 = path.u8string();
        std::vector<char*> argStrs;
        argStrs.push_back((char*)(pathU8.c_str()));
        for (const std::string& arg : args)
            argStrs.push_back((char *)(arg.c_str()));
        
        argStrs.push_back(nullptr);
        execvp((const char*)(pathU8.c_str()), argStrs.data());
        raise(SIGKILL);
    }

    return true;
}

void os::process::CheckConsole()
{
    // Always visible on Linux.
    g_consoleVisible = true;
}

void os::process::ShowConsole()
{
    // Unnecessary on Linux.
}
