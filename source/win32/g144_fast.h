// win32/g144_fast.h -- G144: INCREMENTAL LFB COMPOSITOR FAST PATH (race only) + the cheap windowed counters of the scene profile prepared for G145.
// Pure (no GL, no guest access, no logging, no globals): tools/g144_test.cpp drives exactly this code against the frozen G113 reference functions, pixel for pixel.
//
// What the G143 real test measured (Moosejaw Falls): compositor 10.57 ms (the whole 1 MB scan: current frame + previous snapshot, 1000 tiles x 16 rows x 2 libc memcmp), tex_upload 7.63 ms and
// compositor_upload_kB = 500.0 EVERY frame: glTexSubImage2D of the whole 640x400 RGBA5551 texture. vitaGL's glTexSubImage2D (disassembled from libvitaGL.a, textures.o: it compares
// `vgl_framecount - texture.last_frame` with 4 and branches) REALLOCATES a texture drawn in the last 4 frames and memcpy's the whole old storage (GPU-mapped memory) into the new one before writing the new texels, so
// a sub-rectangle upload to the texture that is being drawn costs the same as a full one. A texture not drawn for >= 5 frames is written in place, the rectangle only. Hence the RING below: kRingN textures, the next
// one in the ring is brought up to date (only the tiles converted since its last upload) and drawn; it was last drawn kRingN frames earlier (>= 6 > 4) whatever the cadence of the changes.
//
// Everything here restates what the G143 path computes; nothing is approximated:
//  * scan_fast      == ign::g113::scan_and_update (masks, changed / occupied flags, every ScanStats field), proven on random sequences;
//  * convert_fast   == ign::g113::convert_tiles_5551 (texels, converted flags, tile / byte counts; the three per-pixel counters are not kept: only diagnostics read them);
//  * Ring + plan_upload: the texture the overlay is drawn from holds, for every ACTIVE tile, exactly the texels of the staging buffer (what the G143 full upload would have sent). An inactive tile is never drawn (one
//    quad per active tile) and a tile that becomes active again is converted again (convert_tiles_5551 clears `converted` of every inactive tile), which marks it dirty in every slot.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g113_overlay.h"

