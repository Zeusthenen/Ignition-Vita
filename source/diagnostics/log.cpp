// diagnostics/log.cpp -- G134 logger: G132's open/append/close-per-line write path, behind a short static allowlist.
// No static constructors (everything here is zero-initialised POD / lock-free atomics), no dynamic memory, no mutex, no writer thread,
// no buffering, nothing runs before main(). See log.h for the rules; the rule tables below ARE the allowlist.
#include "diagnostics/log.h"

// wx86_now_ms()/wx86_now_us(): WinVita's host-monotonic clock (runtime/host_clock.h, already in libwinx86_vita.a).
#include "runtime/host_clock.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#ifdef __vita__
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#endif

// Base directory of the data tree. Overridden only by the host tests (they cannot write ux0:).
#ifndef IGN_LOG_BASE
#define IGN_LOG_BASE "ux0:data/ignition/"
#endif

namespace ign {

namespace {

// ---- literal paths (nothing is composed at run time except the "previous" names, which are literals too) ----
const char* const kDirNew       = IGN_LOG_BASE "logs";
const char* const kProbe        = IGN_LOG_BASE "logs/write.probe";
const char* const kBootNew      = IGN_LOG_BASE "logs/boot.txt";
const char* const kBootPrev     = IGN_LOG_BASE "logs/boot.previous.txt";
const char* const kMainNew      = IGN_LOG_BASE "logs/ignitionvita.log";
const char* const kMainPrev     = IGN_LOG_BASE "logs/ignitionvita.previous.log";
const char* const kCritNew      = IGN_LOG_BASE "logs/ignition_critical.log";
const char* const kCritPrev     = IGN_LOG_BASE "logs/ignition_critical.previous.log";
const char* const kSessNew      = IGN_LOG_BASE "logs/ignition_session_id.txt";
// legacy (G132) locations: read-only for the session counter, written only in fallback mode
const char* const kMainOld      = IGN_LOG_BASE "ignitionvita.log";
const char* const kMainOldPrev  = IGN_LOG_BASE "ignitionvita.previous.log";
const char* const kCritOld      = IGN_LOG_BASE "ignition_critical.log";
const char* const kCritOldPrev  = IGN_LOG_BASE "ignition_critical.previous.log";
const char* const kSessOld      = IGN_LOG_BASE "ignition_session_id.txt";

// ---- state: zero-initialised statics only ----
bool g_prepared = false;
bool g_fallback = false;
int g_mkdirRc = 0;
unsigned g_rotOk = 0, g_rotFail = 0, g_rotSkipEmpty = 0;     // bit 0 boot, 1 main, 2 critical
bool g_ownerSet = false;
#ifdef __vita__
int g_owner = 0;
#else
uintptr_t g_owner = 0;
#endif
uint32_t g_sessionId = 0;
bool g_sessionLoaded = false;
volatile uint64_t g_perfTotalUs = 0, g_perfMaxUs = 0, g_perfLines = 0;
uint32_t g_mainLines = 0, g_critLines = 0;
std::atomic<uint32_t> g_cpSeq;
std::atomic<uint32_t> g_suppressed;

const char* mainPath()     { return g_fallback ? kMainOld : kMainNew; }
const char* critPath()     { return g_fallback ? kCritOld : kCritNew; }
const char* sessPath()     { return g_fallback ? kSessOld : kSessNew; }

bool is_owner() {
    if (!g_ownerSet) return true;
#ifdef __vita__
    return sceKernelGetThreadId() == g_owner;
#else
    return (uintptr_t)pthread_self() == g_owner;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------
// ALLOWLIST. First matching rule wins; no match = dropped before formatting. cap = max lines for the tag in this run (-1 none).
// `any`: only messages containing one of the '|'-separated words are kept. `crit`: the line is also written to ignition_critical.log.
// Everything not listed here (including the BUILD-G* history, G104/G112/G115 periodic summaries, input routing, per-file lines,
// MCI queries, audio feed/sink summaries) is dropped.
// ---------------------------------------------------------------------------------------------------------------------------
enum { kExact = 0, kPrefix = 1, kSubstr = 2 };
struct Rule { const char* m; unsigned char kind; short cap; const char* any; bool crit; };

const Rule kMain[] = {
    // errors / halts / faults (also copied to the critical log)
    { "ERROR",  kSubstr, -1, nullptr, true },
    { "ABORT",  kSubstr, -1, nullptr, true },
    { "FAULT",  kSubstr, -1, nullptr, true },
    { "FATAL",  kSubstr, -1, nullptr, true },
    { "[STOP]", kExact,  -1, nullptr, true },
    { "[GLIDE-G20]", kExact, -1, "UNIMPLEMENTED|halt|HALT", true },
    // G134 tags and boot phases (stages)
    { "[G134",  kPrefix, -1, nullptr, false },
    { "[PHASE]", kExact, 12, nullptr, true },
    { "[BOOT]",  kExact, 4,  nullptr, false },
    { "[G35-SESSION]", kExact, 1, nullptr, false },
    // frozen-module build/state lines that identify the binary and the audio set-up
    { "[G132]",     kExact, 4, nullptr, false },
    { "[G131-VIS]", kExact, 2, nullptr, false },
    { "[G131-KEY]", kExact, 2, nullptr, false },
    { "[G131-AB]",  kExact, 4, nullptr, false },
    { "[G132-MCI]", kExact, 4, nullptr, false },
    // [G132-MUSIC] is split in three rules (same tag; the first rule whose message filter AND cap accept the line wins), so a burst of
    // plays can never starve the failure / lifecycle lines:
    { "[G132-MUSIC]", kExact, 12, "FAILED|REFUSED|failed|refused|unavailable|error", true },
    { "[G132-MUSIC]", kExact, 6,  "backend ready|closed|thread placement|port open", true },
    { "[G132-MUSIC]", kExact, 8,  "play cdaudio|further plays", true },
    { "[G131-DS]",  kExact, 6, nullptr, false },
    // milestones the frozen modules already emit once (capped); copied to the critical log = the stage markers
    { "[G103-BOOT-VISIBILITY]", kExact, 4, nullptr, true },      // language selector reached, intro, first LFB group, first swap
    { "[G128-LANG]",   kExact, 4, nullptr, true },               // language selector enter / exit
    { "[G125-CARSEL]", kExact, 8, nullptr, true },   // G163: the first two entries of the car selection (enter, state checks, exit with the replay totals); the old row below is never reached while this one has room
    { "[G125-CARSEL]", kExact, 3, nullptr, true },               // car selection enter / exit
    { "[G43-3D]",      kExact, 2, nullptr, true },               // first 3D triangle (front-end 3D reached)
    { "[G74-TRACK-LOAD-BEGIN]", kExact, 2, nullptr, true },      // circuit confirmed, loading begins
    { "[G74-TRACK-FILE]", kExact, 8, nullptr, false },           // the first distinct level files opened while loading
    { "[G74-LOAD-HEARTBEAT]", kExact, 8, nullptr, true },        // every 5 s while loading
    { "[G74-NEXT-BLOCKER]", kExact, 2, nullptr, true },
    { "[G80-USA-RACE-RUNTIME]", kExact, 1, nullptr, true },      // track geometry complete
    { "[G74-FIRST-RACE-SWAP]", kExact, 1, nullptr, true },
    { "[G76-FIRST-REAL-RACE-SWAP]", kExact, 1, nullptr, true },  // first swap of the race
    { "[G111-FRAMETIME]", kExact, 8, nullptr, true },            // phase / fps windows (phase=MENU/LOADING/RACE): proves the race is running
    { "[G108-BAD-TRIANGLE-SUMMARY]", kExact, 2, nullptr, false },
    // iteration-G135/G136: the pause probe's tags (below) and the two already-existing summaries the frozen renderer prints
    // 2 s after each ESC press during a race (triangle/texture/palette/LFB-dirty counters of the pause window)
    // iteration-G136 replaces the G135 tags: per REAL pause one ENTER, one EXIT, one RESTORE and one +60-swap RACE-GFX-CHECK line, plus the one
    // "visual detection ignored" line and a handful of invalid-triangle correlation lines
    { "[G136-PAUSE-REAL-ENTER]", kExact, 4, nullptr, true },
    { "[G136-PAUSE-REAL-EXIT]", kExact, 4, nullptr, true },
    { "[G136-PAUSE-RESTORE]", kExact, 4, nullptr, true },
    { "[G136-RACE-GFX-CHECK]", kExact, 6, nullptr, true },
    { "[G136-BAD-TRI-CORRELATION]", kExact, 5, nullptr, true },
    // iteration-G137: track-geometry integrity. Bounded by construction: the first capture is ONE header + 3 vertices + the 16-entry ring, once; at most
    // 4 burst begin/end pairs; a cumulative summary at most every 300 swaps. Never one line per triangle. (No tag here contains ERROR/ABORT/FAULT/FATAL,
    // which would bypass the caps and flood the critical log.) Only the burst start is mirrored to the critical log.
    { "[G137-TRACK-BURST-BEGIN]", kExact, 4, nullptr, true },
    { "[G137-TRACK-VERTEX-CAPTURE]", kExact, 24, nullptr, false },
    { "[G137-TRACK-BURST-END]", kExact, 4, nullptr, false },
    { "[G137-TRACK-GEOMETRY-SUMMARY]", kExact, 30, nullptr, false },
    // iteration-G138: frame-window telemetry. One line per 120 race frames (race ~15 min fits), hotspots every 5 windows, a final summary at the end of the race and every 10 windows,
    // and the non-race baseline windows. Bounded; none of these tags contains ERROR/ABORT/FAULT/FATAL.
    { "[G138-PERF-BASELINE]", kExact, 8, nullptr, false },
    { "[G138-PERF-RACE]", kExact, 80, nullptr, false },
    { "[G138-PERF-HOTSPOTS]", kExact, 20, nullptr, false },
    { "[G138-PERF-FINAL]", kExact, 12, nullptr, true },
    // iteration-G139: submission summaries -- one SUBMIT line per 120 race frames, batch / shadow summaries every 5 windows, a final line (also in the critical log). Never per batch or per triangle.
    { "[G139-SUBMIT-RACE]", kExact, 80, nullptr, false },
    { "[G139-BATCH-SUMMARY]", kExact, 20, nullptr, false },
    { "[G139-STATE-SHADOW]", kExact, 20, nullptr, false },
    { "[G139-PERF-FINAL]", kExact, 12, nullptr, true },
    // iteration-G140: language persistence -- one line per transition (selector entry / exit, a stale btz value, a restoration, the tree check): 28 lines at most in the whole run. Never per frame.
    { "[G140-LANG-NODE-ENTER]", kExact, 6, nullptr, false },
    { "[G140-LANG-CONFIRMED]", kExact, 5, nullptr, true },
    { "[G140-LANG-RESET-DETECTED]", kExact, 4, nullptr, true },
    { "[G140-LANG-RESTORED]", kExact, 8, nullptr, false },
    { "[G140-LANG-MENU-CHECK]", kExact, 5, nullptr, false },
    // iteration-G141: first-race HUD comparison -- at most kRaceMaxLines (20) lines per race over the four tags (race 1: FIRST-RACE, race 2: REFERENCE, later races: one REFERENCE line each). Never per frame / triangle.
    { "[G141-HUD-FIRST-RACE]", kExact, 12, nullptr, false },
    { "[G141-HUD-REFERENCE]", kExact, 24, nullptr, false },
    { "[G141-HUD-STATE-DIFF]", kExact, 24, nullptr, false },
    { "[G141-HUD-FIX-ACTIVE]", kExact, 8, nullptr, true },
    // iteration-G142: dynamic HUD / overlay stability -- one line per transition of the stable visual class, per tile spike, per real pause (the user's marker) and per race entry; at most 38 lines per race. Never per frame / triangle.
    { "[G142-VISUAL-STATE-CHANGE]", kExact, 60, nullptr, false },
    { "[G142-HUD-SOURCE-CHECK]", kExact, 16, nullptr, false },
    { "[G142-HUD-OUTPUT-CHECK]", kExact, 12, nullptr, false },
    { "[G142-OVERLAY-TILE-SPIKE]", kExact, 20, nullptr, false },
    { "[G142-BLUR-CAUSE]", kExact, 24, nullptr, true },
    { "[G142-TRACK-ID]", kExact, 16, nullptr, true },
    // iteration-G143: HUD region lifetime -- mode changes (race / pause / other) with their boundary action, the region map (first frames of a race + each real pause), the first anomaly per region and kind, the BOTTOM_RIGHT scrub and the key-vs-palette check.
    // At most ~35 lines per race over all G142 + G143 tags. Never per frame / triangle. ([G142-HUD-SOURCE-CHECK] / [G142-HUD-OUTPUT-CHECK] / [G142-BLUR-CAUSE] are no longer written: the G142 read-back verdict was an instrument artefact.)
    { "[G143-MODE-CHANGE]", kExact, 48, nullptr, false },
    { "[G143-REGION-MAP]", kExact, 16, nullptr, false },
    { "[G143-REGION-ANOMALY]", kExact, 32, nullptr, true },
    { "[G143-REGION-SCRUB]", kExact, 12, nullptr, true },
    { "[G143-GUARD-SELFCHECK]", kExact, 16, nullptr, true },
    { "[G143-KEY-PALETTE]", kExact, 12, nullptr, true },
    // iteration-G144: incremental compositor fast path -- one compositor line per 120 race frames, a cumulative line at the end of a race, one line if the fast path is ever switched off. G145 preparation: the windowed scene-profile
    // lines (the profile every window, the two tables every 3rd window), a cumulative line per race. Never per frame / triangle / Glide call.
    { "[G144-FASTPATH-RACE]", kExact, 200, nullptr, false },   // iteration-G145: 80 -> 200 (the windows now follow the lifecycle of every race of the process)
    { "[G144-FASTPATH-FINAL]", kExact, 24, nullptr, true },
    { "[G144-FASTPATH-DISABLED]", kExact, 6, nullptr, true },
    { "[G145-SCENE-PROFILE]", kExact, 80, nullptr, false },
    { "[G145-API-COST]", kExact, 30, nullptr, false },
    { "[G145-BOUNDARY-CAUSES]", kExact, 30, nullptr, false },
    { "[G145-SCENE-FINAL]", kExact, 12, nullptr, true },
    // iteration-G145: the race lifecycle (a handful of event lines per race + one summary per race, critical), one geometry / compositor line per 120 race frames, a cumulative line per race, one line if a FULL verifier ever
    // switches a geometry fast path off. Never per frame / triangle / Glide call.
    { "[G145-RACE-LIFECYCLE]", kExact, 140, nullptr, false },
    { "[G145-RACE-SUMMARY]", kExact, 60, nullptr, true },
    { "[G145-GEOMETRY-RACE]", kExact, 160, nullptr, false },
    { "[G145-GEOMETRY-FINAL]", kExact, 24, nullptr, true },
    { "[G145-GEOMETRY-DISABLED]", kExact, 8, nullptr, true },
    // iteration-G146: the geometry window (fps, percentiles, triangles received / eligible / rejected, decode tiers, state flow), the eligibility reasons per window, the totals of a race (+ correlations), the one-off detail of the first
    // undecided triangle of a race, the control file notice, and the FULL verifiers' disable line. Never per frame / triangle / Glide call.
    { "[G146-GEOMETRY-RACE]", kExact, 200, nullptr, false },
    { "[G146-ELIGIBILITY]", kExact, 200, nullptr, false },
    { "[G146-GEOMETRY-FINAL]", kExact, 24, nullptr, true },
    { "[G146-ELIGIBILITY-DETAIL]", kExact, 12, nullptr, true },
    { "[G146-GEO-CONTROL]", kExact, 4, nullptr, true },
    { "[G146-GEOMETRY-DISABLED]", kExact, 8, nullptr, true },
    // iteration-G148: deferred state packet. One line per 120 race frames each (families, GL writers + batch key, timing split), one total per race, the calibration once, the control notice, the outlier lines of a race (first 8) and the FULL verifier's
    // family-disable line. Never per frame / triangle / Glide call.
    { "[G148-FAMILIES-RACE]", kExact, 200, nullptr, false },
    { "[G148-DEFER-RACE]", kExact, 200, nullptr, false },
    { "[G148-TIMING-RACE]", kExact, 200, nullptr, false },
    { "[G148-FAMILIES-FINAL]", kExact, 24, nullptr, true },
    { "[G148-DEFER-FINAL]", kExact, 24, nullptr, true },
    { "[G148-CALIBRATION]", kExact, 2, nullptr, true },
    { "[G148-CONTROL]", kExact, 4, nullptr, true },
    { "[G148-OUTLIER]", kExact, 40, nullptr, true },
    { "[G148-FAMILY-DISABLED]", kExact, 8, nullptr, true },
    // iteration-G149: cost profiler of the guest->host boundary. One line per 120 race frames each (gap pairs by call site, triangle segments, crossing census, swap stages, optimisations), one total per race, the calibration once,
    // the control notice and the vector-scan fallback line. Never per frame / triangle / Glide call.
    { "[G149-GAP-RACE]", kExact, 300, nullptr, false },
    { "[G149-TRI-RACE]", kExact, 300, nullptr, false },
    { "[G149-CENSUS-RACE]", kExact, 300, nullptr, false },
    { "[G149-SWAP-RACE]", kExact, 300, nullptr, false },
    { "[G149-OPT-RACE]", kExact, 300, nullptr, false },
    { "[G149-GAP-FINAL]", kExact, 24, nullptr, true },
    { "[G149-TRI-FINAL]", kExact, 24, nullptr, true },
    { "[G149-CENSUS-FINAL]", kExact, 24, nullptr, true },
    { "[G149-SWAP-FINAL]", kExact, 24, nullptr, true },
    { "[G149-OPT-FINAL]", kExact, 24, nullptr, true },
    { "[G149-CALIBRATION]", kExact, 2, nullptr, true },
    { "[G149-CONTROL]", kExact, 4, nullptr, true },
    { "[G149-SCAN-DISABLED]", kExact, 4, nullptr, true },
    { "[G149-DYN]", kExact, 4, nullptr, true },
    // iteration-G150: the resume of the guest run when its block budget ends, the memory-guard decision, the measured cost of a shim-side guest read, the control tokens, the guest hot-loop replacements
    { "[G150-BUDGET]", kExact, 24, nullptr, true },
    { "[G150-MEMGUARD]", kExact, 2, nullptr, true },
    { "[G150-CALIBRATION]", kExact, 2, nullptr, true },
    { "[G150-CONTROL]", kExact, 4, nullptr, true },
    { "[G150-HOOK]", kExact, 24, nullptr, true },
    { "[G150-HOOK-RACE]", kExact, 300, nullptr, false },
    { "[G150-HOOK-FINAL]", kExact, 24, nullptr, true },
    { "[G151-CONTROL]", kExact, 4, nullptr, true },   // G151
    { "[G151-SESSION]", kExact, 40, nullptr, true },   // G151
    { "[G151-SESSION-FINAL]", kExact, 2, nullptr, true },   // G151
    { "[G152-CHAMP-RACE]", kExact, 40, nullptr, true },   // G152
    { "[G152-CHAMP-RACE-END]", kExact, 40, nullptr, true },   // G152
    { "[G152-CHAMP-REJECT]", kExact, 12, nullptr, true },   // G152
    { "[G152-CHAMP-SESSION-FINAL]", kExact, 2, nullptr, true },   // G152
    { "[G153-SMOKE-PROBE]", kExact, 30, nullptr, true },   // G153
    { "[G153-SMOKE-SUMMARY]", kExact, 40, nullptr, true },   // G153
    { "[G154-KEY-SUMMARY]", kExact, 40, nullptr, true },   // G154
    { "[G154-KEY-NOTICE]", kExact, 8, nullptr, true },   // G154
    { "[G155-KEYED-SUMMARY]", kExact, 40, nullptr, true },   // G155
    { "[G155-KEYED-SESSION]", kExact, 40, nullptr, true },   // G155
    { "[G155-KEYED-NOTICE]", kExact, 8, nullptr, true },   // G155
    { "[G156-CULL-SUMMARY]", kExact, 40, nullptr, true },   // G156
    { "[G156-CULL-NOTICE]", kExact, 12, nullptr, true },   // G156
    { "[G157-SPLITSCREEN-BLOCKED]", kExact, 200, nullptr, true },   // G157
    { "[G157-SPLITSCREEN-NOTICE]", kExact, 12, nullptr, true },   // G157
    { "[G158-ONLINE-BLOCKED]", kExact, 200, nullptr, true },   // G158
    { "[G158-ONLINE-NOTICE]", kExact, 12, nullptr, true },   // G158
    { "[G159-NAME-IME-NOTICE]", kExact, 12, nullptr, true },   // G159
    { "[G159-NAME-IME-OPEN]", kExact, 200, nullptr, true },   // G159
    { "[G159-NAME-IME-COMMIT]", kExact, 200, nullptr, true },   // G159
    { "[G159-NAME-IME-CANCEL]", kExact, 200, nullptr, true },   // G159
    { "[G159-NAME-IME-ERROR]", kExact, 60, nullptr, true },   // G159
    { "[G160-CLOCKS]", kExact, 4, nullptr, true },   // G160
    { "[G160-CLOCKS-RESTORE]", kExact, 4, nullptr, true },   // G160
    { "[RELEASE-FPS-LIMITER]", kExact, 2, nullptr, true },   // G161
    { "[RELEASE-FPS-SUMMARY]", kExact, 2, nullptr, true },   // G161
    { "[G163-VERSION]", kExact, 2, nullptr, false },   // G163: the Championship unlocks test build; the lines below are written at race boundaries, at the car selection and on a refused draw: bounded, main log only (no write per event to the critical log)
    { "[G163-STATE]", kExact, 40, nullptr, false },   // G163
    { "[G163-CARS]", kExact, 8, nullptr, false },   // G163
    { "[G163-GUARD]", kExact, 3, nullptr, false },   // G163
    { "[G163-SESSION-FINAL]", kExact, 2, nullptr, true },   // G163
    { "[G164-END]", kExact, 40, nullptr, false },   // G164: the end of a single race (button presses, the keys sent, the flow words at the end and start of a race); bounded, main log only
    { "[G78-PAUSE-RENDER-SUMMARY]", kExact, 3, nullptr, false },
    { "[G79-LFB-DIRTY-REGION]", kExact, 3, nullptr, false },
    // exit path
    { "[G117-EXIT]", kExact, 6, nullptr, false },
    { "[EXIT-",      kPrefix, 8, nullptr, false },
};
constexpr int kMainN = (int)(sizeof kMain / sizeof kMain[0]);

const Rule kCrit[] = {
    { "ERROR", kSubstr, -1, nullptr, false },
    { "ABORT", kSubstr, -1, nullptr, false },
    { "FAULT", kSubstr, -1, nullptr, false },
    { "FATAL", kSubstr, -1, nullptr, false },
    { "[STOP]", kExact, -1, nullptr, false },
    { "[G134", kPrefix, -1, nullptr, false },
    { "[G35-SESSION]", kExact, -1, nullptr, false },
    { "[G35-TRACK-SELECTED]", kExact, -1, nullptr, false },
    { "[G35-RACE-ENTER]", kExact, -1, nullptr, false },
    { "[G35-EXIT", kPrefix, -1, nullptr, false },
    { "[G117-EXIT]", kExact, 10, nullptr, false },
    { "[G35-LANG]", kExact, 6, nullptr, false },
    { "[G35-FILE]", kExact, -1, "FAILED", false },               // essential-file failures only, never the per-file OK lines
};
constexpr int kCritN = (int)(sizeof kCrit / sizeof kCrit[0]);

std::atomic<int> g_mainCnt[kMainN];
std::atomic<int> g_critCnt[kCritN];

bool match(const Rule& r, const char* tag) {
    switch (r.kind) {
        case kExact:  return strcmp(tag, r.m) == 0;
        case kPrefix: return strncmp(tag, r.m, strlen(r.m)) == 0;
        default:      return strstr(tag, r.m) != nullptr;
    }
}
int find_rule(const Rule* t, int n, const char* tag) {
    if (!tag || tag[0] != '[') {                       // untagged: only the substring (ERROR...) rules can match
        for (int i = 0; i < n; ++i) if (t[i].kind == kSubstr && tag && match(t[i], tag)) return i;
        return -1;
    }
    for (int i = 0; i < n; ++i) if (match(t[i], tag)) return i;
    return -1;
}
bool contains_any(const char* msg, const char* list) {
    const char* p = list;
    // (a plain substring search per '|'-separated word)
    while (*p) {
        const char* bar = strchr(p, '|');
        const size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n && n < 48) { char k[48]; memcpy(k, p, n); k[n] = 0; if (strstr(msg, k)) return true; }
        if (!bar) break;
        p = bar + 1;
    }
    return false;
}
// cap check; atomic so that any thread may ask
bool take_slot(const Rule& r, std::atomic<int>& cnt) {
    if (r.cap < 0) return true;
    return cnt.fetch_add(1, std::memory_order_relaxed) < r.cap;
}
// The first rule that matches the tag, whose message filter accepts the message and whose cap still has room. -1 = drop.
// A rule whose filter rejects the message lets the next rule of the same tag decide; a rule whose cap is spent ends the search.
int select_rule(const Rule* t, int n, std::atomic<int>* cnt, const char* tag, const char* msg) {
    for (int i = 0; i < n; ++i) {
        if (!match(t[i], tag)) continue;
        if (t[i].any && !contains_any(msg, t[i].any)) continue;
        return take_slot(t[i], cnt[i]) ? i : -1;
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Ring for lines produced by threads other than the owner (lock-free, static, 16 slots). Producers never touch the disk.
// ---------------------------------------------------------------------------------------------------------------------------
enum { kToMain = 1, kToCrit = 2 };
struct Pending {
    std::atomic<uint32_t> state;      // 0 free, 1 being filled, 2 ready
    uint32_t seq;
    uint32_t ms;
    unsigned char target;
    char tag[40];
    char msg[400];
};
constexpr int kRing = 16;
Pending g_ring[kRing];
std::atomic<uint32_t> g_ringSeq;
std::atomic<int> g_ringReady;
std::atomic<uint32_t> g_ringDrops, g_ringQueued;

void enqueue(int target, const char* tag, const char* msg) {
    const uint32_t start = g_ringSeq.load(std::memory_order_relaxed);
    for (int i = 0; i < kRing; ++i) {
        Pending& p = g_ring[(start + (uint32_t)i) % kRing];
        uint32_t expect = 0;
        if (!p.state.compare_exchange_strong(expect, 1u, std::memory_order_acquire)) continue;
        p.target = (unsigned char)target;
        p.ms = (uint32_t)wx86_now_ms();
        snprintf(p.tag, sizeof p.tag, "%s", tag);
        snprintf(p.msg, sizeof p.msg, "%s", msg);
        p.seq = g_ringSeq.fetch_add(1, std::memory_order_relaxed);
        g_ringReady.fetch_add(1, std::memory_order_relaxed);
        p.state.store(2u, std::memory_order_release);
        g_ringQueued.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ringDrops.fetch_add(1, std::memory_order_relaxed);        // never wait
}

// ---------------------------------------------------------------------------------------------------------------------------
// Direct writers: owner thread only. fopen(append) + fprintf + fclose, exactly G132's write path.
// ---------------------------------------------------------------------------------------------------------------------------
uint32_t load_session_id() {
    if (!g_sessionLoaded) {
        g_sessionLoaded = true;
        uint32_t prev = 0; bool have = false;
        if (FILE* rf = fopen(sessPath(), "r")) { unsigned v = 0; if (fscanf(rf, "%u", &v) == 1) { prev = v; have = true; } fclose(rf); }
        if (!have && !g_fallback) {                                     // continue the numbering of the G132/G133 era counter (read-only)
            if (FILE* rf = fopen(kSessOld, "r")) { unsigned v = 0; if (fscanf(rf, "%u", &v) == 1) prev = v; fclose(rf); }
        }
        g_sessionId = prev + 1;
        if (FILE* wf = fopen(sessPath(), "w")) { fprintf(wf, "%u\n", g_sessionId); fclose(wf); }
    }
    return g_sessionId;
}

// G150-BEGIN
// iteration-G150: LOG BATCHES. On the memory card one fopen(append)+fclose costs 11-20 ms whatever the line length (measured in the G149 real run: consecutive lines of one window are 11-20 ms apart), so the 13 lines of a
// window stalled one frame for 200-400 ms (max_ms 150-450 in every window; 1.7-3.3 ms per frame on average). Between log_batch_begin() and log_batch_end() the lines of the OWNER thread are formatted exactly as before
// into two static buffers and written with ONE fopen/fwrite/fclose per file when the batch ends (or earlier when a buffer is full). Same bytes, same order, same files; the only difference is WHEN they reach the card
// (at the end of the batch: microseconds after the last line, so a crash cannot lose more than the lines of the batch in progress). Zero-initialised statics only.
constexpr size_t kBatchMainCap = 28u * 1024u, kBatchCritCap = 8u * 1024u;
char g_batchMain[kBatchMainCap], g_batchCrit[kBatchCritCap];
size_t g_batchMainN = 0, g_batchCritN = 0;
uint32_t g_batchMainLines = 0, g_batchCritLines = 0;
int g_batchDepth = 0;
uint32_t g_batchFlushes = 0, g_batchLinesTotal = 0;
volatile uint64_t g_batchFlushUs = 0, g_batchFlushMaxUs = 0;

void flush_main_batch() {
    if (!g_batchMainN) return;
    if (FILE* f = fopen(mainPath(), "a")) { fwrite(g_batchMain, 1, g_batchMainN, f); fclose(f); g_mainLines += g_batchMainLines; }
    g_batchMainN = 0; g_batchMainLines = 0;
}
void flush_crit_batch() {
    if (!g_batchCritN) return;
    if (FILE* f = fopen(critPath(), "a")) { fwrite(g_batchCrit, 1, g_batchCritN, f); fclose(f); g_critLines += g_batchCritLines; }   // NEVER "w": append-only
    g_batchCritN = 0; g_batchCritLines = 0;
}

// G150-END
void write_main(const char* tag, const char* msg, uint32_t ms) {
// G150-BEGIN
    if (g_batchDepth > 0) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int n = snprintf(g_batchMain + g_batchMainN, kBatchMainCap - g_batchMainN, "[%6u.%03u] %-10s %s\n", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u), tag, msg);
            if (n > 0 && (size_t)n < kBatchMainCap - g_batchMainN) { g_batchMainN += (size_t)n; ++g_batchMainLines; ++g_batchLinesTotal; return; }
            flush_main_batch();   // the buffer was full: write what it holds, then the line goes into the empty buffer
        }
        return;                    // a line larger than the whole buffer cannot happen (msg is at most 2,047 characters)
    }
// G150-END
    if (FILE* f = fopen(mainPath(), "a")) {
        fprintf(f, "[%6u.%03u] %-10s %s\n", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u), tag, msg);
        fclose(f);
        ++g_mainLines;
    }
}
void write_crit(const char* tag, const char* msg, uint32_t ms) {
    const uint32_t sid = load_session_id();
// G150-BEGIN
    if (g_batchDepth > 0) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int n = snprintf(g_batchCrit + g_batchCritN, kBatchCritCap - g_batchCritN, "[session=%u][%6u.%03u] %-10s %s\n", (unsigned)sid, (unsigned)(ms / 1000u), (unsigned)(ms % 1000u), tag, msg);
            if (n > 0 && (size_t)n < kBatchCritCap - g_batchCritN) { g_batchCritN += (size_t)n; ++g_batchCritLines; ++g_batchLinesTotal; return; }
            flush_crit_batch();
        }
        return;
    }
