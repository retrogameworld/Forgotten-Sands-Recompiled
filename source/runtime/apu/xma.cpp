#include <deque>
#include <condition_variable>
#include <map>
#include <array>
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/import_log.h>
#include <os/logger.h>
#include <atomic>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
// Cached "is this debug env var set" (getenv on every draw cost several % of the render thread).
#define ENV_FLAG(name) ([] { static const bool s_v = getenv(name) != nullptr; return s_v; }())
}

// =============================================================================================
// XMA audio decoding (Forgotten Sands: menu music and streamed sounds).
//
// The game drives XMA "hardware" contexts: 64-byte structures in physical memory (XMA_CONTEXT_DATA, layout
// as in Xenia) holding input buffer pointers / packet counts / valid bits and a small PCM output ring
// (256-byte blocks). It sets them up through the kernel XMA* helpers and also writes them directly (it maps
// them with MmMapIoSpace). A host thread plays the XMA decoder: for every enabled context it decodes the
// 2048-byte input packets with FFmpeg's XMA2 decoder and writes 16-bit big-endian PCM into the output ring.
// =============================================================================================

namespace
{
    // XMA_CONTEXT_DATA dwords (big-endian in guest memory); bit positions from the LSB.
    enum : uint32_t
    {
        DW0 = 0,  // in0 packet count 0-11, loop count 12-19, in0 valid 20, in1 valid 21, out block count 22-26, out write offset 27-31
        DW1 = 1,  // in1 packet count 0-11, ..., subframe decode count 20-23, sample rate 27-28, stereo 29, out valid 31
        DW2 = 2,  // input buffer read offset (bits) 0-25, error 26-31
        DW3 = 3,  // loop start 0-25
        DW4 = 4,  // loop end 0-25, packet metadata 26-30, current buffer 31
        DW_IN0 = 5, DW_IN1 = 6, DW_OUT = 7, DW_WORK = 8,
        DW9 = 9,  // out read offset 0-4
    };

    struct XmaContext
    {
        uint32_t address = 0;
        bool enabled = false;
        bool kicked = false; // started by the sound engine's kick (sound effects), not XMAEnableContext
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        int channels = 0, sampleRate = 0;
        std::vector<uint8_t> pending; // decoded PCM (16-bit BE, interleaved) not written to the ring yet
        uint64_t expected = 0, produced = 0; // samples per channel: promised by the packet headers / given out
        int primePackets = 0;
        uint32_t lastKickRead = 0, lastKickInput = 0; // sound engine kicks (sub_82D01F10 hook)
        std::chrono::steady_clock::time_point initAt{};    // diag FS_XMA_LATENCY: when the sound was set up
        bool firstOutLogged = true;
        int64_t leadSilent = 0;   // diag: decoded samples so far while the output is still silent (-1: sound seen)
        uint64_t packetsFed = 0;  // diag: packets decoded since the last reset
        std::vector<std::array<uint8_t, 2048>> hist; // the current sound's last input packets (step 35c)
        std::vector<uint64_t> histBase;              // frames that began before each of them
        uint64_t frames = 0;      // frames begun so far in the current sound
        uint64_t pos = 0;         // samples (per channel) of the current sound given out so far
        uint32_t lastWrite = 0;   // output ring write offset (blocks) this decoder stored last
        // Step 35c: the input read offset of the packet decoded last is only reported once its samples are in the
        // output ring (see WorkContext): the offset to store, the offset and input pointer it replaces.
        uint32_t deferredRead = 0, deferredFrom = 0, deferredInput = 0;
        // Step 35f: the sound engine has locked this context (it is reading / changing it) until its next kick.
        bool locked = false;
        std::chrono::steady_clock::time_point lockedAt{};
        // Step 35f: a one-shot sound (its data ends with the engine's empty end-of-data loop) whose input was
        // read to the end; the engine made the same input valid again (see the kick hook).
        bool oneShot = false, ended = false, replaying = false;
        uint64_t replayedSamples = 0;
        int soundPeak = 0; int16_t tail[512] = {}; uint32_t tailAt = 0; // diag: does the sound end quietly?
        int env[64] = {}; uint64_t envSamples = 0;                      // diag: peak of each 1024 samples of the sound (its shape)
        FILE* dumpPcm = nullptr;                                         // test FS_XMA_DUMP_DIR: the sound's decoded samples
        bool soloMute = true;                                            // test FS_XMA_SOLO: this sound is not one of the chosen ones
        uint64_t cutFrom = 0, cutTo = 0;                                 // Step 35o: decoded samples [cutFrom, cutTo) of this sound are left out
    };

    std::recursive_mutex g_xmaMutex;
    std::atomic<int> g_kicksRunning{0}; // Step 37: kicks writing context memory right now (WorkContext waits)
    // Step 37: the kernel XMA functions (called by other game threads) wait up to 10 ms for a running kick before
    // they take the lock - like the console, where these calls never overlapped a kick's context writes. (Not
    // longer: a caller may hold a game lock the kick needs; holding g_xmaMutex across the kick froze the game.)
    thread_local bool t_inKick = false;
    std::unique_lock<std::recursive_mutex> LockForImport()
    {
        if (t_inKick) // (the kick's own code calling in)
            return std::unique_lock<std::recursive_mutex>(g_xmaMutex);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        while (g_kicksRunning.load() > 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        return std::unique_lock<std::recursive_mutex>(g_xmaMutex);
    }
    std::unordered_map<uint32_t, XmaContext*> g_xmaContexts;
    std::atomic<bool> g_xmaThreadRunning{ false };
    std::atomic<int> g_xmaFreeze{ 0 }; // > 0: the sound engine is reading its contexts back (sub_82D01DA0)
    // FS_XMA_TRACE2=<seconds>: every decode / hand-back / kick of the first kicked context that starts a new sound
    // after that time (600 lines).
    std::atomic<uint32_t> g_trace2Ctx{ 0 };
    bool Trace2Time()
    {
        static const double s_from = [] { const char* e = getenv("FS_XMA_TRACE2"); return e ? atof(e) : -1.0; }();
        static const auto s_t0 = std::chrono::steady_clock::now();
        return s_from >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count() >= s_from;
    }
    bool Trace2(uint32_t context)
    {
        static std::atomic<int> s_n{0};
        return context != 0 && g_trace2Ctx.load() == context && s_n++ < 4000;
    }
    std::atomic<uint64_t> g_xmaPackets{ 0 };

    uint8_t* Phys(uint32_t address)
    {
        if (address == 0)
            return nullptr;
        if (address < 0x20000000)
            address |= 0xA0000000; // physical -> our 0xA0000000 view
        return reinterpret_cast<uint8_t*>(g_memory.Translate(address));
    }

    std::atomic<uint32_t>* Word(uint32_t context, uint32_t index)
    {
        return reinterpret_cast<std::atomic<uint32_t>*>(Phys(context + index * 4));
    }

    uint32_t Get(uint32_t context, uint32_t index)
    {
        return ByteSwap(Word(context, index)->load(std::memory_order_acquire));
    }

    // Read-modify-write of one dword without losing the game's concurrent writes to the other bits.
    void Update(uint32_t context, uint32_t index, uint32_t clearMask, uint32_t setBits)
    {
        auto* w = Word(context, index);
        uint32_t old = w->load(std::memory_order_acquire);
        for (;;)
        {
            const uint32_t v = (ByteSwap(old) & ~clearMask) | (setBits & clearMask);
            if (w->compare_exchange_weak(old, ByteSwap(v), std::memory_order_acq_rel))
                return;
        }
    }

    void SetField(uint32_t context, uint32_t index, uint32_t shift, uint32_t bits, uint32_t value)
    {
        const uint32_t mask = (bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1)) << shift;
        Update(context, index, mask, value << shift);
    }

