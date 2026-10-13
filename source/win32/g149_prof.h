// win32/g149_prof.h -- G149: COST PROFILER of the guest->host boundary (pure: no GL, no Vita API, no logging; tests: tools/g149_test.cpp)
//
// What the real G148 run said (five tracks, 52 windows of 120 frames, 500 MHz): the frame is 48-59 ms and its biggest block, "guest_code_and_the_rest" (24-35 ms, 50 % of the frame), was a single number. The Glide side that
// G148 could reach (state calls 1.1-2.5 ms, triangle calls 9.6-12.4 ms) is a quarter of the frame. A processed triangle call costs 30.7 us and a REJECTED one still 13.7 us (regression over the 52 windows, R2 = 0.997).
// To decide what to attack next without guessing, G149 measures where the time between two host calls goes, with instruments that cost almost nothing:
//
//  1. THE GAP PROFILER. The guest runs between two Glide calls; the interval between the END of one host call and the START of the next one contains (a) the way out of the host (trap epilogue, dynarec re-entry), (b) the translated
//     guest code, (c) the way in (dynarec exit, trap lookup, GIL, shim dispatch) and (d) any host call that is not a Glide call. One call in kGapEvery starts a PAIR: it stamps the clock and the translated-block counter when it
//     returns; the NEXT Glide call stamps them again at its entry. The difference is one sample of a gap, ATTRIBUTED to the guest return address of the call that ends it (the call site: the guest function that was about to call).
//     A sample is a uniformly random gap (every gap follows exactly one call), so the sums are unbiased estimates of where the time goes. Over the pairs a least-squares line gap = a + b * blocks separates the cost per host
//     crossing (a, microseconds) from the cost per translated block (b): "shim / bridge" versus "dynarec + game code". No clock read happens on the other calls.
//  2. THE SEGMENTS OF THE TRIANGLE CALL. One triangle call in 32 (a different phase from the G148 sampler) stamps the clock at 8 fixed points of guDrawTriangleWithClip; the aggregate says how many microseconds go to the
//     prologue, the level-0 rejection, the vertex decode, the validation, the level-1 rejection, state / uv / colour and the key + submit.
//  3. THE CROSSING CENSUS. WinVita counts every guest->host crossing per trap slot (d2rt::trapcnt::hits, always on, exact, free). A window reads the deltas: how many crossings per frame, and which slots.
//  4. RACE-FRAME ACCOUNTING (the "pause contamination" of G148): the G148 totals were differences of cumulative counters, so the frames of a long pause (7,265 and 43,749 frames in two of the five races) polluted the per-frame
//     figures of the race totals. Here a counter is added to the window / race sums only at the end of a frame that the race statistics count (add_u64s).
//  5. A ONE-ENTRY TEXTURE DIMENSION CACHE for the triangle path (valid while the texture allocation map has not been modified).
//
// Everything is switchable one by one from ux0:data/ignition/g146_geo.txt: g149=off (all) or g149-<gap|seg|census|tex|scan>=off. With g149=off the Glide shims are registered exactly as in G148.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include "win32/g148_packet.h"

