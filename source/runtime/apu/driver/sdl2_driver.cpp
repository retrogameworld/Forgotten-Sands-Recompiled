// From UnleashedRecomp apu/driver/sdl2_driver.cpp (Sonic config enums replaced).
#include <stdafx.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <os/logger.h>
#include <user/config.h>

// Forgotten Sands registers TWO render driver clients (two XAudio2 engines: the Dare sound engine's and the
// one used for the logo movies) and never unregisters the first. Every client must get its callback each
// audio frame; their frames are summed. Before, the second registration replaced the first, so the game's
// sound engine was never run again after the logos: no menu music, no sound at all in game.
struct AudioClient { uint32_t driver; PPCFunc* callback; uint32_t param; };
static std::mutex g_clientMutex;
static std::vector<AudioClient> g_clients;
static PPCFunc* g_clientCallback{}; // non-null while any client is registered
static std::array<float, XAUDIO_NUM_CHANNELS * XAUDIO_NUM_SAMPLES> g_mixFrame; // planar, host order
static uint32_t g_mixSubmissions = 0;
static void OutputFrame(const float* planar);
static SDL_AudioDeviceID g_audioDevice{};
static bool g_downMixToStereo;

static void CreateAudioDevice()
{
    if (g_audioDevice != NULL)
        SDL_CloseAudioDevice(g_audioDevice);

    bool surround = Config::Surround;
    int allowedChanges = surround ? SDL_AUDIO_ALLOW_CHANNELS_CHANGE : 0;

    SDL_AudioSpec desired{}, obtained{};
    desired.freq = XAUDIO_SAMPLES_HZ;
    desired.format = AUDIO_F32SYS;
    desired.channels = surround ? XAUDIO_NUM_CHANNELS : 2;
    desired.samples = XAUDIO_NUM_SAMPLES;
    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, allowedChanges);

    if (obtained.channels != 2 && obtained.channels != XAUDIO_NUM_CHANNELS) // This check may fail only when surround sound is enabled.
    {
        SDL_CloseAudioDevice(g_audioDevice);
        g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    }

    if (!g_audioDevice)
        LOGFN_ERROR("Failed to open audio device: {}", SDL_GetError());

    g_downMixToStereo = (obtained.channels == 2);
    LOGFN("[audio] device opened: {} Hz, {} channels, buffer {} samples ({:.1f} ms), driver {}", obtained.freq, obtained.channels, obtained.samples,
        obtained.freq ? 1000.0 * obtained.samples / obtained.freq : 0.0, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
}

void XAudioInitializeSystem()
{
    // FS_MUTE=1 (Claude's automated test runs): no sound at all. SDL's dummy driver accepts and discards
    // the samples, so the game's audio timing still runs.
    // FS_MUTE=2: the real WASAPI device with every sample zeroed (silent, but with real device timing).
    const char* mute = getenv("FS_MUTE");
    const bool muted = mute && *mute && *mute != '0';
    const bool realSilent = mute && strcmp(mute, "2") == 0;
    if (realSilent)
        Config::MasterVolume = 0.0f;
#ifdef _WIN32
    // Force wasapi on Windows.
    SDL_setenv("SDL_AUDIODRIVER", (muted && !realSilent) ? "dummy" : "wasapi", true);
#endif
    if (muted)
        LOGFN("[audio] muted (FS_MUTE): using SDL's dummy audio driver");

    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Forgotten Sands Recompiled");

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    {
        LOGFN_ERROR("Failed to init audio subsystem: {}", SDL_GetError());
        return;
    }

    CreateAudioDevice();
}

static std::unique_ptr<std::thread> g_audioThread;
static volatile bool g_audioThreadShouldExit;

