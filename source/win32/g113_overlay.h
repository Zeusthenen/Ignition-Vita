// iteration-G113/G114: pure, dependency-free HUD-overlay lifecycle logic (no GL,
// no Cpu, no globals) so the exact code the compositor runs is also driven by
// tools/g113_synthetic_test.cpp on the host. glide2x_ign.cpp owns the real state
// (arrays, counters) and passes it in; nothing here allocates.
#pragma once
#include <cstdint>
#include <cstring>

namespace ign { namespace g113 {

constexpr int kTileW = 16, kTileH = 16, kCols = 40, kRows = 25;
constexpr int kTiles = kCols * kRows;
constexpr int kWidth = kCols * kTileW, kHeight = kRows * kTileH;   // 640x400

// A pixel is "the overlay key" if it equals the key ignoring the lowest bit
// (RGB565 blue LSB). The game's two observed transparent values 0x1082/0x1083
// differ exactly in that bit (raw B=16 vs B=24 both map there after
// quantisation), so one dedicated key covers both without widening the test to
// anything else: legitimate black (0x0000) never matches a non-black key.
constexpr uint16_t kKeyMask = 0xFFFEu;
constexpr uint16_t kExactMask = 0xFFFFu;
inline bool is_key(uint16_t v, uint16_t key, uint16_t mask) { return ((uint16_t)(v ^ key) & mask) == 0; }

// GrColor_t (as grSstWinOpen selected: 0=ARGB 1=ABGR 2=RGBA 3=BGRA) -> RGB565.
inline uint16_t argb_to_rgb565(uint32_t value, uint32_t colorFormat) {
    auto ch = [&](int shift) { return (value >> shift) & 0xFFu; };
    uint32_t r, g, b;
    switch (colorFormat) {
        case 0x1: b = ch(16); g = ch(8);  r = ch(0);  break;
        case 0x2: r = ch(24); g = ch(16); b = ch(8);  break;
        case 0x3: b = ch(24); g = ch(16); r = ch(8);  break;
        default:  r = ch(16); g = ch(8);  b = ch(0);  break;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Dedicated chromakey of the LFB overlay layer, independent of the live 3D
// pipeline state (G113 sampled it only at write locks gated on pixelPipeline &&
// chromakey-enabled; the log proved every HUD lock has pipe=0/en=0, so it never
// latched and the layer was cleared to black).
//  - The candidate is captured DIRECTLY in grChromakeyValue, whatever the mode.
//  - grChromakeyMode never touches it.
//  - A zero (black) value is never a key: it is counted and rejected, so a
//    temporary 0 can neither become nor replace the key.
//  - The key itself is the value that precedes the HUD's LFB write locks most
//    often (histogram of the latched candidate sampled at every write lock,
//    buckets merged by RGB565|1), chosen with a minimum sample count and a 2x
//    hysteresis so a few one-off values (menus, 3D materials) can't steal it.
//  - It survives scene boundaries (a second race must not start unkeyed) and is
//    invalidated only by a new Glide context.
struct OverlayKey {
    enum Source : uint8_t { SRC_NONE = 0, SRC_LOCK_CONFIRMED = 1, SRC_CACHED_PROVISIONAL = 2, SRC_SEED_PROVISIONAL = 3 };
    // the key in force
    uint32_t raw = 0; uint16_t rgb565 = 0; bool valid = false; Source source = SRC_NONE;
    bool provisional = false;           // valid but not yet confirmed by lock samples of THIS session
    uint32_t generation = 0;            // Glide context generation this key belongs to
    uint32_t lastConfirmedSwap = 0;     // last swap on which a lock sampled the key's bucket
    uint32_t changes = 0, zeroRejected = 0, valueCalls = 0, samples = 0;
    uint32_t provisionalConfirmed = 0, provisionalRejected = 0;
    bool persistRequested = false;      // a confirmed/changed key should be written to the cache file
    // latest nonzero grChromakeyValue, regardless of mode
    uint32_t candRaw = 0; uint16_t cand565 = 0; bool candValid = false;
    struct Bucket { uint16_t id; uint16_t cnt[2]; uint32_t raw[2]; uint32_t total; };
    static constexpr int kMaxBuckets = 8;
    static constexpr uint32_t kMinSamples = 8;
    Bucket b[kMaxBuckets];
    int n = 0; uint32_t bucketsDropped = 0;

    void reset(uint32_t newGeneration) { *this = OverlayKey(); generation = newGeneration; }

    // Early availability: a PROVISIONAL key (learned and persisted by a previous
    // session, or the value documented from observed grChromakeyValue calls) is
    // usable immediately but must be confirmed by this session's own write-lock
    // samples; contradicting dominant evidence replaces it. Ignored when a key
    // already exists or the value is black.
    bool seedProvisional(uint32_t rawValue, uint16_t v565, Source src) {
        if (valid || v565 == 0) return false;
        raw = rawValue; rgb565 = v565; valid = true; provisional = true; source = src; return true;
    }
    // Returns true when the candidate was accepted (nonzero).
    bool onChromakeyValue(uint32_t rawValue, uint16_t v565) {
        ++valueCalls;
        if (v565 == 0) { ++zeroRejected; return false; }
        candRaw = rawValue; cand565 = v565; candValid = true; return true;
    }
    // One write lock happened: sample the candidate. Returns true if the key changed.
    bool onWriteLock(uint32_t swap) {
        if (!candValid) return false;
        ++samples;
        const uint16_t id = (uint16_t)(cand565 | 1u);
        const int exact = cand565 & 1;
        int i = 0;
        for (; i < n; ++i) if (b[i].id == id) break;
        if (i == n) {
            if (n >= kMaxBuckets) { ++bucketsDropped; return false; }
            b[n] = Bucket{id, {0, 0}, {0, 0}, 0}; ++n;
        }
        b[i].cnt[exact] = (uint16_t)(b[i].cnt[exact] < 65535 ? b[i].cnt[exact] + 1 : 65535);
        b[i].raw[exact] = candRaw; ++b[i].total;
        if (valid && (uint16_t)(rgb565 | 1u) == id) lastConfirmedSwap = swap;
        int best = 0;
        for (int k = 1; k < n; ++k) if (b[k].total > b[best].total) best = k;
        if (b[best].total < kMinSamples) return false;
        const int bx = b[best].cnt[1] >= b[best].cnt[0] ? 1 : 0;
        const uint16_t exact565 = (uint16_t)((b[best].id & ~1u) | (uint32_t)bx);
        if (valid) {
            const uint16_t curId = (uint16_t)(rgb565 | 1u);
            if (b[best].id == curId) {      // same bucket: confirm a provisional key / follow the dominant exact value
                bool changed = false;
                if (provisional) { provisional = false; source = SRC_LOCK_CONFIRMED; ++provisionalConfirmed; persistRequested = true; lastConfirmedSwap = swap; }
                if (exact565 != rgb565) { rgb565 = exact565; raw = b[best].raw[bx]; changed = true; persistRequested = true; }
                return changed;
            }
            int ci = -1; for (int k = 0; k < n; ++k) if (b[k].id == curId) ci = k;
            if (!provisional && ci >= 0 && b[best].total <= 2u * b[ci].total) return false;   // hysteresis (confirmed keys only)
            if (provisional) ++provisionalRejected;
        }
        rgb565 = exact565; raw = b[best].raw[bx]; valid = true; provisional = false; source = SRC_LOCK_CONFIRMED;
        ++changes; lastConfirmedSwap = swap; persistRequested = true;
        return true;
    }
    // A frame may clear the layer only against a real, non-black key.
    bool may_clear() const { return valid && rgb565 != 0; }
};
inline void fill_key(uint8_t* buf, size_t bytes, uint16_t key) {
    uint32_t pat = ((uint32_t)key << 16) | key;
    size_t words = bytes / 4;
    for (size_t i = 0; i < words; ++i) memcpy(buf + i * 4, &pat, 4);
    for (size_t i = words * 4; i + 2 <= bytes; i += 2) memcpy(buf + i, &key, 2);
}

struct ScanStats {
    uint32_t tilesNew = 0, tilesRetired = 0, tilesStillVisible = 0, tilesReappeared = 0;
    uint32_t changedTiles = 0, occupiedTiles = 0, changedBytes = 0, unchangedOccupiedNotActive = 0;
    int minCol = -1, maxCol = -1, minRow = -1, maxRow = -1;
};

struct MaskArrays {
    bool* active;            // kTiles, what the overlay draws
    uint32_t* activeCount;
    bool* retiredLast;       // kTiles, tile retired by the previous scan (measurement only)
    bool* changed;           // kTiles, out: differs from `prev`
    bool* occupied;          // kTiles, out: >=1 non-key pixel
};

// Scene boundary (GL scene entry, context change): nothing from the previous
// scene may stay in the overlay.
inline void reset_mask(MaskArrays& m) {
    memset(m.active, 0, kTiles * sizeof(bool));
    *m.activeCount = 0;
    memset(m.retiredLast, 0, kTiles * sizeof(bool));
}

// ONE pass over the buffer: change detection (vs prev), occupancy (vs key) and
// the mask update. Fast path: a row that is exactly the key row (what a cleared
// layer is made of) costs one memcmp, so the common mostly-empty layer is
// cheap; only non-exact rows fall to the tolerant per-pixel test.
//  cleanLayer == true : the layer was cleared to the key at the start of this
//    frame and rebuilt only by this frame's own writes, so occupied tiles ARE
//    the current HUD: active := occupied (no history, no base buffer).
//  cleanLayer == false: fallback = G112 rule (retire an active tile once it is
//    empty, activate an inactive tile only if it changed AND has content).
inline void scan_and_update(const uint8_t* cur, const uint8_t* prev, uint32_t stride,
                            uint16_t key, uint16_t cmpMask, bool cleanLayer,
                            MaskArrays& m, ScanStats& st) {
    uint8_t keyRow[kTileW * 2];
    fill_key(keyRow, sizeof(keyRow), key);
    for (int ty = 0; ty < kRows; ++ty) {
        for (int tx = 0; tx < kCols; ++tx) {
            bool tileChanged = (prev == nullptr);
            bool occupied = false;
            for (int row = 0; row < kTileH; ++row) {
                const size_t off = (size_t)(ty * kTileH + row) * stride + (size_t)tx * kTileW * 2;
                const uint8_t* c = cur + off;
                if (!occupied && memcmp(c, keyRow, sizeof(keyRow)) != 0) {
                    for (int px = 0; px < kTileW; ++px) {
                        uint16_t v; memcpy(&v, c + px * 2, 2);
                        if (!is_key(v, key, cmpMask)) { occupied = true; break; }
                    }
                }
                if (prev && !tileChanged && memcmp(prev + off, c, (size_t)kTileW * 2) != 0) tileChanged = true;
                if (occupied && tileChanged) break;
            }
            const int idx = ty * kCols + tx;
            const bool wasActive = m.active[idx];
            m.changed[idx] = tileChanged;
            m.occupied[idx] = occupied;
            if (occupied) ++st.occupiedTiles;
            if (tileChanged) {
                ++st.changedTiles; st.changedBytes += (uint32_t)kTileW * kTileH * 2;
                if (st.minCol < 0 || tx < st.minCol) st.minCol = tx;
                if (tx > st.maxCol) st.maxCol = tx;
                if (st.minRow < 0 || ty < st.minRow) st.minRow = ty;
                if (ty > st.maxRow) st.maxRow = ty;
            }
            if (m.retiredLast[idx] && occupied) ++st.tilesReappeared;
            bool retiredNow = false;
            if (cleanLayer) {
                if (occupied) {
                    if (wasActive) ++st.tilesStillVisible;
                    else { m.active[idx] = true; ++(*m.activeCount); ++st.tilesNew; if (!tileChanged) ++st.unchangedOccupiedNotActive; }
                } else if (wasActive) {
                    m.active[idx] = false; --(*m.activeCount); ++st.tilesRetired; retiredNow = true;
                }
            } else {
                if (wasActive && !occupied) { m.active[idx] = false; --(*m.activeCount); ++st.tilesRetired; retiredNow = true; }
                else if (wasActive) ++st.tilesStillVisible;
                else if (occupied && tileChanged) { m.active[idx] = true; ++(*m.activeCount); ++st.tilesNew; }
            }
            m.retiredLast[idx] = retiredNow;
        }
    }
}

// prev := cur, but only for tiles that actually differ (identical tiles are
// already equal). Replaces a 512000-byte memcpy per snapshot.
inline void copy_changed_tiles(uint8_t* prev, const uint8_t* cur, uint32_t stride, const bool* changed) {
    for (int ty = 0; ty < kRows; ++ty) {
        int tx = 0;
        while (tx < kCols) {
            if (!changed[ty * kCols + tx]) { ++tx; continue; }
            int tx1 = tx; while (tx1 < kCols && changed[ty * kCols + tx1]) ++tx1;
            for (int row = 0; row < kTileH; ++row) {
                const size_t off = (size_t)(ty * kTileH + row) * stride + (size_t)tx * kTileW * 2;
                memcpy(prev + off, cur + off, (size_t)(tx1 - tx) * kTileW * 2);
            }
            tx = tx1;
        }
    }
}

struct ClearStats { uint32_t tiles = 0; uint64_t bytes = 0; };

// Sparse clear: refill only the tiles marked dirty (merged into horizontal
// runs, one fill per pixel row per run) with the exact key, then clear the
// dirty flags. `cleared` (optional, kTiles) records which tiles were refilled.
inline void clear_dirty_tiles(uint8_t* buf, uint32_t stride, bool* dirty, uint16_t key, ClearStats& cs, bool* cleared) {
    for (int ty = 0; ty < kRows; ++ty) {
        int tx = 0;
        while (tx < kCols) {
            if (!dirty[ty * kCols + tx]) { ++tx; continue; }
            int tx1 = tx; while (tx1 < kCols && dirty[ty * kCols + tx1]) ++tx1;
            const size_t runBytes = (size_t)(tx1 - tx) * kTileW * 2;
            for (int row = 0; row < kTileH; ++row)
                fill_key(buf + (size_t)(ty * kTileH + row) * stride + (size_t)tx * kTileW * 2, runBytes, key);
            for (int k = tx; k < tx1; ++k) { dirty[ty * kCols + k] = false; if (cleared) cleared[ty * kCols + k] = true; }
            cs.tiles += (uint32_t)(tx1 - tx); cs.bytes += runBytes * kTileH;
            tx = tx1;
        }
    }
}

// Self-check of a clear: how many of the tiles flagged in `cleared` are not
// exactly the key everywhere. Expected 0.
inline uint32_t verify_cleared(const uint8_t* buf, uint32_t stride, const bool* cleared, uint16_t key) {
    uint8_t keyRow[kTileW * 2]; fill_key(keyRow, sizeof(keyRow), key);
    uint32_t bad = 0;
    for (int ty = 0; ty < kRows; ++ty) for (int tx = 0; tx < kCols; ++tx) {
        if (!cleared[ty * kCols + tx]) continue;
        for (int row = 0; row < kTileH; ++row)
            if (memcmp(buf + (size_t)(ty * kTileH + row) * stride + (size_t)tx * kTileW * 2, keyRow, sizeof(keyRow)) != 0) { ++bad; break; }
    }
    return bad;
}

// Overlay texture, RGBA5551 (R5 G5 B5 A1, the layout GL_UNSIGNED_SHORT_5_5_5_1
// uses and vitaGL copies without conversion for a GL_RGBA texture): the same
// 16 bits per pixel as the RGB565 shadow, with alpha = 0 exactly for the
// dedicated overlay key (bit-0 tolerance) and alpha = 1 for every other
// pixel, legitimate black included. With GL_ALPHA_TEST the overlay is drawn
// as ONE quad per active tile -- no per-pixel geometry.
inline uint16_t rgb565_to_5551(uint16_t v, bool opaque) {
    uint32_t r5 = v >> 11, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
    uint32_t g5 = (g6 + 1u) >> 1; if (g5 > 31u) g5 = 31u;
    return (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | (opaque ? 1u : 0u));
}

struct ConvertStats { uint32_t tiles = 0; uint64_t bytes = 0; uint32_t keyPixels = 0, drawnPixels = 0, blackPixelsDrawn = 0; };

// Converts only what must be converted: active tiles that changed this frame or
// whose texels in `dst` are not valid yet (`converted[i] == false`). Inactive
// tiles are never drawn, so their texels are left alone (and marked stale so
// they are converted again if they become active). dst/src share the shadow's
// layout (640 px per row, `stride` bytes).
inline void convert_tiles_5551(const uint8_t* src565, uint8_t* dst5551, uint32_t stride, const bool* active,
                               const bool* changed, bool* converted, uint16_t key, uint16_t mask, ConvertStats& cs) {
    for (int ty = 0; ty < kRows; ++ty) for (int tx = 0; tx < kCols; ++tx) {
        const int idx = ty * kCols + tx;
        if (!active[idx]) { converted[idx] = false; continue; }
        if (converted[idx] && !changed[idx]) continue;
        for (int row = 0; row < kTileH; ++row) {
            const size_t off = (size_t)(ty * kTileH + row) * stride + (size_t)tx * kTileW * 2;
            for (int px = 0; px < kTileW; ++px) {
                uint16_t v; memcpy(&v, src565 + off + px * 2, 2);
                const bool opaque = !is_key(v, key, mask);
                if (opaque) { ++cs.drawnPixels; if (v == 0) ++cs.blackPixelsDrawn; } else ++cs.keyPixels;
                const uint16_t o = rgb565_to_5551(v, opaque);
                memcpy(dst5551 + off + px * 2, &o, 2);
            }
        }
        converted[idx] = true; ++cs.tiles; cs.bytes += (uint64_t)kTileW * kTileH * 2;
    }
}
}}  // namespace ign::g113
