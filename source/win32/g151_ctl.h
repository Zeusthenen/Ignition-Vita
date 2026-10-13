// win32/g151_ctl.h -- iteration-G151, PURE (no GL, no Vita API, no guest access, no logging, no heap, no global state; proven by tools/g151_test.cpp).
//  1. the native HUD hooks are ON by default; the control file (ux0:data/ignition/g146_geo.txt) can switch each one off (or on again): the exact tokens hook-hud-draw=off|on and hook-hud-sprite=off|on, the last one wins;
//  2. which race windows write their lines (release telemetry): the first window of a race, every fourth one, and every window below 25 FPS; the totals of a race (the FINAL lines) are always written;
//  3. the one-line session summary: hooks installed, native calls, fallbacks by cause, errors, frame time.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g150_ctl.h"
#include "win32/g150_hud.h"

namespace ign { namespace g151 {

// ---- 1. the hooks ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kHooksDefault = g150::kHookDrawSurface | g150::kHookSprite;   // both proven in the G150 real run (hooks=0x3): 119..6,492 draw calls and 3.9 k..584 k sprite calls per race, no visual difference
struct HookChoice { uint32_t mask = kHooksDefault; bool fromFile = false; };
inline bool tok(const char* p, size_t n, const char* lit) { return std::strlen(lit) == n && std::memcmp(p, lit, n) == 0; }
inline HookChoice hooks_from_text(const char* text) {
    HookChoice h;
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        const size_t n = (size_t)(e - p);
        if (n) {
            if (tok(p, n, "hook-hud-draw=off")) { h.mask &= ~g150::kHookDrawSurface; h.fromFile = true; }
            else if (tok(p, n, "hook-hud-draw=on")) { h.mask |= g150::kHookDrawSurface; h.fromFile = true; }
            else if (tok(p, n, "hook-hud-sprite=off")) { h.mask &= ~g150::kHookSprite; h.fromFile = true; }
            else if (tok(p, n, "hook-hud-sprite=on")) { h.mask |= g150::kHookSprite; h.fromFile = true; }
        }
        p = e;
    }
    return h;
}

// ---- 2. the release window lines -------------------------------------------------------------------------------------------------------------------------------------------------------------
// windowNo counts from 1 inside a race. A slow window is always written: the tail is what the next iteration works on.
inline bool window_wanted(uint32_t windowNo, double fps) { return windowNo <= 1u || (windowNo % 4u) == 1u || fps < 25.0; }

// ---- 3. the summary ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct SummaryIn {
    uint32_t hooksInstalled = 0, hooksMask = kHooksDefault; bool fromFile = false;
    uint64_t palCalls = 0, palNative = 0, palWhy[g150::kWhyCount] = {};
    uint64_t sprCalls = 0, sprNative = 0, sprOtherPath = 0, sprBpp8 = 0, sprWhy[g150::kWhyCount] = {};
    uint64_t glErrors = 0, ringErrors = 0, shadowChecks = 0, shadowMismatches = 0, scanMismatches = 0, budgetResumes = 0;
    uint32_t races = 0; uint64_t frames = 0; double fps = 0, avgMs = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, maxMs = 0;
    bool final = false;
};
inline size_t why_list(char* out, size_t cap, const uint64_t* w) {
    size_t p = 0; out[0] = 0; bool any = false;
    for (uint32_t i = 1; i < g150::kWhyCount; ++i) if (w[i] && p + 64 < cap) { p += (size_t)std::snprintf(out + p, cap - p, "%s%s=%llu", any ? " " : "", g150::why_name(i), (unsigned long long)w[i]); any = true; }
    if (!any) std::snprintf(out, cap, "none");
    return std::strlen(out);
}
inline int format_summary(char* out, size_t cap, const SummaryIn& s) {
    if (!out || !cap) return 0;
    char pw[320], sw[320]; why_list(pw, sizeof pw, s.palWhy); why_list(sw, sizeof sw, s.sprWhy);
    const int n = std::snprintf(out, cap,
        "%s hooks{installed=0x%x wanted=0x%x (%s) drawsurface_palette{calls=%llu native=%llu fallback=%llu by_cause: %s} sprite_blit{calls=%llu native=%llu fallback=%llu by_cause: %s; original_path_not_covered{other_path=%llu bpp8_surface=%llu}}} "
        "errors{gl=%llu ring=%llu shadow_mismatches=%llu/%llu scan_mismatches=%llu budget_resumes=%llu} races=%u frames=%llu | frame_ms{fps=%.2f avg=%.2f p50=%.0f p90=%.0f p95=%.0f p99=%.0f max=%.0f}",
        s.final ? "SESSION-FINAL" : "SESSION", s.hooksInstalled, s.hooksMask, s.fromFile ? "control file" : "default", (unsigned long long)s.palCalls, (unsigned long long)s.palNative, (unsigned long long)(s.palCalls - s.palNative), pw,
        (unsigned long long)s.sprCalls, (unsigned long long)s.sprNative, (unsigned long long)(s.sprCalls - s.sprNative - s.sprOtherPath - s.sprBpp8), sw, (unsigned long long)s.sprOtherPath, (unsigned long long)s.sprBpp8,
        (unsigned long long)s.glErrors, (unsigned long long)s.ringErrors, (unsigned long long)s.shadowMismatches, (unsigned long long)s.shadowChecks, (unsigned long long)s.scanMismatches, (unsigned long long)s.budgetResumes,
        s.races, (unsigned long long)s.frames, s.fps, s.avgMs, s.p50, s.p90, s.p95, s.p99, s.maxMs);
    if (n < 0) { out[0] = 0; return 0; }
    if ((size_t)n >= cap) { out[cap - 1] = 0; return (int)(cap - 1); }
    return n;
}

} }   // namespace ign::g151