static void AudioThread()
{
    using namespace std::chrono_literals;

    GuestThreadContext ctx(0);

    size_t channels = g_downMixToStereo ? 2 : XAUDIO_NUM_CHANNELS;
#ifdef _WIN32
    // Step 35b: the game's sound engine runs in this thread's callbacks; under CPU load it was preempted and the
    // device ran dry (see the clock lock below).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    // Step 35b (the tester: sounds drift out of sync - the first explosion fine, the second late): every time the device
    // ran dry it played silence and the whole sound timeline moved later, for good (the tester's log: 80 ms per 30 s).
    // The sound engine's clock is now locked to real time: frames the device missed are still run through the
    // game's mixer (so its clock and every voice advance) but not played.
    static const bool s_noClockLock = getenv("FS_NO_AUDIO_CLOCK_LOCK") != nullptr;
    auto clockStart = std::chrono::steady_clock::time_point{};
    uint64_t framesMade = 0, framesSkipped = 0;
    auto lastSkipLog = std::chrono::steady_clock::now();

    while (!g_audioThreadShouldExit)
    {
        uint32_t queuedAudioSize = SDL_GetQueuedAudioSize(g_audioDevice);
        constexpr size_t MAX_LATENCY = 10;
        const size_t callbackAudioSize = channels * XAUDIO_NUM_SAMPLES * sizeof(float);

        // Step 12c: log when the device ran dry (an audible gap / click) and slow game audio callbacks
        static uint32_t underruns = 0;
        static auto lastUnderrunLog = std::chrono::steady_clock::now();
        if (queuedAudioSize == 0 && g_clientCallback)
        {
            underruns++;
            const auto t = std::chrono::steady_clock::now();
            if (t - lastUnderrunLog > 1s)
            {
                LOGFN("[audio] output ran dry {} time(s) (gaps)", underruns);
                underruns = 0;
                lastUnderrunLog = t;
            }
        }
        // Step 12c: keep a few frames queued (about 21 ms). Before, one frame was made per wake-up, so the
        // queue sat at 0-1 frames and every late wake-up left the device without sound: ~30 tiny gaps
        // a second, heard as a repeating click (the tester, most audible on quiet screens like Exit to Title).
        constexpr size_t TARGET_FRAMES = 4;
        // Frames the device should have played by now (real time) vs frames it really got.
        size_t skip = 0;
        if (!s_noClockLock && clockStart != std::chrono::steady_clock::time_point{} && g_clientCallback)
        {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart).count();
            const double due = elapsed * XAUDIO_SAMPLES_HZ / XAUDIO_NUM_SAMPLES;
            const double played = double(framesMade) - double(queuedAudioSize / callbackAudioSize);
            if (due - played > 3.0)
                skip = std::min<size_t>(size_t(due - played - 1.0), 64);
        }
        for (size_t made = 0; made < MAX_LATENCY + skip && ((queuedAudioSize / callbackAudioSize) < TARGET_FRAMES || skip > 0); made++)
        {
            std::vector<AudioClient> clients;
            {
                std::lock_guard lock(g_clientMutex);
                clients = g_clients;
            }
            g_mixFrame.fill(0.0f);
            g_mixSubmissions = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (const auto& client : clients)
            {
                ctx.ppcContext.r3.u32 = client.param;
                client.callback(ctx.ppcContext, g_memory.base);
            }
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 20.0)
                LOGFN("[audio] game audio callbacks took {:.0f} ms", ms);
            if (g_mixSubmissions == 0)
                break; // the game made no new frame
            if (clockStart == std::chrono::steady_clock::time_point{})
                clockStart = std::chrono::steady_clock::now();
            framesMade++;
            if (skip > 0)
            {
                skip--;
                framesSkipped++;
                continue; // missed by the device: mixed, not played
            }
            OutputFrame(g_mixFrame.data());
            queuedAudioSize = SDL_GetQueuedAudioSize(g_audioDevice);
        }
        if (framesSkipped && std::chrono::steady_clock::now() - lastSkipLog > 5s)
        {
            LOGFN("[audio] kept in time with the clock: {} frames ({:.0f} ms) the device missed were skipped", framesSkipped, framesSkipped * 1000.0 * XAUDIO_NUM_SAMPLES / XAUDIO_SAMPLES_HZ);
            framesSkipped = 0;
            lastSkipLog = std::chrono::steady_clock::now();
        }

        auto now = std::chrono::steady_clock::now();
        constexpr auto INTERVAL = 1000000000ns * XAUDIO_NUM_SAMPLES / XAUDIO_SAMPLES_HZ;
        auto next = now + (INTERVAL - now.time_since_epoch() % INTERVAL);

        std::this_thread::sleep_for(std::chrono::floor<std::chrono::milliseconds>(next - now));

        while (std::chrono::steady_clock::now() < next)
            std::this_thread::yield();
    }
}

