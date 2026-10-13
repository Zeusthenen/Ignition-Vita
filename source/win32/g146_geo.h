// win32/g146_geo.h -- G146: ACTIVE GEOMETRY FAST PATH. Pure (no GL, no guest access, no logging, no globals): tools/g146_test.cpp drives exactly this code.
//
// WHY G145's TWO GEOMETRY PATHS NEVER RAN (real run: rejected 0.0 %, decode fast 0.0 %, fallback 100 %, not_eligible 100 %, both races): the three conditions that every triangle had to pass were all environmental and could not be
// told apart from the log (one `not_eligible` counter): the scissor knowledge, the WinVita memory guard / Cpu::hostptr, and a single all-or-nothing memory tier. G146 (1) counts the FIRST failing condition of every triangle, (2) adds a
// second, universal rejection AFTER the stable decode that needs neither the guest-memory fast read nor the scissor record, (3) reads guest vertices through three tiers (host span, one bulk Cpu::read, the dword path), and
// (4) prints the values of the first failing triangle once per race ([G146-ELIGIBILITY-DETAIL]) so a remaining cause is named by data, not guessed.
//
// THE REJECTION RULE (two predicates, either one is enough, both sound):
//  * VIEWPORT: the draw path itself builds glOrtho(vp.x, vp.x + vp.w, vp.y, vp.y + vp.h) from the GL viewport `vp` and maps every vertex to window coordinates with the same `vp`. A triangle whose three vertices are beyond the same
//    edge of the viewport rectangle is outside the clip volume of that projection: it is clipped, it produces no fragment, whatever the scissor, depth, blend or culling state. This is the predicate G144 measured (66.9 % of the
//    heavy zone); G146 uses it with a 4-pixel margin.
//  * SCISSOR: the rectangle grClipWindow handed to glScissor, with the 40-pixel margin of G145 (needs the scissor record: armed, equal to the Glide clip registers).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g145_life.h"

namespace ign { namespace g146 {

namespace G5 = ign::g145;
using G5::Plane;

// ================================================================================================================================================
// 1. THE PREDICATES
// ================================================================================================================================================
constexpr float kViewportMarginPx = 4.0f;   // float rounding of the NDC mapping + the 4x MSAA footprint; the clip itself is exact
struct Rect { float x0, y0, x1, y1; };      // window coordinates, [x0, x1] x [y0, y1]
inline Plane outside_rect(const Rect& r, float margin, float ax, float ay, float bx, float by, float cx, float cy) {
    const float minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx), maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    const float miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy), maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    if (maxx < r.x0 - margin) return G5::kLeft;
    if (minx > r.x1 + margin) return G5::kRight;
    if (maxy < r.y0 - margin) return G5::kBottom;
    if (miny > r.y1 + margin) return G5::kTop;
    if (maxx < r.x0 || minx > r.x1 || maxy < r.y0 || miny > r.y1) return G5::kNearOutside;
    return G5::kInside;
}
enum By : uint8_t { kByNone = 0, kByViewport = 1, kByScissor = 2 };
struct Decision { Plane plane = G5::kNotDecidable; By by = kByNone; bool outsideViewportExact = false; };
// vp = the GL viewport the draw path uses (x, y, w, h). The scissor is optional (unusable -> only the viewport predicate decides).
inline Decision decide(const G5::Scissor& s, const int32_t vp[4], float ax, float ay, float bx, float by, float cx, float cy) {
    Decision d;
    if (!G5::sane(ax) || !G5::sane(ay) || !G5::sane(bx) || !G5::sane(by) || !G5::sane(cx) || !G5::sane(cy) || vp[2] <= 0 || vp[3] <= 0) return d;   // kNotDecidable
    const Rect v{(float)vp[0], (float)vp[1], (float)vp[0] + (float)vp[2], (float)vp[1] + (float)vp[3]};
    const Plane pv = outside_rect(v, kViewportMarginPx, ax, ay, bx, by, cx, cy);
    d.outsideViewportExact = outside_rect(v, 0.0f, ax, ay, bx, by, cx, cy) != G5::kInside;   // the G144 definition (viewport, no margin), counted next to the margin-based decision to reconcile the two
    if (G5::is_reject(pv)) { d.plane = pv; d.by = kByViewport; return d; }
    Plane ps = G5::kInside;
    if (s.usable()) { ps = G5::classify_outside(s, ax, ay, bx, by, cx, cy); if (G5::is_reject(ps)) { d.plane = ps; d.by = kByScissor; return d; } }
    d.plane = (pv == G5::kNearOutside || ps == G5::kNearOutside) ? G5::kNearOutside : G5::kInside;
    return d;
}