namespace ign { namespace g149 {

// ---- groups --------------------------------------------------------------------------------------------------------------------------------------------------
enum Group : uint32_t { kGrGap = 1, kGrSeg = 2, kGrCensus = 4, kGrTex = 8, kGrScan = 16, kGrAll = 31 };
constexpr uint32_t kGroupCount = 5;
inline const char* group_name(uint32_t i) {
    static const char* const n[kGroupCount] = {"gap", "seg", "census", "tex", "scan"};
    return i < kGroupCount ? n[i] : "?";
}
inline uint32_t parse_groups(const char* text) {
    uint32_t g = kGrAll;
    if (ign::g148::token_present(text, "g149=off")) return 0;
    for (uint32_t i = 0; i < kGroupCount; ++i) { char tok[40]; snprintf(tok, sizeof tok, "g149-%s=off", group_name(i)); if (ign::g148::token_present(text, tok)) g &= ~(1u << i); }
    return g;
}

// ---- the translation knobs of WinVita's dynarec (OFF unless the control file asks) -------------------------------------------------------------------------------
// libwinx86_vita.a carries translation-time switches that WinVita's own harness arms for Diablo II and that Ignition never armed (all default 0 = the original box86 behaviour, instruction for instruction): the translator reads them
// when it translates a block, so they must be set before the first guest instruction runs. G149 does NOT arm any by default (the dynarec stays exactly as in G148); the control file can arm them for an A/B on the real console, one at a time,
// and the profiler's figure b (microseconds per translated block) and the race windows measure the effect. Tokens:
//   dyn-budgettail=on   moves the 10 dead instructions of every block's preemption prologue to the block tail (same code, other placement: the first real instruction is 40 bytes closer to the block entry)
//   dyn-mmufold=on      folds the guest-memory base into absolute addressings (needs the arena, which Ignition uses)
//   dyn-mmustack=1|2|3  1 = chains of POP r32, 2 = chains of PUSH r32, 3 = both (shorter dependent chains)
//   dyn-callret=on      return-address prediction on the host stack (box86's CALLRET: a wrong prediction falls back to the regular jump)
//   dyn-nopend=1        does not force the archiving of the live flags at the end of a block that ends in a CALL (mode 3 is incorrect by construction and is REFUSED)
struct Dyn {
    bool budgetTail = false, mmuFold = false, callRet = false; int mmuStack = 0, noPend = 0;
    bool any() const { return budgetTail || mmuFold || callRet || mmuStack != 0 || noPend != 0; }
};
inline Dyn parse_dyn(const char* text, char* refused, size_t refusedN) {
    Dyn d; if (refused && refusedN) refused[0] = 0;
    auto refuse = [&](const char* what) { if (refused && refusedN && strlen(refused) + strlen(what) + 2 < refusedN) { if (refused[0]) strcat(refused, " "); strcat(refused, what); } };
    if (ign::g148::token_present(text, "dyn-budgettail=on")) d.budgetTail = true;
    if (ign::g148::token_present(text, "dyn-mmufold=on")) d.mmuFold = true;
    if (ign::g148::token_present(text, "dyn-callret=on")) d.callRet = true;
    for (int v = 1; v <= 3; ++v) { char tok[24]; snprintf(tok, sizeof tok, "dyn-mmustack=%d", v); if (ign::g148::token_present(text, tok)) d.mmuStack = v; }
    if (ign::g148::token_present(text, "dyn-nopend=1")) d.noPend = 1;
    if (ign::g148::token_present(text, "dyn-nopend=3")) refuse("dyn-nopend=3(incorrect-by-construction)");
    if (ign::g148::token_present(text, "dyn-nopend=2")) refuse("dyn-nopend=2(undefined)");
    return d;
}

// ---- generic helpers -----------------------------------------------------------------------------------------------------------------------------------------
// a struct made only of uint64_t counters: a += b (the sum of the frames the race statistics count)
template <class T>
inline void add_u64s(T& a, const T& b) {
    static_assert(sizeof(T) % sizeof(uint64_t) == 0, "a plain array of uint64_t");
    uint64_t* x = reinterpret_cast<uint64_t*>(&a); const uint64_t* y = reinterpret_cast<const uint64_t*>(&b);
    for (size_t i = 0; i < sizeof(T) / sizeof(uint64_t); ++i) x[i] += y[i];
}
template <class T>
inline void sub_u64s(T& out, const T& now, const T& before) {   // out = now - before (never negative)
    static_assert(sizeof(T) % sizeof(uint64_t) == 0, "a plain array of uint64_t");
    uint64_t* o = reinterpret_cast<uint64_t*>(&out); const uint64_t* a = reinterpret_cast<const uint64_t*>(&now); const uint64_t* b = reinterpret_cast<const uint64_t*>(&before);
    for (size_t i = 0; i < sizeof(T) / sizeof(uint64_t); ++i) o[i] = a[i] >= b[i] ? a[i] - b[i] : 0;
}
// bounded append (never writes past `n`, never lets the remaining size underflow)
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
inline void app(char* out, size_t n, int& w, const char* fmt, ...) {
    if (n == 0 || w < 0 || (size_t)w + 1 >= n) return;
    va_list ap; va_start(ap, fmt);
    const int r = vsnprintf(out + w, n - (size_t)w, fmt, ap);
    va_end(ap);
    if (r > 0) w += r;
    if ((size_t)w >= n) w = (int)n - 1;
}

// ---- the API table (names are static strings) -----------------------------------------------------------------------------------------------------------------
constexpr int kMaxApis = 32;
struct ApiTable {
    const char* name[kMaxApis] = {};
    uint32_t n = 0;
    int find_or_add(const char* s) {
        for (uint32_t i = 0; i < n; ++i) if (name[i] && !strcmp(name[i], s)) return (int)i;
        if (n >= (uint32_t)kMaxApis) return -1;
        name[n] = s; return (int)n++;
    }
    const char* at(int i) const { return i >= 0 && (uint32_t)i < n ? name[i] : "?"; }
};

// ---- the gap profiler ----------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kGapEvery = 64;            // one Glide call in 64 starts a pair (3 clock reads + 2 block reads + 1 guest read per pair: ~0.05 us per call on average)
constexpr uint64_t kGapMaxUs = 200000;        // a gap of 200 ms or more is not a frame (a pause, a load, a transition): counted as dropped, never summed
constexpr int kSiteSlots = 512;               // distinct call sites kept (open addressing, 16 probes)
struct Site { uint32_t eip; int32_t api; uint32_t n; uint32_t pad; uint64_t gapUs, blocks, handlerUs; };   // n == 0: empty slot
struct ApiGap { uint64_t n, gapUs, blocks, handlerUs; };                      // pairs whose SECOND call was this API
struct GapProf {
    Site site[kSiteSlots];
    ApiGap api[kMaxApis];
    uint32_t used;
    uint32_t pad_;
    uint64_t pairs, dropped, siteOverflow, siteOverflowUs;
    uint64_t sumGapUs, sumBlocks, sumHandlerUs;
    double sg, sb, sgb, sbb, sgg;   // regression sums: gap (us) against translated blocks
    void clear() { static_assert(std::is_trivially_copyable<GapProf>::value, "memset-able"); memset(this, 0, sizeof *this); }
    // one pair: the gap before a call (gapUs, blocks executed in it) and the duration of that call's own handler
    void record(uint32_t eip, int apiId, uint64_t gapUs, uint64_t blocks, uint64_t handlerUs) {
        if (gapUs >= kGapMaxUs) { ++dropped; return; }
        ++pairs; sumGapUs += gapUs; sumBlocks += blocks; sumHandlerUs += handlerUs;
        const double g = (double)gapUs, b = (double)blocks;
        sg += g; sb += b; sgb += g * b; sbb += b * b; sgg += g * g;
        if (apiId >= 0 && apiId < kMaxApis) { ApiGap& a = api[apiId]; ++a.n; a.gapUs += gapUs; a.blocks += blocks; a.handlerUs += handlerUs; }
        const uint32_t h = (eip * 2654435761u) >> 23;   // 9 bits
        for (uint32_t p = 0; p < 16; ++p) {
            Site& s = site[(h + p) & (uint32_t)(kSiteSlots - 1)];
            if (s.n == 0) { s.eip = eip; s.api = apiId; s.n = 1; s.gapUs = gapUs; s.blocks = blocks; s.handlerUs = handlerUs; ++used; return; }
            if (s.eip == eip) { ++s.n; s.gapUs += gapUs; s.blocks += blocks; s.handlerUs += handlerUs; return; }
        }
        ++siteOverflow; siteOverflowUs += gapUs;
    }
};
// The protocol of the pairs as a pure state machine (the shim wrapper in glide2x_ign.cpp drives it with the real clock; tools/g149_test.cpp drives it with a simulated one).
//  * every Glide call runs need_slow(): ONE decrement and ONE test (the whole cost of a call that is not sampled);
//  * a slow-path call (every kGapEvery-th call, the call that ends an open pair, the first call after a swap) runs enter() at its entry: it ends the open pair (the gap before this call), measures the logic gap of the frame, or
//    starts a new pair; after the handler of a call that starts a pair, started() stamps its exit;
//  * a gap of kLogicMaxUs or more (a frame of one second or more) is a stall, not logic.
constexpr uint64_t kLogicMaxUs = ign::g148::kOutlierUs;
struct PairMachine {
    int32_t countdown = (int32_t)kGapEvery;
    uint32_t armed = 0;                      // bit 0: a pair is open; bit 1: the next call measures the logic gap (a swap just returned)
    uint64_t tExit = 0, tSwapExit = 0; uint32_t bExit = 0; int prevApi = -1;
    uint64_t logicUs = 0, logicN = 0;        // logic gaps since the last take_logic()
    inline bool need_slow() { return --countdown <= 0 || armed != 0u; }
    void swap_exit(uint64_t now, bool gate) { tSwapExit = now; if (gate) armed |= 2u; }
    struct Entry { bool start = false, end = false; uint64_t gapUs = 0; uint32_t blocks = 0; };
    Entry enter(bool gate, uint64_t now, uint32_t blocksNow) {
        Entry e; const bool startPair = countdown <= 0; if (startPair) countdown = (int32_t)kGapEvery;
        if (!gate) { armed = 0u; return e; }
        if (armed & 2u) { const uint64_t gap = now - tSwapExit; if (gap < kLogicMaxUs) { logicUs += gap; ++logicN; } armed &= ~2u; }
        if (armed & 1u) { e.end = true; e.gapUs = now - tExit; e.blocks = blocksNow - bExit; armed &= ~1u; return e; }   // (modular 32-bit block counter)
        e.start = startPair; return e;
    }
    void started(uint64_t now, uint32_t blocksNow, int api) { tExit = now; bExit = blocksNow; prevApi = api; armed |= 1u; }
    void take_logic(uint64_t& us, uint64_t& n) { us = logicUs; n = logicN; logicUs = 0; logicN = 0; }
    void reset() { countdown = (int32_t)kGapEvery; armed = 0u; logicUs = 0; logicN = 0; }
};
struct Fit { bool ok = false; double a = 0, b = 0, r2 = 0, meanGap = 0, meanBlocks = 0, meanHandler = 0; };
// gap = a + b * blocks (microseconds). a = what one crossing costs outside the handler; b = what one translated block costs.
inline Fit fit(const GapProf& g) {
    Fit f; if (g.pairs < 8) return f;
    const double n = (double)g.pairs;
    f.meanGap = g.sg / n; f.meanBlocks = g.sb / n; f.meanHandler = (double)g.sumHandlerUs / n;
    const double vb = g.sbb / n - f.meanBlocks * f.meanBlocks, cov = g.sgb / n - f.meanGap * f.meanBlocks, vg = g.sgg / n - f.meanGap * f.meanGap;
    f.ok = true;
    if (vb <= 1e-9) { f.a = f.meanGap; f.b = 0.0; f.r2 = 0.0; return f; }   // every pair crossed the same number of blocks: no slope
    f.b = cov / vb; f.a = f.meanGap - f.b * f.meanBlocks;
    f.r2 = vg > 1e-9 ? (cov * cov) / (vb * vg) : 0.0;
    return f;
}

// ---- the segments of one triangle call -------------------------------------------------------------------------------------------------------------------------
enum Seg : uint32_t { kSegPrologue = 0, kSegLevel0, kSegDecode, kSegValidate, kSegLevel1, kSegState, kSegSubmit, kSegCount };
constexpr uint32_t kTriMarks = kSegCount + 1;   // 8 stamps delimit 7 segments
inline const char* seg_name(uint32_t i) {
    static const char* const n[kSegCount] = {"prologue", "level0_reject", "decode", "validate", "level1_reject_apply", "state_uv_colour", "key_submit"};
    return i < kSegCount ? n[i] : "?";
}
enum Outcome : uint32_t { kOutRejected0 = 0, kOutDrawn, kOutOther, kOutCount };   // rejected at level 0 / reached the submit / left in between (dropped, level-1 reject, abort)
struct SegStats {
    uint64_t calls, totalUs;
    uint64_t segN[kSegCount], segUs[kSegCount];
    uint64_t outN[kOutCount], outUs[kOutCount];
    void clear() { memset(this, 0, sizeof *this); }
    // `t` = the stamps taken (t[0] .. t[marks-1], microseconds)
    void add(const uint64_t* t, uint32_t marks) {
        if (marks < 2) return;
        if (marks > kTriMarks) marks = kTriMarks;
        ++calls; const uint64_t total = t[marks - 1] >= t[0] ? t[marks - 1] - t[0] : 0; totalUs += total;
        for (uint32_t i = 0; i + 1 < marks; ++i) { ++segN[i]; segUs[i] += t[i + 1] >= t[i] ? t[i + 1] - t[i] : 0; }
        const uint32_t o = marks == kTriMarks ? (uint32_t)kOutDrawn : (marks == 3 ? (uint32_t)kOutRejected0 : (uint32_t)kOutOther);
        ++outN[o]; outUs[o] += total;
    }
};

// ---- the crossing census (WinVita's trapcnt::hits) -----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kCensusSlots = 4096;
struct CensusTop { uint32_t idx = 0; uint64_t delta = 0; };
// keeps the topN largest (idx, d) in `out`, sorted largest first; n = how many are in it
inline void census_insert(CensusTop* out, uint32_t& n, uint32_t topN, uint32_t i, uint64_t d) {
    if (topN == 0 || d == 0) return;
    uint32_t k;
    if (n < topN) k = n++;
    else if (d > out[topN - 1].delta) k = topN - 1;
    else return;
    while (k > 0 && out[k - 1].delta < d) { out[k] = out[k - 1]; --k; }
    out[k].idx = i; out[k].delta = d;
}
// the topN largest of an accumulated array; `total` = the sum of all of it
inline uint32_t census_top(const uint64_t* v, uint32_t slots, CensusTop* out, uint32_t topN, uint64_t& total) {
    total = 0; uint32_t n = 0;
    for (uint32_t i = 0; i < slots; ++i) { if (!v[i]) continue; total += v[i]; census_insert(out, n, topN, i, v[i]); }
    return n;
}
// deltas of `hits` against `prev` (prev is updated); fills the topN largest in `out` (sorted, largest first), returns how many; `total` = all crossings of the interval
inline uint32_t census_window(const uint64_t* hits, uint64_t* prev, uint32_t slots, CensusTop* out, uint32_t topN, uint64_t& total) {
    total = 0; uint32_t n = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        const uint64_t d = hits[i] >= prev[i] ? hits[i] - prev[i] : 0; prev[i] = hits[i];
        if (!d) continue;
        total += d; census_insert(out, n, topN, i, d);
    }
    return n;
}

// ---- one-entry texture dimension cache ------------------------------------------------------------------------------------------------------------------------
// The triangle path looked the texture allocation up in a std::map on EVERY textured triangle only to read its width and height. The map is modified in exactly two places (a texture allocation, guTexMemReset's
// clear): each bumps `gen`. While gen and the mmid are the same the answer cannot have changed.
struct TexDimCache {
    uint32_t gen = 0, mmid = 0, w = 0, h = 0; bool valid = false;
    uint64_t hits = 0, misses = 0;
    bool get(uint32_t curGen, uint32_t m, uint32_t& ow, uint32_t& oh) {
        if (valid && gen == curGen && mmid == m) { ow = w; oh = h; ++hits; return true; }
        ++misses; return false;
    }
    void set(uint32_t curGen, uint32_t m, uint32_t ww, uint32_t hh) { gen = curGen; mmid = m; w = ww; h = hh; valid = true; }
};

// ---- lines ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct GapIn {
    uint32_t frames = 0;
    double callsPerFrame = 0, blocksPerFrame = 0, crossingsPerFrame = 0;   // Glide calls, translated blocks, all guest->host crossings
    double logicMs = 0; uint64_t logicN = 0;                               // swap return -> first Glide call of the next frame (exact, every frame)
    uint32_t topSites = 8, topApis = 6;   // (the log keeps 2,047 characters of a line: the whole line must fit)
};
inline int format_gap(char* out, size_t n, const char* label, const GapProf& g, const ApiTable& apis, const uint64_t* apiCallsInWindow, const GapIn& in) {
    int w = 0; out[0] = 0;
    const Fit f = fit(g);
    const double cpf = in.callsPerFrame;
    app(out, n, w, "%s frames=%u | pairs=%llu dropped_long=%llu site_overflow=%llu | per_frame{glide_calls=%.1f crossings=%.1f translated_blocks=%.0f} | logic_gap_swap_to_first_call{ms=%.2f frames=%llu}",
        label, in.frames, (unsigned long long)g.pairs, (unsigned long long)g.dropped, (unsigned long long)g.siteOverflow, cpf, in.crossingsPerFrame, in.blocksPerFrame, in.logicMs, (unsigned long long)in.logicN);
    if (f.ok) {
        const double crossMs = f.a * (in.crossingsPerFrame > 0 ? in.crossingsPerFrame : cpf) / 1000.0, blocksMs = f.b * in.blocksPerFrame / 1000.0, gapMs = f.meanGap * cpf / 1000.0;
        app(out, n, w, " | fit_gap_us=a+b*blocks{a=%.2f us/crossing b=%.4f us/block (%.1f ns) r2=%.2f mean_gap=%.2f us mean_blocks=%.1f mean_handler=%.2f us} | est_ms_per_frame{gaps=%.2f of_which crossings(a)=%.2f blocks(b)=%.2f}",
            f.a, f.b, f.b * 1000.0, f.r2, f.meanGap, f.meanBlocks, f.meanHandler, gapMs, crossMs, blocksMs);
    }
    // APIs by gap time
    int order[kMaxApis]; uint32_t cnt = 0;
    for (uint32_t i = 0; i < apis.n && i < (uint32_t)kMaxApis; ++i) if (g.api[i].n) order[cnt++] = (int)i;
    for (uint32_t i = 1; i < cnt; ++i) { const int v = order[i]; uint32_t j = i; while (j > 0 && g.api[order[j - 1]].gapUs < g.api[v].gapUs) { order[j] = order[j - 1]; --j; } order[j] = v; }
    app(out, n, w, " | next_call_by_api{");
    for (uint32_t i = 0; i < cnt && i < in.topApis; ++i) {
        const ApiGap& a = g.api[order[i]]; const double callsPf = apiCallsInWindow && in.frames ? (double)apiCallsInWindow[order[i]] / (double)in.frames : 0.0;
        app(out, n, w, "%s%s:calls=%.1f gap=%.1fus blk=%.0f handler=%.1fus est=%.2fms", i ? " " : "", apis.at(order[i]), callsPf, (double)a.gapUs / (double)a.n, (double)a.blocks / (double)a.n, (double)a.handlerUs / (double)a.n,
            g.pairs ? ((double)a.gapUs / (double)g.pairs) * cpf / 1000.0 : 0.0);
    }
    app(out, n, w, "}");
    // call sites by gap time
    uint32_t idx[kSiteSlots]; uint32_t sc = 0;
    for (int i = 0; i < kSiteSlots; ++i) if (g.site[i].n) idx[sc++] = (uint32_t)i;
    const uint32_t top = in.topSites < sc ? in.topSites : sc;
    for (uint32_t i = 0; i < top; ++i) { uint32_t m = i; for (uint32_t j = i + 1; j < sc; ++j) if (g.site[idx[j]].gapUs > g.site[idx[m]].gapUs) m = j; const uint32_t t = idx[i]; idx[i] = idx[m]; idx[m] = t; }
    app(out, n, w, " | sites{");
    for (uint32_t i = 0; i < top; ++i) {
        const Site& s = g.site[idx[i]];
        app(out, n, w, "%s0x%08x/%s:n=%u gap=%.1fus blk=%.0f share=%.1f%% est=%.2fms", i ? " " : "", s.eip, apis.at(s.api), s.n, (double)s.gapUs / (double)s.n, (double)s.blocks / (double)s.n,
            g.sumGapUs ? 100.0 * (double)s.gapUs / (double)g.sumGapUs : 0.0, g.pairs ? ((double)s.gapUs / (double)g.pairs) * cpf / 1000.0 : 0.0);
    }
    app(out, n, w, "}");
    return w;
}

