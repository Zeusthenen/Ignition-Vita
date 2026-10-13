// win32/g148_packet.h -- G148: DEFERRED STATE PACKET (pure: no GL, no Vita API, no logging; tests: tools/g148_test.cpp)
//
// What the real G147 run showed: the deferred GL state of G147 (blend + scissor) was correct but small. The Glide state calls of the heavy zones are mostly LOGICAL (chromakey mode / value, colour combine, constant colour,
// alpha source, depth, cull, texture source): they never wrote GL, they only wrote the guest shadow (g_glideContext) and, between two visible triangles, 60 % of them were overwritten before anything used them
// (G146 logs: 1,136 of 1,874 and 1,352 of 2,112 state calls). Their cost is the cost of the CALL itself (the trap into the translator, the per-call bookkeeping), not GL.
//
// G148 therefore has three parts, all in this header or in its integration:
//  1. THE PACKET: one compact record per state family (12 families), a 32-bit dirty mask, and per family integer counters. A state call writes the guest shadow (as always), compares ONE 64-bit value with the previous one,
//     sets or clears ONE dirty bit and bumps a few integers: no GL, no batch close, no hashing, no allocation, no traversal. A visible triangle consumes the dirty bits (what really reached a draw). From the bits the counters
//     tell, per family, what the guest asked (received), what really changed (changed), what was overwritten before a draw (superseded), what came back to the value of the last draw (cancelled), what a draw used (materialised).
//  2. THE GL-EFFECTIVE FAMILIES (blend, clip): the G147 idea, reduced: desired vs applied GL state, the NET plan issued before the next visible triangle (or before a wrapped GL call that can observe it), but applied INSIDE the open
//     draw session (the pending triangles are drawn first, in order, at the old state; the session is NOT closed and re-opened: ~25 GL calls per change that G146 and G147 still paid).
//  3. THE BATCH KEY: the G146 key of the open draw session also held logical fields (colour combine, alpha source, chromakey, blend record, constant flags) that never reach the GL state of the draw: the per-vertex colours are
//     computed per triangle on the CPU. Two triangles that differ only in those fields are drawn by one glDrawArrays in the same order with the same GL state: identical pixels, fewer draw calls. The dirty bits say, per
//     triangle, whether G146's wide key would have differed (and therefore flushed): that is the "flushes avoided" counter.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g148 {

// ---- families ------------------------------------------------------------------------------------------------------------------------------------------------
enum Fam : uint32_t { kFamBlend = 0, kFamClip, kFamChromaMode, kFamChromaValue, kFamColorCombine, kFamAlphaSource, kFamConstColor, kFamDepthMode, kFamDepthFunc, kFamDepthMask, kFamCull, kFamTexSource, kFamCount };
inline const char* fam_name(uint32_t f) {
    static const char* const n[kFamCount] = {"blend", "clip", "chroma_mode", "chroma_value", "color_combine", "alpha_source", "const_color", "depth_mode", "depth_func", "depth_mask", "cull", "tex_source"};
    return f < kFamCount ? n[f] : "?";
}
constexpr uint32_t fbit(uint32_t f) { return 1u << f; }
// the families whose values were part of the G146 batch key (blend record, colour combine, alpha source, chromakey mode + value; the constant-colour flags derive from colour combine / alpha source). They never reach GL state.
constexpr uint32_t kWideKeyMask = fbit(kFamBlend) | fbit(kFamChromaMode) | fbit(kFamChromaValue) | fbit(kFamColorCombine) | fbit(kFamAlphaSource);