    uint32_t Field(uint32_t context, uint32_t index, uint32_t shift, uint32_t bits)
    {
        return (Get(context, index) >> shift) & (bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1));
    }

    // Step 35c: every packet is decoded by a decoder that was never used before (see DecodePacket). Opening one
    // costs about 0.8 ms, so a background thread keeps a few opened ones ready per format and frees the used ones.
    struct CodecSet { AVCodecContext* codec = nullptr; AVPacket* packet = nullptr; AVFrame* frame = nullptr; };
    std::mutex g_poolMutex;
    std::condition_variable g_poolCv;
    std::map<std::pair<int, int>, std::vector<CodecSet>> g_pool; // (channels, sample rate) -> ready decoders
    std::vector<CodecSet> g_usedCodecs;
    constexpr size_t POOL_SIZE = 8;

    void FreeCodecSet(CodecSet& set)
    {
        if (set.codec) avcodec_free_context(&set.codec);
        if (set.packet) av_packet_free(&set.packet);
        if (set.frame) av_frame_free(&set.frame);
    }

    CodecSet OpenCodecSet(int channels, int sampleRate)
    {
        CodecSet set;
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec)
        {
            static std::atomic<bool> s_logged{false};
            if (!s_logged.exchange(true)) LOGFN_ERROR("[xma] FFmpeg has no XMA2 decoder");
            return set;
        }
        set.codec = avcodec_alloc_context3(codec);
        av_channel_layout_default(&set.codec->ch_layout, channels);
        set.codec->sample_rate = sampleRate;
        set.codec->block_align = 2048; // XMA packet size (the decoder refuses to open without it)
        // Keep FFmpeg from trimming the stream's leading samples: the game counts every frame in the packet headers
        // (512 samples each) and stops feeding a voice that comes back short.
        set.codec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        // XMA2WAVEFORMATEX-sized extradata: FFmpeg takes one stream of `channels` channels from it.
        set.codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        set.codec->extradata_size = 34;
        set.codec->extradata[0] = 1;                                    // NumStreams = 1 (LE16)
        set.codec->extradata[2] = uint8_t(channels == 2 ? 0x03 : 0x04); // ChannelMask (LE32): FL|FR or FC
        set.codec->extradata[10] = 0x00; set.codec->extradata[11] = 0x08; // BytesPerBlock = 2048 (LE32 at +10)
        if (int err = avcodec_open2(set.codec, codec, nullptr); err < 0)
        {
            char msg[128] = {}; av_strerror(err, msg, sizeof(msg));
            LOGFN_ERROR("[xma] could not open the XMA2 decoder ({} ch, {} Hz): {}", channels, sampleRate, msg);
            FreeCodecSet(set);
            return set;
        }
        set.packet = av_packet_alloc();
        set.frame = av_frame_alloc();
        return set;
    }

    void PoolThread()
    {
        for (;;)
        {
            std::vector<CodecSet> used;
            std::pair<int, int> want{ 0, 0 };
            {
                std::unique_lock lock(g_poolMutex);
                g_poolCv.wait_for(lock, std::chrono::milliseconds(50), []
                {
                    if (!g_usedCodecs.empty()) return true;
                    for (auto& [key, ready] : g_pool) if (ready.size() < POOL_SIZE) return true;
                    return false;
                });
                used.swap(g_usedCodecs);
                for (auto& [key, ready] : g_pool)
                    if (ready.size() < POOL_SIZE) { want = key; break; }
            }
            for (CodecSet& set : used)
                FreeCodecSet(set);
            if (want.first != 0)
            {
                CodecSet set = OpenCodecSet(want.first, want.second);
                if (set.codec)
                {
                    std::lock_guard lock(g_poolMutex);
                    g_pool[want].push_back(set);
                }
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    // Hand the context's decoder (if any) to the background thread to be freed.
    void FreeCodec(XmaContext& c)
    {
        if (c.codec || c.packet || c.frame)
        {
            std::lock_guard lock(g_poolMutex);
            g_usedCodecs.push_back(CodecSet{ c.codec, c.packet, c.frame });
            g_poolCv.notify_one();
        }
        c.codec = nullptr; c.packet = nullptr; c.frame = nullptr;
        c.channels = c.sampleRate = 0;
    }

    // Give the context a decoder that was never used.
    bool TakeCodec(XmaContext& c, int channels, int sampleRate)
    {
        FreeCodec(c);
        CodecSet set;
        {
            std::lock_guard lock(g_poolMutex);
            auto& ready = g_pool[{ channels, sampleRate }]; // also tells the pool thread this format is in use
            if (!ready.empty()) { set = ready.back(); ready.pop_back(); }
            g_poolCv.notify_one();
        }
        if (!set.codec)
        {
            static std::atomic<int> s_inline{0};
            if (s_inline++ < 10 || (s_inline % 200) == 0)
                LOGFN("[xma] no ready decoder for {} ch {} Hz, opening one now ({} times so far)", channels, sampleRate, s_inline.load());
            set = OpenCodecSet(channels, sampleRate);
        }
        if (!set.codec)
            return false;
        c.codec = set.codec; c.packet = set.packet; c.frame = set.frame;
        c.channels = channels;
        c.sampleRate = sampleRate;
        return true;
    }

    // The context's format is known (and a decoder can be had for it).
    bool EnsureCodec(XmaContext& c, int channels, int sampleRate)
    {
        if (c.codec && c.channels == channels && c.sampleRate == sampleRate)
            return true;
        return TakeCodec(c, channels, sampleRate);
    }

    // Decoded samples of the frame just received, as interleaved 16-bit.
    void AppendFrame(XmaContext& c, std::vector<int16_t>& out)
    {
        const int n = c.frame->nb_samples, ch = c.channels;
        const size_t start = out.size();
        out.resize(start + size_t(n) * ch);
        int16_t* o = out.data() + start;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++)
            {
                const float* plane = reinterpret_cast<const float*>(c.frame->extended_data[std::min(k, c.frame->ch_layout.nb_channels - 1)]);
                *o++ = int16_t(std::lrint(std::clamp(plane[i], -1.0f, 1.0f) * 32767.0f));
            }
    }

    // Decode the history packets [from, end) with a fresh decoder and flush it: every sample of every complete
    // frame in them, then FFmpeg's 512-sample end-of-stream tail. The first frame only primes the decoder
    // (FFmpeg gives nothing for it), so out[0] is the first sample of the second frame of packet `from`.
    // (A decoder reset with avcodec_flush_buffers is not the same as a new one: FFmpeg keeps the sound's trim
    // values and cuts samples off the end - measured with FS_XMA_SELFCHECK. So always a new one.)
    bool DecodeRun(XmaContext& c, size_t from, std::vector<int16_t>& out)
    {
        const int channels = c.channels, rate = c.sampleRate;
        if (!TakeCodec(c, channels, rate))
            return false;
        auto drain = [&] { while (avcodec_receive_frame(c.codec, c.frame) == 0) AppendFrame(c, out); };
        for (size_t i = from; i < c.hist.size(); i++)
        {
            if (av_new_packet(c.packet, 2048) < 0)
                return false;
            memcpy(c.packet->data, c.hist[i].data(), 2048);
            if (avcodec_send_packet(c.codec, c.packet) == 0)
                drain();
            av_packet_unref(c.packet);
        }
        avcodec_send_packet(c.codec, nullptr);
        drain();
        return true;
    }

    // Step 35o (the tester: "there's just one vault that isn't [synced] where he jumps real high and hits them on their
    // back" ... "its in the original (a bug)" ... "you couldnt fix the vault sound?"): that move plays ONE sound for
    // the whole attack (14 packets, 3.3 s at 24 kHz, started when the attack is pressed): the jump's whoosh, 0.34 s
    // of near silence (0.73 - 1.07 s), then the heavy hit from 1.15 s. The blade lands 1.05 s after the press, and
    // the sound reaches the speakers 0.12 - 0.19 s after it is started, so the hit was heard about 0.25 s after the
    // blow (in step with the sparks that follow it - probably what the game's makers timed it to; the tester hears it as
    // late). For this one sound 0.23 s of the silent stretch are left out. Measured after that (scratch
    // solo_vault.ps1: FS_XMA_SOLO=14 + frames): the hit is heard between 0.07 s before and 0.2 s after the landing,
    // depending on the run (the landing comes 0.92 - 1.05 s after the press, the start latency varies).
    // A sound is recognised by its packet count and the FNV-1a hash of its first packet. Times are in decoded
    // samples of the sound (512 samples of encoder lead-in come first). FS_NO_SOUND_CUTS=1: off.
    struct SoundCut { uint32_t packets; uint64_t firstPacketHash; uint32_t from, to; const char* what; };
    const SoundCut kSoundCuts[] = {
        { 14, 0xBB3893F0D9C5A786ull, 512 + 18960, 512 + 24480, "high-jump vault attack" },
    };
    uint64_t Fnv1a(const uint8_t* p, size_t n)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        return h;
    }
    const SoundCut* FindSoundCut(uint32_t packets, const uint8_t* input)
    {
        static const bool s_off = getenv("FS_NO_SOUND_CUTS") != nullptr;
        if (s_off || input == nullptr)
            return nullptr;
        for (auto& k : kSoundCuts)
            if (k.packets == packets && Fnv1a(input, 2048) == k.firstPacketHash)
                return &k;
        return nullptr;
    }

    // A new sound starts on this context: nothing of the previous one may be heard any more.
    void ResetSound(XmaContext& c)
    {
        c.pending.clear();
        c.hist.clear();
        c.histBase.clear();
        c.frames = c.pos = 0;
        c.expected = c.produced = 0;
        c.leadSilent = 0; c.packetsFed = 0;
        c.deferredRead = 0;
        c.oneShot = c.ended = c.replaying = false;
        c.replayedSamples = 0;
        c.soundPeak = 0; c.tailAt = 0;
        memset(c.env, 0, sizeof(c.env)); c.envSamples = 0;
        c.cutFrom = c.cutTo = 0;
        c.initAt = std::chrono::steady_clock::now();
    }

    // Step 35c: one more input packet. The real decoder gives out a packet's samples as soon as it has the
    // packet. FFmpeg's XMA2 decoder keeps the last 4096 samples (and the first frame) until more packets come or
    // the stream is flushed, so every short sound lost its last 4608 samples (192 ms at 24 kHz), and they were
    // heard in front of the NEXT sound of that voice, which started that much late (the tester: "the swooshing of the
    // prince is pretty late", "u need to sort the syncing problem generally"). Now every packet is decoded
    // together with the packets before it by a fresh decoder that is flushed at once, and the samples not given
    // out yet are taken from that by their position in the sound.
    void DecodePacket(XmaContext& c, const uint8_t* data)
    {
        constexpr size_t HISTORY = 8;
        constexpr uint64_t FRAME = 512;
        c.hist.emplace_back();
        memcpy(c.hist.back().data(), data, 2048);
        c.histBase.push_back(c.frames);
        if (c.hist.size() > HISTORY) { c.hist.erase(c.hist.begin()); c.histBase.erase(c.histBase.begin()); }
        c.frames += uint64_t(data[0] >> 2);
        c.expected = c.frames * FRAME;
        c.packetsFed++;
        g_xmaPackets++;

        // Start at the latest packet whose second frame is not past what was given out already.
        size_t from = 0;
        for (size_t i = c.hist.size(); i-- > 0;)
            if (c.histBase[i] == 0 || (c.histBase[i] + 1) * FRAME <= c.pos) { from = i; break; }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<int16_t> out;
        if (!DecodeRun(c, from, out))
            return;
        const int ch = c.channels;
        const uint64_t total = out.size() / ch;
        const uint64_t coverStart = (c.histBase[from] + 1) * FRAME;
        const uint64_t coverEnd = std::min(coverStart + (total > FRAME ? total - FRAME : 0), c.expected);

        static const bool s_check = getenv("FS_XMA_SELFCHECK") != nullptr;
        if (s_check && from > 0 && coverEnd > c.pos)
        {
            // The same samples decoded from one packet earlier must come out identical.
            std::vector<int16_t> ref;
            if (DecodeRun(c, from - 1, ref))
            {
                const uint64_t refStart = (c.histBase[from - 1] + 1) * FRAME;
                int worst = 0;
                for (uint64_t q = c.pos; q < coverEnd; q++)
                    for (int k = 0; k < ch; k++)
                    {
                        const size_t x = size_t(q - coverStart) * ch + k, y = size_t(q - refStart) * ch + k;
                        if (y < ref.size()) worst = std::max(worst, std::abs(int(out[x]) - int(ref[y])));
                    }
                static std::atomic<int> s_n{0};
                if (worst > 2 || s_n++ < 40)
                    LOGFN("[xma] selfcheck 0x{:08X}: samples {}..{} decoded from packet {} and from the one before differ by at most {}", c.address, c.pos, coverEnd, from, worst);
            }
        }

        if (coverEnd > c.pos)
        {
            if (c.pos < coverStart)
            {
                // The sound's first frame (the encoder's lead-in), or a hole the history no longer covers.
                static std::atomic<int> s_holes{0};
                if (c.histBase[from] != 0 && s_holes++ < 20)
                    LOGFN("[xma] context 0x{:08X}: {} samples not covered by the packet history (silence)", c.address, coverStart - c.pos);
                c.pending.insert(c.pending.end(), size_t(coverStart - c.pos) * ch * 2, 0);
                if (c.leadSilent >= 0) c.leadSilent += int64_t(coverStart - c.pos);
                c.pos = coverStart;
            }
            const size_t first = size_t(c.pos - coverStart) * ch, last = size_t(coverEnd - coverStart) * ch;
            const size_t start = c.pending.size();
            c.pending.resize(start + (last - first) * 2);
            uint8_t* o = c.pending.data() + start;
            static const bool s_markReplay = getenv("FS_XMA_MARK_REPLAY") != nullptr; // test: a 1 kHz tone instead
            for (size_t i = first; i < last; i++)
            {
                int16_t v = out[i];
                if (c.cutTo != 0)
                {
                    // Step 35o: see SoundCuts. 64 samples of fade on both sides of the cut (the stretch is nearly
                    // silent, this only keeps the join from clicking).
                    const uint64_t q = coverStart + i / ch;
                    if (q >= c.cutFrom && q < c.cutTo)
                        continue;
                    if (q + 64 >= c.cutFrom && q < c.cutFrom) v = int16_t(int(v) * int(c.cutFrom - q) / 64);
                    else if (q >= c.cutTo && q < c.cutTo + 64) v = int16_t(int(v) * int(q - c.cutTo) / 64);
                }
                if (c.soloMute && ENV_FLAG("FS_XMA_SOLO")) v = 0;
                if (c.replaying)
                {
                    // Step 35f: a one-shot sound whose data has all been given out. The sound engine makes the
                    // same input valid again and keeps reading the output until the game stops the voice
                    // (20-220 ms later): decoded as it is, the beginning of the sound was heard again at its end
                    // (the tester: breaking urns / barrels "crackles or repeats itself"; measured with the test tone:
                    // 75 bursts, 6.9 s in a 74 s run). The input is still read and handed back as before, but
                    // what comes out is silence. FS_XMA_PLAY_REPLAY=1: old behaviour.
                    static const bool s_playReplay = getenv("FS_XMA_PLAY_REPLAY") != nullptr;
                    c.replayedSamples++;
                    if (s_markReplay)
                        v = int16_t(12000.0 * std::sin(6.283185307 * 1000.0 * double(c.replayedSamples / ch) / double(std::max(1, c.sampleRate))));
                    else if (!s_playReplay)
                        v = 0;
                }
                else
                {
                    c.soundPeak = std::max(c.soundPeak, std::abs(int(v)));
                    c.tail[c.tailAt++ & 511] = v;
                    { const uint64_t k = (c.envSamples++ / ch) / 1024; if (k < 64) c.env[k] = std::max(c.env[k], std::abs(int(v))); }
                    if (c.dumpPcm) fwrite(&v, 2, 1, c.dumpPcm);
                }
                if (c.leadSilent >= 0 && (i % ch) == 0)
                {
                    if (std::abs(int(v)) > 100)
                    {
                        static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
                        static std::atomic<int> s_n{0};
                        if (s_lat && s_n++ < 3000)
                            LOGFN("[xma] context 0x{:08X}: first audible sample after {} silent decoded samples ({} packets fed, {} Hz, kicked {}, {:.1f} ms after start)", c.address, c.leadSilent, c.packetsFed, c.sampleRate, c.kicked,
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c.initAt).count());
                        c.leadSilent = -1;
                    }
                    else
                        c.leadSilent++;
                }
                static std::atomic<int> s_peak{0}; if (std::abs(int(v)) > s_peak) s_peak = std::abs(int(v));
                static std::atomic<uint64_t> s_samples{0}; if ((++s_samples % 480000) == 0) { LOGFN("[xma] decoded {} samples, peak {}", s_samples.load(), s_peak.load()); s_peak = 0; }
                *o++ = uint8_t(uint16_t(v) >> 8);
                *o++ = uint8_t(uint16_t(v) & 0xFF);
            }
            c.pending.resize(size_t(o - c.pending.data())); // (shorter when a cut left samples out)
            c.pos = coverEnd;
        }
        c.produced = c.pos;

        // diag: decoding cost and what each packet gave
        static std::atomic<uint64_t> s_us{0}; static std::atomic<int> s_packets{0};
        s_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        if ((++s_packets % 500) == 0) { LOGFN("[xma] {} packets decoded, {:.0f} us each on average", s_packets.load(), double(s_us.load()) / 500.0); s_us = 0; }
        static std::atomic<int> s_logged{0};
        if (ENV_FLAG("FS_XMA_TRACE") && s_logged++ < 200)
            LOGFN("[xma] packet 0x{:08X} hdr {:02X}{:02X}{:02X}{:02X}: {} frames begin here, decoded from history packet {} of {}, {} samples, given out {} of {} promised", c.address, data[0], data[1], data[2], data[3], data[0] >> 2, from, c.hist.size(), total, c.pos, c.expected);
    }

    void WorkContext(XmaContext& c)
    {
        // Step 37: hands off every context while the sound engine's kick writes context memory (see sub_82D01F10).
        if (g_kicksRunning.load() > 0)
            return;
        // Step 35f: hands off while the sound engine has the context locked (see the sub_82D01DA0 hook).
        if (c.kicked && g_xmaFreeze.load(std::memory_order_relaxed) > 0)
            return;
        if (c.locked)
        {
            if (std::chrono::steady_clock::now() - c.lockedAt < std::chrono::milliseconds(100))
                return;
            static std::atomic<int> s_n{0};
            if (s_n++ < 20)
                LOGFN("[xma] context 0x{:08X} was locked for 100 ms without a kick - released", c.address);
            c.locked = false;
        }
        const uint32_t ctx = c.address;
        static int s_trace = 0;
        static auto s_last = std::chrono::steady_clock::now();
        if (ENV_FLAG("FS_XMA_TRACE") && std::chrono::steady_clock::now() - s_last > std::chrono::milliseconds(250) && s_trace < 400) { s_last = std::chrono::steady_clock::now(); s_trace++; LOGFN("[xma] work 0x{:08X}: dw0 {:08X} dw1 {:08X} dw2 {:08X} dw4 {:08X} in0 {:08X} out {:08X} dw9 {:08X}", ctx, Get(ctx, DW0), Get(ctx, DW1), Get(ctx, DW2), Get(ctx, DW4), Get(ctx, DW_IN0), Get(ctx, DW_OUT), Get(ctx, DW9)); }
        if (Field(ctx, DW1, 31, 1) == 0) // output buffer not valid
            return;
        uint8_t* out = Phys(Get(ctx, DW_OUT));
        const uint32_t blocks = Field(ctx, DW0, 22, 5);
        if (out == nullptr || blocks == 0)
            return;
        static const int kRates[4] = { 24000, 32000, 44100, 48000 };
        const int channels = Field(ctx, DW1, 29, 1) ? 2 : 1;
        if (!EnsureCodec(c, channels, kRates[Field(ctx, DW1, 27, 2)]))
            return;

        // The sound engine starts a new sound on a voice by resetting the output ring (write offset 0) and the
        // input read offset. Whatever is left of the previous sound must not be played in front of the new one.
        if (Field(ctx, DW0, 27, 5) == 0 && c.lastWrite != 0 && Field(ctx, DW2, 0, 26) <= 32 && (c.pos != 0 || !c.pending.empty()))
        {
            static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
            static std::atomic<int> s_n{0};
            if (s_lat && s_n++ < 400)
                LOGFN("[xma] context 0x{:08X}: output ring was reset, new sound ({} bytes of the previous one dropped)", ctx, c.pending.size());
            ResetSound(c);
        }
        const uint32_t capacity = blocks * 256;
        uint32_t write = Field(ctx, DW0, 27, 5) * 256 % capacity;
        const uint32_t read = Field(ctx, DW9, 0, 5) * 256 % capacity;
        if (Trace2(ctx))
        {
            static uint32_t s_w = ~0u, s_r = ~0u;
            if (s_w != write || s_r != read) { s_w = write; s_r = read; LOGFN("[xt] 0x{:08X} ring: {} blocks, write {} read {}, pending {} bytes, pos {}", ctx, blocks, write / 256, read / 256, c.pending.size(), c.pos); }
        }
        // Free blocks in the ring (one block kept empty so that read == write means "empty").
        auto freeBytes = [&] { return (read + capacity - write - 256) % capacity; };

        for (int guard = 0; guard < 64 && freeBytes() >= 256; guard++)
        {
            if (c.pending.size() >= 256)
            {
                if (!c.firstOutLogged)
                {
                    c.firstOutLogged = true;
                    static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
                    static std::atomic<int> s_n{0};
                    if (s_lat && s_n++ < 2000)
                        LOGFN("[xma] context 0x{:08X}: first PCM {:.1f} ms after {} (rate {}, packets in0 {} in1 {}, pending {})", c.address,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c.initAt).count(), c.kicked ? "kick" : "init", c.sampleRate,
                            Field(c.address, DW0, 0, 12), Field(c.address, DW1, 0, 12), c.pending.size());
                }
                memcpy(out + write, c.pending.data(), 256);
                c.pending.erase(c.pending.begin(), c.pending.begin() + 256);
                write = (write + 256) % capacity;
                continue;
            }

            // Everything decoded so far is in the ring now: report the last packet as read. (A whole packet is
            // decoded at once - up to 0.3 s of sound - while the real decoder only runs as far as the ring has
            // room. The sound engine ends a sound when its input is read to the end, so with the offset stored
            // right after decoding, the last packet of every short sound was cut off: FS_XMA_READ_EARLY=1.)
            if (c.deferredRead != 0)
            {
                const uint32_t cur = Field(ctx, DW4, 31, 1);
                if (Field(ctx, DW2, 0, 26) == c.deferredFrom && Get(ctx, cur ? DW_IN1 : DW_IN0) == c.deferredInput)
                    SetField(ctx, DW2, 0, 26, c.deferredRead);
                c.deferredRead = 0;
            }
            // Need more PCM: decode the next packet of the current input buffer.
            const uint32_t current = Field(ctx, DW4, 31, 1);
            const bool valid = Field(ctx, DW0, current ? 21 : 20, 1) != 0;
            if (!valid)
            {
                // The game may refill only the other buffer: continue there (as the hardware/Xenia do).
                if (Field(ctx, DW0, current ? 20 : 21, 1) == 0)
                    break;
                SetField(ctx, DW4, 31, 1, current ^ 1);
                SetField(ctx, DW2, 0, 26, 32);
                continue;
            }
            uint8_t* in = Phys(Get(ctx, current ? DW_IN1 : DW_IN0));
            const uint32_t packets = current ? Field(ctx, DW1, 0, 12) : Field(ctx, DW0, 0, 12);
            const uint32_t readBits = Field(ctx, DW2, 0, 26);
            const uint32_t packet = (readBits >> 3) / 2048;
            if (in == nullptr || packet >= packets)
            {
                // This input buffer is used up: hand it back and continue with the other one.
                if (Trace2(ctx)) LOGFN("[xt] 0x{:08X} hand back buffer {} (packets {}, read {}), pending {}", ctx, current, packets, readBits, c.pending.size());
                // Step 35f (the tester: "when you destroy barrels/urns the sound either crackles or repeats itself"):
                // the sound engine marks the end of every sound's data with an empty loop (loop start = loop end
                // = the last data bit, loop count 1). The real decoder counts the loop down when it passes that
                // point; a buffer handed back with the count still at 1 means "not through yet" to the engine,
                // which then makes the same buffer valid again from the top - the sound started over (heard as a
                // repeat, or as a torn piece when the voice was stopped soon after). FS_XMA_NO_LOOP_COUNT=1: old.
                {
                    // Step 35h: the count-down is OFF (FS_XMA_LOOP_COUNT=1 turns it on). It crashed the tester's game at a
                    // level change: the engine compares the context's loop count with its own (voice +202) in
                    // sub_82D06188 and, for every loop "done", fires the loop-end callback through the voice's
                    // current buffer (voice +328) - a null pointer once the buffer was flushed. The engine
                    // re-validates the input whatever the count is, so nothing is gained by counting.
                    static const bool s_noLoop = getenv("FS_XMA_LOOP_COUNT") == nullptr;
                    const uint32_t loops = Field(ctx, DW0, 12, 8), loopEnd = Field(ctx, DW4, 0, 26);
                    if (current == 0 && c.kicked && Field(ctx, DW3, 0, 26) == loopEnd && loopEnd > 32 && loopEnd <= packets * 2048 * 8)
                    {
                        c.oneShot = true;
                        c.ended = true;
                        static const bool s_lat2 = getenv("FS_XMA_LATENCY") != nullptr;
                        static std::atomic<int> s_k2{0};
                        if (s_lat2 && s_k2++ < 400)
                        {
                            int tailPeak = 0, lastPeak = 0;
                            const uint32_t have = std::min<uint32_t>(c.tailAt, 512);
                            for (uint32_t q = 0; q < have; q++) tailPeak = std::max(tailPeak, std::abs(int(c.tail[q])));
                            for (uint32_t q = 1; q <= std::min<uint32_t>(have, 32); q++) lastPeak = std::max(lastPeak, std::abs(int(c.tail[(c.tailAt - q) & 511])));
                            LOGFN("[xma] context 0x{:08X}: sound ended after {} samples ({} Hz): peak {}, last 512 samples peak {}, last 32 samples peak {}", ctx, c.pos, c.sampleRate, c.soundPeak, tailPeak, lastPeak);
                            { std::string e; for (int k = 0; k < 64 && uint64_t(k) * 1024 < c.pos; k++) e += fmt::format(" {}", c.env[k] / 328); LOGFN("[xma] context 0x{:08X}: shape (peak per 1024 samples, 0-99):{}", ctx, e); }
                        }
                    }
                    if (!s_noLoop && current == 0 && c.oneShot && loops > 0 && loops < 255)
                    {
                        SetField(ctx, DW0, 12, 8, loops - 1);
                        static std::atomic<int> s_k{0};
                        static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
                        if (s_lat && s_k++ < 200)
                            LOGFN("[xma] context 0x{:08X}: end of data passed (loop {}..{}), loop count {} -> {}", ctx, Field(ctx, DW3, 0, 26), loopEnd, loops, loops - 1);
                    }
                }
                SetField(ctx, DW0, current ? 21 : 20, 1, 0);
                SetField(ctx, DW4, 31, 1, current ^ 1);
                SetField(ctx, DW2, 0, 26, 32);
                continue;
            }
            if (Trace2(ctx)) LOGFN("[xt] 0x{:08X} decode packet {} of {} (buffer {}, in 0x{:08X}, read {}), pos {}", ctx, packet, packets, current, Get(ctx, current ? DW_IN1 : DW_IN0), readBits, c.pos);
            DecodePacket(c, in + packet * 2048);
            static const bool s_readEarly = getenv("FS_XMA_READ_EARLY") != nullptr;
            if (s_readEarly || c.pending.size() < 256)
                SetField(ctx, DW2, 0, 26, (packet + 1) * 2048 * 8 + 32);
            else
            {
                c.deferredRead = (packet + 1) * 2048 * 8 + 32;
                c.deferredFrom = readBits;
                c.deferredInput = Get(ctx, current ? DW_IN1 : DW_IN0);
            }
        }
        SetField(ctx, DW0, 27, 5, write / 256);
        c.lastWrite = write / 256;
        // Like the hardware (and Xenia): a full output ring is handed back by clearing its valid bit; the game
        // consumes it and sets it valid again.
        // (Taken out in Step 14 without a note. Test: FS_XMA_OUT_INVALIDATE=1 does it again.)
        if (ENV_FLAG("FS_XMA_OUT_INVALIDATE") && freeBytes() < 256)
            SetField(ctx, DW1, 31, 1, 0);

    }

    void DecoderThread()
    {
        while (g_xmaThreadRunning)
        {
            {
                std::lock_guard lock(g_xmaMutex);
                for (auto& [address, c] : g_xmaContexts)
                    if (c->enabled)
                        WorkContext(*c);
            }
            static auto s_dumpAt = std::chrono::steady_clock::now();
            if (ENV_FLAG("FS_XMA_TRACE") && std::chrono::steady_clock::now() - s_dumpAt > std::chrono::seconds(1))
            {
                s_dumpAt = std::chrono::steady_clock::now();
                std::lock_guard lock(g_xmaMutex);
                std::string s;
                for (auto& [address, c] : g_xmaContexts) s += fmt::format(" 0x{:08X}{}[{:08X} {:08X} {:08X} {:08X} r{:X} p{}]", address, c->enabled ? "(on)" : "", Get(address, 0), Get(address, 1), Get(address, 2), Get(address, 4), Get(address, 9), c->pending.size());
                LOGFN("[xma] known contexts:{}", s);
                for (uint32_t a : { 0xAAD40300u, 0xAAD40500u, 0xAAD40C00u })
                {
                    std::string w; for (int i = 0; i < 10; i++) w += fmt::format(" {:08X}", Get(a, i));
                    LOGFN("[xma] mapped context 0x{:08X}:{}", a, w);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void EnsureThread()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            av_log_set_level(AV_LOG_QUIET);
            if (ENV_FLAG("FS_XMA_TRACE"))
            {
                av_log_set_level(AV_LOG_VERBOSE);
                av_log_set_callback([](void* avcl, int level, const char* fmt, va_list vl)
                {
                    if (level > AV_LOG_VERBOSE) return;
                    static std::atomic<int> s_n{0};
                    if (s_n++ > 200) return;
                    char line[512]; vsnprintf(line, sizeof(line), fmt, vl);
                    LOGFN("[ffmpeg] {}", line);
                });
            }
            g_xmaThreadRunning = true;
            std::thread(DecoderThread).detach();
            std::thread(PoolThread).detach();
            LOGFN("[xma] decoder thread started (FFmpeg XMA2)");
        });
    }

    XmaContext* Lookup(uint32_t context)
    {
        auto it = g_xmaContexts.find(context);
        if (it != g_xmaContexts.end())
            return it->second;
        // A context the game set up in memory we did not allocate: track it too.
        auto* c = new XmaContext();
        c->address = context;
        g_xmaContexts[context] = c;
        return c;
    }
}