// G150-END
    if (FILE* f = fopen(critPath(), "a")) {                             // NEVER "w": append-only
        fprintf(f, "[session=%u][%6u.%03u] %-10s %s\n", (unsigned)sid, (unsigned)(ms / 1000u), (unsigned)(ms % 1000u), tag, msg);
        fclose(f);
        ++g_critLines;
    }
}
void write_direct(int target, const char* tag, const char* msg, uint32_t ms) {
    const uint64_t t0 = wx86_now_us();
    if (target & kToMain) write_main(tag, msg, ms);
    if (target & kToCrit) write_crit(tag, msg, ms);
    const uint64_t dt = wx86_now_us() - t0;
    g_perfTotalUs = g_perfTotalUs + dt;
    if (dt > g_perfMaxUs) g_perfMaxUs = dt;
    g_perfLines = g_perfLines + 1;
}

void drain_ring() {
    if (g_ringReady.load(std::memory_order_acquire) <= 0) return;
    for (;;) {
        int best = -1; uint32_t bestSeq = 0;
        for (int i = 0; i < kRing; ++i) {
            if (g_ring[i].state.load(std::memory_order_acquire) != 2u) continue;
            if (best < 0 || (int32_t)(g_ring[i].seq - bestSeq) < 0) { best = i; bestSeq = g_ring[i].seq; }
        }
        if (best < 0) break;
        Pending& p = g_ring[best];
        write_direct(p.target, p.tag, p.msg, p.ms);
        p.state.store(0u, std::memory_order_release);
        g_ringReady.fetch_sub(1, std::memory_order_relaxed);
    }
}