// ---- groups (what can be switched off one by one: the control file tokens g148-<name>=off, g148=off = all) ---------------------------------------------------------
enum Group : uint32_t { kGrBlend = 1, kGrClip = 2, kGrKey = 4, kGrChroma = 8, kGrCombine = 16, kGrDepthCull = 32, kGrTex = 64, kGrAll = 127 };
inline const char* group_name(uint32_t bitIndex) {
    static const char* const n[7] = {"blend", "clip", "key", "chroma", "combine", "depthcull", "tex"};
    return bitIndex < 7 ? n[bitIndex] : "?";
}
inline bool token_present(const char* text, const char* tok) {
    if (!text) return false;
    const size_t tl = strlen(tok);
    for (const char* p = text; (p = strstr(p, tok)) != nullptr; ++p) {
        const bool startOk = p == text || p[-1] == ' ' || p[-1] == ',' || p[-1] == ';';
        const char e = p[tl];
        if (startOk && (e == 0 || e == ' ' || e == ',' || e == ';')) return true;
    }
    return false;
}
inline uint32_t parse_groups(const char* text) {
    uint32_t g = kGrAll;
    if (token_present(text, "g148=off")) return 0;
    for (uint32_t i = 0; i < 7; ++i) { char tok[40]; snprintf(tok, sizeof tok, "g148-%s=off", group_name(i)); if (token_present(text, tok)) g &= ~(1u << i); }
    return g;
}

// ---- counters (all uint64_t: a window is a plain difference of two snapshots) --------------------------------------------------------------------------------------
struct FamCounters { uint64_t received = 0, changed = 0, redundant = 0, superseded = 0, cancelled = 0, materialized = 0, dropped = 0; };   // dropped = pending changes forgotten by a context reset
struct Stats {
    FamCounters fam[kFamCount];
    // GL-effective families (blend, clip)
    uint64_t glEquiv = 0, glIssued = 0;            // GL calls G146 would have issued for the deferred calls (1 / 2 / 4) vs the calls the net plans really issued
    uint64_t deferred = 0, immediate = 0;          // calls of the two writers that only edited `desired` / took the immediate G146 path
    uint64_t syncs = 0, syncVisible = 0, syncForeign = 0, syncSwap = 0, syncEmpty = 0;
    uint64_t callsWithOpenSession = 0;             // a deferred call arrived while a draw session was open: G146 closed it (flush + restore + reopen ~25 GL calls) at each of these
    uint64_t appliesWithFlush = 0;                 // an apply had to draw pending triangles first (the flush G146 also did); the session stayed open
    uint64_t unknownViewport = 0;
    // batch key
    uint64_t triangles = 0, trianglesWithWideDiff = 0, flushesAvoidedByKey = 0;   // visible triangles; of them: the G146 wide key would have differed from the previous one; of those: the open session really stayed one draw call
    // cost of the calls themselves
    uint64_t stateCalls = 0, stateSamples = 0, stateSampleUs = 0, triCalls = 0, triSamples = 0, triSampleUs = 0, clockReadsRemoved = 0;
};
inline Stats delta(const Stats& now, const Stats& before) {
    static_assert(sizeof(Stats) % sizeof(uint64_t) == 0, "Stats must be a plain array of uint64_t");
    Stats d; const uint64_t* a = (const uint64_t*)&now; const uint64_t* b = (const uint64_t*)&before; uint64_t* o = (uint64_t*)&d;
    for (size_t i = 0; i < sizeof(Stats) / sizeof(uint64_t); ++i) o[i] = a[i] >= b[i] ? a[i] - b[i] : 0;
    return d;
}

