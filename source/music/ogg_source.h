// music/ogg_source.h -- streaming Vorbis decoder (stb_vorbis, third_party/stb_vorbis). Decodes on demand into a
// caller buffer; never loads a whole file. All decoder memory comes from ONE fixed buffer handed to stb_vorbis
// (no alloca, no per-frame malloc), so the footprint is bounded and the music thread needs a small stack.
#pragma once
#include <cstdint>
#include <string>

namespace ign { namespace music {

class OggSource {
public:
    OggSource() {}
    ~OggSource() { close(); }
    // Opens `path` for streaming; only 44.1k-class stereo is accepted by the player but any rate/2ch opens.
    bool open(const char* path, std::string* err);
    // Interleaved stereo int16; returns frames written (0 = end of stream or error).
    int read(int16_t* stereo, int maxFrames);
    void close();
    bool is_open() const { return v_ != nullptr; }
    uint32_t rate() const { return rate_; }
    int channels() const { return channels_; }
    uint64_t decoded_frames() const { return decoded_; }
    static uint32_t buffer_bytes();       // size of the fixed decoder memory block
    static int live();                    // instances currently open (leak check in the host tests)
    // memory stb_vorbis reported needing for the stream just opened (setup, temp, max frame), for sizing checks
    uint32_t setup_bytes() const { return setupBytes_; }
    uint32_t temp_bytes() const { return tempBytes_; }
private:
    uint32_t setupBytes_ = 0, tempBytes_ = 0;
    void* v_ = nullptr;                   // stb_vorbis*
    char* mem_ = nullptr;
    uint32_t rate_ = 0; int channels_ = 0; uint64_t decoded_ = 0;
    OggSource(const OggSource&) = delete; OggSource& operator=(const OggSource&) = delete;
};

}} // namespace ign::music
