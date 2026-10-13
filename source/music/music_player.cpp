#include "music/music_player.h"

#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ign { namespace music {

namespace {
constexpr uint32_t kSbFrames = 4096;       // source window (frames)
constexpr uint32_t kChunk = 1024;          // frames decoded per call
constexpr uint64_t kBlockUs = (uint64_t)kBlockFrames * 1000000ull / (uint64_t)kOutRate;     // 21333
inline void maxu(std::atomic<uint64_t>& a, uint64_t v) { uint64_t c = a.load(std::memory_order_relaxed); while (v > c && !a.compare_exchange_weak(c, v)) {} }
inline void minu(std::atomic<uint64_t>& a, uint64_t v) { uint64_t c = a.load(std::memory_order_relaxed); while (v < c && !a.compare_exchange_weak(c, v)) {} }
}

std::string MusicPlayer::last_error() { std::lock_guard<std::mutex> lk(mu_); return lastErr_; }

bool MusicPlayer::post_play(const PlayRequest& r) {
    if (backendFailed_.load()) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return false;
        if (!threadUp_) {
            pthread_attr_t at; pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 192 * 1024);                 // no alloca in the decoder (fixed buffer): generous
            pthread_t* t = new pthread_t;
            const int rc = pthread_create(t, &at, &MusicPlayer::thread_tramp, this);
            pthread_attr_destroy(&at);
            if (rc != 0) {
                delete t; backendFailed_.store(true);
                char b[64]; snprintf(b, sizeof b, "pthread_create rc=%d", rc); lastErr_ = b;
                return false;
            }
            thr_ = t; threadUp_ = true;
        }
        cmd_.kind = 1; cmd_.req = r; cmd_.seq = seq_.fetch_add(1) + 1;
        run_.store(kStarting, std::memory_order_release);
    }
    cv_.notify_all();
    plays_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MusicPlayer::post_stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        run_.store(kIdle, std::memory_order_release);            // `mode` stops saying "playing" at once
        if (!threadUp_) return;
        cmd_.kind = 2; cmd_.seq = seq_.fetch_add(1) + 1;
    }
    cv_.notify_all();
}

void MusicPlayer::shutdown() {
    void* t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        run_.store(kIdle, std::memory_order_release);
        if (!threadUp_) return;
        shutdown_ = true; seq_.fetch_add(1);                     // interrupts a running stream between blocks
        t = thr_;
    }
    cv_.notify_all();
    pthread_join(*(pthread_t*)t, nullptr);
    delete (pthread_t*)t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        thr_ = nullptr; threadUp_ = false; shutdown_ = false; cmd_ = Cmd();
        backendFailed_.store(false);                              // a closed device may be opened again
    }
}

void* MusicPlayer::thread_tramp(void* self) { ((MusicPlayer*)self)->thread_main(); return nullptr; }

void MusicPlayer::thread_main() {
    plat_->thread_enter();
    uint32_t handled = 0;
    for (;;) {
        Cmd c;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return shutdown_ || cmd_.seq != handled; });
            if (shutdown_) break;
            c = cmd_; handled = c.seq;
        }
        if (c.kind == 2) continue;
        stream(c.req, c.seq);
    }
    src_.close();
    delete[] sb_; sb_ = nullptr;
    if (portOpen_) { plat_->out_close(true); portOpen_ = false; }
}

