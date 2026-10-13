// music/cd_toc.h -- the virtual CD of G132: CD-frame/MSF conversions, a cheap OGG header probe and the
// 8-track TOC. Pure logic: no Vita, no GL, no threads, no decoder -- everything here is unit-tested on the host.
//
// Why 8 tracks: the game clamps every requested track to `status cdaudio number of tracks` (0x4F3870); the
// results jingle is track 8, the level tracks are 2..7 and the front-end uses 7. Track 1 is the data track of the
// original mixed-mode CD: it exists in the TOC (so numbering matches the OGG file names) but has no audio.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ign { namespace music {

constexpr uint32_t kCdFps = 75;            // CD frames per second
constexpr int      kTracks = 8;            // 1 = data (virtual), 2..8 = Music/TrackNN.ogg
// Virtual data track: starts at 00:00:00 and lasts exactly 60 s. Start 0 matters: the game's TOC reader
// (0x455020) computes start[N+1] = [0x4F3874] + length, and on the FIRST read [0x4F3874] is still 0, so only
// start[1] == 0 makes the last track's length come out right on every read.
constexpr uint32_t kDataTrackFrames = 60 * kCdFps;

struct OggMeta {
    bool ok = false;
    uint32_t rate = 0;
    int channels = 0;
    uint64_t samples = 0;                  // per channel, from the last page's granule position
    uint64_t fileBytes = 0;
    std::string err;
};
// Reads the first page (Vorbis identification header) and the tail of the file (last page granule position).
// At most ~66 KB are read; nothing is decoded.
bool ogg_probe(const char* path, OggMeta* out);

// Sample count <-> CD frames. ceil in both directions so a track never loses its last samples.
uint32_t samples_to_frames_ceil(uint64_t samples, uint32_t rate);
uint64_t frames_to_samples_ceil(uint64_t frames, uint32_t rate);
std::string frames_to_msf(uint32_t frames);                 // "MM:SS:FF"
// "MM:SS:FF" (three ':'-separated decimal fields). false if malformed; *outOfRange = ss>=60 or ff>=75.
bool msf_to_frames(const char* s, uint32_t* frames, bool* outOfRange);

struct TrackInfo {
    int number = 0;
    bool data = false;
    std::string path;                      // empty for the data track
    uint32_t rate = 0;
    uint64_t samples = 0;                  // exact per-channel sample count (audio tracks)
    uint32_t startFrame = 0, lengthFrames = 0, endFrame = 0;    // endFrame exclusive
};
struct Toc {
    bool valid = false;
    TrackInfo t[kTracks + 1];              // [1..8]
    uint32_t totalFrames = 0;              // = endFrame of track 8 (start of track 1 is 0)
};
// Locates dir/TrackNN.ogg for N = 2..8 (dir ends with '/'), probes each and builds the TOC. Start frames come from
// the CUMULATIVE sample count rounded once (no per-track rounding drift: the TOC is never more than one frame away
// from the true running time). `log` receives one line per track and the TOC.
bool build_toc(const std::string& dir, Toc* toc, std::vector<std::string>* log, std::string* err);

}} // namespace ign::music
