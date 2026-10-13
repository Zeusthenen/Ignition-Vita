// win32/g136_pause_tracker.h -- the pure decision logic of the G136 pause probe (no GL, no guest access, no logging), so that the host test
// (tools/g136_test.cpp) runs exactly the code the console runs.
//
// A REAL pause is recognised ONLY from the guest's own structural condition (proved from Ign_3dfx.exe: the in-race frame function draws the pause UI
// when [0x0049BBC0]==1 and [0x005371A0]!=1, see tmp/G135_PAUSE_MENU_REPORT.txt section 2). A picture that merely looks like the menu (visible
// pixels in the menu rectangle) while that condition is false is PASSIVE TELEMETRY: it is counted and reported once, and it never opens an episode,
// never starts a measurement, never reads or writes any graphics state.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ign { namespace g136 {

constexpr int kMaxEpisodes = 4;
constexpr uint32_t kVisualThreshold = 500;   // visible menu-rectangle pixels that make a frame "look like the menu" (telemetry only)
constexpr int kSampleAt = 3;                 // swaps after a real entry: the single LFB + GL measurement
constexpr int kExitHysteresis = 2;           // consecutive swaps with the condition false that end a real pause
constexpr int kResidualAfterExit = 2;        // swaps after the exit: the single GL read that looks for a menu left behind
constexpr int kPostWindow = 60;              // swaps after the exit: the follow-up summary
constexpr uint32_t kScanEvery = 8;           // passive visual telemetry: one rectangle scan every 8th race swap ...
constexpr uint32_t kScanBudget = 600;        // ... at most this many scans, and none after the first ignored detection was reported

enum : uint32_t { kEvNone = 0, kEvRealEnter = 1, kEvSample = 2, kEvRealExit = 4, kEvResidual = 8, kEvPost = 16, kEvIgnoredVisual = 32 };

class Tracker {
public:
    static bool pause_condition(uint32_t raceState, uint32_t flag) { return raceState == 1u && flag != 1u; }

    // true when the caller should count the visible pixels of the menu rectangle on this swap (passive telemetry only)
    bool wants_pixel_scan(uint32_t swapIdx) const {
        return !inPause_ && ignoredReported_ == 0 && scans_ < kScanBudget && (swapIdx % kScanEvery) == 0u;
    }
    // nothing left to do: all episodes done and every follow-up delivered
    bool idle() const { return episodes_ >= kMaxEpisodes && !inPause_ && !residualPending_ && !postPending_; }

    uint32_t step(uint32_t swap, uint32_t raceState, uint32_t flag, bool pixelsScanned, uint32_t menuPixels) {
        uint32_t ev = kEvNone;
        const bool P = pause_condition(raceState, flag);
        if (!inPause_) {
            if (P && episodes_ < kMaxEpisodes) {
                inPause_ = true; ++episodes_; enterSwap_ = swap; since_ = 0; gone_ = 0;
                residualPending_ = false; postPending_ = false;            // a new pause supersedes the follow-ups of the previous one
                ev |= kEvRealEnter;
            } else if (!P && pixelsScanned) {
                ++scans_;
                if (menuPixels >= kVisualThreshold) { ++visualIgnored_; if (ignoredReported_ == 0) { ignoredReported_ = 1; ev |= kEvIgnoredVisual; } }
            }
            if (residualPending_ && --residualCountdown_ <= 0) { ev |= kEvResidual; residualPending_ = false; }
            if (postPending_ && ++postSwaps_ >= kPostWindow) { ev |= kEvPost; postPending_ = false; }
        } else {
            ++since_;
            if (since_ == kSampleAt) { ev |= kEvSample; }
            if (P) { gone_ = 0; }
            else if (++gone_ >= kExitHysteresis) {
                inPause_ = false; exitSwap_ = swap; ev |= kEvRealExit;
                residualPending_ = true; residualCountdown_ = kResidualAfterExit; postPending_ = true; postSwaps_ = 0;
            }
        }
        return ev;
    }

    bool in_pause() const { return inPause_; }
    int episodes() const { return episodes_; }
    int since_enter() const { return since_; }
    uint32_t enter_swap() const { return enterSwap_; }
    uint32_t exit_swap() const { return exitSwap_; }
    uint32_t visual_ignored() const { return visualIgnored_; }
    uint32_t scans() const { return scans_; }

private:
    bool inPause_ = false, residualPending_ = false, postPending_ = false;
    int episodes_ = 0, since_ = 0, gone_ = 0, residualCountdown_ = 0, postSwaps_ = 0;
    uint32_t enterSwap_ = 0, exitSwap_ = 0, scans_ = 0, visualIgnored_ = 0, ignoredReported_ = 0;
};

// The part of the graphics state that NO guest call and NO per-material path writes: the viewport, the framebuffer / program binding and the
// front-face mode. A real pause that leaves the renderer clean must give changed_fields = 0 for exactly these. (The guest legitimately changes blend,
// depth, cull, texture, scissor... on every material, so a before/after comparison of THOSE says nothing about a leak -- they are checked for
// coherence with the Glide-side record instead, see g136_blend_coherence in glide2x_ign.cpp.)
struct StableState { int vp[4]; int framebuffer; int program; int frontFace; };

inline int changed_fields(const StableState& a, const StableState& b, char* names, size_t n) {
    int c = 0; size_t pos = 0;
    if (n) { names[0] = 0; }
    auto note = [&](const char* nm, int x, int y) {
        ++c;
        if (n && pos + 1 < n) { const int w = snprintf(names + pos, n - pos, "%s:%d->%d ", nm, x, y); if (w > 0 && (size_t)w < n - pos) { pos += (size_t)w; } }
    };
    static const char* const kVp[4] = {"viewport0", "viewport1", "viewport2", "viewport3"};
    for (int i = 0; i < 4; ++i) { if (a.vp[i] != b.vp[i]) { note(kVp[i], a.vp[i], b.vp[i]); } }
    if (a.framebuffer != b.framebuffer) { note("framebuffer", a.framebuffer, b.framebuffer); }
    if (a.program != b.program) { note("program", a.program, b.program); }
    if (a.frontFace != b.frontFace) { note("front_face", a.frontFace, b.frontFace); }
    return c;
}

}} // namespace ign::g136
