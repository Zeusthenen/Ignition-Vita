#include "win32/kernel32_ign.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/pe_image.h"
#include "runtime/guest_files.h"
#include "runtime/guest_region.h"
#include "runtime/guest_scratch.h"
#include "runtime/host_clock.h"
#include "runtime/win32_shims_kernel32.h"
#include "runtime/win32_shims_memory.h"
#include "runtime/ignition_paths.h"
#include "runtime/ignition_boot.h"
#include "runtime/ignition_identity.h"
#include "runtime/guest_thread_ctx.h"
#include "win32/dinput.h"
#include "win32/glide2x_ign.h"
#include "win32/g140_lang.h"
#include "win32/g163_save.h"   // G163: the save file is written through a temporary file and replaced only when complete
#include "diagnostics/log.h"
#include "diagnostics/phase_state.h"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <dirent.h>

using namespace d2rt;

// iteration-G15, Parte G/H: same cross-TU plain-C-global counter pattern
// user32_window.cpp/winmm_ign.cpp already use -- read by
// win32/glide2x_ign.cpp's per-swap-interval activity summary.
extern "C" {
    volatile uint32_t g_ign_kernel32_call_count = 0;
    volatile uint32_t g_ign_createfilea_count = 0;
    volatile uint32_t g_ign_readfile_count = 0;
    volatile uint32_t g_g99QpcSerial = 0;
    volatile uint64_t g_g99QpcValue = 0;
    volatile uint32_t g_g99QpcCaller = 0;
}

// iteration-G74, Parte 21: read-only cross-TU view of win32/dinput.cpp's
// own true-global (not anonymous-namespace) DirectInput activity counter
// -- same one win32/glide2x_ign.cpp already externs for its own [GLIDE-
// G15] summary. Used only by [G74-LOAD-HEARTBEAT]'s
// input_getdata_since_last field.
extern volatile uint32_t g_ign_dinput_call_count;

// iteration-G49, Parte T: aggregate-only file I/O timing -- "no verbose
// per-read dump", just running totals wrapped around the two real host
// I/O calls (fopen/fread) this file already makes, logged periodically
// (same 5s-window pattern used elsewhere this iteration). Answers "is
// file I/O actually a meaningful fraction of the 119s stall, or a red
// herring" with real microsecond timing instead of a guess.
uint64_t g_g49FileOpenTotalUs = 0;
uint64_t g_g49FileReadTotalUs = 0;
uint64_t g_g49FileReadMaxUs = 0;
uint32_t g_g49FileReadMaxBytes = 0;
uint64_t g_g49FileIoWindowStartMs = 0;

// iteration-G74, Parte 11: same raw-timing pattern as the G49 counters
// just above, extended to seek/close so the [G49-FILEIO] summary line can
// report ALL FOUR raw host I/O operations (open/read/seek/close),
// separately from any diagnostic-logging time (g_g74FileDiagLogUs below).
uint64_t g_g49FileSeekTotalUs = 0;
uint64_t g_g49FileCloseTotalUs = 0;

// iteration-G74, Parte 8-11: wall-clock time spent specifically inside
// the VERBOSE file-diagnostic IGN_LOG blocks this iteration gates behind
// kVerboseFileIoDiagnostics (ignition_paths.h) -- measured whether the
// gate is on or off, so flipping it back to true for a future comparison
// run immediately shows real numbers here instead of an assumption.
uint64_t g_g74FileDiagLogUs = 0;

// iteration-G74, Parte 14/21: cross-TU state so win32/glide2x_ign.cpp's
// grBufferSwap_impl can tell "are we inside track-load right now" for
// [G74-FIRST-RACE-SWAP]/[G74-RACE-RUNTIME] without this file knowing
// anything about Glide/swap counts. True-global (declared before
// `namespace ign {` opens, like g_ign_createfilea_count above) so a plain
// `extern` from another TU resolves it, same pattern already proven
// working for g_g56TargetAllocBase/g_ign_dinput_call_count.
bool g_g74TrackLoadActive = false;
uint64_t g_g74TrackLoadBeginMs = 0;

// iteration-G78, Parte 30: defined in runtime/ignition_boot.cpp (same
// global-scope-before-namespace-ign pattern), set once at boot_run()
// entry -- read here only to compute this file's own [G78-STARTUP-TIMING]
// checkpoint's elapsed_ms.
extern uint64_t g_g78ProcessStartMs;

// iteration-G76, Parte 19: true-global (cross-TU, same pattern as
// g_g74TrackLoadActive) -- set once BOTH the selected track's .TRI and
// .SRF files have been seen (Part 19's own "strong evidence" pair),
// giving win32/glide2x_ign.cpp's [G76-FIRST-REAL-RACE-SWAP]/[G76-RACE-
// RUNTIME] a much stronger completion signal than "any structural file
// seen" alone.
bool g_g76TrackGeometryComplete = false;

// iteration-G113: set on EVERY structural track-file open (not one-shot like
// g_g74TrackLoadBeginLogged/g_g76TrackGeometryComplete, which only ever fire
// for the FIRST race of a session) and consumed (cleared) by
// win32/glide2x_ign.cpp's compositor when the next GL scene begins -- the
// one race-vs-menu discriminator that also works for a second race/track.
bool g_g113TrackLoadSeen = false;

// iteration-G84, Parte 31: moved out of the anonymous namespace below (it
// used to live right next to g_g76TrackTriSeen/g_g76TrackSrfSeen with
// internal-only linkage) to true-global, same before-`namespace ign {`
// pattern as g_g74TrackLoadActive above -- needed so win32/glide2x_ign.cpp
// can read it by plain `extern` for the new [G35-RACE-ENTER] critical-log
// hook. Behavior/assignment site unchanged, only its linkage.
std::string g_g76SelectedTrackName;      // e.g. "CANADA" -- derived, not hardcoded

// iteration-G142: g_g76SelectedTrackName above is set ONCE (first track of the process): later races keep it, which is why the G141 lines kept saying CANADA.
// Passive identification of the track that is LOADING (read by glide2x_ign.cpp for the [G142-TRACK-ID] lines and the performance windows). Updated at the first structural file of every load
// (g_g113TrackLoadSeen is cleared when the GL scene of the race starts), so every race has its own directory and first .TRI / .SRF file. Nothing reads these to decide anything.
char g_g142LoadDir[24] = {0};            // directory of the first structural file of the current load, upper case, e.g. "CANADA"
char g_g142LoadTri[112] = {0};           // tail of the host path of the first .TRI / .SRF file of that load
uint32_t g_g142LoadSeq = 0;              // number of loads seen

// iteration-G74, Parte 21: low-overhead track-load heartbeat state --
// updated from the handful of already-instrumented file shims below, read
// (and reset) only by the heartbeat's own 5s-max-cadence check.
uint32_t g_g74FileOpsSinceHeartbeat = 0;
uint32_t g_g74LastHeartbeatGuestEip = 0;
uint64_t g_g74LastHeartbeatMs = 0;
uint32_t g_g74LastHeartbeatSwapCount = 0;
uint32_t g_g74LastHeartbeatTriangleCount = 0;
uint32_t g_g74LastHeartbeatDinputCount = 0;

// iteration-G56, Parte 6-9: target-range heap lifetime tracking -- NOT a
// global allocator trace (that stays forbidden, Part 6/22 of this
// iteration's own request), just two fixed addresses this run's own
// hardware log named (the failing draw's object pointer and its +0x18
// field, G52-G55). Armed for the WHOLE run at negligible cost (one
// range check per HeapAlloc/HeapFree call, both already-hot host
// functions this project already instruments) -- no "first N" cap,
// Part 7's own explicit instruction. Read cross-TU by
// win32/glide2x_ign.cpp's failure branch to answer Part 9's
// [G56-OBJECT-HEAP] query ("is esi live, and under what allocation,
// AT THE MOMENT OF THE FAILING DRAW").
constexpr uint32_t kG56TargetAddrA = 0x01039DC0u;   // esi at the failing draw
constexpr uint32_t kG56TargetAddrB = 0x01039DD8u;   // esi+0x18, the failing field
uint32_t g_g56TargetAllocBase = 0;
uint32_t g_g56TargetAllocSize = 0;
uint32_t g_g56TargetAllocCallerEip = 0;
uint32_t g_g56TargetGeneration = 0;   // 0 = never observed allocated yet
bool g_g56TargetLive = false;

// iteration-G57, Parte 4/11: G56's own real hardware run identified the
// SPECIFIC allocation that matters -- generation 31, base=0x01039CA0,
// size=0x850 (2128 bytes), still live at the failing draw (ruling out
// use-after-free, G56's own definitive result, frozen -- Part 2). This
// narrows the field-change watch to fire ONLY once THAT exact allocation
// signature is seen, instead of the noisy full-run tracking G56 added
// (which is now reduced per Part 28/29, since generations 8-30 are
// understood plain ReadFile-scratch churn, all size==4096). Read
// cross-TU by win32/dinput.cpp's poll to know when to start comparing
// (Part 11: "arm ONLY after generation==31"), avoiding the G53-era noise
// of watching from process start.
constexpr uint32_t kG57Gen31Size = 0x850u;
bool g_g57Gen31Armed = false;
uint32_t g_g57Gen31FieldSnapshotAtAlloc = 0;

