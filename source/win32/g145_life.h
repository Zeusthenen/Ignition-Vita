// win32/g145_life.h -- G145: RACE LIFECYCLE (explicit state machine), TRIVIAL REJECTION of triangles that lie completely outside the scissor, FAST GUEST-VERTEX READ and the cheap windowed counters of both.
// Pure (no GL, no guest access, no logging, no globals): tools/g145_test.cpp drives exactly this code.
//
// 1. WHY THE SECOND RACE WAS NOT ARMED (G144 real test: race 1 98 % fast frames, race 2 none). G143's mode machine decides RACE / OTHER from the guest word [0x00537180] (`flag == 1` -> RACE, a trusted flag != 1
//    -> OTHER). Disassembly of the guest main loop (0x00411330): that word is set to 1 only for ONE iteration of the track load (0x004113E0), then 1 -> 2 -> 3 -> 0 happens across the load steps (0x00411473,
//    0x0041144E, 0x00411430) and the racing frames run with the word at 0 and [0x00509D2C] = 1; it is set to 1 again only by the results code (0x00411D32 / 0x00411D54 path). G143 latches "trusted" the first time a swap
//    sees the word equal to 1 (any swap once a race scene exists: the loading swaps of race 2 do), and from then on every frame with the word at 0 is OTHER, i.e. the whole second race. Race 1 never latched it (all its
//    swaps saw 0), which is why race 1 worked. The word is therefore not a "race running" signal and nothing in G145 trusts it (it is only LOGGED next to the new state, as evidence).
//    G145 builds the lifecycle from facts that were proven in earlier phases: the structural track load (G142 load sequence: first .COL/.PAN/.TAB/.PLC/.MSH/.POS/.TRI/.SRF open of a new load), the .TRI / .SRF of that load, race
//    geometry actually submitted by the guest (the three track display-list callers 0x00450C88 / 0x00451336 / 0x00451618, G137), the pause condition (G136) and the car carousel (G125).
//
// 2. TRIVIAL REJECTION. A triangle is dropped before any decoding only when its three vertices, mapped to window coordinates EXACTLY as the draw path maps them (glide_logical_xy_to_physical with the current viewport),
//    are all on the outside of the SAME edge of the GL scissor rectangle that grClipWindow set (and that is enabled), by more than kRejectMarginPx. Whatever the rasteriser does, such a triangle covers no pixel inside the
//    scissor rectangle: the margin (40 px > one 32 px GXM tile) makes the proof independent of the rounding of the scissor, of the 4x MSAA footprint and of any tile-granular region clip. Anything doubtful (not finite, huge,
//    clip unknown / disabled / changed behind our back, car carousel) takes the unchanged G144 path.
//
// 3. FAST READ. A GrVertex is 15 dwords of guest memory. The G144 path reads them one dword at a time through Cpu::read (a virtual call + range check + 4-byte copy each, 45 per triangle + the arguments). The fast path
//    validates the whole 60-byte range ONCE (Cpu::hostptr: the range lies inside the guest arena) and copies it with one memcpy; it exists only when the WinVita memory guard (WX86_MEMGUARD / D2_MEMGUARD, which makes
//    read() return zeros for unmapped pages) is off, so that read() == "arena check + memcpy" == what the fast path does. Bits are copied, never converted. Any failed check -> the dword path.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g144_fast.h"

