// Clean Bink movie frames from FFmpeg (Forgotten Sands logo and story movies).
//
// The game's own (recompiled) Bink decoder writes wrong data into a few blocks (seen: a 16x16 chroma block of the
// Ubisoft logo filled with luma "black" 0x10 -> bright green squares). FFmpeg decodes the same .bik files
// cleanly, so every time the game's movie player (sub_823C10D8) copies a newly decoded frame from Bink's plane
// buffers into its textures, we first overwrite those plane buffers with FFmpeg's frame. Frames are matched by
// comparing a coarse luma signature with the next few FFmpeg frames, so skipped or repeated frames stay in sync.
// FS_NO_BINK_OVERRIDE=1 turns it off.
#include <stdafx.h>
#include <os/process.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <os/logger.h>
#include <deque>
#include <mutex>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace
{
    struct Frame
    {
        int64_t index = 0;
        int width = 0, height = 0;
        std::vector<uint8_t> y, u, v;
        std::vector<float> sig;
    };

    struct Decoder
    {
        std::filesystem::path path;
        AVFormatContext* fmt = nullptr;
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        int stream = -1;
        int64_t decoded = 0;     // frames produced so far
        bool eof = false;
        std::deque<Frame> ring;  // recent frames
        int64_t current = -1;    // index of the frame matched last
        int chromaOrder = -1;    // -1 unknown, 0: plane 1 = U, 1: plane 1 = V
        bool failed = false;
    };

    std::mutex g_binkMutex;
    std::deque<std::filesystem::path> g_recentPaths; // .bik files opened lately (the game opens the next movie early)
    std::map<uint32_t, Decoder> g_decoders;          // per movie player object
    std::map<uint32_t, int> g_misses;                // consecutive frames without a match, per player
    struct Redirect { uint32_t set, y, p1, p2, sy, s1, s2; } g_redirect{};

    void Close(Decoder& d)
    {
        if (d.codec) avcodec_free_context(&d.codec);
        if (d.fmt) avformat_close_input(&d.fmt);
        if (d.packet) av_packet_free(&d.packet);
        if (d.frame) av_frame_free(&d.frame);
        d = Decoder();
    }

    // Coarse luma signature: 32 x 18 cell means.
    std::vector<float> Signature(const uint8_t* y, int pitch, int width, int height)
    {
        constexpr int GX = 32, GY = 18;
        std::vector<float> s(GX * GY, 0.0f);
        for (int gy = 0; gy < GY; gy++)
            for (int gx = 0; gx < GX; gx++)
            {
                const int x0 = gx * width / GX, x1 = (gx + 1) * width / GX, y0 = gy * height / GY, y1 = (gy + 1) * height / GY;
                uint32_t sum = 0, n = 0;
                for (int yy = y0; yy < y1; yy += 4)
                    for (int xx = x0; xx < x1; xx += 4) { sum += y[size_t(yy) * pitch + xx]; n++; }
                s[gy * GX + gx] = n ? float(sum) / n : 0.0f;
            }
        return s;
    }

    float Distance(const std::vector<float>& a, const std::vector<float>& b)
    {
        float d = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i++) d += std::fabs(a[i] - b[i]);
        return a.empty() ? 1e9f : d / a.size();
    }

    bool Open(Decoder& d, const std::filesystem::path& path)
    {
        Close(d);
        d.path = path;
        const std::string u8 = (const char*)path.u8string().c_str();
        if (avformat_open_input(&d.fmt, u8.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(d.fmt, nullptr) < 0)
        {
            LOGFN_ERROR("[bink] FFmpeg could not open {}", u8);
            d.failed = true;
            return false;
        }
        d.stream = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const AVCodec* c = d.stream >= 0 ? avcodec_find_decoder(d.fmt->streams[d.stream]->codecpar->codec_id) : nullptr;
        if (!c)
        {
            LOGFN_ERROR("[bink] no FFmpeg decoder for {}", u8);
            d.failed = true;
            return false;
        }
        d.codec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(d.codec, d.fmt->streams[d.stream]->codecpar);
        if (avcodec_open2(d.codec, c, nullptr) < 0)
        {
            d.failed = true;
            return false;
        }
        d.packet = av_packet_alloc();
        d.frame = av_frame_alloc();
        LOGFN("[bink] decoding {} with FFmpeg ({}x{})", u8, d.codec->width, d.codec->height);
        return true;
    }

    bool DecodeNext(Decoder& d)
    {
        while (!d.eof)
        {
            const int r = avcodec_receive_frame(d.codec, d.frame);
            if (r == 0)
            {
                Frame f;
                f.index = d.decoded++;
                f.width = d.frame->width;
                f.height = d.frame->height;
                const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
                f.y.resize(size_t(f.width) * f.height);
                f.u.resize(size_t(cw) * ch);
                f.v.resize(size_t(cw) * ch);
                for (int yy = 0; yy < f.height; yy++)
                    memcpy(&f.y[size_t(yy) * f.width], d.frame->data[0] + size_t(yy) * d.frame->linesize[0], f.width);
                for (int yy = 0; yy < ch; yy++)
                {
                    memcpy(&f.u[size_t(yy) * cw], d.frame->data[1] + size_t(yy) * d.frame->linesize[1], cw);
                    memcpy(&f.v[size_t(yy) * cw], d.frame->data[2] + size_t(yy) * d.frame->linesize[2], cw);
                }
                f.sig = Signature(f.y.data(), f.width, f.width, f.height);
                d.ring.push_back(std::move(f));
                while (d.ring.size() > 8)
                    d.ring.pop_front();
                return true;
            }
            if (r != AVERROR(EAGAIN))
            {
                d.eof = true;
                return false;
            }
            int rr;
            while ((rr = av_read_frame(d.fmt, d.packet)) >= 0 && d.packet->stream_index != d.stream)
                av_packet_unref(d.packet);
            if (rr < 0)
            {
                avcodec_send_packet(d.codec, nullptr);
                continue;
            }
            avcodec_send_packet(d.codec, d.packet);
            av_packet_unref(d.packet);
        }
        return false;
    }

    void WritePlane(uint32_t dst, uint32_t pitch, const uint8_t* src, int width, int rows)
    {
        if (!dst || pitch < uint32_t(width))
            return;
        uint8_t* p = reinterpret_cast<uint8_t*>(g_memory.Translate(dst));
        for (int yy = 0; yy < rows; yy++)
            memcpy(p + size_t(yy) * pitch, src + size_t(yy) * width, width);
    }
}

