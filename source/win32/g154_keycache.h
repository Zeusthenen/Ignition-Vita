// win32/g154_keycache.h -- iteration-G154, PURE (no GL, no Vita API, no guest access, no logging, no heap, no global state; tools/g154_test.cpp drives exactly this code).
//
// WHAT G153 COST AND WHY (the G153 real run, tmp/G153_REAL_RUN_LOGS; the analysis: tmp/G154_COST_ANALYSIS.txt):
//  * the keyed path itself is a handful of compares per keyed triangle; the scene cost per drawn triangle in the G153 run (47.6 us Moosejaw, 46.6 us Yodel) is inside the range of the G151 / G152 runs (46.9 - 52.6 us): what changed between
//    the runs is the number of triangles per frame (G153: 516 / 577; G151: 421 / 531; G152: 380 / 498), i.e. the route, the cars and the particles, not the cost of a triangle;
//  * what G153 DID waste: (1) 40 of its 87 variant uploads were REBUILDS: the variant was keyed on the chroma-key VALUE and the value moves while the palette fades (menu / loading: 0xFFFFFF, 0xFCFCFC, 0xA5A5A5 ... 0x000000), each value
//    rebuilt every variant that had any palette entry equal to it; (2) half of the variants (probe lines: mmid 1, 13, 6, 23, 25 ...) had NO keyed texel at all: they were built, bound and alpha-tested for nothing and made the session switch
//    the alpha test on and off around them; (3) the palette was scanned (256 compares + a map lookup) every time the one-entry cache missed (72,698 draws "left as before" and every texture change of a keyed run); (4) every one of the
//    143 downloads kept a 64 KiB index image and a 1 KiB palette snapshot, whether the texture could ever be keyed or not; (5) 24 probe lines built strings on the first appearance of each texture.
//  * the fix is semantic AND exact: the Voodoo keys a texel when palette[index] (the palette IN EFFECT when the pixel is drawn) equals the key. Game: key = palette[0] of that same palette, so the keyed indices are the ones that share
//    the colour of index 0 in the current palette: a MASK of at most 256 bits that is identical for every texture, changes only when the palette really changes, and is O(1) to test against a texture's 256-bit "indices used" set.
//    A variant is needed only when (indices used) AND (mask) is not empty, and is rebuilt only when the mask CONTENT or the texture CONTENT really changes.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g153_key.h"