namespace ign { namespace g145 {

// ================================================================================================================================================
// 1. RACE LIFECYCLE
// ================================================================================================================================================
enum State : uint8_t { kMenu = 0, kLoading, kRaceSettling, kRace, kPause, kResults, kExit, kStateCount };
inline const char* state_name(uint32_t s) {
    switch (s) { case kMenu: return "MENU"; case kLoading: return "LOADING"; case kRaceSettling: return "RACE_SETTLING"; case kRace: return "RACE"; case kPause: return "PAUSE"; case kResults: return "RESULTS"; case kExit: return "EXIT"; default: return "?"; }
}
struct LifeIn {
    uint32_t loadSeq = 0;         // structural track loads seen so far (kernel32_ign.cpp: g_g142LoadSeq)
    bool geometryFiles = false;   // the .TRI or the .SRF of the load in progress has been opened (EVIDENCE only: it is logged next to the state, it is not a condition of any transition)
    uint32_t trackTris = 0;       // triangles of the track display list submitted by the guest during this swap (rejected or not)
    bool anyTris = false;         // the swap had triangles at all
    bool pause = false;           // the guest's pause condition (G136)
    bool carousel = false;        // the front-end car carousel / its iris (G125)
    bool exiting = false;         // the process is closing the window
};
constexpr uint32_t kEnterSwaps = 2;     // swaps with race geometry in a row before a load (or the results) becomes a race
constexpr uint32_t kSettleSwaps = 8;    // swaps after a race entry / a resume before the state is RACE (same number as the arbiter's settling)
constexpr uint32_t kLeaveSwaps = 5;     // swaps without race geometry in a row (not paused) before the race is over
constexpr uint32_t kMenuSwaps = 120;    // swaps without any 3D after the results before the state is MENU again
class Life {
public:
    struct Change { bool changed = false, newRace = false; State from = kMenu, to = kMenu; };
    Change feed(const LifeIn& in) {
        Change ch;
        ++swaps_;
        if (state_ == kExit) return ch;
        if (in.exiting) { enter(kExit, ch); return ch; }
        const bool loadNew = in.loadSeq != lastLoad_;
        lastLoad_ = in.loadSeq;
        const bool geom = in.trackTris > 0 && in.anyTris && !in.carousel;
        if (!in.pause) { if (geom) { ++geomRun_; noGeomRun_ = 0; } else { geomRun_ = 0; ++noGeomRun_; } }
        if (loadNew) { enter(kLoading, ch); geomRun_ = geom ? 1u : 0u; noGeomRun_ = geom ? 0u : 1u; return ch; }
        switch (state_) {
            case kMenu: ++inState_; break;
            case kLoading:
                ++inState_;
                if (geomRun_ >= kEnterSwaps && !in.pause) { enter(kRaceSettling, ch); ch.newRace = true; }   // (in.geometryFiles is evidence for the log, not a condition: a load whose GL scene entry reset the load latch mid-way would otherwise never be a race)
                break;
            case kRaceSettling: case kRace:
                if (in.pause) { enter(kPause, ch); break; }
                if (noGeomRun_ >= kLeaveSwaps) { enter(kResults, ch); break; }
                ++inState_; ++entry_;
                if (state_ == kRaceSettling && entry_ >= kSettleSwaps) enter(kRace, ch);
                break;
            case kPause:
                ++inState_;
                if (!in.pause) { enter(kRaceSettling, ch); }   // resume: settle again (the G143 boundary clear of the resume is scheduled by its own machine)
                break;
            case kResults:
                ++inState_;
                if (geomRun_ >= kEnterSwaps && !in.pause) { enter(kRaceSettling, ch); ch.newRace = true; }   // a restart without a new load
                else if (noGeomRun_ >= kMenuSwaps && !in.anyTris) enter(kMenu, ch);
                break;
            default: break;
        }
        return ch;
    }
    State state() const { return state_; }
    bool race_family() const { return state_ == kRaceSettling || state_ == kRace; }   // the race is running (settling included)
    uint32_t entry_swaps() const { return entry_; }       // swaps since the race entry / the last resume (the entry swap counts as 1)
    uint32_t in_state() const { return inState_; }
    uint32_t race_no() const { return raceNo_; }
    uint32_t changes() const { return changes_; }
    uint32_t swaps() const { return swaps_; }
    void context_reset() { if (state_ != kExit) { state_ = kMenu; inState_ = 0; entry_ = 0; geomRun_ = noGeomRun_ = 0; } }   // a new Glide context: nothing of the previous one is trusted (the race count and the load sequence are kept)
private:
    void enter(State to, Change& ch) {
        ch.changed = (to != state_); ch.from = state_; ch.to = to;
        if (to != state_) ++changes_;
        if (to == kRaceSettling) { entry_ = 1; if (state_ == kLoading || state_ == kResults) ++raceNo_; }   // the entry (or resume) swap counts as 1; a resume from the pause is not a new race
        else if (to != kRace) entry_ = 0;
        state_ = to; inState_ = 0;
    }
    State state_ = kMenu; uint32_t inState_ = 0, entry_ = 0, raceNo_ = 0, changes_ = 0, swaps_ = 0, lastLoad_ = 0, geomRun_ = 0, noGeomRun_ = 0;
};

// The three display-list callers of the track geometry (G137): race geometry is what these submit.
constexpr uint32_t kTrackCaller0 = 0x00450C88u, kTrackCaller1 = 0x00451336u, kTrackCaller2 = 0x00451618u;
inline bool is_track_caller(uint32_t eip) { return eip == kTrackCaller0 || eip == kTrackCaller1 || eip == kTrackCaller2; }

// ---- per-race tally: what the compositor did in this race --------------------------------------------------------------------------------------------
struct RaceTally {
    uint32_t no = 0, enterSwap = 0, leaveSwap = 0, firstFullSwap = 0, firstFastSwap = 0, swaps = 0, scanned = 0, fast = 0, full = 0, pauses = 0, resumes = 0;
    uint32_t reasons[ign::g144::kReasonCount] = {};
    uint32_t guestFlag = 0xFFFFFFFFu, guestLoop = 0xFFFFFFFFu, trackIdx = 0xFFFFFFFFu;     // [0x00537180] / [0x00509D2C] / track index [0x00536EA4] at the race entry (evidence, never used for a decision)
    void begin(uint32_t raceNo, uint32_t swap) { *this = RaceTally(); no = raceNo; enterSwap = swap; }
    void swap_in_race() { ++swaps; }
    void frame(uint32_t swap, bool fastFrame, uint32_t reason) {
        ++scanned; ++reasons[reason < ign::g144::kReasonCount ? reason : 0];
        if (fastFrame) { ++fast; if (!firstFastSwap) firstFastSwap = swap; }
        else { ++full; if (!firstFullSwap) firstFullSwap = swap; }
    }
    bool armed() const { return fast > 0; }
};
inline int format_race_line(char* out, size_t n, const RaceTally& t, const char* modeName, const char* g143Mode, bool final) {
    const double sc = t.scanned ? (double)t.scanned : 1.0;
    int len = snprintf(out, n, "race#%u mode=%s fastpath_armed=%d first_full_swap=%u first_incremental_swap=%u | %s: race_swaps=%u scanned=%u fast=%u(%.1f%%) full_path=%u pauses=%u | life{enter_swap=%u leave_swap=%u} | guest_evidence{track_idx[0x00536EA4]=%d race_flag[0x00537180]=%u loop[0x00509D2C]=%u g143_mode=%s} | full_path_reasons{",
                       t.no, modeName, t.armed() ? 1 : 0, t.firstFullSwap, t.firstFastSwap, final ? "race_total" : "so_far", t.swaps, t.scanned, t.fast, 100.0 * (double)t.fast / sc, t.full, t.pauses, t.enterSwap, t.leaveSwap,
                       (int)t.trackIdx, t.guestFlag, t.guestLoop, g143Mode ? g143Mode : "?");
    bool any = false;
    for (int r = 1; r < ign::g144::kReasonCount && len > 0 && (size_t)len + 90 < n; ++r)
        if (t.reasons[r]) { len += snprintf(out + len, n - (size_t)len, "%s%s=%u", any ? " " : "", ign::g144::reason_name((uint32_t)r), t.reasons[r]); any = true; }
    if (len > 0 && (size_t)len + 4 < n) len += snprintf(out + len, n - (size_t)len, "%s}", any ? "" : "none");
    return len;
}

// ================================================================================================================================================
// 2. TRIVIAL REJECTION against the GL scissor rectangle (window coordinates, origin bottom-left, the same space as the mapped vertices)
// ================================================================================================================================================
constexpr float kRejectMarginPx = 40.0f;      // > one 32-pixel GXM tile: no rounding of the scissor / region clip can matter
constexpr float kRejectMaxAbs = 1.0e6f;       // coordinates beyond this are "doubtful": the full path decides
struct Scissor {
    bool armed = false;                       // grClipWindow enabled the scissor test and nothing has disabled it since
    int32_t x = 0, y = 0, w = 0, h = 0;       // the rectangle handed to glScissor
    bool usable() const { return armed && w > 0 && h > 0; }
};
enum Plane : uint8_t { kInside = 0, kLeft, kRight, kBottom, kTop, kNearOutside, kNotDecidable };
inline const char* plane_name(uint32_t p) { switch (p) { case kLeft: return "left"; case kRight: return "right"; case kBottom: return "bottom"; case kTop: return "top"; case kNearOutside: return "near_outside"; case kNotDecidable: return "not_decidable"; default: return "inside"; } }
inline bool sane(float v) { return std::isfinite(v) && v > -kRejectMaxAbs && v < kRejectMaxAbs; }
// kLeft..kTop: all three vertices are beyond the same edge by more than the margin (reject). kNearOutside: all beyond the same edge but within the margin (counted, never rejected).
// kInside: not entirely outside any single edge. kNotDecidable: a coordinate is not finite / huge, or the scissor is not usable.
inline Plane classify_outside(const Scissor& s, float ax, float ay, float bx, float by, float cx, float cy) {
    if (!s.usable() || !sane(ax) || !sane(ay) || !sane(bx) || !sane(by) || !sane(cx) || !sane(cy)) return kNotDecidable;
    const float x0 = (float)s.x, x1 = (float)s.x + (float)s.w, y0 = (float)s.y, y1 = (float)s.y + (float)s.h, m = kRejectMarginPx;
    const float minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx), maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    const float miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy), maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    if (maxx < x0 - m) return kLeft;
    if (minx > x1 + m) return kRight;
    if (maxy < y0 - m) return kBottom;
    if (miny > y1 + m) return kTop;
    if (maxx < x0 || minx > x1 || maxy < y0 || miny > y1) return kNearOutside;
    return kInside;
}
inline bool is_reject(Plane p) { return p >= kLeft && p <= kTop; }