namespace
{
    // Which .bik file each Bink handle plays: BinkOpen (inside sub_823BF518) opens the file on the calling thread.
    uint64_t g_openSeq = 0;
    std::filesystem::path g_lastOpened;
    std::thread::id g_lastOpenedThread;
    std::map<uint32_t, std::filesystem::path> g_binkPath;   // Bink handle -> file
    std::map<uint32_t, uint32_t> g_playerBink;               // texture player -> Bink handle
}

void BinkOverrideFileOpened(const std::filesystem::path& path)
{
    std::lock_guard lock(g_binkMutex);
    g_openSeq++;
    g_lastOpened = path;
    g_lastOpenedThread = std::this_thread::get_id();
    std::erase(g_recentPaths, path);
    g_recentPaths.push_front(path);
    while (g_recentPaths.size() > 6) g_recentPaths.pop_back();
}

// sub_823C10D8(player): copies the decoded Bink planes into the movie textures when +312 is set.
// Player layout: +76 plane-buffer set, +80 + 48 * set: Y {valid, ptr, pitch}, plane 1, plane 2, alpha;
// +300 luma rows, +308 chroma rows.
void MovieHdAfterCopy(uint32_t player, const std::filesystem::path& bik, int64_t binkFrame, uint32_t texY, uint32_t tex1, uint32_t tex2, bool chromaU);
PPC_FUNC_IMPL(__imp__sub_823C10D8);
static void MovieCopyBody(PPCContext& ctx, uint8_t* base, std::filesystem::path& hdPath, int64_t& hdFrame, bool& hdChromaU);
PPC_FUNC(sub_823C10D8)
{
    // Step 35: after the game's copy, the 4K version of the same frame (apu/movie_hd.cpp).
    const uint32_t obj = ctx.r3.u32;
    const bool copying = *reinterpret_cast<uint8_t*>(base + obj + 312) != 0;
    const uint32_t newSet = ~uint32_t(*reinterpret_cast<be<uint32_t>*>(base + obj + 16)) & 1; // the texture set written now
    std::filesystem::path hdPath; int64_t hdFrame = -1; bool hdChromaU = false;
    MovieCopyBody(ctx, base, hdPath, hdFrame, hdChromaU);
    if (copying && hdFrame >= 0 && !hdPath.empty())
    {
        auto W = [&](uint32_t off) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + obj + off)); };
        // +176/+180/+184 (+16 per set) are the player's plane objects; their first word is the D3D texture
        // (sub_823C0910 locks the object, copies the rows, then flushes the cache range of *(object)).
        auto T = [&](uint32_t off) { const uint32_t o = W(off); return o ? uint32_t(*reinterpret_cast<be<uint32_t>*>(base + o)) : 0u; };
        MovieHdAfterCopy(obj, hdPath, hdFrame, T(176 + 16 * newSet), T(180 + 16 * newSet), T(184 + 16 * newSet), hdChromaU);
    }
}
static void MovieCopyBody(PPCContext& ctx, uint8_t* base, std::filesystem::path& hdPath, int64_t& hdFrame, bool& hdChromaU)
{
    static const bool s_disabled = getenv("FS_NO_BINK_OVERRIDE") != nullptr;
    const uint32_t obj = ctx.r3.u32;
    if (static const bool s_trace = getenv("FS_MOVIE_TRACE") != nullptr; s_trace && *reinterpret_cast<uint8_t*>(base + obj + 312) != 0)
    {
        static int s_uploads = 0; static auto s_t = std::chrono::steady_clock::now();
        s_uploads++;
        if (std::chrono::steady_clock::now() - s_t > std::chrono::seconds(1)) { LOGFN("[movie] {} new movie frames shown in the last second", s_uploads); s_uploads = 0; s_t = std::chrono::steady_clock::now(); }
    }
    if (!s_disabled && *reinterpret_cast<uint8_t*>(base + obj + 312) != 0)
    {
        std::lock_guard lock(g_binkMutex);
        auto W = [&](uint32_t off) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + obj + off)); };
        Decoder& g_dec = g_decoders[obj];
        const int rowsNow = int(W(300));
        const uint32_t setNow = obj + 80 + 48 * W(76);
        const uint32_t yNow = *reinterpret_cast<be<uint32_t>*>(base + setNow + 4), yPitchNow = *reinterpret_cast<be<uint32_t>*>(base + setNow + 8);
        // Known file and Bink frame number: show exactly that FFmpeg frame (no picture matching, no scans).
        const Frame* byNumber = nullptr;
        if (auto pb = g_playerBink.find(obj); pb != g_playerBink.end())
            if (auto bp = g_binkPath.find(pb->second); bp != g_binkPath.end())
            {
                const int64_t target = int64_t(uint32_t(*reinterpret_cast<be<uint32_t>*>(base + pb->second + 12))) - 1;
                hdPath = bp->second; hdFrame = target; hdChromaU = g_dec.chromaOrder == 0;
                if (g_dec.path != bp->second || g_dec.codec == nullptr || (!g_dec.ring.empty() && target < g_dec.ring.front().index))
                {
                    if (Open(g_dec, bp->second))
                        LOGFN("[bink] player 0x{:08X} plays {} (by Bink handle 0x{:08X})", obj, (const char*)bp->second.u8string().c_str(), pb->second);
                }
                if (g_dec.codec && !g_dec.failed && target >= 0)
                {
                    for (int k = 0; k < 400 && (g_dec.ring.empty() || g_dec.ring.back().index < target); k++)
                        if (!DecodeNext(g_dec))
                            break;
                    for (auto& f : g_dec.ring)
                        if (f.index == target)
                            byNumber = &f;
                }
            }
        if (!byNumber && (g_dec.codec == nullptr || g_misses[obj] > 2) && yNow && yPitchNow && !g_recentPaths.empty())
        {
            // Find which recently opened movie this player shows: scan each one for a frame equal to Bink's.
            g_misses[obj] = 0;
            bool found = false;
            for (const auto& path : std::vector<std::filesystem::path>(g_recentPaths.begin(), g_recentPaths.end()))
            {
                if (!Open(g_dec, path) || g_dec.codec->height != rowsNow || int(yPitchNow) < g_dec.codec->width)
                    continue;
                const auto sig = Signature(reinterpret_cast<uint8_t*>(g_memory.Translate(yNow)), int(yPitchNow), g_dec.codec->width, rowsNow);
                for (int k = 0; k < 400 && DecodeNext(g_dec); k++)
                    if (Distance(sig, g_dec.ring.back().sig) < 2.0f)
                    {
                        g_dec.current = g_dec.ring.back().index - 1;
                        found = true;
                        break;
                    }
                if (found)
                {
                    LOGFN("[bink] player 0x{:08X} shows {} (synced at frame {})", obj, (const char*)path.u8string().c_str(), g_dec.current + 1);
                    break;
                }
            }
            if (!found)
                Close(g_dec);
        }
        if (const char* e = getenv("FS_BINK_DUMP"))
        {
            static int s_calls = 0;
            if (++s_calls == atoi(e))
                for (uint32_t k = 0; k < 2; k++)
                {
                    const uint32_t st = obj + 80 + 48 * k;
                    auto SS = [&](uint32_t off) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + st + off)); };
                    for (uint32_t pl = 0; pl < 3; pl++)
                    {
                        const uint32_t ptr = SS(4 + pl * 12), pitch = SS(8 + pl * 12), h = pl == 0 ? W(300) : W(308);
                        if (!ptr || !pitch) continue;
                        std::ofstream f(os::process::GetExecutableRoot().string() + fmt::format("/debug/bink_set{}_p{}_{}x{}.pgm", k, pl, pitch, h), std::ios::binary);
                        f << "P5\n" << pitch << " " << h << "\n255\n";
                        f.write(reinterpret_cast<const char*>(g_memory.Translate(ptr)), size_t(pitch) * h);
                    }
                    LOGFN("[bink] dumped set {} (current set {})", k, W(76));
                }
        }
        const uint32_t set = obj + 80 + 48 * W(76);
        auto S = [&](uint32_t off) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + set + off)); };
        const uint32_t yPtr = S(4), yPitch = S(8), p1Ptr = S(16), p1Pitch = S(20), p2Ptr = S(28), p2Pitch = S(32);
        const int rows = int(W(300)), crows = int(W(308));
        if (!g_dec.failed && g_dec.codec && S(0) && S(12) && S(24) && yPtr && p1Ptr && p2Ptr)
        {
            while (!byNumber && !g_dec.eof && (g_dec.ring.empty() || g_dec.ring.back().index < g_dec.current + 4))
                if (!DecodeNext(g_dec))
                    break;
            const Frame* best = byNumber;
            float bestD = 1e9f;
            if (byNumber && rows == byNumber->height && int(yPitch) >= byNumber->width) // for the log: how close Bink's own frame is
                bestD = Distance(Signature(reinterpret_cast<uint8_t*>(g_memory.Translate(yPtr)), int(yPitch), byNumber->width, rows), byNumber->sig);
            if (byNumber && bestD >= 12.0f)
                bestD = 0.0f; // trust the frame number even when Bink's picture differs (its decoder has bugs)
            if (!byNumber && !g_dec.ring.empty() && g_dec.ring.front().width == g_dec.codec->width && rows == g_dec.codec->height && int(yPitch) >= g_dec.codec->width)
            {
                const auto sig = Signature(reinterpret_cast<uint8_t*>(g_memory.Translate(yPtr)), int(yPitch), g_dec.codec->width, rows);
                for (auto& f : g_dec.ring)
                {
                    if (f.index < g_dec.current)
                        continue;
                    // Prefer the frame after the last one shown. (The penalty used to count from the last frame itself,
                    // so during the Ubisoft logo's dark fade-in, where all frames look alike, frame 0 kept winning: the
                    // logo sat on black for 1.5 s and then jumped ahead - the tester: "lags hard and barely plays".)
                    const float dd = Distance(sig, f.sig) + 0.05f * std::abs(float(f.index - (g_dec.current + 1)));
                    if (dd < bestD) { bestD = dd; best = &f; }
                }
            }
            // No match among the next frames: the game may be further ahead (it skips frames when late).
            // Decode forward (up to 150 frames) until one matches.
            if (!byNumber && !(best && bestD < 6.0f) && !g_dec.ring.empty() && rows == g_dec.codec->height && int(yPitch) >= g_dec.codec->width)
            {
                const auto sig = Signature(reinterpret_cast<uint8_t*>(g_memory.Translate(yPtr)), int(yPitch), g_dec.codec->width, rows);
                for (int k = 0; k < 150 && !g_dec.eof; k++)
                {
                    if (!DecodeNext(g_dec))
                        break;
                    const float dd = Distance(sig, g_dec.ring.back().sig);
                    if (dd < bestD) { bestD = dd; best = &g_dec.ring.back(); }
                    if (dd < 3.0f)
                        break;
                }
                if (best && best != &g_dec.ring.back())
                    best = nullptr, bestD = 1e9f; // the best one fell out of the ring; try again next frame
            }
            static int s_logged = 0;
            if (best && bestD < 12.0f)
            {
                const int cw = (best->width + 1) / 2, chh = std::min(crows, (best->height + 1) / 2);
                if (g_dec.chromaOrder < 0)
                {
                    // Which game plane holds U (Cb)? Compare plane 1 with FFmpeg's U and V - only once the frame has
                    // colour (on grey/black frames U and V are equal and tell nothing).
                    const auto sigP = Signature(reinterpret_cast<uint8_t*>(g_memory.Translate(p1Ptr)), int(p1Pitch), cw, chh);
                    const auto sigU = Signature(best->u.data(), cw, cw, chh), sigV = Signature(best->v.data(), cw, cw, chh);
                    if (Distance(sigU, sigV) > 0.5f)
                    {
                        g_dec.chromaOrder = Distance(sigP, sigU) <= Distance(sigP, sigV) ? 0 : 1;
                        LOGFN("[bink] plane 1 is {} (distance U {:.2f} V {:.2f})", g_dec.chromaOrder == 0 ? "U/Cb" : "V/Cr", Distance(sigP, sigU), Distance(sigP, sigV));
                    }
                }
                // Bink keeps decoding against its own buffers (they are its reference frames), so they are left
                // alone: the copy into the textures reads FFmpeg's planes from scratch buffers instead.
                static uint32_t s_scratch = 0, s_scratchSize = 0;
                const uint32_t need = yPitch * uint32_t(rows) + (p1Pitch + p2Pitch) * uint32_t(crows) + 3 * 128;
                if (need > s_scratchSize)
                {
                    s_scratch = g_memory.MapVirtual(g_userHeap.AllocPhysical(need, 128));
                    s_scratchSize = need;
                }
                const uint32_t sy = s_scratch, s1 = (sy + yPitch * rows + 127) & ~127u, s2 = (s1 + p1Pitch * crows + 127) & ~127u;
                memcpy(g_memory.Translate(sy), g_memory.Translate(yPtr), size_t(yPitch) * rows);
                memcpy(g_memory.Translate(s1), g_memory.Translate(p1Ptr), size_t(p1Pitch) * crows);
                memcpy(g_memory.Translate(s2), g_memory.Translate(p2Ptr), size_t(p2Pitch) * crows);
                WritePlane(sy, yPitch, best->y.data(), best->width, std::min(rows, best->height));
                {
                    // Order still unknown: U and V are (near) equal in this frame, either order gives the same picture.
                    // Unknown order: assume plane 1 = V (every movie of this game so far). Assuming U showed the
                    // Ubisoft logo's first second in red (the tester).
                    const auto& a = g_dec.chromaOrder == 0 ? best->u : best->v;
                    const auto& b = g_dec.chromaOrder == 0 ? best->v : best->u;
                    WritePlane(s1, p1Pitch, a.data(), cw, chh);
                    WritePlane(s2, p2Pitch, b.data(), cw, chh);
                }
                g_redirect = { set, yPtr, p1Ptr, p2Ptr, sy, s1, s2 };
                if (s_logged < 5 || best->index != g_dec.current + 1)
                {
                    s_logged++;
                    if (s_logged < 200)
                        LOGFN("[bink] frame {} from FFmpeg (signature distance {:.2f})", best->index, bestD);
                }
                g_dec.current = best->index;
                g_misses[obj] = 0;
            }
            else if (++g_misses[obj], s_logged < 200)
            {
                s_logged++;
                LOGFN("[bink] no matching FFmpeg frame (best distance {:.2f}, ring {}..{}, current {}) - game frame kept", bestD,
                    g_dec.ring.empty() ? -1 : g_dec.ring.front().index, g_dec.ring.empty() ? -1 : g_dec.ring.back().index, g_dec.current);
            }
        }
    }
    if (g_redirect.set)
    {
        auto P = [&](uint32_t off) -> be<uint32_t>& { return *reinterpret_cast<be<uint32_t>*>(base + g_redirect.set + off); };
        P(4) = g_redirect.sy; P(16) = g_redirect.s1; P(28) = g_redirect.s2;
        __imp__sub_823C10D8(ctx, base);
        P(4) = g_redirect.y; P(16) = g_redirect.p1; P(28) = g_redirect.p2;
        g_redirect = {};
        return;
    }
    __imp__sub_823C10D8(ctx, base);
}