void MusicPlayer::stream(const PlayRequest& req, uint32_t seq) {
    auto interrupted = [&] { return seq_.load(std::memory_order_acquire) != seq; };
    auto fail = [&](const std::string& why, bool backend) {
        { std::lock_guard<std::mutex> lk(mu_); lastErr_ = why; }
        failures_.fetch_add(1, std::memory_order_relaxed);
        if (backend) backendFailed_.store(true, std::memory_order_release);
        int exp = kStarting; run_.compare_exchange_strong(exp, kFailed);
        char b[300]; snprintf(b, sizeof b, "play track %d FAILED: %s", req.track, why.c_str()); plat_->log(b);
        src_.close();
    };
    src_.close();
    std::string err;
    if (!src_.open(req.path.c_str(), &err)) { fail("open: " + err, false); return; }
    if (src_.rate() != req.srcRate) { fail("rate " + std::to_string(src_.rate()) + " != probed " + std::to_string(req.srcRate), false); return; }
    if (!portOpen_) {
        const int rc = plat_->out_open(kBlockFrames, kOutRate);
        char b[160]; snprintf(b, sizeof b, "music port open: block=%d frames, %d Hz stereo rc=0x%08x", kBlockFrames, kOutRate, (unsigned)rc); plat_->log(b);
        if (rc < 0) { fail("music port open failed", true); return; }
        portOpen_ = true;
    }
    if (!sb_) sb_ = new int16_t[(size_t)kSbFrames * 2];
    rs_.reset(src_.rate());
    sbBase_ = 0; sbN_ = 0; srcEof_ = false; outDone_ = 0;
    outTotal_ = src_samples_to_out_frames(req.endSample, src_.rate());
    deliveredOut_.store(0); curRate_.store(src_.rate());
    gain_ = muteTarget_.load() ? 0.0f : 1.0f;
    { int exp = kStarting; run_.compare_exchange_strong(exp, kPlaying); }
    everPlayed_ = true; trackChanges_.fetch_add(1, std::memory_order_relaxed);
    if (!wStart_.load()) wStart_.store(plat_->now_us());

    alignas(16) int16_t blk[kBlockFrames * 2];
    uint64_t tPrevEnter = 0; int consecErr = 0; bool ended = false;
    while (!interrupted()) {
        const uint32_t depth = sbN_ - (uint32_t)(rs_.idx - sbBase_);
        qSum_.fetch_add(depth, std::memory_order_relaxed); qN_.fetch_add(1, std::memory_order_relaxed); minu(qMin_, depth);
        const uint64_t tb = plat_->now_us();
        decBlockUs_ = 0;
        const int valid = fill_block(blk);
        const uint64_t tBuilt = plat_->now_us();
        { const uint64_t rsUs = (tBuilt - tb) > decBlockUs_ ? (tBuilt - tb) - decBlockUs_ : 0;
          rsN_.fetch_add(1, std::memory_order_relaxed); rsUs_.fetch_add(rsUs, std::memory_order_relaxed); maxu(rsMax_, rsUs); }
        if (valid == 0) { ended = true; break; }
        if (interrupted()) break;
        const uint64_t tEnter = plat_->now_us();
        if (tPrevEnter) {
            const uint64_t iv = tEnter - tPrevEnter;
            playUs_.fetch_add(iv, std::memory_order_relaxed);
            if (iv > kBlockUs + kBlockUs / 2) underruns_.fetch_add(1, std::memory_order_relaxed);     // the port ran dry
        }
        tPrevEnter = tEnter;
        const int rc = plat_->out_write(blk);
        if (rc < 0) {
            portErrors_.fetch_add(1, std::memory_order_relaxed);
            if (++consecErr >= 200) { fail("music port keeps refusing writes", true); return; }
            continue;                                         // the platform paces refused writes (no busy loop)
        }
        consecErr = 0;
        deliveredOut_.fetch_add((uint64_t)valid, std::memory_order_relaxed);
        deliveredTotal_.fetch_add((uint64_t)valid, std::memory_order_relaxed);
        blocks_.fetch_add(1, std::memory_order_relaxed);
        if (valid < kBlockFrames) { ended = true; break; }
    }
    if (ended) { int exp = kPlaying; run_.compare_exchange_strong(exp, kEnded); }
    src_.close();                                              // decoder memory goes back as soon as the stream stops
}