// ---------------------------------------------------------------------------------------------
// Kernel XMA helpers (same semantics as Xenia's xboxkrnl_audio_xma.cc)

// Step 37 (the rare crash / freeze, PC and Odin): on the console XMA contexts live in the audio hardware's own
// context array, so a released context's memory never becomes anything else. Here it went back to the general
// heap and was handed out again at once - but the sound engine's kick (sub_82D01F10) copies 48 bytes of a voice's
// context state into the context pointer it read from the voice, and the voice teardown (sub_82D01688) releases
// that context without the kick's lock. A kick that raced a teardown then wrote over whatever object had just
// been given that memory (seen as critical sections in the game's data holding "voice + 0x01000000", and as
// voice objects with a vtable of 1). Contexts now come from a pool of their own, handed out round-robin, so a
// late write can only land in an unused (or another) XMA context, like on the console. FS_XMA_HEAP_CONTEXTS=1: old.
struct XmaContextPool
{
    static constexpr uint32_t SLOTS = 1024, STRIDE = 0x100;
    std::mutex m;
    uint32_t base = 0;
    std::deque<uint32_t> free;
};
static XmaContextPool g_xmaPool;
static bool XmaHeapContexts() { static const bool s_heap = getenv("FS_XMA_HEAP_CONTEXTS") != nullptr; return s_heap; }

