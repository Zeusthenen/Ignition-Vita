// iteration-G142: dynamic visual stability of the HUD / LFB overlay -- cheap accumulators, transition detection, source-vs-output checks and the verdict (pure: no GL, no guest access, host-testable).
//
// What the port draws as "the HUD" (G141 report): the guest writes it into the LFB shadow (RGB565, 640x400); the compositor keeps a layer of ACTIVE 16x16 tiles, converts them to the RGBA5551 overlay texture (alpha 0 = overlay key)
// and draws one NEAREST quad per active tile over the 3D scene. A visual defect that appears only in some zones of a lap can therefore come from three places, which this header lets the console tell apart:
//   SOURCE  : the LFB shadow already holds smoothed / duplicated / extra content (a writer family, stale accumulation, key collisions)         -> source_stats() against a baseline taken in the first frames of the race
//   OUTPUT  : the shadow is right but the frame buffer is not (blend, wrong texture, tile overlap)                                              -> output stats: read-back rows compared with the expected NEAREST colour of the shadow
//   NEITHER : the overlay is exactly the shadow and the shadow looks like the baseline -> the defect is in the 3D layer behind it or a perception of the frame rate.
// Everything per frame is a handful of integer operations (Rec, class word, three small state machines); the expensive parts (source stats ~0.3 ms, a few read-back rows) run only on a transition, a spike or a real pause.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ign::g142 {

constexpr uint32_t kRing = 64;                 // frame summaries kept for the pause marker (the last 60 are dumped)
constexpr uint32_t kHold = 4;                  // a new class must persist this many frames to become the stable class
constexpr uint32_t kCooldown = 12;
constexpr uint32_t kWarmFrames = 12;           // the first frames of a race (loading picture still in the shadow, key / layer settling) adopt their class silently             // minimum frames between two transition lines
constexpr uint32_t kBaseFrom = 8, kBaseTo = 120, kBaseSourceFrame = 60;
constexpr uint32_t kSpikeMinTiles = 500; constexpr float kSpikeFactor = 2.5f;
constexpr uint32_t kSpikeRun = 3, kSpikeEndRun = 3;
constexpr uint32_t kForceStreak = 3;           // consecutive frames of the armed race layer that are not clean before the accumulation guard may act
constexpr uint32_t kLineBudget = 26, kPauseLineBudget = 12;   // per race: transitions + spikes / pause marker (the two together stay <= 38)
constexpr int kTileW = 16, kTileH = 16, kCols = 40, kRows = 25, kTiles = kCols * kRows;
constexpr int kMenuX0 = 160, kMenuY0 = 150, kMenuX1 = 480, kMenuY1 = 250;   // the pause menu rectangle (G135 / G136), logical

// ---- per-frame record (16 bytes) ----
enum RecFlags : uint16_t { kFClean = 1, kFAlpha = 2, kFProv = 4, kFBlend = 8, kFCk = 16, kFMixedSafe = 32, kFContractBad = 64, kFValidGeom = 128, kFPipe = 256, kFForced = 512, kFSessionOpen = 1024, kFRaceFlag = 2048 };
struct Rec { uint32_t swap = 0; uint16_t tiles = 0; uint8_t cls = 0, writers = 0; uint16_t flags = 0, reject = 0; uint32_t mat = 0; };

