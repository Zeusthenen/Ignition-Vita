// music/music_platform.h -- everything the music player needs from the platform. The Vita implementation lives in
// music_vita.cpp (second sceAudioOut port, thread placement); the host tests provide a recording implementation.
#pragma once
#include <cstdint>

namespace ign { namespace music {

constexpr int kOutRate = 48000;       // output port rate
constexpr int kBlockFrames = 1024;    // frames per port write (21.33 ms)

struct MusicPlatform {
    virtual ~MusicPlatform() {}
    virtual uint64_t now_us() = 0;
    // The music port. Never opened before the first `play` (open_cdaudio only prepares state).
    virtual int  out_open(int blockFrames, int rate) = 0;       // 0 = ok, negative = platform error code
    virtual int  out_write(const int16_t* stereo) = 0;          // blocks until the port accepts the block; <0 = error
    virtual void out_close(bool drain) = 0;
    // Called first thing on the music thread: affinity/priority/registration. Must not block.
    virtual void thread_enter() = 0;
    // Core/priority of the music thread and of the game thread, and the CPU time the music thread has used (us).
    virtual int  thread_probe(int* musicCore, int* musicPrio, int* gameCore, uint64_t* cpuUs) = 0;
    virtual void log(const char* line) = 0;                     // one-shot diagnostics sink
};

}} // namespace ign::music
