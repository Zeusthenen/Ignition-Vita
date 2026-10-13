// win32/g155_keyfast.h -- iteration-G155, PURE (no GL, no Vita API, no guest access, no logging, no heap, no global state; tools/g155_test.cpp drives exactly this code).
//
// WHAT THE G154 REAL RUN SAID (tmp/G154_REAL_RUN_LOGS, five races, 10,795 frames, 1,709,196 keyed triangles; analysis: tmp/G155_COST_ANALYSIS.txt):
//  * the keyed resolution was already cached (99.96 % hits) but it ran for EVERY visible keyed triangle: lookup + mask compare + counters + a clock pair for 1 triangle in 64 (3.8 us mean, mostly the clock);
//  * the draw session switched the alpha test on and off around every run of keyed draws (643,150 + 643,150) and the texture environment REPLACE <-> MODULATE (271,189 + 271,189) although a run is 2.1 draws on average
//    and the draws between two runs are plain textured draws whose fragment alpha is 1 (every game texture is GL_RGB 565): the alpha test and MODULATE with white vertex colours cannot change them;
//  * 550 of the 658 variant builds were rebuilds "because the mask changed": the mask is the set of palette indices equal to the key, the variant only depends on (mask AND the indices the texture uses): a palette
//    change that moves other indices does not change a single texel of the variant;
//  * the variants of one race are freed with the textures at the next reset and built again for the next race from the very same content.
// WHAT THIS HEADER ADDS (on top of win32/g154_keycache.h, which stays the reference of G154):
//  * Memo: the result of the resolution of the LAST keyed triangle (texture, texture generation, key, palette generation, epoch of every texture / variant change). The next triangle with the same inputs costs six compares
//    and two increments; the full resolution runs once per change (= once per group in practice). The epoch is bumped by everything that can alter a decision: a download, a refuse, a build, a free, a pool adoption.
//  * TexKey2: the variant is current while (content generation, SELECTED indices = used AND mask) are the ones it was built for. A mask change that leaves the selection alone costs nothing and rebuilds nothing; an empty
//    selection is a plain draw that KEEPS the variant for the next palette.
//  * VariantPool: a bounded LRU (16 entries = 2 MiB) of GL textures of freed variants, keyed by (content hash of index image + translation, selection, sampler state): the next race that downloads the same particle texture
//    adopts it instead of converting and uploading again.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g154_keycache.h"

