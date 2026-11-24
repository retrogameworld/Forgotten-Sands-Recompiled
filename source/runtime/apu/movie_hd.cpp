// Step 35: 4K / 60 fps movies (the tester: "4k intro videos 120fps (or 60 fps)").
//
// tools\make_hd_movies.py turns each .bik into video_pack\<name>.mp4 (RIFE 2x frame interpolation, Real-ESRGAN
// 3x upscale, 3840x2160 at 59.94 fps, video only). While the game plays <name>.bik, its movie player copies each
// new Bink frame into three 8-bit plane textures (Y, and the two chroma planes); right after that copy the 4K
// planes of the same moment (HD frame 2 * Bink frame) replace them on the host (MovieHdUpload in gpu\video.cpp),
// and half a Bink frame later (MovieHdTick, at the next Swap) the in-between HD frame does. The game's own movie
// shader, sound and timing stay as they are. A background thread decodes ahead. FS_NO_HD_MOVIES=1: off.
//
// Step 35k (the tester: "make them 120hz with frame interpolation"): tools\make_hd_movies120.py makes
// video_pack\<name>_120.mp4 with 4 frames per Bink frame (119.88 fps). When that file exists it is played
// instead: HD frame 4n at the copy of Bink frame n, then 4n+1, 4n+2, 4n+3 one render frame apart (at a lower
// render rate the frames that are due are skipped to the newest one). FS_NO_HD_120=1: use the 60 fps file.
#include <stdafx.h>
#include <os/logger.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

void MovieHdUpload(uint32_t header, const uint8_t* data, uint32_t width, uint32_t height, uint32_t pitch);

namespace
{
    struct HdFrame
    {
        int64_t index = 0;
        int width = 0, height = 0;
        std::vector<uint8_t> y, u, v; // tightly packed
    };

    struct HdStream
    {
        std::filesystem::path path;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<HdFrame> frames;
        int64_t want = 0;      // the consumer needs frames from this index on
        int mult = 2;          // HD frames per Bink frame (2: 59.94 fps file, 4: 119.88 fps file)
        int64_t baseIndex = -1; // mult * Bink frame of the last copy
        int shownK = 0;         // in-between frames of that Bink frame shown so far (1 .. mult - 1)
        int ticksSinceCopy = 0; // Swaps since the last copy
        std::chrono::steady_clock::time_point copyAt;
        bool stop = false, eof = false, failed = false;

        // planes of the last copy, and the in-between frame waiting for its time
        uint32_t texY = 0, texU = 0, texV = 0;
        int64_t pendingIndex = -1;
        std::chrono::steady_clock::time_point pendingAt;
        // Step 35b: the player double-buffers its planes and shows the set filled by the PREVIOUS copy while the
        // new one is filled; the in-between frame goes into that shown set (it went into the hidden one before,
        // where the next HD frame overwrote it before it was ever shown: the logos looked 30 fps).
        uint32_t pendY = 0, pendU = 0, pendV = 0;
        HdFrame pendingFrame;
        bool havePending = false;