static uint32_t AllocContextMemory()
{
    if (!XmaHeapContexts())
    {
        std::lock_guard l(g_xmaPool.m);
        if (g_xmaPool.base == 0)
        {
            if (void* pool = g_userHeap.AllocPhysical(XmaContextPool::SLOTS * XmaContextPool::STRIDE, 0x1000))
            {
                memset(pool, 0, XmaContextPool::SLOTS * XmaContextPool::STRIDE);
                g_xmaPool.base = g_memory.MapVirtual(pool);
                for (uint32_t i = 0; i < XmaContextPool::SLOTS; i++) g_xmaPool.free.push_back(g_xmaPool.base + i * XmaContextPool::STRIDE);
                LOGFN("[xma] context pool at 0x{:08X} ({} contexts)", g_xmaPool.base, XmaContextPool::SLOTS);
            }
        }
        if (!g_xmaPool.free.empty())
        {
            const uint32_t a = g_xmaPool.free.front();
            g_xmaPool.free.pop_front();
            return a;
        }
        LOGFN_ERROR("[xma] context pool exhausted, using the heap");
    }
    void* mem = g_userHeap.AllocPhysical(0x40, 0x100);
    return mem ? g_memory.MapVirtual(mem) : 0;
}

static void FreeContextMemory(uint32_t address)
{
    {
        std::lock_guard l(g_xmaPool.m);
        if (g_xmaPool.base != 0 && address >= g_xmaPool.base && address < g_xmaPool.base + XmaContextPool::SLOTS * XmaContextPool::STRIDE)
        {
            g_xmaPool.free.push_back(address); // back of the queue: reused as late as possible
            return;
        }
    }
    g_userHeap.Free(g_memory.Translate(address));
}