namespace ign { namespace g155 {

using ign::g154::Bits256;
using ign::g154::intersects;
using ign::g154::PaletteTracker;
using ign::g154::MaskCache;
using ign::g154::kWhyCount;
using ign::g154::kWhyNone;
using ign::g154::kWhyNoMask;
using ign::g154::kWhyNoKeyedTexel;
using ign::g154::kWhyNoIndexImage;
using ign::g154::kWhyIdxNotKept;
using ign::g154::kWhyGlError;
using ign::g154::Cause;
using ign::g154::kCauseNew;
using ign::g154::kCauseContent;
using ign::g154::kCauseMask;
using ign::g154::kCauseCount;

inline Bits256 and_bits(const Bits256& a, const Bits256& b) { Bits256 r; for (int i = 0; i < 4; ++i) r.w[i] = a.w[i] & b.w[i]; return r; }

// 64-bit digest of an index image + its translation table (8 bytes per step; the image is a multiple of 8 bytes). NOT cryptographic: it only keys the pool, and a pool hit is also checked on the selection and the sampler state.
inline uint64_t content_hash(const uint8_t* idx, uint32_t n, const uint8_t xlat[256]) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, idx + i, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
    for (; i < n; ++i) h = (h ^ idx[i]) * 0x100000001b3ull;
    for (uint32_t j = 0; j < 256; j += 8) { uint64_t w; memcpy(&w, xlat + j, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
    return h ? h : 1ull;   // 0 = "no hash"
}

// the A/B token g155=classic in ux0:data/ignition/g146_geo.txt (text with newlines / tabs already turned into spaces): the draw session switches the alpha test and the texture environment draw by draw as G154 did (same pixels, more GL calls)
inline bool classic_from_text(const char* text) {
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == 12 && memcmp(p, "g155=classic", 12) == 0) return true;
        p = e;
    }
    return false;
}
// ---- the per-texture state ------------------------------------------------------------------------------------------------------------------------------------------------------------
struct TexKey2 {
    bool usedValid = false, idxKept = false; Bits256 used; uint32_t why = kWhyNoIndexImage;
    uint32_t contentGen = 0; uint64_t hash = 0;                         // contentGen bumps only when the index image or its translation REALLY changes; hash = content_hash() of the kept image (0 = none)
    bool decValid = false, decNeeds = false; uint32_t decSerial = 0, decGen = 0, decWhy = kWhyNone; Bits256 decSel;   // the decision for (mask serial, content generation): needs a variant for the selection decSel?
    uint32_t varGl = 0, varGen = 0xFFFFFFFFu; Bits256 varSel; uint64_t uses = 0; uint32_t filt = 0;   // the variant: built for (content generation, selection); filt = the sampler state it was created with
};
enum Action : uint8_t { kPlain = 0, kUse, kBuild };
// the whole decision for a keyed draw from the texture's own state and the current mask: O(1), no scan
inline Action resolve(TexKey2& t, uint32_t maskSerial, const Bits256& mask, uint32_t& why, bool& cached) {
    why = kWhyNone; cached = true;
    if (!t.usedValid) { why = t.why; return kPlain; }
    if (!(t.decValid && t.decSerial == maskSerial && t.decGen == t.contentGen)) {
        cached = false; t.decValid = true; t.decSerial = maskSerial; t.decGen = t.contentGen;
        const Bits256 sel = and_bits(t.used, mask);
        if (sel.empty()) { t.decNeeds = false; t.decWhy = kWhyNoKeyedTexel; }
        else if (!t.idxKept) { t.decNeeds = false; t.decWhy = kWhyIdxNotKept; }
        else { t.decNeeds = true; t.decWhy = kWhyNone; t.decSel = sel; }
    }
    if (!t.decNeeds) { why = t.decWhy; return kPlain; }
    if (t.varGl != 0u && t.varGen == t.contentGen && t.varSel == t.decSel) return kUse;
    return kBuild;
}
inline void note_built(TexKey2& t, uint32_t glId) { t.varGl = glId; t.varGen = t.contentGen; t.varSel = t.decSel; }
inline void note_failed(TexKey2& t) { t.decNeeds = false; t.decWhy = kWhyGlError; }   // no retry on every triangle: the next selection / content change decides again
inline Cause build_cause(const TexKey2& t) { return t.varGl == 0u ? kCauseNew : (t.varGen != t.contentGen ? kCauseContent : kCauseMask); }

// ---- the bounded pool of variants of freed textures ----------------------------------------------------------------------------------------------------------------------------------
class VariantPool {
public:
    static const int kCap = 16;
    // a pool entry for exactly this content / selection / sampler state is removed and its GL texture returned (0 = none)
    uint32_t take(uint64_t hash, const Bits256& sel, uint32_t filt) {
        if (hash == 0) return 0;
        for (int i = 0; i < kCap; ++i) if (e_[i].gl && e_[i].hash == hash && e_[i].filt == filt && e_[i].sel == sel) { const uint32_t g = e_[i].gl; e_[i].gl = 0; --live_; ++hits_; return g; }
        ++misses_; return 0;
    }
    // keeps a freed variant; returns the GL texture it had to evict (the caller deletes it), 0 if nothing was evicted. hash == 0 (nothing known about the content): not kept, returned as evicted.
    uint32_t put(uint64_t hash, const Bits256& sel, uint32_t filt, uint32_t gl) {
        if (gl == 0) return 0;
        if (hash == 0) return gl;
        int slot = -1; uint32_t evicted = 0;
        for (int i = 0; i < kCap; ++i) if (e_[i].gl == 0) { slot = i; break; }
        if (slot < 0) { slot = 0; for (int i = 1; i < kCap; ++i) if (e_[i].stamp < e_[slot].stamp) slot = i; evicted = e_[slot].gl; --live_; ++evictions_; }
        e_[slot].hash = hash; e_[slot].sel = sel; e_[slot].filt = filt; e_[slot].gl = gl; e_[slot].stamp = ++clock_; ++live_; ++puts_;
        if (live_ > peak_) peak_ = live_;
        return evicted;
    }
    // every GL texture still in the pool (for the final release); each call returns one and removes it, 0 when empty
    uint32_t drain_one() { for (int i = 0; i < kCap; ++i) if (e_[i].gl) { const uint32_t g = e_[i].gl; e_[i].gl = 0; --live_; return g; } return 0; }
    uint32_t live() const { return live_; }
    uint32_t peak() const { return peak_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t puts() const { return puts_; }
    uint64_t evictions() const { return evictions_; }
private:
    struct Ent { uint64_t hash = 0; Bits256 sel; uint32_t filt = 0, gl = 0; uint64_t stamp = 0; };
    Ent e_[kCap]; uint64_t clock_ = 0, hits_ = 0, misses_ = 0, puts_ = 0, evictions_ = 0; uint32_t live_ = 0, peak_ = 0;
};

// ---- counters -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Stats {
    uint64_t requested = 0, materialized = 0, plain[kWhyCount] = {};          // requested = keyed-state triangles that reached the resolution (visible ones only); materialized = real resolutions (memo misses)
    uint64_t texCached = 0, texComputed = 0;                                   // of the materializations: decision cached / computed
    uint64_t builds = 0, buildCause[kCauseCount] = {}, buildFails = 0, poolReuse = 0, convertUs = 0, uploadUs = 0, buildBytes = 0, texelsKeyed = 0;
    uint64_t downloads = 0, idxKept = 0, idxSkipped = 0, sameContent = 0, newContent = 0, refused = 0, hashed = 0, hashUs = 0;
    uint64_t freedVariants = 0, freedNeverUsed = 0, poolPut = 0, poolEvict = 0, poolTaken = 0;
    uint64_t rejKeyOn = 0;                                                     // triangles rejected by the viewport / scissor tests while the Glide chroma key was enabled (no keyed work was done for them)
    uint64_t peakVariants = 0, liveVariants = 0;
    double calHitNs = 0.0, calMissNs = 0.0;                                   // calibration at the summary (a private resolver run 20,000 hits / 2,000 misses; never in the triangle path): what one hit / one materialization costs on THIS machine
    uint64_t idxLiveBytes = 0, idxPeakBytes = 0;                              // index images kept (the peak is the memory the chroma key ever needed for them)
};
struct SessionIn {
    uint64_t draws = 0, tris = 0, keyedDraws = 0, keyedTris = 0, transTris = 0, alphaOn = 0, alphaOff = 0, offForced = 0, offHorizon = 0, offEnd = 0, neutralKept = 0, envMod = 0, envEnd = 0, keyedFlushes = 0, logicalOnlyFlushes = 0, transDepthWrite = 0;
};
struct GlIn { uint64_t alpha = 0, texenv = 0, bind = 0, draw = 0; };          // the GL calls really issued by the draw-session adapter (glEnable + glAlphaFunc / glDisable count as one alpha call)
struct MemIn { uint64_t textures = 0, idxTextures = 0, idxBytes = 0, variantTextures = 0, variantBytes = 0, variantsUnused = 0, poolLive = 0, poolPeak = 0; };

inline size_t why_list(char* out, size_t cap, const uint64_t* w) {
    size_t p = 0; out[0] = 0; bool any = false;
    for (uint32_t i = 1; i < kWhyCount; ++i) if (w[i] && p + 80 < cap) { p += (size_t)snprintf(out + p, cap - p, "%s%s=%llu", any ? " " : "", ign::g154::why_name(i), (unsigned long long)w[i]); any = true; }
    if (!any) snprintf(out, cap, "none");
    return strlen(out);
}
// the line is written as TWO log lines (the logger caps a line at 2048 characters): A = what was resolved and built, B = what the draw session did to GL
inline int format_summary(char* out, size_t n, const Stats& s, const MemIn& m, uint64_t paletteUpdates, uint64_t paletteChanged, uint64_t maskHits, uint64_t maskComputes, uint64_t maskChanges, uint64_t keyedTrisSession, uint64_t transTris, bool final) {
    char why[360]; why_list(why, sizeof why, s.plain);
    uint64_t plainTris = 0; for (uint32_t i = 1; i < kWhyCount; ++i) plainTris += s.plain[i];
    const uint64_t keyedTris = s.requested >= plainTris ? s.requested - plainTris : 0;
    const uint64_t memoHits = s.requested >= s.materialized ? s.requested - s.materialized : 0;
    return snprintf(out, n,
        "%s keyed{tris=%llu session_tris=%llu translucent=%llu cutout=%llu} left_as_before{%llu: %s} | resolve{requested=%llu materialized=%llu memo_hits=%llu avoided_by_batching=%llu (%.4f%%) decision_cached=%llu computed=%llu "
        "rejected_before_any_keyed_work=%llu calibrated_ns{hit=%.1f materialize=%.1f}} palette{updates=%llu changed=%llu} mask{cached=%llu computed=%llu changed=%llu} | "
        "variants{builds=%llu created=%llu content=%llu mask=%llu pool_reuse=%llu failed=%llu convert_ms=%.2f upload_ms=%.2f bytes=%llu texels_keyed=%llu freed=%llu never_used=%llu pool{put=%llu evicted=%llu taken=%llu}} | "
        "downloads{total=%llu index_kept=%llu index_skipped_no_key_candidate=%llu same_content=%llu changed_content=%llu refused=%llu hashed=%llu hash_ms=%.2f} | "
        "memory{textures=%llu index_images=%llu (%llu B) peak_index_bytes=%llu variants=%llu (%llu B) variants_unused=%llu peak_variants=%llu pool_live=%llu pool_peak=%llu} errors{gl_build_fail=%llu}",
        final ? "FINAL" : "RACE", (unsigned long long)keyedTris, (unsigned long long)keyedTrisSession, (unsigned long long)transTris, (unsigned long long)(keyedTrisSession >= transTris ? keyedTrisSession - transTris : 0), (unsigned long long)plainTris, why,
        (unsigned long long)s.requested, (unsigned long long)s.materialized, (unsigned long long)memoHits, (unsigned long long)memoHits, s.requested ? 100.0 * (double)memoHits / (double)s.requested : 0.0,
        (unsigned long long)s.texCached, (unsigned long long)s.texComputed, (unsigned long long)s.rejKeyOn, s.calHitNs, s.calMissNs,
        (unsigned long long)paletteUpdates, (unsigned long long)paletteChanged, (unsigned long long)maskHits, (unsigned long long)maskComputes, (unsigned long long)maskChanges,
        (unsigned long long)s.builds, (unsigned long long)s.buildCause[kCauseNew], (unsigned long long)s.buildCause[kCauseContent], (unsigned long long)s.buildCause[kCauseMask], (unsigned long long)s.poolReuse, (unsigned long long)s.buildFails,
        (double)s.convertUs / 1000.0, (double)s.uploadUs / 1000.0, (unsigned long long)s.buildBytes, (unsigned long long)s.texelsKeyed, (unsigned long long)s.freedVariants, (unsigned long long)s.freedNeverUsed,
        (unsigned long long)s.poolPut, (unsigned long long)s.poolEvict, (unsigned long long)s.poolTaken,
        (unsigned long long)s.downloads, (unsigned long long)s.idxKept, (unsigned long long)s.idxSkipped, (unsigned long long)s.sameContent, (unsigned long long)s.newContent, (unsigned long long)s.refused, (unsigned long long)s.hashed, (double)s.hashUs / 1000.0,
        (unsigned long long)m.textures, (unsigned long long)m.idxTextures, (unsigned long long)m.idxBytes, (unsigned long long)s.idxPeakBytes, (unsigned long long)m.variantTextures, (unsigned long long)m.variantBytes, (unsigned long long)m.variantsUnused,
        (unsigned long long)s.peakVariants, (unsigned long long)m.poolLive, (unsigned long long)m.poolPeak, (unsigned long long)s.buildFails);
}
inline int format_session(char* out, size_t n, const SessionIn& d, const GlIn& g, bool final) {
    return snprintf(out, n,
        "%s session{draws=%llu keyed_draws=%llu avg_batch=%.2f keyed_avg_batch=%.2f alpha_on=%llu off=%llu (forced_by_untextured=%llu horizon=%llu session_end=%llu) neutral_draws_kept_on=%llu env_modulate=%llu env_restore=%llu "
        "flushes_by_keyed_state=%llu logical_only_flushes=%llu translucent_keyed_tris=%llu translucent_keyed_draws_with_depth_write=%llu} | gl_calls_real{alpha_test=%llu texenv=%llu bind_texture=%llu draw=%llu}",
        final ? "FINAL" : "RACE", (unsigned long long)d.draws, (unsigned long long)d.keyedDraws, d.draws ? (double)d.tris / (double)d.draws : 0.0, d.keyedDraws ? (double)d.keyedTris / (double)d.keyedDraws : 0.0,
        (unsigned long long)d.alphaOn, (unsigned long long)d.alphaOff, (unsigned long long)d.offForced, (unsigned long long)d.offHorizon, (unsigned long long)d.offEnd, (unsigned long long)d.neutralKept,
        (unsigned long long)d.envMod, (unsigned long long)d.envEnd, (unsigned long long)d.keyedFlushes, (unsigned long long)d.logicalOnlyFlushes, (unsigned long long)d.transTris, (unsigned long long)d.transDepthWrite,
        (unsigned long long)g.alpha, (unsigned long long)g.texenv, (unsigned long long)g.bind, (unsigned long long)g.draw);
}
// ---- the resolver: everything the console does per keyed triangle, and nothing else (the console passes the allocation lookup and the real build; the host test passes fakes) ----------------
// Four memo entries (indexed by the low bits of the texture id) so that a run of particles of two or three textures does not thrash a single entry. An entry also remembers the texture's state record and its owner (the allocation), so a
// miss caused by the palette or the key does not search the allocation map again.
struct Memo { bool valid = false, ok = false; uint32_t mmid = 0, texGen = 0, key = 0, palGen = 0, epoch = 0, gl = 0, why = kWhyNone; TexKey2* t = nullptr; void* owner = nullptr; };
class Resolver {
public:
    PaletteTracker pt; MaskCache mc; Stats st; VariantPool pool; uint32_t epoch = 1;
    void touch() { ++epoch; }                    // a texture / variant changed in a way a memoised decision may not survive
    void invalidate_memo() { for (int i = 0; i < 4; ++i) memo_[i].valid = false; negValid_ = false; }
    // true = draw with `glId` (the keyed variant); false = leave the draw as G152 drew it.
    //   lookup(mmid, void*& owner) -> TexKey2* (null when the texture is unknown); build(TexKey2&, const Bits256& selection, void* owner) -> bool (uploads, calls note_built)
    template <class Lookup, class Build>
    bool resolve_draw(uint32_t mmid, uint32_t texGen, uint32_t keyRgb, uint32_t& glId, Lookup&& lookup, Build&& build) {
        ++st.requested;
        if (negValid_ && negKey_ == keyRgb && negPal_ == pt.gen()) { ++st.plain[kWhyNoMask]; return false; }   // the global negative: no palette entry equals the key, whatever the texture
        Memo& m = memo_[mmid & 3u];
        if (m.valid && m.mmid == mmid && m.texGen == texGen) {
            if (m.key == keyRgb && m.palGen == pt.gen() && m.epoch == epoch) {
                if (m.ok) { glId = m.gl; return true; }
                ++st.plain[m.why]; return false;
            }
        }
        ++st.materialized;
        const Bits256& mask = mc.get(pt, keyRgb);
        negValid_ = mask.empty(); negKey_ = keyRgb; negPal_ = pt.gen();
        uint32_t why = kWhyNoMask; bool ok = false; TexKey2* t = nullptr; void* owner = nullptr;
        if (!mask.empty()) {   // the global negative: no palette entry equals the key -> nothing is keyed, no texture is looked at
            if (m.valid && m.mmid == mmid && m.texGen == texGen && m.t) { t = m.t; owner = m.owner; }   // the texture is the one this entry resolved before: its record is still the right one
            else t = lookup(mmid, owner);
            if (!t) why = kWhyNoIndexImage;
            else {
                bool cached = true; Action act = resolve(*t, mc.serial(), mask, why, cached);
                if (cached) ++st.texCached; else ++st.texComputed;
                if (act == kBuild) { if (build(*t, t->decSel, owner)) act = kUse; else { act = kPlain; why = kWhyGlError; } }
                if (act == kUse) { ++t->uses; ok = true; }
            }
        }
        // the build / the decision may have changed the epoch: the entry records the epoch AFTER them
        m.valid = (why != kWhyNoIndexImage || t != nullptr); m.ok = ok; m.mmid = mmid; m.texGen = texGen; m.key = keyRgb; m.palGen = pt.gen(); m.epoch = epoch; m.why = why < kWhyCount ? why : 0u; m.t = t; m.owner = owner;
        if (ok) { glId = t->varGl; m.gl = glId; return true; }
        ++st.plain[m.why]; return false;
    }
    const Memo& memo_of(uint32_t mmid) const { return memo_[mmid & 3u]; }
private:
    Memo memo_[4]; bool negValid_ = false; uint32_t negKey_ = 0, negPal_ = 0;
};

} }   // namespace ign::g155