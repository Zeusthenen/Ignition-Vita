#include "music/ogg_source.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// stb_vorbis v1.22 (public domain / MIT), included unmodified as C++. Only the pull API with file input and the
// 16-bit conversion is used.
#define STB_VORBIS_NO_PUSHDATA_API 1
#define STB_VORBIS_NO_CRT_FREE_UNUSED 0
#include "../../third_party/stb_vorbis/stb_vorbis.c"

namespace ign { namespace music {

// Fixed decoder memory: setup memory + temp memory for the largest blocksize used by these streams. Measured by the
// host test (g132_test.cpp prints setup/temp requirements of every Track*.ogg); 1 MiB leaves a wide margin.
static constexpr uint32_t kDecoderBytes = 1024u * 1024u;

uint32_t OggSource::buffer_bytes() { return kDecoderBytes; }
static std::atomic<int> g_live{0};
int OggSource::live() { return g_live.load(); }

bool OggSource::open(const char* path, std::string* err) {
    close();
    FILE* f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open file"; return false; }
    setvbuf(f, nullptr, _IOFBF, 32 * 1024);                 // few large reads from the SD card instead of many tiny ones
    mem_ = (char*)malloc(kDecoderBytes);
    if (!mem_) { fclose(f); if (err) *err = "no memory for the decoder buffer"; return false; }
    stb_vorbis_alloc a; a.alloc_buffer = mem_; a.alloc_buffer_length_in_bytes = (int)kDecoderBytes;
    int e = 0;
    stb_vorbis* v = stb_vorbis_open_file(f, 1 /*close file with the decoder*/, &e, &a);
    if (!v) {
        // stb_vorbis closes `f` itself on failure only when it opened it; with open_file it has taken ownership.
        free(mem_); mem_ = nullptr;
        if (err) { char b[64]; snprintf(b, sizeof b, "stb_vorbis open error %d", e); *err = b; }
        return false;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    rate_ = info.sample_rate; channels_ = info.channels; decoded_ = 0;
    setupBytes_ = info.setup_memory_required; tempBytes_ = info.temp_memory_required;
    if (channels_ != 2) {
        stb_vorbis_close(v); free(mem_); mem_ = nullptr;
        if (err) *err = "only stereo streams are supported";
        return false;
    }
    v_ = v;
    g_live.fetch_add(1);
    return true;
}

int OggSource::read(int16_t* stereo, int maxFrames) {
    if (!v_ || maxFrames <= 0) return 0;
    const int n = stb_vorbis_get_samples_short_interleaved((stb_vorbis*)v_, 2, stereo, maxFrames * 2);
    if (n > 0) decoded_ += (uint64_t)n;
    return n;
}

void OggSource::close() {
    if (v_) { stb_vorbis_close((stb_vorbis*)v_); v_ = nullptr; g_live.fetch_sub(1); }     // also closes the FILE
    if (mem_) { free(mem_); mem_ = nullptr; }
}

}} // namespace ign::music