// ================================================================================================================================================
// 3. FAST READ OF A GUEST GRVERTEX / ITS POSITION (templated on a memory interface so the host test can fake guest memory)
// ================================================================================================================================================
constexpr uint32_t kVertexBytes = 60;   // 15 dwords (x y z r g b ooz a oow sow0 tow0 oow0 sow1 tow1 oow1)
constexpr uint32_t kPosBytes = 8;       // x, y
// Mem: `const uint8_t* range(uint32_t va, uint32_t n)` returns a host pointer to [va, va + n) when that range lies inside the guest arena, else nullptr.
template <class Mem>
inline bool read_vertex_fast(Mem& m, uint32_t va, float out[15]) {
    if (va > 0xFFFFFFFFu - kVertexBytes) return false;
    const uint8_t* p = m.range(va, kVertexBytes);
    if (!p) return false;
    memcpy(out, p, kVertexBytes);
    return true;
}
template <class Mem>
inline bool read_pos_fast(Mem& m, uint32_t va, float& x, float& y) {
    if (va > 0xFFFFFFFFu - kPosBytes) return false;
    const uint8_t* p = m.range(va, kPosBytes);
    if (!p) return false;
    memcpy(&x, p, 4); memcpy(&y, p + 4, 4);
    return true;
}
// The dword-by-dword reference (exactly what gu_read_grvertex does): Mem2::read_u32(va).
template <class Mem2>
inline void read_vertex_slow(Mem2& m, uint32_t va, float out[15]) {
    for (int i = 0; i < 15; ++i) { const uint32_t bits = m.read_u32(va + (uint32_t)i * 4u); memcpy(&out[i], &bits, 4); }
}

