#pragma once

// Step 9a - loading-time instrumentation (our own code).
//
// 1. Counters: cheap atomics (count + nanoseconds) for file opens/reads, shader translation, pipeline
//    creation, texture uploads, guest sleeps, frame times. A background thread prints one "[perf]" line
//    per second while something is happening (loading), and nothing in steady gameplay.
// 2. Sampling profiler (debug only, Windows only): TL_PROFILE="35-45[,60-70]" (seconds since start)
//    samples every thread every ~1 ms with an unwound native call stack (the recompiled guest functions
//    are native functions, so this is also the guest call stack) and writes
//    <exe folder>/profile_<from>_<to>.txt with per-thread inclusive / exclusive function lists.

namespace Perf
{
    enum Counter : uint32_t
    {
        FileOpen,        // NtCreateFile / NtOpenFile (all)
        FileOpenMissing, // ... that ended "not found"
        FileRead,        // NtReadFile (count, bytes in Bytes[FileRead])
        FileQuery,       // NtQueryFullAttributesFile / NtQueryDirectoryFile / NtQueryInformationFile
        ShaderTranslate, // Xenos -> HLSL -> SPIR-V (shader cache miss)
        ShaderCacheLoad, // SPIR-V read from the on-disk cache
        Pipeline,        // Vulkan pipeline creation
        TextureUpload,   // CPU untile/convert + staging copy
        TextureCreate,   // host texture creation (CreateTexture / D3DX DDS)
        GuestSleep,      // KeDelayExecutionThread (requested time)
        GuestCompile,    // D3DXCompileShader in guest code
        FrameWait,       // frame limiter sleep
        ShaderWait,      // Step 11: draw waiting for a shader a worker thread is still translating
        Count_
    };

    void Add(Counter c, uint64_t ns, uint64_t bytes = 0);
    uint64_t Count(Counter c);       // calls so far
    // true if the game did loading work (file opens/reads, resource or shader creation) since the last call
    bool LoadingActivitySinceLastCheck();
    uint64_t NowNs();

    struct Scope
    {
        Counter c;
        uint64_t start;
        uint64_t bytes = 0;
        explicit Scope(Counter counter) : c(counter), start(NowNs()) {}
        ~Scope() { Add(c, NowNs() - start, bytes); }
    };

    // Called at every present (frame times for the per-second line).
    void FramePresented();

    // Step 11: true if a frame within the last windowNs was long (> 25 ms) or did loading work.
    bool RecentlyBusy(uint64_t windowNs);

    // Starts the per-second reporter (and the profiler when TL_PROFILE is set). Safe to call twice.
    void Init();
    void Shutdown();
}