uint32_t XMACreateContext(be<uint32_t>* context)
{
    EnsureThread();
    const uint32_t address = AllocContextMemory();
    memset(g_memory.Translate(address), 0, 0x40);
    *context = address;
    auto lock = LockForImport();
    Lookup(address);
    return 0;
}

uint32_t XMAReleaseContext(uint32_t context)
{
    {
        auto lock = LockForImport();
        auto it = g_xmaContexts.find(context);
        if (it != g_xmaContexts.end())
        {
            FreeCodec(*it->second);
            delete it->second;
            g_xmaContexts.erase(it);
        }
    }
    // Step 37 (Odin: crashes in the sound engine's voice code right after a kick, the kicked voice in r3): the
    // context memory went back to the heap at once and was handed out again (voice objects) while the sound
    // engine still read the released context through a voice. On the console that window is too short to matter;
    // on the phone it was not. Released context memory is now freed 2 s later. FS_XMA_FREE_NOW=1: at once.
    if (context)
    {
        static const bool s_now = getenv("FS_XMA_FREE_LATER") == nullptr; // Step 37: OFF by default - measured: with it on, 5 of 8 PC runs crashed; off (as before), 0 of 20. Opt-in for tests.
        static std::mutex s_m;
        static std::deque<std::pair<uint32_t, std::chrono::steady_clock::time_point>> s_later;
        if (s_now)
        {
            FreeContextMemory(context);
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        std::vector<uint32_t> due;
        {
            std::lock_guard l(s_m);
            s_later.emplace_back(context, now);
            while (!s_later.empty() && now - s_later.front().second > std::chrono::seconds(2))
            {
                due.push_back(s_later.front().first);
                s_later.pop_front();
            }
        }
        for (uint32_t a : due)
        {
            std::lock_guard lock(g_xmaMutex);
            if (g_xmaContexts.find(a) == g_xmaContexts.end()) // (not created again at the same address meanwhile)
                FreeContextMemory(a);
        }
    }
    return 0;
}

struct XmaContextInit
{
    be<uint32_t> input0, input0Packets, input1, input1Packets, inputReadOffset;
    be<uint32_t> output, outputBlocks, workBuffer, subframeDecodeCount, channelCount, sampleRate;
    be<uint32_t> loopStart, loopEnd;
    uint8_t loopCount, loopSubframeEnd, loopSubframeSkip, pad;
};

uint32_t XMAInitializeContext(uint32_t context, XmaContextInit* init)
{
    EnsureThread();
    auto lock = LockForImport();
    XmaContext* c = Lookup(context);
    c->firstOutLogged = false;
    {
        static const bool s_chain = getenv("FS_XMA_LATENCY") != nullptr;
        static std::atomic<int> s_k{0};
        if (s_chain && s_k++ < 400)
        {
            extern std::string CurrentGuestCallChain(int maxFrames);
            LOGFN("[xma] init 0x{:08X} on r13 0x{:08X} |{}", context, GetPPCContext() ? GetPPCContext()->r13.u32 : 0, CurrentGuestCallChain(28));
        }
    }
    // A new sound on this context.
    ResetSound(*c);
    c->lastWrite = 0;
    auto w = reinterpret_cast<be<uint32_t>*>(Phys(context));
    for (int i = 0; i < 16; i++)
        w[i] = 0;
    const uint32_t in0Packets = init->input0Packets, in1Packets = init->input1Packets, outBlocks = init->outputBlocks, sub = init->subframeDecodeCount;
    const uint32_t rate = init->sampleRate, chans = init->channelCount, readOff = init->inputReadOffset, loopStart = init->loopStart, loopEnd = init->loopEnd;
    w[DW0] = (in0Packets & 0xFFF) | (uint32_t(init->loopCount) << 12) | ((outBlocks & 0x1F) << 22);
    w[DW1] = (in1Packets & 0xFFF) | ((uint32_t(init->loopSubframeEnd) & 7) << 14) | ((uint32_t(init->loopSubframeSkip) & 7) << 17) |
        ((sub & 0xF) << 20) | ((rate & 3) << 27) | ((chans >= 1 ? 1u : 0u) << 29);
    w[DW2] = readOff & 0x3FFFFFF;
    w[DW3] = loopStart & 0x3FFFFFF;
    w[DW4] = loopEnd & 0x3FFFFFF;
    w[DW_IN0] = init->input0;
    w[DW_IN1] = init->input1;
    w[DW_OUT] = init->output;
    w[DW_WORK] = init->workBuffer;
    static int s_logged = 0;
    if (s_logged++ < 8)
        LOGFN("[xma] context 0x{:08X} init: in0 0x{:08X} x{} in1 0x{:08X} x{} read {} out 0x{:08X} x{} blocks, {} ch, rate {}",
            context, init->input0.get(), init->input0Packets.get(), init->input1.get(), init->input1Packets.get(), init->inputReadOffset.get(),
            init->output.get(), init->outputBlocks.get(), init->channelCount.get() >= 1 ? 2 : 1, init->sampleRate.get());
    return 0;
}

uint32_t XMAEnableContext(uint32_t context)
{
    EnsureThread();
    static const bool s_noMusic = getenv("FS_XMA_NO_IMPORT_ENABLE") != nullptr; // debug: silence the music voices
    if (s_noMusic) return 0;
    auto lock = LockForImport();
    Lookup(context)->enabled = true;
    if (ENV_FLAG("FS_XMA_TRACE")) LOGFN("[xma] enable 0x{:08X}", context);
    return 0;
}

uint32_t XMADisableContext(uint32_t context, uint32_t wait)
{
    auto lock = LockForImport();
    Lookup(context)->enabled = false;
    if (ENV_FLAG("FS_XMA_TRACE")) LOGFN("[xma] disable 0x{:08X}", context);
    return 0;
}

uint32_t XMAIsInputBuffer0Valid(uint32_t context) { return Field(context, DW0, 20, 1); }
uint32_t XMAIsInputBuffer1Valid(uint32_t context) { return Field(context, DW0, 21, 1); }
static void TraceRefill(uint32_t context, int buffer)
{
    static std::atomic<int> s_count{0};
    if (ENV_FLAG("FS_XMA_TRACE") && s_count++ < 300)
        LOGFN("[xma] refill 0x{:08X} buf{}: ptr 0x{:08X} packets {} cur {} read {} w{} r{}", context, buffer, Get(context, buffer ? DW_IN1 : DW_IN0),
            buffer ? Field(context, DW1, 0, 12) : Field(context, DW0, 0, 12), Field(context, DW4, 31, 1), Field(context, DW2, 0, 26), Field(context, DW0, 27, 5), Field(context, DW9, 0, 5));
}
uint32_t XMASetInputBuffer0Valid(uint32_t context) { auto lock = LockForImport(); TraceRefill(context, 0); SetField(context, DW0, 20, 1, 1); return 0; }
uint32_t XMASetInputBuffer1Valid(uint32_t context) { auto lock = LockForImport(); TraceRefill(context, 1); SetField(context, DW0, 21, 1, 1); return 0; }
uint32_t XMAIsOutputBufferValid(uint32_t context) { return Field(context, DW1, 31, 1); }
uint32_t XMASetOutputBufferValid(uint32_t context) { auto lock = LockForImport(); SetField(context, DW1, 31, 1, 1); return 0; }
uint32_t XMAGetOutputBufferReadOffset(uint32_t context) { return Field(context, DW9, 0, 5); }
uint32_t XMASetOutputBufferReadOffset(uint32_t context, uint32_t offset) { auto lock = LockForImport(); SetField(context, DW9, 0, 5, offset & 0x1F); return 0; }
uint32_t XMAGetOutputBufferWriteOffset(uint32_t context) { return Field(context, DW0, 27, 5); }

uint64_t XmaDecodedPackets() { return g_xmaPackets.load(); }

GUEST_IMPORT(XMACreateContext, XMACreateContext);
GUEST_IMPORT(XMAReleaseContext, XMAReleaseContext);
GUEST_IMPORT(XMAInitializeContext, XMAInitializeContext);
GUEST_IMPORT(XMAEnableContext, XMAEnableContext);
GUEST_IMPORT(XMADisableContext, XMADisableContext);
GUEST_IMPORT(XMAIsInputBuffer0Valid, XMAIsInputBuffer0Valid);
GUEST_IMPORT(XMAIsInputBuffer1Valid, XMAIsInputBuffer1Valid);
GUEST_IMPORT(XMASetInputBuffer0Valid, XMASetInputBuffer0Valid);
GUEST_IMPORT(XMASetInputBuffer1Valid, XMASetInputBuffer1Valid);
GUEST_IMPORT(XMAIsOutputBufferValid, XMAIsOutputBufferValid);
GUEST_IMPORT(XMASetOutputBufferValid, XMASetOutputBufferValid);
GUEST_IMPORT(XMAGetOutputBufferReadOffset, XMAGetOutputBufferReadOffset);
GUEST_IMPORT(XMASetOutputBufferReadOffset, XMASetOutputBufferReadOffset);
GUEST_IMPORT(XMAGetOutputBufferWriteOffset, XMAGetOutputBufferWriteOffset);

// FS_XMA_TRACE: the game's per-voice XMA service routine (sub_829CC080): object +8 = context, +12 = refill
// callback, +16 = callback user data.
PPC_FUNC_IMPL(__imp__sub_829CC080);
PPC_FUNC(sub_829CC080)
{
    static const bool trace = getenv("FS_XMA_TRACE") != nullptr;
    if (trace)
    {
        static std::mutex s_m;
        static std::unordered_set<uint32_t> s_seen;
        const uint32_t obj = ctx.r3.u32;
        std::lock_guard lock(s_m);
        if (s_seen.insert(obj).second && s_seen.size() < 40)
        {
            auto w = reinterpret_cast<be<uint32_t>*>(base + obj);
            LOGFN("[xma] voice 0x{:08X}: +0 {:08X} ctx {:08X} cb {:08X} user {:08X} +20 {:08X}", obj, w[0].get(), w[2].get(), w[3].get(), w[4].get(), w[5].get());
        }
        static uint32_t s_first = 0;
        static auto s_at = std::chrono::steady_clock::now();
        if (s_first == 0)
        {
            s_first = obj;
            if (ENV_FLAG("FS_XMA_WATCH"))
            {
                void ArmPersistentWatchAt(uint32_t lo, uint32_t size, bool anyAccess);
                ArmPersistentWatchAt(reinterpret_cast<be<uint32_t>*>(base + obj)[4] + 416, 4, false);
            }
        }
        if (obj == s_first && std::chrono::steady_clock::now() - s_at > std::chrono::milliseconds(500))
        {
            s_at = std::chrono::steady_clock::now();
            const uint32_t user = reinterpret_cast<be<uint32_t>*>(base + obj)[4];
            auto u = [&](uint32_t off) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + user + off)); };
            LOGFN("[xma] voice user 0x{:08X}: state {} pkt {} end {} done388 {} f400 {} sub412 {} play416 {} next436 {} 236 {:X} 248 {:08X} | shift156 {} 228 {} 240 {} skip404 {} got408 {}",
                user, u(4), u(244), u(256), u(388), u(400), u(412), u(416), u(436), u(236), u(248), u(156), u(228), u(240), u(404), u(408));
        }
    }
    // Decode right before the game reads this voice's output ring, so the ring is always full when it looks
    // (the real decoder is far faster than playback). With only the 2 ms decoder thread, back-to-back audio
    // callbacks found the ring empty, the voice ran dry after 60 ms and the game never fed it again.
    if (const uint32_t context = reinterpret_cast<be<uint32_t>*>(base + ctx.r3.u32)[2]; context != 0)
    {
        std::lock_guard lock(g_xmaMutex);
        auto it = g_xmaContexts.find(context);
        if (it != g_xmaContexts.end() && it->second->enabled)
            WorkContext(*it->second);
    }
    __imp__sub_829CC080(ctx, base);
}

