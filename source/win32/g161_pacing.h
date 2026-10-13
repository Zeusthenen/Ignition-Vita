// win32/g161_pacing.h -- iteration-G161 (v1.0 RC1), PURE (no Vita API, no log, no heap, no global state; the clock and the sleep are the caller's: tools/g161_test.cpp drives this code with a simulated clock).
//
// WHAT IS PACED (audit, tmp/G151_PACING_AUDIT.txt + tmp/G161_PACING_AUDIT.txt): the game's loop produces ONE grBufferSwap per frame; its simulation is driven by the wall clock (delta = real time of the frame, in 25 ms units, clamped at 300 ms;
// the music / effects run on the port's own output threads), so the number of swaps per second is the PRESENTATION rate, not the speed of the game. Holding the swaps to 30 per second therefore changes how often a picture is produced and
// nothing else. The pacer waits ONCE per frame, right before the present (vglSwapBuffers), for the time that is left of the frame period.
//
// ALGORITHM (absolute grid, no catch-up):
//   target = grid + period                      (the grid is the last release time, or the last target when the frame was early)
//   now <  target -> sleep (target - now); grid = target        (early: only the remaining time is waited; oversleeping does not shift the grid, so it is not accumulated)
//   now >= target -> no wait; grid = now                         (late: present at once and continue from the real time: no debt, no compensation by faster frames)
// A clock step larger than two periods (suspend, a debugger) is treated as a restart of the grid.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ign { namespace g161 {

constexpr uint32_t kDefaultFps = 30;
constexpr uint32_t kMinFps = 10, kMaxFps = 60;
constexpr uint32_t kBinUs = 500, kBins = 512;           // interval histogram: 0.5 ms bins up to 256 ms (the last bin collects the rest)
constexpr uint32_t kLateToleranceUs = 1000;             // a frame is counted "late" when it reaches the pacer more than 1 ms after its target
constexpr uint32_t kStallUs = 250000;                   // an interval above this is a stall (loading, a pause of the whole process), counted apart

enum Source : uint8_t { kFromDefault = 0, kFromFile, kRefused };
struct Config {
    uint32_t fps = kDefaultFps;                         // 0 = unlimited (diagnostic)
    uint32_t targetUs = 1000000u / kDefaultFps;         // 33333
    Source source = kFromDefault;
    char refused[24] = {0};
    bool enabled() const { return fps != 0; }
};

// control-file token: fps-limit=30 (default), fps-limit=0 | off (unlimited, for comparison), fps-limit=N with 10 <= N <= 60. Anything else is refused (reported) and the default stays.
inline Config parse_fps_limit(const char* text) {
    Config c;
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
        const char* e = p; while (*e && *e != ' ' && *e != '\n' && *e != '\r' && *e != '\t') ++e;
        const size_t n = (size_t)(e - p);
        if (n >= 10 && memcmp(p, "fps-limit=", 10) == 0) {
            char v[24] = {0}; const size_t vn = n - 10 < sizeof v - 1 ? n - 10 : sizeof v - 1; memcpy(v, p + 10, vn);
            if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "OFF") || !strcmp(v, "unlimited")) { c.fps = 0; c.targetUs = 0; c.source = kFromFile; return c; }
            char* end = nullptr; const long x = strtol(v, &end, 10);
            if (end && *end == 0 && end != v && x >= (long)kMinFps && x <= (long)kMaxFps) { c.fps = (uint32_t)x; c.targetUs = 1000000u / (uint32_t)x; c.source = kFromFile; return c; }
            c.source = kRefused; memcpy(c.refused, v, vn < sizeof c.refused - 1 ? vn : sizeof c.refused - 1); c.refused[sizeof c.refused - 1] = 0; return c;   // refused: the default (30) stays
        }
        p = e;
    }
    return c;
}

struct Stats {
    uint64_t frames = 0, lateFrames = 0, stalls = 0, sleptFrames = 0, sleepUs = 0, totalUs = 0, maxUs = 0;
    uint32_t hist[kBins] = {};
};
struct Pacer {
    Config cfg; Stats st;
    bool have = false, firstDone = false; uint64_t grid = 0, lastRelease = 0, firstUs = 0;
    // called with the time at which the frame reaches the present; returns how long to sleep (0 = present now)
    uint64_t plan(uint64_t now) {
        if (!have) { have = true; grid = now; lastRelease = now; firstUs = now; return 0; }
        if (!cfg.enabled()) return 0;
        const uint64_t period = cfg.targetUs;
        if (now > grid + 2 * period + kStallUs) { grid = now; return 0; }       // a jump of the clock / a long stall: restart the grid, nothing to wait for
        const uint64_t target = grid + period;
        if (now >= target) { if (now > target + kLateToleranceUs) ++st.lateFrames; grid = now; return 0; }
        grid = target; return target - now;
    }
    // called with the time at which the frame is released to the present (after the sleep, or at once); slept = the time really spent sleeping
    void released(uint64_t rel, uint64_t slept) {
        if (rel < lastRelease) { lastRelease = rel; return; }
        const uint64_t iv = rel - lastRelease; lastRelease = rel;
        if (!firstDone) { firstDone = true; ++st.frames; return; }              // the very first frame has no interval
        ++st.frames; st.totalUs += iv; if (iv > st.maxUs) st.maxUs = iv;
        if (iv >= kStallUs) ++st.stalls;
        const uint64_t b = iv / kBinUs; ++st.hist[b >= kBins ? kBins - 1 : b];
        if (slept) { ++st.sleptFrames; st.sleepUs += slept; }
    }
    // percentile (0..100) of the recorded intervals in milliseconds (bin upper edge, 0.5 ms resolution)
    double percentile_ms(double pc) const {
        uint64_t n = 0; for (uint32_t i = 0; i < kBins; ++i) n += st.hist[i];
        if (!n) return 0.0;
        const uint64_t need = (uint64_t)((pc / 100.0) * (double)n + 0.999999); uint64_t acc = 0;
        for (uint32_t i = 0; i < kBins; ++i) { acc += st.hist[i]; if (acc >= (need ? need : 1)) return (double)(i + 1) * (double)kBinUs / 1000.0; }
        return (double)kBins * (double)kBinUs / 1000.0;
    }
    double avg_fps() const { return st.totalUs ? (double)(st.frames > 0 ? st.frames - 1 : 0) * 1e6 / (double)st.totalUs : 0.0; }
};

inline int format_summary(char* buf, size_t n, const Pacer& p) {
    return std::snprintf(buf, n, "frames=%llu avg_fps=%.2f p50_ms=%.1f p90_ms=%.1f p95_ms=%.1f late_frames=%llu sleep_ms=%llu slept_frames=%llu max_ms=%.0f stalls_250ms=%llu",
                         (unsigned long long)p.st.frames, p.avg_fps(), p.percentile_ms(50), p.percentile_ms(90), p.percentile_ms(95), (unsigned long long)p.st.lateFrames,
                         (unsigned long long)(p.st.sleepUs / 1000u), (unsigned long long)p.st.sleptFrames, (double)p.st.maxUs / 1000.0, (unsigned long long)p.st.stalls);
}

} }   // namespace ign::g161