        void Run()
        {
            AVFormatContext* fmt = nullptr;
            if (avformat_open_input(&fmt, (const char*)path.u8string().c_str(), nullptr, nullptr) != 0 || avformat_find_stream_info(fmt, nullptr) < 0)
            {
                std::lock_guard l(mutex); failed = true; cv.notify_all();
                if (fmt) avformat_close_input(&fmt);
                return;
            }
            const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            const AVCodec* dec = stream >= 0 ? avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id) : nullptr;
            AVCodecContext* codec = dec ? avcodec_alloc_context3(dec) : nullptr;
            if (!codec || avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar) < 0)
            {
                std::lock_guard l(mutex); failed = true; cv.notify_all();
                avformat_close_input(&fmt);
                return;
            }
            codec->thread_count = 16;
            if (avcodec_open2(codec, dec, nullptr) < 0)
            {
                std::lock_guard l(mutex); failed = true; cv.notify_all();
                avcodec_free_context(&codec); avformat_close_input(&fmt);
                return;
            }
            AVPacket* pkt = av_packet_alloc();
            AVFrame* frm = av_frame_alloc();
            int64_t index = 0;
            bool inputDone = false;
            while (true)
            {
                {
                    std::unique_lock l(mutex);
                    cv.wait(l, [&] { return stop || frames.size() < size_t(mult * 4); });
                    if (stop) break;
                }
                int r = avcodec_receive_frame(codec, frm);
                if (r == AVERROR(EAGAIN))
                {
                    if (inputDone) break;
                    if (av_read_frame(fmt, pkt) < 0) { avcodec_send_packet(codec, nullptr); inputDone = true; continue; }
                    if (pkt->stream_index == stream) avcodec_send_packet(codec, pkt);
                    av_packet_unref(pkt);
                    continue;
                }
                if (r < 0)
                    break;
                const int64_t i = index++;
                bool keep;
                { std::lock_guard l(mutex); keep = i + 1 >= want; } // frames the player has already passed are dropped
                if (keep && frm->format == AV_PIX_FMT_YUV420P)
                {
                    HdFrame f;
                    f.index = i; f.width = frm->width; f.height = frm->height;
                    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
                    f.y.resize(size_t(f.width) * f.height); f.u.resize(size_t(cw) * ch); f.v.resize(size_t(cw) * ch);
                    for (int row = 0; row < f.height; row++) memcpy(&f.y[size_t(row) * f.width], frm->data[0] + size_t(row) * frm->linesize[0], f.width);
                    for (int row = 0; row < ch; row++)
                    {
                        memcpy(&f.u[size_t(row) * cw], frm->data[1] + size_t(row) * frm->linesize[1], cw);
                        memcpy(&f.v[size_t(row) * cw], frm->data[2] + size_t(row) * frm->linesize[2], cw);
                    }
                    std::lock_guard l(mutex);
                    frames.push_back(std::move(f));
                    cv.notify_all();
                }
            }
            av_frame_free(&frm); av_packet_free(&pkt);
            avcodec_free_context(&codec); avformat_close_input(&fmt);
            std::lock_guard l(mutex); eof = true; cv.notify_all();
        }

        // The frame with this index if it is decoded already (does not drop it otherwise).
        bool TakeIfReady(int64_t index, HdFrame& out)
        {
            std::lock_guard l(mutex);
            while (!frames.empty() && frames.front().index < index) frames.pop_front();
            cv.notify_all();
            if (frames.empty() || frames.front().index != index)
                return false;
            out = std::move(frames.front());
            frames.pop_front();
            cv.notify_all();
            return true;
        }

        // The frame with this index (waits up to `waitMs` for the decoder), or nullptr.
        bool Take(int64_t index, HdFrame& out, int waitMs)
        {
            std::unique_lock l(mutex);
            want = index;
            while (!frames.empty() && frames.front().index < index) frames.pop_front();
            cv.notify_all();
            cv.wait_for(l, std::chrono::milliseconds(waitMs), [&] { return failed || eof || (!frames.empty() && frames.front().index >= index); });
            if (frames.empty() || frames.front().index != index)
                return false;
            out = std::move(frames.front());
            frames.pop_front();
            cv.notify_all();
            return true;
        }

        ~HdStream()
        {
            { std::lock_guard l(mutex); stop = true; }
            cv.notify_all();
            if (thread.joinable()) thread.join();
        }
    };

    std::mutex g_hdMutex;
    std::map<uint32_t, std::unique_ptr<HdStream>> g_streams; // per movie player object
    std::map<uint32_t, int64_t> g_lastBinkFrame;

    // Uploads take the renderer's lock: never call with g_hdMutex held (the Swap hook holds the renderer's lock
    // and then takes g_hdMutex).
    void Show(uint32_t texY, uint32_t texU, uint32_t texV, const HdFrame& f)
    {
        MovieHdUpload(texY, f.y.data(), f.width, f.height, f.width);
        const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
        if (texU) MovieHdUpload(texU, f.u.data(), cw, ch, cw);
        if (texV) MovieHdUpload(texV, f.v.data(), cw, ch, cw);
    }
}

// Called by the movie player hook right after the game copied Bink frame `binkFrame` (0-based) of `bik` into
// the plane textures texY / tex1 / tex2 (plane 1 is V unless chromaU).
void MovieHdAfterCopy(uint32_t player, const std::filesystem::path& bik, int64_t binkFrame, uint32_t texY, uint32_t tex1, uint32_t tex2, bool chromaU)
{
    static const bool s_off = getenv("FS_NO_HD_MOVIES") != nullptr;
    if (s_off || binkFrame < 0 || texY == 0)
        return;
    const auto packDir = bik.parent_path().parent_path().parent_path() / "video_pack";
    const auto hd60 = packDir / (bik.stem().string() + ".mp4"), hd120 = packDir / (bik.stem().string() + "_120.mp4");
    static const bool s_no120 = getenv("FS_NO_HD_120") != nullptr;
    std::unique_lock lock(g_hdMutex);
    auto& s = g_streams[player];
    std::error_code ec;
    if (!s || (s->path != hd60 && s->path != hd120) || binkFrame < g_lastBinkFrame[player])
    {
        s.reset();
        const bool use120 = !s_no120 && std::filesystem::exists(hd120, ec);
        if (!use120 && !std::filesystem::exists(hd60, ec))
            return;
        s = std::make_unique<HdStream>();
        s->path = use120 ? hd120 : hd60;
        s->mult = use120 ? 4 : 2;
        s->want = binkFrame * s->mult;
        s->thread = std::thread([p = s.get()] { p->Run(); });
        LOGFN("[movie] 4K version: {} ({} frames per movie frame)", (const char*)s->path.u8string().c_str(), s->mult);
    }
    g_lastBinkFrame[player] = binkFrame;
    s->texY = texY;
    s->texU = chromaU ? tex1 : tex2;
    s->texV = chromaU ? tex2 : tex1;
    s->havePending = false;
    s->pendingIndex = -1;
    s->baseIndex = -1;
    HdFrame f;
    // The first frames wait for the decoder to start; later ones are normally ready.
    if (s->Take(binkFrame * s->mult, f, binkFrame < 2 ? 200 : 25))
    {
        // The in-between frames go into the same planes (the player draws the set it just filled). An upload at a
        // Swap only shows from the next frame on, so in-between frame k is due k / mult of a Bink frame after the
        // copy minus one render frame (and a little slack): see MovieHdTick.
        s->baseIndex = binkFrame * s->mult;
        s->shownK = 0;
        s->ticksSinceCopy = 0;
        s->copyAt = std::chrono::steady_clock::now();
        s->pendY = s->texY; s->pendU = s->texU; s->pendV = s->texV;
        const uint32_t y = s->texY, u = s->texU, v = s->texV;
        static int s_dbg = 0;
        if (getenv("FS_LOG_MOVIE_TEX") && s_dbg++ < 400) LOGFN("[movie] copy bink {} -> hd {} into Y 0x{:08X}", binkFrame, binkFrame * s->mult, y);
        lock.unlock();
        Show(y, u, v, f);
    }
    else
    {
        static int s_missed = 0;
        if (s_missed++ < 20)
            LOGFN("[movie] 4K frame {} not ready - the game's own frame is shown", binkFrame * s->mult);
    }
}

