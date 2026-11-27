#include <stdafx.h>
#include <os/logger.h>

// Step 38d (Android 1.2, GitHub #1: Retroid Pocket 5 / Adreno 650): the Qualcomm driver of Adreno 6xx devices has no
// 64-bit shader integers, which the translated shaders need. Players of such devices can choose a custom Vulkan driver
// (a Mesa "Turnip" build, the kind Android emulators use) in Settings > GPU driver. The app unpacks the driver .zip into
// its private folder and passes it here (environment, GameActivity):
//   FS_GPU_DRIVER_DIR  folder with the driver library (ends with '/')
//   FS_GPU_DRIVER_LIB  file name of the driver library (from the zip's meta.json "libraryName")
//   FS_GPU_DRIVER_NAME what the player sees ("Turnip ..." from meta.json "name" + "driverVersion")
//   FS_NATIVE_LIB_DIR  the app's native library folder (libadrenotools' hook libraries are there)
// libadrenotools (BSD-2) loads the system Vulkan loader in its own linker namespace with the custom driver in place of
// the system one (the system loader still does the Android swap chain). Built only with FS_HAVE_ADRENOTOOLS; without
// it the choice is reported as unavailable and the system driver is used.
#ifdef __ANDROID__
#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>
#ifdef FS_HAVE_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif

static std::string g_driverStatus = "system";      // shown in the log and next to the frame rate
static std::string g_customDriverProblem;          // why a chosen custom driver is not used (empty = no problem)

const std::string& FsCustomDriverProblem() { return g_customDriverProblem; }
const std::string& FsDriverStatus() { return g_driverStatus; }

// Returns a handle of a Vulkan loader library using the custom driver, or nullptr (then the system driver is used).
void* FsOpenCustomVulkanLibrary()
{
    // The app switched back to the system driver because the custom one did not get the game running last time.
    if (const char* failed = getenv("FS_GPU_DRIVER_FAILED_LAST"); failed && *failed)
    {
        g_customDriverProblem = std::string("it did not get the game running last time (") + failed + ": the game froze, closed or showed no picture)";
        LOGFN_WARNING("[gpu] custom GPU driver {}: {}", failed, g_customDriverProblem);
        return nullptr;
    }
    const char* dir = getenv("FS_GPU_DRIVER_DIR");
    const char* lib = getenv("FS_GPU_DRIVER_LIB");
    const char* name = getenv("FS_GPU_DRIVER_NAME");
    if (dir == nullptr || *dir == 0)
        return nullptr; // the system driver was chosen
    const std::string shown = name && *name ? name : (lib ? lib : "custom driver");
    if (lib == nullptr || *lib == 0)
    {
        g_customDriverProblem = "the driver package has no library";
        LOGFN_WARNING("[gpu] custom GPU driver {}: {}", shown, g_customDriverProblem);
        return nullptr;
    }
#ifdef FS_HAVE_ADRENOTOOLS
    const char* hookDir = getenv("FS_NATIVE_LIB_DIR");
    void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hookDir, dir, lib, nullptr, nullptr);
    if (handle == nullptr)
    {
        const char* err = dlerror();
        g_customDriverProblem = "it could not be loaded (" + std::string(err ? err : "unknown error") + ")";
        LOGFN_WARNING("[gpu] custom GPU driver {} ({}{}): {}", shown, dir, lib, g_customDriverProblem);
        // the player is told now; the "did it get running" trial does not apply (test: FS_TEST_KEEP_TRIAL=1 keeps it)
        if (const char* marker = getenv("FS_GPU_DRIVER_MARKER"); marker && getenv("FS_TEST_KEEP_TRIAL") == nullptr)
            unlink(marker);
        return nullptr;
    }
    g_driverStatus = shown;
    LOGFN("[gpu] custom GPU driver loaded: {} ({}{})", shown, dir, lib);
    return handle;
#else
    g_customDriverProblem = "custom GPU drivers are not supported by this build";
    LOGFN_WARNING("[gpu] custom GPU driver {}: {}", shown, g_customDriverProblem);
    return nullptr;
#endif
}

// The device came from the system driver although a custom one was "loaded" (libadrenotools falls back to the system
// driver when the custom library cannot be used): report it like a load failure.
void FsCustomDriverNotUsed(const std::string& why)
{
    if (g_driverStatus == "system")
        return;
    LOGFN_WARNING("[gpu] custom GPU driver {}: {}", g_driverStatus, why);
    g_customDriverProblem = why;
    g_driverStatus = "system";
    if (const char* marker = getenv("FS_GPU_DRIVER_MARKER"); marker && getenv("FS_TEST_KEEP_TRIAL") == nullptr)
        unlink(marker);
}

// Called once per presented frame: after 2 minutes of frames with a custom driver the app's "trial" marker is removed
// (FS_GPU_DRIVER_MARKER): the driver works. If the game freezes or closes before that, the next start uses the system
// driver again (GameActivity) and says why.
void FsGpuDriverFramePresented()
{
    static const char* s_marker = getenv("FS_GPU_DRIVER_MARKER");
    static std::chrono::steady_clock::time_point s_first{};
    static bool s_done = false;
    if (s_done || s_marker == nullptr || g_driverStatus == "system")
        return;
    const auto now = std::chrono::steady_clock::now();
    if (s_first == std::chrono::steady_clock::time_point{})
        s_first = now;
    if (now - s_first >= std::chrono::seconds(120))
    {
        s_done = true;
        unlink(s_marker);
        LOGFN("[gpu] custom GPU driver {}: 2 minutes of frames, kept", g_driverStatus);
    }
}

// "system" or the custom driver's name, for the frame rate counter (GameActivity)
extern "C" JNIEXPORT jstring JNICALL Java_com_fsrecomp_forgottensands_GameActivity_nativeGpuDriver(JNIEnv* env, jclass)
{
    return env->NewStringUTF(g_driverStatus.c_str());
}
#endif