// ================================================================================================================================================
// 2. ELIGIBILITY: the FIRST failing condition of every triangle (the old single `not_eligible` hid all of them)
// ================================================================================================================================================
enum El : uint8_t {
    kElOk = 0,
    kElDisabled,        // switched off: control file (geo=off / reject=off), or a FULL verifier
    kElCarousel,        // the car carousel is active (its triangles are replayed with the scissor OFF)
    kElNoFirstTriangle, // before the first triangle of the process was drawn
    kElViewportInvalid, // the viewport the draw path would use has no area
    kElPtrWrap,         // a vertex pointer within 8 bytes of the end of the 32-bit space
    kElMemUnreadable,   // neither a host span nor a bulk Cpu::read could deliver the 8 bytes of a position
    kElGuardUnmapped,   // the WinVita memory guard is on and the page is not mapped
    kElNotFinite,       // a coordinate is NaN / Inf / beyond 1e6: the stable path decides
    kElCount
};
inline const char* el_name(uint32_t r) {
    static const char* const n[kElCount] = {"ok", "disabled", "carousel_active", "before_first_triangle", "viewport_invalid", "pointer_wraps", "memory_unreadable", "memory_guard_unmapped", "not_finite_or_huge"};
    return r < kElCount ? n[r] : "?";
}
enum Dr : uint8_t { kDrNone = 0, kDrDisabled, kDrPtrWrap, kDrMemUnreadable, kDrGuardUnmapped, kDrCount };   // why a vertex went through the dword path
inline const char* dr_name(uint32_t r) { static const char* const n[kDrCount] = {"none", "disabled", "pointer_wraps", "memory_unreadable", "memory_guard_unmapped"}; return r < kDrCount ? n[r] : "?"; }
// the order in which the conditions of level 0 are checked, and how a memory failure maps to an eligibility reason (the cpp uses exactly these two functions)
inline El preconditions(bool rejectOn, bool carousel, bool firstTriangleDrawn) { return !rejectOn ? kElDisabled : carousel ? kElCarousel : !firstTriangleDrawn ? kElNoFirstTriangle : kElOk; }
inline El el_from_dr(Dr d) { return d == kDrPtrWrap ? kElPtrWrap : d == kDrGuardUnmapped ? kElGuardUnmapped : kElMemUnreadable; }

// ================================================================================================================================================
// 3. THREE-TIER READ OF GUEST MEMORY (templated on the memory interface so the host test can fake guest memory and its failures)
//    M::range(va, n)  -> host pointer to [va, va + n) when the range lies inside the guest arena (Cpu::hostptr), else nullptr
//    M::read(va, dst, n) -> bool  one bulk Cpu::read: true when it delivered the bytes (a failed read zero-fills and returns false)
//    M::mapped(va)    -> bool   the page of `va` is mapped (only consulted while the memory guard is on)
//  tier 1 = host span (memcpy), tier 2 = one bulk read, 0 = failed (`why` says why; the caller uses the dword path, the reference).
//  Exactness: Cpu::read of n bytes == "[guard: unmapped first byte -> zeros] + arena check + memcpy". A vertex / position range spans at most two adjacent pages, so with the guard on,
//  mapped(first) && mapped(last) means the whole range is mapped and the bulk read is the same bytes the dword reads would have returned.
// ================================================================================================================================================
enum Tier : uint8_t { kTierNone = 0, kTierSpan = 1, kTierBulk = 2 };
template <class M>
inline Tier read_block(M& m, uint32_t va, uint32_t n, void* out, bool guard, Dr& why) {
    why = kDrNone;
    if (va > 0xFFFFFFFFu - n) { why = kDrPtrWrap; return kTierNone; }
    if (guard) { if (!m.mapped(va) || !m.mapped(va + n - 1u)) { why = kDrGuardUnmapped; return kTierNone; } }
    else { const uint8_t* p = m.range(va, n); if (p) { memcpy(out, p, n); return kTierSpan; } }
    if (m.read(va, out, n)) return kTierBulk;
    why = kDrMemUnreadable; return kTierNone;
}

