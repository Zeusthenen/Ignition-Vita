// win32/g149_scan.h -- G149: the incremental compositor scan with 128-bit vectors (pure: no GL, no guest access, no logging; tests: tools/g149_test.cpp)
//
// What the real G148 run showed: the fast scan of the LFB shadow (ign::g144::scan_fast) costs 3.8-4.1 ms per race frame although only 17-20 of 1,000 tiles change (G144-FASTPATH-RACE: scan=3.81 ms of a 4.2 ms compositor).
// It reads the 640x400 RGB565 buffer (512,000 bytes) once, sequentially, in 64-bit words: on the Vita's 32-bit ARM core every 64-bit word is two loads, two XORs and an OR chain that goes through an accumulator array
// in memory. The result of that first pass is only a BOOLEAN per tile ("the 16x16 pixels are exactly the key"), and the two tile tests of the second pass (tile_equal, tile_occupied_mask) are pure byte predicates.
//
// scan_fast_v is scan_fast with exactly those three computations done on 128-bit vectors (GCC generic vectors: NEON on the Vita, SSE on the host, the SAME source in both). Nothing else changed: the same loops in the same
// order, the same state updates, the same counters. It is bit-identical by construction and proven so by tools/g149_test.cpp (random frames, key regions, noise, every output compared with scan_fast). The integration
// also re-runs the scalar scan on copies every 256th scan and falls back to it for good on the first difference (the LIGHT build's own check; the test is the proof).
#pragma once
#include <cstdint>
#include <cstring>
#include "win32/g144_fast.h"