void emit(int target, const char* tag, const char* msg) {
    if (!is_owner()) { enqueue(target, tag, msg); return; }
    drain_ring();
    write_direct(target, tag, msg, (uint32_t)wx86_now_ms());
// G150-BEGIN
    // iteration-G150: inside a batch, a line that goes to the critical log (errors, halts, checkpoints) reaches the card at once, like before; the cumulative FINAL lines of a window wait for the end of the batch
    if (g_batchDepth > 0 && (target & kToCrit) && !strstr(tag, "FINAL")) { flush_main_batch(); flush_crit_batch(); }
// G150-END
}

// ---------------------------------------------------------------------------------------------------------------------------
// Folder creation + rotation
// ---------------------------------------------------------------------------------------------------------------------------
int make_dir(const char* path) {
#ifdef __vita__
    const int rc = sceIoMkdir(path, 0777);
    return (rc >= 0 || (uint32_t)rc == 0x80010011u /* SCE_ERROR_ERRNO_EEXIST: already there is fine */) ? 0 : rc;
#else
#ifdef _WIN32
    const int rc = _mkdir(path);
#else
    const int rc = mkdir(path, 0777);
#endif
    if (rc == 0) return 0;
    struct stat st;
    return (errno == EEXIST && stat(path, &st) == 0 && (st.st_mode & S_IFDIR)) ? 0 : -1;
#endif
}
long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    fclose(f);
    return n;
}
// current -> previous. An empty current file is left alone (it must not replace a previous generation that holds evidence);
// if the rename fails the CURRENT file is never touched or removed -- the new session simply appends after it.
void rotate_one(const char* cur, const char* prev, unsigned bit) {
    const long sz = file_size(cur);
    if (sz < 0) return;                         // nothing to rotate
    if (sz == 0) { g_rotSkipEmpty |= bit; return; }
    remove(prev);                               // the older generation goes (rename cannot replace it on this filesystem)
    if (rename(cur, prev) == 0) g_rotOk |= bit; else g_rotFail |= bit;
}

} // namespace