// ---- GL state owned by the two GL-writing families ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kGlBlend = 0x0BE2, kGlScissorTest = 0x0C11;
struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};
struct GlState {
    bool blendKnown = false, blendOn = false;
    bool fKnown = false; uint32_t f[4] = {0, 0, 0, 0};
    bool scKnown = false, scOn = false;
    bool rectKnown = false; Rect rect;
};
inline bool same_f(const uint32_t* a, const uint32_t* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }
enum Toggle : uint8_t { kNone = 0, kEnable = 1, kDisable = 2 };
struct Plan {
    uint8_t blend = kNone; bool blendFunc = false; uint8_t scissor = kNone; bool rect = false;
    bool empty() const { return blend == kNone && !blendFunc && scissor == kNone && !rect; }
    bool blend_part() const { return blend != kNone || blendFunc; }
    bool scissor_part() const { return scissor != kNone || rect; }
    int calls() const { return (blend != kNone ? 1 : 0) + (blendFunc ? 1 : 0) + (scissor != kNone ? 1 : 0) + (rect ? 1 : 0); }
};
// the GL calls that bring `a` (what the driver holds) to `d` (what G146 would hold), in G146's order: enable / disable, then the factors or the rectangle. An unknown field is never assumed to hold anything.
inline Plan plan(const GlState& a, const GlState& d) {
    Plan p;
    if (d.blendKnown) {
        if (d.blendOn) { if (!(a.blendKnown && a.blendOn)) p.blend = kEnable; }
        else if (!(a.blendKnown && !a.blendOn)) p.blend = kDisable;
    }
    // the factors G146 would hold (the last non-identity ones the guest set) are issued whenever the driver holds others, also while blending is off: a later enable by anyone else (a foreign glEnable(GL_BLEND) that sets no
    // factors) then shows exactly what G146 would have shown. Rare (the guest alternates among a few factor sets) and one call.
    if (d.fKnown && !(a.fKnown && same_f(a.f, d.f))) p.blendFunc = true;
    if (d.scKnown) {
        if (d.scOn) { if (!(a.scKnown && a.scOn)) p.scissor = kEnable; }
        else if (!(a.scKnown && !a.scOn)) p.scissor = kDisable;
    }
    if (d.rectKnown && !(a.rectKnown && a.rect == d.rect)) p.rect = true;
    return p;
}
inline void commit(GlState& a, const GlState& d, const Plan& p) {
    if (p.blend != kNone) { a.blendKnown = true; a.blendOn = d.blendOn; }
    if (p.blendFunc) { a.fKnown = true; memcpy(a.f, d.f, sizeof a.f); }
    if (p.scissor != kNone) { a.scKnown = true; a.scOn = d.scOn; }
    if (p.rect) { a.rectKnown = true; a.rect = d.rect; }
}
inline bool blend_identity(uint32_t rgbSf, uint32_t rgbDf, uint32_t aSf, uint32_t aDf) { return rgbSf == 0x4 && rgbDf == 0x0 && aSf == 0x4 && aDf == 0x0; }
// what two GL states look like to anything that can observe GL: bit mask of the differing fields (0 = same)
enum Field : uint32_t { kFBlendOn = 1, kFFactors = 2, kFScissorOn = 4, kFRect = 8 };
inline uint32_t diff_effective(const GlState& ref, const GlState& got) {
    uint32_t m = 0;
    if (ref.blendKnown) {
        if (!got.blendKnown || got.blendOn != ref.blendOn) m |= kFBlendOn;
        if (ref.blendOn && ref.fKnown && (!got.fKnown || !same_f(ref.f, got.f))) m |= kFFactors;
    }
    if (ref.scKnown) {
        if (!got.scKnown || got.scOn != ref.scOn) m |= kFScissorOn;
        if (ref.scOn && ref.rectKnown && (!got.rectKnown || got.rect != ref.rect)) m |= kFRect;
    }
    return m;
}
inline const char* field_names(uint32_t m, char* out, size_t n) {
    out[0] = 0; size_t u = 0;
    const struct { uint32_t b; const char* s; } t[4] = {{kFBlendOn, "blend_enable"}, {kFFactors, "blend_factors"}, {kFScissorOn, "scissor_enable"}, {kFRect, "scissor_rect"}};
    for (const auto& e : t) if ((m & e.b) && u + 20 < n) u += (size_t)snprintf(out + u, n - u, "%s%s", u ? "," : "", e.s);
    if (!u) snprintf(out, n, "none");
    return out;
}
enum SyncWhy : uint8_t { kSyncVisible = 0, kSyncForeign, kSyncSwap };
// what an observing GL call needs applied: the blend part (enable + factors) and / or the scissor part (enable + rectangle). A call that can see neither (a matrix, a viewport, an array, a depth function, a read-back) needs none.
constexpr uint32_t kNeedBlend = 1, kNeedScissor = 2, kNeedAll = 3;

