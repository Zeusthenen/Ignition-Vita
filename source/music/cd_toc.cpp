#include "music/cd_toc.h"

#include <cstdio>
#include <cstring>

namespace ign { namespace music {

namespace {
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
}

bool ogg_probe(const char* path, OggMeta* out) {
    *out = OggMeta();
    FILE* f = fopen(path, "rb");
    if (!f) { out->err = "cannot open"; return false; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    if (sz < 64) { fclose(f); out->err = "file too small"; return false; }
    out->fileBytes = (uint64_t)sz;
    // ---- first page: "OggS", ... segment table, then the Vorbis identification header
    uint8_t head[512];
    fseek(f, 0, SEEK_SET);
    const size_t nh = fread(head, 1, sizeof head, f);
    if (nh < 64 || memcmp(head, "OggS", 4) != 0) { fclose(f); out->err = "no OggS capture pattern"; return false; }
    const unsigned nseg = head[26];
    const size_t off = 27 + nseg;
    if (off + 30 > nh) { fclose(f); out->err = "first page too long"; return false; }
    if (memcmp(head + off, "\x01vorbis", 7) != 0) { fclose(f); out->err = "first packet is not a Vorbis identification header"; return false; }
    out->channels = head[off + 11];
    out->rate = rd32(head + off + 12);
    // ---- tail: the last page that carries a granule position
    const long tail = sz < 70000 ? sz : 70000;
    std::vector<uint8_t> buf((size_t)tail);
    fseek(f, sz - tail, SEEK_SET);
    const size_t nt = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    long best = -1;
    for (long i = (long)nt - 27; i >= 0; --i)
        if (buf[(size_t)i] == 'O' && memcmp(&buf[(size_t)i], "OggS", 4) == 0) { best = i; break; }
    if (best < 0) { out->err = "no Ogg page in the tail"; return false; }
    const int64_t gp = (int64_t)rd64(&buf[(size_t)best + 6]);
    if (gp <= 0) { out->err = "last page has no granule position"; return false; }
    out->samples = (uint64_t)gp;
    if (out->rate == 0 || out->channels <= 0) { out->err = "bad stream parameters"; return false; }
    out->ok = true;
    return true;
}

uint32_t samples_to_frames_ceil(uint64_t samples, uint32_t rate) {
    if (!rate) return 0;
    return (uint32_t)((samples * kCdFps + rate - 1) / rate);
}
uint64_t frames_to_samples_ceil(uint64_t frames, uint32_t rate) {
    return (frames * rate + kCdFps - 1) / kCdFps;
}
std::string frames_to_msf(uint32_t frames) {
    char b[24];
    snprintf(b, sizeof b, "%02u:%02u:%02u", frames / (60 * kCdFps), (frames / kCdFps) % 60, frames % kCdFps);
    return b;
}
bool msf_to_frames(const char* s, uint32_t* frames, bool* outOfRange) {
    if (outOfRange) *outOfRange = false;
    if (!s) return false;
    unsigned v[3] = {0, 0, 0};
    const char* p = s;
    for (int k = 0; k < 3; ++k) {
        if (*p < '0' || *p > '9') return false;
        unsigned x = 0; int digits = 0;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (unsigned)(*p - '0'); ++p; if (++digits > 4) return false; }
        v[k] = x;
        if (k < 2) { if (*p != ':') return false; ++p; }
    }
    if (*p != 0) return false;
    if (v[1] >= 60 || v[2] >= kCdFps) { if (outOfRange) *outOfRange = true; return false; }
    *frames = (v[0] * 60 + v[1]) * kCdFps + v[2];
    return true;
}

bool build_toc(const std::string& dir, Toc* toc, std::vector<std::string>* log, std::string* err) {
    *toc = Toc();
    char line[320];
    OggMeta m[kTracks + 1];
    for (int n = 2; n <= kTracks; ++n) {
        char name[32]; snprintf(name, sizeof name, "Track%02d.ogg", n);
        toc->t[n].number = n;
        toc->t[n].path = dir + name;
        if (!ogg_probe(toc->t[n].path.c_str(), &m[n])) {
            if (err) *err = std::string(name) + ": " + m[n].err;
            if (log) { snprintf(line, sizeof line, "track %d %s MISSING/INVALID: %s", n, toc->t[n].path.c_str(), m[n].err.c_str()); log->push_back(line); }
            return false;
        }
        if (m[n].channels != 2) {
            if (err) *err = std::string(name) + ": " + std::to_string(m[n].channels) + " channels (stereo required)";
            return false;
        }
        toc->t[n].rate = m[n].rate; toc->t[n].samples = m[n].samples;
        if (log) {
            snprintf(line, sizeof line, "track %d %s: vorbis %u Hz %d ch, %llu samples (%.2f s), %llu bytes", n, toc->t[n].path.c_str(),
                     m[n].rate, m[n].channels, (unsigned long long)m[n].samples, (double)m[n].samples / (double)m[n].rate,
                     (unsigned long long)m[n].fileBytes);
            log->push_back(line);
        }
    }
    // track 1: virtual data track
    toc->t[1].number = 1; toc->t[1].data = true; toc->t[1].startFrame = 0; toc->t[1].lengthFrames = kDataTrackFrames;
    toc->t[1].endFrame = kDataTrackFrames;
    // audio tracks: start = data frames + ceil(cumulative samples * 75 / rate) (one rounding of the running total)
    bool sameRate = true;
    for (int n = 3; n <= kTracks; ++n) if (toc->t[n].rate != toc->t[2].rate) sameRate = false;
    uint64_t cum = 0; uint32_t cursor = kDataTrackFrames;
    for (int n = 2; n <= kTracks; ++n) {
        TrackInfo& t = toc->t[n];
        t.startFrame = sameRate ? kDataTrackFrames + samples_to_frames_ceil(cum, t.rate) : cursor;
        cum += t.samples;
        t.endFrame = sameRate ? kDataTrackFrames + samples_to_frames_ceil(cum, t.rate)
                              : t.startFrame + samples_to_frames_ceil(t.samples, t.rate);
        if (t.endFrame <= t.startFrame) { if (err) *err = "empty track"; return false; }
        t.lengthFrames = t.endFrame - t.startFrame;
        cursor = t.endFrame;
    }
    toc->totalFrames = toc->t[kTracks].endFrame;
    toc->valid = true;
    if (log) {
        for (int n = 1; n <= kTracks; ++n) {
            snprintf(line, sizeof line, "TOC track %d start=%s (%u) length=%s (%u) end=%u%s", n, frames_to_msf(toc->t[n].startFrame).c_str(),
                     toc->t[n].startFrame, frames_to_msf(toc->t[n].lengthFrames).c_str(), toc->t[n].lengthFrames, toc->t[n].endFrame,
                     toc->t[n].data ? " (virtual data track, no audio)" : "");
            log->push_back(line);
        }
        snprintf(line, sizeof line, "TOC total length=%s (%u frames)", frames_to_msf(toc->totalFrames).c_str(), toc->totalFrames);
        log->push_back(line);
    }
    return true;
}

}} // namespace ign::music