bool logs_prepare() {
    if (g_prepared) return !g_fallback;
    g_prepared = true;
#ifdef __vita__
    g_owner = sceKernelGetThreadId();
#else
    g_owner = (uintptr_t)pthread_self();
#endif
    g_ownerSet = true;
    g_mkdirRc = make_dir(kDirNew);
    bool ok = (g_mkdirRc == 0);
    if (ok) {                                   // the folder must really be writable
        FILE* f = fopen(kProbe, "wb");
        if (f) { fputc('x', f); fclose(f); remove(kProbe); } else ok = false;
    }
    if (!ok) g_fallback = true;
    if (!g_fallback) rotate_one(kBootNew, kBootPrev, 1u);
    rotate_one(mainPath(), g_fallback ? kMainOldPrev : kMainPrev, 2u);
    rotate_one(critPath(), g_fallback ? kCritOldPrev : kCritPrev, 4u);
    return !g_fallback;
}

void logs_describe(char* out, size_t n) {
    snprintf(out, n, "dir=%s mkdir_rc=%d mode=%s rotated(boot=%d main=%d critical=%d) rotate_failed_mask=0x%x skipped_empty_mask=0x%x",
             kDirNew, g_mkdirRc, g_fallback ? "FALLBACK_LEGACY_PATHS" : "logs/",
             (g_rotOk & 1u) ? 1 : 0, (g_rotOk & 2u) ? 1 : 0, (g_rotOk & 4u) ? 1 : 0, g_rotFail, g_rotSkipEmpty);
}