// ---- values of the families (one 64-bit word each) ---------------------------------------------------------------------------------------------------------------
inline uint64_t blend_value(bool valid, uint32_t rgbS, uint32_t rgbD, uint32_t aS, uint32_t aD) { return (valid ? 1ull << 20 : 0ull) | ((uint64_t)(rgbS & 15u) << 12) | ((uint64_t)(rgbD & 15u) << 8) | ((uint64_t)(aS & 15u) << 4) | (uint64_t)(aD & 15u); }
inline uint64_t clip_value(uint32_t minx, uint32_t miny, uint32_t maxx, uint32_t maxy) { return ((uint64_t)(minx & 0xFFFFu) << 48) | ((uint64_t)(miny & 0xFFFFu) << 32) | ((uint64_t)(maxx & 0xFFFFu) << 16) | (uint64_t)(maxy & 0xFFFFu); }
constexpr uint64_t kNoValue = ~0ull;   // "nothing seen yet"; no family value can take it

// ---- the packet --------------------------------------------------------------------------------------------------------------------------------------------------
class Packet {
public:
    Stats st;
    GlState desired, applied;
    uint32_t dirty = 0;                                  // families whose value differs from the value the last visible draw saw
    uint64_t cur[kFamCount], consumed[kFamCount];
    Packet() { for (uint32_t i = 0; i < kFamCount; ++i) cur[i] = consumed[i] = kNoValue; }
    // a state call: ONE compare, ONE bit, a few integers. Returns true when the value changed.
    inline bool note(uint32_t f, uint64_t v) {
        FamCounters& c = st.fam[f]; ++c.received;
        if (v == cur[f]) { ++c.redundant; return false; }
        ++c.changed; cur[f] = v;
        const uint32_t b = 1u << f;
        if (dirty & b) { if (v == consumed[f]) { dirty &= ~b; ++c.cancelled; } else ++c.superseded; }
        else dirty |= b;
        return true;
    }
    // a visible triangle uses the state: the pending changes are materialised. Returns the mask that was dirty.
    inline uint32_t consume() {
        const uint32_t d = dirty;
        if (d) { for (uint32_t m = d; m; m &= m - 1) { const uint32_t f = (uint32_t)__builtin_ctz(m); ++st.fam[f].materialized; consumed[f] = cur[f]; } dirty = 0; }
        return d;
    }
    void resync() { for (uint32_t m = dirty; m; m &= m - 1) ++st.fam[(uint32_t)__builtin_ctz(m)].dropped; for (uint32_t i = 0; i < kFamCount; ++i) cur[i] = consumed[i] = kNoValue; dirty = 0; }   // a new context: the values are unknown again (counters are kept; a pending change is counted as dropped)

    // ---- GL-effective families -------------------------------------------------------------------------------------------------------------------------------------
    bool pending() const { return pendMask_ != 0u; }
    uint32_t pending_mask() const { return pendMask_; }   // bit 0: a blend change is pending, bit 1: a scissor change
    void gl_reset() { desired = GlState{}; applied = GlState{}; pendMask_ = 0u; touched_ = false; }
    void swap_boundary() { if (touched_) { touched_ = false; applied = GlState{}; refresh(); } }   // a frame that deferred something: the driver may re-establish its state at a scene boundary, issue what is desired again
    void want_blend(bool identity, const uint32_t f[4], bool sessionOpen) {
        ++st.deferred; touched_ = true; st.glEquiv += identity ? 1u : 2u; if (sessionOpen) ++st.callsWithOpenSession;
        desired.blendKnown = true; desired.blendOn = !identity;
        if (!identity) { desired.fKnown = true; memcpy(desired.f, f, sizeof desired.f); }
        refresh();
    }
    void want_scissor(const Rect& r, bool sessionOpen) {
        ++st.deferred; touched_ = true; st.glEquiv += 4u; if (sessionOpen) ++st.callsWithOpenSession;
        desired.scKnown = true; desired.scOn = true; desired.rectKnown = true; desired.rect = r;
        refresh();
    }
    // someone else wrote GL (after the sync that preceded it): both sides take the value, as if G146 had written it
    void foreign_toggle(uint32_t cap, bool on) {
        if (cap == kGlBlend) { desired.blendKnown = true; desired.blendOn = on; applied.blendKnown = true; applied.blendOn = on; }
        else if (cap == kGlScissorTest) { desired.scKnown = true; desired.scOn = on; applied.scKnown = true; applied.scOn = on; }
        refresh();
    }
    void foreign_factors(const uint32_t f[4]) { desired.fKnown = true; memcpy(desired.f, f, sizeof desired.f); applied.fKnown = true; memcpy(applied.f, f, sizeof applied.f); refresh(); }
    void foreign_rect(const Rect& r) { desired.rectKnown = true; desired.rect = r; applied.rectKnown = true; applied.rect = r; refresh(); }
    // the net plan to execute at a sync point (the caller issues the calls and then calls finish_apply)
    Plan take_plan(SyncWhy why, bool pendingTriangles, uint32_t need = kNeedAll) {
        Plan p = plan(applied, desired);
        if (!(need & kNeedBlend)) { p.blend = kNone; p.blendFunc = false; }
        if (!(need & kNeedScissor)) { p.scissor = kNone; p.rect = false; }
        if (p.empty()) { ++st.syncEmpty; return p; }
        ++st.syncs; st.glIssued += (uint64_t)p.calls(); if (pendingTriangles) ++st.appliesWithFlush;
        if (why == kSyncVisible) ++st.syncVisible; else if (why == kSyncSwap) ++st.syncSwap; else ++st.syncForeign;
        return p;
    }
    void finish_apply(const Plan& p) { commit(applied, desired, p); refresh(); }
    void force_desired(const GlState& d) { desired = d; applied = GlState{}; refresh(); }
    // a deferred call that left nothing pending and changed nothing is counted by the caller through note(); these two classify a GL-writer call by its effect on the plan
    uint64_t closes_avoided() const { return st.callsWithOpenSession; }   // each of these calls closed the session in G146; no call of G148 closes it
private:
    void refresh() { const Plan p = plan(applied, desired); pendMask_ = (p.blend_part() ? kNeedBlend : 0u) | (p.scissor_part() ? kNeedScissor : 0u); }
    uint32_t pendMask_ = 0u; bool touched_ = false;
};