struct TriIn { uint32_t frames = 0; double tri_calls_per_frame = 0, clock_cost_us = 0; };
inline int format_seg(char* out, size_t n, const char* label, const SegStats& s, const TriIn& in) {
    int w = 0; out[0] = 0;
    const double c = in.clock_cost_us;   // each segment is measured between two clock reads and therefore includes about one read: subtracted
    app(out, n, w, "%s frames=%u | sampled_calls=%llu (every 32nd, own phase) of %.1f per frame | clock_read_cost_subtracted=%.2f us per segment | outcome{rejected_level0:n=%llu mean=%.1fus drawn:n=%llu mean=%.1fus other:n=%llu mean=%.1fus} | segments_mean_us{",
        label, in.frames, (unsigned long long)s.calls, in.tri_calls_per_frame, c,
        (unsigned long long)s.outN[kOutRejected0], s.outN[kOutRejected0] ? (double)s.outUs[kOutRejected0] / (double)s.outN[kOutRejected0] - c : 0.0,
        (unsigned long long)s.outN[kOutDrawn], s.outN[kOutDrawn] ? (double)s.outUs[kOutDrawn] / (double)s.outN[kOutDrawn] - c : 0.0,
        (unsigned long long)s.outN[kOutOther], s.outN[kOutOther] ? (double)s.outUs[kOutOther] / (double)s.outN[kOutOther] - c : 0.0);
    for (uint32_t i = 0; i < kSegCount; ++i) {
        const double mean = s.segN[i] ? (double)s.segUs[i] / (double)s.segN[i] - c : 0.0;
        app(out, n, w, "%s%s=%.2f(n=%llu)", i ? " " : "", seg_name(i), mean < 0 ? 0.0 : mean, (unsigned long long)s.segN[i]);
    }
    app(out, n, w, "}");
    return w;
}

