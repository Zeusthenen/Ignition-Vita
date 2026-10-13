// iteration-G143: HUD REGION LIFETIME -- pure, dependency-free logic (no GL, no guest access, host-testable): the region partition of the 40x25 tile grid, the per-region state, the mode machine (race / pause / other),
// the LFB-writer rectangles decoded from the guest stack, the key-vs-palette collision guard, the regional scrub tile set and the CPU-side source / submitted-texel checks.
//
// What the guest does (static analysis of Ign_3dfx.exe, tmp/G143_WRITERS_AND_REGIONS.txt): the race HUD is NOT drawn with textures. Sprites and text are drawn into an 8-bit canvas ([0x00547C40]) and the canvas is copied to the LFB
// in RECTANGLES by 0x004451A0 (palette blit: index <= 5 is skipped, 6..15 use 0x00488FBC, >= 16 use 0x0061E520): top strip rows 8..37 of the canvas -> (160,0) 320 wide (0x0043955E), bottom strip (0x004395E8), the pause menu
// 320x100 at (160,150) and the yes/no dialog 320x34 at (160,185) (0x004372C6 / 0x00437494). Text and sprites go straight to the LFB through the sprite engine 0x0045B821 (lock 0x00454A0F). The speed text is drawn at
// (viewport right - 44, viewport bottom - 29) for the normal HUD set (-88 / -56 for *_h) from the layout tables 0x00488448 / 0x00488468 / 0x00488488. The whole HUD is redrawn every frame ([0x00533858] is only the player's HUD on/off toggle).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "g113_overlay.h"