// Every Swap: the in-between 59.94 fps frame, half a Bink frame after the copy.
void MovieHdTick()
{
    struct Job { uint32_t y, u, v; HdFrame f; };
    std::vector<Job> jobs;
    {
        std::lock_guard lock(g_hdMutex);
        const auto now = std::chrono::steady_clock::now();
        // render frame time in microseconds (smoothed)
        static auto s_last = now; static double s_period = 8333.0;
        const double dt = std::chrono::duration<double, std::micro>(now - s_last).count();
        s_last = now;
        if (dt > 1000.0 && dt < 100000.0) s_period += (dt - s_period) * 0.1;
        for (auto& [player, s] : g_streams)
        {
            if (!s || s->baseIndex < 0 || s->shownK >= s->mult - 1)
                continue;
            // What is uploaded at the j-th Swap after the copy is seen one render frame later, j render frames after
            // the frame that showed HD frame mult * n: the in-between frame nearest to that time (at 120 fps:
            // 1, 2, 3; at 60 fps: 2; the last one is held if the next Bink frame is late).
            s->ticksSinceCopy++;
            int k = int(std::lround(double(s->ticksSinceCopy) * s_period / (33367.0 / s->mult)));
            k = std::min(k, s->mult - 1);
            // nothing new, or the next Bink frame's copy comes before this upload would be seen
            if (k <= s->shownK || double(s->ticksSinceCopy) * s_period >= 33367.0 - s_period * 0.5)
                continue;
            if (s->pendingIndex != s->baseIndex + k) { s->pendingIndex = s->baseIndex + k; s->havePending = false; }
            // Step 35b (the tester: the logos still look 30 fps): the decoder is often only just in time, so the in-between
            // frame was not there yet at the first Swap after its due time and was dropped. Try again at every Swap
            // until the next Bink frame replaces it.
            if (!s->havePending && s->TakeIfReady(s->pendingIndex, s->pendingFrame))
                s->havePending = true;
            if (s->havePending)
            {
                static int s_dbg2 = 0;
                if (getenv("FS_LOG_MOVIE_TEX") && s_dbg2++ < 400) LOGFN("[movie] tick hd {} into Y 0x{:08X}", s->pendingIndex, s->pendY);
                jobs.push_back(Job{ s->pendY, s->pendU, s->pendV, std::move(s->pendingFrame) });
                s->havePending = false;
                s->shownK = int(s->pendingIndex - s->baseIndex);
                s->pendingIndex = -1;
            }
        }
    }
    for (auto& j : jobs)
        Show(j.y, j.u, j.v, j.f);
    static int s_between = 0, s_ticks = 0, s_pending = 0; static auto s_at = std::chrono::steady_clock::now();
    s_between += int(jobs.size()); s_ticks++;
    { std::lock_guard lock(g_hdMutex); for (auto& [p, st] : g_streams) if (st && st->baseIndex >= 0 && st->shownK < st->mult - 1) s_pending++; }
    if (!g_streams.empty() && std::chrono::steady_clock::now() - s_at > std::chrono::seconds(1))
    {
        LOGFN("[movie] last second: {} in-between frames shown, {} swaps, {} swaps with one pending", s_between, s_ticks, s_pending);
        s_between = s_ticks = s_pending = 0; s_at = std::chrono::steady_clock::now();
    }
}