void log_init() { logs_prepare(); }

// G151-BEGIN
// iteration-G151: release windows. The lines of a race window (tags ending in -RACE] and [G146-ELIGIBILITY]) are written only for the windows the release rule selects (win32/g151_ctl.h window_wanted): the first one, every fourth and every slow one.
// The filter is set by the caller around the lines of ONE window, so nothing else is ever dropped by it; the totals of the race (FINAL lines) and every other tag are untouched.
bool g_windowFilter = false; uint32_t g_windowDropped = 0;
bool is_window_tag(const char* tag) {
    const size_t n = std::strlen(tag);
    if (n >= 7 && std::strcmp(tag + n - 6, "-RACE]") == 0) return true;
    return std::strcmp(tag, "[G146-ELIGIBILITY]") == 0;
}
void log_set_window_filter(bool suppress) { g_windowFilter = suppress; }
uint32_t log_window_lines_dropped() { return g_windowDropped; }
// G151-END
void log_line(const char* tag, const char* fmt, ...) {
    // Policy first: a dropped tag costs a few string compares and nothing else.
    if (g_windowFilter && is_window_tag(tag)) { ++g_windowDropped; return; }   // G151
    if (find_rule(kMain, kMainN, tag) < 0) { g_suppressed.fetch_add(1, std::memory_order_relaxed); return; }
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const int ri = select_rule(kMain, kMainN, g_mainCnt, tag, msg);
    if (ri < 0) { g_suppressed.fetch_add(1, std::memory_order_relaxed); return; }
    emit(kToMain | (kMain[ri].crit ? kToCrit : 0), tag, msg);
}