// Forgotten Sands unregisters and registers its render driver client again after the logos: the old
// thread must be stopped first (assigning over a running std::thread aborts the process).
static void StopAudioThread()
{
    if (!g_audioThread)
        return;
    g_audioThreadShouldExit = true;
    if (g_audioThread->get_id() == std::this_thread::get_id())
        g_audioThread->detach();
    else if (g_audioThread->joinable())
        g_audioThread->join();
    g_audioThread.reset();
}

static void CreateAudioThread()
{
    StopAudioThread();
    SDL_PauseAudioDevice(g_audioDevice, 0);
    g_audioThreadShouldExit = false;
    g_audioThread = std::make_unique<std::thread>(AudioThread);
}

void XAudioUnregisterClient(uint32_t driver)
{
    bool empty;
    {
        std::lock_guard lock(g_clientMutex);
        std::erase_if(g_clients, [&](const AudioClient& c) { return c.driver == driver; });
        empty = g_clients.empty();
    }
    LOGFN("[audio] render client 0x{:08X} unregistered ({} left)", driver, g_clients.size());
    if (empty)
    {
        StopAudioThread();
        g_clientCallback = nullptr;
    }
}

void XAudioRegisterClient(uint32_t driver, PPCFunc* callback, uint32_t param)
{
    auto* pClientParam = static_cast<uint32_t*>(g_userHeap.Alloc(sizeof(param)));
    ByteSwapInplace(param);
    *pClientParam = param;
    size_t count;
    {
        std::lock_guard lock(g_clientMutex);
        g_clients.push_back({ driver, callback, g_memory.MapVirtual(pClientParam) });
        count = g_clients.size();
    }
    LOGFN("[audio] render client 0x{:08X} registered ({} total)", driver, count);
    g_clientCallback = callback;
    if (!g_audioThread)
        CreateAudioThread();
}

// Optional capture of the mixed output (stereo downmix, 16-bit 48 kHz WAV): audio_dump = "file.wav" in the
// config or --audio-dump <file.wav>. The header is rewritten about once a second so the file stays valid
// even if the process is killed. Also logs the peak level every ~5 seconds (quick "is it silent?" check).
static std::ofstream g_wavFile;
static uint32_t g_wavDataBytes = 0;
static bool g_wavTried = false;

static void WriteWavHeader()
{
    auto put32 = [](uint8_t* p, uint32_t v) { memcpy(p, &v, 4); };
    auto put16 = [](uint8_t* p, uint16_t v) { memcpy(p, &v, 2); };
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ' };
    put32(h + 4, 36 + g_wavDataBytes);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, XAUDIO_SAMPLES_HZ);
    put32(h + 28, XAUDIO_SAMPLES_HZ * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, g_wavDataBytes);
    auto pos = g_wavFile.tellp();
    g_wavFile.seekp(0);
    g_wavFile.write((const char*)h, 44);
    g_wavFile.seekp(pos);
    g_wavFile.flush();
}