namespace ign { namespace g149 {

namespace K = ign::g113;
typedef uint32_t v4u __attribute__((vector_size(16)));
inline v4u ld16(const uint8_t* p) { v4u v; memcpy(&v, p, 16); return v; }
inline v4u rep8(uint16_t v16) { const uint32_t w = (uint32_t)v16 * 0x00010001u; const v4u r = {w, w, w, w}; return r; }
inline bool any_set(v4u v) { return (v[0] | v[1] | v[2] | v[3]) != 0u; }

// The three tile predicates accumulate the 16 rows in vector registers and reduce ONCE at the end (no early exit per row): on the Cortex-A9 a NEON -> ARM transfer is a ~20-cycle pipeline stall, so a per-row reduction would cost
// more than the 64-bit scalar loop it replaces. The boolean result is the same as the scalar early-exit loops by construction (an OR over the same bytes).
// the 16 rows of tile (ty,tx) of `a` and `b` are identical
inline bool tile_equal_v(const uint8_t* a, const uint8_t* b, uint32_t stride, int ty, int tx) {
    v4u acc = {0u, 0u, 0u, 0u};
    for (int row = 0; row < K::kTileH; ++row) {
        const size_t off = (size_t)(ty * K::kTileH + row) * stride + (size_t)tx * K::kTileW * 2;
        acc |= (ld16(a + off) ^ ld16(b + off)) | (ld16(a + off + 16) ^ ld16(b + off + 16));
    }
    return !any_set(acc);
}
// at least one pixel of the tile is not the key under the tolerant mask
inline bool tile_occupied_v(const uint8_t* cur, uint32_t stride, int ty, int tx, v4u kv, v4u mv) {
    v4u acc = {0u, 0u, 0u, 0u};
    for (int row = 0; row < K::kTileH; ++row) {
        const uint8_t* p = cur + (size_t)(ty * K::kTileH + row) * stride + (size_t)tx * K::kTileW * 2;
        acc |= ((ld16(p) ^ kv) | (ld16(p + 16) ^ kv)) & mv;
    }
    return any_set(acc);
}

// Same contract and the same results as ign::g144::scan_fast.
inline void scan_fast_v(const uint8_t* cur, const uint8_t* prev, uint32_t stride, uint16_t key, uint16_t cmpMask, bool cleanLayer,
                        K::MaskArrays& m, K::ScanStats& st, ign::g144::FastScanState& fs, ign::g144::FastScanCounters* fc = nullptr) {
    const v4u kv = rep8(key), mv = rep8(cmpMask);
    const bool flagsOk = fs.valid && fs.key == key && prev != nullptr;
    bool curExact[K::kTiles];
    for (int ty = 0; ty < K::kRows; ++ty) {
        v4u acc[K::kCols];
        for (int tx = 0; tx < K::kCols; ++tx) { const v4u z = {0u, 0u, 0u, 0u}; acc[tx] = z; }
        for (int row = 0; row < K::kTileH; ++row) {
            const uint8_t* r = cur + (size_t)(ty * K::kTileH + row) * stride;
            for (int tx = 0; tx < K::kCols; ++tx) {
                const uint8_t* p = r + (size_t)tx * K::kTileW * 2;
                __builtin_prefetch(p + 2 * (size_t)stride, 0, 0);
                acc[tx] |= (ld16(p) ^ kv) | (ld16(p + 16) ^ kv);
            }
        }
        for (int tx = 0; tx < K::kCols; ++tx) {
            const int idx = ty * K::kCols + tx;
            const bool exact = !any_set(acc[tx]);
            bool occupied = false, tileChanged;
            if (exact) {
                if (prev == nullptr) tileChanged = true;
                else if (flagsOk && fs.exact[idx]) { tileChanged = false; if (fc) ++fc->tilesSkippedPrev; }
                else { tileChanged = !tile_equal_v(prev, cur, stride, ty, tx); if (fc) ++fc->tilesCompared; }
                if (fc) ++fc->tilesExact;
            } else {
                occupied = tile_occupied_v(cur, stride, ty, tx, kv, mv);
                tileChanged = prev == nullptr || !tile_equal_v(prev, cur, stride, ty, tx);
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
    memcpy(fs.exact, curExact, sizeof fs.exact);
    fs.valid = true; fs.key = key;
}

// ---- the result of one scan, comparable (the in-build self-check and the test) ---------------------------------------------------------------------------------------
struct ScanResult {
    bool active[K::kTiles], retiredLast[K::kTiles], changed[K::kTiles], occupied[K::kTiles];
    uint32_t activeCount;
    K::ScanStats st;
    bool fsExact[K::kTiles]; bool fsValid; uint16_t fsKey;
    ign::g144::FastScanCounters fc;
};
inline bool same_scan(const ScanResult& a, const ScanResult& b) {
    return a.activeCount == b.activeCount && memcmp(a.active, b.active, sizeof a.active) == 0 && memcmp(a.retiredLast, b.retiredLast, sizeof a.retiredLast) == 0 && memcmp(a.changed, b.changed, sizeof a.changed) == 0 &&
           memcmp(a.occupied, b.occupied, sizeof a.occupied) == 0 && a.st.tilesNew == b.st.tilesNew && a.st.tilesRetired == b.st.tilesRetired && a.st.tilesStillVisible == b.st.tilesStillVisible && a.st.tilesReappeared == b.st.tilesReappeared &&
           a.st.changedTiles == b.st.changedTiles && a.st.occupiedTiles == b.st.occupiedTiles && a.st.changedBytes == b.st.changedBytes && a.st.unchangedOccupiedNotActive == b.st.unchangedOccupiedNotActive &&
           a.st.minCol == b.st.minCol && a.st.maxCol == b.st.maxCol && a.st.minRow == b.st.minRow && a.st.maxRow == b.st.maxRow &&
           memcmp(a.fsExact, b.fsExact, sizeof a.fsExact) == 0 && a.fsValid == b.fsValid && a.fsKey == b.fsKey &&
           a.fc.tilesExact == b.fc.tilesExact && a.fc.tilesSkippedPrev == b.fc.tilesSkippedPrev && a.fc.tilesCompared == b.fc.tilesCompared && a.fc.tilesOccupiedScan == b.fc.tilesOccupiedScan;
}

}}   // namespace ign::g149