// iteration-G138: exactly the first test of log_line (a tag without any rule is dropped before formatting).
bool log_tag_enabled(const char* tag) { return find_rule(kMain, kMainN, tag) >= 0; }

uint32_t ignition_session_id() { return load_session_id(); }

void critical_log(const char* tag, const char* fmt, ...) {
    if (find_rule(kCrit, kCritN, tag) < 0) { g_suppressed.fetch_add(1, std::memory_order_relaxed); return; }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (select_rule(kCrit, kCritN, g_critCnt, tag, msg) < 0) { g_suppressed.fetch_add(1, std::memory_order_relaxed); return; }
    emit(kToCrit, tag, msg);
}

void checkpoint(const char* name, const char* fmt, ...) {
    char detail[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    char msg[400];
    const uint32_t n = g_cpSeq.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (detail[0]) snprintf(msg, sizeof msg, "#%u %s | %s", (unsigned)n, name, detail);
    else           snprintf(msg, sizeof msg, "#%u %s", (unsigned)n, name);
    emit(kToMain | kToCrit, "[G134-CP]", msg);
}

void log_drain_pending() { if (is_owner()) drain_ring(); }

// G150-BEGIN
// iteration-G150: see the comment above flush_main_batch(). Nestable; only the owner thread batches (any other thread already goes through the ring).
bool g_batchDisabled = false;   // zero-initialised: batches are ON until a control file says otherwise
void log_batch_set_enabled(bool on) { g_batchDisabled = !on; }
void log_batch_begin() { if (!g_batchDisabled && is_owner()) ++g_batchDepth; }
void log_batch_end() {
    if (!is_owner() || g_batchDepth <= 0) return;
    if (--g_batchDepth > 0) return;
    if (!g_batchMainN && !g_batchCritN) return;
    const uint64_t t0 = wx86_now_us();
    flush_main_batch(); flush_crit_batch();
    const uint64_t dt = wx86_now_us() - t0;
    g_batchFlushUs = g_batchFlushUs + dt; if (dt > g_batchFlushMaxUs) g_batchFlushMaxUs = dt; ++g_batchFlushes;
    g_perfTotalUs = g_perfTotalUs + dt; if (dt > g_perfMaxUs) g_perfMaxUs = dt;
}
void log_get_batch_stats(uint32_t* flushes, uint32_t* lines, uint64_t* totalUs, uint64_t* maxUs) {
    if (flushes) { *flushes = g_batchFlushes; }
    if (lines) { *lines = g_batchLinesTotal; }
    if (totalUs) { *totalUs = g_batchFlushUs; }
    if (maxUs) { *maxUs = g_batchFlushMaxUs; }
}

// G150-END
void log_get_stats(LogStats* out) {
    out->mainLines = g_mainLines; out->critLines = g_critLines;
    out->ringQueued = g_ringQueued.load(); out->ringDrops = g_ringDrops.load(); out->suppressed = g_suppressed.load();
}

void log_get_perf_stats(uint64_t* out_total_us, uint64_t* out_max_us, uint64_t* out_line_count) {
    if (out_total_us) *out_total_us = g_perfTotalUs;
    if (out_max_us) *out_max_us = g_perfMaxUs;
    if (out_line_count) *out_line_count = g_perfLines;
}

} // namespace ign