namespace ign { namespace g143 {

constexpr int kCols = 40, kRows = 25, kTiles = kCols * kRows, kTileW = 16, kTileH = 16, kWidth = 640, kHeight = 400;

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Regions: a GEOMETRIC partition of the tile grid (every tile belongs to exactly one region). The rectangles that come from decoded guest code are marked; the rest is a partition by screen zone (the instrument measures the real
// bounding boxes of each region at run time, see RegionFrame, so the next phase has exact numbers).
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum Region : uint8_t { kTop = 0, kLeft, kRight, kBottomRight, kBottom, kPauseWindow, kCenter, kRegionCount };
inline const char* region_name(uint32_t r) {
    static const char* const n[kRegionCount] = {"TOP", "LEFT", "RIGHT", "BOTTOM_RIGHT", "BOTTOM", "PAUSE_WINDOW", "CENTER"};
    return r < kRegionCount ? n[r] : "FULLSCREEN";
}
// tile rectangles (inclusive columns / rows)
constexpr int kTopRows = 3;                                            // rows 0..2 (y < 48): the top strip (160,0,320x29) + indicators
constexpr int kBrCol0 = 28, kBrRow0 = 19;                              // BOTTOM_RIGHT: x >= 448, y >= 304 (speed text origin (596,371) normal / (552,344) *_h, gears)
constexpr int kPwCol0 = 10, kPwCol1 = 29, kPwRow0 = 8, kPwRow1 = 16;   // PAUSE_WINDOW: x 160..479, y 128..271 (menu 160,150 320x100; dialog 160,185 320x34, decoded)
constexpr int kBottomRow0 = 22;                                        // BOTTOM: y >= 352 (bottom strip 640x16 at y 384, or 320x8 at y 392)
constexpr int kLeftCols = 10, kRightCol0 = 30;                         // LEFT: x < 160 ; RIGHT: x >= 480
inline uint32_t region_of_tile(int tx, int ty) {
    if (ty < kTopRows) return kTop;
    if (tx >= kBrCol0 && ty >= kBrRow0) return kBottomRight;
    if (tx >= kPwCol0 && tx <= kPwCol1 && ty >= kPwRow0 && ty <= kPwRow1) return kPauseWindow;
    if (ty >= kBottomRow0) return kBottom;
    if (tx < kLeftCols) return kLeft;
    if (tx >= kRightCol0) return kRight;
    return kCenter;
}
struct RegionMap {
    uint8_t of[kTiles]; uint16_t count[kRegionCount];
    RegionMap() { memset(count, 0, sizeof count); for (int ty = 0; ty < kRows; ++ty) for (int tx = 0; tx < kCols; ++tx) { const uint32_t r = region_of_tile(tx, ty); of[ty * kCols + tx] = (uint8_t)r; ++count[r]; } }
};
inline const RegionMap& region_map() { static const RegionMap m; return m; }

// what one frame did in each region (counted from the masks the compositor already maintains)
struct RegionFrame {
    uint16_t active = 0, changed = 0, occupied = 0; int8_t minCol = -1, maxCol = -1, minRow = -1, maxRow = -1;
    uint16_t kept = 0, added = 0, removed = 0;   // against the previous frame's active tiles: kept (still drawn), added (new this frame), removed (no longer drawn)
};
inline void accumulate_regions(const bool* active, const bool* changed, const bool* occupied, RegionFrame out[kRegionCount], const bool* prevActive = nullptr) {
    const RegionMap& m = region_map();
    for (int r = 0; r < kRegionCount; ++r) out[r] = RegionFrame();
    for (int i = 0; i < kTiles; ++i) {
        RegionFrame& f = out[m.of[i]];
        const bool a = active[i];
        if (changed && changed[i]) ++f.changed;
        if (occupied && occupied[i]) ++f.occupied;
        if (prevActive) { const bool p = prevActive[i]; if (a && p) ++f.kept; else if (a) ++f.added; else if (p) ++f.removed; }
        if (!a) continue;
        ++f.active; const int tx = i % kCols, ty = i / kCols;
        if (f.minCol < 0 || tx < f.minCol) f.minCol = (int8_t)tx;
        if (tx > f.maxCol) f.maxCol = (int8_t)tx;
        if (f.minRow < 0 || ty < f.minRow) f.minRow = (int8_t)ty;
        if (ty > f.maxRow) f.maxRow = (int8_t)ty;
    }
}
inline uint32_t total_active(const RegionFrame f[kRegionCount]) { uint32_t t = 0; for (int r = 0; r < kRegionCount; ++r) t += f[r].active; return t; }
inline uint32_t total_occupied(const RegionFrame f[kRegionCount]) { uint32_t t = 0; for (int r = 0; r < kRegionCount; ++r) t += f[r].occupied; return t; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Per-region state of the race HUD (independent of every other region): lifetime counters, valid rectangle, need for clearing, last real write.
// A speed-digit update only touches BOTTOM_RIGHT; nothing here is shared between regions.
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum AnomalyKind : uint8_t { kAnNotClean = 0, kAnTileSpike = 1, kAnFullScreen = 2, kAnKinds = 3 };
inline const char* anomaly_name(uint32_t k) { static const char* const n[kAnKinds] = {"LAYER_NOT_CLEAN", "REGION_TILE_SPIKE", "FULLSCREEN_CONTENT_IN_RACE"}; return k < kAnKinds ? n[k] : "?"; }
constexpr uint32_t kBaseFrom = 8, kBaseTo = 60;       // baseline window (hud frames): the region's largest active-tile count there is its normal size
constexpr uint32_t kWarmFrames = 12;                   // the first frames of a race (loading picture still in the shadow, layer settling) raise no anomaly
constexpr uint32_t kFullScreenTiles = 700;             // occupied tiles of one race frame that can only be a full-screen picture / iris / transition

struct RegionState {
    uint32_t generation = 0;      // frames in which a tile of this region changed (the region's update counter)
    uint32_t lastWriteFrame = 0;  // last hud frame in which the region was provably written by the guest (clean frame + occupied tile)
    uint32_t lifetime = 0;        // consecutive frames with active tiles (resets when the region empties)
    uint16_t active = 0, changed = 0, occupied = 0; int8_t minCol = -1, maxCol = -1, minRow = -1, maxRow = -1;
    uint16_t kept = 0, added = 0, removed = 0;
    uint16_t baseMax = 0; bool baseKnown = false;
    bool needsClear = false;      // the region holds guest content that the next sparse clear must remove
    uint8_t reported = 0;         // anomaly kinds already reported for this region in this race
    uint32_t anomalies = 0;
    bool valid_rect() const { return active > 0 && minCol >= 0; }
    int rect_x0() const { return minCol * kTileW; }  int rect_y0() const { return minRow * kTileH; }
    int rect_x1() const { return (maxCol + 1) * kTileW; }  int rect_y1() const { return (maxRow + 1) * kTileH; }
};

class RegionTracker {
public:
    void reset() { *this = RegionTracker(); }
    const RegionState& st(uint32_t r) const { return s_[r < kRegionCount ? r : 0]; }
    uint32_t hud_frame() const { return frame_; }
    // one race HUD frame. `clean` = the layer was provably the key + this frame's writes. Returns a bit mask of (1 << region) per anomaly kind in `outMask[k]` (first occurrence per race only).
    void feed(bool clean, bool scanned, const RegionFrame f[kRegionCount], uint32_t outMask[kAnKinds]) {
        ++frame_;
        for (int k = 0; k < kAnKinds; ++k) outMask[k] = 0;
        fullScreen_ = false;
        for (int r = 0; r < kRegionCount; ++r) {
            RegionState& s = s_[r]; const RegionFrame& x = f[r];
            s.active = x.active; s.changed = x.changed; s.occupied = x.occupied; s.minCol = x.minCol; s.maxCol = x.maxCol; s.minRow = x.minRow; s.maxRow = x.maxRow; s.kept = x.kept; s.added = x.added; s.removed = x.removed;
            if (x.changed) ++s.generation;
            if (x.active) ++s.lifetime; else s.lifetime = 0;
            if (clean && scanned && x.occupied) s.lastWriteFrame = frame_;
            s.needsClear = x.occupied > 0;
            if (frame_ >= kBaseFrom && frame_ <= kBaseTo) { if (x.active > s.baseMax) s.baseMax = x.active; }
            if (frame_ == kBaseTo) s.baseKnown = true;
            if (frame_ <= kWarmFrames) continue;
            if (!clean && x.active && !(s.reported & (1u << kAnNotClean))) { s.reported |= (uint8_t)(1u << kAnNotClean); outMask[kAnNotClean] |= 1u << r; ++s.anomalies; }
            if (s.baseKnown && x.active > (uint32_t)(2u * s.baseMax + 8u) && !(s.reported & (1u << kAnTileSpike))) { s.reported |= (uint8_t)(1u << kAnTileSpike); outMask[kAnTileSpike] |= 1u << r; ++s.anomalies; }
        }
        if (frame_ > kWarmFrames && total_occupied(f) >= kFullScreenTiles) {
            fullScreen_ = true;
            if (!fullReported_) { fullReported_ = true; outMask[kAnFullScreen] |= 1u << kCenter; }
        }
    }
    bool fullscreen_now() const { return fullScreen_; }
private:
    RegionState s_[kRegionCount]; uint32_t frame_ = 0; bool fullScreen_ = false, fullReported_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Mode machine: what the guest is doing, from facts (the race flag [0x00537180], the pause condition of G136) -- never from tile counts.
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum Mode : uint8_t { kModeNone = 0, kModeRace = 1, kModePause = 2, kModeOther = 3 };
inline const char* mode_name(uint32_t m) { switch (m) { case kModeRace: return "RACE"; case kModePause: return "PAUSE"; case kModeOther: return "OTHER(menu/loading/results)"; default: return "NONE"; } }
inline uint32_t mode_of(bool raceScene, bool flagTrusted, uint32_t raceFlag, bool paused) {
    if (!raceScene) return kModeNone;
    if (paused) return kModePause;
    if (flagTrusted && raceFlag != 1u) return kModeOther;
    return kModeRace;
}
// boundaries that must not leave anything of the previous mode behind: every change that has the race or the pause on one side
inline bool edge_needs_clear(uint32_t from, uint32_t to) {
    if (from == to) return false;
    const bool a = (from == kModeRace || from == kModePause), b = (to == kModeRace || to == kModePause);
    return a || b;
}
class ModeTracker {
public:
    uint32_t mode() const { return mode_; }
    uint32_t frames_in_mode() const { return n_; }
    void reset() { *this = ModeTracker(); }
    // returns true when the mode changed (the previous mode is in *from)
    bool feed(uint32_t m, uint32_t* from) { if (m == mode_) { ++n_; return false; } if (from) *from = mode_; mode_ = (uint8_t)m; n_ = 1; ++changes_; return true; }
    uint32_t changes() const { return changes_; }
private:
    uint8_t mode_ = kModeNone; uint32_t n_ = 0, changes_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The G142 recovery clear (kept, now SELF-VERIFIED per region) and its regional fallback.
// What G142 really did: on a frame the compositor did not clean, once the layer had been not clean for kForceStreak frames AND had spiked, it cleared the WHOLE layer at the swap end, so the next frame was clean again
// (alpha path: key pixels transparent) instead of the fallback path (whole 16x16 tiles opaque, key-grey boxes + whatever stale pixels). The alpha path is a property of the whole layer: a regional clear cannot restore it.
// So the clear stays whole-layer, but after it the regions are measured: if a region other than BOTTOM_RIGHT lost tiles or visible pixels (the G142 regression of the top indicators), the clear is DISABLED for the
// rest of the race and only the BOTTOM_RIGHT tiles are scrubbed (the regional fallback). BOTTOM_RIGHT is the region the guard was shown to help; it is not judged by the check.
// ---------------------------------------------------------------------------------------------------------------------------------------------
inline void scrub_tiles(bool* mask) { const RegionMap& m = region_map(); for (int i = 0; i < kTiles; ++i) mask[i] = (m.of[i] == kBottomRight); }
// a region (never BOTTOM_RIGHT) regressed after the recovery clear: compared with its own race baseline (frames 8..60, 0 = not known yet) it lost >= 25 % of its active tiles or >= 30 % of its visible pixels
inline bool region_regressed(uint32_t baseActive, uint32_t nowActive, uint32_t baseVisible, uint32_t nowVisible) {
    if (baseActive >= 8u && nowActive * 4u < baseActive * 3u) return true;
    if (baseVisible >= 200u && nowVisible * 10u < baseVisible * 7u) return true;
    return false;
}
constexpr uint32_t kGuardCheckWindow = 30;   // frames after a recovery clear in which the first clean frame is awaited

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The LFB writers: the rectangle each canvas blit (0x004451A0) writes, read from the guest stack at the lock (the lock helper 0x00445160 is called from 0x004451A2; args of the blit at S + 0x40 + 4k).
// ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kRetBlit = 0x004451A7u;    // return address of the lock helper inside 0x004451A0
constexpr uint32_t kRetIris = 0x00436C6Cu;    // ... inside the iris / mask routine 0x00436C10 (writes black where the mask byte is 0x14)
constexpr uint32_t kRetFrontIris = 0x00406472u;  // ... inside the front-end iris 0x00406450 (lock 0x004065BF)
constexpr uint32_t kRetSprite = 0x004549C9u;  // return address of the sprite-engine lock helper 0x004549F0 (lock 0x00454A0F)
constexpr uint32_t kCallerBlit = 0x0044517Fu, kCallerSprite = 0x00454A0Fu, kCallerFrontIris = 0x004065BFu, kCallerFullPicture = 0x00404F07u;
enum Writer : uint8_t { kWBlit = 0, kWIris, kWSprite, kWFrontIris, kWFullPicture, kWOther, kWriterKinds };
inline const char* writer_name(uint32_t w) { static const char* const n[kWriterKinds] = {"CANVAS_BLIT(0x4451A0)", "IRIS_MASK(0x436C10)", "SPRITE_ENGINE(0x454A0F)", "FRONT_END_IRIS(0x4065BF)", "FULL_PICTURE(0x404F07)", "OTHER"}; return w < kWriterKinds ? n[w] : "?"; }
inline uint32_t classify_writer(uint32_t lockCaller, uint32_t retOfHelper) {
    if (lockCaller == kCallerBlit) return retOfHelper == kRetBlit ? kWBlit : retOfHelper == kRetIris ? kWIris : kWOther;
    if (lockCaller == kCallerSprite) return kWSprite;
    if (lockCaller == kCallerFrontIris) return kWFrontIris;
    if (lockCaller == kCallerFullPicture) return kWFullPicture;
    return kWOther;
}
struct Blit { int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0, dy = 0, pitch = 0; bool valid = false; };
// a[k] = the k-th dword argument of 0x004451A0: 0 source base, 1 x0, 2 y0, 3 x1, 4 y1, 6 destination x, 7 destination y, 9 source pitch
inline Blit decode_blit(const uint32_t a[10]) {
    Blit b; b.x0 = (int32_t)a[1]; b.y0 = (int32_t)a[2]; b.x1 = (int32_t)a[3]; b.y1 = (int32_t)a[4]; b.dx = (int32_t)a[6]; b.dy = (int32_t)a[7]; b.pitch = (int32_t)a[9];
    const int32_t w = b.x1 - b.x0, h = b.y1 - b.y0;
    b.valid = w > 0 && h > 0 && w <= kWidth && h <= kHeight && b.dx > -kWidth && b.dx < kWidth && b.dy > -kHeight && b.dy < kHeight && b.pitch > 0 && b.pitch <= 2048;
    return b;
}
// destination rectangle clipped to the logical screen
inline bool blit_dest(const Blit& b, int& rx0, int& ry0, int& rx1, int& ry1) {
    if (!b.valid) return false;
    rx0 = b.dx; ry0 = b.dy; rx1 = b.dx + (b.x1 - b.x0); ry1 = b.dy + (b.y1 - b.y0);
    if (rx0 < 0) { rx0 = 0; }
    if (ry0 < 0) { ry0 = 0; }
    if (rx1 > kWidth) { rx1 = kWidth; }
    if (ry1 > kHeight) { ry1 = kHeight; }
    return rx1 > rx0 && ry1 > ry0;
}
struct RectLog {
    static constexpr uint32_t kMax = 8;
    int16_t x0[kMax] = {}, y0[kMax] = {}, x1[kMax] = {}, y1[kMax] = {}; uint16_t count[kMax] = {}; uint32_t n = 0, overflow = 0;
    void reset() { n = 0; overflow = 0; }
    void note(int rx0, int ry0, int rx1, int ry1) {
        for (uint32_t i = 0; i < n; ++i) if (x0[i] == rx0 && y0[i] == ry0 && x1[i] == rx1 && y1[i] == ry1) { if (count[i] < 65535) ++count[i]; return; }
        if (n >= kMax) { ++overflow; return; }
        x0[n] = (int16_t)rx0; y0[n] = (int16_t)ry0; x1[n] = (int16_t)rx1; y1[n] = (int16_t)ry1; count[n] = 1; ++n;
    }
};
// the region that holds the centre of a rectangle
inline uint32_t region_of_rect(int rx0, int ry0, int rx1, int ry1) {
    int cx = (rx0 + rx1) / 2, cy = (ry0 + ry1) / 2;
    if (cx < 0) { cx = 0; }
    if (cy < 0) { cy = 0; }
    if (cx >= kWidth) { cx = kWidth - 1; }
    if (cy >= kHeight) { cy = kHeight - 1; }
    return region_of_tile(cx / kTileW, cy / kTileH);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Key vs the guest's palettes. Every pixel the guest writes into the LFB is a palette entry (0x0061E520 for index >= 16, 0x00488FBC / 0x0048EFA8 for 6..15) or black (the iris): a key that does not collide with any of them
// (tolerance of the overlay: bit 0 ignored) can never make content transparent. The check is evidence-gated: the key is only changed when a palette entry PROVES a collision.
// ---------------------------------------------------------------------------------------------------------------------------------------------
inline bool key_collides(uint16_t v, uint16_t key) { return (uint16_t)((v ^ key) & ign::g113::kKeyMask) == 0; }
struct PaletteScan { uint32_t entries = 0, collisions = 0; uint16_t value[4] = {}; uint16_t index[4] = {}; uint32_t sum = 0; };
// pal256: 256 RGB565 entries at 0x0061E520 (entries 0..5 are never written: skipped); a / b: 16 entries at 0x00488FBC / 0x0048EFA8 (entries 6..15 are used)
inline PaletteScan scan_palette(const uint16_t* pal256, const uint16_t* a, const uint16_t* b, uint16_t key) {
    PaletteScan s;
    auto one = [&](uint16_t v, uint16_t idx) { ++s.entries; s.sum = s.sum * 31u + v; if (key_collides(v, key)) { if (s.collisions < 4) { s.value[s.collisions] = v; s.index[s.collisions] = idx; } ++s.collisions; } };
    for (uint16_t i = 6; i < 256; ++i) one(pal256[i], i);
    for (uint16_t i = 6; i < 16; ++i) { one(a[i], (uint16_t)(0x1000u | i)); one(b[i], (uint16_t)(0x2000u | i)); }
    return s;
}
inline uint32_t palette_signature(const uint16_t* pal256, const uint16_t* a, const uint16_t* b) { return scan_palette(pal256, a, b, 0).sum; }
// a key that collides with no palette entry and not with black (0x0000); 0 when none is found. Candidates are dark, near-black greys first (an unexpected leak would be nearly invisible).
inline uint16_t choose_key(const uint16_t* pal256, const uint16_t* a, const uint16_t* b, uint16_t current) {
    static const uint16_t kCand[] = {0x0841, 0x0821, 0x1082, 0x18C3, 0x0861, 0x2104, 0x10A2, 0x0C63, 0x0042, 0x0802, 0x1004, 0x0022, 0x1042, 0x0883, 0x20C4, 0x2945};
    for (uint16_t k : kCand) {
        if (k == current || key_collides(0x0000, k)) continue;
        if (scan_palette(pal256, a, b, k).collisions == 0) return k;
    }
    for (uint32_t k = 0x0100; k < 0x2000; k += 3) { if (key_collides(0x0000, (uint16_t)k)) continue; if ((uint16_t)k != current && scan_palette(pal256, a, b, (uint16_t)k).collisions == 0) return (uint16_t)k; }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// CPU-side checks of a region (only at an anomaly / baseline / pause): what the guest wrote (source) and what is submitted to GL (the RGBA5551 staging texels). NO GPU read-back: a read-back before the swap returns an
// older frame (G142 finding), so it can neither confirm nor refute the output.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct SourceStats { uint32_t tiles = 0, visible = 0, keyExact = 0, collisions = 0, distinct = 0, hash = 0; };
inline SourceStats region_source(const uint8_t* sh, uint32_t stride, const bool* tiles, uint16_t key) {
    SourceStats s; uint8_t seen[8192]; memset(seen, 0, sizeof seen); uint32_t h = 2166136261u;
    for (int ty = 0; ty < kRows; ++ty) for (int tx = 0; tx < kCols; ++tx) {
        if (!tiles[ty * kCols + tx]) continue;
        ++s.tiles;
        for (int row = 0; row < kTileH; ++row) {
            const uint16_t* p = (const uint16_t*)(sh + (size_t)(ty * kTileH + row) * stride) + tx * kTileW;
            for (int px = 0; px < kTileW; ++px) {
                const uint16_t v = p[px];
                if (v == key) { ++s.keyExact; continue; }
                if (key_collides(v, key)) { ++s.collisions; continue; }
                ++s.visible; if (!(seen[v >> 3] & (1u << (v & 7)))) { seen[v >> 3] |= (uint8_t)(1u << (v & 7)); ++s.distinct; }
                h = (h ^ v) * 16777619u;
            }
        }
    }
    s.hash = h; return s;
}
struct SubmitStats { uint32_t tiles = 0, pixels = 0, same = 0, near_ = 0, diff = 0, hash = 0; };   // near_: same alpha, every colour channel within one 5-bit step
// `staging` is the RGBA5551 buffer handed to GL (same layout as the shadow): every pixel of the tiles must equal the conversion of the shadow pixel (alpha 0 for the key)
inline SubmitStats region_submit(const uint8_t* sh, const uint8_t* staging, uint32_t stride, const bool* tiles, uint16_t key) {
    SubmitStats s; uint32_t h = 2166136261u;
    for (int ty = 0; ty < kRows; ++ty) for (int tx = 0; tx < kCols; ++tx) {
        if (!tiles[ty * kCols + tx]) continue;
        ++s.tiles;
        for (int row = 0; row < kTileH; ++row) {
            const size_t off = (size_t)(ty * kTileH + row) * stride + (size_t)tx * kTileW * 2;
            for (int px = 0; px < kTileW; ++px) {
                uint16_t v, t; memcpy(&v, sh + off + px * 2, 2); memcpy(&t, staging + off + px * 2, 2);
                const uint16_t e = ign::g113::rgb565_to_5551(v, !ign::g113::is_key(v, key, ign::g113::kKeyMask));
                ++s.pixels;
                if (e == t) { ++s.same; }
                else {
                    const int dr = std::abs((int)(e >> 11) - (int)(t >> 11)), dg = std::abs((int)((e >> 6) & 31) - (int)((t >> 6) & 31)), db = std::abs((int)((e >> 1) & 31) - (int)((t >> 1) & 31));
                    if ((e & 1u) == (t & 1u) && dr <= 1 && dg <= 1 && db <= 1) { ++s.near_; } else { ++s.diff; }
                }
                h = (h ^ t) * 16777619u;
            }
        }
    }
    s.hash = h; return s;
}
// tile set of one region intersected with a mask (e.g. the active tiles)
inline void region_tiles(uint32_t r, const bool* within, bool* out) { const RegionMap& m = region_map(); for (int i = 0; i < kTiles; ++i) out[i] = (m.of[i] == r) && (!within || within[i]); }

}}  // namespace ign::g143