// Skipping the boot logos (the tester: "X skips the intro"). sub_823BF998(movie) is the game's per-frame movie update.
// Movie: +36 Bink handle (+8 frame count, +12 current frame), +40 texture player, +52 state flags
// (0x80 playing, 0x40 stopped, 0x08 end reported), +32 options (0x20 = loops). At the natural end the game
// sets flag 0x08 and posts event 4 (sub_823C07D0(movie, 4)). Posting that event early only froze the logo (the
// logo screen waits on the Bink frame itself), so a skip seeks Bink to its last frame (BinkGoto sub_82DE8690).
// Cross/A, Square/X or START pressed during Ubi_new_logo.bik / TAGLogo.bik ends that logo. FS_NO_LOGO_SKIP=1: off.
#include <hid/hid.h>
PPC_FUNC_IMPL(__imp__sub_823BF998);
PPC_FUNC_IMPL(__imp__sub_82DE8690);
PPC_FUNC(sub_823BF998)
{
    static const bool s_disabled = getenv("FS_NO_LOGO_SKIP") != nullptr;
    const uint32_t movie = ctx.r3.u32;
    // FS_MOVIE_TRACE=1: every 250 ms, log each playing movie's Bink frame (to measure playback speed).
    static const bool s_trace = getenv("FS_MOVIE_TRACE") != nullptr;
    if (s_trace)
    {
        static std::mutex s_tm;
        static std::map<uint32_t, std::chrono::steady_clock::time_point> s_last;
        std::lock_guard lock(s_tm);
        const auto now = std::chrono::steady_clock::now();
        const uint32_t bink = *reinterpret_cast<be<uint32_t>*>(base + movie + 36);
        if (bink && now - s_last[movie] > std::chrono::milliseconds(250))
        {
            s_last[movie] = now;
            LOGFN("[movie] 0x{:08X} frame {} of {} flags 0x{:02X}", movie, uint32_t(*reinterpret_cast<be<uint32_t>*>(base + bink + 12)),
                uint32_t(*reinterpret_cast<be<uint32_t>*>(base + bink + 8)), *reinterpret_cast<uint8_t*>(base + movie + 52));
        }
    }
    if (!s_disabled)
    {
        static std::mutex s_m;
        static uint16_t s_prev = 0;
        static std::chrono::steady_clock::time_point s_requestAt{};
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(s_m);

        XAMINPUT_STATE state{};
        if (hid::IsInputAllowed() && hid::GetState(0, &state) == 0)
        {
            const uint16_t buttons = state.Gamepad.wButtons & (XAMINPUT_GAMEPAD_A | XAMINPUT_GAMEPAD_X | XAMINPUT_GAMEPAD_START);
            if (buttons & ~s_prev)
                s_requestAt = now;
            s_prev = buttons;
        }

        auto B = [&](uint32_t a) { return *reinterpret_cast<uint8_t*>(base + a); };
        auto W = [&](uint32_t a) { return uint32_t(*reinterpret_cast<be<uint32_t>*>(base + a)); };
        const uint32_t bink = W(movie + 36), player = W(movie + 40);
        if (bink && player)
        {
            std::lock_guard lockBink(g_binkMutex);
            g_playerBink[player] = bink;
        }
        const uint8_t flags = B(movie + 52);
        if (now - s_requestAt < std::chrono::milliseconds(300) && bink && (flags & 0x80) && !(flags & 0x48) && !(B(movie + 32) & 0x20))
        {
            std::string name;
            {
                std::lock_guard lockBink(g_binkMutex);
                if (auto it = g_decoders.find(player); it != g_decoders.end())
                    name = (const char*)it->second.path.filename().u8string().c_str();
            }
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return char(tolower(c)); });
            if (name == "ubi_new_logo.bik" || name == "taglogo.bik")
            {
                LOGFN("[bink] skip pressed: ending {} at frame {} of {}", name, W(bink + 12), W(bink + 8));
                s_requestAt = {};
                // BinkGoto(bink, last frame - 1, 1): the game's own end check (frame + 1 == frame count) then
                // fires on the next update, exactly as when the movie runs out.
                PPCContext c = ctx;
                c.r3.u64 = bink;
                c.r4.u64 = W(bink + 8) > 1 ? W(bink + 8) - 1 : 1;
                c.r5.u64 = 1;
                __imp__sub_82DE8690(c, base);
                LOGFN("[bink] after skip: frame {} of {}", W(bink + 12), W(bink + 8));
            }
        }
    }
    __imp__sub_823BF998(ctx, base);
}