inline uint32_t tile_bucket(uint32_t tiles) { return tiles < 300 ? 0u : tiles < 600 ? 1u : tiles < 900 ? 2u : 3u; }
// the "visual class" of a frame: what a user could perceive as a different state of the overlay
inline uint32_t class_word(const Rec& r) {
    return (uint32_t)(r.cls & 7) | ((uint32_t)(r.flags & (kFClean | kFAlpha | kFProv | kFBlend | kFCk | kFContractBad | kFValidGeom | kFPipe)) << 3) | (tile_bucket(r.tiles) << 11) | ((uint32_t)(r.writers > 7 ? 7 : r.writers) << 13);
}
// names of the parts of the class word that differ (comma separated); returns how many
inline uint32_t class_diff(uint32_t a, uint32_t b, char* out, size_t n) {
    static const struct { uint32_t mask; const char* name; } kParts[] = {
        {7u, "class"}, {kFClean << 3, "clean_layer"}, {kFAlpha << 3, "alpha_mode"}, {kFProv << 3, "key_provisional"}, {kFBlend << 3, "gl_blend"}, {kFCk << 3, "chromakey"},
        {kFContractBad << 3, "contract_bad"}, {kFValidGeom << 3, "valid_geometry"}, {kFPipe << 3, "pixel_pipeline_writer"}, {3u << 11, "tile_bucket"}, {7u << 13, "writer_count"}};
    uint32_t cnt = 0; size_t len = 0; if (n) out[0] = 0;
    for (const auto& p : kParts) {
        if ((a & p.mask) == (b & p.mask)) continue;
        ++cnt; const size_t l = strlen(p.name);
        if (len + l + 2 < n) { if (len) out[len++] = ','; memcpy(out + len, p.name, l); len += l; out[len] = 0; }
    }
    return cnt;
}
inline const char* cls_name(uint32_t c) {
    switch (c) { case 0: return "LFB_ONLY"; case 1: return "GL_ONLY"; case 2: return "GL_PLUS_UNCHANGED_LFB"; case 3: return "GL_PLUS_SPARSE_LFB"; case 4: return "GL_PLUS_FULLSCREEN_LFB"; case 5: return "UNKNOWN"; default: return "?"; }
}

// ---- LFB writer families seen in the current frame (lock callers; pixelPipeline / origin / write mode are the arguments the port used to ignore) ----
struct Writers {
    static constexpr uint32_t kMax = 8;
    uint32_t caller[kMax] = {}; uint16_t count[kMax] = {}; uint8_t pipe[kMax] = {}, origin[kMax] = {}, mode[kMax] = {};
    uint32_t n = 0, total = 0, overflow = 0;
    void reset() { n = 0; total = 0; overflow = 0; }
    void note(uint32_t c, uint32_t m, uint32_t o, uint32_t p) {
        ++total; uint32_t i = 0;
        for (; i < n; ++i) if (caller[i] == c) break;
        if (i == n) {
            if (n >= kMax) { ++overflow; return; }
            caller[n] = c; count[n] = 0; pipe[n] = 0; origin[n] = 0; mode[n] = 0; ++n;
        }
        if (count[i] < 65535) { ++count[i]; }
        if (p) { pipe[i] = 1; }
        if (o) { origin[i] = 1; }
        if (m) { mode[i] = 1; }
    }
    bool any_pipe() const { for (uint32_t i = 0; i < n; ++i) if (pipe[i]) return true; return false; }
    bool any_origin() const { for (uint32_t i = 0; i < n; ++i) if (origin[i]) return true; return false; }
};

// ---- stable-class transitions ----
class Transitions {
public:
    bool have() const { return have_; }
    uint32_t stable() const { return stable_; }
    void reset() { *this = Transitions(); }
    // feeds the class word of one frame; true when the STABLE class changed (the new class persisted kHold frames and kCooldown frames passed since the last one); *old receives the previous stable class
    bool feed(uint32_t cls, uint32_t* old, bool warm = false) {
        ++since_;
        if (warm) { have_ = true; stable_ = cand_ = cls; run_ = 0; since_ = kCooldown; return false; }
        if (!have_) { have_ = true; stable_ = cand_ = cls; run_ = 0; since_ = kCooldown; return false; }
        if (cls == stable_) { cand_ = stable_; run_ = 0; return false; }
        if (cls == cand_) { ++run_; } else { cand_ = cls; run_ = 1; }
        if (run_ >= kHold && since_ >= kCooldown) { if (old) *old = stable_; stable_ = cls; run_ = 0; since_ = 0; return true; }
        return false;
    }
private:
    bool have_ = false; uint32_t stable_ = 0, cand_ = 0, run_ = 0, since_ = kCooldown;
};