// FS_XMA_TRACE: the sound engine's per-voice mix loop (sub_82A1B620, r4 = voice, voice+48 = output sink).
PPC_FUNC_IMPL(__imp__sub_82A1B620);
PPC_FUNC(sub_82A1B620)
{
    static const bool trace = getenv("FS_XMA_TRACE") != nullptr;
    if (trace)
    {
        static std::mutex s_m;
        static auto s_at = std::chrono::steady_clock::now();
        static int s_n = 0;
        std::lock_guard lock(s_m);
        static bool s_armed = false;
        if (!s_armed && ENV_FLAG("FS_MIX_WATCH"))
        {
            s_armed = true;
            void ArmPersistentWatchAt(uint32_t lo, uint32_t size, bool anyAccess);
            ArmPersistentWatchAt(ctx.r4.u32 + 16, 4, false);
        }
        if (std::chrono::steady_clock::now() - s_at > std::chrono::milliseconds(700) && s_n < 80)
        {
            s_at = std::chrono::steady_clock::now(); s_n++;
            auto W = [&](uint32_t a) { return a ? uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)) : 0u; };
            const uint32_t self = ctx.r3.u32, voice = ctx.r4.u32, sink = W(voice + 48), vt = W(sink);
            LOGFN("[mix] self {:08X} [+20 {} +28 {}] voice {:08X} [+0 {:08X} +16 {} +20 {}] sink {:08X} vt {:08X} fn1 {:08X} [+0..+28: {} {} {} {} {} {} {} {}]",
                self, W(self + 20), W(self + 28), voice, W(voice), W(voice + 16), W(voice + 20), sink, vt, W(vt + 4),
                W(sink + 4), W(sink + 8), W(sink + 12), W(sink + 16), W(sink + 20), W(sink + 24), W(sink + 28), W(sink + 32));
            if (s_n <= 2)
            {
                std::string v, f;
                for (int i = 0; i < 12; i++) v += fmt::format(" {:08X}", W(vt + i * 4));
                for (int i = 9; i < 28; i++) f += fmt::format(" {:08X}", W(sink + i * 4));
                LOGFN("[mix] sink vtable:{} | sink +36..:{}", v, f);
            }
        }
    }
    __imp__sub_82A1B620(ctx, base);
}

// FS_XMA_TRACE: the sound engine's output cursor update (sub_82A033B0): obj vtable[4] returns the play position.
PPC_FUNC_IMPL(__imp__sub_82A033B0);
PPC_FUNC(sub_82A033B0)
{
    static const bool trace = getenv("FS_XMA_TRACE") != nullptr;
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_82A033B0(ctx, base);
    static bool s_armed = false;
    if (!s_armed && ENV_FLAG("FS_CURSOR_WATCH") && *reinterpret_cast<be<uint32_t>*>(base + obj + 8) != 0)
    {
        s_armed = true;
        void ArmPersistentWatchAt(uint32_t lo, uint32_t size, bool anyAccess);
        ArmPersistentWatchAt(obj + 168, 16, false);
    }
    if (trace && *reinterpret_cast<be<uint32_t>*>(base + obj + 8) != 0)
    {
        static auto s_at = std::chrono::steady_clock::now();
        static int s_n = 0;
        if (std::chrono::steady_clock::now() - s_at > std::chrono::milliseconds(300) && s_n < 60)
        {
            s_at = std::chrono::steady_clock::now(); s_n++;
            auto W = [&](uint32_t a) { return a ? uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)) : 0u; };
            const uint32_t vt = W(obj);
            std::string x; for (int i = 36; i < 48; i++) x += fmt::format(" {:08X}", W(obj + i * 4));
            LOGFN("[mix] cursor obj {:08X} fn4 {:08X} voice {:08X} state16 {} 20 {} last24 {:08X}{:08X} +144..+188:{}",
                obj, W(vt + 16), W(obj + 8), W(obj + 16), W(obj + 20), W(obj + 24), W(obj + 28), x);
        }
    }
}

