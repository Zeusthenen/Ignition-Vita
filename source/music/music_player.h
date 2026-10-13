// music/music_player.h -- the music stream: one thread that opens the OGG, decodes it in small chunks, resamples
// 44100 -> 48000 with a persistent fractional phase and writes 1024-frame blocks to the platform's music port.
//
// The guest (mciSendStringA) only POSTS commands (play/stop/mute) under a short mutex and returns; it never decodes,
// opens a file, waits for the port or touches the thread. Commands are last-wins and are looked at between blocks
// (<= one 21 ms block of latency). The thread owns the decoder, the resampler and the port.
#pragma once
#include "music/music_platform.h"
#include "music/ogg_source.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace ign { namespace music {

struct PlayRequest {
    int track = 0;
    std::string path;
    uint32_t srcRate = 44100;
    uint64_t endSample = 0;              // stop after this many source samples (<= track samples)
};

// Exact source-position accounting: output frames delivered -> source samples (floor, no drift).
inline uint64_t out_frames_to_src_samples(uint64_t outFrames, uint32_t srcRate) { return outFrames * srcRate / (uint64_t)kOutRate; }
inline uint64_t src_samples_to_out_frames(uint64_t src, uint32_t srcRate) { return src * (uint64_t)kOutRate / srcRate; }

// Continuous linear resampler core (also used by the host tests directly). Position = idx + frac/kOutRate in source
// samples; each output frame advances frac by srcRate (integer arithmetic: no accumulated error, no phase reset
// between blocks). `fetch(i, l, r)` must return false only past the end of the stream.
struct Resampler {
    uint32_t srcRate = 44100;
    uint64_t idx = 0;                    // integer source index of the left neighbour
    uint32_t frac = 0;                   // numerator over kOutRate
    void reset(uint32_t rate) { srcRate = rate; idx = 0; frac = 0; }
    template <class Fetch>
    bool next(Fetch&& fetch, int16_t* l, int16_t* r) {
        int16_t l0, r0, l1, r1;
        if (!fetch(idx, &l0, &r0)) return false;
        if (!fetch(idx + 1, &l1, &r1)) { l1 = l0; r1 = r0; }          // hold the last sample at the very end
        *l = (int16_t)(l0 + (int32_t)(((int64_t)(l1 - l0) * frac) / kOutRate));
        *r = (int16_t)(r0 + (int32_t)(((int64_t)(r1 - r0) * frac) / kOutRate));
        frac += srcRate;
        while (frac >= (uint32_t)kOutRate) { frac -= (uint32_t)kOutRate; ++idx; }
        return true;
    }
};

class MusicPlayer {
public:
    enum Run { kIdle = 0, kStarting, kPlaying, kEnded, kFailed };

    explicit MusicPlayer(MusicPlatform* plat) : plat_(plat) {}
    ~MusicPlayer() { shutdown(); }

    // ---- commands (guest thread; never block) ----
    bool post_play(const PlayRequest& r);        // false: the thread could not be started
    void post_stop();
    void set_mute(bool m) { muteTarget_.store(m, std::memory_order_relaxed); }
    // Stops, joins the thread, closes the port. Idempotent. May block for <= ~25 ms (one block).
    void shutdown();

    // ---- state (any thread) ----
    Run run() const { return (Run)run_.load(std::memory_order_acquire); }
    bool backend_failed() const { return backendFailed_.load(std::memory_order_acquire); }
    uint64_t delivered_out_frames() const { return deliveredOut_.load(std::memory_order_relaxed); }
    uint32_t cur_rate() const { return curRate_.load(std::memory_order_relaxed); }
    std::string last_error();

    // One compact line for the window since the previous call (decode/resample timing, queue depth, underruns...).
    // Returns 0 if the player never played.
    int summary_line(char* out, unsigned n, int curTrack, uint64_t posSamples, uint64_t lenSamples, bool mute);

private:
    static void* thread_tramp(void* self);
    void thread_main();
    void stream(const PlayRequest& req, uint32_t seq);
    int fill_block(int16_t* out);                // builds one block; returns the valid frames (< kBlockFrames = stream finished; the rest is zero)

    MusicPlatform* plat_;
    std::mutex mu_;
    std::condition_variable cv_;
    struct Cmd { int kind = 0; PlayRequest req; uint32_t seq = 0; } cmd_;   // kind: 1 play, 2 stop
    bool shutdown_ = false;
    bool threadUp_ = false;
    void* thr_ = nullptr;                        // pthread_t storage (opaque here)
    std::atomic<uint32_t> seq_{0};
    std::atomic<int> run_{kIdle};
    std::atomic<bool> backendFailed_{false};
    std::atomic<bool> muteTarget_{false};
    std::string lastErr_;

    // ---- stream state (music thread only) ----
    OggSource src_;
    Resampler rs_;
    int16_t* sb_ = nullptr;                      // source window (interleaved stereo)
    uint64_t sbBase_ = 0; uint32_t sbN_ = 0; bool srcEof_ = false;
    uint64_t outDone_ = 0, outTotal_ = 0, decBlockUs_ = 0;
    float gain_ = 1.0f;
    bool portOpen_ = false;

    // ---- counters ----
    std::atomic<uint64_t> deliveredOut_{0};      // output frames returned by the port for the current play
    std::atomic<uint32_t> curRate_{44100};
    std::atomic<uint64_t> decodedFrames_{0}, decodedTotal_{0}, deliveredTotal_{0}, blocks_{0}, underruns_{0}, portErrors_{0};
    std::atomic<uint64_t> decN_{0}, decUs_{0}, decMax_{0}, rsN_{0}, rsUs_{0}, rsMax_{0}, qSum_{0}, qN_{0}, qMin_{~0ull}, playUs_{0};
    std::atomic<uint64_t> trackChanges_{0}, plays_{0}, failures_{0}, wStart_{0}, prevCpu_{0};
    bool everPlayed_ = false;
};

}} // namespace ign::music