struct CensusIn { uint32_t frames = 0, base = 0x7F000000u; uint64_t total = 0; double glideCallsPerFrame = 0; };
inline int format_census(char* out, size_t n, const char* label, const CensusIn& in, const CensusTop* top, uint32_t count) {
    int w = 0; out[0] = 0;
    const double fr = in.frames ? (double)in.frames : 1.0;
    app(out, n, w, "%s frames=%u | crossings_per_frame=%.1f of_which_glide=%.1f other=%.1f | top_slots{", label, in.frames, (double)in.total / fr, in.glideCallsPerFrame, (double)in.total / fr - in.glideCallsPerFrame);
    for (uint32_t i = 0; i < count; ++i)
        app(out, n, w, "%sslot%u(va=0x%08x)=%.1f", i ? " " : "", top[i].idx, in.base + top[i].idx * 16u, (double)top[i].delta / fr);
    app(out, n, w, "}");
    return w;
}

// the stages of one grBufferSwap (milliseconds per frame): where the "swap_wait" of G148 goes
struct SwapIn { uint32_t frames = 0; double total = 0, telemetry = 0, hostStages = 0, compositor = 0, upload = 0, overlay = 0, vglSwap = 0, postSwap = 0, otherPre = 0, scanMs = 0, convertMs = 0; };
inline int format_swap(char* out, size_t n, const char* label, const SwapIn& s) {
    int w = 0; out[0] = 0;
    app(out, n, w, "%s frames=%u | grBufferSwap_total=%.2f ms = entry_hooks_and_telemetry %.2f + pre_swap_host_stages %.2f (compositor %.2f incl. scan %.2f convert %.2f, upload %.2f, overlay %.2f, other %.2f) + present_call[driver/GPU wait] %.2f + post_swap_hooks %.2f",
        label, s.frames, s.total, s.telemetry, s.hostStages, s.compositor, s.scanMs, s.convertMs, s.upload, s.overlay, s.otherPre, s.vglSwap, s.postSwap);
    return w;
}