namespace ign {

namespace {

// iteration-G15, Parte H: bytes read per open handle SINCE THE LAST
// snapshot -- read and cleared only by
// kernel32_ign_g15_snapshot_file_activity() (declared in kernel32_ign.h,
// called once per grBufferSwap from glide2x_ign.cpp). Filenames are
// resolved at snapshot time via the engine's own wx86_file_paths(), not
// duplicated here.
std::map<uint32_t, size_t>& g_g15BytesReadPerHandle() {
    static std::map<uint32_t, size_t> m;
    return m;
}

// iteration-G24, Parte H: FindFirstFileA's own handle namespace -- an
// internal map keyed by a synthetic guest HANDLE, never a raw host DIR*/
// pointer exposed to the guest (same reasoning as wx86_files()'s handles,
// just a separate counter range so it can never alias a real file handle
// or the 0x8600_/0x8700_ fake sync/GDI handles other subsystems use).
struct FindEntry {
    std::string name;
    uint64_t size = 0;
    bool isDir = false;
};
struct FindHandleState {
    std::vector<FindEntry> entries;
    size_t index = 0;   // next entry a future FindNextFileA would deliver
    std::string guestPattern;
    std::string hostDir;
};
std::map<uint32_t, FindHandleState>& g_findHandles() {
    static std::map<uint32_t, FindHandleState> m;
    return m;
}
uint32_t next_find_handle() {
    static uint32_t h = 0x60000001u;
    return h++;
}

// iteration-G26, Parte E/O: the exact WIN32_FIND_DATAA-writing logic
// FindFirstFileA had inline (G24's own offset audit -- 318-byte struct, no
// padding gaps) is factored out here so FindNextFileA can share it
// byte-for-byte instead of duplicating it. Returns the FILE_ATTRIBUTE_*
// value written, purely so callers can log it without recomputing it.
uint32_t write_find_data(Cpu& c, uint32_t pFindData, const FindEntry& e) {
    uint32_t attr = e.isDir ? 0x10u : 0x80u;   // FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL
    if (!pFindData) return attr;
    c.write_u32(pFindData + 0, attr);
    uint8_t zeros24[24] = {0};
    c.write(pFindData + 4, zeros24, sizeof zeros24);   // 3x FILETIME -- not tracked, honestly zeroed
    c.write_u32(pFindData + 28, (uint32_t)(e.size >> 32));         // nFileSizeHigh
    c.write_u32(pFindData + 32, (uint32_t)(e.size & 0xFFFFFFFFu)); // nFileSizeLow
    c.write_u32(pFindData + 36, 0);   // dwReserved0
    c.write_u32(pFindData + 40, 0);   // dwReserved1
    uint8_t nameBuf[260] = {0};
    size_t nlen = e.name.size();
    if (nlen > 259) nlen = 259;
    memcpy(nameBuf, e.name.data(), nlen);
    c.write(pFindData + 44, nameBuf, sizeof nameBuf);
    uint8_t altBuf[14] = {0};
    c.write(pFindData + 304, altBuf, sizeof altBuf);
    return attr;
}

// iteration-G24, Parte E: minimal '*'/'?' DOS-style wildcard match,
// case-insensitive (real Win32 filename matching is case-insensitive).
// libc's own fnmatch() was tried first (its header exists in this
// toolchain) but the actual implementation is NOT present in vitasdk's
// libc.a -- confirmed by a link failure ("undefined reference to
// `fnmatch'") and by `nm`ing libc.a directly, not assumed. Per Part E's
// own instruction not to build full globbing if it isn't needed, this is
// just the standard two-pointer backtracking '*'/'?' matcher (the same
// algorithm behind every minimal glob implementation), not a general
// pattern-matching library.
bool wildcard_match_ci(const char* pattern, const char* text) {
    const char* p = pattern;
    const char* t = text;
    const char* starP = nullptr;
    const char* starT = nullptr;
    while (*t) {
        if (*p == '?' || (*p && tolower((unsigned char)*p) == tolower((unsigned char)*t))) {
            ++p; ++t;
        } else if (*p == '*') {
            starP = p++; starT = t;
        } else if (starP) {
            p = starP + 1; t = ++starT;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

// iteration-G25, Parte C/D/E: audited against documented Win32 behavior,
// not memory/assumption. Win32's FindFirstFileA/FindNextFileA special-case
// the exact pattern "*.*" as a legacy MS-DOS 8.3-filename compatibility
// rule: it is defined to match EVERY file, including ones with no '.' in
// the name at all (a plain POSIX-style glob would require a literal '.' to
// be present in the text for a "*.*" pattern to match, which is wrong here
// -- this is a genuine POSIX/Win32 semantic difference, not a POSIX bug).
// This is documented Microsoft platform behavior (see the wildcard-matching
// rules under "Naming Files, Paths, and Namespaces" / FindFirstFile's own
// remarks: "*.*" -> "any file name"), and independently well-known enough
// that other Win32 reimplementations (e.g. Wine's FindFirstFileA) special-
// case it identically for the same compatibility reason -- it is a fixed
// platform ABI/semantic fact, not project-specific reverse-engineered
// behavior the way an enum value or struct offset would be, so it is
// applied directly rather than left pending a hardware A/B test. Only
// "*.*" is special-cased; every other pattern (*, ?, *.wav, *.dat, foo?.*)
// keeps going through the plain matcher, so no other guest pattern's
// behavior changes -- and the fix lives here, in the Win32 layer used by
// every FindFirstFileA call, not as a hack keyed on this specific guest
// path (Part F).
bool win32_wildcard_match(const std::string& pattern, const char* text) {
    if (pattern == "*.*") return true;
    return wildcard_match_ci(pattern.c_str(), text);
}

uint32_t next_handle() {
    // Stays clear of the small fixed handle values other subsystems (sync
    // objects, fake GDI/USER32 handles like 0x8600_/0x8700_) use, so a stray
    // comparison against one of those can never alias a real file handle.
    // AUDITED this iteration (iteration 13): wx86_files()/wx86_file_paths()
    // (guest_files.h) are std::map<uint32_t,...>, NOT a fixed-size array --
    // there is no MAX_HANDLES/handles[N] anywhere in this project or in
    // WinVita's own guest_files.h/.cpp (checked both). A monotonically
    // increasing key into a std::map has no overflow/aliasing risk the way
    // an array index would, so this is intentionally left as-is rather than
    // rebuilt into a free-slot pool -- there was nothing to fix here.
    static uint32_t h = 0x50000001u;
    return h++;
}

// ---- iteration-13 file-I/O diagnostics: a small ring of the last file API
// calls (name + guest return address/ESP + up to 3 args), dumped only when
// one of THESE shims is about to request_halt(). Scoped to file I/O only --
// not a generic cross-subsystem trace facility, since the crash under
// investigation this iteration is bounded to CreateFileA/ReadFile/
// SetFilePointer/CloseHandle.
struct FileCallRecord {
    const char* api = "";
    uint32_t retEip = 0, esp = 0, a1 = 0, a2 = 0, a3 = 0;
};
constexpr size_t kRingSize = 32;
FileCallRecord g_ring[kRingSize];
size_t g_ringPos = 0;

void ring_record(Cpu& c, const char* api, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
    uint32_t esp = c.reg(R_ESP);
    uint32_t retEip = c.read_u32(esp);   // guest return address, [ESP] at trap entry
    g_ring[g_ringPos % kRingSize] = FileCallRecord{api, retEip, esp, a1, a2, a3};
    ++g_ringPos;
}

// iteration-G74, Parte 14-15/21: track-load milestone detection + a
// low-overhead heartbeat. Both TU-local (only [G74-FIRST-RACE-SWAP]/
// [G74-RACE-RUNTIME] in win32/glide2x_ign.cpp need cross-TU state, via
// the true-global g_g74TrackLoadActive/g_g74TrackLoadBeginMs declared
// above, same pattern as g_g56TargetAllocBase).
// G163-BEGIN
// iteration-G163: the open save handles (only ign_3dfx.btz opened for writing is ever in this map: it is empty, and a WriteFile / CloseHandle costs one empty() test, the rest of the time)
std::map<uint32_t, ign::g163::Pending>& g163_pending() { static std::map<uint32_t, ign::g163::Pending> m; return m; }
// G163-END
bool g_g74TrackLoadBeginLogged = false;
std::vector<std::string> g_g74TrackFilesSeen;   // basenames already reported via [G74-TRACK-FILE]

std::string g74_basename_upper(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    for (char& ch : base) ch = (char)toupper((unsigned char)ch);
    return base;
}

// iteration-G76, Parte 16-18: G75's own hardware run PROVED the naive
// "any path under LEVELS/" gate below was a false-positive generator --
// it fired on levels/japan/japan.tex during ordinary startup (a shared/
// menu asset that happens to live under a LEVELS/<name>/ folder for
// unrelated reasons), well before the user ever reached Quick Race, car
// selection, or track selection. Fixed by requiring a STRUCTURALLY
// track-geometry-specific extension -- .COL/.PAN/.TAB/.PLC/.MSH/.POS,
// none of which this project's own history has ever seen used for a
// shared/menu asset (unlike .TEX/.PIC, both independently confirmed
// reused for non-track content: japan.tex here, and G13's own
// n_Sysgfx.pic/n_Sysg_2.pic menu textures) -- before declaring track-
// load begin. The track NAME itself is derived from the path
// (LEVELS/<NAME>/...), never hardcoded to CANADA, so this generalizes to
// whichever track the user actually selects in a given run (Part 18's
// own explicit preference).
bool g_g76TrackTriSeen = false;
bool g_g76TrackSrfSeen = false;

// iteration-G82, Parte 29/30: loading-asset candidate tracking (TU-local,
// only read/written from g74_note_file_open above).
std::string g_g82LastFileBeforeLoad;
uint64_t    g_g82LastFileBeforeLoadMs = 0;
std::string g_g82LoadingAssetCandidate;

// Extracts the track directory name from a host path of the form
// ".../LEVELS/<NAME>/...", uppercased. Empty if the path doesn't match.
std::string g76_extract_track_name(const std::string& upperPath) {
    size_t levelsPos = upperPath.find("LEVELS/");
    if (levelsPos == std::string::npos) levelsPos = upperPath.find("LEVELS\\");
    if (levelsPos == std::string::npos) return std::string();
    size_t nameStart = levelsPos + 7;   // strlen("LEVELS/")
    size_t nameEnd = upperPath.find_first_of("/\\", nameStart);
    if (nameEnd == std::string::npos || nameEnd <= nameStart) return std::string();
    return upperPath.substr(nameStart, nameEnd - nameStart);
}

// Called once per successful CreateFileA. Detects the objective
// transition into SELECTED-TRACK loading (Part 17-18: a generic startup
// asset under LEVELS/ must NOT trigger this -- only a structural,
// geometry-specific extension does) and reports each unique level file
// exactly once (Part 14: "no verbose field dump").
void g74_note_file_open(const std::string& hostPath, long fileSize, uint64_t openRawUs, uint32_t callerEip) {
    std::string upper = hostPath;
    for (char& ch : upper) ch = (char)toupper((unsigned char)ch);

    bool isLevelFile = upper.find("LEVELS/") != std::string::npos || upper.find("LEVELS\\") != std::string::npos;
    std::string base = isLevelFile ? g74_basename_upper(hostPath) : std::string();
    auto hasExt = [&](const char* ext) {
        size_t elen = strlen(ext);
        return base.size() > elen && base.compare(base.size() - elen, elen, ext) == 0;
    };
    // iteration-G76, Parte 17: structural, track-geometry-specific
    // extensions only -- deliberately EXCLUDES .TEX/.PIC (both proven
    // reused for non-track assets, see the comment above).
    bool isStructuralTrackFile = isLevelFile &&
        (hasExt(".COL") || hasExt(".PAN") || hasExt(".TAB") || hasExt(".PLC") ||
         hasExt(".MSH") || hasExt(".POS") || hasExt(".TRI") || hasExt(".SRF"));
    if (isStructuralTrackFile && !g_g113TrackLoadSeen) {   // iteration-G142: the first structural file of a NEW load names the track directory of this race
        ++g_g142LoadSeq; g_g142LoadTri[0] = 0;
        const std::string dir = g76_extract_track_name(upper);
        snprintf(g_g142LoadDir, sizeof g_g142LoadDir, "%s", dir.c_str());
    }
    if (isStructuralTrackFile && (hasExt(".TRI") || hasExt(".SRF")) && !g_g142LoadTri[0]) {
        const size_t keep = sizeof g_g142LoadTri - 1;
        snprintf(g_g142LoadTri, sizeof g_g142LoadTri, "%s", hostPath.size() > keep ? hostPath.c_str() + (hostPath.size() - keep) : hostPath.c_str());
    }
    if (isStructuralTrackFile) g_g113TrackLoadSeen = true;   // iteration-G113, see its declaration above

    // iteration-G82, Parte 29/30: sparse "loading asset" candidate trace
    // -- snapshot the LAST file opened BEFORE this one, at the exact
    // instant structural track-geometry loading begins (before updating
    // the running "last file" tracker with THIS, the structural, file
    // itself) -- a reasonable, evidence-based proxy for "whatever asset
    // the loading screen itself displays" (still UNTRACED at the specific
    // parser/draw-route level, Part 29's own honest scope).
    if (isStructuralTrackFile && !g_g74TrackLoadBeginLogged) {
        g_g82LoadingAssetCandidate = g_g82LastFileBeforeLoad;
    }
    if (!g_g74TrackLoadActive) {
        g_g82LastFileBeforeLoad = hostPath;
        g_g82LastFileBeforeLoadMs = openRawUs;
    }

    if (isStructuralTrackFile && !g_g74TrackLoadBeginLogged) {
        g_g74TrackLoadBeginLogged = true;
        g_g74TrackLoadActive = true;
        g_g74TrackLoadBeginMs = wx86_now_ms();
        g_g76SelectedTrackName = g76_extract_track_name(upper);
        // iteration-G84, Parte 31: new critical-log hook -- this is the
        // one-shot moment the selected track is actually known (derived
        // from the real structural file path, never hardcoded), one of the
        // handful of events ignition_critical.log's own new allowlist
        // still keeps (see log.cpp's g84_critical_tag_is_allowed).
        IGN_CRITICAL("[G35-TRACK-SELECTED]", "track=\"%s\" first_structural_file=\"%s\"",
                     g_g76SelectedTrackName.c_str(), hostPath.c_str());
        IGN_LOG("[G74-TRACK-LOAD-BEGIN]", "first_level_file=\"%s\" caller_guest_eip=0x%08x elapsed_ms=%llu",
                hostPath.c_str(), callerEip, (unsigned long long)g_g74TrackLoadBeginMs);
        // iteration-G76, Parte 18: the CORRECTED milestone -- fires only
        // on a structural track-geometry file, with the track name
        // derived from the path itself, not hardcoded.
        IGN_LOG("[G76-TRACK-LOAD-BEGIN]", "selected_track=\"%s\" first_structural_file=\"%s\" elapsed_ms=%llu "
                                         "(CORRECTED -- G74/G75's own equivalent tags fired too early on a generic "
                                         "LEVELS/ asset, see [G76-LOG-SPAM-AUDIT] narrative for the false-positive "
                                         "this fixes)",
                g_g76SelectedTrackName.c_str(), hostPath.c_str(), (unsigned long long)g_g74TrackLoadBeginMs);
        IGN_LOG("[G75-QUICK-RACE]", "REACHED=YES (car selection + track selection both completed -- inferred from "
                                    "reaching STRUCTURAL level-geometry loading, this project's own confirmed guest flow)");
        IGN_LOG("[G75-TRACK-LOAD]", "BEGIN first_level_file=\"%s\" elapsed_ms=%llu",
                hostPath.c_str(), (unsigned long long)g_g74TrackLoadBeginMs);
        IGN_LOG("[G82-LOADING-ASSET]", "candidate_asset=\"%s\" opened_at_raw_us=%llu (last file opened immediately "
                                       "before structural track-geometry loading began -- a candidate, NOT confirmed "
                                       "to be the actual loading-screen image; draw route/parser still UNTRACED)",
                g_g82LoadingAssetCandidate.c_str(), (unsigned long long)g_g82LastFileBeforeLoadMs);
    }
    if (!isLevelFile) return;
    bool sameTrack = !g_g76SelectedTrackName.empty() && g76_extract_track_name(upper) == g_g76SelectedTrackName;
    if (hasExt(".TRI") && sameTrack) g_g76TrackTriSeen = true;
    if (hasExt(".SRF") && sameTrack) g_g76TrackSrfSeen = true;
    if (g_g76TrackTriSeen && g_g76TrackSrfSeen && !g_g76TrackGeometryComplete) {
        g_g76TrackGeometryComplete = true;
        // iteration-G80, Parte 32: generic confirmation (never hardcoded
        // to CANADA/USA/any specific track -- g_g76SelectedTrackName is
        // always derived from the actual path, see g76_extract_track_name
        // above) that structural race-geometry loading completed for
        // WHICHEVER track the user actually selected this run.
        IGN_LOG("[G80-USA-RACE-RUNTIME]", "selected_track=\"%s\" tri_seen=YES srf_seen=YES race_entry_reached=YES "
                                          "(tag name references G80's own USA test run; this milestone is track-name-agnostic)",
                g_g76SelectedTrackName.c_str());
    }

    for (auto& seen : g_g74TrackFilesSeen) if (seen == base) return;   // already reported once
    g_g74TrackFilesSeen.push_back(base);
    IGN_LOG("[G74-TRACK-FILE]", "file=\"%s\" operation=open size=%ld raw_open_us=%llu",
            base.c_str(), fileSize, (unsigned long long)openRawUs);
}

// Fires at most once per 5 real seconds, ONLY while track-load is active,
// and ONLY from an already-instrumented file shim's own call site (Part
// 21: "every time an existing host-visible shim is reached... no global
// instruction trace"). PROGRESSING is never asserted as HANG -- only
// UNKNOWN_NO_HOST_BOUNDARY when nothing host-visible changed, per Part 21
// 's own explicit instruction not to auto-label a hang.
void g74_track_load_heartbeat(uint32_t callerEip) {
    if (!g_g74TrackLoadActive) return;
    ++g_g74FileOpsSinceHeartbeat;
    uint64_t nowMs = wx86_now_ms();
    if (g_g74LastHeartbeatMs == 0) g_g74LastHeartbeatMs = g_g74TrackLoadBeginMs;
    if (nowMs - g_g74LastHeartbeatMs < 5000) return;

    uint32_t swapCount = ign_get_completed_swap_count();
    uint32_t triCount = ign_get_total_triangles();
    uint32_t swapsSince = swapCount - g_g74LastHeartbeatSwapCount;
    uint32_t trisSince = triCount - g_g74LastHeartbeatTriangleCount;
    uint32_t dinputSince = g_ign_dinput_call_count - g_g74LastHeartbeatDinputCount;
    bool progressing = (g_g74FileOpsSinceHeartbeat > 0) || (swapsSince > 0) || (trisSince > 0) || (dinputSince > 0);
    IGN_LOG("[G74-LOAD-HEARTBEAT]", "elapsed_ms=%llu last_guest_eip=0x%08x file_ops_since_last=%u "
                                    "swaps_since_last=%u triangles_since_last=%u input_getdata_since_last=%u "
                                    "PROGRESSING=%s",
            (unsigned long long)(nowMs - g_g74TrackLoadBeginMs), callerEip, g_g74FileOpsSinceHeartbeat,
            swapsSince, trisSince, dinputSince, progressing ? "YES" : "UNKNOWN_NO_HOST_BOUNDARY");

    // iteration-G74, Parte 26/33: NOT an automatic hang label (Part 21's
    // own explicit instruction) -- just a heartbeat-driven signal that
    // nothing host-visible has changed for 2 consecutive windows (10s+),
    // so a future iteration knows where to look without re-deriving it
    // from raw timestamps.
    static int g74ConsecutiveUnknown = 0;
    static bool g74NextBlockerLogged = false;
    g74ConsecutiveUnknown = progressing ? 0 : (g74ConsecutiveUnknown + 1);
    if (!progressing && g74ConsecutiveUnknown >= 2 && !g74NextBlockerLogged) {
        g74NextBlockerLogged = true;
        IGN_LOG("[G74-NEXT-BLOCKER]", "last_guest_eip=0x%08x elapsed_ms=%llu -- no host-visible progress "
                                     "(file ops/swaps/triangles/dinput) for >=10s -- APPARENT_TRACK_LOAD_STALL, "
                                     "NOT confirmed as a hard hang (see [G74-LOAD-HEARTBEAT] PROGRESSING field)",
                callerEip, (unsigned long long)(nowMs - g_g74TrackLoadBeginMs));
    }

    g_g74FileOpsSinceHeartbeat = 0;
    g_g74LastHeartbeatMs = nowMs;
    g_g74LastHeartbeatSwapCount = swapCount;
    g_g74LastHeartbeatTriangleCount = triCount;
    g_g74LastHeartbeatDinputCount = g_ign_dinput_call_count;
}

// ---- iteration-20 mechanism REMOVED this iteration (iteration-21).
// Real hardware disproved its premise: a "verify at the next HeapAlloc call"
// canary check produces false positives whenever the allocation is used for
// its real purpose before that next call -- e.g. MenuBkg.dat's own buffer
// legitimately gets ReadFile'd over (file bytes, not zeros) right after
// HeapAlloc returns it, so the canary check would flag a perfectly correct
// allocation as "zero-fill did NOT complete" just because something else
// wrote real data there afterward, same as it would for any heap buffer.
// The technique conflated "still zero when I later checked" with "zero-fill
// completed correctly", which only holds for a buffer nobody has touched
// since. Removed rather than patched, per this iteration's explicit
// instruction not to keep inferring from memory observed too late.
//
// Real disassembly citations (kept for the record, still accurate): REP
// STOSD's runtime body (dynarec_arm_00.c:2688-2698, case 0xAB) is emitted
// ONCE at JIT-translation time by MARK/B_MARK (dynarec_arm_helper.h:414,430)
// and MMU_SAI4 (dynarec_arm_helper.h:75) -- all ARM-instruction-emitting
// macros, not runtime functions. The result is a tight native ARM loop
// (ADD/STR/ADD/SUBS/BNE) with zero calls back into C++ for all 81000
// iterations -- there is no runtime helper to hook, and set_alternate()
// cannot reach it either: that primitive only redirects CALL/JMP TARGETS
// (bridge.h's own doc: "the dynarec sends call/jmp targets equal to `from`
// to `to`"), and the REP STOSD at VA 0x0045AFDE is reached by straight-line
// fallthrough from the previous instruction, not by any call/jmp -- it is
// not a valid `from` for that mechanism. The only call/jmp target on this
// path is calloc itself (0x0045AFB0), and redirecting THAT would replace
// calloc for the REST of the run (every future calloc call, not just this
// one) -- exactly the broad, unproven intervention explicitly ruled out
// this iteration and in iteration 18 ("NO interceptar TODO calloc").

// ---- iteration-14: per-handle running checksum of bytes actually handed to
// the guest (FNV-1a, same algorithm ddraw.cpp uses for palette checksums --
// duplicated locally rather than shared across translation units for
// something this small, matching that file's own existing pattern).
// Compared at CloseHandle time against a fresh read of the SAME host file,
// to prove (or disprove) that the file shim delivered the donor file
// byte-for-byte.
uint32_t fnv1a_update(uint32_t h, const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 16777619u; }
    return h;
}
constexpr uint32_t kFnvSeed = 2166136261u;
std::map<uint32_t, uint32_t>& g_readChecksum() { static std::map<uint32_t, uint32_t> m; return m; }
// iteration-G36, Parte U: this checksum (FNV-1a over every byte handed to
// the guest, PLUS a fresh full re-read of the same host file at
// CloseHandle to compare) already did its job -- it proved byte-for-byte
// file delivery across many iterations (menu.col, MenuBkg.dat, and now
// ign1.cdp/ign2.cdp per G35/G36's own logs), and G36's own hardware run
// showed real files in the hundreds-of-KB range going through it
// (ign2.cdp=839186 bytes) during the exact slow post-confirm window this
// iteration is trying to speed up. Disabled by default (compile-time
// constant, not a runtime branch) -- Part U's own instruction: "salvo
// error diagnóstico explícito". Re-enable by flipping this back to true.
constexpr bool kG36FileFullChecksum = false;

void ring_dump() {
    size_t count = g_ringPos < kRingSize ? g_ringPos : kRingSize;
    size_t start = g_ringPos < kRingSize ? 0 : g_ringPos % kRingSize;
    IGN_LOG("[TRACE]", "LAST FILE-IO CALLS (oldest first, %u of %u ring slots used):", (unsigned)count, (unsigned)kRingSize);
    for (size_t i = 0; i < count; ++i) {
        const FileCallRecord& r = g_ring[(start + i) % kRingSize];
        IGN_LOG("[TRACE]", "  [%u] %-16s eip=0x%08x esp=0x%08x args=(0x%08x,0x%08x,0x%08x)",
                (unsigned)i, r.api, r.retEip, r.esp, r.a1, r.a2, r.a3);
    }
}

// ---- iteration-15: "last_guest_context" snapshot. AUDITED this iteration:
// Cpu (cpu.h) exposes no singlestep/instruction-trace/watchpoint facility
// for arbitrary guest addresses -- the only redirect primitive,
// set_alternate(), is a PERMANENT translation-time redirect (the dynarec's
// internalDBGetBlock() refuses to ever translate an address with an
// alternate registered again), which is exactly what caused the WinMain
// regression documented in ignition_boot.cpp when it was misused for
// "observe and continue". So this project does not attempt instruction-
// level tracing or a memory watch on arbitrary guest ranges -- there is no
// safe primitive for it, and reusing set_alternate for that purpose is the
// same mistake, not a new idea. What IS safe and used here: capturing the
// full guest register file (regs_gp(), already a WinVita primitive, no new
// mechanism) at every point we're ALREADY entered via the real trap
// mechanism (a file-I/O shim call) -- the last such snapshot before a crash
// is the best "last known guest state" obtainable without modifying the
// dynarec.
void log_guest_context(Cpu& c, const char* tag) {
    uint32_t regs[8];
    c.regs_gp(regs);
    uint32_t esp = regs[R_ESP];
    uint32_t retEip = c.read_u32(esp);
    IGN_LOG(tag, "guest context at trap entry:");
    IGN_LOG(tag, "  return_eip(=[ESP])=0x%08x", retEip);
    IGN_LOG(tag, "  EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x", regs[R_EAX], regs[R_EBX], regs[R_ECX], regs[R_EDX]);
    IGN_LOG(tag, "  ESP=0x%08x EBP=0x%08x ESI=0x%08x EDI=0x%08x", regs[R_ESP], regs[R_EBP], regs[R_ESI], regs[R_EDI]);
    // Top of the guest stack, for a cheap "does this look corrupted" read --
    // the stack region is 0x06420000..0x0661FFFF (ignition_memlayout.cpp);
    // this just dumps what's there, it doesn't itself validate the range
    // (ESP is already known-good here, since we got HERE via a real stdcall
    // return address on it).
    uint8_t stackBytes[32] = {0};
    c.read(esp, stackBytes, sizeof stackBytes);
    char hex[3 * 32 + 1] = {0};
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 3, 4, "%02x ", stackBytes[i]);
    IGN_LOG(tag, "  stack[ESP..ESP+32) = %s", hex);
}

std::string read_guest_cstr(Cpu& c, uint32_t va, size_t maxlen = 260) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < maxlen; ++i) {
        uint8_t b = 0;
        c.read(va + (uint32_t)i, &b, 1);
        if (!b) break;
        s.push_back((char)b);
    }
    return s;
}

// iteration-G140: the original code loads the language back from ign_3dfx.btz every time the front end is (re)initialised: 4 bytes at file offset 0x593 -> [0x0049051C] (0x00404A8F) and the
// whole 0x597-byte option block -> 0x004914B0 (0x0040E5B3), whose last dword [0x00491A43] feeds the selector / the automatic Enter (0x00405808). Once the user has confirmed a language in
// this process the block that was just read gets that language in those four bytes, so the original init, selector and Enter path pick the right tree and text tables by themselves.
// Called only for reads that overlap the field (a rare, cheap test first); never for any other file; the btz on disk is not modified.
void g140_patch_btz_read(uint32_t handle, long fileOffset, uint8_t* data, uint32_t got) {
    auto pit = wx86_file_paths().find(handle);
    if (pit == wx86_file_paths().end() || !ign::g140::is_btz_path(pit->second.c_str())) return;
    ign::g140::Session& s = ign::g140::g_session;
    uint32_t oldK = 0xFFFFFFFFu;
    const size_t changed = ign::g140::btz_patch(s, (int64_t)fileOffset, data, got, &oldK);
    if (!changed) return;
    ++s.restores;
    if (oldK != 0xFFFFFFFFu && oldK != s.index) {
        ++s.resets;
        IGN_LOG("[G140-LANG-RESET-DETECTED]", "where=btz_read ign_3dfx.btz holds k=%u but the confirmed session language is k=%u (read: offset=%ld bytes=%u)", oldK, s.index, fileOffset, got);
    }
    IGN_LOG("[G140-LANG-RESTORED]", "where=btz_read file_offset=%ld bytes=%u field_bytes_changed=%u old_k=%d -> session_k=%u (restores=%u) track=\"%s\"",
            fileOffset, got, (unsigned)changed, oldK == 0xFFFFFFFFu ? -1 : (int)oldK, s.index, s.restores, g_g76SelectedTrackName.c_str());
}

} // namespace