// Debug FS_LOG_MEMCPY=<hex lo>,<hex hi>: log guest memcpy (sub_82998F50) calls whose destination is in [lo, hi)
// (used to follow how the Bink planes get filled).
PPC_FUNC_IMPL(__imp__sub_82998F50);
PPC_FUNC(sub_82998F50)
{
    static uint32_t s_lo = 0, s_hi = 0;
    static const bool s_on = [] { const char* e = getenv("FS_LOG_MEMCPY"); if (!e) return false; char* end; s_lo = uint32_t(strtoul(e, &end, 16)); s_hi = (*end == ',') ? uint32_t(strtoul(end + 1, nullptr, 16)) : s_lo + 0x1000; return true; }();
    if (s_on && ctx.r3.u32 >= s_lo && ctx.r3.u32 < s_hi)
    {
        static std::atomic<int> s_n{0};
        if (s_n++ < 4000)
            LOGFN("[memcpy] dst 0x{:08X} src 0x{:08X} size 0x{:X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    }
    __imp__sub_82998F50(ctx, base);
}


// The game's own sound engine (statically linked XMA code at 0x82D01xxx) runs its XMA voices - the menu and
// game sound effects - without XMAEnableContext: it copies its local context copy into the context memory and
// "kicks" the hardware by writing bit (index & 31) of the APU register 0x7FEA1940 + 4 * (index >> 5)
// (Xenia: Context0Kick, register 0x650). Those register writes went nowhere, so the contexts never ran and
// only the music (which uses XMAEnableContext) was heard - the tester: "no sound in main menu, only music".
// sub_82D01F10(voice): voice +0 = entry count, +8 = entries (96 bytes each: 64-byte local context copy,
// +64 = the context address). After the game's own code ran, every entry's context is enabled here.
// Step 35f (the tester: "when you destroy barrels/urns the sound either crackles or repeats itself"): the sound
// engine's protocol for a context is lock -> read it back -> change it -> write the changed groups back -> kick.
// sub_82D01DA0 (once per audio pass) stops the decoder (register 0x7FEA1804), copies dwords 0-3 of every
// active voice's contexts into its local copies, writes their lock bits (0x7FEA1A40) and flags the voice
// (+4 |= 0x20000); the kick routine sub_82D01F10 writes the local copy's dirty 16-byte groups back and clears
// the flag. Those register writes go nowhere here, and the decoder kept working on locked contexts: when it
// used up input between the read-back and the kick, the kick wrote the OLD input state back (valid bit, read
// offset) and the same packets were decoded a second time - a repeated or torn piece of the sound. Now nothing
// is decoded on a context from the read-back until its kick. FS_NO_XMA_LOCK=1: off.
PPC_FUNC_IMPL(__imp__sub_82D01DA0);
PPC_FUNC(sub_82D01DA0)
{
    // (This game's voices never get the lock flag - 0 locks in 1900 calls per 5 s - so the hook is off unless
    // FS_XMA_LOCK=1 is set; kept for the record of how the protocol works.)
    static const bool s_off = getenv("FS_XMA_LOCK") == nullptr;
    if (s_off)
    {
        __imp__sub_82D01DA0(ctx, base);
        return;
    }
    {
        std::lock_guard lock(g_xmaMutex); // a decode in progress finishes first
        g_xmaFreeze++;
    }
    __imp__sub_82D01DA0(ctx, base);
    {
        std::lock_guard lock(g_xmaMutex);
        auto W = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        constexpr uint32_t VOICE_TABLE = 0x82F55500; // 320 voice objects (sub_82D01DA0's loop)
        const auto now = std::chrono::steady_clock::now();
        uint32_t lockedNow = 0;
        static uint64_t s_calls = 0, s_locked = 0;
        static auto s_logAt = now;
        for (uint32_t i = 0; i < 320; i++)
        {
            const uint32_t voice = W(VOICE_TABLE + i * 4);
            if (voice == 0 || (W(voice + 4) & 0x20000) == 0)
                continue;
            const uint32_t count = W(voice), entries = W(voice + 8);
            if (count == 0 || count > 16 || entries == 0)
                continue;
            for (uint32_t k = 0; k < count; k++)
            {
                auto it = g_xmaContexts.find(W(entries + k * 96 + 64));
                if (it != g_xmaContexts.end() && !it->second->locked)
                {
                    it->second->locked = true;
                    it->second->lockedAt = now;
                    lockedNow++;
                    if (Trace2(it->first)) LOGFN("[xt] 0x{:08X} LOCK: local dw0 {:08X} dw2 {:08X} | context dw0 {:08X} dw2 {:08X}", it->first, W(entries + k * 96), W(entries + k * 96 + 8), Get(it->first, DW0), Get(it->first, DW2));
                }
            }
        }
        g_xmaFreeze--;
        s_calls++; s_locked += lockedNow;
        static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
        if (s_lat && now - s_logAt > std::chrono::seconds(5))
        {
            LOGFN("[xma] lock pass: {} calls and {} context locks in the last 5 s (r13 0x{:08X})", s_calls, s_locked, ctx.r13.u32);
            s_calls = s_locked = 0; s_logAt = now;
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_82D01F10);
PPC_FUNC(sub_82D01F10)
{
    const uint32_t voice = ctx.r3.u32;
    // Step 37: a voice entry whose context pointer is not an XMA context (a torn-down voice's entries that already
    // hold something else): the kick would copy 48 bytes of context state there. Its "changed" flags are cleared so
    // the kick copies nothing for it. FS_NO_KICK_CHECK=1: off.
    if (static const bool s_check = getenv("FS_NO_KICK_CHECK") == nullptr && !XmaHeapContexts(); s_check)
    {
        auto W0 = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        const uint32_t count0 = W0(voice), entries0 = W0(voice + 8);
        uint32_t poolBase, poolEnd;
        { std::lock_guard l(g_xmaPool.m); poolBase = g_xmaPool.base; poolEnd = poolBase + XmaContextPool::SLOTS * XmaContextPool::STRIDE; }
        if (poolBase != 0 && count0 <= 16 && entries0 != 0)
            for (uint32_t i = 0; i < count0; i++)
            {
                const uint32_t e = entries0 + i * 96, c = W0(e + 64);
                if (c == 0 || (c >= poolBase && c < poolEnd && (c & 0xFF) == 0))
                    continue;
                static std::atomic<int> s_n{0};
                if (s_n++ < 20)
                {
                    extern std::string CurrentGuestCallChain(int maxFrames);
                    LOGFN("[xma] kick of voice 0x{:08X} entry {}: context pointer 0x{:08X} is not an XMA context - not copied (r13 0x{:08X}) |{}", voice, i, c, ctx.r13.u32, CurrentGuestCallChain(10));
                }
                *reinterpret_cast<be<uint32_t>*>(base + e + 48) = 0u;
                *reinterpret_cast<be<uint32_t>*>(base + e + 52) = 0u;
                *reinterpret_cast<be<uint32_t>*>(base + e + 56) = 0u;
            }
    }
    {
        auto W0 = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        const uint32_t count0 = W0(voice), entries0 = W0(voice + 8);
        if (count0 >= 1 && count0 <= 16 && entries0 != 0 && Trace2(W0(entries0 + 64)))
        {
            const uint32_t context0 = W0(entries0 + 64);
            LOGFN("[xt] 0x{:08X} kick: local dw0 {:08X} dw1 {:08X} dw2 {:08X} dirty {}{}{} | context dw0 {:08X} dw1 {:08X} dw2 {:08X} dw9 {:08X}", context0, W0(entries0), W0(entries0 + 4), W0(entries0 + 8),
                W0(entries0 + 48) ? 1 : 0, W0(entries0 + 52) ? 1 : 0, W0(entries0 + 56) ? 1 : 0, context0 ? Get(context0, DW0) : 0, context0 ? Get(context0, DW1) : 0, context0 ? Get(context0, DW2) : 0, context0 ? Get(context0, DW9) : 0);
        }
    }
    // Step 37 (Odin crashes in voice code just after a kick, the kicked voice in r4): the kick copies the voice's
    // changed context words into the context memory while the decoder thread could be reading that context - a
    // new output buffer with the old block count, for example, and samples were written past the new buffer into
    // the objects next to it. The decoder now waits while the kick writes. FS_XMA_KICK_UNLOCKED=1 = as before.
    // (Not by holding g_xmaMutex across the game's code: the kick enters the game's own locks, and a game thread
    // holding one of those while calling an XMA function waited for g_xmaMutex - the game froze on the tester's Odin.
    // The decoder thread skips every context while a kick runs; a decode already running is waited for.)
    static const bool s_unlocked = getenv("FS_XMA_KICK_PAUSE") == nullptr; // Step 37: OFF by default - measured: with it on, 5 of 8 PC runs crashed; off (as before), 0 of 20. Opt-in for tests.
    if (!s_unlocked)
    {
        std::lock_guard wait(g_xmaMutex); // a decode in progress finishes first
        g_kicksRunning++;
    }
    t_inKick = true;
    __imp__sub_82D01F10(ctx, base);
    t_inKick = false;
    if (!s_unlocked)
        g_kicksRunning--;
    static const bool s_off = getenv("FS_NO_XMA_KICK") != nullptr;
    if (s_off)
        return;
    auto W = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
    const uint32_t count = W(voice), entries = W(voice + 8);
    if (count == 0 || count > 16 || entries == 0)
        return;
    EnsureThread();
    std::lock_guard lock(g_xmaMutex);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t context = W(entries + i * 96 + 64);
        if (context == 0)
            continue;
        // Step 37 (crashes on the Odin in voice code, right at a kick, with the kicked voice in r3): a kick of a
        // context the game had already released (XMAReleaseContext frees its memory) made a new tracking entry
        // here, and the decoder thread then kept writing context fields and samples into that freed memory -
        // which the game had handed out again (a voice object). Only contexts we know (created or initialised
        // and not released) are run. FS_XMA_KICK_ANY=1 = as before.
        static const bool s_kickAny = getenv("FS_XMA_KICK_KNOWN") == nullptr; // Step 37: OFF by default - measured: with it on, 5 of 8 PC runs crashed; off (as before), 0 of 20. Opt-in for tests.
        if (!s_kickAny && g_xmaContexts.find(context) == g_xmaContexts.end())
        {
            static std::atomic<int> s_unknown{0};
            if (s_unknown++ < 20)
                LOGFN("[xma] kick of an unknown (released?) context 0x{:08X} (voice 0x{:08X}) ignored", context, voice);
            continue;
        }
        XmaContext* c = Lookup(context);
        // A new sound in the same context: the input read offset went back. Drop the old decoder state and
        // the previous sound's leftover samples, so the new one starts clean.
        const uint32_t readOffset = Get(context, DW2) & 0x3FFFFFF, input = Get(context, DW_IN0);
        // A new sound: the output ring write offset and the input read offset were reset. (The ring's read offset
        // is 4 or 16 then, not 0 - the old test wanted 0, so the reset below never ran for the game's sounds.)
        const bool freshContext = readOffset <= 32 && Field(context, DW0, 27, 5) == 0;
        {
            static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
            static std::atomic<int> s_n{0};
            if (s_lat && (readOffset < c->lastKickRead || input != c->lastKickInput) && s_n++ < 3000)
                LOGFN("[xma] kick new input on 0x{:08X} (voice 0x{:08X}): fresh {} enabled {} read {} (was {}) in 0x{:08X} (was 0x{:08X}) x{} packets, out w{} r{}, held back {} samples, pending {} bytes, rate {}",
                    context, voice, freshContext, c->enabled, readOffset, c->lastKickRead, input, c->lastKickInput, Field(context, DW0, 0, 12), Field(context, DW0, 27, 5), Field(context, DW9, 0, 5),
                    int64_t(c->expected) - int64_t(c->produced), c->pending.size(), c->sampleRate);
        }
        if (c->enabled && freshContext && (readOffset < c->lastKickRead || input != c->lastKickInput || (c->lastWrite != 0 && c->pos != 0)))
        {
            {
                static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
                static std::atomic<int> s_k{0};
                if (s_lat && s_k++ < 400)
                    LOGFN("[xma] new sound on 0x{:08X}: {} packets, loop count {}, loop start bit {}, loop end bit {} (input ends at bit {}), subframe end {} skip {}", context, Field(context, DW0, 0, 12), Field(context, DW0, 12, 8),
                        Field(context, DW3, 0, 26), Field(context, DW4, 0, 26), Field(context, DW0, 0, 12) * 2048 * 8, Field(context, DW1, 14, 3), Field(context, DW1, 17, 3));
            }
            // Test: FS_XMA_DUMP_DIR=<folder>: the input of every new one-shot sound of 2 to 40 packets is written there
            // (<log ms>_<context>_<packets>.bin), to look at its frames outside the game.
            if (static const char* s_dir = getenv("FS_XMA_DUMP_DIR"); s_dir && *s_dir)
            {
                const uint32_t packets = Field(context, DW0, 0, 12);
                static std::atomic<int> s_d{0};
                static const auto s_t0 = std::chrono::steady_clock::now();
                if (packets >= 2 && packets <= 40 && s_d++ < 4000)
                    if (uint8_t* in = Phys(Get(context, DW_IN0)))
                    {
                        char name[300];
                        snprintf(name, sizeof(name), "%s/%08lld_%08X_%u.bin", s_dir, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count(), context, packets);
                        if (FILE* f = fopen(name, "wb")) { fwrite(in, 1, size_t(packets) * 2048, f); fclose(f); }
                        if (c->dumpPcm) fclose(c->dumpPcm);
                        strcat(name, ".pcm");
                        c->dumpPcm = fopen(name, "wb");
                    }
            }
            if (g_trace2Ctx.load() == 0 && Trace2Time()) g_trace2Ctx = context;
            // Test: FS_XMA_TRACE_PACKETS=<n>: trace the first new sound of n packets instead.
            if (static const int s_tp = [] { const char* e = getenv("FS_XMA_TRACE_PACKETS"); return e ? atoi(e) : 0; }(); s_tp > 0 && g_trace2Ctx.load() == 0 && int(Field(context, DW0, 0, 12)) == s_tp)
                g_trace2Ctx = context;
            if (Trace2(context)) LOGFN("[xt] 0x{:08X} NEW SOUND: dw0 {:08X} dw1 {:08X} dw2 {:08X} dw3 {:08X} dw4 {:08X} in0 0x{:08X} in1 0x{:08X} dw9 {:08X}", context, Get(context, DW0), Get(context, DW1), Get(context, DW2), Get(context, DW3), Get(context, DW4), Get(context, DW_IN0), Get(context, DW_IN1), Get(context, DW9));
            ResetSound(*c);
            c->lastWrite = 0;
            if (const SoundCut* cut = FindSoundCut(Field(context, DW0, 0, 12), Phys(Get(context, DW_IN0))))
            {
                c->cutFrom = cut->from; c->cutTo = cut->to;
                static std::atomic<int> s_n{0};
                if (s_n++ < 20)
                    LOGFN("[xma] context 0x{:08X}: {} sound - samples {}..{} ({:.2f} s of silence) are left out", context, cut->what, cut->from, cut->to, double(cut->to - cut->from) / 24000.0);
            }
            {
                // Test: FS_XMA_SOLO=<packets>[,<packets>...]: only one-shot sounds of these lengths are heard
                // (everything else decodes to silence) - to time one sound against the picture.
                static const std::string s_solo = [] { const char* e = getenv("FS_XMA_SOLO"); return std::string(e ? e : ""); }();
                if (!s_solo.empty())
                    c->soloMute = ("," + s_solo + ",").find("," + std::to_string(Field(context, DW0, 0, 12)) + ",") == std::string::npos;
            }
        }
        if (freshContext && (readOffset < c->lastKickRead || input != c->lastKickInput || !c->enabled))
        {
            c->initAt = std::chrono::steady_clock::now();
            c->firstOutLogged = false;
        }
        if (c->ended && !c->replaying && Field(context, DW0, 20, 1) != 0 && readOffset <= 32)
        {
            c->replaying = true;
            static const bool s_lat = getenv("FS_XMA_LATENCY") != nullptr;
            static std::atomic<int> s_k{0};
            if (s_lat && s_k++ < 300)
                LOGFN("[xma] context 0x{:08X}: the ended sound's input was made valid again (out w{} r{})", context, Field(context, DW0, 27, 5), Field(context, DW9, 0, 5));
        }
        c->lastKickRead = readOffset;
        c->lastKickInput = input;
        c->kicked = true;
        c->locked = false; // the kick ends the sound engine's lock
        if (!c->enabled)
        {
            c->enabled = true;
            static int s_logged = 0;
            if (s_logged++ < 20)
                LOGFN("[xma] kick: sound engine started context 0x{:08X} (voice 0x{:08X}, dw0 {:08X} dw1 {:08X})", context, voice, Get(context, DW0), Get(context, DW1));
        }
        WorkContext(*c); // like the hardware: decoding goes on right at the kick
    }
}

// Step 35b: the sound engine's "feed the next XMA packet?" check (sub_829E6248, voice user object r3): it skips
// feeding while the voice's submitted samples (+412) are 2400 or more ahead of what was played (+416). FFmpeg's
// XMA decoder holds back up to 4608 samples, so without start-up silence (which delayed every sound) the voices
// stalled. During the check the played count is raised by that hold-back (and lowered by it again afterwards).
// FS_XMA_PAD=1: off (old start-up padding instead).
PPC_FUNC_IMPL(__imp__sub_829E6248);
PPC_FUNC(sub_829E6248)
{
    // Step 35c: not needed any more (no samples are held back); FS_XMA_FEED_HACK=1 turns it back on.
    static const bool s_off = getenv("FS_XMA_FEED_HACK") == nullptr;
    const uint32_t obj = ctx.r3.u32;
    auto& played = *reinterpret_cast<be<uint32_t>*>(base + obj + 416);
    const uint32_t submitted = *reinterpret_cast<be<uint32_t>*>(base + obj + 412);
    constexpr uint32_t HOLD_BACK = 5120;
    const uint32_t p = played;
    if (s_off || obj == 0 || submitted <= p || submitted - p < 2400 || submitted - p >= 2400 + HOLD_BACK)
    {
        __imp__sub_829E6248(ctx, base);
        return;
    }
    played = p + HOLD_BACK;
    __imp__sub_829E6248(ctx, base);
    played = uint32_t(played) - HOLD_BACK;
}