namespace ign { namespace g144 {

namespace K = ign::g113;

// ================================================================================================================================================
// 1. FAST SCAN
// ================================================================================================================================================
inline uint64_t rep4(uint16_t v) { return (uint64_t)v * 0x0001000100010001ull; }
inline uint64_t load64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

// Per tile: "the previous snapshot of this tile is exactly the key row pattern". Valid only for the key it was computed with and only while `prev` is changed exclusively by copy_changed_tiles / the first full
// copy right after a scan_fast (the compositor is the only writer of the snapshot). Any other scan path calls invalidate().
struct FastScanState {
    bool exact[K::kTiles];
    bool valid = false; uint16_t key = 0;
    FastScanState() { memset(exact, 0, sizeof exact); }
    void invalidate() { valid = false; }
};
struct FastScanCounters { uint32_t tilesExact = 0, tilesSkippedPrev = 0, tilesCompared = 0, tilesOccupiedScan = 0; };

// the 16 rows of tile (ty,tx) of `a` and `b` are identical
inline bool tile_equal(const uint8_t* a, const uint8_t* b, uint32_t stride, int ty, int tx) {
    for (int row = 0; row < K::kTileH; ++row) {
        const size_t off = (size_t)(ty * K::kTileH + row) * stride + (size_t)tx * K::kTileW * 2;
        if (load64(a + off) != load64(b + off) || load64(a + off + 8) != load64(b + off + 8) || load64(a + off + 16) != load64(b + off + 16) || load64(a + off + 24) != load64(b + off + 24)) return false;
    }
    return true;
}
// at least one pixel of the tile is not the key under the tolerant mask (the predicate of ign::g113::is_key, four pixels per 64-bit word)
inline bool tile_occupied_mask(const uint8_t* cur, uint32_t stride, int ty, int tx, uint64_t kp, uint64_t mp) {
    for (int row = 0; row < K::kTileH; ++row) {
        const uint8_t* p = cur + (size_t)(ty * K::kTileH + row) * stride + (size_t)tx * K::kTileW * 2;
        if ((((load64(p) ^ kp) | (load64(p + 8) ^ kp) | (load64(p + 16) ^ kp) | (load64(p + 24) ^ kp)) & mp) != 0) return true;
    }
    return false;
}

// Same contract and the same results as ign::g113::scan_and_update. The current frame is read ONCE, sequentially, in 64-bit words, one tile row (16 scanlines) at a time: a tile whose 16x16 pixels are exactly the key
// is "empty" without any per-pixel test, and when the snapshot of that tile was also exactly the key (FastScanState) it is "unchanged" without reading the snapshot. Only the tiles that hold something are examined
// further.
inline void scan_fast(const uint8_t* cur, const uint8_t* prev, uint32_t stride, uint16_t key, uint16_t cmpMask, bool cleanLayer,
                      K::MaskArrays& m, K::ScanStats& st, FastScanState& fs, FastScanCounters* fc = nullptr) {
    const uint64_t kp = rep4(key), mp = rep4(cmpMask);
    const bool flagsOk = fs.valid && fs.key == key && prev != nullptr;
    bool curExact[K::kTiles];
    for (int ty = 0; ty < K::kRows; ++ty) {
        uint64_t acc[K::kCols];
        for (int tx = 0; tx < K::kCols; ++tx) acc[tx] = 0;
        for (int row = 0; row < K::kTileH; ++row) {
            const uint8_t* r = cur + (size_t)(ty * K::kTileH + row) * stride;
            for (int tx = 0; tx < K::kCols; ++tx) {
                const uint8_t* p = r + (size_t)tx * K::kTileW * 2;
                acc[tx] |= (load64(p) ^ kp) | (load64(p + 8) ^ kp) | (load64(p + 16) ^ kp) | (load64(p + 24) ^ kp);
            }
        }
        for (int tx = 0; tx < K::kCols; ++tx) {
            const int idx = ty * K::kCols + tx;
            const bool exact = acc[tx] == 0;
            bool occupied = false, tileChanged;
            if (exact) {
                if (prev == nullptr) tileChanged = true;
                else if (flagsOk && fs.exact[idx]) { tileChanged = false; if (fc) ++fc->tilesSkippedPrev; }
                else { tileChanged = !tile_equal(prev, cur, stride, ty, tx); if (fc) ++fc->tilesCompared; }
                if (fc) ++fc->tilesExact;
            } else {
                occupied = tile_occupied_mask(cur, stride, ty, tx, kp, mp);
                tileChanged = prev == nullptr || !tile_equal(prev, cur, stride, ty, tx);
                if (fc) { ++fc->tilesOccupiedScan; ++fc->tilesCompared; }
            }
            curExact[idx] = exact;
            const bool wasActive = m.active[idx];
            m.changed[idx] = tileChanged;
            m.occupied[idx] = occupied;
            if (occupied) ++st.occupiedTiles;
            if (tileChanged) {
                ++st.changedTiles; st.changedBytes += (uint32_t)K::kTileW * K::kTileH * 2;
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
    // the snapshot is about to become `cur` (copy_changed_tiles / the first full copy): its tiles are exactly the key where `cur`'s are
    memcpy(fs.exact, curExact, sizeof fs.exact);
    fs.valid = true; fs.key = key;
}

// ================================================================================================================================================
// 2. FAST CONVERSION (RGB565 shadow -> RGBA5551 staging, alpha 0 = overlay key)
// ================================================================================================================================================
inline uint16_t conv_px(uint16_t v, uint16_t key, uint16_t mask) {
    const uint32_t g6 = (v >> 5) & 0x3Fu;
    uint32_t g5 = (g6 + 1u) >> 1; g5 -= (g5 >> 5);   // min(31, (g6 + 1) / 2): the only overflow is 32
    const uint32_t opaque = (((uint32_t)(uint16_t)(v ^ key) & mask) != 0u) ? 1u : 0u;
    return (uint16_t)((v & 0xF800u) | (g5 << 6) | ((v & 0x1Fu) << 1) | opaque);
}
// == ign::g113::convert_tiles_5551 for texels, `converted` flags, cs.tiles and cs.bytes
inline void convert_fast(const uint8_t* src565, uint8_t* dst5551, uint32_t stride, const bool* active, const bool* changed, bool* converted, uint16_t key, uint16_t mask, K::ConvertStats& cs) {
    for (int ty = 0; ty < K::kRows; ++ty) for (int tx = 0; tx < K::kCols; ++tx) {
        const int idx = ty * K::kCols + tx;
        if (!active[idx]) { converted[idx] = false; continue; }
        if (converted[idx] && !changed[idx]) continue;
        for (int row = 0; row < K::kTileH; ++row) {
            const size_t off = (size_t)(ty * K::kTileH + row) * stride + (size_t)tx * K::kTileW * 2;
            for (int px = 0; px < K::kTileW; ++px) {
                uint16_t v; memcpy(&v, src565 + off + (size_t)px * 2, 2);
                const uint16_t o = conv_px(v, key, mask);
                memcpy(dst5551 + off + (size_t)px * 2, &o, 2);
            }
        }
        converted[idx] = true; ++cs.tiles; cs.bytes += (uint64_t)K::kTileW * K::kTileH * 2;
    }
}

// the tiles convert_fast / convert_tiles_5551 are about to convert (active AND (not converted yet OR changed)): the tiles that become dirty in every ring slot. Call BEFORE the conversion.
inline uint32_t will_convert(const bool* active, const bool* changed, const bool* converted, bool* out) {
    uint32_t n = 0;
    for (int i = 0; i < K::kTiles; ++i) { out[i] = active[i] && !(converted[i] && !changed[i]); n += out[i] ? 1u : 0u; }
    return n;
}

// ================================================================================================================================================
// 3. UPLOAD PLANNER: which rectangles of the staging buffer go to the texture
// ================================================================================================================================================
constexpr int kMaxRects = 40;
constexpr uint32_t kCallCostBytes = 16384;   // MODEL: one glTexSubImage2D call costs about as much as copying 16 KiB. Only a model: the Vita tells the real constant (the window counters calibrate it for G145).
constexpr uint32_t kTileBytes = (uint32_t)K::kTileW * K::kTileH * 2;
constexpr uint32_t kNoCost = 0xFFFFFFFFu;
struct Rect { uint8_t tx0, ty0, tx1, ty1; };   // tile units, half-open [tx0,tx1) x [ty0,ty1)
enum Strategy : uint8_t { kStratNone = 0, kStratRects = 1, kStratBands = 2, kStratBbox = 3, kStratFull = 4 };
inline const char* strategy_name(uint32_t s) { switch (s) { case kStratRects: return "rects"; case kStratBands: return "bands"; case kStratBbox: return "bbox"; case kStratFull: return "full"; default: return "none"; } }
struct UploadPlan {
    Strategy kind = kStratNone;
    int n = 0; Rect r[kMaxRects];
    uint32_t wantedTiles = 0, bytes = 0;                                     // tiles that must reach the texture / bytes the plan sends (>= wantedTiles * 512)
    uint32_t costRects = kNoCost, costBands = kNoCost, costBbox = kNoCost, costFull = kNoCost;   // model cost of each strategy
};
inline uint32_t rect_bytes(const Rect& r) { return (uint32_t)(r.tx1 - r.tx0) * (uint32_t)(r.ty1 - r.ty0) * kTileBytes; }
inline bool rect_full_width(const Rect& r) { return r.tx0 == 0 && r.tx1 == K::kCols; }

// `want[i]`: tile i must be in the texture. Cheapest (by the model) of: exact rectangles (runs per tile row, merged vertically when identical), full-width bands (contiguous rows of the staging buffer: no repacking),
// the bounding box and the whole texture. Every strategy covers every wanted tile (property-tested); the choice is only about cost.
inline void plan_upload(const bool* want, UploadPlan& p) {
    p = UploadPlan();
    int minC = K::kCols, maxC = -1, minR = K::kRows, maxR = -1; uint32_t tiles = 0;
    bool rowAny[K::kRows];
    for (int ty = 0; ty < K::kRows; ++ty) {
        rowAny[ty] = false;
        for (int tx = 0; tx < K::kCols; ++tx) if (want[ty * K::kCols + tx]) {
            ++tiles; rowAny[ty] = true; if (tx < minC) minC = tx; if (tx > maxC) maxC = tx; if (ty < minR) minR = ty; if (ty > maxR) maxR = ty;
        }
    }
    p.wantedTiles = tiles;
    if (!tiles) return;
    // A: rectangles
    Rect ra[kMaxRects]; int na = 0; bool feasibleA = true; uint32_t bytesA = 0;
    {
        int prevIdx[K::kCols], curIdx[K::kCols]; int nPrev = 0, nCur = 0;
        for (int ty = 0; ty < K::kRows && feasibleA; ++ty) {
            nCur = 0;
            int tx = 0;
            while (tx < K::kCols) {
                if (!want[ty * K::kCols + tx]) { ++tx; continue; }
                int tx1 = tx; while (tx1 < K::kCols && want[ty * K::kCols + tx1]) ++tx1;
                int found = -1;
                for (int k = 0; k < nPrev; ++k) { const Rect& q = ra[prevIdx[k]]; if (q.tx0 == tx && q.tx1 == tx1) { found = prevIdx[k]; break; } }
                if (found >= 0) { ra[found].ty1 = (uint8_t)(ty + 1); curIdx[nCur++] = found; }
                else {
                    if (na >= kMaxRects) { feasibleA = false; break; }
                    ra[na].tx0 = (uint8_t)tx; ra[na].tx1 = (uint8_t)tx1; ra[na].ty0 = (uint8_t)ty; ra[na].ty1 = (uint8_t)(ty + 1);
                    curIdx[nCur++] = na++;
                }
                tx = tx1;
            }
            for (int k = 0; k < nCur; ++k) prevIdx[k] = curIdx[k];
            nPrev = nCur;
        }
        if (feasibleA) { for (int i = 0; i < na; ++i) bytesA += rect_bytes(ra[i]); p.costRects = (uint32_t)na * kCallCostBytes + bytesA; }
    }
    // B: full-width bands (consecutive tile rows holding something)
    Rect rb[kMaxRects]; int nb = 0; uint32_t bytesB = 0;
    for (int ty = 0; ty < K::kRows;) {
        if (!rowAny[ty]) { ++ty; continue; }
        int t1 = ty; while (t1 < K::kRows && rowAny[t1]) ++t1;
        rb[nb].tx0 = 0; rb[nb].tx1 = (uint8_t)K::kCols; rb[nb].ty0 = (uint8_t)ty; rb[nb].ty1 = (uint8_t)t1; bytesB += rect_bytes(rb[nb]); ++nb;
        ty = t1;
    }
    p.costBands = (uint32_t)nb * kCallCostBytes + bytesB;
    // C: bounding box; D: everything
    Rect bb{(uint8_t)minC, (uint8_t)minR, (uint8_t)(maxC + 1), (uint8_t)(maxR + 1)};
    const uint32_t bytesC = rect_bytes(bb);
    p.costBbox = kCallCostBytes + bytesC;
    Rect full{0, 0, (uint8_t)K::kCols, (uint8_t)K::kRows};
    const uint32_t bytesD = rect_bytes(full);
    p.costFull = kCallCostBytes + bytesD;
    // choose the cheapest by the model; on a tie the more exact plan wins (full, then bbox, then bands, then rectangles)
    uint32_t best = p.costFull; p.kind = kStratFull; p.n = 1; p.r[0] = full; p.bytes = bytesD;
    if (p.costBbox <= best) { best = p.costBbox; p.kind = kStratBbox; p.n = 1; p.r[0] = bb; p.bytes = bytesC; }
    if (p.costBands <= best) { best = p.costBands; p.kind = kStratBands; p.n = nb; for (int i = 0; i < nb; ++i) p.r[i] = rb[i]; p.bytes = bytesB; }
    if (feasibleA && p.costRects <= best) { best = p.costRects; p.kind = kStratRects; p.n = na; for (int i = 0; i < na; ++i) p.r[i] = ra[i]; p.bytes = bytesA; }
}

// ================================================================================================================================================
// 4. THE TEXTURE RING (bookkeeping only: the GL texture ids live in glide2x_ign.cpp)
// ================================================================================================================================================
constexpr int kRingN = 6;   // vitaGL reallocates a texture drawn within the last 4 frames; a ring of 6 draws each texture >= 6 frames apart
class Ring {
public:
    Ring() { reset(); }
    // the textures are gone (new Glide context / GL error): nothing exists, nothing is valid
    void reset() { cur_ = kRingN - 1; curValid_ = false; for (int s = 0; s < kRingN; ++s) created_[s] = false; all_dirty(); }
    // conversions happened that the ring did not see (the G143 path, a re-armed layer): every slot is stale
    void invalidate() { curValid_ = false; all_dirty(); }
    // tiles converted into the staging buffer this frame: stale in every slot
    void mark_converted(const bool* conv) {
        for (int i = 0; i < K::kTiles; ++i) if (conv[i]) for (int s = 0; s < kRingN; ++s) dirty_[s][i] = true;
    }
    int next_slot() const { return (cur_ + 1) % kRingN; }
    int cur() const { return cur_; }
    bool cur_valid() const { return curValid_; }
    bool created(int s) const { return created_[s]; }
    // the tiles the next slot needs: dirty there AND active now (an inactive tile is never drawn and is converted -- hence marked dirty again -- when it returns)
    int begin(const bool* active, bool* want) const {
        const int s = next_slot();
        for (int i = 0; i < K::kTiles; ++i) want[i] = dirty_[s][i] && active[i];
        return s;
    }
    // the slot now holds every active tile up to date and is the one to draw
    void commit(int s) { created_[s] = true; for (int i = 0; i < K::kTiles; ++i) dirty_[s][i] = false; cur_ = s; curValid_ = true; }
    uint32_t dirty_count(int s) const { uint32_t n = 0; for (int i = 0; i < K::kTiles; ++i) n += dirty_[s][i] ? 1u : 0u; return n; }
    bool dirty(int s, int i) const { return dirty_[s][i]; }
private:
    void all_dirty() { for (int s = 0; s < kRingN; ++s) for (int i = 0; i < K::kTiles; ++i) dirty_[s][i] = true; }
    int cur_; bool curValid_; bool created_[kRingN]; bool dirty_[kRingN][K::kTiles];
};

// ================================================================================================================================================
// 5. THE CONTRACT: when the fast path may run (every condition must hold; anything else is the G143 path, unchanged)
// ================================================================================================================================================
enum Reason : uint8_t {
    kRFast = 0, kRRingOff, kRNotRace, kRTransition, kRGuardPending, kRNotArmed, kRNoGL, kRKeyNotConfirmed, kRAlphaOff, kRBadGeometry, kRNotClean, kRNoPrev, kRSettling, kRRearm, kRRefresh, kRSetup, kReasonCount
};
inline const char* reason_name(uint32_t r) {
    switch (r) {
        case kRFast: return "fast"; case kRRingOff: return "ring_disabled(gl_error_or_unsupported)"; case kRNotRace: return "not_race_mode(menu/loading/pause/results/selector)"; case kRTransition: return "transition_or_carousel";
        case kRGuardPending: return "g143_boundary_or_key_switch_pending"; case kRNotArmed: return "scene_not_armed"; case kRNoGL: return "no_3d_this_frame"; case kRKeyNotConfirmed: return "key_not_confirmed";
        case kRAlphaOff: return "alpha_path_unavailable"; case kRBadGeometry: return "lfb_geometry_or_contract_invalid"; case kRNotClean: return "layer_not_clean"; case kRNoPrev: return "no_previous_snapshot";
        case kRSettling: return "mode_settling"; case kRRearm: return "rearm_after_full_path"; case kRRefresh: return "periodic_full_refresh"; case kRSetup: return "ring_setup";
        default: return "?";
    }
}
struct Inputs {
    bool ringEnabled = true;     // no GL error / unsupported geometry has switched the ring off for good
    bool raceMode = false;       // the G143 mode machine says RACE (not pause, not menu, not results)
    uint32_t framesInMode = 0;   // swaps since the mode last changed
    bool transition = false;     // a tile spike episode is open, or the car carousel is active
    bool guardPending = false;   // a G143 boundary clear / key switch is scheduled for the end of this swap
    bool armed = false, hasGL = false;
    bool keyUsable = false;      // valid, non-black, confirmed or pinned (not provisional)
    bool alphaOk = false;        // the RGBA5551 path is available
    bool geometryOk = false;     // valid transaction group, UPPER_LEFT origin, 640x400 with stride 1280, host pointer
    bool cleanLayer = false;     // the G113 clean-layer model holds for this frame
    bool havePrev = false;
};
constexpr uint32_t kSettleFrames = 8;       // frames of a mode before its first fast frame
constexpr uint32_t kRearmFrames = 3;        // valid frames on the G143 path before the fast path (re)starts
constexpr uint32_t kRefreshFrames = 3600;   // one G143 frame every 3600 fast frames (self-healing; costs one burst of uploads)
struct Decision { bool fast = false; Reason reason = kRSetup; };
class Arbiter {
public:
    Decision feed(const Inputs& in) {
        Decision d;
        Reason r = first_failure(in);
        if (r != kRFast) { streak_ = 0; sinceRefresh_ = 0; d.reason = r; ++counts_[r]; return d; }
        ++streak_;
        if (streak_ <= kRearmFrames) { d.reason = kRRearm; ++counts_[kRRearm]; return d; }
        if (++sinceRefresh_ > kRefreshFrames) { sinceRefresh_ = 0; d.reason = kRRefresh; ++counts_[kRRefresh]; return d; }
        d.fast = true; d.reason = kRFast; ++counts_[kRFast];
        return d;
    }
    void reset() { streak_ = 0; sinceRefresh_ = 0; }
    uint32_t count(uint32_t r) const { return counts_[r]; }
    uint32_t streak() const { return streak_; }
private:
    static Reason first_failure(const Inputs& in) {
        if (!in.ringEnabled) return kRRingOff;
        if (!in.raceMode) return kRNotRace;
        if (in.transition) return kRTransition;
        if (in.guardPending) return kRGuardPending;
        if (!in.armed) return kRNotArmed;
        if (!in.hasGL) return kRNoGL;
        if (!in.keyUsable) return kRKeyNotConfirmed;
        if (!in.alphaOk) return kRAlphaOff;
        if (!in.geometryOk) return kRBadGeometry;
        if (!in.cleanLayer) return kRNotClean;
        if (!in.havePrev) return kRNoPrev;
        if (in.framesInMode < kSettleFrames) return kRSettling;
        return kRFast;
    }
    uint32_t streak_ = 0, sinceRefresh_ = 0, counts_[kReasonCount] = {};
};

// ================================================================================================================================================
// 6. WINDOWED COUNTERS OF THE COMPOSITOR (one line per race window; cheap integers only)
// ================================================================================================================================================
struct FrameRec {
    bool fast = false; Reason reason = kRSetup; Strategy strategy = kStratNone;
    uint32_t tilesConverted = 0, uploadBytes = 0, uploadCalls = 0, wantedTiles = 0, activeTiles = 0, changedTiles = 0;
    uint64_t convertedBytes = 0, scanUs = 0, convertUs = 0, uploadUs = 0;
    FastScanCounters scan;
};
struct CompWindow {
    uint32_t frames = 0, fastFrames = 0, fullFrames = 0, uploads = 0, ringCreates = 0, glErrors = 0;
    uint32_t reasons[kReasonCount] = {}; uint32_t strategies[5] = {};
    uint64_t tilesConverted = 0, convertedBytes = 0, uploadBytes = 0, uploadCalls = 0, wantedTiles = 0, activeTiles = 0, changedTiles = 0, scanUs = 0, convertUs = 0, uploadUs = 0;
    uint64_t fastScanUs = 0, fullScanUs = 0, scanExact = 0, scanPrevSkipped = 0, scanCompared = 0, scanOccupied = 0;
    void clear() { *this = CompWindow(); }
    void add(const FrameRec& r) {
        ++frames; if (r.fast) ++fastFrames; else ++fullFrames; ++reasons[r.reason];
        tilesConverted += r.tilesConverted; convertedBytes += r.convertedBytes; uploadBytes += r.uploadBytes; uploadCalls += r.uploadCalls; if (r.uploadCalls) ++uploads; ++strategies[r.strategy];
        wantedTiles += r.wantedTiles; activeTiles += r.activeTiles; changedTiles += r.changedTiles; scanUs += r.scanUs; convertUs += r.convertUs; uploadUs += r.uploadUs;
        if (r.fast) fastScanUs += r.scanUs; else fullScanUs += r.scanUs;
        scanExact += r.scan.tilesExact; scanPrevSkipped += r.scan.tilesSkippedPrev; scanCompared += r.scan.tilesCompared; scanOccupied += r.scan.tilesOccupiedScan;
    }
    void add_window(const CompWindow& o) {
        frames += o.frames; fastFrames += o.fastFrames; fullFrames += o.fullFrames; uploads += o.uploads; ringCreates += o.ringCreates; glErrors += o.glErrors;
        for (int i = 0; i < kReasonCount; ++i) reasons[i] += o.reasons[i];
        for (int i = 0; i < 5; ++i) strategies[i] += o.strategies[i];
        tilesConverted += o.tilesConverted; convertedBytes += o.convertedBytes; uploadBytes += o.uploadBytes; uploadCalls += o.uploadCalls; wantedTiles += o.wantedTiles; activeTiles += o.activeTiles; changedTiles += o.changedTiles;
        scanUs += o.scanUs; convertUs += o.convertUs; uploadUs += o.uploadUs; fastScanUs += o.fastScanUs; fullScanUs += o.fullScanUs;
        scanExact += o.scanExact; scanPrevSkipped += o.scanPrevSkipped; scanCompared += o.scanCompared; scanOccupied += o.scanOccupied;
    }
};
inline int format_comp_window(char* out, size_t n, const char* label, const CompWindow& w) {
    const double f = w.frames ? (double)w.frames : 1.0;
    int len = snprintf(out, n,
        "%s frames=%u fast=%u(%.1f%%) full_path=%u | per_frame: converted_kB=%.1f uploaded_kB=%.1f upload_calls=%.2f tiles{active=%.0f changed=%.0f converted=%.1f} | ms_per_frame: scan=%.2f (fast=%.2f/full=%.2f) convert=%.2f upload=%.2f | "
        "uploads=%u ring_creates=%u gl_errors=%u strategies{rects=%u bands=%u bbox=%u full=%u none=%u} | scan{tiles_key_only=%.0f prev_not_read=%.0f prev_compared=%.0f occupied_examined=%.0f per frame} | full_path_reasons{",
        label, w.frames, w.fastFrames, w.frames ? 100.0 * (double)w.fastFrames / f : 0.0, w.fullFrames,
        (double)w.convertedBytes / f / 1024.0, (double)w.uploadBytes / f / 1024.0, (double)w.uploadCalls / f, (double)w.activeTiles / f, (double)w.changedTiles / f, (double)w.tilesConverted / f,
        (double)w.scanUs / f / 1000.0, w.fastFrames ? (double)w.fastScanUs / (double)w.fastFrames / 1000.0 : 0.0, w.fullFrames ? (double)w.fullScanUs / (double)w.fullFrames / 1000.0 : 0.0,
        (double)w.convertUs / f / 1000.0, (double)w.uploadUs / f / 1000.0,
        w.uploads, w.ringCreates, w.glErrors, w.strategies[kStratRects], w.strategies[kStratBands], w.strategies[kStratBbox], w.strategies[kStratFull], w.strategies[kStratNone],
        (double)w.scanExact / f, (double)w.scanPrevSkipped / f, (double)w.scanCompared / f, (double)w.scanOccupied / f);
    bool any = false;
    for (int r = 1; r < kReasonCount && len > 0 && (size_t)len + 80 < n; ++r) if (w.reasons[r]) { len += snprintf(out + len, n - (size_t)len, "%s%s=%u", any ? " " : "", reason_name((uint32_t)r), w.reasons[r]); any = true; }
    if (len > 0 && (size_t)len + 4 < n) len += snprintf(out + len, n - (size_t)len, "%s}", any ? "" : "none");
    return len;
}

// ================================================================================================================================================
// 7. G145 PREPARATION: the cheap windowed scene-profile counters (no timer per call, no line per call, no heap)
// ================================================================================================================================================
// 7.1 batch-size histogram: triangles per draw call
constexpr int kBatchBuckets = 8;
struct BatchHist {
    uint32_t b[kBatchBuckets] = {};
    static int bucket(uint32_t n) { return n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : n <= 32 ? 5 : n <= 64 ? 6 : 7; }
    void add(uint32_t n) { ++b[bucket(n)]; }
    void clear() { memset(b, 0, sizeof b); }
    uint32_t total() const { uint32_t t = 0; for (int i = 0; i < kBatchBuckets; ++i) t += b[i]; return t; }
};
// 7.2 duplicate-triangle detector: one hash per triangle (geometry + colour + texture + uv), epoch-tagged table (no per-frame clearing)
class DupFilter {
public:
    static constexpr uint32_t kSize = 4096;   // power of two, > 2x the worst frame (1493 triangles seen)
    // true when an identical triangle (same hash) was already submitted in this epoch (frame)
    bool seen_or_insert(uint32_t h, uint32_t epoch) {
        uint32_t i = (h * 2654435761u) >> 20 & (kSize - 1u);
        for (uint32_t probe = 0; probe < 16; ++probe, i = (i + 1u) & (kSize - 1u)) {
            Entry& e = e_[i];
            if (e.epoch != epoch) { e.epoch = epoch; e.h = h; return false; }
            if (e.h == h) return true;
        }
        return false;   // table region full: counted as new (an underestimate of the duplicates, never an overestimate)
    }
private:
    struct Entry { uint32_t h = 0, epoch = 0; };
    Entry e_[kSize];
};
inline uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline uint32_t tri_hash(const float* verts6, const float* colors12, const float* uvs6, bool textured, uint32_t tex) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; h ^= h >> 15; };
    for (int i = 0; i < 6; ++i) mix(bits_of(verts6[i]));
    for (int i = 0; i < 12; ++i) mix(bits_of(colors12[i]));
    mix(textured ? tex : 0xFFFFFFFFu);
    if (textured) for (int i = 0; i < 6; ++i) mix(bits_of(uvs6[i]));
    return h;
}
// 7.3 per-Glide-call table (call counts are exact; time is sampled): the translator cost per API, and the "no effective state change" calls
struct ApiStat { const char* api = nullptr; uint64_t calls = 0, sampledCalls = 0, sampledUs = 0, redundant = 0; };
class ApiTable {
public:
    static constexpr int kMax = 48;
    // index of `api` (pointer identity: the names are string literals); -1 when the table is full
    int find_or_add(const char* api) {
        if (last_ >= 0 && s_[last_].api == api) return last_;
        for (int i = 0; i < n_; ++i) if (s_[i].api == api) { last_ = i; return i; }
        if (n_ >= kMax) return -1;
        s_[n_].api = api; last_ = n_; return n_++;
    }
    ApiStat& at(int i) { return s_[i]; }
    const ApiStat& at(int i) const { return s_[i]; }
    int size() const { return n_; }
    void clear_counts() { for (int i = 0; i < n_; ++i) { s_[i].calls = 0; s_[i].sampledCalls = 0; s_[i].sampledUs = 0; s_[i].redundant = 0; } }
    // estimated translator microseconds: exact call count x mean of the sampled calls
    double estimated_us(int i) const { const ApiStat& a = s_[i]; return a.sampledCalls ? (double)a.calls * ((double)a.sampledUs / (double)a.sampledCalls) : 0.0; }
private:
    ApiStat s_[kMax]; int n_ = 0, last_ = -1;
};
// 7.4 who closes the draw session: (GL wrapper kind, Glide call in progress) -> number of sessions closed
enum GlKind : uint8_t { kGkQuery = 0, kGkToggle, kGkBind, kGkTexEnv, kGkMatrix, kGkArray, kGkDraw, kGkColor, kGkTexData, kGkRead, kGkSwap, kGkClear, kGkOther, kGkCount };
inline const char* glkind_name(uint32_t k) {
    static const char* const n[kGkCount] = {"query(glGet*/glIsEnabled/glGetError)", "enable/disable", "bindTexture", "texEnv/texParameter", "matrix", "client-array/pointer", "drawArrays(other)", "colour/depth/cull/blend/alpha/scissor/mask state", "texture upload/gen/delete", "readPixels", "swap/finish/flush", "clear", "other"};
    return k < kGkCount ? n[k] : "?";
}
struct CauseStat { const char* api = nullptr; uint8_t kind = 0; uint32_t closes = 0; };
class CauseTable {
public:
    static constexpr int kMax = 64;
    void note(const char* api, uint8_t kind) {
        for (int i = 0; i < n_; ++i) if (s_[i].api == api && s_[i].kind == kind) { ++s_[i].closes; ++total_; return; }
        if (n_ < kMax) { s_[n_].api = api; s_[n_].kind = kind; s_[n_].closes = 1; ++n_; ++total_; } else { ++other_; ++total_; }
    }
    void clear() { n_ = 0; total_ = 0; other_ = 0; }
    int size() const { return n_; }
    const CauseStat& at(int i) const { return s_[i]; }
    uint32_t total() const { return total_; }
    uint32_t other() const { return other_; }
private:
    CauseStat s_[kMax]; int n_ = 0; uint32_t total_ = 0, other_ = 0;
};
// 7.5 the scene window: one set of integers per race window
struct SceneWindow {
    uint32_t frames = 0;
    uint64_t tris = 0, dupTris = 0, offscreenTris = 0, degenerateTris = 0, texturedTris = 0, textureChanges = 0, texturesSeen = 0;
    uint64_t sampledTris = 0, decodeUs = 0, translateUs = 0, submitUs = 0, sampledTotalUs = 0;
    uint64_t guestApiCalls = 0, frameUs = 0, sceneUs = 0, hostStagesUs = 0;
    BatchHist batch; uint64_t draws = 0, boundaryCloses = 0;
    void clear() { *this = SceneWindow(); }
    void add_window(const SceneWindow& o) {
        frames += o.frames; tris += o.tris; dupTris += o.dupTris; offscreenTris += o.offscreenTris; degenerateTris += o.degenerateTris; texturedTris += o.texturedTris; textureChanges += o.textureChanges; texturesSeen += o.texturesSeen;
        sampledTris += o.sampledTris; decodeUs += o.decodeUs; translateUs += o.translateUs; submitUs += o.submitUs; sampledTotalUs += o.sampledTotalUs; guestApiCalls += o.guestApiCalls;
        frameUs += o.frameUs; sceneUs += o.sceneUs; hostStagesUs += o.hostStagesUs; draws += o.draws; boundaryCloses += o.boundaryCloses; for (int i = 0; i < kBatchBuckets; ++i) batch.b[i] += o.batch.b[i];
    }
};
inline int format_scene_profile(char* out, size_t n, int windowIdx, const SceneWindow& w, double translatorEstUsPerFrame) {
    const double f = w.frames ? (double)w.frames : 1.0, t = w.tris ? (double)w.tris : 1.0, s = w.sampledTris ? (double)w.sampledTris : 1.0;
    const double perFrameMs = (double)w.frameUs / f / 1000.0, sceneMs = (double)w.sceneUs / f / 1000.0, transMs = translatorEstUsPerFrame / 1000.0;
    return snprintf(out, n,
        "win=%d frames=%u | per_frame: tris=%.0f draws=%.0f tris_per_draw=%.2f textured=%.0f tex_changes=%.1f glide_calls=%.0f session_closes_by_other_gl_calls=%.1f | "
        "triangles{identical_resubmission=%llu(%.1f%%) fully_offscreen=%llu(%.1f%%) zero_area=%llu(%.1f%%)} | batch_size(tris per draw){1=%u 2=%u 3-4=%u 5-8=%u 9-16=%u 17-32=%u 33-64=%u 65+=%u} | "
        "sampled_1_in_16_triangles(%llu): translator_us{decode+validate=%.2f state+uv+colour=%.2f submit(vertex copy + any flush)=%.2f total=%.2f} | "
        "split_ms_per_frame: frame=%.2f scene(first triangle..swap)=%.2f of which Glide translator(estimated: calls x sampled mean)=%.2f and guest code+untimed=%.2f",
        windowIdx, w.frames, (double)w.tris / f, (double)w.draws / f, w.draws ? (double)w.tris / (double)w.draws : 0.0, (double)w.texturedTris / f, (double)w.textureChanges / f, (double)w.guestApiCalls / f, (double)w.boundaryCloses / f,
        (unsigned long long)w.dupTris, 100.0 * (double)w.dupTris / t, (unsigned long long)w.offscreenTris, 100.0 * (double)w.offscreenTris / t, (unsigned long long)w.degenerateTris, 100.0 * (double)w.degenerateTris / t,
        w.batch.b[0], w.batch.b[1], w.batch.b[2], w.batch.b[3], w.batch.b[4], w.batch.b[5], w.batch.b[6], w.batch.b[7],
        (unsigned long long)w.sampledTris, (double)w.decodeUs / s, (double)w.translateUs / s, (double)w.submitUs / s, (double)w.sampledTotalUs / s,
        perFrameMs, sceneMs, transMs, sceneMs > transMs ? sceneMs - transMs : 0.0);
}
// top-N lines of a table, one string (no line per call): the Glide calls ranked by estimated translator time, and the session-closing causes ranked by count
inline int format_api_cost(char* out, size_t n, int windowIdx, const ApiTable& t, uint32_t frames, int topN) {
    const double f = frames ? (double)frames : 1.0;
    bool used[ApiTable::kMax] = {};
    int len = snprintf(out, n, "win=%d per_frame (calls, redundant=no effective state change, estimated_translator_us) top by estimated time: ", windowIdx);
    for (int k = 0; k < topN; ++k) {
        int best = -1; double bv = -1.0;
        for (int i = 0; i < t.size(); ++i) if (!used[i] && t.estimated_us(i) > bv) { bv = t.estimated_us(i); best = i; }
        if (best < 0 || len < 0 || (size_t)len + 120 >= n) break;
        used[best] = true; const ApiStat& a = t.at(best);
        len += snprintf(out + len, n - (size_t)len, "%s%s{calls=%.1f redundant=%.1f est_us=%.0f mean_sampled_us=%.2f}", k ? " " : "", a.api ? a.api : "?", (double)a.calls / f, (double)a.redundant / f, bv / f, a.sampledCalls ? (double)a.sampledUs / (double)a.sampledCalls : 0.0);
    }
    return len;
}
inline int format_causes(char* out, size_t n, int windowIdx, const CauseTable& t, uint32_t frames, int topN) {
    const double f = frames ? (double)frames : 1.0;
    bool used[CauseTable::kMax] = {};
    int len = snprintf(out, n, "win=%d session closes per frame=%.1f (draw session ended by another GL call; cause = GL call kind / Glide call in progress): ", windowIdx, (double)t.total() / f);
    for (int k = 0; k < topN; ++k) {
        int best = -1; uint32_t bv = 0;
        for (int i = 0; i < t.size(); ++i) if (!used[i] && t.at(i).closes > bv) { bv = t.at(i).closes; best = i; }
        if (best < 0 || len < 0 || (size_t)len + 140 >= n) break;
        used[best] = true; const CauseStat& c = t.at(best);
        len += snprintf(out + len, n - (size_t)len, "%s%s<-%s=%.1f", k ? " | " : "", glkind_name(c.kind), c.api ? c.api : "(outside a Glide call)", (double)c.closes / f);
    }
    if (t.other() && len > 0 && (size_t)len + 40 < n) len += snprintf(out + len, n - (size_t)len, " | table_full_other=%.1f", (double)t.other() / f);
    return len;
}

}}  // namespace ign::g144