// iteration-G74, Parte 16-18: static reconstruction of the G73 hardware
// log's own stall bookend, caller_guest_eip=0x00466361 (identical across
// all 3 reported stalls). Cross-referenced this project's own confirmed
// IAT listing (audit/ign_3dfx_imports.txt + a fresh objdump -p of the
// untouched Ign_3dfx.exe): the call immediately before 0x466361 is
// `call DWORD PTR ds:0x46a040`, and 0x0046a040 is CreateFileA's IAT
// slot (confirmed by RVA cross-reference against the real import table,
// not guessed) -- so 0x466361 is exactly the return address right after
// THIS function's own internal CreateFileA call, i.e. `mov esi,eax`
// capturing the returned handle.
//
// The containing function starts at 0x004661AF (`push ebp / mov
// ebp,esp / sub esp,0x1c`, a standard C-runtime-style prologue) and its
// body (0x4661af-0x466463, fully read this iteration) does exactly
// three things, in order: (1) translate a POSIX-style open-flags word
// into a Win32 CreateFileA disposition/attributes pair via a lookup
// table (the 0x100/0x200/.../0x700 comparisons at 0x466260-0x4662dd);
// (2) call CreateFileA once (the call at 0x46635b, return=0x466361);
// (3) call GetFileType on the new handle (IAT 0x46a054) and, ONLY if it
// returns FILE_TYPE_UNKNOWN=0, CloseHandle it again -- otherwise fall
// through into an internal file-descriptor-table bookkeeping call
// (0x46606a). This is a GENERIC C-runtime-level `_open()`/`_sopen()`
// equivalent used for EVERY file the game opens (not track/CANADA.PIC-
// specific), confirmed via the flag-translation table's own shape
// (matches the classic O_CREAT/O_TRUNC/O_EXCL -> CREATE_NEW/CREATE_
// ALWAYS/OPEN_EXISTING/OPEN_ALWAYS/TRUNCATE_EXISTING mapping). It
// contains NO loop, NO decompression, and NO bulk memory copy -- so
// this specific function cannot itself be the source of a multi-second
// stall; whatever takes multiple seconds happens strictly AFTER it
// returns to its own caller (a higher-level asset-loading routine this
// iteration did not fully trace, given the effort this static
// investigation alone already required).
//
// GetFileType matters here: this project's OWN kernel32_ign.cpp never
// registers it -- it is answered entirely by WinVita's vendored default
// stub (external/WinVita/src/runtime/win32_shims_kernel32.cpp:84,
// `K("GetFileType",1,[](Cpu&){ return 1u; })`, i.e. always returns 1,
// nonzero) -- confirmed by reading that file (read-only, not modified,
// per the standing rule on vendored code). Since 1 != 0, the CloseHandle
// fallback branch is dead code in practice; GetFileType itself is a pure
// in-memory stub, zero I/O, effectively instant.
//
// Stall #1's own "next_guest_eip"=0x00462563 was cross-referenced too:
// it is the return address right after a DIFFERENT call, `call DWORD
// PTR ds:0x46a0f8` (CloseHandle's IAT slot), inside a completely
// different function (~0x462xxx) reached from this one only indirectly
// (through its caller). This confirms the gap spans AT LEAST a return
// to the caller and a subsequent, separately-located CloseHandle call --
// consistent with, but not conclusive proof of, guest-side CPU-bound
// work in between (this project's own standing limitation: Cpu::
// set_code_hook is a no-op on the real ARM dynarec target, so no
// per-instruction guest trace exists to close this gap further by
// static means alone). Honestly reported as PARTIALLY localized
// (ACCEPTABLE tier, not BEST): the checkpoint function is fully
// identified and proven incapable of the stall itself, but the exact
// subsequent guest routine responsible for the multi-second gap is not.
// The newly-added [G74-LOAD-HEARTBEAT] (this iteration) is the concrete
// tool to close this gap empirically on the next real hardware run,
// without more speculative disassembly.
//
// Section 15's own question (why CANADA.PIC is opened several times)
// is NOT fully determined this iteration either -- the shared _open()
// wrapper above is called identically for every open regardless of
// which file, so repeated opens of the same filename are architecturally
// unsurprising for CRT-style buffered I/O (e.g. a header/size probe
// followed by a separate full-data open is a common, mundane pattern in
// software of this era) -- but no specific evidence localizes CANADA.
// PIC's own THREE call sites to that exact explanation this iteration.
// Reported as unresolved rather than guessed, per this iteration's own
// explicit instruction ("Do NOT call repeated opens a bug without
// proof").
void g74_log_466361_static_findings() {
    IGN_LOG("[G74-466361-STATIC]", "function=0x004661AF-0x00466463 role=CRT_STYLE_OPEN_WRAPPER "
                                   "(POSIX-oflag-to-CreateFileA-disposition translation + ONE CreateFileA call "
                                   "at 0x46635b/ret=0x00466361 + GetFileType(IAT 0x46a054, answered by WinVita's "
                                   "own vendored instant stub, always returns 1) + fd-table bookkeeping call "
                                   "0x46606a) -- NO loop, NO decompression, NO bulk memcpy -- cannot itself "
                                   "produce a multi-second stall; generic, used for every file open in the game, "
                                   "NOT track/CANADA.PIC-specific");
    IGN_LOG("[G74-466361-STATIC]", "stall_1_next=0x00462563 -- confirmed return address right after a DIFFERENT "
                                   "CloseHandle call (IAT 0x46a0f8) in a separate function (~0x462xxx) -- gap "
                                   "spans at least a return-to-caller plus that separate call; exact intervening "
                                   "guest routine NOT fully traced this iteration (no per-instruction guest trace "
                                   "exists on the real ARM dynarec target -- standing limitation, not new)");
    IGN_LOG("[G74-CANADA-PIC-PATH]", "repeated CANADA.PIC opens NOT fully explained this iteration -- the shared "
                                     "CRT-style _open() wrapper above is identical for every file regardless of "
                                     "name, so repeated-open-by-name is architecturally unsurprising but not "
                                     "specifically proven for this file; reported unresolved rather than guessed");
    IGN_LOG("[G74-STALL-CLASS]", "stall_1_ms=5121 stall_2_ms=6663 stall_3_ms=5614 (all reported by the user's own "
                                 "G74 hardware log) classification=INCONCLUSIVE_THIS_ITERATION -- static analysis "
                                 "proves the checkpoint function itself is fast (no loop/no I/O beyond one "
                                 "CreateFileA+GetFileType), and this build's own raw-I/O-vs-diagnostic-logging "
                                 "separation (see [G49-FILEIO]/[G74-LOGGER-PERF]) plus [G74-LOAD-HEARTBEAT] are "
                                 "the concrete next-run evidence needed to finish this classification -- NOT "
                                 "asserted as HOST_IO, LOGGER_OVERHEAD, or GUEST_CPU without that evidence");
}

// iteration-G74, Parte 32: kVerboseFileIoDiagnostics is a compile-time
// `inline constexpr bool` (ignition_paths.h) -- a single build only ever
// exercises ONE of its two values, so there is no way to A/B it at
// runtime inside one binary. The actual guarantee ("only log TEXT volume
// differs, never file semantics") is structural: every gated block added
// this iteration (grep kVerboseFileIoDiagnostics in this file) contains
// ONLY IGN_LOG calls plus their own timing wrapper -- never a call that
// touches fopen/fread/fseek/fclose, a guest buffer, or a return value --
// true by inspection at the point each block was written, not by a
// fabricated runtime test. What this selftest DOES verify at runtime is
// the new parsing/dedup logic itself (g74_basename_upper), since that is
// genuinely testable in isolation.
void g74_selftest() {
    bool pass = true;
    std::string b1 = g74_basename_upper("ux0:data/ignition/LEVELS/CANADA/canada.pic");
    bool p1 = (b1 == "CANADA.PIC");
    pass = pass && p1;
    IGN_LOG("[G74-SELFTEST]", "basename_upper(\"LEVELS/CANADA/canada.pic\")=\"%s\" expect=\"CANADA.PIC\" %s",
            b1.c_str(), p1 ? "PASS" : "FAIL");

    std::string b2 = g74_basename_upper("nobackslash.col");
    bool p2 = (b2 == "NOBACKSLASH.COL");
    pass = pass && p2;
    IGN_LOG("[G74-SELFTEST]", "basename_upper(\"nobackslash.col\")=\"%s\" expect=\"NOBACKSLASH.COL\" %s",
            b2.c_str(), p2 ? "PASS" : "FAIL");

    IGN_LOG("[G74-SELFTEST]", "overall=%s (kVerboseFileIoDiagnostics file-semantics guarantee is STRUCTURAL, "
                              "verified by code inspection, not a runtime A/B -- see this function's own comment)",
            pass ? "PASS" : "FAIL");
}