// ---- tile-count baseline and spikes ----
class Spike {
public:
    enum Ev : uint32_t { kNone = 0, kStart = 1, kEnd = 2 };
    void reset() { *this = Spike(); }
    uint32_t baseline() const { return n_ ? (uint32_t)(sum_ / n_) : 0; }
    bool baseline_known() const { return n_ >= 16; }
    uint32_t threshold() const { if (!baseline_known()) return 0; const uint32_t t = (uint32_t)(kSpikeFactor * (float)baseline()); return t > kSpikeMinTiles ? t : kSpikeMinTiles; }
    bool active() const { return in_; }
    uint32_t episodes() const { return episodes_; }
    uint32_t max_tiles() const { return max_; }
    uint32_t start_frame() const { return start_; }
    uint32_t feed(uint32_t frame, uint32_t tiles) {
        if (frame >= kBaseFrom && frame <= kBaseTo && !in_) { sum_ += tiles; ++n_; }
        const uint32_t thr = threshold(); if (!thr) return kNone;
        if (tiles >= thr) { ++hi_; lo_ = 0; if (tiles > max_) max_ = tiles; if (!in_ && hi_ >= kSpikeRun) { in_ = true; start_ = frame; ++episodes_; return kStart; } }
        else if (tiles * 10u < thr * 8u) { hi_ = 0; if (in_) { if (++lo_ >= kSpikeEndRun) { in_ = false; lo_ = 0; return kEnd; } } else { max_ = 0; } }
        return kNone;
    }
private:
    uint64_t sum_ = 0; uint32_t n_ = 0, hi_ = 0, lo_ = 0, max_ = 0, start_ = 0, episodes_ = 0; bool in_ = false;
};

// ---- the accumulation guard: the only action of G142, evidence-gated ----
// Signature of stale accumulation: the armed race layer is NOT clean for kForceStreak consecutive frames AND its tile count reached the spike threshold. From then on, every frame that is not clean gets the same
// end-of-frame layer clear that clean frames get (so the layer shows only what the guest wrote this frame); the latch drops as soon as a frame is clean or the race / scene ends.
class Gate {
public:
    void reset() { *this = Gate(); }
    uint32_t streak() const { return streak_; }
    bool latched() const { return latched_; }
    void update(bool raceHud, bool clean, uint32_t tiles, uint32_t spikeThreshold) {
        if (!raceHud || clean) { streak_ = 0; latched_ = false; return; }
        ++streak_;
        if (streak_ >= kForceStreak && spikeThreshold && tiles >= spikeThreshold) latched_ = true;
    }
    // true when the end-of-frame clear must also run on this (not clean) frame
    bool want_clear(bool raceHud, bool clean, bool keyOk) const { return raceHud && keyOk && !clean && latched_; }
private:
    uint32_t streak_ = 0; bool latched_ = false;
};

// ---- source: statistics of the LFB shadow restricted to the active tiles (menu tiles excluded by the caller through `use`) ----
struct SrcStats { uint32_t tiles = 0, visible = 0, collisions = 0, distinct = 0, soft = 0, hard = 0, hash = 0; };
inline void expand565(uint16_t v, int& r, int& g, int& b) { r = (v >> 11) & 31; r = (r << 3) | (r >> 2); g = (v >> 5) & 63; g = (g << 2) | (g >> 4); b = v & 31; b = (b << 3) | (b >> 2); }
inline int luma(int r, int g, int b) { return (r * 30 + g * 59 + b * 11) / 100; }
inline bool is_key(uint16_t v, uint16_t key, uint16_t mask) { return (uint16_t)(v & mask) == (uint16_t)(key & mask); }
// collisions = pixels that differ from the key but are equal to it under the mask (treated as transparent although they are content); soft / hard = horizontal neighbour pairs of different colour, luma step <= 24 / > 24
inline SrcStats source_stats(const uint8_t* sh, uint32_t stride, const bool* use, uint16_t key, uint16_t mask) {
    SrcStats s; uint8_t seen[8192]; memset(seen, 0, sizeof seen); uint32_t h = 2166136261u;
    for (int ty = 0; ty < kRows; ++ty) for (int tx = 0; tx < kCols; ++tx) {
        if (!use[ty * kCols + tx]) { continue; }
        ++s.tiles;
        for (int row = 0; row < kTileH; ++row) {
            const uint16_t* p = (const uint16_t*)(sh + (size_t)(ty * kTileH + row) * stride) + tx * kTileW;
            bool havePrev = false; uint16_t pv = 0;
            for (int px = 0; px < kTileW; ++px) {
                const uint16_t v = p[px];
                if (is_key(v, key, mask)) { if (v != key) ++s.collisions; havePrev = false; continue; }
                ++s.visible; if (!(seen[v >> 3] & (1u << (v & 7)))) { seen[v >> 3] |= (uint8_t)(1u << (v & 7)); ++s.distinct; }
                h = (h ^ v) * 16777619u;
                if (havePrev && v != pv) { int r1, g1, b1, r2, g2, b2; expand565(v, r1, g1, b1); expand565(pv, r2, g2, b2); const int d = luma(r1, g1, b1) - luma(r2, g2, b2); if ((d < 0 ? -d : d) <= 24) ++s.soft; else ++s.hard; }
                pv = v; havePrev = true;
            }
        }
    }
    s.hash = h; return s;
}
inline double soft_ratio(const SrcStats& s) { const uint32_t t = s.soft + s.hard; return t ? (double)s.soft / (double)t : 0.0; }

