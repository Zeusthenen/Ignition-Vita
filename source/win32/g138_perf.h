// win32/g138_perf.h -- G138 frame-window telemetry: fixed accumulators + a fixed histogram, no heap, no per-frame vector, no timers of its own.
// Pure (no GL, no guest access, no logging) so that tools/g138_test.cpp runs exactly the code the console runs.
//
// One FrameSample is added per swap (once per frame!). Everything that feeds it is a cumulative counter that already exists or is a plain integer
// increment on a path that was already executed (see glide2x_ign.cpp, "iteration-G138"). Frames are split into RACE / PAUSE / MENU: a race window is
// closed every kWindowFrames RACE frames and summarised with ONE line; pause frames are counted apart and never enter the race statistics.
//
// Time split (coarse, per frame, all from timestamps taken once per frame or from counters the compositor already kept):
//   frame      = swap entry to swap entry
//   scene      = first triangle of the frame to the swap entry (guest rendering code + the Glide translation, interleaved: it cannot be split without per-call timers,
//                which this build deliberately does not have; the FULL build adds the old per-triangle timer as `tri_draw`)
//   compositor / upload / overlay / present = the host stages the port owns (cumulative counters of the LFB compositor and the swap)
//   logic      = frame - scene - (compositor+upload+overlay+present): guest game logic and everything not timed (JIT, input, audio callbacks, waits)
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g138 {

constexpr int kWindowFrames = 120;
constexpr int kBins = 64;              // 4 ms bins covering 0..252 ms; the last bin collects every slower frame
constexpr uint32_t kBinUs = 4000;
constexpr int kHotspotEveryWindows = 5;
constexpr int kFinalEveryWindows = 10;

enum Kind : int { kMenu = 0, kRace = 1, kPause = 2 };

struct FrameSample {
    uint64_t frameUs = 0;
    Kind kind = kMenu;
    uint32_t tris = 0, draws = 0, texChanges = 0, stateCalls = 0, stateRedundant = 0, lfbLocks = 0, lfbUnlocks = 0;
    uint64_t texBytes = 0, compBytes = 0;
    uint64_t sceneUs = 0, compUs = 0, uploadUs = 0, overlayUs = 0, presentUs = 0, triDrawUs = 0;   // triDrawUs: FULL build only (0 in LIGHT)
    uint64_t firstTriUs = 0;   // duration of the whole first triangle of the frame (a GPU/scene-start wait shows up here, once per frame)
};

class Hist {
public:
    void clear() { std::memset(bin_, 0, sizeof bin_); n_ = 0; }
    void add(uint64_t us) { uint64_t b = us / kBinUs; if (b >= (uint64_t)kBins) b = kBins - 1; ++bin_[b]; ++n_; }
    uint32_t count() const { return n_; }
    // approximate percentile: the middle of the bin that holds the pct-th frame (error <= kBinUs/2)
    uint32_t percentile_us(int pct) const {
        if (!n_) return 0;
        uint32_t target = (uint32_t)(((uint64_t)n_ * (uint64_t)pct + 99u) / 100u); if (target < 1) target = 1;
        uint32_t cum = 0;
        for (int i = 0; i < kBins; ++i) { cum += bin_[i]; if (cum >= target) return (uint32_t)i * kBinUs + kBinUs / 2; }
        return (uint32_t)(kBins - 1) * kBinUs + kBinUs / 2;
    }
    uint32_t bin(int i) const { return bin_[i]; }
private:
    uint32_t bin_[kBins] = {};
    uint32_t n_ = 0;
};

struct Window {
    uint32_t frames = 0;
    uint64_t totalUs = 0, minUs = ~0ull, maxUs = 0;
    Hist hist;
    uint64_t tris = 0, draws = 0, texChanges = 0, stateCalls = 0, stateRedundant = 0, lfbLocks = 0, lfbUnlocks = 0, texBytes = 0, compBytes = 0;
    uint64_t sceneUs = 0, compUs = 0, uploadUs = 0, overlayUs = 0, presentUs = 0, triDrawUs = 0, firstTriUs = 0;
    void clear() { *this = Window{}; }
    void add(const FrameSample& s) {
        ++frames; totalUs += s.frameUs; if (s.frameUs < minUs) minUs = s.frameUs; if (s.frameUs > maxUs) maxUs = s.frameUs; hist.add(s.frameUs);
        tris += s.tris; draws += s.draws; texChanges += s.texChanges; stateCalls += s.stateCalls; stateRedundant += s.stateRedundant; lfbLocks += s.lfbLocks; lfbUnlocks += s.lfbUnlocks;
        texBytes += s.texBytes; compBytes += s.compBytes;
        sceneUs += s.sceneUs; compUs += s.compUs; uploadUs += s.uploadUs; overlayUs += s.overlayUs; presentUs += s.presentUs; triDrawUs += s.triDrawUs; firstTriUs += s.firstTriUs;
    }
    double avg_ms() const { return frames ? (double)totalUs / frames / 1000.0 : 0.0; }
    double fps() const { return totalUs ? 1e6 * (double)frames / (double)totalUs : 0.0; }
    double per_frame(uint64_t v) const { return frames ? (double)v / frames : 0.0; }
    uint64_t host_known_us() const { return compUs + uploadUs + overlayUs + presentUs; }
    // guest logic + untimed: never negative (the stages are measured by independent counters)
    uint64_t logic_us() const { const uint64_t k = sceneUs + host_known_us(); return totalUs > k ? totalUs - k : 0; }
};

// ---- ONE summary line per window -------------------------------------------------------------------------------------------------------------
inline int format_window(char* out, size_t n, const char* phase, int windowIdx, const Window& w, uint32_t pauseFramesExcluded, bool full) {
    const double f = w.frames ? (double)w.frames : 1.0;
    int len = snprintf(out, n,
        "phase=%s win=%d frames=%u pause_frames_excluded=%u | fps=%.2f avg_ms=%.2f min_ms=%.1f max_ms=%.1f p50_ms=%.0f p90_ms=%.0f p95_ms=%.0f p99_ms=%.0f (4ms bins, +-2ms) | "
        "per_frame: tris=%.0f draws=%.0f tex_changes=%.1f state_calls=%.1f state_redundant=%.1f lfb_locks=%.1f lfb_unlocks=%.1f tex_upload_kB=%.1f compositor_upload_kB=%.1f | "
        "ms_per_frame: logic_untimed=%.2f scene=%.2f (of which the first triangle=%.2f) compositor=%.2f tex_upload=%.2f overlay=%.2f present=%.2f",
        phase, windowIdx, w.frames, pauseFramesExcluded, w.fps(), w.avg_ms(), w.frames ? (double)w.minUs / 1000.0 : 0.0, (double)w.maxUs / 1000.0,
        w.hist.percentile_us(50) / 1000.0, w.hist.percentile_us(90) / 1000.0, w.hist.percentile_us(95) / 1000.0, w.hist.percentile_us(99) / 1000.0,
        (double)w.tris / f, (double)w.draws / f, (double)w.texChanges / f, (double)w.stateCalls / f, (double)w.stateRedundant / f, (double)w.lfbLocks / f, (double)w.lfbUnlocks / f,
        (double)w.texBytes / f / 1024.0, (double)w.compBytes / f / 1024.0,
        (double)w.logic_us() / f / 1000.0, (double)w.sceneUs / f / 1000.0, (double)w.firstTriUs / f / 1000.0, (double)w.compUs / f / 1000.0, (double)w.uploadUs / f / 1000.0, (double)w.overlayUs / f / 1000.0,
        (double)w.presentUs / f / 1000.0);
    if (full && len > 0 && (size_t)len < n) len += snprintf(out + len, n - (size_t)len, " tri_draw=%.2f(FULL: per-triangle timer)", (double)w.triDrawUs / f / 1000.0);
    return len;
}
// hotspots of a (cumulative) window: where the average frame goes, and what each FPS step needs
inline int format_hotspots(char* out, size_t n, const Window& w, bool full) {
    const double avg = w.avg_ms(); const double f = w.frames ? (double)w.frames : 1.0;
    auto pct = [&](uint64_t us) { return w.totalUs ? 100.0 * (double)us / (double)w.totalUs : 0.0; };
    const double t15 = 1000.0 / 15.0, t20 = 1000.0 / 20.0, t30 = 1000.0 / 30.0;
    auto need = [&](double target) { return avg > target ? avg - target : 0.0; };
    auto needPct = [&](double target) { return avg > 0.0 && avg > target ? 100.0 * (avg - target) / avg : 0.0; };
    int len = snprintf(out, n,
        "frames=%u avg_ms=%.2f fps=%.2f | share_of_frame: logic_untimed=%.1f%% scene(guest render code + Glide translation)=%.1f%% compositor=%.1f%% tex_upload=%.1f%% overlay=%.1f%% present=%.1f%% | "
        "scene_us_per_triangle=%.1f tris_per_frame=%.0f first_triangle_of_frame=%.1f%% of the frame (a GPU/scene-start wait would show here) | to_reach: 15fps=%.1fms(-%.0f%%) 20fps=%.1fms(-%.0f%%) 30fps=%.1fms(-%.0f%%) of the average frame | "
        "host_stages_ms(compositor+upload+overlay+present)=%.2f of which present=%.2f",
        w.frames, avg, w.fps(), pct(w.logic_us()), pct(w.sceneUs), pct(w.compUs), pct(w.uploadUs), pct(w.overlayUs), pct(w.presentUs),
        w.tris ? (double)w.sceneUs / (double)w.tris : 0.0, (double)w.tris / f, pct(w.firstTriUs), need(t15), needPct(t15), need(t20), needPct(t20), need(t30), needPct(t30),
        (double)w.host_known_us() / f / 1000.0, (double)w.presentUs / f / 1000.0);
    if (full && len > 0 && (size_t)len < n) len += snprintf(out + len, n - (size_t)len, " | tri_draw_ms_per_frame(FULL build: per-triangle timer)=%.2f", (double)w.triDrawUs / f / 1000.0);
    return len;
}

// ---- frame classification (G139 fix of the G138 emission) -------------------------------------------------------------------------------------------
// G138 classified a frame as RACE only when g_g74TrackLoadActive was false. That flag is set when the first structural track file is opened and NEVER cleared anywhere in the project, so after the
// first track load every frame was a MENU frame: the [G138-PERF-BASELINE] (menu) windows filled their small cap and [G138-PERF-RACE]/[HOTSPOTS]/[FINAL] were never produced.
// The race latch (first real race swap, never cleared) and the car carousel are the only reliable signals; a pause is the guest's structural condition.
inline Kind classify_frame(bool raceLatch, bool carCarouselActive, bool pauseCondition) {
    if (!raceLatch || carCarouselActive) return kMenu;
    return pauseCondition ? kPause : kRace;
}

// ---- the telemetry state machine (pure): feed it one sample per swap, it tells what to print -------------------------------------------------------
enum : uint32_t { kEvNone = 0, kEvRaceWindow = 1, kEvMenuWindow = 2, kEvHotspots = 4, kEvFinal = 8 };

class Telemetry {
public:
    // returns the events produced by this frame
    uint32_t add(const FrameSample& s) {
        uint32_t ev = kEvNone;
        if (s.kind == kPause) { if (inRace_) { ++pauseFrames_; ++pauseFramesWin_; pauseUs_ += s.frameUs; if (s.frameUs > pauseMaxUs_) pauseMaxUs_ = s.frameUs; } return ev; }
        if (s.kind == kRace) {
            if (!inRace_) {                                                    // a new race: its own statistics (the previous race was reported when it ended)
                inRace_ = true; total_.clear(); win_.clear(); raceWindows_ = 0; pauseFrames_ = pauseFramesWin_ = 0; pauseUs_ = pauseMaxUs_ = 0; menuWin_.clear();
            }
            win_.add(s); total_.add(s);
            if (win_.frames >= (uint32_t)kWindowFrames) {
                ++raceWindows_; ev |= kEvRaceWindow; lastWin_ = win_; lastPauseWin_ = pauseFramesWin_;
                if ((raceWindows_ % kHotspotEveryWindows) == 0) ev |= kEvHotspots;
                if ((raceWindows_ % kFinalEveryWindows) == 0) ev |= kEvFinal;
                win_.clear(); pauseFramesWin_ = 0;
            }
        } else {
            if (inRace_) { inRace_ = false; ev |= kEvFinal | kEvHotspots; raceEnded_ = true; win_.clear(); }   // the race ended: final summary from the cumulative totals
            menuWin_.add(s);
            if (menuWin_.frames >= (uint32_t)kWindowFrames) { ++menuWindows_; ev |= kEvMenuWindow; lastWin_ = menuWin_; menuWin_.clear(); }
        }
        return ev;
    }
    bool in_race() const { return inRace_; }
    bool race_ended() const { return raceEnded_; }
    const Window& last_window() const { return lastWin_; }          // the window of the last kEvRaceWindow / kEvMenuWindow
    uint32_t last_window_pause_frames() const { return lastPauseWin_; }
    const Window& race_total() const { return total_; }
    const Window& race_partial() const { return win_; }
    int race_windows() const { return raceWindows_; }
    int menu_windows() const { return menuWindows_; }
    uint32_t pause_frames() const { return pauseFrames_; }
    uint64_t pause_total_us() const { return pauseUs_; }
    uint64_t pause_max_us() const { return pauseMaxUs_; }
private:
    Window win_, total_, menuWin_, lastWin_;
    int raceWindows_ = 0, menuWindows_ = 0;
    uint32_t pauseFrames_ = 0, pauseFramesWin_ = 0, lastPauseWin_ = 0;
    uint64_t pauseUs_ = 0, pauseMaxUs_ = 0;
    bool inRace_ = false, raceEnded_ = false;
};

// ---- the one G138 optimisation: do not issue a GL state call whose requested value equals the current one ---------------------------------------------
// guDrawTriangleWithClip used to run, for EVERY triangle: glEnable/glDisable(DEPTH_TEST), glEnable/glDisable(CULL_FACE), glEnable/glDisable(TEXTURE_2D), glBindTexture(tri texture) on entry and the
// mirror image (restore of what glIsEnabled/glGetIntegerv had returned) on exit, plus a restore of GL_BLEND that this function never changes. Enabling something already enabled (or binding the
// texture already bound) cannot change GL state, so the plan below issues a call only when the value really changes, and restores only what it changed. The planner is pure so that
// tools/g138_test.cpp executes the same code against a model of GL state and proves: identical state at the moment of the draw and identical final state, for every combination.
// `skipRedundant == false` reproduces the G137 sequence exactly (FULL builds).
enum Toggle : uint8_t { kToggleNone = 0, kToggleEnable = 1, kToggleDisable = 2 };
struct GlWas  { bool depth, cull, tex2d, blend; uint32_t boundTex; };      // what glIsEnabled / glGetIntegerv returned at the start of the triangle
struct GlWant { bool depth, cull, texReady; uint32_t texId; };              // what this triangle needs
struct GlPlan {
    Toggle depthIn = kToggleNone, depthOut = kToggleNone, cullIn = kToggleNone, cullOut = kToggleNone, texIn = kToggleNone, texOut = kToggleNone, blendOut = kToggleNone;
    bool bindIn = false, bindOut = false;                                  // bindIn: glBindTexture(texId); bindOut: glBindTexture(boundTex)
};
inline GlPlan plan_gl_state(const GlWas& w, const GlWant& n, bool skipRedundant) {
    GlPlan p;
    auto pair = [&](bool was, bool want, Toggle& in, Toggle& out) {
        if (!skipRedundant) { in = want ? kToggleEnable : kToggleDisable; out = was ? kToggleEnable : kToggleDisable; return; }
        in  = (was == want) ? kToggleNone : (want ? kToggleEnable : kToggleDisable);
        out = (was == want) ? kToggleNone : (was ? kToggleEnable : kToggleDisable);
    };
    pair(w.depth, n.depth, p.depthIn, p.depthOut);
    pair(w.cull, n.cull, p.cullIn, p.cullOut);
    pair(w.tex2d, n.texReady, p.texIn, p.texOut);
    p.blendOut = skipRedundant ? kToggleNone : (w.blend ? kToggleEnable : kToggleDisable);   // blending is never changed between the query and this restore
    if (!skipRedundant) { p.bindIn = n.texReady; p.bindOut = true; }
    else { p.bindIn = n.texReady && n.texId != w.boundTex; p.bindOut = p.bindIn; }          // the binding is restored only when it was changed
    return p;
}

}}   // namespace ign::g138