void kernel32_ign_install(Bridge& br, PeImage& exe) {
    g74_log_466361_static_findings();
    g74_selftest();
    auto K = [&](const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
        Shim s; s.argc = argc; s.stdcall_cleanup = true;
        s.tag = std::string("KERNEL32.dll!") + name;
        // iteration-G30, Parte A/B/C: one of several central per-call
        // wrapper points the Vita input poll now hooks into -- see
        // dinput.h's own comment for why grBufferSwap alone (G29) proved
        // far too sparse at the language-selection screen. Every
        // KERNEL32 call this project handles passes through here, so
        // sampling is at least as frequent as whatever the guest's own
        // file/heap/module activity already is -- still fully synchronous
        // on the single thread that runs the guest, no new thread.
        s.fn = [fn](Cpu& c) -> uint32_t { ++g_ign_kernel32_call_count; dinput_poll_vita_input(c); return fn(c); };
        br.register_shim("KERNEL32.dll", name, s);
    };

    const uint32_t exe_base = exe.load_base();

    K("GetModuleHandleA", 1, [exe_base](Cpu& c) -> uint32_t {
        uint32_t nameVa = c.arg(0);
        if (!nameVa) {
            IGN_LOG("[WIN32]", "GetModuleHandleA(NULL) -> 0x%08x", exe_base);
            return exe_base;
        }
        std::string name = read_guest_cstr(c, nameVa);
        IGN_LOG("[WIN32]", "GetModuleHandleA(\"%s\") -> 0 (only the exe's own module is known so far)", name.c_str());
        return 0;
    });

    K("GetModuleFileNameA", 3, [](Cpu& c) -> uint32_t {
        uint32_t buf = c.arg(1), size = c.arg(2);
        // iteration-G1: was hardcoded to "...\\Ign_d3d.exe" independently of
        // which exe actually got loaded -- see runtime/ignition_identity.h.
        const char* kName = kGuestExePathBackSlash;
        uint32_t n = (uint32_t)strlen(kName);
        if (size && n >= size) n = size - 1;
        if (buf && size) {
            c.write(buf, kName, n);
            uint8_t z = 0; c.write(buf + n, &z, 1);
        }
        IGN_LOG("[WIN32]", "GetModuleFileNameA -> \"%.*s\"", (int)n, kName);
        return n;
    });

    K("GetCommandLineA", 0, [](Cpu& c) -> uint32_t {
        static uint32_t s_va = 0;
        if (!s_va) s_va = wx86_scratch_put_cstr(c, kGuestExeName);
        IGN_LOG("[WIN32]", "GetCommandLineA -> \"%s\"", kGuestExeName);
        return s_va;
    });

    K("ExitProcess", 1, [](Cpu& c) -> uint32_t {
        uint32_t code = c.arg(0);
        // [ESP] at trap entry (before any arg cleanup) is the guest return
        // address -- i.e. the actual call site in Ignition's/the CRT's own
        // code, not the trap-window slot's own address. Requested so a
        // premature ExitProcess can be traced back to exactly which branch
        // called it.
        uint32_t esp = c.reg(R_ESP);
        uint32_t callSite = c.read_u32(esp);
        uint32_t eax = c.reg(R_EAX);
        IGN_LOG("[WIN32]", "ExitProcess(%u)", code);
        IGN_LOG("[WIN32]", "  called from guest EIP=0x%08x ESP=0x%08x EAX=0x%08x", callSite, esp, eax);
        char reason[48]; snprintf(reason, sizeof reason, "ExitProcess(%u)", code);
        request_halt(reason);
        return 0;
    });

    K("TerminateProcess", 2, [](Cpu& c) -> uint32_t {
        uint32_t code = c.arg(1);
        uint32_t esp = c.reg(R_ESP);
        uint32_t callSite = c.read_u32(esp);
        uint32_t eax = c.reg(R_EAX);
        IGN_LOG("[WIN32]", "TerminateProcess(handle=0x%08x, code=%u)", c.arg(0), code);
        IGN_LOG("[WIN32]", "  called from guest EIP=0x%08x ESP=0x%08x EAX=0x%08x", callSite, esp, eax);
        char reason[48]; snprintf(reason, sizeof reason, "TerminateProcess(%u)", code);
        request_halt(reason);
        return 1;
    });

    K("CreateFileA", 7, [](Cpu& c) -> uint32_t {
        ++g_ign_createfilea_count;
        if (g_ign_createfilea_count == 1) {
            IGN_LOG("[G78-STARTUP-TIMING]", "checkpoint=first_game_file elapsed_ms=%llu",
                    (unsigned long long)(wx86_now_ms() - g_g78ProcessStartMs));
        }
        std::string guestPath = read_guest_cstr(c, c.arg(0));
        uint32_t access = c.arg(1), shareMode = c.arg(2), disposition = c.arg(4), flags = c.arg(5);
        ring_record(c, "CreateFileA", access, disposition, flags);
        std::string hostPath = translate_path(guestPath.c_str());

        constexpr uint32_t GENERIC_WRITE = 0x40000000u;
        constexpr uint32_t CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_ALWAYS = 4;
        const char* mode = "rb";
        if (disposition == CREATE_NEW || disposition == CREATE_ALWAYS) mode = "wb";
        else if (access & GENERIC_WRITE) mode = "r+b";

        // G163-BEGIN
        const bool g163Btz = ign::g140::is_btz_path(hostPath.c_str());
        if (g163Btz && mode[0] == 'r') ign::g163::recover(hostPath, ign::g163::g_stats);   // an interrupted commit of the save is finished before the game reads it
        std::string g163Tmp; if (g163Btz && mode[0] == 'w') { g163Tmp = ign::g163::tmp_path(hostPath); ++ign::g163::g_stats.opens; mode = "rb"; }   // the game's "wb" open of the save: the open below only LOOKS at the old file (nothing is truncated); the save is written to the temporary file
        // G163-END
        uint64_t g49OpenStartUs = wx86_now_us();
        FILE* f = fopen(hostPath.c_str(), mode);
        if (!g163Tmp.empty()) { if (f) fclose(f); f = fopen(g163Tmp.c_str(), "wb"); if (!f) { g163Tmp.clear(); ++ign::g163::g_stats.tmp_open_failed; f = fopen(hostPath.c_str(), "wb"); } }   // G163: the temporary file (or, if none is possible, the game's own way)
        if (!f && disposition == OPEN_ALWAYS) f = fopen(hostPath.c_str(), "wb");
        uint64_t g49OpenUs = wx86_now_us() - g49OpenStartUs;
        g_g49FileOpenTotalUs += g49OpenUs;
        if (!f) {
            IGN_LOG("[FILE]", "CreateFileA(\"%s\") -> INVALID_HANDLE_VALUE (host open of %s failed, mode=\"%s\")",
                    guestPath.c_str(), hostPath.c_str(), mode);
            // iteration-G35, Parte K: name + result only, dual-logged --
            // looking for intro/video assets (AVI/MPEG/SMK/FLI/etc.) opened
            // after a real language confirm, without assuming a format.
            IGN_CRITICAL("[G35-FILE]", "CreateFileA(\"%s\") -> FAILED", guestPath.c_str());
            return 0xFFFFFFFFu;
        }
        uint32_t h = next_handle();
        wx86_files()[h] = f;
        wx86_file_paths()[h] = hostPath;
        if (!g163Tmp.empty()) { ign::g163::Pending& g163p = g163_pending()[h]; g163p.tmp = g163Tmp; g163p.fin = hostPath; }   // G163

        long fileSize = -1;
        { long cur = ftell(f); fseek(f, 0, SEEK_END); fileSize = ftell(f); fseek(f, cur, SEEK_SET); }

        // iteration-G74, Parte 8-13: the 10-line verbose dump below is
        // exactly the kind of per-call diagnostic text ignition_paths.h's
        // kVerboseFileIoDiagnostics gates -- each line is its own
        // fopen(append)+fprintf+fclose cycle (diagnostics/log.cpp's own
        // design, never buffered). Timed separately from the raw open
        // above (g_g49FileOpenTotalUs, untouched) so the two costs are
        // never conflated. RAM-only counters (g_ign_createfilea_count,
        // already incremented; the track-file dedup below) stay identical
        // either way -- only log TEXT volume changes.
        if (kVerboseFileIoDiagnostics) {
            uint64_t g74DiagStartUs = wx86_now_us();
            IGN_LOG("[FILE]", "CreateFileA");
            IGN_LOG("[FILE]", "  path_guest          = \"%s\"", guestPath.c_str());
            IGN_LOG("[FILE]", "  path_vita           = \"%s\"", hostPath.c_str());
            IGN_LOG("[FILE]", "  desiredAccess       = 0x%08x", access);
            IGN_LOG("[FILE]", "  shareMode           = 0x%08x", shareMode);
            IGN_LOG("[FILE]", "  creationDisposition = %u", disposition);
            IGN_LOG("[FILE]", "  flagsAndAttributes  = 0x%08x", flags);
            IGN_LOG("[FILE]", "  host_fd             = %p (fopen mode \"%s\")", (void*)f, mode);
            IGN_LOG("[FILE]", "  file_size           = %ld", fileSize);
            IGN_LOG("[FILE]", "  guest_handle        = 0x%08x", h);
            IGN_LOG("[FILE]", "  active_handles      = %u", (unsigned)wx86_files().size());
            g_g74FileDiagLogUs += (wx86_now_us() - g74DiagStartUs);
        }
        IGN_CRITICAL("[G35-FILE]", "CreateFileA(\"%s\") -> OK size=%ld", guestPath.c_str(), fileSize);

        uint32_t callerEip = c.read_u32(c.reg(R_ESP));
        g74_note_file_open(hostPath, fileSize, g49OpenUs, callerEip);
        g74_track_load_heartbeat(callerEip);
        return h;
    });

    K("ReadFile", 5, [](Cpu& c) -> uint32_t {
        ++g_ign_readfile_count;
        uint32_t h = c.arg(0), buf = c.arg(1), n = c.arg(2), pOut = c.arg(3);
        ring_record(c, "ReadFile", h, buf, n);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) {
            IGN_LOG("[FILE]", "ReadFile(bad handle 0x%08x)", h);
            if (pOut) c.write_u32(pOut, 0);
            return 0;
        }
        long offBefore = ftell(it->second);
        if (kVerboseFileIoDiagnostics) {
            uint64_t g74DiagStartUs = wx86_now_us();
            IGN_LOG("[FILE]", "ReadFile handle=0x%08x host_fd=%p guest_buffer=0x%08x bytes_requested=%u file_offset_before=%ld",
                    h, (void*)it->second, buf, n, offBefore);
            g_g74FileDiagLogUs += (wx86_now_us() - g74DiagStartUs);
        }

        // AUDITED this iteration: cpu_box86.cpp's Cpu::write() only bounds-
        // checks against the whole arena span (arena_check) -- unlike
        // read()/mapped(), it does NOT verify the target was ever actually
        // map()-ed (its own comment: "a wild guest pointer silently aliases
        // whatever host object sits there ... corruption surfaces far from
        // its cause"). So an invalid buf here would NOT be caught by write()
        // itself -- it has to be caught HERE, before the copy. This is
        // deliberately checked BEFORE reading the host file, so a bad guest
        // buffer never touches guest memory at all.
        if (n > 0 && (!buf || !c.mapped(buf) || !c.mapped(buf + n - 1))) {
            IGN_LOG("[FILE]", "ReadFile REFUSED: guest_buffer=0x%08x..0x%08x (n=%u) is NOT fully mapped guest memory",
                    buf, buf + n - 1, n);
            ring_dump();
            auto pit = wx86_file_paths().find(h);
            char reason[160];
            snprintf(reason, sizeof reason,
                     "ReadFile: invalid/unmapped guest destination buffer 0x%08x (n=%u), handle=0x%08x, path=%s",
                     buf, n, h, pit != wx86_file_paths().end() ? pit->second.c_str() : "?");
            request_halt(reason);
            if (pOut) c.write_u32(pOut, 0);
            return 0;
        }

        // AUDITED this iteration: "buf is mapped" is necessary but NOT
        // sufficient -- the mapped-page check above only proves the address
        // belongs to SOME region Cpu::map()-ed (HEAP/VA/SCRATCH/PE/stack),
        // not that it fits inside the SPECIFIC allocation Ignition's own
        // code obtained for it. Ignition's HeapAlloc/malloc-family calls are
        // already tracked by ignition_heap_region() (the same GuestRegion
        // win32_shims_memory.cpp's real HeapAlloc carves blocks from), which
        // already has exactly the lookup this needs (GuestRegion::block_of/
        // size_of) -- no new tracker built, this reuses the engine's own
        // bookkeeping.
        if (n > 0) {
            wx86::GuestRegion& heap = ignition_heap_region();
            uint32_t base = heap.block_of(buf);
            if (base) {
                uint32_t allocSize = heap.size_of(base);
                uint32_t offset = buf - base;
                uint32_t remaining = (offset < allocSize) ? (allocSize - offset) : 0;
                if (kVerboseFileIoDiagnostics) {
                    uint64_t g74DiagStartUs = wx86_now_us();
                    IGN_LOG("[HEAPCHECK]", "buffer=0x%08x allocation_base=0x%08x allocation_size=%u offset=%u remaining_capacity=%u bytes_to_write=%u",
                            buf, base, allocSize, offset, remaining, n);
                    g_g74FileDiagLogUs += (wx86_now_us() - g74DiagStartUs);
                }
                if (n > remaining) {
                    IGN_LOG("[HEAPCHECK]", "BUFFER OVERFLOW PREVENTED: bytes_to_write=%u > remaining_capacity=%u (allocation 0x%08x, size %u)",
                            n, remaining, base, allocSize);
                    ring_dump();
                    char reason[176];
                    snprintf(reason, sizeof reason,
                             "ReadFile: would overflow guest HEAP allocation 0x%08x (size=%u) at offset %u with %u bytes, handle=0x%08x",
                             base, allocSize, offset, n, h);
                    request_halt(reason);
                    if (pOut) c.write_u32(pOut, 0);
                    return 0;
                }
            } else if (kVerboseFileIoDiagnostics) {
                uint64_t g74DiagStartUs = wx86_now_us();
                IGN_LOG("[HEAPCHECK]", "buffer=0x%08x not tracked in the HEAP region (VA/scratch/PE/stack, or a raw guest address) -- allocation-bound check skipped, mapped-page check above still applies",
                        buf);
                g_g74FileDiagLogUs += (wx86_now_us() - g74DiagStartUs);
            }
        }

        std::vector<uint8_t> tmp(n);
        uint64_t g49ReadStartUs = wx86_now_us();
        size_t got = n ? fread(tmp.data(), 1, n, it->second) : 0;
        uint64_t g49ReadUs = wx86_now_us() - g49ReadStartUs;
        g_g49FileReadTotalUs += g49ReadUs;
        if (g49ReadUs > g_g49FileReadMaxUs) g_g49FileReadMaxUs = g49ReadUs;
        if ((uint32_t)got > g_g49FileReadMaxBytes) g_g49FileReadMaxBytes = (uint32_t)got;
        uint64_t g49FileIoNowMs = wx86_now_ms();
        if (g_g49FileIoWindowStartMs == 0) g_g49FileIoWindowStartMs = g49FileIoNowMs;
        if (g49FileIoNowMs - g_g49FileIoWindowStartMs >= 5000) {
            // iteration-G74, Parte 11: open/read raw timing UNCHANGED from
            // G49 (confirmed by re-reading this whole function: both
            // windows wrap ONLY the fopen/fread calls themselves, never an
            // IGN_LOG call) -- seek_total_us/close_total_us added
            // alongside for the same raw-I/O-only guarantee, and
            // file_diag_log_us reported separately so the two costs are
            // never conflated.
            IGN_LOG("[G49-FILEIO]", "open_total_us=%llu read_total_us=%llu seek_total_us=%llu close_total_us=%llu "
                                    "read_max_us=%llu read_max_bytes=%u createfilea_calls=%u readfile_calls=%u "
                                    "file_diag_log_us=%llu (RAW I/O ONLY above, logging cost isolated here)",
                    (unsigned long long)g_g49FileOpenTotalUs, (unsigned long long)g_g49FileReadTotalUs,
                    (unsigned long long)g_g49FileSeekTotalUs, (unsigned long long)g_g49FileCloseTotalUs,
                    (unsigned long long)g_g49FileReadMaxUs, g_g49FileReadMaxBytes, g_ign_createfilea_count, g_ign_readfile_count,
                    (unsigned long long)g_g74FileDiagLogUs);
            uint64_t loggerTotalUs = 0, loggerMaxUs = 0, loggerLines = 0;
            log_get_perf_stats(&loggerTotalUs, &loggerMaxUs, &loggerLines);
            IGN_LOG("[G74-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull);
            IGN_LOG("[G75-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu low_overhead_mode=%s",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    kG75LowOverheadRuntimeLog ? "YES" : "NO");
            // iteration-G76, Parte 32: same measurement, re-tagged for
            // this iteration's own aggressive [G21]/[G23] fix -- goal is
            // logger_total_us/elapsed_runtime_us < 10%% (prefer <5%%).
            IGN_LOG("[G76-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G77, Parte 30-33: G76's own real-hardware result
            // (lines=3976, overhead=44.3%% at ~163s) proved its own
            // [G21]/[G23] fix INSUFFICIENT -- this iteration additionally
            // gated mciSendStringA's per-call stack dump (win32/
            // winmm_ign.cpp, the actual dominant spam source this time,
            // Part 31's own "routine MCI stack dump" callout). Same
            // measurement, re-tagged again so this run's number is never
            // confused with G76's own already-published 44.3%% figure.
            IGN_LOG("[G77-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f target_lines_5min=500 target_overhead_percent=10.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G78, Parte 29/33: G77's own real-hardware result
            // (74.7% peak, one single line costing 1,066,330us) proved
            // EVERY prior per-call-site gate insufficient -- this run's
            // number is what the NEW central tag-denylist filter
            // (diagnostics/log.h kG78CentralLogFilterEnabled, diagnostics/
            // log.cpp's g78_tag_is_denied) actually achieves, re-tagged so
            // it's never confused with G76/G77's own already-published
            // figures. Hard target: <300 lines before race, <600 after
            // 5 minutes, overhead <5%% (10%% maximum acceptable).
            // iteration-G79, Parte 24-27: G78's own hardware run proved
            // THAT tag's own denylist incomplete (79.2%/45.9% overhead) --
            // this replaces it as the ONE retained logger-perf line (the
            // prior G74-G78 tags are now in diagnostics/log.cpp's own
            // denylist, so only this one still reaches the file). Hard
            // target this iteration: <150 lines before race, <300 in 5
            // minutes, overhead <3%% (5%% maximum acceptable).
            // iteration-G80, Parte 33/36: G79's own hardware run proved
            // ITS tag insufficient too (77.4%%/37.9%%, plus the separate
            // G67-WATCH-CHANGE spam bug fixed elsewhere this iteration) --
            // replaces it as the one retained logger-perf line (G74-G79's
            // own tags are now all denylisted in diagnostics/log.cpp).
            // Hard target: <100 lines before race, <200 in 5 minutes,
            // overhead <2%% (5%% maximum acceptable, no excuses).
            IGN_LOG("[G80-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f target_lines_before_race=100 target_lines_5min=200 "
                                         "target_overhead_percent=2.0 max_acceptable_overhead_percent=5.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G81, Parte 36-39: G80's own hardware run proved
            // ITS tag insufficient too (82.2%%/41.8%%) -- replaces it as
            // the one retained logger-perf line (G74-G80's own tags are
            // now all denylisted in diagnostics/log.cpp, alongside four
            // NEW routine tags found this iteration: [G24]/[G25]/[G26]
            // FindFirstFileA/FindNextFileA enumeration, [G39]/[G40]
            // texture alloc/download, [G74-TRACK-FILE] per-file level
            // loading, [GLIDE-G12] grClipWindow's periodic summary).
            // RUN25/RUN27/RUN29 re-checked again this iteration and still
            // do not exist as real log tags anywhere in this codebase.
            // Hard target: <80 lines before race, <150 in 5 minutes,
            // overhead <1%% (3%% maximum acceptable, no excuses).
            IGN_LOG("[G81-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f target_lines_before_race=80 target_lines_5min=150 "
                                         "target_overhead_percent=1.0 max_acceptable_overhead_percent=3.0 "
                                         "RUN25_RUN27_RUN29_exist_in_codebase=NO",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G82, Parte 35-39: G81's own hardware run proved
            // the REAL dominant boot cost was never a per-swap tag at all
            // -- it was ~42 giant one-paragraph historical narrative lines
            // (bare "[G40]".."[G81]", one per past iteration) printing
            // UNCONDITIONALLY on every boot, now denylisted in
            // diagnostics/log.cpp. Hard target: <80 lines before race,
            // <150 in 5 minutes, overhead <1%% (3%% max acceptable).
            IGN_LOG("[G82-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f target_lines_before_race=60 target_lines_5min=120 "
                                         "target_overhead_percent=1.0 max_acceptable_overhead_percent=3.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G83, Parte 30-32: the denylist that G82's own line
            // above still names is GONE -- diagnostics/log.cpp now runs an
            // ALLOWLIST (only "[BUILD"/"[G82"/"[G83"/[STOP]/ERROR/ABORT
            // tags survive log_line()'s own check, before the file is even
            // opened), so every pre-G82 tag family is silenced by
            // construction, not by having been individually found and
            // listed. New, tighter targets per this iteration's own spec:
            // <25 lines at boot, <40 before race, <100 in 5 minutes,
            // overhead <1%% target / >2%% counts as still failing.
            IGN_LOG("[G83-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=ALLOWLIST_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G84, Parte 32: G83's own hardware run measured
            // 13.3%% overhead at 511 lines/68.2s DESPITE the allowlist --
            // largely explained by [RUN27]/[RUN29] (see
            // [G84-RUN-MARKER-SOURCE] in ignition_boot.cpp for the exact
            // source, found this iteration in vendored dynarec code this
            // project's own g_g74Logger* counters CANNOT see, since those
            // lines bypass log_line() entirely via a raw fopen/fprintf).
            // This project's own allowlist prefix is now "[BUILD-G84"/
            // "[G84" ONLY (was "[BUILD"/"[G82"/"[G83", which let through
            // every historical build marker back to G1). Diagnostic-clear
            // build gets a looser 5-minute target since [G84-CLEAR-PROBE]
            // itself adds up to 30 extra lines by design.
            IGN_LOG("[G84-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G84_TAGS_ONLY build_variant=%s "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=%d "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0 "
                                         "note=this_projects_own_log_line_counters_do_not_and_cannot_see_"
                                         "RUN27_RUN29_lines_written_by_vendored_dynarec_via_raw_fopen_see_"
                                         "G84_RUN_MARKER_SOURCE",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0,
#ifdef G84_CLEAR_PROBE
                    "clear_probe", 150
#else
                    "normal", 100
#endif
                    );
            // iteration-G85, Parte 27: G84's own hardware run hit the
            // logger's best-ever numbers (~0.8-1.0%% overhead) -- this
            // iteration adds ZERO new hot-path IGN_LOG calls (all new G85
            // material capture is RAM-only counters/ring, one aggregate
            // dump at RACE_10 or the 128-cap, matching Part 27's own
            // explicit "no high-frequency logging again" instruction).
            IGN_LOG("[G85-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G85_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0 "
                                         "new_hot_path_log_calls_this_iteration=0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G86, Parte 34: same discipline -- ALL new capture
            // (world-scalar ring, tex-alloc ring, validation) is RAM-only
            // counters/arrays, zero new hot-path IGN_LOG calls, one
            // aggregate dump at RACE_10/256-cap (g86_dump_report).
            IGN_LOG("[G86-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G86_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0 "
                                         "new_hot_path_log_calls_this_iteration=0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G87, Parte 32: this iteration's own Vita-side work
            // is almost entirely documentation/static-audit logs (one-shot
            // at boot) plus native-proxy changes -- no new per-frame/
            // per-allocation RAM counters were added on the Vita side
            // either.
            IGN_LOG("[G87-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G87_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0 "
                                         "new_hot_path_log_calls_this_iteration=0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G88, Parte 25-26: no ABI fix was implemented this
            // iteration either (still gated on native proof this project
            // does not have), so there is nothing new on the Vita hot
            // path at all -- this iteration's real work is the native
            // proxy package and documentation only.
            IGN_LOG("[G88-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G88_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0 "
                                         "new_hot_path_log_calls_this_iteration=0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G89, Parte 26/31: the MMID fix itself adds no new
            // per-call file logging (guTexAllocateMemory/guTexSource/
            // guTexCombineFunction/guTexMemReset log lines are the SAME
            // existing ones, already gated the same way as before -- only
            // the guest-visible VALUE they print changed shape from
            // 0x700000xx to a small int). All new G89 counters are
            // RAM-only, one aggregate dump.
            IGN_LOG("[G89-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G89_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0 "
                                         "new_hot_path_log_calls_this_iteration=0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G90, Parte 21: grChromakeyValue_impl's own new log
            // lines are all bounded (first 32 runtime calls, first 8
            // progress dumps, 2 one-shot semantics lines) -- no unbounded
            // per-call hot-path logging added.
            IGN_LOG("[G90-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G90_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G91, Parte 29: the validator fix itself is a pure
            // boolean-derivation correction, zero new hot-path logging;
            // the new [G91-NONFINITE-TRIANGLE]/[G91-FAIL-CALLER] capture
            // is a one-shot (first occurrence only), same discipline as
            // every other "capture the first bad one" diagnostic already
            // in this file.
            IGN_LOG("[G91-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G91_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.0 fail_overhead_percent=2.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G92, Parte 26: all 3 phases' 10-frame windows are
            // RAM-buffered (G92Phase::samples), one aggregate dump each --
            // zero per-frame fopen, target <1.5%%, fail >2%%.
            IGN_LOG("[G92-LOGGER-PERF]", "lines=%llu total_us=%llu max_us=%llu avg_us=%llu elapsed_ms=%llu "
                                         "overhead_percent=%.1f logging_policy=G92_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.5 fail_overhead_percent=2.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            // iteration-G93, Parte 30: G92 reached ~2.6%% during heavy
            // capture -- trimmed per-frame G92 lines to first/last frame
            // only (was every frame), and G93's own timeline dump is a
            // ONE-SHOT, collapsed (run-length-encoded) event log, not one
            // line per triangle. Target back down to <1.5%%.
            IGN_LOG("[G103-PERF]", "logger_lines=%llu logger_total_us=%llu logger_max_us=%llu logger_avg_us=%llu elapsed_ms=%llu "
                                         "logger_overhead_percent=%.1f logging_policy=G103_TAGS_ONLY "
                                         "target_lines_boot=25 target_lines_before_race=40 target_lines_5min=100 "
                                         "target_overhead_percent=1.5 fail_overhead_percent=2.0",
                    (unsigned long long)loggerLines, (unsigned long long)loggerTotalUs, (unsigned long long)loggerMaxUs,
                    loggerLines ? (unsigned long long)(loggerTotalUs / loggerLines) : 0ull,
                    (unsigned long long)g49FileIoNowMs,
                    g49FileIoNowMs ? (100.0 * (double)loggerTotalUs / 1000.0 / (double)g49FileIoNowMs) : 0.0);
            g_g49FileIoWindowStartMs = g49FileIoNowMs;
        }
        if (ign::g140::g_session.valid && ign::g140::overlaps_btz_lang(offBefore, (uint32_t)got)) {
            g140_patch_btz_read(h, offBefore, tmp.data(), (uint32_t)got);   // iteration-G140: gives the confirmed session language back where the original code loads it
        }
        if (got) {
            g_g15BytesReadPerHandle()[h] += got;
            c.write(buf, tmp.data(), (uint32_t)got);
            if (kG36FileFullChecksum) {
                uint32_t blockChecksum = fnv1a_update(kFnvSeed, tmp.data(), got);
                auto& running = g_readChecksum();
                auto cit = running.find(h);
                uint32_t before = (cit != running.end()) ? cit->second : kFnvSeed;
                uint32_t after = fnv1a_update(before, tmp.data(), got);
                running[h] = after;
                IGN_LOG("[FILE]", "  block_checksum=0x%08x running_checksum=0x%08x", blockChecksum, after);
            }
        }
        if (pOut) c.write_u32(pOut, (uint32_t)got);
        long offAfter = ftell(it->second);
        if (kVerboseFileIoDiagnostics) {
            uint64_t g74DiagStartUs = wx86_now_us();
            IGN_LOG("[FILE]", "ReadFile -> bytes_read=%u file_offset_after=%ld", (unsigned)got, offAfter);
            g_g74FileDiagLogUs += (wx86_now_us() - g74DiagStartUs);
        }
        g74_track_load_heartbeat(c.read_u32(c.reg(R_ESP)));
        return 1;
    });

    K("WriteFile", 5, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), buf = c.arg(1), n = c.arg(2), pOut = c.arg(3);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) {
            IGN_LOG("[WIN32]", "WriteFile(bad handle 0x%08x)", h);
            return 0;
        }
        std::vector<uint8_t> tmp(n);
        if (n) c.read(buf, tmp.data(), n);
        size_t wrote = n ? fwrite(tmp.data(), 1, n, it->second) : 0;
        { auto& g163m = g163_pending(); if (!g163m.empty()) { auto g163i = g163m.find(h); if (g163i != g163m.end()) ign::g163::note_write(g163i->second, n, wrote, ign::g163::g_stats); } }   // G163
        if (pOut) c.write_u32(pOut, (uint32_t)wrote);
        return 1;
    });

    K("CloseHandle", 1, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0);
        ring_record(c, "CloseHandle", h);
        auto it = wx86_files().find(h);
        if (it != wx86_files().end()) {
            FILE* fd = it->second;
            auto pathIt0 = wx86_file_paths().find(h);
            std::string path = pathIt0 != wx86_file_paths().end() ? pathIt0->second : "";
            uint64_t g49CloseStartUs = wx86_now_us();
            fclose(fd);
            g_g49FileCloseTotalUs += (wx86_now_us() - g49CloseStartUs);
            { auto& g163m = g163_pending(); if (!g163m.empty()) { auto g163i = g163m.find(h); if (g163i != g163m.end()) { ign::g163::commit(g163i->second.fin, g163i->second.requested, g163i->second.written, g163i->second.failed, ign::g163::g_stats); ++ign::g163::g_stats.closes; g163m.erase(g163i); } } }   // G163: the save is replaced only now, and only when every byte was written

            // Source-vs-delivered checksum (sections 20-21): only meaningful
            // for a handle we actually READ from this session (g_readChecksum
            // is only populated by ReadFile) -- re-reads the SAME host path
            // fresh from disk (cheap: these are small asset files) and
            // compares against the running FNV-1a accumulated as bytes were
            // handed to the guest, to prove or disprove that the file shim
            // itself delivered the donor file byte-for-byte.
            auto csIt = g_readChecksum().find(h);
            if (csIt != g_readChecksum().end()) {
                uint32_t sourceChecksum = kFnvSeed;
                long sourceSize = -1;
                if (!path.empty()) {
                    FILE* sf = fopen(path.c_str(), "rb");
                    if (sf) {
                        fseek(sf, 0, SEEK_END); sourceSize = ftell(sf); fseek(sf, 0, SEEK_SET);
                        std::vector<uint8_t> buf(4096);
                        size_t got;
                        while ((got = fread(buf.data(), 1, buf.size(), sf)) > 0)
                            sourceChecksum = fnv1a_update(sourceChecksum, buf.data(), got);
                        fclose(sf);
                    }
                }
                bool match = sourceChecksum == csIt->second;
                IGN_LOG("[FILE]", "checksum path=\"%s\" source_size=%ld source_checksum=0x%08x guest_checksum=0x%08x -> %s",
                        path.c_str(), sourceSize, sourceChecksum, csIt->second, match ? "MATCH" : "MISMATCH");
                g_readChecksum().erase(csIt);
            }

            wx86_files().erase(it);
            wx86_file_paths().erase(h);
            // iteration-G75, Parte 18: this line was unconditional on
            // every CloseHandle -- named explicitly in this iteration's
            // own "repeated file CloseHandle informational lines" gate
            // list. First 5 + periodic summary, same pattern as every
            // other hot Glide/file call this project has already reduced.
            static uint32_t g75CloseHandleCount = 0;
            static uint64_t g75CloseHandleWindowStartMs = 0;
            ++g75CloseHandleCount;
            if (!kG75LowOverheadRuntimeLog || g75CloseHandleCount <= 5) {
                IGN_LOG("[FILE]", "CloseHandle handle=0x%08x host_fd=%p -> closed/released, active_handles=%u",
                        h, (void*)fd, (unsigned)wx86_files().size());
            } else {
                uint64_t nowMsClose = wx86_now_ms();
                if (g75CloseHandleWindowStartMs == 0) g75CloseHandleWindowStartMs = nowMsClose;
                if (nowMsClose - g75CloseHandleWindowStartMs >= 5000) {
                    IGN_LOG("[G75-CLOSEHANDLE-SUMMARY]", "closehandle_calls_total=%u active_handles=%u",
                            g75CloseHandleCount, (unsigned)wx86_files().size());
                    g75CloseHandleWindowStartMs = nowMsClose;
                }
            }

            // iteration-15 surgical trace: this file's own file-close return
            // path is the last point we get a real trap (and therefore a
            // guaranteed-flushed log line, see log.cpp -- every IGN_LOG call
            // opens/writes/closes the log file, there is no stdio buffering
            // to lose) before control goes back to pure guest code. Scoped
            // to MenuBkg.dat specifically (case-insensitive) rather than
            // every CloseHandle, to keep the log from ballooning for the
            // dozen other files already loading fine.
            bool isMenuBkg = path.size() >= 10;
            if (isMenuBkg) {
                std::string lower = path;
                for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
                isMenuBkg = lower.find("menubkg") != std::string::npos;
            }
            if (isMenuBkg) {
                set_phase(Phase::MenuBkgCloseReturn, nullptr);
                IGN_LOG("[BREADCRUMB]", "1: returned from CloseHandle(MenuBkg.dat) inside the host shim");
                log_guest_context(c, "[MENUBKG]");

                // iteration-16: real-disassembly finding (VA 0x4200E0, reached
                // via 0x4180E7 right after this whole load sequence returns)
                // is `calloc([0x5530E8], 0x0004F1A0)` used WITHOUT a NULL
                // check, then filled via a raw guest MOV loop whose length is
                // the FIXED constant 0x4F1A0 -- independent of what calloc
                // actually returned. If that allocation fails or comes back
                // smaller than 0x4F1A0 bytes, the fill loop overruns it. This
                // logs the ACTUAL headroom in the same HEAP region calloc
                // draws from, right before that call would run, using the
                // existing GuestRegion bookkeeping (no new tracker).
                wx86::GuestRegion& heap = ignition_heap_region();
                uint32_t used = heap.used_bytes();
                // iteration-16 correction: 0x0004F1A0 = 324000 decimal, NOT
                // 325536 -- that was my own hand-arithmetic error in the
                // previous iteration's log text (caught this iteration by
                // recomputing with PowerShell instead of by hand, same
                // discipline as the 0x423FB0/0x45AFB0 fix). No allocator
                // metadata mystery; the number itself was simply wrong.
                constexpr uint32_t kSuspectAllocSize = 0x0004F1A0u;   // = 324000
                IGN_LOG("[HEAPCHECK]", "heap snapshot before the suspect calloc(count,0x4F1A0=%u) at VA 0x004200ED: "
                                        "used=%u bytes, peak=%u bytes, largest_free_block=%u bytes",
                        kSuspectAllocSize, used, heap.peak(), heap.largest_free());

                // iteration-17: read the calloc COUNT argument directly out
                // of guest memory -- a plain passive c.read_u32(), zero risk,
                // no execution-flow change. Static analysis found exactly
                // one write site for this global (VA 0x417EFD, storing some
                // OTHER function's return value) and could not pin down its
                // runtime value without this. 64-bit multiply guards against
                // a wraparound hiding a genuine overflow.
                uint32_t callocCount = c.read_u32(0x005530E8u);
                uint64_t total64 = (uint64_t)callocCount * (uint64_t)kSuspectAllocSize;
                IGN_LOG("[CALLOC-DIAG]", "count=[0x005530E8]=%u  size=0x4F1A0=%u  total=%llu bytes%s",
                        callocCount, kSuspectAllocSize, (unsigned long long)total64,
                        total64 > 0xFFFFFFFFull ? "  *** OVERFLOWS 32-bit size_t ***" : "");
                if (callocCount == 0) {
                    IGN_LOG("[CALLOC-DIAG]", "count=0 -- calloc(0,N) is legitimately allowed to return NULL or a "
                                              "zero-length block; the guest's own fill loop afterward writes a FIXED "
                                              "0x4F1A0 bytes regardless of what calloc actually returned, so THIS is "
                                              "the most likely overflow trigger if it holds true here");
                } else if (callocCount != 1) {
                    IGN_LOG("[CALLOC-DIAG]", "count=%u (not 1) -- the guest's fill loop still only writes a FIXED "
                                              "0x4F1A0 bytes, not count*0x4F1A0, so a count>1 allocation would be "
                                              "UNDER-filled (harmless) while a too-small/failed allocation would "
                                              "overflow exactly as with count=0", callocCount);
                }

                IGN_LOG("[BREADCRUMB]", "2: about to return control to guest -- everything after this line, if the "
                                          "process dies, executed entirely in translated guest code with no further "
                                          "host shim call (confirmed: no mechanism exists to trace that safely, see "
                                          "log_guest_context()'s comment)");
            }
        }
        // Silent for non-file handles on purpose -- CloseHandle is also the
        // real API for sync objects/other fake handles this project hands
        // out (0x8600_/0x8700_ ranges); those aren't tracked here and were
        // never meant to be, so this isn't a missed case.
        return 1;
    });

    // iteration-17: HeapAlloc visibility for the calloc-path investigation.
    // NOT a reimplementation -- this replicates win32_shims_memory.cpp's own
    // HeapAlloc body VERBATIM (`n=c.arg(2), a=g_plan.heap->alloc(n); if(a &&
    // (c.arg(1)&0x8u)) gzero(c,a,n); return a;`, read directly from its
    // source), using the SAME ignition_heap_region() GuestRegion the engine
    // shim itself draws from (win32_shims_memory.cpp's g_plan.heap is that
    // exact object, wired in ignition_boot.cpp). Zero behavioral difference,
    // installed after win32_shims_memory_install() so "last registration
    // wins" makes this the one that sticks (same proven pattern as
    // PostQuitMessage in user32_window.cpp) -- purely additive logging,
    // answers "does calloc's internal allocator ever reach real HeapAlloc,
    // and with what size" empirically instead of guessing.
    // iteration-21: scoped back down to JUST the suspect size (324000,
    // = 0x0004F1A0, calloc's own aligned request -- see the [CALLOC-DIAG]
    // comment above). Every other HeapAlloc call takes the plain, silent,
    // engine-identical path (same 2 lines as win32_shims_memory.cpp) with NO
    // extra host-side memory touches -- iteration 20's per-allocation
    // canary writes were real overhead across every asset load (contributing
    // to the multi-minute wall-clock time to reach the crash), not just log
    // volume, and its methodology was invalidated by real hardware evidence
    // anyway (see the removed-mechanism comment above). Diagnostics for
    // n==324000 stay: the range confirmation and R/W probe are still valid,
    // useful checks (they don't rely on memory staying untouched afterward).
    K("HeapAlloc", 3, [](Cpu& c) -> uint32_t {
        uint32_t hHeap = c.arg(0), flags = c.arg(1), n = c.arg(2);
        (void)hHeap;
        wx86::GuestRegion& heap = ignition_heap_region();
        uint32_t a = heap.alloc(n);
        if (a && (flags & 0x8u)) wx86_gzero(c, a, n);   // HEAP_ZERO_MEMORY, same as the engine shim

        // iteration-G55, Parte 4-9: G54's [G54-70000018] capture pinned the
        // guest-only region containing the write to a very narrow window --
        // last_bridge_eip=this_bridge_eip=0x0045E810, which disassembly
        // identifies as the return address immediately after
        // `call DWORD PTR ds:0x46a0a0` (the real HeapAlloc IAT slot) inside
        // Ignition's own CRT malloc()-style wrapper at 0x0045e7dc-0x0045e811
        // -- i.e. the two most recent trapped calls were both HeapAlloc,
        // from this exact guest call site, with flags=0 (no
        // HEAP_ZERO_MEMORY -- confirmed by this project's own disassembly
        // of the call site, `push 0x0` before the call). This narrow,
        // caller-EIP-scoped probe (same pattern as the existing n==324000
        // diagnostic just above, not a new mechanism) tests directly
        // whether THIS call site's own allocations land at/near the
        // failing object's address (0x01039DC0) -- if so, that is strong
        // evidence the field in question is simply uninitialized,
        // freshly-allocated heap memory (real HeapAlloc without
        // HEAP_ZERO_MEMORY returns whatever was already there), not an
        // explicit guest write of a texture handle into an unrelated
        // field. Capped at 4 logged occurrences, no per-call spam.
        // iteration-G58, Parte 4/33: G55's [G55-MALLOC] probe is now
        // superseded -- it only proved the call site (0x0045E810), which
        // G56-G57 already refined into the exact generation-31 signature
        // below. Disabled per this iteration's own "disable superseded
        // G55/G56 spam" instruction (kept as dead code intentionally, not
        // deleted, matching this project's established kGxxToggle pattern
        // so a future iteration can re-enable it without re-deriving why
        // it existed).
        constexpr bool kG55MallocSiteProbeEnabled = false;
        if (kG55MallocSiteProbeEnabled) {
            static uint32_t g_g55HeapAllocSiteCount = 0;
            uint32_t g55CallerEip = c.read_u32(c.reg(R_ESP));
            if (g55CallerEip == 0x0045E810u && g_g55HeapAllocSiteCount < 4) {
                ++g_g55HeapAllocSiteCount;
                IGN_LOG("[G55-MALLOC]", "call#%u caller_eip=0x0045E810 size=%u flags=0x%08x -> guest_ptr=0x%08x "
                                        "range=0x%08x..0x%08x (target_field=0x01039DD8 in_range=%s)",
                        g_g55HeapAllocSiteCount, n, flags, a, a, a + n,
                        (a != 0 && 0x01039DD8u >= a && 0x01039DD8u < a + n) ? "YES" : "NO");
            }
        }

        // iteration-G56, Parte 6-8 (REDUCED per G57 Part 28-29): target-
        // range lifetime tracking, still armed for the ENTIRE run and
        // still no "first N" cap -- but now that G56's own real hardware
        // run identified generations 8-30 as plain, well-understood
        // size==4096 ReadFile-scratch churn at this address, that class is
        // no longer logged (bookkeeping below still runs unconditionally,
        // so [G56-OBJECT-HEAP]'s live-generation answer stays accurate --
        // only the noisy per-generation log lines are suppressed for the
        // already-understood case).
        if (a != 0 && ((kG56TargetAddrA >= a && kG56TargetAddrA < a + n) ||
                       (kG56TargetAddrB >= a && kG56TargetAddrB < a + n))) {
            uint32_t g56CallerEip = c.read_u32(c.reg(R_ESP));
            bool isReuse = (a == g_g56TargetAllocBase && g_g56TargetGeneration > 0 && !g_g56TargetLive);
            uint32_t oldGeneration = g_g56TargetGeneration;
            uint32_t oldSize = g_g56TargetAllocSize;
            ++g_g56TargetGeneration;
            g_g56TargetAllocBase = a;
            g_g56TargetAllocSize = n;
            g_g56TargetAllocCallerEip = g56CallerEip;
            g_g56TargetLive = true;
            bool isBoringFileScratch = (n == 4096u);
            if (isReuse && !isBoringFileScratch) {
                IGN_LOG("[G56-TARGET-REUSE]", "base=0x%08x old_generation=%u new_generation=%u old_size=%u new_size=%u alloc_caller=0x%08x",
                        a, oldGeneration, g_g56TargetGeneration, oldSize, n, g56CallerEip);
            } else if (!isBoringFileScratch) {
                IGN_LOG("[G56-TARGET-ALLOC]", "caller_eip=0x%08x base=0x%08x size=%u flags=0x%08x generation=%u",
                        g56CallerEip, a, n, flags, g_g56TargetGeneration);
            }

            // iteration-G57, Parte 5/11: this run's own hardware log
            // identified generation 31 by its exact signature
            // (base=0x01039CA0, size=0x850) -- when THIS SPECIFIC
            // allocation recurs, capture a stack-scan of caller candidates
            // (Part 5/6: the malloc wrapper chain at 0x0045e79e-0x0045e811
            // has no linked frame pointer, so the game's own call site is
            // several fixed-size stdcall frames above the immediate
            // return address 0x0045E810 -- rather than risk a hand-
            // computed offset being wrong again, as this project's own
            // G49 mistake already demonstrated can happen, this dumps
            // several candidate words above ESP tagged by whether they
            // fall inside the PE image's own code range, Part 6's
            // "static candidates" fallback) and snapshots the field's
            // value at the moment of allocation (Part 19's own old_raw),
            // then arms the dinput.cpp poll (g_g57Gen31Armed) to catch
            // the FIRST subsequent change to that field with much less
            // noise than watching from process start (G53's own lesson).
            // iteration-G63, Parte 35: G62's real hardware run closed the
            // record+0x18 provenance mystery for good (table_base=
            // 0x00490590 measured directly, entry23=0x70000018, confirmed
            // by static disassembly to be TEX24's own handle -- see this
            // file's guDrawTriangleWithClip_impl G63 comment for the full
            // proof). The entire G57/G59/G61/G62 provenance-forensics
            // chain below is retired -- kept as dead code, not deleted,
            // matching this project's established toggle pattern, so a
            // future iteration investigating a DIFFERENT record/object
            // can re-arm it without re-deriving why it existed. This is
            // now a renderer-semantics phase, not a data-provenance phase.
            constexpr bool kG57Gen31DiagnosticsEnabled = false;
            if (kG57Gen31DiagnosticsEnabled && n == kG57Gen31Size && a == kG56TargetAddrA - 0x120u) {
                uint32_t fieldNow = c.read_u32(a + 0x138u);
                g_g57Gen31FieldSnapshotAtAlloc = fieldNow;
                g_g57Gen31Armed = true;
                IGN_LOG("[G57-GEN31-ALLOC]", "caller_eip=0x%08x base=0x%08x size=0x%x field_at_alloc(+0x138)=0x%08x",
                        g56CallerEip, a, n, fieldNow);
                uint32_t esp = c.reg(R_ESP);   // used below for the T+0x28/T+0x44/T+0x58 stack-forensics reads

                // iteration-G61, Parte 33: the raw 16-word stack-scan
                // already did its job (G59/G60 fully explained every one
                // of those candidates by disassembly) -- disabled per
                // this iteration's own "logging must be minimal" request,
                // kept as dead code rather than deleted (same kGxxToggle
                // pattern as kG55MallocSiteProbeEnabled above) so a future
                // iteration investigating a DIFFERENT internal branch of
                // 0x00457E60 can re-enable it without re-deriving why it
                // existed.
                constexpr bool kG57StackScanEnabled = false;
                if (kG57StackScanEnabled) {
                    uint32_t espDbg = c.reg(R_ESP);
                    for (int i = 1; i <= 16; ++i) {
                        uint32_t val = c.read_u32(espDbg + (uint32_t)i * 4);
                        bool looksLikeCode = (val >= 0x00400000u && val < 0x0066E000u);
                        IGN_LOG("[G57-STACK-SCAN]", "esp+0x%02x = 0x%08x %s",
                                i * 4, val, looksLikeCode ? "(PE_CODE_RANGE candidate)" : "");
                    }
                }

                // iteration-G58, Parte 1/5/7-9: the stack-scan candidates
                // above are now fully explained by disassembly, not left
                // as raw numbers -- esp+0x14=0x0045E7C0 is the return
                // address inside the malloc retry-loop wrapper
                // (0x0045e7b0's own call to 0x0045e7dc at 0x0045e7bb);
                // esp+0x1C=0x0045E7AD is the return address inside the
                // malloc entry point (0x0045e79e's own call to 0x0045e7b0
                // at 0x0045e7a8); esp+0x28=0x00457EF9 is the return
                // address inside the REAL caller -- a generic named-
                // resource cache/registry function at 0x00457E60 (hash
                // table with 64 buckets at ds:0x00620440, up to 16
                // entries per bucket, allocates a caller-supplied size
                // only when the requested key is NOT already cached; the
                // call itself is at 0x00457ef4). The size mismatch this
                // iteration's own request flagged (0x850 requested vs.
                // 0x84C echoed on the stack) is now fully resolved as
                // ordinary 16-byte allocator rounding, NOT a header+4
                // relationship: 0x84C=2124 is the real caller-requested
                // size (visible at stack esp+0x18/esp+0x20, both echoing
                // the SAME pushed argument at different call depths);
                // (0x84C+15)&~15 = 0x850=2128 is this project's own
                // GuestRegion::alloc()'s 16-byte-aligned block size --
                // exactly matching its own documented alignment
                // contract, not a bug, and not evidence of a header
                // field. iteration-G59, Parte 5/9/11/36: G58's own real
                // hardware run showed ds:0x00536f78 reads back as 0 AFTER
                // the allocation -- and disassembly now shows why that
                // probe was the wrong approach entirely (Part 36): arg0
                // at 0x00457E60's OWN entry is used DIRECTLY as
                // `[eax*4+0x00620440]`, an array index with NO hashing
                // visible -- meaning it is already a small integer
                // BUCKET INDEX (0..63), not a resource-name pointer. 0 is
                // therefore a perfectly valid bucket index (bucket #0),
                // not evidence of an empty/failed read -- G58's own
                // "looks_like_ptr" framing was built on a wrong premise
                // and is retired.
                //
                // Rather than read anything AFTER the allocation (too
                // late, Part 9), this retroactively reconstructs
                // 0x00457E60's OWN entry-time state from ITS OWN saved
                // registers, which remain sitting on the guest stack,
                // unmodified, for as long as 0x00457E60 has not yet
                // returned -- true for the entire duration of the nested
                // malloc call this shim is currently trapping. 0x00457E60
                // pushes ecx/ebx/ebp/esi/edi (5 words) at its own entry
                // with no additional `sub esp,N`; from THIS run's already-
                // confirmed call path (esp+0x28=0x00457EF9, i.e. the
                // 0x00457eef->0x00457ef4 branch, "existing bucket, empty
                // inner slot"), the arithmetic is: 0x00457e60's own entry
                // esp (call it H) satisfies H = (this shim's own esp) +
                // 0x44, derived by walking every push/call/cleanup from H
                // down to the traced malloc call, cross-checked against
                // every already-confirmed stack value (T+0x14/0x18/0x1C/
                // 0x20/0x28 all matched exactly in G57/G58). [H+0] is the
                // return address into 0x00457E60's OWN caller -- the
                // actual answer to "which of the ~60+ call sites fired"
                // that a post-hoc global read could never give. Guarded
                // by re-checking the T+0x28 signature first (Part 6's own
                // "use disassembly to validate return addresses, do not
                // rely only on raw stack scanning") -- if it does not
                // match, this run took a DIFFERENT internal branch
                // (e.g. 0x00457f13's "brand-new bucket" path) and the
                // H=T+0x44 arithmetic would not apply, so this logs that
                // mismatch honestly instead of printing wrong data.
                {
                    uint32_t t28 = c.read_u32(esp + 0x28u);
                    if (t28 == 0x00457EF9u) {
                        uint32_t hEntry = esp + 0x44u;
                        uint32_t callerReturn = c.read_u32(hEntry + 0x00u);
                        uint32_t bucketIndexArg = c.read_u32(hEntry + 0x04u);
                        uint32_t sizeArg = c.read_u32(hEntry + 0x08u);
                        IGN_LOG("[G59-CACHE-ENTRY]", "caller_return=0x%08x bucket_index_arg=0x%08x(%u) size_arg=0x%08x(%u) "
                                                     "(H=T+0x44, validated via T+0x28==0x00457EF9 -- the 0x00457eef->0x00457ef4 branch)",
                                callerReturn, bucketIndexArg, bucketIndexArg, sizeArg, sizeArg);

                        // iteration-G60, Parte 2-11: G59's own real hardware
                        // run confirmed caller_return=0x004491EA, which
                        // disassembly identifies precisely as the return
                        // address for `call 0x00457E60` at 0x004491E5,
                        // inside a resource-format parser dispatching on a
                        // 4-byte chunk magic (0x3330696C="li03") -- the
                        // enclosing function starts at 0x004491C0 and, right
                        // after the cache allocation, loops building an
                        // array of 36-byte (0x24) records at the returned
                        // payload base; record #N's fields come from 7
                        // consecutive 16-bit values read from the resource's
                        // OWN input data stream (arg0+8), and record+0x18 is
                        // populated via `mov ecx,[ebp+ebx*4]` at 0x00449299
                        // (writer: `mov [edi+edx+0x18],ecx` at 0x0044929D) --
                        // ebp is this function's OWN arg2, passed as the
                        // FIXED LITERAL 0x004a2474 by the "li03" dispatcher
                        // (`push 0x4a2474` at 0x00448e8b) -- and ebx is the
                        // 7th raw 16-bit value read directly from the input
                        // stream for that record, used completely
                        // UNVALIDATED as a table index (no bounds mask). An
                        // unrestricted disassembly search finds no
                        // instruction anywhere that ever WRITES to
                        // 0x004a2474's table region -- it is very likely
                        // static, compile-time PE data, not a runtime-
                        // populated texture-handle table.
                        //
                        // Our failing object (0x01039DC0) is exactly
                        // allocation_base+0x120, and 0x120/0x24=8 exactly --
                        // record #8 (0-indexed), confirming the 36-byte
                        // stride and this exact producer function by
                        // arithmetic, not assumption. This lets the raw
                        // input value AND the resulting table read be
                        // recovered retroactively, BEFORE the loop even
                        // executes, the same stack-forensics technique used
                        // for [G59-CACHE-ENTRY] above: 0x004491C0's own true
                        // entry esp (call it K) is H+20 (2 register pushes
                        // + 2 pushed call args + the call's own return
                        // address, all counted from its `call 0x00457E60`
                        // backward) = T+0x58. [K+0] is the return address
                        // into the "li03" dispatcher (expected 0x00448E97,
                        // used here as a second, independent self-check);
                        // [K+4] is arg0, the resource struct pointer.
                        //
                        // iteration-G61, Parte 4/10/16-17: G60's real
                        // hardware run exposed a contradiction -- this
                        // formula predicted table[25]=0, but the field's
                        // real observed value was 0x70000018. Re-deriving
                        // the per-record INPUT stride byte-for-byte (per
                        // this iteration's own Part 17 instruction, "verify
                        // exact machine bytes, do not rely on an earlier
                        // textual disassembly") finds the bug: G60 assumed
                        // 7 input words (14 bytes) per record, but the loop
                        // tail (0x00449382 `add edi,0x24` immediately
                        // preceded by an 8TH `add esi,2` at 0x0044937F with
                        // NO corresponding read instruction anywhere in the
                        // loop body) proves each record actually consumes
                        // 8 input words (16 bytes) -- the 8th word is read
                        // past (a skipped/reserved field the parser never
                        // uses) but still consumes stream position. G60's
                        // formula was off by 8*(16-14)=16 bytes for record
                        // #8 specifically, reading the WRONG record's data
                        // entirely. Corrected: record #8's raw table index
                        // sits at (arg0+8) + 8*16 + 12 = arg0+0x94.
                        uint32_t kEntry = esp + 0x58u;
                        uint32_t liDispatcherReturn = c.read_u32(kEntry + 0x00u);
                        if (liDispatcherReturn == 0x00448E97u) {
                            uint32_t resourceStructPtr = c.read_u32(kEntry + 0x04u);
                            uint32_t rawIndexAddr = resourceStructPtr + 0x94u;   // G61 fix: was 0x84 (wrong 14-byte stride)
                            uint32_t rawIndex = c.read_u32(rawIndexAddr) & 0xFFFFu;

                            // iteration-G62, Parte 20: re-verifying the exact
                            // push order at the "li03" call site
                            // (0x00448e8b: push 0x4a2474; 0x00448e90: push
                            // edi; 0x00448e91: push esi; 0x00448e92: call
                            // 0x004491c0) against the STDCALL rule this
                            // project has used consistently since G39 (last
                            // push = first accessed via [esp+4] at entry)
                            // finds a genuine SECOND self-caught error: EBP
                            // inside 0x004491C0 is loaded via
                            // `mov ebp,[esp+0x14]` at 0x00449205, which
                            // resolves to arg1 (edi from the CALLER), NOT
                            // arg2 (the literal 0x004A2474) as G60/G61
                            // assumed. The literal 0x004A2474 is arg2 --
                            // used by something else inside this function,
                            // not the table-lookup base. This directly
                            // explains why the static prediction against a
                            // hardcoded 0x004A2474 kept disagreeing with
                            // real hardware: the wrong base address was
                            // being read the whole time. It also retracts
                            // G60's "zero writes to 0x004A2474" claim --
                            // 0x00448E12-0x00448E2C DOES write there (a
                            // 16-entry copy loop from a caller-supplied
                            // list), just not via the literal-displacement
                            // pattern the earlier grep searched for -- but
                            // since EBP isn't actually 0x004A2474, that
                            // copy loop's relevance is now separately
                            // unresolved rather than ruled in or out.
                            //
                            // The REAL ebp (arg1) is read directly here --
                            // measured, not assumed a second time -- at
                            // [K+8] (K=kEntry), matching the same
                            // stdcall-slot arithmetic already validated by
                            // the K+0/K+4 self-checks above.
                            uint32_t realEbp = c.read_u32(kEntry + 0x08u);
                            uint32_t tableEntryAddr = realEbp + rawIndex * 4u;
                            uint32_t tableEntryValue = c.read_u32(tableEntryAddr);
                            IGN_LOG("[G62-CALLER]", "li_dispatcher_return=0x%08x resource_struct_ptr=0x%08x real_ebp_arg1=0x%08x "
                                                    "(validated via K=T+0x58, [K+0]==0x00448E97; ebp CORRECTED to arg1 -- "
                                                    "G60/G61 wrongly used arg2's literal 0x004A2474)",
                                    liDispatcherReturn, resourceStructPtr, realEbp);
                            IGN_LOG("[G62-PAYLOAD]", "record8_base=alloc+0x120 record8_raw_index_addr=0x%08x "
                                                     "raw_index=0x%04x(%u) lookup_table_base=0x%08x(real_ebp, NOT 0x004A2474) "
                                                     "table_entry_addr=0x%08x table_entry_value=0x%08x "
                                                     "(this is record+0x18's value BEFORE the write executes -- "
                                                     "compare against [G57-FIELD-WRITE]'s new_raw for the same run)",
                                    rawIndexAddr, rawIndex, rawIndex, realEbp, tableEntryAddr, tableEntryValue);
                        } else {
                            IGN_LOG("[G62-CALLER]", "OFFSET_ASSUMPTION_MISMATCH: K+0=0x%08x (expected 0x00448E97) -- "
                                                    "not printing unverified resource_struct_ptr/record data", liDispatcherReturn);
                        }
                    } else {
                        IGN_LOG("[G59-CACHE-ENTRY]", "OFFSET_ASSUMPTION_MISMATCH: T+0x28=0x%08x (expected 0x00457EF9) -- "
                                                     "this run took a different internal branch of 0x00457E60 than the one "
                                                     "the H=T+0x44 arithmetic was derived for; not printing unverified data",
                                t28);
                    }
                }
            }
        }

        if (n != 324000u) return a;

        uint32_t callerEip = c.read_u32(c.reg(R_ESP));
        IGN_LOG("[HEAPALLOC]", "caller_eip=0x%08x size=%u flags=0x%08x -> guest_ptr=0x%08x (heap used=%u peak=%u)",
                callerEip, n, flags, a, heap.used_bytes(), heap.peak());
        if (!a) {
            IGN_LOG("[HEAPALLOC]", "  ALLOCATION FAILED (NULL) -- largest_free_block=%u", heap.largest_free());
            return a;
        }
        uint32_t end = a + n;
        IGN_LOG("[HEAPALLOC]", "  range=0x%08x..0x%08x (%u bytes, %u pages, align4=%u align16=%u) -- fully "
                                 "inside the single contiguous HEAP mapping, no unmapped gap is possible here",
                a, end, n, (n + 0xFFFu) >> 12, a % 4, a % 16);

        // Real read/write probe (unchanged from iteration 19): touches the
        // actual host memory backing the first and last dword, restores the
        // original bytes after, zero guest-visible effect.
        uint32_t firstBefore = c.read_u32(a);
        uint32_t lastBefore = c.read_u32(end - 4);
        c.write_u32(a, 0xCAFEBABEu);
        c.write_u32(end - 4, 0xCAFEBABEu);
        uint32_t firstReadback = c.read_u32(a);
        uint32_t lastReadback = c.read_u32(end - 4);
        c.write_u32(a, firstBefore);
        c.write_u32(end - 4, lastBefore);
        bool probeOk = (firstReadback == 0xCAFEBABEu) && (lastReadback == 0xCAFEBABEu);
        IGN_LOG("[HEAPALLOC]", "  R/W probe: first_dword %s, last_dword %s%s",
                firstReadback == 0xCAFEBABEu ? "OK" : "FAILED",
                lastReadback == 0xCAFEBABEu ? "OK" : "FAILED",
                probeOk ? "" : "  *** MEMORY NOT ACTUALLY WRITABLE DESPITE BOOKKEEPING ***");
        return a;
    });

    // iteration-G56, Parte 6/9/24: HeapFree was previously left to the
    // vendored engine's own default (external/WinVita's
    // win32_shims_memory.cpp, `g_plan.heap->free(c.arg(2))`) -- NOT edited
    // (still forbidden without authorization); this registers AFTER that
    // default (same "last registration wins" pattern already used for
    // HeapAlloc above and PostQuitMessage in user32_window.cpp), replicating
    // the real body verbatim and adding ONLY target-block lifetime logging
    // plus a free-validity check (Part 24: exact-base vs. interior-pointer
    // vs. unknown-pointer), never changing free's own behavior.
    K("HeapFree", 3, [](Cpu& c) -> uint32_t {
        uint32_t ptr = c.arg(2);
        wx86::GuestRegion& heap = ignition_heap_region();
        if (ptr == g_g56TargetAllocBase && g_g56TargetLive) {
            // iteration-G57, Parte 28-29: reduced -- the size==4096
            // ReadFile-scratch generations (8-30) are now understood, so
            // only a free of the generation-31-shaped allocation (or any
            // non-4096-byte one) is worth a log line.
            if (g_g56TargetAllocSize != 4096u) {
                uint32_t g56FreeCallerEip = c.read_u32(c.reg(R_ESP));
                IGN_LOG("[G56-TARGET-FREE]", "caller_eip=0x%08x base=0x%08x size=%u generation=%u",
                        g56FreeCallerEip, ptr, g_g56TargetAllocSize, g_g56TargetGeneration);
            }
            g_g56TargetLive = false;
        } else if (ptr != 0) {
            // Part 24: exact-base vs. interior vs. unknown, logged once
            // per anomaly type only if it ever affects a block that has
            // EVER overlapped our two target addresses (g_g56TargetAllocBase
            // != 0) -- not a global free-validity trace.
            uint32_t blockBase = heap.block_of(ptr);
            if (g_g56TargetAllocBase != 0 && blockBase == g_g56TargetAllocBase && ptr != blockBase) {
                static bool g_g56InteriorFreeLogged = false;
                if (!g_g56InteriorFreeLogged) {
                    g_g56InteriorFreeLogged = true;
                    IGN_LOG("[G56-FREE-ANOMALY]", "interior_pointer_free ptr=0x%08x block_base=0x%08x", ptr, blockBase);
                }
            }
        }
        heap.free(ptr);
        return 1u;   // real body, verbatim (see win32_shims_memory.cpp: return value is NOT free()'s result)
    });

    K("GetFileSize", 2, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0), pHi = c.arg(1);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) return 0xFFFFFFFFu;
        FILE* f = it->second;
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, cur, SEEK_SET);
        if (pHi) c.write_u32(pHi, 0);
        IGN_LOG("[FILE]", "GetFileSize handle=0x%08x -> %ld", h, sz);
        return (uint32_t)sz;
    });

    K("SetFilePointer", 4, [](Cpu& c) -> uint32_t {
        uint32_t h = c.arg(0);
        int32_t dist = (int32_t)c.arg(1);
        uint32_t method = c.arg(3);
        ring_record(c, "SetFilePointer", h, (uint32_t)dist, method);
        auto it = wx86_files().find(h);
        if (it == wx86_files().end()) {
            IGN_LOG("[FILE]", "SetFilePointer(bad handle 0x%08x)", h);
            return 0xFFFFFFFFu;
        }
        long before = ftell(it->second);
        int whence = method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET;
        const char* methodName = method == 1 ? "FILE_CURRENT" : method == 2 ? "FILE_END" : "FILE_BEGIN";
        uint64_t g49SeekStartUs = wx86_now_us();
        fseek(it->second, dist, whence);
        g_g49FileSeekTotalUs += (wx86_now_us() - g49SeekStartUs);
        long after = ftell(it->second);
        if (kVerboseFileIoDiagnostics) {
            uint64_t g74DiagStartUs = wx86_now_us();
            IGN_LOG("[FILE]", "SetFilePointer handle=0x%08x distance=%d method=%s old_offset=%ld new_offset=%ld",
                    h, dist, methodName, before, after);
            g_g74FileDiagLogUs += (wx86_now_us() - g74DiagStartUs);
        }
        g74_track_load_heartbeat(c.read_u32(c.reg(R_ESP)));
        return (uint32_t)after;
    });

    K("DeleteFileA", 1, [](Cpu& c) -> uint32_t {
        std::string guestPath = read_guest_cstr(c, c.arg(0));
        IGN_LOG("[WIN32]", "DeleteFileA(\"%s\") -> honest failure (not implemented yet, nothing deleted)",
                guestPath.c_str());
        return 0;
    });

    // iteration-G24: HANDLE FindFirstFileA(LPCSTR lpFileName,
    // LPWIN32_FIND_DATAA lpFindFileData) -- stdcall, argc=2 (confirmed by
    // disassembling the one real call site this iteration's hardware log
    // pointed at, caller_guest_eip=0x0045f670: `push eax(lpFindFileData) /
    // push [ebp+8](lpFileName) / call [IAT 0x46a0c8]` at 0x45f65f-0x45f66a,
    // exactly 2 pushes, return value used directly as a HANDLE compared
    // against 0xFFFFFFFF -- that caller is the CRT's own _findfirst-style
    // wrapper, not Ignition's game code directly, and afterward calls
    // GetLastError (already implemented by the engine, wx86_get/set_lasterr)
    // to map the failure into errno -- so a real ERROR_FILE_NOT_FOUND /
    // ERROR_PATH_NOT_FOUND here matters, not just a nonzero/zero return.
    //
    // Path mapping reuses ignition_paths.h's translate_path() UNCHANGED --
    // translate_path() only does string substitution, so calling it on the
    // full guest pattern (including the wildcard filename component) is
    // safe and gives the full host path with the wildcard still attached at
    // the end; splitting that on the last '/' yields the real host
    // directory to open plus the pattern to match filenames against. No
    // parallel/duplicate path mapper was written.
    //
    // Wildcards: libc's fnmatch() was tried first but vitasdk's libc.a does
    // not actually implement it (link error, confirmed via nm) despite the
    // header existing -- wildcard_match_ci() is the minimal case-insensitive
    // '*'/'?' backtracking matcher used instead, wrapped by
    // win32_wildcard_match() which special-cases "*.*" to match everything
    // (documented Win32 legacy-8.3 semantics, NOT the same as POSIX glob --
    // see its own comment; this is what G25 found and fixed after G24's
    // "baltazar\data\sound\*.*" returned 0 matches against a real,
    // non-empty directory whose only entry, "0_grp0", has no '.' in it).
    //
    // WIN32_FIND_DATAA layout (32-bit, standard, no padding gaps -- every
    // field is naturally DWORD-aligned): dwFileAttributes@0(4),
    // ftCreationTime@4(8), ftLastAccessTime@12(8), ftLastWriteTime@20(8),
    // nFileSizeHigh@28(4), nFileSizeLow@32(4), dwReserved0@36(4),
    // dwReserved1@40(4), cFileName@44(260), cAlternateFileName@304(14) --
    // total 318 bytes. Timestamps are zero-filled (not tracked on this
    // filesystem) -- Part G explicitly allows this if the guest doesn't
    // need them; nothing here is faked as a real timestamp.
    K("FindFirstFileA", 2, [](Cpu& c) -> uint32_t {
        uint32_t esp = c.reg(R_ESP);
        uint32_t callerEip = c.read_u32(esp);
        uint32_t pFileName = c.arg(0);
        uint32_t pFindData = c.arg(1);
        std::string guestPattern = read_guest_cstr(c, pFileName);

        IGN_LOG("[G24]", "FindFirstFileA entry: pattern_guest=\"%s\" lpFindFileData=0x%08x caller_guest_eip=0x%08x guest_esp=0x%08x",
                guestPattern.c_str(), pFindData, callerEip, esp);
        IGN_LOG("[G24]", "stack_check: entry_esp=0x%08x argc_dwords=2 expected_esp_after_return=0x%08x (entry_esp+12) return_target=0x%08x",
                esp, esp + 12, callerEip);

        std::string hostFull = translate_path(guestPattern.c_str());
        size_t slash = hostFull.find_last_of('/');
        std::string hostDir = (slash == std::string::npos) ? std::string(kDataRoot) : hostFull.substr(0, slash);
        std::string filePattern = (slash == std::string::npos) ? hostFull : hostFull.substr(slash + 1);
        IGN_LOG("[G24]", "FindFirstFileA: mapped_dir=\"%s\" file_pattern=\"%s\"", hostDir.c_str(), filePattern.c_str());

        // iteration-G25, Parte A/B: raw directory audit BEFORE any matching,
        // so a real-hardware run distinguishes the three possible causes of
        // "0 entries matched" without conflating them: (A) the directory
        // itself doesn't exist (opendir fails), (B) it exists but is
        // genuinely empty (entry_count_raw==0), or (C) it has real entries
        // that the matcher is failing to match (entry_count_raw>0 but
        // windows_matches==0). This logging is unconditional and changes no
        // behavior by itself.
        DIR* d = opendir(hostDir.c_str());
        IGN_LOG("[G25]", "directory_exists=%s dir=\"%s\"", d ? "YES" : "NO", hostDir.c_str());
        if (!d) {
            IGN_LOG("[G24]", "FindFirstFileA: opendir(\"%s\") failed -> INVALID_HANDLE_VALUE, ERROR_PATH_NOT_FOUND",
                    hostDir.c_str());
            wx86_set_lasterr(c, 3u);   // ERROR_PATH_NOT_FOUND
            return 0xFFFFFFFFu;
        }
        std::vector<FindEntry> matches;
        uint32_t rawCount = 0;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            ++rawCount;
            IGN_LOG("[G25]", "raw_dir_entry=\"%s\" is_dir=%s size=%lld",
                    de->d_name, SCE_S_ISDIR(de->d_stat.st_mode) ? "YES" : "NO", (long long)de->d_stat.st_size);
            if (!win32_wildcard_match(filePattern, de->d_name)) continue;
            matches.push_back(FindEntry{ de->d_name, (uint64_t)de->d_stat.st_size, (bool)SCE_S_ISDIR(de->d_stat.st_mode) });
        }
        closedir(d);
        IGN_LOG("[G25]", "entry_count_raw=%u windows_matches=%u pattern=\"%s\"",
                rawCount, (unsigned)matches.size(), filePattern.c_str());
        IGN_LOG("[G24]", "FindFirstFileA: %u entr%s matched \"%s\" in \"%s\"",
                (unsigned)matches.size(), matches.size() == 1 ? "y" : "ies", filePattern.c_str(), hostDir.c_str());

        if (matches.empty()) {
            const char* reason = (rawCount == 0) ? "directory genuinely empty" : "matcher rejected all raw entries";
            IGN_LOG("[G25]", "FindFirstFileA -> INVALID_HANDLE_VALUE (no match, %s), ERROR_FILE_NOT_FOUND", reason);
            wx86_set_lasterr(c, 2u);   // ERROR_FILE_NOT_FOUND
            return 0xFFFFFFFFu;
        }

        uint32_t handle = next_find_handle();
        FindHandleState st;
        st.entries = matches;
        st.index = 1;   // entry 0 delivered below; FindNextFileA (G26) resumes from here
        st.guestPattern = guestPattern;
        st.hostDir = hostDir;
        g_findHandles()[handle] = st;

        const FindEntry& first = matches[0];
        uint32_t attr = write_find_data(c, pFindData, first);

        IGN_LOG("[G24]", "FindFirstFileA:");
        IGN_LOG("[G24]", "  pattern=\"%s\"", guestPattern.c_str());
        IGN_LOG("[G24]", "  mapped_path=\"%s/%s\"", hostDir.c_str(), filePattern.c_str());
        IGN_LOG("[G24]", "  match=\"%s\"", first.name.c_str());
        IGN_LOG("[G24]", "  size=%llu", (unsigned long long)first.size);
        IGN_LOG("[G24]", "  attributes=0x%08x (%s)", attr, first.isDir ? "FILE_ATTRIBUTE_DIRECTORY" : "FILE_ATTRIBUTE_NORMAL");
        IGN_LOG("[G24]", "  handle=0x%08x", handle);
        return handle;
    });

    // iteration-G26: BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA
    // lpFindFileData) -- stdcall, argc=2 (8 bytes cleanup), same
    // HANDLE+LPWIN32_FIND_DATAA shape as FindFirstFileA. Uses the EXACT
    // FindHandleState FindFirstFileA already built and stored in
    // g_findHandles() -- no opendir()/re-scan/re-sort here (Part A/D): the
    // entries vector and its order are whatever FindFirstFileA's own
    // readdir() loop produced, walked forward by index only. Shares
    // write_find_data() with FindFirstFileA (Part E/O) instead of
    // duplicating the WIN32_FIND_DATAA-filling logic.
    K("FindNextFileA", 2, [](Cpu& c) -> uint32_t {
        uint32_t esp = c.reg(R_ESP);
        uint32_t callerEip = c.read_u32(esp);
        uint32_t handle = c.arg(0);
        uint32_t pFindData = c.arg(1);
        IGN_LOG("[G26]", "FindNextFileA entry: handle=0x%08x lpFindFileData=0x%08x caller_guest_eip=0x%08x guest_esp=0x%08x",
                handle, pFindData, callerEip, esp);
        IGN_LOG("[G26]", "stack_check: entry_esp=0x%08x argc_dwords=2 expected_esp_after_return=0x%08x (entry_esp+12) return_target=0x%08x",
                esp, esp + 12, callerEip);

        auto it = g_findHandles().find(handle);
        if (it == g_findHandles().end()) {
            IGN_LOG("[G26]", "FindNextFileA(0x%08x) -> FALSE (not a live find-enumeration handle), ERROR_INVALID_HANDLE",
                    handle);
            wx86_set_lasterr(c, 6u);   // ERROR_INVALID_HANDLE
            return 0u;   // FALSE
        }

        FindHandleState& st = it->second;
        if (st.index >= st.entries.size()) {
            // iteration-G26, Parte I/J: end of enumeration is NOT a handle
            // error -- ERROR_NO_MORE_FILES (18), not ERROR_FILE_NOT_FOUND,
            // and the handle/state stays alive until an explicit
            // FindClose(), exactly like real Win32.
            IGN_LOG("[G26]", "FindNextFileA(0x%08x) -> FALSE index=%u/%u (end of enumeration), ERROR_NO_MORE_FILES",
                    handle, (unsigned)st.index, (unsigned)st.entries.size());
            wx86_set_lasterr(c, 18u);   // ERROR_NO_MORE_FILES
            return 0u;   // FALSE
        }

        const FindEntry& e = st.entries[st.index];
        uint32_t attr = write_find_data(c, pFindData, e);
        IGN_LOG("[G26]", "FindNextFileA:");
        IGN_LOG("[G26]", "  handle=0x%08x", handle);
        IGN_LOG("[G26]", "  index=%u/%u", (unsigned)(st.index + 1), (unsigned)st.entries.size());
        IGN_LOG("[G26]", "  match=\"%s\"", e.name.c_str());
        IGN_LOG("[G26]", "  size=%llu", (unsigned long long)e.size);
        IGN_LOG("[G26]", "  attributes=0x%08x (%s)", attr, e.isDir ? "FILE_ATTRIBUTE_DIRECTORY" : "FILE_ATTRIBUTE_NORMAL");
        IGN_LOG("[G26]", "  return=TRUE");
        ++st.index;
        return 1u;   // TRUE
    });

    // iteration-G25, Parte G/H/I/J: BOOL FindClose(HANDLE hFindFile) --
    // stdcall, argc=1 (4 bytes cleanup), standard single-HANDLE-argument
    // Win32 signature, no ambiguity to audit beyond that. Two cases:
    //  - a handle this file's own FindFirstFileA actually created (present
    //    in g_findHandles()): erase the enumeration state (no leak) and
    //    return TRUE.
    //  - anything else, INCLUDING INVALID_HANDLE_VALUE (0xFFFFFFFF) itself
    //    -- confirmed to reach here in G25's own hardware log, since
    //    FindFirstFileA("*.*") returned INVALID_HANDLE_VALUE and Ignition's
    //    CRT wrapper still calls FindClose on it unconditionally -- is
    //    looked up in our OWN map only (never dereferenced as a pointer, so
    //    there is nothing to crash on) and answered with real Win32
    //    behavior: FALSE + GetLastError()==ERROR_INVALID_HANDLE (6). No
    //    success is faked either way.
    K("FindClose", 1, [](Cpu& c) -> uint32_t {
        uint32_t esp = c.reg(R_ESP);
        uint32_t callerEip = c.read_u32(esp);
        uint32_t handle = c.arg(0);
        IGN_LOG("[G25]", "FindClose entry: handle=0x%08x caller_guest_eip=0x%08x guest_esp=0x%08x",
                handle, callerEip, esp);
        IGN_LOG("[G25]", "stack_check: entry_esp=0x%08x argc_dwords=1 expected_esp_after_return=0x%08x (entry_esp+8) return_target=0x%08x",
                esp, esp + 8, callerEip);

        auto it = g_findHandles().find(handle);
        if (it == g_findHandles().end()) {
            IGN_LOG("[G25]", "FindClose(0x%08x) -> FALSE (not a handle this shim ever issued -- %s), ERROR_INVALID_HANDLE",
                    handle, handle == 0xFFFFFFFFu ? "INVALID_HANDLE_VALUE" : "unknown/foreign handle");
            wx86_set_lasterr(c, 6u);   // ERROR_INVALID_HANDLE
            return 0u;   // FALSE
        }
        IGN_LOG("[G25]", "FindClose(0x%08x) -> TRUE (pattern was \"%s\", dir \"%s\", %u matched entr%s freed)",
                handle, it->second.guestPattern.c_str(), it->second.hostDir.c_str(),
                (unsigned)it->second.entries.size(), it->second.entries.size() == 1 ? "y" : "ies");
        g_findHandles().erase(it);
        return 1u;   // TRUE
    });

    // ---- Bootstrap batch (Phase 3): trivial, well-defined KERNEL32 entry
    // points the engine genuinely doesn't implement (confirmed by reading
    // win32_shims_kernel32.cpp/win32_shims_memory.cpp -- none of these were
    // silently missed, they simply aren't there). ------------------------

    // DWORD GetVersion(void). Bit layout is the same on every Windows
    // version (LOWORD = major | minor<<8; if bit31==0, HIWORD = build
    // number; if bit31==1, it's the Win9x family and the encoding differs).
    // NOT reported as "Windows 95" despite Ignition being a Win95-era game:
    // the engine's own GetVersionExA (win32_shims_kernel32.cpp) already
    // hardcodes major=5 minor=1 build=2600 platform=VER_PLATFORM_WIN32_NT
    // (Windows XP) -- answering GetVersion with Windows 95 instead would
    // make the guest observe two contradictory OS identities depending on
    // which function it calls, which is a worse bug than "wrong OS". This
    // mirrors those exact values instead.
    K("GetVersion", 0, [](Cpu&) -> uint32_t {
        const uint32_t major = 5, minor = 1, build = 2600;   // matches GetVersionExA
        uint32_t v = major | (minor << 8) | (build << 16);   // bit31=0: NT family
        IGN_LOG("[WIN32]", "GetVersion() -> 0x%08x (5.1.2600, NT -- matches GetVersionExA)", v);
        return v;
    });

    // HANDLE GetCurrentProcess(void) -- always the pseudo-handle -1, never fails.
    K("GetCurrentProcess", 0, [](Cpu&) -> uint32_t { return 0xFFFFFFFFu; });

    // BOOL QueryPerformanceCounter(LARGE_INTEGER*) -- must count in the same
    // unit as QueryPerformanceFrequency (win32_shims_kernel32.cpp, default
    // 1,000,000 Hz, i.e. microseconds -- we never called
    // wx86_set_perf_frequency() to change that). wx86_now_us() is WinVita's
    // own monotonic clock, already used elsewhere for exactly this purpose.
    K("QueryPerformanceCounter", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        uint64_t t = wx86_now_us();
        c.write_u32(p, (uint32_t)t);
        c.write_u32(p + 4, (uint32_t)(t >> 32));
        uint32_t esp=c.reg(R_ESP);
        g_g99QpcCaller=c.read_u32(esp);
        g_g99QpcValue=t;
        ++g_g99QpcSerial;
        return 1u;
    });

    // Global*: win32_shims_memory.cpp's GlobalAlloc returns the guest
    // address itself as the "handle" (GMEM_FIXED-style, no moveable-memory
    // indirection -- confirmed by reading it). For a fixed-memory model,
    // real Windows defines GlobalLock/GlobalHandle as the identity function
    // on that pointer, and GlobalUnlock always reports "now unlocked" (0) --
    // a fixed block has no real lock count to decrement.
    K("GlobalHandle", 1, [](Cpu& c) -> uint32_t { return c.arg(0); });
    K("GlobalLock", 1, [](Cpu& c) -> uint32_t { return c.arg(0); });
    K("GlobalUnlock", 1, [](Cpu&) -> uint32_t { return 0u; });

    // DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION). 172-byte
    // struct: Bias(4) StandardName[32](64) StandardDate(16) StandardBias(4)
    // DaylightName[32](64) DaylightDate(16) DaylightBias(4). Reporting UTC
    // with no DST and empty names is the documented, valid
    // TIME_ZONE_ID_UNKNOWN response ("time zone data has not been filled
    // in") -- an honest "we don't have real timezone data", not a guess.
    K("GetTimeZoneInformation", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        if (p) {
            uint8_t zero[172] = {0};
            c.write(p, zero, sizeof zero);
        }
        return 0u;   // TIME_ZONE_ID_UNKNOWN
    });
}

// iteration-G15, Parte H: builds "name(bytes) name2(bytes2) ..." for every
// handle that had bytes read since the last call, resolving names via the
// engine's own wx86_file_paths() (already populated by CreateFileA) rather
// than tracking a second copy -- then CLEARS the per-handle byte map, so
// the next call only reports what happened in the NEXT interval. Read-only
// with respect to file behavior; this never changes what ReadFile does.
std::string kernel32_ign_g15_snapshot_file_activity() {
    std::string out;
    for (const auto& kv : g_g15BytesReadPerHandle()) {
        auto pit = wx86_file_paths().find(kv.first);
        std::string name = (pit != wx86_file_paths().end()) ? pit->second : "?";
        // Just the filename component -- the full host path is already
        // logged in [FILE] lines elsewhere; this summary stays compact.
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        char buf[128];
        snprintf(buf, sizeof buf, "%s(read=%u) ", name.c_str(), (unsigned)kv.second);
        out += buf;
    }
    g_g15BytesReadPerHandle().clear();
    if (out.empty()) out = "(none)";
    return out;
}

} // namespace ign