// ---- output: read-back rows (RGB8, row 0 = bottom, tightly packed) compared with the colour the NEAREST overlay must produce ----
struct OutStats { uint32_t mapped = 0, exact = 0, near_ = 0, far_ = 0, hash = 0; uint32_t worstTile = 0xFFFFFFFFu, worstFar = 0; uint32_t tileFar[kTiles] = {}; };
// the colour the overlay texture produces for an RGB565 shadow pixel: the RGB565 texture (fallback path) shows it as is, the RGBA5551 texture (alpha path, G115) quantises green to 5 bits
inline void expected_colour(uint16_t v, bool tex5551, int& r, int& g, int& b) {
    if (!tex5551) { expand565(v, r, g, b); return; }
    int r5 = (v >> 11) & 31, g5 = (((v >> 5) & 63) + 1) >> 1, b5 = v & 31; if (g5 > 31) { g5 = 31; }
    r = (r5 << 3) | (r5 >> 2); g = (g5 << 3) | (g5 >> 2); b = (b5 << 3) | (b5 >> 2);
}
inline bool in_menu(int ix, int iy) { return ix >= kMenuX0 && ix < kMenuX1 && iy >= kMenuY0 && iy < kMenuY1; }
// Pixels near a texel boundary (the sampler's rounding) and the pause menu are skipped; everything else inside an ACTIVE tile whose shadow pixel is not the key must equal the expanded RGB565 colour (tolerance 4 / 24).
inline void out_accumulate(OutStats& o, const uint8_t* rgb, int gx0, int gy0, int gw, int gh, const int vp[4], int Wl, int Hl, const uint8_t* sh, uint32_t stride, const bool* active, uint16_t key, uint16_t mask, bool tex5551 = false) {
    const float sx = Wl > 0 ? (float)vp[2] / (float)Wl : 1.0f, sy = Hl > 0 ? (float)vp[3] / (float)Hl : 1.0f;
    uint32_t h = o.hash ? o.hash : 2166136261u;
    for (int gy = 0; gy < gh; ++gy) for (int gx = 0; gx < gw; ++gx) {
        const float lx = ((float)(gx0 + gx) + 0.5f - (float)vp[0]) / sx, ly = ((float)vp[3] - ((float)(gy0 + gy) + 0.5f - (float)vp[1])) / sy;
        const int ix = (int)std::floor(lx), iy = (int)std::floor(ly);
        if (ix < 0 || ix >= Wl || iy < 0 || iy >= Hl || in_menu(ix, iy)) continue;
        const float fx = lx - (float)ix, fy = ly - (float)iy;
        if (fx < 0.08f || fx > 0.92f || fy < 0.08f || fy > 0.92f) continue;
        const int t = (iy / kTileH) * kCols + (ix / kTileW);
        if (t < 0 || t >= kTiles || !active[t]) continue;
        const uint16_t v = ((const uint16_t*)(sh + (size_t)iy * stride))[ix];
        if (is_key(v, key, mask)) continue;
        const uint8_t* px = rgb + ((size_t)gy * gw + gx) * 3;
        int er, eg, eb; expected_colour(v, tex5551, er, eg, eb);
        const int dr = std::abs((int)px[0] - er), dg = std::abs((int)px[1] - eg), db = std::abs((int)px[2] - eb);
        const int dm = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
        ++o.mapped; if (dm <= 4) ++o.exact; else if (dm <= 24) ++o.near_; else { ++o.far_; if (++o.tileFar[t] > o.worstFar) { o.worstFar = o.tileFar[t]; o.worstTile = (uint32_t)t; } }
        h = (h ^ px[0]) * 16777619u; h = (h ^ px[1]) * 16777619u; h = (h ^ px[2]) * 16777619u;
    }
    o.hash = h;
}