// ================================================================================================================================================
// 4. WINDOW COUNTERS (integers only; one line per 120 race frames)
// ================================================================================================================================================
constexpr int kHistBins = 80;               // frame-time histogram: 5 ms bins up to 400 ms (+ overflow in the last bin)
constexpr uint32_t kHistBinUs = 5000;
constexpr uint32_t kSlowFrameUs = 66667;    // below 15 FPS
struct Frame {
    uint32_t frames = 0;
    uint64_t received = 0, eligible = 0, elig[kElCount] = {}, rejected[4] = {0, 0, 0, 0}, rejectedEarly = 0, rejectedPost = 0, rejectedByViewport = 0, rejectedByScissor = 0, nearOutside = 0, postChecked = 0, outsideViewportExact = 0,
             processed = 0, drawn = 0, decodeSpan = 0, decodeBulk = 0, decodeSlow = 0, decWhy[kDrCount] = {}, trackTris = 0, verified = 0, verifyBad = 0, decodeChecked = 0, decodeBad = 0, slowFrames = 0, maxFrameUs = 0;
    uint64_t draws = 0, sessionTris = 0, sessionCloses = 0, glCalls = 0;
    uint64_t sceneUs = 0, compUs = 0, swapUs = 0, frameUs = 0, uploadUs = 0, overlayUs = 0, presentUs = 0;
    uint32_t hist[kHistBins] = {};
    G5::StateFlow flow;
    void clear() { *this = Frame(); }
    uint64_t rejected_total() const { return rejected[0] + rejected[1] + rejected[2] + rejected[3]; }
    uint64_t decode_fast() const { return decodeSpan + decodeBulk; }
    void note_frame_time(uint64_t us) { int b = (int)(us / kHistBinUs); if (b >= kHistBins) b = kHistBins - 1; ++hist[b]; if (us > maxFrameUs) maxFrameUs = us; if (us > kSlowFrameUs) ++slowFrames; }
    // upper edge (ms) of the bin that holds the p-th percentile (0..100) of the frame times
    double percentile_ms(double p) const {
        uint64_t n = 0; for (int i = 0; i < kHistBins; ++i) n += hist[i];
        if (!n) return 0.0;
        const uint64_t need = (uint64_t)(p / 100.0 * (double)n + 0.999999); uint64_t acc = 0;
        for (int i = 0; i < kHistBins; ++i) { acc += hist[i]; if (acc >= (need ? need : 1)) return (double)(i + 1) * (double)kHistBinUs / 1000.0; }
        return (double)kHistBins * (double)kHistBinUs / 1000.0;
    }
    void add(const Frame& o) {
        frames += o.frames; received += o.received; eligible += o.eligible; for (int i = 0; i < kElCount; ++i) elig[i] += o.elig[i]; for (int i = 0; i < 4; ++i) rejected[i] += o.rejected[i];
        rejectedEarly += o.rejectedEarly; rejectedPost += o.rejectedPost; rejectedByViewport += o.rejectedByViewport; rejectedByScissor += o.rejectedByScissor; nearOutside += o.nearOutside; postChecked += o.postChecked; outsideViewportExact += o.outsideViewportExact;
        processed += o.processed; drawn += o.drawn; decodeSpan += o.decodeSpan; decodeBulk += o.decodeBulk; decodeSlow += o.decodeSlow; for (int i = 0; i < kDrCount; ++i) decWhy[i] += o.decWhy[i];
        trackTris += o.trackTris; verified += o.verified; verifyBad += o.verifyBad; decodeChecked += o.decodeChecked; decodeBad += o.decodeBad; slowFrames += o.slowFrames; if (o.maxFrameUs > maxFrameUs) maxFrameUs = o.maxFrameUs;
        draws += o.draws; sessionTris += o.sessionTris; sessionCloses += o.sessionCloses; glCalls += o.glCalls;
        sceneUs += o.sceneUs; compUs += o.compUs; swapUs += o.swapUs; frameUs += o.frameUs; uploadUs += o.uploadUs; overlayUs += o.overlayUs; presentUs += o.presentUs;
        for (int i = 0; i < kHistBins; ++i) hist[i] += o.hist[i];
        G5::StateFlow f = o.flow; f.take(flow);
    }
};
// worst 30-frame stretch of a window (the "sustained minimum" the average of 120 frames hides)
struct Roll30 {
    uint64_t ring[30] = {}; uint32_t n = 0, head = 0; uint64_t sum = 0, worstSum = 0;
    void clear_window() { worstSum = 0; }
    void push(uint64_t us) { if (n == 30) sum -= ring[head]; else ++n; ring[head] = us; head = (head + 1) % 30; sum += us; if (n == 30 && sum > worstSum) worstSum = sum; }
    double worst_fps() const { return worstSum ? 30.0e6 / (double)worstSum : 0.0; }
    void reset() { *this = Roll30(); }
};
// Pearson correlation of the frame time with one per-frame quantity over a race (doubles; five sums per quantity)
struct Corr {
    double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    void add(double x, double y) { n += 1; sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; }
    double r() const { const double dx = n * sxx - sx * sx, dy = n * syy - sy * sy; if (n < 3 || dx <= 0 || dy <= 0) return 0.0; return (n * sxy - sx * sy) / __builtin_sqrt(dx * dy); }
};
struct RaceCorr { Corr tris, state, gl, closes, draws; void clear() { *this = RaceCorr(); } };