// FS_MOVIE_TRACE: time the game's Bink calls (sub_82DE7A18 = BinkDoFrame, sub_82DE8490 = BinkNextFrame).
PPC_FUNC_IMPL(__imp__sub_82DE7A18);
PPC_FUNC(sub_82DE7A18)
{
    static const bool s_trace = getenv("FS_MOVIE_TRACE") != nullptr;
    if (!s_trace) { __imp__sub_82DE7A18(ctx, base); return; }
    const auto t0 = std::chrono::steady_clock::now();
    __imp__sub_82DE7A18(ctx, base);
    static double s_sum = 0, s_max = 0; static int s_n = 0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s_sum += ms; s_max = std::max(s_max, ms);
    static int s_true = 0; if (ctx.r3.u32) s_true++;
    if (++s_n == 30) { LOGFN("[movie] sub_82DE7A18 (BinkShouldSkip?) true {} of 30 calls, avg {:.1f} ms", s_true, s_sum / 30); s_sum = s_max = 0; s_n = 0; s_true = 0; }
}

// sub_823BF518(movie, flags) opens the movie's Bink file and returns the Bink handle (stored at movie +36).
PPC_FUNC_IMPL(__imp__sub_823BF518);
PPC_FUNC(sub_823BF518)
{
    uint64_t seq0;
    {
        std::lock_guard lock(g_binkMutex);
        seq0 = g_openSeq;
    }
    __imp__sub_823BF518(ctx, base);
    std::lock_guard lock(g_binkMutex);
    if (ctx.r3.u32 && g_openSeq != seq0 && g_lastOpenedThread == std::this_thread::get_id())
    {
        g_binkPath[ctx.r3.u32] = g_lastOpened;
        LOGFN("[bink] Bink handle 0x{:08X} = {}", ctx.r3.u32, (const char*)g_lastOpened.u8string().c_str());
    }
}

// FS_MOVIE_TRACE: time the game's Bink plane decoder (sub_82DEEE50, called from sub_82DE8290 per frame).
PPC_FUNC_IMPL(__imp__sub_82DEEE50);
PPC_FUNC(sub_82DEEE50)
{
    static const bool s_trace = getenv("FS_MOVIE_TRACE") != nullptr;
    if (!s_trace) { __imp__sub_82DEEE50(ctx, base); return; }
    const auto t0 = std::chrono::steady_clock::now();
    __imp__sub_82DEEE50(ctx, base);
    static std::mutex s_m; std::lock_guard lock(s_m);
    static double s_sum = 0, s_max = 0; static int s_n = 0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s_sum += ms; s_max = std::max(s_max, ms);
    if (++s_n == 30) { LOGFN("[movie] Bink plane decode avg {:.1f} ms max {:.1f} ms (30 frames, result {})", s_sum / 30, s_max, ctx.r3.u32); s_sum = s_max = 0; s_n = 0; }
}