// ---- when the deferral of the GL families may run -----------------------------------------------------------------------------------------------------------------
struct Gate {
    bool controlOn = true, autoOn = true, raceState = false, rejectionActive = false, carousel = false;
    bool open() const { return controlOn && autoOn && raceState && rejectionActive && !carousel; }
    const char* why_not() const { return !controlOn ? "control_file" : !autoOn ? "auto_disabled" : !raceState ? "not_race_state" : !rejectionActive ? "rejection_off" : carousel ? "carousel" : "open"; }
};

// ---- window statistics: bands of the window FPS, outliers ---------------------------------------------------------------------------------------------------------
constexpr uint64_t kOutlierUs = 1000000;   // a frame of one second or more is a stall (a load, a transition, a pause release, a hitch): reported on its own, never part of the race statistics
struct Bands {
    uint32_t windows = 0, below15 = 0, below20 = 0, below25 = 0, below30 = 0;
    void add(double fps) { ++windows; if (fps < 15.0) ++below15; if (fps < 20.0) ++below20; if (fps < 25.0) ++below25; if (fps < 30.0) ++below30; }
    void clear() { *this = Bands{}; }
};

// ---- lines -------------------------------------------------------------------------------------------------------------------------------------------------------
inline int format_families(char* out, size_t n, const char* label, uint32_t groups, uint32_t frames, const Stats& w) {
    const double f = frames ? (double)frames : 1.0;
    int len = snprintf(out, n, "%s frames=%u groups=0x%02x | per_frame{family: received/changed/superseded/cancelled/materialized (redundant)}:", label, frames, groups);
    for (uint32_t i = 0; i < kFamCount && len > 0 && (size_t)len + 110 < n; ++i) {
        const FamCounters& c = w.fam[i];
        len += snprintf(out + len, n - (size_t)len, " %s=%.1f/%.1f/%.1f/%.1f/%.1f(%.1f)", fam_name(i), (double)c.received / f, (double)c.changed / f, (double)c.superseded / f, (double)c.cancelled / f, (double)c.materialized / f, (double)c.redundant / f);
    }
    return len;
}
inline int format_gl_batch(char* out, size_t n, const char* label, const char* mode, uint32_t frames, const Stats& w) {
    const double f = frames ? (double)frames : 1.0, eq = (double)w.glEquiv;
    const uint64_t avoided = w.glEquiv > w.glIssued ? w.glEquiv - w.glIssued : 0;
    return snprintf(out, n,
        "%s mode=%s frames=%u | gl_writers_per_frame{calls=%.1f deferred=%.1f immediate=%.1f} | gl_calls_per_frame{g146_equivalent=%.1f issued=%.1f avoided=%.1f (%.1f%%)} | sync_per_frame{issued=%.1f (visible=%.1f foreign=%.1f swap=%.1f) found_nothing=%.1f} | "
        "draw_sessions_per_frame{closes_avoided=%.1f (calls that arrived with the session open; G148 closes none) applies_that_flushed=%.1f} | batch_key{visible_triangles=%.1f wide_key_would_differ=%.1f flushes_avoided=%.1f (%.1f%% of those)} | unknown_viewport=%llu",
        label, mode, frames, (double)(w.deferred + w.immediate) / f, (double)w.deferred / f, (double)w.immediate / f, eq / f, (double)w.glIssued / f, (double)avoided / f, eq > 0.0 ? 100.0 * (double)avoided / eq : 0.0,
        (double)w.syncs / f, (double)w.syncVisible / f, (double)w.syncForeign / f, (double)w.syncSwap / f, (double)w.syncEmpty / f, (double)w.callsWithOpenSession / f, (double)w.appliesWithFlush / f,
        (double)w.triangles / f, (double)w.trianglesWithWideDiff / f, (double)w.flushesAvoidedByKey / f, w.trianglesWithWideDiff ? 100.0 * (double)w.flushesAvoidedByKey / (double)w.trianglesWithWideDiff : 0.0, (unsigned long long)w.unknownViewport);
}
// where the frame goes: scene = guest code + the translator's state calls + the triangle calls (which contain the GL submit); every part is an estimate from 1-in-N samples
struct TimingIn {
    uint32_t frames = 0;
    double frameMs = 0, sceneMs = 0, swapMs = 0, compMs = 0, uploadMs = 0, overlayMs = 0, presentMs = 0;
    double stateCallsPerFrame = 0, stateMeanUs = 0, triCallsPerFrame = 0, triMeanUs = 0, drawsPerFrame = 0, drawMeanUs = 0, clockRemovedPerFrame = 0, clockCostUs = -1.0;
    uint64_t stateSamples = 0, triSamples = 0, drawSamples = 0;
};
inline int format_timing(char* out, size_t n, const char* label, const TimingIn& t) {
    const double st = t.stateSamples ? t.stateCallsPerFrame * t.stateMeanUs / 1000.0 : 0.0, tr = t.triSamples ? t.triCallsPerFrame * t.triMeanUs / 1000.0 : 0.0, dr = t.drawSamples ? t.drawsPerFrame * t.drawMeanUs / 1000.0 : 0.0;
    const double rest = t.sceneMs - st - tr; const double removed = t.clockCostUs >= 0.0 ? t.clockRemovedPerFrame * t.clockCostUs / 1000.0 : -1.0;
    return snprintf(out, n,
        "%s frames=%u | ms_per_frame{frame=%.2f scene=%.2f swap_wait=%.2f compositor=%.2f upload=%.2f overlay=%.2f present=%.2f} | scene_split_est{state_calls=%.2f (%.0f calls x %.2f us, %llu samples) triangle_calls=%.2f (%.0f x %.2f us, %llu samples; of which GL draw submit %.2f from %llu sampled glDrawArrays, %.0f draws x %.0f us) guest_code_and_the_rest=%.2f} | "
        "per_call_clock_reads_removed{per_frame=%.1f cost_each_us(calibrated)=%.2f saved_est_ms=%.2f}",
        label, t.frames, t.frameMs, t.sceneMs, t.swapMs, t.compMs, t.uploadMs, t.overlayMs, t.presentMs, st, t.stateCallsPerFrame, t.stateMeanUs, (unsigned long long)t.stateSamples, tr, t.triCallsPerFrame, t.triMeanUs, (unsigned long long)t.triSamples,
        dr, (unsigned long long)t.drawSamples, t.drawsPerFrame, t.drawMeanUs, rest > 0.0 ? rest : 0.0, t.clockRemovedPerFrame, t.clockCostUs, removed);
}

}}   // namespace ign::g148