static void CaptureFrame(const float* floatSamples)
{
    static uint64_t frames = 0;
    static float peak = 0.0f;
    static uint64_t nonSilentFrames = 0;
    float framePeak = 0.0f;

    int16_t pcm[2 * XAUDIO_NUM_SAMPLES];
    for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
    {
        float c[6];
        for (size_t j = 0; j < 6; j++) c[j] = floatSamples[j * XAUDIO_NUM_SAMPLES + i];
        float l = c[0] + c[2] * 0.75f + c[4];
        float r = c[1] + c[2] * 0.75f + c[5];
        framePeak = std::max(framePeak, std::max(std::fabs(l), std::fabs(r)));
        pcm[i * 2 + 0] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
        pcm[i * 2 + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
    }
    peak = std::max(peak, framePeak);
    if (framePeak > 0.001f) nonSilentFrames++;

    if (!g_wavTried && Config::AudioDump.empty())
        if (const char* e = getenv("FS_AUDIO_DUMP"); e && *e) Config::AudioDump = e; // test runs: record silently
    if (!Config::AudioDump.empty() && !g_wavTried)
    {
        g_wavTried = true;
        g_wavFile.open(std::filesystem::path(std::u8string_view((const char8_t*)Config::AudioDump.c_str())), std::ios::binary | std::ios::trunc);
        if (g_wavFile) { WriteWavHeader(); LOGFN("[audio] capturing mixed output to {}", Config::AudioDump); }
        else LOGFN_ERROR("[audio] cannot open {}", Config::AudioDump);
    }
    if (g_wavFile)
    {
        g_wavFile.write((const char*)pcm, sizeof(pcm));
        g_wavDataBytes += sizeof(pcm);
        if ((frames % 188) == 0) WriteWavHeader(); // ~1 s
    }

    if ((++frames % 940) == 0) // ~5 s at 187.5 frames/s
    {
        const uint32_t queued = g_audioDevice ? SDL_GetQueuedAudioSize(g_audioDevice) : 0;
        const double queuedMs = 1000.0 * queued / (double(XAUDIO_SAMPLES_HZ) * sizeof(float) * (g_downMixToStereo ? 2 : XAUDIO_NUM_CHANNELS));
        LOGFN("[audio] {:.0f} s: peak {:.3f}, non-silent frames {} of 940, queued {:.1f} ms", frames * double(XAUDIO_NUM_SAMPLES) / XAUDIO_SAMPLES_HZ, peak, nonSilentFrames, queuedMs);
        peak = 0.0f;
        nonSilentFrames = 0;
    }
}

// Called by each client's callback: add its frame (planar 6 x 256, big-endian floats) to this tick's mix.
void XAudioSubmitFrame(void* samples)
{
    auto in = reinterpret_cast<be<float>*>(samples);
    for (size_t i = 0; i < g_mixFrame.size(); i++)
        g_mixFrame[i] += in[i];
    g_mixSubmissions++;
}

static void OutputFrame(const float* floatSamples)
{
    CaptureFrame(floatSamples);

    if (g_downMixToStereo)
    {
        // 0: left 1.0f, right 0.0f
        // 1: left 0.0f, right 1.0f
        // 2: left 0.75f, right 0.75f
        // 3: left 0.0f, right 0.0f
        // 4: left 1.0f, right 0.0f
        // 5: left 0.0f, right 1.0f

        std::array<float, 2 * XAUDIO_NUM_SAMPLES> audioFrames;

        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            float ch0 = floatSamples[0 * XAUDIO_NUM_SAMPLES + i];
            float ch1 = floatSamples[1 * XAUDIO_NUM_SAMPLES + i];
            float ch2 = floatSamples[2 * XAUDIO_NUM_SAMPLES + i];
            float ch3 = floatSamples[3 * XAUDIO_NUM_SAMPLES + i];
            float ch4 = floatSamples[4 * XAUDIO_NUM_SAMPLES + i];
            float ch5 = floatSamples[5 * XAUDIO_NUM_SAMPLES + i];

            audioFrames[i * 2 + 0] = (ch0 + ch2 * 0.75f + ch4) * Config::MasterVolume;
            audioFrames[i * 2 + 1] = (ch1 + ch2 * 0.75f + ch5) * Config::MasterVolume;
        }

        SDL_QueueAudio(g_audioDevice, &audioFrames, sizeof(audioFrames));
    }
    else
    {
        std::array<float, XAUDIO_NUM_CHANNELS * XAUDIO_NUM_SAMPLES> audioFrames;

        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            for (size_t j = 0; j < XAUDIO_NUM_CHANNELS; j++)
                audioFrames[i * XAUDIO_NUM_CHANNELS + j] = floatSamples[j * XAUDIO_NUM_SAMPLES + i] * Config::MasterVolume;
        }

        SDL_QueueAudio(g_audioDevice, &audioFrames, sizeof(audioFrames));
    }
}