// ================================================================================================================================================
// 4. STATE FLOW (aggregated counters only: nothing is skipped or merged): how many state calls only precede geometry that is rejected, how many are superseded before the next processed triangle
// ================================================================================================================================================
struct StateFlow {
    uint32_t stateCalls = 0, superseded = 0, groups = 0, groupsRejectedOnly = 0, callsBeforeRejectedOnly = 0;
    // calls that never reached a triangle at all (state set again before any triangle) are the `superseded` ones; the others are counted by group
    void note_call(uint32_t id) { ++stateCalls; ++run_; const uint32_t bit = 1u << (id & 31u); if (pend_ & bit) ++superseded; pend_ |= bit; }
    void note_triangle(bool processed) {
        if (run_) {                                   // a run of state calls ended: close the previous group, open a new one
            close_group();
            gOpen_ = true; gCalls_ = run_; gProcessed_ = false; run_ = 0;
        }
        if (processed) { gProcessed_ = true; pend_ = 0; }
    }
    void frame_end() { close_group(); gOpen_ = false; run_ = 0; pend_ = 0; gCalls_ = 0; gProcessed_ = false; }
    void take(StateFlow& into) { into.stateCalls += stateCalls; into.superseded += superseded; into.groups += groups; into.groupsRejectedOnly += groupsRejectedOnly; into.callsBeforeRejectedOnly += callsBeforeRejectedOnly; stateCalls = superseded = groups = groupsRejectedOnly = callsBeforeRejectedOnly = 0; }
private:
    void close_group() { if (gOpen_) { ++groups; if (!gProcessed_) { ++groupsRejectedOnly; callsBeforeRejectedOnly += gCalls_; } } gOpen_ = false; }
    uint32_t pend_ = 0, run_ = 0, gCalls_ = 0; bool gOpen_ = false, gProcessed_ = false;
};