int MusicPlayer::fill_block(int16_t* out) {
    const float target = muteTarget_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    auto ensure = [&](uint64_t i) -> bool {
        while (i >= sbBase_ + sbN_) {
            if (srcEof_) return false;
            if (sbN_ + kChunk > kSbFrames) {                   // compact: drop what the resampler has passed
                const uint32_t shift = (uint32_t)(rs_.idx - sbBase_);
                if (shift) { memmove(sb_, sb_ + (size_t)shift * 2, (size_t)(sbN_ - shift) * 2 * sizeof(int16_t)); sbBase_ += shift; sbN_ -= shift; }
            }
            const uint64_t t0 = plat_->now_us();
            const int n = src_.read(sb_ + (size_t)sbN_ * 2, (int)kChunk);
            const uint64_t dt = plat_->now_us() - t0;
            decN_.fetch_add(1, std::memory_order_relaxed); decUs_.fetch_add(dt, std::memory_order_relaxed); maxu(decMax_, dt);
            decBlockUs_ += dt;
            if (n <= 0) { srcEof_ = true; return false; }
            sbN_ += (uint32_t)n;
            decodedFrames_.fetch_add((uint64_t)n, std::memory_order_relaxed); decodedTotal_.fetch_add((uint64_t)n, std::memory_order_relaxed);
        }
        return i >= sbBase_;
    };
    auto fetch = [&](uint64_t i, int16_t* l, int16_t* r) -> bool {
        if (!ensure(i)) return false;
        const int16_t* p = sb_ + (size_t)(i - sbBase_) * 2;
        *l = p[0]; *r = p[1];
        return true;
    };
    int valid = 0;
    for (int i = 0; i < kBlockFrames; ++i) {
        if (outDone_ >= outTotal_) break;
        int16_t l, r;
        if (!rs_.next(fetch, &l, &r)) { outTotal_ = outDone_; break; }           // stream ended earlier than the probe said
        if (gain_ != target) {                                                  // 10 ms linear ramp: no click on mute/unmute
            const float step = 1.0f / 480.0f;
            gain_ = gain_ < target ? (gain_ + step > target ? target : gain_ + step) : (gain_ - step < target ? target : gain_ - step);
        }
        if (gain_ != 1.0f) { l = (int16_t)((float)l * gain_); r = (int16_t)((float)r * gain_); }
        out[2 * i] = l; out[2 * i + 1] = r;
        ++outDone_; ++valid;
    }
    if (valid < kBlockFrames) memset(out + (size_t)valid * 2, 0, (size_t)(kBlockFrames - valid) * 2 * sizeof(int16_t));
    return valid;
}

int MusicPlayer::summary_line(char* out, unsigned n, int curTrack, uint64_t posSamples, uint64_t lenSamples, bool mute) {
    if (!plays_.load() || !out || !n) return 0;
    auto X = [](std::atomic<uint64_t>& a) { return (unsigned long long)a.exchange(0); };
    const uint64_t tn = plat_->now_us();
    const uint64_t t0 = wStart_.exchange(tn);
    const double wall = t0 ? (double)(tn - t0) / 1e6 : 0.0;
    const unsigned long long dN = X(decN_), dUs = X(decUs_), dMax = X(decMax_), rN = X(rsN_), rUs = X(rsUs_), rMax = X(rsMax_);
    const unsigned long long qS = X(qSum_), qN = X(qN_), blocks = X(blocks_), play = X(playUs_);
    const unsigned long long qMin = qMin_.exchange(~0ull);
    const unsigned long long decWin = X(decodedFrames_);
    const double expBlocks = (double)play / (double)kBlockUs;
    int mc = -1, mp = 0, gc = -1; uint64_t cpu = 0;
    plat_->thread_probe(&mc, &mp, &gc, &cpu);
    const uint64_t pc = prevCpu_.exchange(cpu);
    const double rate = (double)cur_rate();
    return snprintf(out, n,
        "music track=%d pos=%.1f/%.1f s decoded=%llu(win %llu) delivered=%llu frames | win=%.1fs blocks=%llu(exp %.0f) underruns=%llu(total) porterr=%llu"
        " | decode n=%llu avg=%.2f max=%.2f ms | resample avg=%.3f max=%.2f ms | qdepth min=%lld avg=%lld frames | cpu=%.1f%% core_music=%d prio_music=0x%x core_game=%d"
        " | plays=%llu track_changes=%llu failures=%llu mute=%d",
        curTrack, rate > 0 ? (double)posSamples / rate : 0.0, rate > 0 ? (double)lenSamples / rate : 0.0, (unsigned long long)decodedTotal_.load(), decWin,
        (unsigned long long)deliveredTotal_.load(), wall, blocks, expBlocks, (unsigned long long)underruns_.load(), (unsigned long long)portErrors_.load(),
        dN, dN ? (double)dUs / (double)dN / 1000.0 : 0.0, (double)dMax / 1000.0, rN ? (double)rUs / (double)rN / 1000.0 : 0.0, (double)rMax / 1000.0,
        qN ? (long long)qMin : -1ll, qN ? (long long)(qS / qN) : -1ll,
        wall > 0 ? 100.0 * (double)(cpu - pc) / 1e6 / wall : 0.0, mc, (unsigned)mp, gc,
        (unsigned long long)plays_.load(), (unsigned long long)trackChanges_.load(), (unsigned long long)failures_.load(), mute ? 1 : 0);
}

}} // namespace ign::music