inline int format_window(char* out, size_t n, const char* label, const char* track, const Frame& w, double worst30Fps, bool full) {
    const double f = w.frames ? (double)w.frames : 1.0, rcv = w.received ? (double)w.received : 1.0, proc = w.processed ? (double)w.processed : 1.0, dr = w.draws ? (double)w.draws : 1.0;
    const double fps = w.frameUs ? 1.0e6 * (double)w.frames / (double)w.frameUs : 0.0;
    int len = snprintf(out, n,
        "%s frames=%u fps=%.2f worst30_fps=%.2f frame_ms{p50=%.0f p90=%.0f p95=%.0f p99=%.0f max=%.1f} frames_below_15fps=%llu | ms_per_frame{frame=%.2f scene=%.2f compositor=%.2f swap=%.2f upload=%.2f overlay=%.2f present=%.2f} | "
        "per_frame{received=%.1f eligible=%.1f rejected=%.1f(%.1f%%){early=%.1f after_decode=%.1f by_viewport=%.1f by_scissor=%.1f | left=%.1f right=%.1f bottom=%.1f top=%.1f} near_outside=%.1f outside_viewport_exact=%.1f processed=%.1f drawn=%.1f track_geometry=%.1f} | "
        "decode{span=%.1f%% bulk=%.1f%% dword=%.1f%%} | submit{draws=%.1f avg_batch=%.2f est_draws_avoided=%.1f session_closes=%.1f gl_calls=%.0f} | scene_us_per_received=%.1f scene_us_per_drawn=%.1f | "
        "state_flow_per_frame{state_calls=%.1f superseded_before_visible=%.1f call_groups=%.1f groups_with_only_rejected_triangles=%.1f(calls=%.1f)}",
        label, w.frames, fps, worst30Fps, w.percentile_ms(50), w.percentile_ms(90), w.percentile_ms(95), w.percentile_ms(99), (double)w.maxFrameUs / 1000.0, (unsigned long long)w.slowFrames,
        (double)w.frameUs / f / 1000.0, (double)w.sceneUs / f / 1000.0, (double)w.compUs / f / 1000.0, (double)w.swapUs / f / 1000.0, (double)w.uploadUs / f / 1000.0, (double)w.overlayUs / f / 1000.0, (double)w.presentUs / f / 1000.0,
        (double)w.received / f, (double)w.eligible / f, (double)w.rejected_total() / f, 100.0 * (double)w.rejected_total() / rcv, (double)w.rejectedEarly / f, (double)w.rejectedPost / f, (double)w.rejectedByViewport / f, (double)w.rejectedByScissor / f,
        (double)w.rejected[0] / f, (double)w.rejected[1] / f, (double)w.rejected[2] / f, (double)w.rejected[3] / f, (double)w.nearOutside / f, (double)w.outsideViewportExact / f, (double)w.processed / f, (double)w.drawn / f, (double)w.trackTris / f,
        100.0 * (double)w.decodeSpan / proc, 100.0 * (double)w.decodeBulk / proc, 100.0 * (double)w.decodeSlow / proc,
        (double)w.draws / f, (double)w.sessionTris / dr, w.draws ? (double)w.rejected_total() / ((double)w.sessionTris / dr) / f : 0.0, (double)w.sessionCloses / f, (double)w.glCalls / f,
        w.received ? (double)w.sceneUs / (double)w.received : 0.0, w.drawn ? (double)w.sceneUs / (double)w.drawn : 0.0,
        (double)w.flow.stateCalls / f, (double)w.flow.superseded / f, (double)w.flow.groups / f, (double)w.flow.groupsRejectedOnly / f, (double)w.flow.callsBeforeRejectedOnly / f);
    if (full && len > 0 && (size_t)len + 160 < n)
        len += snprintf(out + len, n - (size_t)len, " | FULL_verifiers{reject_checked=%llu reject_bad=%llu decode_checked=%llu decode_bad=%llu}", (unsigned long long)w.verified, (unsigned long long)w.verifyBad, (unsigned long long)w.decodeChecked, (unsigned long long)w.decodeBad);
    if (len > 0 && (size_t)len + 100 < n && track && track[0]) len += snprintf(out + len, n - (size_t)len, " | %s", track);
    return len;
}
// [G146-ELIGIBILITY] race#N win=N received=X eligible=X reasons{...} decode{...}
inline int format_eligibility(char* out, size_t n, uint32_t race, uint32_t win, const Frame& w) {
    int len = snprintf(out, n, "race#%u win=%u received=%llu eligible=%llu(%.1f%%) reasons{", race, win, (unsigned long long)w.received, (unsigned long long)w.eligible, w.received ? 100.0 * (double)w.eligible / (double)w.received : 0.0);
    bool any = false;
    for (int r = 0; r < kElCount && len > 0 && (size_t)len + 80 < n; ++r) if (w.elig[r]) { len += snprintf(out + len, n - (size_t)len, "%s%s=%llu", any ? " " : "", el_name((uint32_t)r), (unsigned long long)w.elig[r]); any = true; }
    if (len > 0 && (size_t)len + 3 < n) len += snprintf(out + len, n - (size_t)len, "%s}", any ? "" : "none");
    if (len > 0 && (size_t)len + 200 < n) len += snprintf(out + len, n - (size_t)len, " decode{processed=%llu span=%llu bulk=%llu dword=%llu why{", (unsigned long long)w.processed, (unsigned long long)w.decodeSpan, (unsigned long long)w.decodeBulk, (unsigned long long)w.decodeSlow);
    any = false;
    for (int r = 1; r < kDrCount && len > 0 && (size_t)len + 60 < n; ++r) if (w.decWhy[r]) { len += snprintf(out + len, n - (size_t)len, "%s%s=%llu", any ? " " : "", dr_name((uint32_t)r), (unsigned long long)w.decWhy[r]); any = true; }
    if (len > 0 && (size_t)len + 80 < n) len += snprintf(out + len, n - (size_t)len, "%s}} rejected{early=%llu after_decode=%llu} post_decode_checked=%llu", any ? "" : "none", (unsigned long long)w.rejectedEarly, (unsigned long long)w.rejectedPost, (unsigned long long)w.postChecked);
    return len;
}

// ================================================================================================================================================
// 5. THE CONTROL FILE (optional, ux0:data/ignition/g146_geo.txt; absent = normal run): tokens geo=off, reject=off, decode=off  (lets ONE VPK measure the geometry paths on and off)
// ================================================================================================================================================
struct Control { bool rejectOn = true, decodeOn = true; };
inline Control parse_control(const char* text) {
    Control c; if (!text) return c;
    const bool geoOff = strstr(text, "geo=off") != nullptr;
    if (geoOff || strstr(text, "reject=off")) c.rejectOn = false;
    if (geoOff || strstr(text, "decode=off")) c.decodeOn = false;
    return c;
}

} }   // namespace ign::g146