// ================================================================================================================================================
// 5. WINDOWED COUNTERS (integers only; one line per 120 race frames)
// ================================================================================================================================================
constexpr uint32_t kGeoWindowFrames = 120;
struct GeoFrame {
    uint32_t frames = 0;
    uint64_t received = 0, rejected[4] = {0, 0, 0, 0}, nearOutside = 0, notEligible = 0, processed = 0, decodeFast = 0, decodeSlow = 0, trackTris = 0, verified = 0, verifyBad = 0, decodeChecked = 0, decodeBad = 0;
    uint64_t draws = 0, sessionTris = 0, sessionCloses = 0, glCalls = 0;
    uint64_t sceneUs = 0, compUs = 0, swapUs = 0, frameUs = 0, uploadUs = 0, overlayUs = 0, presentUs = 0;
    uint64_t closesAnalysed = 0, closesSameKey = 0, framesAnalysed = 0;
    StateFlow flow;
    void clear() { *this = GeoFrame(); }
    uint64_t rejected_total() const { return rejected[0] + rejected[1] + rejected[2] + rejected[3]; }
    void add(const GeoFrame& o) {
        frames += o.frames; received += o.received; for (int i = 0; i < 4; ++i) rejected[i] += o.rejected[i]; nearOutside += o.nearOutside; notEligible += o.notEligible; processed += o.processed;
        decodeFast += o.decodeFast; decodeSlow += o.decodeSlow; trackTris += o.trackTris; verified += o.verified; verifyBad += o.verifyBad; decodeChecked += o.decodeChecked; decodeBad += o.decodeBad;
        draws += o.draws; sessionTris += o.sessionTris; sessionCloses += o.sessionCloses; glCalls += o.glCalls;
        sceneUs += o.sceneUs; compUs += o.compUs; swapUs += o.swapUs; frameUs += o.frameUs; uploadUs += o.uploadUs; overlayUs += o.overlayUs; presentUs += o.presentUs;
        closesAnalysed += o.closesAnalysed; closesSameKey += o.closesSameKey; framesAnalysed += o.framesAnalysed;
        StateFlow f = o.flow; f.take(flow);
    }
};
inline int format_geo_window(char* out, size_t n, const char* label, const char* track, const GeoFrame& w, bool full) {
    const double f = w.frames ? (double)w.frames : 1.0, rcv = w.received ? (double)w.received : 1.0, proc = w.processed ? (double)w.processed : 1.0, dr = w.draws ? (double)w.draws : 1.0;
    const double fps = w.frameUs ? 1.0e6 * (double)w.frames / (double)w.frameUs : 0.0;
    int len = snprintf(out, n,
        "%s frames=%u fps=%.2f ms_per_frame{frame=%.2f scene=%.2f compositor=%.2f swap=%.2f upload=%.2f overlay=%.2f present=%.2f} | per_frame{tris_received=%.1f rejected=%.1f(%.1f%%){left=%.1f right=%.1f bottom=%.1f top=%.1f} near_outside=%.1f not_eligible=%.1f processed=%.1f track_geometry=%.1f} | "
        "decode{fast=%.1f%% fallback=%.1f(%.1f%%)} | submit{draws=%.1f avg_batch=%.2f session_closes=%.1f gl_calls=%.0f} | scene_us_per_received=%.1f scene_us_per_processed=%.1f | "
        "state_flow_per_frame{state_calls=%.1f superseded_before_visible=%.1f call_groups=%.1f groups_with_only_rejected_triangles=%.1f(calls=%.1f)}",
        label, w.frames, fps, (double)w.frameUs / f / 1000.0, (double)w.sceneUs / f / 1000.0, (double)w.compUs / f / 1000.0, (double)w.swapUs / f / 1000.0, (double)w.uploadUs / f / 1000.0, (double)w.overlayUs / f / 1000.0, (double)w.presentUs / f / 1000.0,
        (double)w.received / f, (double)w.rejected_total() / f, 100.0 * (double)w.rejected_total() / rcv, (double)w.rejected[0] / f, (double)w.rejected[1] / f, (double)w.rejected[2] / f, (double)w.rejected[3] / f,
        (double)w.nearOutside / f, (double)w.notEligible / f, (double)w.processed / f, (double)w.trackTris / f,
        100.0 * (double)w.decodeFast / proc, (double)w.decodeSlow / f, 100.0 * (double)w.decodeSlow / proc, (double)w.draws / f, (double)w.sessionTris / dr, (double)w.sessionCloses / f, (double)w.glCalls / f,
        w.received ? (double)w.sceneUs / (double)w.received : 0.0, w.processed ? (double)w.sceneUs / (double)w.processed : 0.0,
        (double)w.flow.stateCalls / f, (double)w.flow.superseded / f, (double)w.flow.groups / f, (double)w.flow.groupsRejectedOnly / f, (double)w.flow.callsBeforeRejectedOnly / f);
    if (len > 0 && (size_t)len + 200 < n && w.framesAnalysed)
        len += snprintf(out + len, n - (size_t)len, " | batch_closes_that_could_merge(sampled %llu frames){closed_sessions=%llu same_key_as_previous=%llu(%.1f%%)}", (unsigned long long)w.framesAnalysed, (unsigned long long)w.closesAnalysed, (unsigned long long)w.closesSameKey,
                        w.closesAnalysed ? 100.0 * (double)w.closesSameKey / (double)w.closesAnalysed : 0.0);
    if (full && len > 0 && (size_t)len + 160 < n)
        len += snprintf(out + len, n - (size_t)len, " | FULL_verifiers{reject_checked=%llu reject_bad=%llu decode_checked=%llu decode_bad=%llu}", (unsigned long long)w.verified, (unsigned long long)w.verifyBad, (unsigned long long)w.decodeChecked, (unsigned long long)w.decodeBad);
    if (len > 0 && (size_t)len + 100 < n && track && track[0]) len += snprintf(out + len, n - (size_t)len, " | %s", track);
    return len;
}

} }   // namespace ign::g145