// ---- verdict ----
enum Verdict : uint32_t { kNotEnough = 0, kNoDefect = 1, kSource = 2, kOutput = 3, kBoth = 4 };
inline const char* verdict_name(uint32_t v) { switch (v) { case kNoDefect: return "NOT_REPRODUCED_IN_OVERLAY(check the 3D layer / frame rate)"; case kSource: return "SOURCE_ALTERED(LFB shadow content)"; case kOutput: return "OUTPUT_ALTERED(compositor)"; case kBoth: return "SOURCE_AND_OUTPUT"; default: return "NOT_ENOUGH_DATA"; } }
struct Baseline { bool have = false; SrcStats src; uint32_t tiles = 0; };
inline bool source_altered(const Baseline& b, const SrcStats& cur, uint32_t tilesNow, uint32_t spikeThr) {
    if (!b.have) return false;
    if (spikeThr && tilesNow >= spikeThr) return true;
    const uint32_t bd = b.src.distinct < 48 ? 48 : b.src.distinct;
    if (cur.distinct > 3 * bd) return true;
    if (cur.visible > 200 && soft_ratio(cur) > soft_ratio(b.src) + 0.20) return true;
    if (cur.visible > 200 && cur.collisions * 50u > cur.visible) return true;
    return false;
}
inline bool output_altered(const OutStats& o) { return o.mapped >= 400 && (o.near_ + o.far_) * 10u > o.mapped; }
inline uint32_t decide(const Baseline& b, const SrcStats& cur, const OutStats& out, uint32_t tilesNow, uint32_t spikeThr) {
    if (!b.have || out.mapped < 400) return kNotEnough;
    const bool s = source_altered(b, cur, tilesNow, spikeThr), o = output_altered(out);
    return s && o ? kBoth : s ? kSource : o ? kOutput : kNoDefect;
}

// ---- the ring: transitions between consecutive frame summaries of the last frames, oldest first ----
struct RingTrans { uint32_t index, swap, from, to; };
inline uint32_t ring_transitions(const Rec* ring, uint32_t n, uint32_t head, RingTrans* out, uint32_t maxOut) {
    // ring[(head - n + i) % kRing], i = 0..n-1 (head = next write position)
    uint32_t k = 0; uint32_t prev = 0; bool have = false;
    for (uint32_t i = 0; i < n; ++i) {
        const Rec& r = ring[(head + kRing - n + i) % kRing]; const uint32_t c = class_word(r);
        if (have && c != prev && k < maxOut) { out[k++] = {i, r.swap, prev, c}; }
        prev = c; have = true;
    }
    return k;
}

// ---- the track of a race: what the guest says and what the files say ----
inline const char* visible_name(uint32_t idx) { static const char* const n[7] = {"MOOSEJAW FALLS", "GOLD RUSH", "SNAKE ISLAND", "LOST RUINS", "YODEL PEAKS", "CAPE THOR", "TOKYO BULLET"}; return idx < 7 ? n[idx] : "?"; }
inline const char* internal_dir(uint32_t idx) { static const char* const n[7] = {"CANADA", "USA", "CARIB", "BRAZIL", "AUSTRIA", "ICELAND", "JAPAN"}; return idx < 7 ? n[idx] : "?"; }
inline bool dir_matches(uint32_t idx, const char* dirFromFiles) {
    if (idx >= 7 || !dirFromFiles) { return false; }
    const char* a = internal_dir(idx);
    for (; *a && *dirFromFiles; ++a, ++dirFromFiles) { char x = *a, y = *dirFromFiles; if (y >= 'a' && y <= 'z') y = (char)(y - 32); if (x != y) return false; }
    return *a == 0 && *dirFromFiles == 0;
}

}  // namespace ign::g142