// the two optimisations of G149 and what they did in the interval (cumulative counters of the run)
// blocksTotal = the translated blocks executed since the guest's single run() started (WinVita's thread_emu_blocks). That run() has a preemption budget of 2^31 - 1 block entries; the game's main call has no scheduler to
// resume it, so if the budget ever ran out run() would return "finished" (Bridge::call_va returns ok). The figure below says how much of it a session uses.
constexpr uint64_t kBlockBudget = 2147483647ull;
struct OptIn { uint32_t groups = 0; bool scanDisabled = false; uint64_t scanVectorFrames = 0, scanChecks = 0, scanMismatches = 0, texHits = 0, texMisses = 0; const char* scanWhy = ""; bool blocksKnown = false; uint32_t blocksTotal = 0; double uptimeS = 0; };
inline int format_opt(char* out, size_t n, const char* label, const OptIn& o) {
    int w = 0; out[0] = 0;
    app(out, n, w, "%s groups=0x%02x (gap=%d seg=%d census=%d tex=%d scan=%d) | vector_scan{%s frames_vector=%llu scalar_checks=%llu mismatches=%llu%s%s} | texture_dimension_cache{%s hits=%llu misses=%llu hit_rate=%.1f%%}",
        label, o.groups, (o.groups & kGrGap) != 0, (o.groups & kGrSeg) != 0, (o.groups & kGrCensus) != 0, (o.groups & kGrTex) != 0, (o.groups & kGrScan) != 0,
        (o.groups & kGrScan) ? (o.scanDisabled ? "OFF-AUTO" : "ON") : "OFF-CONTROL", (unsigned long long)o.scanVectorFrames, (unsigned long long)o.scanChecks, (unsigned long long)o.scanMismatches, o.scanDisabled ? " why=" : "", o.scanDisabled ? o.scanWhy : "",
        (o.groups & kGrTex) ? "ON" : "OFF-CONTROL", (unsigned long long)o.texHits, (unsigned long long)o.texMisses, (o.texHits + o.texMisses) ? 100.0 * (double)o.texHits / (double)(o.texHits + o.texMisses) : 0.0);
    if (o.blocksKnown) {
        const double used = 100.0 * (double)o.blocksTotal / (double)kBlockBudget;
        app(out, n, w, " | block_budget_of_the_single_run{translated_blocks_executed=%u (%.2f%% of %llu) after %.0f s of run => ~%.0f s to exhaust at this rate}",
            o.blocksTotal, used, (unsigned long long)kBlockBudget, o.uptimeS, (o.blocksTotal && o.uptimeS > 0.0) ? o.uptimeS * (double)kBlockBudget / (double)o.blocksTotal : 0.0);
    }
    return w;
}

}}   // namespace ign::g149