namespace ign { namespace g154 {

struct Bits256 {
    uint64_t w[4] = {0, 0, 0, 0};
    void set(uint32_t i) { w[(i >> 6) & 3u] |= 1ull << (i & 63u); }
    bool test(uint32_t i) const { return (w[(i >> 6) & 3u] >> (i & 63u)) & 1ull; }
    bool empty() const { return (w[0] | w[1] | w[2] | w[3]) == 0ull; }
    uint32_t count() const { uint32_t n = 0; for (int k = 0; k < 4; ++k) { uint64_t v = w[k]; while (v) { v &= v - 1; ++n; } } return n; }
    bool operator==(const Bits256& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3]; }
    bool operator!=(const Bits256& o) const { return !(*this == o); }
};
inline bool intersects(const Bits256& a, const Bits256& b) { return ((a.w[0] & b.w[0]) | (a.w[1] & b.w[1]) | (a.w[2] & b.w[2]) | (a.w[3] & b.w[3])) != 0ull; }
inline Bits256 used_bits(const uint8_t* idx, uint32_t n) { Bits256 b; for (uint32_t i = 0; i < n; ++i) b.set(idx[i]); return b; }
// the palette indices whose colour (RGB, the alpha byte is ignored) equals the key
inline Bits256 equal_to_key(const uint32_t pal[256], uint32_t keyValue) { Bits256 b; const uint32_t k = keyValue & 0x00FFFFFFu; for (uint32_t i = 0; i < 256; ++i) if ((pal[i] & 0x00FFFFFFu) == k) b.set(i); return b; }
inline void expand(const Bits256& b, uint8_t mask[256]) { for (uint32_t i = 0; i < 256; ++i) mask[i] = b.test(i) ? 1u : 0u; }
// a texture is worth keeping its index image for when its indices include one that shares the colour of index 0 (the key of every helper) in the palette it was downloaded with
inline bool keep_index_image(const Bits256& used, const uint32_t pal[256]) { return intersects(used, equal_to_key(pal, pal[0])); }

enum Why : uint32_t { kWhyNone = 0, kWhyNoMask, kWhyNoKeyedTexel, kWhyNoIndexImage, kWhyBadFormat, kWhyPaletteMismatch, kWhyImageMismatch, kWhyIdxNotKept, kWhyGlError, kWhyCount };
inline const char* why_name(uint32_t w) {
    static const char* const n[kWhyCount] = {"none", "no_palette_entry_equals_the_key", "texture_has_no_keyed_texel", "no_index_image", "not_256x256_RGB332", "palette_not_the_translation_table", "image_not_the_index_translation", "index_image_not_kept", "gl_error"};
    return w < kWhyCount ? n[w] : "?";
}
// the verdict of the download (g153's, mapped to this enum)
inline uint32_t from_g153(uint32_t w) {
    switch (w) { case ign::g153::kWhyNone: return kWhyNone; case ign::g153::kWhyBadFormat: return kWhyBadFormat; case ign::g153::kWhyPaletteMismatch: return kWhyPaletteMismatch; case ign::g153::kWhyImageMismatch: return kWhyImageMismatch; default: return kWhyNoIndexImage; }
}

// ---- the palette in effect ----------------------------------------------------------------------------------------------------------------------------------------------------------------
class PaletteTracker {
public:
    // true when the CONTENT changed (a download of the same table is counted but changes nothing)
    bool update(const uint32_t p[256]) {
        ++updates_;
        if (have_ && std::memcmp(pal_, p, sizeof pal_) == 0) { ++same_; return false; }
        std::memcpy(pal_, p, sizeof pal_); have_ = true; ++gen_; ++changed_; return true;
    }
    bool have() const { return have_; }
    uint32_t gen() const { return gen_; }
    const uint32_t* pal() const { return pal_; }
    uint64_t updates() const { return updates_; }
    uint64_t changed() const { return changed_; }
    uint64_t same() const { return same_; }
private:
    uint32_t pal_[256] = {}; bool have_ = false; uint32_t gen_ = 0; uint64_t updates_ = 0, changed_ = 0, same_ = 0;
};
// the keyed indices of the palette in effect for a key; `serial` changes ONLY when the mask content changes (a moving key value that keys the same indices is not a change)
class MaskCache {
public:
    const Bits256& get(const PaletteTracker& pt, uint32_t keyRgb) {
        if (valid_ && palGen_ == pt.gen() && key_ == keyRgb) { ++hits_; return mask_; }
        ++computes_;
        const Bits256 nm = pt.have() ? equal_to_key(pt.pal(), keyRgb) : Bits256();
        if (!valid_ || nm != mask_) { mask_ = nm; ++serial_; ++changes_; }
        valid_ = true; palGen_ = pt.gen(); key_ = keyRgb;
        return mask_;
    }
    uint32_t serial() const { return serial_; }
    uint64_t hits() const { return hits_; }
    uint64_t computes() const { return computes_; }
    uint64_t changes() const { return changes_; }
private:
    Bits256 mask_; bool valid_ = false; uint32_t palGen_ = 0, key_ = 0, serial_ = 0; uint64_t hits_ = 0, computes_ = 0, changes_ = 0;
};

// ---- the per-texture state (kept in the texture allocation) ----------------------------------------------------------------------------------------------------------------------------
struct TexKey {
    bool usedValid = false, idxKept = false; Bits256 used; uint32_t why = kWhyNoIndexImage;   // the download: indices used, whether the index image is kept, the reason it was refused
    uint32_t contentGen = 0;                                                                   // bumps only when the index image or the translation REALLY changes
    bool decValid = false, decNeedsVariant = false; uint32_t decSerial = 0, decGen = 0, decWhy = kWhyNone;   // the decision for (mask serial, content gen)
    uint32_t varGl = 0, varSerial = 0, varGen = 0xFFFFFFFFu; uint64_t uses = 0;                // the variant: the mask serial / content it was built for
};
enum Action : uint8_t { kPlain = 0, kUse, kBuild };
// the whole decision for a keyed draw, from the texture's own state and the current mask: O(1), no scan
inline Action resolve(TexKey& t, uint32_t maskSerial, const Bits256& mask, uint32_t& why, bool& cached) {
    why = kWhyNone; cached = true;
    if (!t.usedValid) { why = t.why; return kPlain; }
    if (!(t.decValid && t.decSerial == maskSerial && t.decGen == t.contentGen)) {
        cached = false; t.decValid = true; t.decSerial = maskSerial; t.decGen = t.contentGen;
        if (!intersects(t.used, mask)) { t.decNeedsVariant = false; t.decWhy = kWhyNoKeyedTexel; }
        else if (!t.idxKept) { t.decNeedsVariant = false; t.decWhy = kWhyIdxNotKept; }
        else { t.decNeedsVariant = true; t.decWhy = kWhyNone; }
    }
    if (!t.decNeedsVariant) { why = t.decWhy; return kPlain; }
    if (t.varGl != 0u && t.varSerial == maskSerial && t.varGen == t.contentGen) return kUse;
    return kBuild;
}
inline void note_built(TexKey& t, uint32_t glId, uint32_t maskSerial) { t.varGl = glId; t.varSerial = maskSerial; t.varGen = t.contentGen; }
inline void note_failed(TexKey& t) { t.decNeedsVariant = false; t.decWhy = kWhyGlError; }   // no retry on every triangle: the next mask / content change decides again
// what made a build necessary (counters)
enum Cause : uint8_t { kCauseNew = 0, kCauseContent, kCauseMask, kCauseCount };
inline Cause build_cause(const TexKey& t) { return t.varGl == 0u ? kCauseNew : (t.varGen != t.contentGen ? kCauseContent : kCauseMask); }
// a download of a texture that already has a stored image: the same content must not invalidate anything
inline bool same_content(const uint8_t* oldIdx, uint32_t oldN, const uint8_t oldXlat[256], const uint8_t* newIdx, uint32_t newN, const uint8_t newXlat[256]) {
    return oldIdx != nullptr && oldN == newN && std::memcmp(oldXlat, newXlat, 256) == 0 && std::memcmp(oldIdx, newIdx, newN) == 0;
}

// ---- counters, memory and the summary line -----------------------------------------------------------------------------------------------------------------------------------------------
struct Stats {
    uint64_t bindCalls = 0, texCached = 0, texComputed = 0, keyedTris = 0, translucentTris = 0, plainTris = 0, plain[kWhyCount] = {};
    uint64_t builds = 0, buildCause[kCauseCount] = {}, buildFails = 0, convertUs = 0, uploadUs = 0, buildBytes = 0, texelsKeyed = 0;
    uint64_t downloads = 0, idxKept = 0, idxSkipped = 0, sameContent = 0, newContent = 0, refused = 0;
    uint64_t freedVariants = 0, freedNeverUsed = 0, freedUsedOnce = 0;
    uint64_t bindSamples = 0, bindSampleUs = 0;   // one keyed triangle in 64 is timed (two clock reads)
};
struct SessionIn { uint64_t draws = 0, tris = 0, keyedDraws = 0, keyedTris = 0, alphaOn = 0, alphaOff = 0, envMod = 0, envRepl = 0, keyedFlushes = 0; };
struct MemIn { uint64_t textures = 0, idxTextures = 0, idxBytes = 0, variantTextures = 0, variantBytes = 0, variantsUnused = 0; };
inline size_t why_list(char* out, size_t cap, const uint64_t* w) {
    size_t p = 0; out[0] = 0; bool any = false;
    for (uint32_t i = 1; i < kWhyCount; ++i) if (w[i] && p + 80 < cap) { p += (size_t)snprintf(out + p, cap - p, "%s%s=%llu", any ? " " : "", why_name(i), (unsigned long long)w[i]); any = true; }
    if (!any) snprintf(out, cap, "none");
    return strlen(out);
}
inline int format_summary(char* out, size_t n, const Stats& s, const SessionIn& d, const MemIn& m, uint64_t paletteUpdates, uint64_t paletteChanged, uint64_t maskHits, uint64_t maskComputes, uint64_t maskChanges, bool final) {
    char why[360]; why_list(why, sizeof why, s.plain);
    const double bindUs = s.bindSamples ? (double)s.bindSampleUs / (double)s.bindSamples : 0.0;
    return snprintf(out, n,
        "%s keyed{tris=%llu translucent=%llu cutout=%llu} left_as_before{%llu: %s} | bind{calls=%llu texture_decision_cached=%llu computed=%llu mean_us(1/64 sampled, %llu samples)=%.3f} palette{updates=%llu changed=%llu} mask{cached=%llu computed=%llu changed=%llu} | "
        "variants{builds=%llu created=%llu content=%llu mask=%llu failed=%llu convert_ms=%.2f upload_ms=%.2f bytes=%llu texels_keyed=%llu freed=%llu never_used=%llu used_once=%llu} | "
        "downloads{total=%llu index_kept=%llu index_skipped_no_key_candidate=%llu same_content=%llu changed_content=%llu refused=%llu} | "
        "session{draws=%llu keyed_draws=%llu avg_batch=%.2f keyed_avg_batch=%.2f alpha_test_on=%llu off=%llu env_modulate=%llu env_replace=%llu flushes_by_keyed_state=%llu} | "
        "memory{textures=%llu index_images=%llu (%llu B) variants=%llu (%llu B) variants_unused=%llu}",
        final ? "FINAL" : "RACE", (unsigned long long)s.keyedTris, (unsigned long long)s.translucentTris, (unsigned long long)(s.keyedTris - s.translucentTris), (unsigned long long)s.plainTris, why,
        (unsigned long long)s.bindCalls, (unsigned long long)s.texCached, (unsigned long long)s.texComputed, (unsigned long long)s.bindSamples, bindUs,
        (unsigned long long)paletteUpdates, (unsigned long long)paletteChanged, (unsigned long long)maskHits, (unsigned long long)maskComputes, (unsigned long long)maskChanges,
        (unsigned long long)s.builds, (unsigned long long)s.buildCause[kCauseNew], (unsigned long long)s.buildCause[kCauseContent], (unsigned long long)s.buildCause[kCauseMask], (unsigned long long)s.buildFails,
        (double)s.convertUs / 1000.0, (double)s.uploadUs / 1000.0, (unsigned long long)s.buildBytes, (unsigned long long)s.texelsKeyed, (unsigned long long)s.freedVariants, (unsigned long long)s.freedNeverUsed, (unsigned long long)s.freedUsedOnce,
        (unsigned long long)s.downloads, (unsigned long long)s.idxKept, (unsigned long long)s.idxSkipped, (unsigned long long)s.sameContent, (unsigned long long)s.newContent, (unsigned long long)s.refused,
        (unsigned long long)d.draws, (unsigned long long)d.keyedDraws, d.draws ? (double)d.tris / (double)d.draws : 0.0, d.keyedDraws ? (double)d.keyedTris / (double)d.keyedDraws : 0.0,
        (unsigned long long)d.alphaOn, (unsigned long long)d.alphaOff, (unsigned long long)d.envMod, (unsigned long long)d.envRepl, (unsigned long long)d.keyedFlushes,
        (unsigned long long)m.textures, (unsigned long long)m.idxTextures, (unsigned long long)m.idxBytes, (unsigned long long)m.variantTextures, (unsigned long long)m.variantBytes, (unsigned long long)m.variantsUnused);
}

// ---- the whole draw-time resolution, for the console and for the host test (the console passes the real build as `build`) ------------------------------------------------------------------------
// build(TexKey&, const Bits256& mask, uint32_t serial) -> bool uploads / refreshes the keyed variant and calls note_built / note_failed itself. Returns true when the draw must use t->varGl.
template <class Build>
inline bool bind_core(const PaletteTracker& pt, MaskCache& mc, Stats& st, uint32_t keyRgb, TexKey* t, Build&& build) {
    ++st.bindCalls;
    const Bits256& mask = mc.get(pt, keyRgb);
    uint32_t why = kWhyNoMask; bool ok = false;
    if (!mask.empty()) {   // the global negative: no palette entry equals the key -> nothing is keyed, no texture is looked at
        if (!t) why = kWhyNoIndexImage;
        else {
            bool cached = true; Action act = resolve(*t, mc.serial(), mask, why, cached);
            if (cached) ++st.texCached; else ++st.texComputed;
            if (act == kBuild) { if (build(*t, mask, mc.serial())) act = kUse; else { act = kPlain; why = kWhyGlError; } }
            if (act == kUse) { ++t->uses; ok = true; }
        }
    }
    if (!ok) { ++st.plainTris; ++st.plain[why < kWhyCount ? why : 0u]; return false; }
    return true;
}
} }   // namespace ign::g154
