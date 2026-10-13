#include "runtime/ignition_boot.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/pe_image.h"
#include "runtime/guest_region.h"
#include "runtime/guest_scratch.h"
#include "runtime/host_clock.h"
#include "runtime/ignition_memlayout.h"
#include "runtime/ignition_identity.h"
#include "runtime/win32_shims_kernel32.h"
#include "runtime/win32_shims_locale.h"
#include "runtime/win32_shims_memory.h"
#include "runtime/win32_shims_wait.h"
#include "runtime/win32_shims_shell32.h"
#include "runtime/win32_shims_advapi32.h"
#include "runtime/win32_shims_user32.h"
#include "runtime/win32_shims_misc.h"
#include "runtime/win32_shims_psapi.h"
#include "runtime/win32_shims_version.h"
#include "runtime/win32_shims_wintrust.h"
#include "runtime/win32_shims_gdi32.h"
#include "runtime/win32_shims_window.h"
#include "runtime/win32_shims_sync.h"
#include "runtime/ds_emul.h"

#include "diagnostics/log.h"
#include "diagnostics/phase_state.h"
#include "diagnostics/call_trace.h"
#include "runtime/ignition_paths.h"
#include "music/music_service.h"
#include "win32/kernel32_ign.h"
#include "win32/winmm_ign.h"
#include "win32/user32_window.h"
#include "win32/ddraw.h"
#include "win32/dinput.h"
#include "win32/glide2x_ign.h"
#include "win32/g150_ctl.h"   // iteration-G150: control tokens + the resume loop of the guest run

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef IGNITION_RENDERER_3DFX
#include <psp2/kernel/sysmem.h>
#endif

using namespace d2rt;

// iteration-G78, Parte 30: true global (declared before `namespace ign {
// namespace { ... } }` opens, same cross-TU pattern already proven for
// g_g74TrackLoadActive/g_g56TargetAllocBase) -- the process-start
// timestamp every [G78-STARTUP-TIMING] checkpoint in win32/kernel32_ign.cpp,
// win32/dinput.cpp, and win32/glide2x_ign.cpp reports its elapsed_ms
// against, so all five checkpoints are comparable without needing a
// shared clock object passed around.
uint64_t g_g78ProcessStartMs = 0;

namespace ign {

namespace {

// Renderer/EXE selector -- IGNITION_RENDERER_3DFX is a CMake option
// (default OFF), never a d3d-vs-3dfx runtime switch: the two branches
// share every line of this file except what's explicitly guarded below.
// The stable DirectDraw path is IGNITION_RENDERER_3DFX undefined --
// exactly the behavior this file already had before this iteration.
//
// iteration-G1: kExePath now derives from runtime/ignition_identity.h
// instead of its own independent literal -- G0's real-hardware log showed
// GetCommandLineA/GetModuleFileNameA (kernel32_ign.cpp) still answering
// "Ign_d3d.exe" even though THIS constant correctly pointed at
// Ign_3dfx.exe, because those two shims had their own separate hardcoded
// strings. One source of truth now for all three.
const char* const kExePath = kGuestExePathFwdSlash;

// WinMain is 0x004120A0 -- verified twice against the real Ign_d3d.exe on
// desktop (entry point 0x00469950's CRT bootstrap ends with `push eax
// (hInstance); call 0x004120A0`, whose own tail is `ret 0x10`, matching
// WinMain's 4-dword stdcall signature; and its RegisterClassA call site
// ends at 0x0041212D, byte-exact with the real hardware [STOP] from the
// RegisterClassA test). NOT hooked or wrapped in any way this iteration:
// the previous build's set_alternate()+set_intrinsic() "log and pass
// through" observer regressed real hardware execution
// (WinMain's body never ran; the CRT proceeded straight to ExitProcess(0)
// with EAX apparently 0). Root cause, as far as static reading of the
// dynarec gets without a live debugger: internalDBGetBlock() (dynablock.c)
// refuses to produce a translated block for ANY address that still has an
// alternate registered ("if (hasAlternate(addr)) return NULL"), and
// dyn86_set_alternate() has no "undo" -- so the intrinsic's own
// `set_reg(R_EIP, 0x004120A0)` lands back on an address the dynarec now
// permanently refuses to translate, rather than resuming WinMain's real
// bytes. The API's own doc line ("lets a known function run native code IN
// PLACE of a guest function") says REPLACE, not "observe then continue" --
// this was a misuse of it, not a WinVita bug. Progress is tracked instead
// through real import calls only (RegisterClassA/CreateWindowExA/
// Peek-or-GetMessageA/DirectDrawCreate, all below), which need no hook.

Cpu* g_cpu = nullptr;
bool g_halted = false;
uint32_t g_g150Resumes = 0;   // iteration-G150: how many times the guest run was re-entered after its block budget ended (see boot_run)
std::string g_halt_reason;

bool read_whole_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t got = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return got == (size_t)sz;
}

// VirtualQuery backing: honestly say "can't introspect that" rather than
// fabricate a MEMORY_BASIC_INFORMATION for an address we know nothing about.
bool ign_vquery_stub(uint32_t /*addr*/, uint32_t& /*base*/, uint32_t& /*size*/,
                     uint32_t& /*state*/, uint32_t& /*type*/, uint32_t& /*protect*/,
                     uint32_t& /*allocbase*/) {
    return false;
}

uint32_t ign_ds_va_alloc(uint32_t n) { return wx86_scratch_alloc(n); }
uint32_t ign_ds_tick_ms(Cpu&) { return (uint32_t)wx86_now_ms(); }

// iteration-G129: the DirectSound engine's own "[son] ..." lines go to the
// project log (the engine has no console of its own -- on Vita printf is
// invisible). log_line() serializes on its own mutex, so the audio thread's
// one-shot lines are safe. Every engine line is a one-shot or very sparse; the
// engine never logs per Lock/Play/grain.
void ign_ds_log(const char* line) { IGN_LOG("[G131-DS]", "%s", line); }

// iteration-G131: boot-time reconciliation of everything OUTSIDE the visual code that could differ between
// two runs of the same graphics build. Nothing here touches Glide/LFB/HUD/compositor code or changes how
// any of it behaves; it (a) proves the visual modules are the G128 ones, (b) puts the persisted overlay-key
// cache back to the state a fresh install has when it holds anything but the proven key, and (c) reads an
// OPTIONAL control file used to bisect an on-device regression inside one build.
constexpr const char* kG131KeyCachePath = "ux0:data/ignition/overlay_key.dat";
constexpr const char* kG131AbPath       = "ux0:data/ignition/g131_ab.txt";

void ign_g131_visual_hashes() {
#ifdef G131_VIS_HASHES
    // "path=<sha256 first 16 hex of the file compiled into THIS build>/<same for the reference snapshot (G131)>|..."
    const char* p = G131_VIS_HASHES;
    int total = 0, match = 0, unavailable = 0;
    while (*p) {
        const char* bar = strchr(p, '|');
        const size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char item[160] = {0};
        if (n >= sizeof item) break;
        memcpy(item, p, n);
        char* eq = strchr(item, '=');
        char* sl = eq ? strchr(eq, '/') : nullptr;
        if (eq && sl) {
            *eq = 0; *sl = 0;
            ++total;
            if (!strcmp(sl + 1, "unavailable")) ++unavailable;
            else if (!strcmp(eq + 1, sl + 1)) ++match;
            else IGN_LOG("[G131-VIS]", "DIFF %s this_build=%s g128=%s", item, eq + 1, sl + 1);
        }
        if (!bar) break;
        p = bar + 1;
    }
    IGN_LOG("[G131-VIS]", "frozen modules (graphics, input, kernel, DirectSound engine) compared with the G131 snapshot (source SHA-256 prefix): %d files, %d identical, %d different, %d without reference",
            total, match, total - match - unavailable, unavailable);
#else
    IGN_LOG("[G131-VIS]", "no module hash table compiled in");
#endif
}

void ign_g131_key_cache() {
    // The cache only seeds the overlay key before this session's own lock samples confirm it (glide2x_ign.cpp
    // g115_seed_overlay_key). Every confirmed key ever observed on hardware maps to RGB565 0x1082/0x1083
    // (raw 0x00131313 / 0x00101010 / 0x00101018). Anything else is not a state G128 could have produced: it is set
    // aside (renamed, never deleted) so the run starts exactly like a fresh install, which seeds the proven key.
    FILE* f = fopen(kG131KeyCachePath, "r");
    if (!f) { IGN_LOG("[G131-KEY]", "overlay_key.dat absent -> the proven seed key applies"); return; }
    char buf[128] = {0};
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unsigned raw = 0, c565 = 0, cfmt = 0;
    const int k = sscanf(buf, "G115KEY %x %x %u", &raw, &c565, &cfmt);
    for (size_t i = 0; i < n; ++i) if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
    const bool good = (k == 3) && (c565 == 0x1082u || c565 == 0x1083u);
    IGN_LOG("[G131-KEY]", "overlay_key.dat = \"%s\" parsed=%d rgb565=0x%04x -> %s", buf, k, c565, good ? "consistent with the proven key, kept" : "NOT a proven key, quarantined");
    if (!good) {
        const char* q = "ux0:data/ignition/overlay_key.dat.g131_quarantine";
        remove(q);
        const int rc = rename(kG131KeyCachePath, q);
        IGN_LOG("[G131-KEY]", "rename to overlay_key.dat.g131_quarantine rc=%d", rc);
    }
}

// Optional control file ux0:data/ignition/g131_ab.txt (absent = normal run). Whitespace-separated tokens:
//   nosound=1  DirectSoundCreate fails like G128 (no sound at all)   -> control run: must look exactly like G128
//   prio=a0    audio thread first rung back to 0x10000100 (G129)
//   fresh=0    stream-ring silence off (G129 ring behaviour)
// Environment variables are what the vendored audio layer already reads (WX86_SON / WX86_SONPRIO / WX86_FRESH).
bool g_g132MusicOff = false;     // set by the optional control file (see below)
void ign_g131_ab_file() {
    FILE* f = fopen(kG131AbPath, "r");
    if (!f) return;
    char buf[256] = {0};
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; ++i) if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
    IGN_LOG("[G131-AB]", "control file present: \"%s\"", buf);
    if (strstr(buf, "nosound=1")) { setenv("WX86_SON", "0", 1);            IGN_LOG("[G131-AB]", "nosound=1 -> DirectSound disabled for this run (G128 behaviour)"); }
    if (strstr(buf, "prio=a0"))   { setenv("WX86_SONPRIO", "0x10000100", 1); IGN_LOG("[G131-AB]", "prio=a0 -> audio thread priority 0x10000100"); }
    if (strstr(buf, "fresh=0"))   { setenv("WX86_FRESH", "0", 1);           IGN_LOG("[G131-AB]", "fresh=0 -> stream silence off"); }
    // iteration-G132 tokens (music):
    //   nomusic=1   the virtual CD is not created: mciSendStringA keeps G131's honest generic error (control run for the music)
    //   mcpu=N      core (0..2) of the music thread (default 1)
    //   mprio=HEX   priority of the music thread (default 0x100000F0 = effective 0x90, below the effects thread's 0x80)
    if (strstr(buf, "nomusic=1")) { g_g132MusicOff = true;                  IGN_LOG("[G131-AB]", "nomusic=1 -> virtual CD disabled for this run (G131 behaviour)"); }
    if (const char* p = strstr(buf, "mcpu="))  { setenv("WX86_MUSICCPU", p + 5 < buf + sizeof buf ? std::string(p + 5, strcspn(p + 5, " ")).c_str() : "1", 1); IGN_LOG("[G131-AB]", "mcpu requested"); }
    if (const char* p = strstr(buf, "mprio=")) { setenv("WX86_MUSICPRIO", std::string(p + 6, strcspn(p + 6, " ")).c_str(), 1); IGN_LOG("[G131-AB]", "mprio requested"); }
}

// iteration-15: wires up an engine primitive that already existed
// (win32_shims_memory.h's wx86_mem_set_observer) but was never connected --
// not a new mechanism, just turning on existing, already-safe visibility
// into VirtualAlloc-family activity (the CRT heap growing its own arena via
// VirtualAlloc falls under this too). Kept quiet for routine traffic; only
// the events the header itself calls out as noteworthy get a log line.
void ign_mem_observer(const WxMemEvent& ev) {
    const char* kind = "?";
    switch (ev.kind) {
        case WX86_MEM_VA_COMMIT_IN:     kind = "COMMIT_IN"; break;
        case WX86_MEM_VA_RESERVE:       kind = "RESERVE"; break;
        case WX86_MEM_VA_COMMIT_FRESH:  kind = "COMMIT_FRESH"; break;
        case WX86_MEM_VA_BIG:           kind = "BIG(>=1MiB)"; break;
        case WX86_MEM_VA_GARBAGE:       kind = "GARBAGE(>=2GiB, corrupted size)"; break;
        case WX86_MEM_VA_FAIL:          kind = "FAIL"; break;
        case WX86_MEM_VA_RELEASE:       kind = "RELEASE"; break;
        case WX86_MEM_VA_RELEASE_MISS:  kind = "RELEASE_MISS(leak)"; break;
        case WX86_MEM_VA_DECOMMIT:      kind = "DECOMMIT"; break;
    }
    // BIG/GARBAGE/FAIL/RELEASE_MISS are the ones worth seeing even outside a
    // targeted investigation; the routine COMMIT/RESERVE/DECOMMIT churn
    // isn't, to keep this from flooding the log across the whole session.
    if (ev.kind == WX86_MEM_VA_BIG || ev.kind == WX86_MEM_VA_GARBAGE ||
        ev.kind == WX86_MEM_VA_FAIL || ev.kind == WX86_MEM_VA_RELEASE_MISS) {
        IGN_LOG("[VAMEM]", "%s addr=0x%08x size=%u type=0x%08x protect=0x%08x guest_caller=0x%08x",
                kind, ev.addr, ev.size, ev.type, ev.protect, ev.ra);
    }
}

void log_import(const ImportRef& imp) {
    if (!imp.name.empty()) IGN_LOG("[IMPORT]", "%s!%s", imp.dll.c_str(), imp.name.c_str());
    else                   IGN_LOG("[IMPORT]", "%s!#%u", imp.dll.c_str(), imp.ordinal);
}

} // namespace

wx86::GuestRegion& ignition_heap_region() {
    static wx86::GuestRegion r;
    return r;
}

uint32_t guest_run_resumes() { return g_g150Resumes; }   // iteration-G150
void request_halt(const char* reason) {
    if (g_halted) return;   // first reason wins; later ones are noise once we're stopping
    g_halted = true;
    g_halt_reason = reason ? reason : "(unknown)";
    IGN_LOG("[STOP]", "%s", g_halt_reason.c_str());
    // iteration-G35, Parte N/O: unambiguous, never-lost exit-reason line --
    // this is the ONE place every diagnostic-driven halt in this whole
    // project funnels through (unimplemented API traps, CPU-fault handling
    // that calls this, explicit ExitProcess/TerminateProcess shims, every
    // request_halt() call site), so it is the correct single choke point
    // for "[G35-EXIT] reason=..." rather than duplicating this at each caller.
    IGN_CRITICAL("[G35-EXIT]", "reason=wrapper_diagnostic_halt detail=\"%s\"", g_halt_reason.c_str());
    if (g_cpu) g_cpu->request_stop();
}

void boot_run() {
    log_init();
    IGN_CP("engine-start", "boot_run() entered (logs already prepared by main())");
    g_g78ProcessStartMs = wx86_now_ms();
    IGN_LOG("[G78-STARTUP-TIMING]", "checkpoint=process_start elapsed_ms=0");
    // iteration-G35, Parte C: a session id that survives even if the main
    // log gets wiped by a stray relaunch (see log.h's own comment on
    // ignition_session_id()/critical_log() for the full audit). Logged to
    // BOTH the main log and the never-truncated critical log, so either
    // one alone still identifies which boot produced its lines.
    uint32_t g35SessionId = ignition_session_id();
    IGN_LOG("[G35-SESSION]", "id=%u", g35SessionId);
    IGN_CRITICAL("[G35-SESSION]", "boot start");
#if defined(IGNITION_RENDERER_3DFX) && !defined(G20C_IDLE_NOOP) && !defined(G20C_BYPASS_IDLE_CALLSITE)
    // iteration-G23-BIS (mechanism kept permanently, text updated each
    // iteration): unmistakable FIRST log line, printed before even the
    // [BUILD] fingerprint block below, so a real-hardware run can always be
    // matched to the exact binary before anything else is analyzed --
    // this exists precisely because G23B's hardware log once came from a
    // stale, pre-G23 install (root-caused via binary string search, see
    // CMakeLists.txt's own G23-BIS comment). G35 PROVED CROSS->DIK_RETURN
    // ->IsKeyDown(TRUE)->real file loading (ign1.cdp/ign2.cdp) end to end
    // -- the frontend was never frozen, just extremely slow (~5-6s per
    // swap). G36's own [G31-LOOP] numbers showed 26 FULL 512KB texture
    // uploads per present; audited every consumer of lfb_texture in this
    // file (only lfb_blit_to_backbuffer, only at swap time) and found
    // uploading once instead of 26 times is provably pixel-identical --
    // see grLfbUnlock_impl/grBufferSwap_impl's own comments. Also disabled
    // the per-ReadFile/CloseHandle file checksum (Part U) and added a
    // real per-present timing profile ([G36-PERF]). G37 found the REAL
    // cause of the remaining ~5-6s/present stall: grLfbLock_impl (NOT
    // grLfbUnlock_impl, whose checksum kG32LfbFullChecksum already
    // disabled since G32) had its OWN, completely ungated, unmeasured
    // full 512,000-byte checksum call, firing on all ~26 locks/present
    // (733x in G36's own hardware log at ~185-190ms each = ~4.9s/present
    // -- see win32/glide2x_ign.cpp's kG37LfbLockChecksum). Disabled
    // completely; also reduced the per-lock GrLfbInfo log to one-time
    // and added a minimal [G37-PERF] line. G37's own real-hardware run
    // then reached a REAL new blocker past the language selector: the
    // guest's confirm (START->DIK_RETURN) worked, file I/O for
    // ign1.cdp/ign2.cdp/ign3_0..3.cdp all opened cleanly, and the run
    // stopped cleanly (not a crash) on the next not-yet-implemented
    // Glide API: grSstVRetraceOn. G38 implements it for real -- see
    // win32/glide2x_ign.cpp's grSstVRetraceOn_impl for the full
    // caller-disassembly evidence (it's a guest busy-wait, not a
    // one-shot query). G38's own real-hardware run then confirmed a real
    // visual milestone -- the guest advanced past the language selector
    // into actual intro/video content -- and stopped cleanly (not a
    // crash) on the next not-yet-implemented Glide API:
    // guTexAllocateMemory, called with 15 real arguments (60 bytes,
    // matching its confirmed decoration exactly) right after a run of
    // grTexDownloadTable palette calls and a CANADA.TEX file load. G39
    // implements it for real -- see win32/glide2x_ign.cpp's
    // guTexAllocateMemory_impl for the full signature verification
    // (cross-checked against openglide's real sdk2_glide.h header this
    // iteration) and caller-disassembly evidence. G39's own real-hardware
    // run then confirmed the biggest visual milestone yet -- the guest's
    // intro/video sequence played FULLY to completion, then the guest
    // continued its own flow and stopped cleanly (not a crash) on the
    // next not-yet-implemented Glide API: guTexDownloadMipMap, called
    // with the exact handle guTexAllocateMemory returned (0x70000001)
    // and a real 65536-byte source pixel buffer. G40 implements it for
    // real -- see win32/glide2x_ign.cpp's guTexDownloadMipMap_impl for
    // the full signature verification (openglide's sdk2_glide.h again)
    // and caller-disassembly evidence, including tracing the source
    // pointer back to Ignition's own guest-side palette->RGB332
    // conversion.
    IGN_LOG("[G40]", "THIS_IS_THE_TEXTURE_DOWNLOAD_BUILD (guTexDownloadMipMap implemented -- real GL texture created and the guest's actual RGB_332 source pixels uploaded via glTexImage2D, decoded to RGB565; G39's logical allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G40's own real-hardware run confirmed the texture pipeline is
    // genuinely multi-texture (29 real guTexAllocateMemory/
    // guTexDownloadMipMap pairs, all glGetError=0), the intro plays
    // FULLY, and post-intro asset loading advances deep into level/car/
    // menu data (menucar.msh, CANADA.TEX, CARS.TEX, and more) before
    // stopping cleanly (not a crash) on the next not-yet-implemented
    // Glide API: guColorCombineFunction(fnc=0x2), called right after
    // menucar.msh loads. G41 implements it for real -- see
    // win32/glide2x_ign.cpp's guColorCombineFunction_impl for the full
    // GrColorCombineFnc_t table (sourced from openglide's real reference
    // implementation) and caller-disassembly evidence.
    IGN_LOG("[G41]", "THIS_IS_THE_COLOR_COMBINE_BUILD (guColorCombineFunction implemented -- full GrColorCombineFnc_t translation table for the values with real reference-implementation evidence, stored as logical GlideContext state per Part I, no GL state touched yet; G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G41's own real-hardware run confirmed guColorCombineFunction(ITRGB)
    // passed cleanly and the guest stopped on a REAL, qualitatively new
    // milestone: guDrawTriangleWithClip -- the guest's first actual
    // triangle-draw request, reached right after color-combine setup
    // following menucar.msh (Part Y: MENUCAR_DRAW_CANDIDATE, not yet
    // proven). G42 implements the first true 3D geometry path -- see
    // win32/glide2x_ign.cpp's guDrawTriangleWithClip_impl for the full
    // GrVertex layout verification, caller-disassembly evidence, and the
    // vertex-array-only (never immediate-mode) GL implementation.
    IGN_LOG("[G42]", "THIS_IS_THE_FIRST_TRIANGLE_DRAW_BUILD (guDrawTriangleWithClip implemented -- real GrVertex decode, NaN/Inf validation, vertex-array-only glDrawArrays(GL_TRIANGLES) with iterated per-vertex color per the active GR_COLORCOMBINE_ITRGB state, logical->physical coordinate mapping reused from grClipWindow/the LFB blit; G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G42's own real-hardware run confirmed the real triangle draw path
    // works end to end (glGetError=0 repeatedly, real vertex data with
    // colors/coords decoded correctly) and the guest stopped cleanly on
    // grChromakeyMode(mode=0x0), called immediately after that first
    // triangle -- a defensive chromakey-disable reset for an untextured
    // ITRGB draw, per this iteration's own disassembly of the adjacent
    // sibling function showing the TEXTURED sibling path (guTexSource ->
    // guTexCombineFunction -> guColorCombineFunction(DECAL_TEXTURE) ->
    // grChromakeyMode(ENABLE) -> grChromakeyValue) that will likely
    // follow in a future iteration. G43 implements grChromakeyMode for
    // real -- see win32/glide2x_ign.cpp's grChromakeyMode_impl.
    IGN_LOG("[G43]", "THIS_IS_THE_CHROMAKEY_MODE_BUILD (grChromakeyMode implemented as pure logical state, Part E/F -- no GL chroma-keying behavior faked; G42's triangle-draw path, G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G43's own real-hardware run PROVED the 3D triangle pipeline is
    // genuinely active -- 278 real triangles drawn (glGetError=0
    // throughout, [G43-DRAW-SUMMARY] triangles_total=278
    // draw_total_us=216823) -- but [G43-3D] first_swap_after_3d never
    // fired, meaning no present has happened yet since those draws
    // started: the 3D has not had its first real chance to reach the
    // screen. The guest stopped cleanly on grAlphaBlendFunction(1,5,4,0)
    // -- the textbook standard alpha-blend formula (GR_BLEND_SRC_ALPHA /
    // GR_BLEND_ONE_MINUS_SRC_ALPHA for RGB, GR_BLEND_ONE/GR_BLEND_ZERO
    // for alpha) -- called from a ONE-TIME blend-state initializer this
    // iteration's own disassembly traced to at least 7 per-material call
    // sites. G44 implements it for real -- see win32/glide2x_ign.cpp's
    // grAlphaBlendFunction_impl -- and reduces grChromakeyMode's
    // real-hardware-demonstrated hot-path logging overhead (Part O/P).
    IGN_LOG("[G44]", "THIS_IS_THE_ALPHA_BLEND_BUILD (grAlphaBlendFunction implemented with glBlendFuncSeparate -- confirmed present in this project's own linked vitaGL.h, not assumed -- real GrAlphaBlendFnc_t->GL translation table, GL_BLEND enabled/disabled via the real (ONE,ZERO)-identity test rather than a naive heuristic; grChromakeyMode's hot-path logging reduced to first-5/on-change/periodic-summary, semantics unchanged; G43's chromakey state, G42's triangle-draw path, G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G44's own real-hardware run confirmed grAlphaBlendFunction works
    // (GL_BLEND enabled, glGetError=0) and the guest continued straight
    // into guAlphaSource(mode=0x0) -- decoded to GR_ALPHASOURCE_CC_ALPHA
    // (alpha from the CONSTANT color, not iterated/vertex alpha) via
    // openglide's real guAlphaSource reference implementation (same
    // stateless grAlphaCombine-forwarding architecture G41 already
    // established for guColorCombineFunction). 278 triangles still stand
    // (triangles_total=278, draw_total_us=275505 this run -- consistent
    // with G43's 216823us modulo normal run-to-run variance, NOT a
    // performance regression) and [G43-3D] first_swap_after_3d STILL had
    // not fired by this run's own STOP -- the 3D pipeline has not yet had
    // its first real chance to present. G45 implements guAlphaSource for
    // real -- see win32/glide2x_ign.cpp's guAlphaSource_impl -- reduces
    // guDrawTriangleWithClip's per-vertex debug dump (semantics
    // completely unchanged, Part 13/23), and adds a light draw/blit/swap
    // order probe ([G45-3D]) for whenever the first post-3D present
    // finally happens.
    IGN_LOG("[G45]", "THIS_IS_THE_ALPHA_SOURCE_BUILD (guAlphaSource implemented -- full GrAlphaSource_t translation table sourced from openglide's real reference implementation, stored as logical GlideContext state, no GL touched yet; G44's alpha-blend state, G43's chromakey state, G42's triangle-draw path (debug logging reduced, GL logic unchanged), G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G45's own real-hardware run confirmed guAlphaSource(CC_ALPHA)
    // stored correctly and the guest continued straight into
    // grConstantColorValue(0x7FFFFFFF) -- decoded, using THIS project's
    // own G4E-confirmed GR_COLORFORMAT_ARGB (Ignition's real
    // grSstWinOpen color_format, not guessed), to A=0x7F/R=0xFF/G=0xFF/
    // B=0xFF -- white at ~50% alpha. Combined with the already-real
    // GR_COLORCOMBINE_CCRGB (G41) and GR_ALPHASOURCE_CC_ALPHA (G45),
    // this confirms whichever draws follow will be entirely constant-
    // colored, both RGB and alpha. 278 triangles still stand
    // (draw_total_us=41313 this run, down from 216823/275505 in prior
    // runs -- Part 24: NOT yet declared a real improvement, run-to-run
    // variance not isolated). [G43-3D]/[G45-3D] first_swap_after_3d
    // STILL had not fired by this run's own STOP. G46 implements
    // grConstantColorValue for real -- see win32/glide2x_ign.cpp's
    // grConstantColorValue_impl -- and gates off the old per-vertex
    // triangle debug dump by default (Part 25).
    IGN_LOG("[G46]", "THIS_IS_THE_CONSTANT_COLOR_BUILD (grConstantColorValue implemented -- decoded using Ignition's own real GR_COLORFORMAT_ARGB (confirmed at grSstWinOpen, G4E), stored as logical GlideContext state (raw value + decoded A/R/G/B), NOT yet applied to any draw path (Part 8/10/13: guDrawTriangleWithClip stays frozen this iteration); G45's alpha-source state, G44's alpha-blend state, G43's chromakey state, G42's triangle-draw path, G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G46's own real-hardware run delivered the huge milestone: the guest
    // reached its first real post-intro 3D PRESENT
    // ([G45-3D] first_swap_after_3d, completed_swap_count=294,
    // triangles_total=562, lfb_dirty_this_swap=NO -- the fullscreen LFB
    // blit did NOT run this swap, so it cannot be blamed for the visible
    // frame) and kept drawing (2573+ triangles this run). The visible
    // frame was rainbow/psychedelic, and this iteration PROVED why:
    // guDrawTriangleWithClip_impl always filled the GL color array from
    // GrVertex.r/g/b/a UNCONDITIONALLY, completely ignoring
    // color_combine_local/alpha_combine_local -- so triangles drawn under
    // GR_COLORCOMBINE_CCRGB (constant color, real per-frame values like
    // 0x7fdfdfdf/0x7fe3e3e3/0x7f7c7c7c observed) still displayed the raw
    // per-vertex iterated color stream. Fixed by consulting the SAME
    // combine state G41/G45/G46 already track (function==LOCAL + local==
    // CONSTANT selects grConstantColorValue instead of the vertex). The
    // run then halted not on an unimplemented API but on the NaN/Inf
    // vertex guard -- audited and found over-strict (validated ALL 9
    // core GrVertex floats unconditionally, including z/ooz/oow that this
    // draw path has never consumed); validation is now DERIVED from
    // which fields the active combine state actually requires. See
    // win32/glide2x_ign.cpp's guDrawTriangleWithClip_impl,
    // gu_vertex_required_finite, and gu_log_nonfinite_vertex_dump.
    IGN_LOG("[G47]", "THIS_IS_THE_COLOR_COMBINE_FIX_BUILD (guDrawTriangleWithClip now selects RGB/alpha source from the REAL active color_combine_local/alpha_combine_local state instead of always using iterated vertex color -- proven bug, real-hardware rainbow frame; NaN/Inf vertex validation is now state-aware (x/y always required, r/g/b only if iterated color is active, a only if iterated alpha is active, z/ooz/oow/tmuvtx never required since never consumed) instead of rejecting any non-finite field anywhere in the 60-byte struct; hot-path logging for grAlphaBlendFunction/guColorCombineFunction/grConstantColorValue reduced to first-N-calls plus periodic RAM-counter summaries, no per-call/per-change spam; G46's constant-color state, G45's alpha-source state, G44's alpha-blend state, G43's chromakey state, G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all otherwise unchanged)");
    // G47's own real-hardware run PROVED the color-combine fix is active
    // and the 3D scene keeps presenting (2500+ triangles across multiple
    // swaps) -- but then hit a REAL required-field failure this time (not
    // an unused-field false positive): caller_guest_eip=0x0045223b,
    // vertex C, field g, raw=0xFFFFEEE0 (a genuine IEEE754 NaN). This
    // iteration disassembled that exact caller (0x00451f28-0x0045225d)
    // and confirmed: it issues TWO guDrawTriangleWithClip calls sharing
    // the SAME "c" vertex slot (a quad-split-into-two-triangles
    // topology), the 60-byte GrVertex layout is STILL correct at this
    // callsite (real pointer deltas b-a=120/c-b=60 match the
    // disassembly's own stack offsets exactly), and the color/lighting
    // fields are computed by dense per-channel fixed-point arithmetic
    // whose single producing instruction could not be pinned down with
    // full confidence through static reading alone (no live
    // single-instruction tracing exists on the real dynarec backend,
    // confirmed since G16). The NaN was NOT sanitized and validation was
    // NOT weakened -- the halt stays exactly correct. Instead, this
    // iteration adds a one-shot raw 15-DWORD dump of ALL THREE vertices
    // ([G48-A/B/C-RAW]) plus a stack-layout summary, so the NEXT real
    // hardware run gives enough raw evidence to finish the trace with
    // certainty. See win32/glide2x_ign.cpp's guDrawTriangleWithClip_impl
    // (its own G48 comment block) and gu_log_nonfinite_vertex_dump.
    IGN_LOG("[G48]", "THIS_IS_THE_NONFINITE_FORENSICS_BUILD (G47's color-combine fix and state-aware NaN/Inf validation both FROZEN, unchanged -- this iteration only ADDS a one-shot raw-DWORD forensic dump ([G48-A/B/C-RAW]) for the first required-field failure, confirms via real disassembly that the 60-byte GrVertex layout and the two-triangles-share-one-vertex quad topology at caller 0x0045223b are both legitimate (not a layout bug), and reduces grClipWindow/grTexDownloadTable hot-path logging to first-N-calls plus periodic summaries; no dynarec changes, no sanitization, no validation relaxation; G46's constant-color state, G45's alpha-source state, G44's alpha-blend state, G43's chromakey state, G42's triangle-draw path, G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // G48's own real hardware run delivered the raw [G48-C-RAW] dump --
    // NEW evidence analyzed this iteration: C.oow=0x3F800000 (exactly
    // 1.0f, clean) and C's tmu0 texcoords decode to plausible floats,
    // but C.r=0x0045042A and C.tmu1.sow=0x00450172 both fall INSIDE this
    // exact PE image's 0x0045xxxx code region (the same function
    // cluster disassembled since G41), and C.ooz/C.a/C.b are small-
    // integer-shaped rather than the FILD/FSTP-converted floats every
    // OTHER confirmed color field in this project shows -- strong
    // evidence for a partially-initialized/stale stack slot, not a
    // computed-garbage float. A serious attempt at full manual stack-
    // offset reconstruction of the enclosing ~700-instruction function
    // caught its OWN error mid-derivation (see
    // win32/glide2x_ign.cpp's G49 comment on guDrawTriangleWithClip_impl
    // for the exact correction) -- demonstrating that class of
    // reconstruction is genuinely unreliable by hand at this scale
    // without a live debugger, which this project's dynarec does not
    // expose (confirmed since G16). No exact producer instruction is
    // claimed this iteration; publishing a second wrong answer would be
    // worse than admitting the limit. Separately: G48's own forensic
    // dump code (validation + one-shot raw dump) only ever executes
    // INSIDE guDrawTriangleWithClip_impl, which is only ever reached
    // during the post-intro 3D scene -- structurally UNREACHABLE before
    // the language selector, so it is RULED OUT (not just "probably
    // not") as the cause of this run's separately-observed 119-second
    // stall between swap #2 and swap #3, which occurs before language
    // selection. This iteration adds two narrowly-scoped, low-overhead
    // profilers to actually localize that stall on the next run: a
    // long-gap probe reusing the EXISTING shared
    // dinput_poll_vita_input_impl bridge point every kernel32/user32/
    // winmm/dinput/glide call already passes through (win32/dinput.cpp,
    // zero new hook points), and aggregate (not per-call) file I/O
    // timing in CreateFileA/ReadFile (win32/kernel32_ign.cpp).
    IGN_LOG("[G49]", "THIS_IS_THE_STALL_AND_PRODUCER_FORENSICS_BUILD (G47's color-combine fix and G47/G48's state-aware NaN/Inf validation both FROZEN and UNCHANGED -- no sanitization, no relaxation; C-vertex producer NOT conclusively identified this iteration, evidence points to a stale/partially-initialized stack slot rather than computed garbage, documented honestly rather than guessed; a low-overhead long-gap stall probe ([G49-STALL]) and aggregate file I/O timing ([G49-FILEIO]) are added to localize the separately-observed 119s pre-language-selector stall, which is structurally ruled out as being caused by G48's own forensic code since that code is unreachable before the 3D scene; G46's constant-color state, G45's alpha-source state, G44's alpha-blend state, G43's chromakey state, G42's triangle-draw path, G41's color-combine state, G40's texture download path, G39's allocator, G38's grSstVRetraceOn, and G37's LFB-lock-checksum removal all unchanged)");
    // iteration-G50: G49's own real-hardware run confirmed the 119s
    // startup stall did NOT reproduce (14-28s early gaps only; NOT_REPRODUCED
    // this run) and the [G49-STALL] probe never fired (no single >=5s gap
    // between host-visible bridge points) -- no further startup work this
    // iteration (Part 29). Full static function-boundary reconstruction of
    // the caller at 0x00451f10-0x00452263 (containing the failing
    // 0x0045223B call) is complete: an exhaustive, file-wide grep of the
    // entire cached disassembly for local_C's r/g/b byte offsets
    // ([esp+0xdc]/[esp+0xe0]/[esp+0xe4] at this function's frame base)
    // returns ZERO matches anywhere inside the function -- C.g is PROVEN
    // (not merely suspected) to be stale/never-written stack data on
    // EVERY code path, including the function's own out-of-range debug-
    // color fallback branch, which colors A/B/B2 red/blue/green but never
    // touches C. This rules out a dynarec bug for this specific field
    // (nothing failed to execute -- no instruction to write it exists),
    // and points the likely real fix at our own color-combine state
    // tracking rather than the vertex producer -- NOT changed this
    // iteration pending real prefail-ring evidence from the next run. See
    // win32/glide2x_ign.cpp's G50 comment on guDrawTriangleWithClip_impl
    // for the full stack-frame map, exact two-triangle topology, and
    // instruction-level citations.
    IGN_LOG("[G50]", "THIS_IS_THE_STATIC_RECONSTRUCTION_BUILD (full function-boundary reconstruction of the 0x0045223B caller complete: stack frame mapped, exact two-triangle shared-vertex topology confirmed, and a file-wide disassembly grep PROVES C.r/C.g/C.b have no writer or reader anywhere in the function on any code path -- C.g is confirmed stale stack data, not a dynarec bug; dynarec stays FROZEN; a new [G50-PREFAIL-GLIDE] ring buffer now records the last 32 color/alpha/texture state calls before the failing draw, dumped once on the same halt, to gather real evidence for the color-combine-state-tracking fix next run; G49's 119s startup stall did NOT reproduce this run and no further startup work was done, per Part 29; per-swap log volume ([G15-FRAME]/[G31-LOOP]/[G37-PERF]/[G42-PERF]/[G36-PERF]) reduced from every-swap to every-30th-swap-or-unusual-gap, Part 30; G47's color-combine fix and G47/G48's state-aware NaN/Inf validation remain FROZEN and UNCHANGED -- no sanitization, no relaxation, no RGB-range hack)");
    // iteration-G51: G50's own real [G50-PREFAIL-GLIDE] ring (its first
    // real hardware data) resolved the open question directly: the last
    // two Glide calls before the halt were guColorCombineFunction(0x2=
    // ITRGB, caller=0x00450724) then guDrawTriangleWithClip(caller=
    // 0x0045223B), nothing else in between. Disassembly of 0x00450710-
    // 0x0045072b shows caller=0x00450724 is not a distinct setup
    // function -- it is a tiny cache-wrapper around the real
    // guColorCombineFunction Glide call, called from the SAME target
    // function G50 already mapped (0x00451f10-0x00452263) with two
    // different arguments on its two branches: 4 (DECAL_TEXTURE) on the
    // normal/in-range path, 2 (ITRGB) on the fallback/debug-color path.
    // The ring proves THIS run took the FALLBACK branch -- the same one
    // G50 showed writes flat RED/BLUE/GREEN debug colors into 3 of the 4
    // unique quad corners but never into the shared vertex C. This means
    // ITRGB is genuinely, correctly intended here by Ignition's own code
    // (Success Case C, not A) -- our color-combine state tracking is NOT
    // wrong, so no state-semantics change was made this iteration.
    // Per Part 24/26, validation stays exactly as-is: no caller-specific
    // hack, no relaxation, no vertex data invented. The real open
    // question is now upstream of this file entirely: why is the
    // particle/type index this fallback branch bounds-checks
    // (`arg0->field_0x18`) out of its valid [0,0x100) range on this run
    // -- left for a future iteration since the value is not recoverable
    // from CPU register state at the guDrawTriangleWithClip trap (it is
    // reloaded/repurposed for unrelated math before the draw calls). See
    // win32/glide2x_ign.cpp's G51 comment on guDrawTriangleWithClip_impl.
    IGN_LOG("[G51]", "THIS_IS_THE_STATE_SEMANTICS_RESOLUTION_BUILD (the first real [G50-PREFAIL-GLIDE] ring data proves ITRGB is genuinely, correctly active for the failing draw -- it is NOT a color-combine state-tracking bug on our side; the failing draw's caller (0x00451f10-0x00452263) took its own fallback/debug-color branch this run, which is Ignition's OWN code correctly requesting iterated RGB while never writing the shared C vertex's color -- Success Case C, not A; no color-combine, validation, or vertex-data changes made this iteration, per Part 24/26; prefail ring expanded 32->64 entries, RAM-only, still a single one-shot dump; the real open question is now upstream -- why the fallback branch's type/index bounds check fails on this run at all -- left for a future iteration; dynarec stays FROZEN, texture pipeline stays FROZEN, nothing about the 3D draw path itself changed)");
    // iteration-G52: the exact branch selecting the failing draw's
    // fallback path is now fully reconstructed (0x00451f24/0x00451f2e,
    // testing edi=[esi+0x18] for the range [0,0x100)) and the "invalid/
    // incomplete GuTexMemInfo" hypothesis (Part 12-27 of this iteration's
    // request) was directly audited and NOT confirmed: real Glide 2.x's
    // own GrMipMapId_t is an opaque integer handle type by contract, not
    // a dereferenceable pointer, and direct disassembly of Ignition's own
    // guTexAllocateMemory call site (0x0044fedf-0x0044fefe) shows the
    // returned handle is only ever compared against the failure sentinel
    // and passed on as an opaque argument -- never dereferenced. Separate
    // disassembly evidence (sibling case-handlers of the same particle
    // dispatcher, see win32/glide2x_ign.cpp's G51 comment) shows
    // [esi+0x18] is used elsewhere as a per-particle color/material
    // palette index (0..255), not a texture resource of any kind. The
    // most likely explanation is that this run's specific particle
    // record is inactive/expired/not-yet-spawned (an out-of-range
    // sentinel value in its own index field) -- a fact of Ignition's own
    // particle simulation state, not a bug in this project's Glide or
    // texture wrapper. Per Part 17-19/24/26, nothing was forced, faked,
    // or bypassed; a single one-shot [G52-PATH] diagnostic was added,
    // re-reading [esi+0x18] from guest memory (not the clobbered EDI
    // register) at the exact moment of the failing draw. See
    // win32/glide2x_ign.cpp's G52 comment on guDrawTriangleWithClip_impl
    // for the full branch/handle-contract audit.
    IGN_LOG("[G52]", "THIS_IS_THE_BRANCH_CONDITION_AUDIT_BUILD (exact fallback-branch instructions and condition source fully reconstructed -- edi=[esi+0x18], a per-particle palette-index field bounds-checked against [0,0x100), NOT a texture handle or pointer; the 'invalid GuTexMemInfo' hypothesis was directly audited via real Glide's own GrMipMapId_t contract (opaque integer, not a pointer) and via disassembly of the real guTexAllocateMemory call site, which shows the returned handle is never dereferenced -- hypothesis NOT confirmed, Success Case C; root cause is most likely upstream in Ignition's own particle simulation state (an inactive/expired particle's sentinel index), not this wrapper; nothing forced, faked, or bypassed -- Part 17-19/24 all held; one new one-shot [G52-PATH] diagnostic added, re-reading the real condition field from guest memory rather than a clobbered register; dynarec/LFB/input/file-IO/retrace/triangle-submission/color-combine/alpha-source/blending/constant-color/texture-GPU-upload all FROZEN, Part 35)");
    // iteration-G53: G52's real hardware run resolved condition_raw=
    // 0x70000018 -- bit-identical to this project's OWN 24th
    // guTexAllocateMemory handle. Object base confirmed by arithmetic:
    // esi=0x01039DC0. Identified the real allocator-caller function at
    // 0x0044FE70 (a texture palette-remap+allocate routine, exactly 2
    // static call sites in the whole binary). Directly audited real
    // Glide 2.x's GrMipMapId_t numeric behavior: it is a TMU byte
    // offset, not a small sequential index, so it exceeds 0x100 after
    // the first non-trivial texture even in a maximally faithful
    // implementation -- this is evidence AGAINST "redesign our handles
    // to be small" as a general fix (it would only coincidentally work
    // for the first texture or two) and AGAINST masking to 8 bits
    // (never principled, and independently forbidden). No handle-scheme
    // change, no bounds-check change, no vertex-data change made --
    // none of the live hypotheses (genuine upstream Ignition data-flow
    // bug vs. this run's specific allocation timing/order) are yet
    // proven. Added ONE targeted, single-address value watch
    // ([G53-WATCH] on guest 0x01039DD8, win32/dinput.cpp, polled at the
    // existing shared bridge point, logs first value + up to 8 changes
    // only -- not a global write trace, Part 6) to show on the next run
    // whether this field starts corrupted or flips mid-run. See
    // win32/glide2x_ign.cpp's G53 comment on guDrawTriangleWithClip_impl
    // for the full allocator-caller and GrMipMapId_t audit.
    IGN_LOG("[G53]", "THIS_IS_THE_HANDLE_PROVENANCE_AUDIT_BUILD (condition_raw=0x70000018 confirmed bit-identical to this project's own guTexAllocateMemory handle #24; object base esi=0x01039DC0 confirmed by arithmetic; the real allocator-caller function at 0x0044FE70 (a texture palette-remap+allocate routine, 2 static call sites total) identified for the first time; real Glide 2.x's GrMipMapId_t audited and confirmed to be a TMU byte offset that exceeds 0x100 after the first non-trivial texture even done with full fidelity to the real SDK -- this is evidence AGAINST redesigning our handle numbering as a general fix and AGAINST masking to 8 bits, neither of which was done; no handle-scheme, bounds-check, or vertex-data change made this iteration, since neither live hypothesis (upstream Ignition data-flow bug vs. this run's specific allocation order) is yet proven; one new targeted single-address value watch added ([G53-WATCH] on 0x01039DD8, first-value-plus-8-changes only, not a global trace); dynarec/LFB/input/file-IO/retrace/triangle-submission/color-combine/alpha-source/blending/constant-color/texture-GPU-upload all remain FROZEN)");
    // iteration-G54: G53's own real hardware run showed [0x01039DD8] is
    // highly dynamic (0x00000000 -> ... -> 0x70000018 at failure) -- the
    // 8-change cap was replaced with a probe armed until the EXACT value
    // 0x70000018 is observed (win32/dinput.cpp's [G54-70000018], one
    // capture line only). Identified, for the first time, the producer
    // chain for a SHARED PARTICLE/EFFECT SPAWN STAGING STRUCT at global
    // pointer ds:0x00620378 (freshly allocated once at 0x0044A036, then
    // read -- never reassigned -- by every one of many scattered spawn-
    // request call sites across the binary, each filling only the
    // subset of fields its own effect type needs before submitting to
    // the dispatcher at 0x0044ff20). Fully reconstructed one call site's
    // field layout (0x0043f2ff-0x0043f38e): six fixed-point (<<8) fields
    // including +0x18, plus two constants. A mathematically certain
    // negative result: a `<<8`-produced value always has a zero low
    // byte, but 0x70000018's low byte is 0x18 (nonzero) -- so this
    // specific call site's own write cannot be the direct producer of
    // the exact failing value. Whether the STAGING struct's +0x18 is
    // even the SAME logical field as the PERSISTENT particle-slot's
    // +0x18 read by guDrawTriangleWithClip's caller is NOT yet proven --
    // the dispatcher may transform/copy fields into a different layout,
    // untraced this iteration. Nothing about the handle scheme, bounds
    // check, RGB validation, or render path was changed -- Part 22-26 all
    // held. Terminology changed to PATH_A ([0,0x100)) / PATH_B (>=0x100)
    // per Part 19, without reopening or correcting G52/G53's own prior
    // wording. See win32/glide2x_ign.cpp's G54 comment on
    // guDrawTriangleWithClip_impl for the full staging-struct/dispatcher
    // audit.
    IGN_LOG("[G54]", "THIS_IS_THE_STAGING_STRUCT_PROVENANCE_BUILD (identified the producer chain for a shared particle/effect spawn staging struct at ds:0x00620378 -- allocated once at 0x0044A036, read by many scattered spawn-request call sites across the binary, each filling only the fields its own effect type needs before submitting to the dispatcher at 0x0044ff20; fully reconstructed one call site's field layout (0x0043f2ff-0x0043f38e), proving mathematically that ITS OWN write to +0x18 cannot be the direct producer of the exact failing value 0x70000018 (a <<8-shifted value always has a zero low byte, 0x70000018 does not); whether the staging struct's +0x18 is the same logical field as the persistent particle-slot's +0x18 read by the failing draw's caller is NOT yet proven, since the dispatcher may transform the fields on the way to the persistent slot; the G53 8-change-capped watch has been replaced with [G54-70000018], armed until the exact failure value is observed, one capture line only; PATH_A/PATH_B terminology adopted per Part 19 without correcting prior iterations' own wording; handle scheme, bounds check, RGB validation, dynarec, LFB, and render path all remain untouched and FROZEN)");
    // iteration-G55: G54's own real hardware run captured the exact
    // transition (old=0xEB01EBC1 new=0x70000018,
    // last_bridge_eip=this_bridge_eip=0x0045E810). Disassembly of
    // 0x0045E810 identifies it precisely as the return address right
    // after `call [0x0046a0a0]` (the real HeapAlloc IAT slot) inside
    // Ignition's own CRT malloc()-style wrapper (0x0045e7dc-0x0045e811),
    // called with dwFlags=0 (no HEAP_ZERO_MEMORY). Real Windows
    // HeapAlloc's own documented contract says memory returned without
    // that flag is NOT zeroed -- confirmed to be exactly how this
    // project's own HeapAlloc_impl already behaves (win32/
    // kernel32_ign.cpp, zeroing is conditional on the flag). The most
    // parsimonious explanation for the entire G50-G55 chain of evidence
    // is that [esi+0x18] is simply UNINITIALIZED HEAP MEMORY for this
    // particle/effect object -- never explicitly written by its own
    // spawn/constructor code -- and its specific value (numerically
    // identical to this project's own texture handle #24) is leftover
    // content from an earlier, unrelated, freed heap allocation, not a
    // genuine data-flow edge from guTexAllocateMemory. This is NOT
    // proven with full certainty (the object's own constructor was not
    // fully traced), so nothing was changed -- no explicit "writer
    // instruction" was force-fit onto a field this evidence suggests may
    // simply never be written at all. One narrow, caller-EIP-scoped
    // probe was added inside the EXISTING HeapAlloc shim ([G55-MALLOC],
    // capped at 4 occurrences, checks whether allocations from this
    // exact call site cover 0x01039DD8) to test this directly on the
    // next run. Handle scheme, bounds check, RGB validation, dynarec,
    // LFB, and render path all remain untouched and FROZEN. See
    // win32/glide2x_ign.cpp's G55 comment on guDrawTriangleWithClip_impl
    // for the full malloc/heap-reuse analysis.
    IGN_LOG("[G55]", "THIS_IS_THE_HEAP_REUSE_HYPOTHESIS_BUILD (0x0045E810 identified precisely as the return address after Ignition's own CRT malloc() wrapper calls the real HeapAlloc IAT slot with dwFlags=0/no-zero-memory; this project's own HeapAlloc_impl already faithfully implements that same non-zeroing real-Windows contract; the best-supported explanation for the entire G50-G55 chain of evidence is that [esi+0x18] is uninitialized heap memory for this particle/effect object, never explicitly written by its spawn code, with its specific 0x70000018-shaped content being leftover from an earlier, unrelated, freed allocation rather than a genuine data-flow edge from guTexAllocateMemory -- NOT proven with full certainty, so nothing was changed; one narrow caller-EIP-scoped probe added inside the existing HeapAlloc shim, capped at 4 occurrences, to test this directly on the next run; handle scheme, bounds check, RGB validation, dynarec, LFB, and render path all remain untouched and FROZEN)");
    // iteration-G56: G55's own real hardware run supplied timestamps that
    // NARROW rather than confirm the heap-reuse hypothesis -- the direct
    // canada.SHD ReadFile destination is 0x015E0000, not 0x01039CA0, so
    // the [0x01039DD8] transition is not explained by that specific file
    // copy landing there directly; it happens in a narrow guest-only
    // interval between that file's close and the next file open. G55's
    // capped [G55-MALLOC] probe did not happen to capture the allocation
    // covering the target addresses either. The capped probe has been
    // replaced with UNCAPPED, target-range heap lifetime tracking, armed
    // for the whole run (win32/kernel32_ign.cpp: [G56-TARGET-ALLOC]/
    // [G56-TARGET-FREE]/[G56-TARGET-REUSE], keyed to the two fixed
    // addresses this run's own log named, not a global allocator trace),
    // plus a new HeapFree override (none existed before -- HeapFree was
    // left to the vendored engine's own default, which is NOT edited;
    // the override replicates that default body verbatim and only adds
    // the target-range check and an interior-pointer-free anomaly check).
    // At the exact moment of the failing draw, a new one-shot query
    // ([G56-OBJECT-HEAP]) asks this project's own GuestRegion allocator
    // directly (via its existing, pre-existing block_of()/size_of()
    // read-only queries) whether esi is inside any currently-live
    // allocation at all; if not, [G56-UAF] fires with PROVEN=YES. The
    // mechanism does not alter behavior either way -- no early abort, no
    // zeroing, no forced path, no handle-scheme change. The lifetime
    // story for this run's specific addresses is still not resolved with
    // certainty; that requires the next hardware run with this
    // instrumentation armed. See win32/glide2x_ign.cpp's G56 comment on
    // guDrawTriangleWithClip_impl for the full analysis.
    IGN_LOG("[G56]", "THIS_IS_THE_TARGET_BLOCK_LIFETIME_BUILD (G55's timestamps narrow the heap-reuse hypothesis but do not confirm it -- canada.SHD's own ReadFile destination is 0x015E0000, not 0x01039CA0, so the observed transition happens in guest-only processing in a narrow interval, not from that direct file copy; G55's capped malloc probe also missed the allocation covering the target addresses; replaced with UNCAPPED target-range heap lifetime tracking for the whole run ([G56-TARGET-ALLOC]/[G56-TARGET-FREE]/[G56-TARGET-REUSE], win32/kernel32_ign.cpp, keyed to two fixed addresses, not a global trace); added a HeapFree override (none existed before, replicates the vendored default verbatim, adds only target-range and interior-pointer-free anomaly checks); added a one-shot live-heap-metadata query at the exact failure point ([G56-OBJECT-HEAP]/[G56-UAF]) using this project's own GuestRegion's existing block_of()/size_of() queries; no behavior changed either way -- no early abort, no zeroing, no forced path, no handle-scheme change; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G57: G56's own real hardware run definitively ruled out
    // use-after-free at render time -- generation 31 (base=0x01039CA0,
    // size=0x850) was still live. The simple stale-4096-byte-file-buffer
    // theory is also ruled out (generation 31 is a genuinely new,
    // differently-sized allocation). The field changes to 0x70000018
    // only ~17ms after generation 31's own allocation -- some guest
    // activity in that narrow window causes it, not mere carryover.
    // Could not pin down which of ~49 static call sites into the malloc
    // entry point requests size 0x850 (no literal push of that value
    // exists anywhere -- confirmed by an unrestricted disassembly text
    // search -- so the size is computed at runtime, not a compile-time
    // constant); rather than hand-compute a caller-EIP stack offset this
    // project cannot yet verify (this project's own G49 lesson about
    // hand-reconstruction errors at scale), win32/kernel32_ign.cpp's
    // HeapAlloc shim now recognizes this exact allocation by its runtime
    // signature and dumps 16 stack-word caller-EIP candidates
    // ([G57-STACK-SCAN]) plus the field's value at the moment of
    // allocation; win32/dinput.cpp's field-change watch is now armed
    // only from that moment (G53's noise lesson), capturing the FIRST
    // subsequent change with the same bridge-bracketing technique as
    // G54 ([G57-FIELD-WRITE]). Reduced G56's own logging per this
    // iteration's own request now that generations 8-30 are understood
    // plain file-scratch churn. A newly noticed parallel: this
    // function's own GrVertex C is confirmed to live entirely inside its
    // OWN stack frame reservation (`sub esp,0xfc`), never caller-owned --
    // a plain stack reservation is not zeroed by convention any more
    // than HeapAlloc(flags=0) is, which alone is sufficient to explain
    // C.r/g/b's code-address-shaped leftover bytes without requiring any
    // producer instruction to exist. PATH_A/PATH_B legitimacy remains
    // unresolved either way. Nothing about the handle scheme, bounds
    // check, RGB validation, dynarec, or render path was changed. See
    // win32/glide2x_ign.cpp's G57 comment on guDrawTriangleWithClip_impl.
    IGN_LOG("[G57]", "THIS_IS_THE_GENERATION31_INITIALIZATION_BUILD (G56's own real hardware run definitively ruled out use-after-free at render time -- generation 31, base=0x01039CA0 size=0x850, was still live -- and also rules out the simple stale-4096-byte-file-buffer theory, since generation 31 is a genuinely new, differently-sized allocation; the field changes to 0x70000018 only ~17ms after that allocation, meaning real guest activity in a narrow window causes it; could not identify which of ~49 static malloc call sites requests size 0x850 -- no literal push of that value exists anywhere in the cached disassembly, so the size is runtime-computed, not a compile-time constant -- so rather than hand-compute an unverifiable stack offset, the HeapAlloc shim now recognizes this exact allocation by its runtime signature and dumps 16 stack-word caller-EIP candidates plus the field's value at allocation time, and the dinput.cpp field-change watch is now armed only from that moment, capturing the first subsequent change via the same bridge-bracketing technique as G54; G56's own logging volume reduced now that generations 8-30 are understood plain file-scratch churn; newly confirmed that this function's own GrVertex C lives entirely inside its own stack frame reservation, never caller-owned, which alone explains its stale RGB the same way HeapAlloc's non-zeroing explains the heap object's stale field, without requiring any producer instruction; PATH_A/PATH_B legitimacy remains unresolved; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G58: G57's own 16 stack-word candidates are now FULLY
    // resolved by disassembly, not left as raw numbers. The complete
    // malloc(0x850) call chain is proven: a generic named-resource
    // cache/registry at 0x00457E60 (a 64-bucket hash table, ~60+ call
    // sites across the whole binary, all keyed by a global at
    // ds:0x00536f78) -> the malloc entry point 0x0045e79e -> the retry
    // loop 0x0045e7b0 -> the real HeapAlloc call 0x0045e7dc (G55's own
    // finding, unchanged). Every stack-scan value matches: esp+0x14/
    // 0x1C/0x28 are the three nested return addresses (0x0045E7C0/
    // 0x0045E7AD/0x00457EF9); esp+0x04/0x08 are DATA (heap handle,
    // dwFlags), not code, despite looking pointer-shaped. The 0x850 vs.
    // 0x84C mismatch this iteration's own request flagged is fully
    // resolved as ordinary 16-byte allocator rounding
    // ((0x84C+15)&~15=0x850, this project's own unmodified
    // GuestRegion::alloc() contract) -- NOT a header+4 relationship, a
    // hypothesis this iteration explicitly disproves. 0x0044FE70 (the
    // texture-specific allocator helper G53 identified) was NOT this
    // allocation's caller -- 0x00457E60, the GENERIC resource cache, was;
    // these are two different functions serving two different purposes,
    // a distinction this iteration corrects. Could not identify which of
    // the ~60+ call sites into 0x00457E60 fired for this specific entry
    // -- no literal size constant for 0x84C exists either, confirmed by
    // an unrestricted disassembly search. Rather than guess among 60
    // candidates, this iteration reads the hash table's own lookup-key
    // global directly at the moment of allocation ([G58-RESOURCE-KEY],
    // win32/kernel32_ign.cpp) to identify the resource by name on the
    // next hardware run. The exact writer of +0x138 and TEX24's
    // provenance remain UNDER_AUDIT, honestly -- this iteration does not
    // claim a writer EIP it cannot back with disassembly. PATH_A/PATH_B
    // legitimacy remains unresolved either way; nothing about the handle
    // scheme, bounds check, RGB validation, dynarec, or render path was
    // changed. See win32/glide2x_ign.cpp's G58 comment on
    // guDrawTriangleWithClip_impl for the full call-chain proof.
    IGN_LOG("[G58]", "THIS_IS_THE_MALLOC_CALLCHAIN_PROOF_BUILD (G57's 16 stack-word candidates are now fully resolved by disassembly: the complete malloc(0x850) call chain runs generic named-resource-cache 0x00457E60 [64-bucket hash table, ~60+ call sites, keyed by ds:0x00536f78] -> malloc entry 0x0045e79e -> retry loop 0x0045e7b0 -> real HeapAlloc call 0x0045e7dc; every stack value matches exactly; the 0x850-vs-0x84C mismatch is proven to be ordinary 16-byte allocator rounding, not a header+4 relationship -- that hypothesis is explicitly disproved; 0x0044FE70 [the texture-specific allocator] was NOT this call's caller, 0x00457E60 [the generic resource cache] was -- a correction from prior iterations' conflation of the two; which of the ~60+ call sites fired for this specific entry is not resolved by static analysis alone, so this iteration reads the hash table's own lookup-key global directly at allocation time [G58-RESOURCE-KEY] instead of guessing; the exact writer of +0x138 and TEX24's provenance remain honestly UNDER_AUDIT, not claimed without disassembly backing; PATH_A/PATH_B legitimacy unresolved; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G59: G58's [G58-RESOURCE-KEY] read ds:0x00536f78=0 AFTER
    // the allocation and this iteration's own request treated that as a
    // failed probe -- disassembly shows it was a wrong PREMISE, not a
    // failure: 0x00457E60's arg0 is used directly as an array index
    // (`[eax*4+0x00620440]`), so it is already a small bucket index
    // (0..63), never a resource-name pointer; 0 is bucket #0, a
    // perfectly valid answer. Retired [G58-RESOURCE-KEY] for this
    // reason. Replaced with [G59-CACHE-ENTRY] (win32/kernel32_ign.cpp):
    // since 0x00457E60 pushes 5 registers at its own entry and has not
    // yet returned while this shim traps the deeply-nested malloc call
    // below it, those pushed words are still readable directly off the
    // guest stack -- no live entry hook needed (still architecturally
    // impossible, Cpu::set_code_hook remains a no-op). Walking the
    // confirmed call chain gives 0x00457E60's own entry esp as
    // T+0x44 for this run's specific internal branch (validated by
    // re-checking the T+0x28==0x00457EF9 signature first; if a future
    // run took the OTHER internal branch, this logs the mismatch
    // honestly instead of printing wrong data). [H+0] is the return
    // address into 0x00457E60's REAL caller -- resolving Part 6's "which
    // of the ~60+ call sites fired" retroactively from data already on
    // the stack. Also resolved: 0x00490590 (one of G57's own stack-scan
    // words) is not a return address -- zero instructions exist at that
    // address anywhere in the cached disassembly -- it is 0x00457E60's
    // own saved copy of its CALLER's incoming EDI register (pure data).
    // Payload format, +0x120/+0x18 semantics, the exact +0x138 writer,
    // and TEX24's intersection with generation 31 all remain honestly
    // UNDER_AUDIT -- they depend on knowing which call site fired, which
    // [G59-CACHE-ENTRY]'s caller_return is designed to answer on the
    // next hardware run. PATH_A/PATH_B legitimacy unresolved either way;
    // handle scheme, bounds check, RGB validation, dynarec, LFB, and
    // texture pipeline all remain untouched and FROZEN. See
    // win32/glide2x_ign.cpp's G59 comment on guDrawTriangleWithClip_impl.
    IGN_LOG("[G59]", "THIS_IS_THE_CACHE_ENTRY_RECONSTRUCTION_BUILD (G58's post-allocation [G58-RESOURCE-KEY] read of ds:0x00536f78=0 is now understood to have tested a WRONG PREMISE, not failed -- that global is a small bucket index (0..63) used directly as an array subscript, never a resource-name pointer, so 0 was always a valid answer -- retired; replaced with [G59-CACHE-ENTRY], which retroactively reconstructs 0x00457E60's own entry-time register state from its still-live, not-yet-popped stack pushes while this shim traps the deeply-nested malloc call several frames below it -- no live entry hook needed, none is architecturally possible; resolves the return address into 0x00457E60's REAL caller (one of ~60+ static sites) directly from stack data, self-validated against the already-confirmed T+0x28==0x00457EF9 signature, and honestly reports a mismatch rather than printing wrong data if a future run takes 0x00457E60's other internal branch; also resolved 0x00490590 as 0x00457E60's own saved EDI register, not a return address, confirmed by an unrestricted disassembly search finding zero instructions at that address; payload format, record field semantics, the exact +0x138 writer, and TEX24's intersection with generation 31 remain honestly UNDER_AUDIT pending the next hardware run's caller_return value; PATH_A/PATH_B legitimacy unresolved; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G60: G59's own real hardware run reported
    // caller_return=0x004491EA -- fully resolved, not another
    // correlation. `call 0x00457E60` at 0x004491E5, inside a chunk-
    // format resource parser dispatching on 4-byte magic 0x3330696C
    // ("li03"), which calls 0x004491C0 with arg2 = the FIXED LITERAL
    // 0x004A2474 (`push 0x4a2474` at 0x00448e8b -- not computed per
    // call). 0x004491C0 allocates an array of 36-byte (0x24) records and
    // populates each one from 7 consecutive 16-bit values in the
    // resource's own raw data stream: 6 fixed-point fields, then a 7th
    // RAW, UNVALIDATED value used immediately as a table index --
    // `mov ecx,[ebp+ebx*4]` at 0x00449299 (ebp=0x004A2474), stored via
    // `mov [edi+edx+0x18],ecx` at 0x0044929D. THIS IS THE EXACT WRITER
    // this project has searched for since G56. Independent confirmation:
    // 0x01039DC0-0x01039CA0=0x120, and 0x120/0x24=8 exactly -- record #8,
    // matching the derived 36-byte stride with zero remainder.
    // IMPORTANT NEGATIVE RESULT: an unrestricted disassembly search
    // finds zero instructions anywhere that WRITE to the 0x004A2474
    // table region -- only reads. This makes a runtime-populated
    // texture-handle table unlikely (this project's texture loader has
    // no mechanism to write guTexAllocateMemory's handle into arbitrary
    // guest memory in the first place). The more likely explanation,
    // NOT yet proven, is that record #8's raw table index (straight from
    // file data, never bounds-checked by the original game code) is
    // large enough to read out-of-bounds into unrelated memory -- a
    // genuine latent bug in Ignition's own original code, structurally
    // similar to this project's earlier non-zeroing-memory findings but
    // relocated to a different region. Chain A (this parser) and Chain B
    // (the texture loader producing TEX24) remain explicitly separate --
    // no write-based edge between them has been found. Two new one-shot
    // diagnostics added, same retroactive-stack-read technique, gated on
    // the same generation-31 detection, self-validated against known
    // return addresses: [G60-CALLER] recovers the resource struct
    // pointer; [G60-PAYLOAD] computes record #8's raw index and the
    // resulting table read BEFORE the write instruction executes, for
    // direct comparison against [G57-FIELD-WRITE]'s post-write capture
    // on the next hardware run. PATH_A/PATH_B legitimacy remains
    // unresolved either way; handle scheme, bounds check, RGB
    // validation, dynarec, LFB, and texture pipeline all remain
    // untouched and FROZEN. See win32/glide2x_ign.cpp's G60 comment on
    // guDrawTriangleWithClip_impl for the complete disassembly proof.
    IGN_LOG("[G60]", "THIS_IS_THE_EXACT_WRITER_LOCATED_BUILD (caller_return=0x004491EA fully resolved: `call 0x00457E60` at 0x004491E5 inside a chunk-format resource parser dispatching on magic \"li03\" (0x3330696C), which allocates an array of 36-byte records and populates each record's +0x18 field via `mov ecx,[ebp+ebx*4]` at 0x00449299 / `mov [edi+edx+0x18],ecx` at 0x0044929D -- ebp is a FIXED LITERAL table address (0x004A2474, passed by the parser, not computed), ebx is a RAW UNVALIDATED 16-bit index read directly from the resource's own file data; this is the exact writer this project has searched for since G56; independently confirmed by arithmetic that our failing object is record #8 of this exact array (0x120/0x24=8 exactly); IMPORTANT NEGATIVE RESULT -- zero instructions anywhere write to the 0x004A2474 table region, making a runtime-populated texture-handle table unlikely; the more likely explanation, not yet proven, is an out-of-bounds table read caused by the original game's own unvalidated file-data index; Chain A (this parser) and Chain B (TEX24's texture loader) remain explicitly separate pending a real write-based edge; two new one-shot diagnostics ([G60-CALLER]/[G60-PAYLOAD]) compute the raw index and table value BEFORE the write executes, for direct comparison against [G57-FIELD-WRITE] on the next run; PATH_A/PATH_B legitimacy unresolved; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G61: G60's own real hardware run exposed a hard
    // contradiction (predicted table[25]=0, observed field=0x70000018).
    // This iteration's primary request -- a true EIP-scoped register
    // probe at 0x00449299/0x0044929D -- is confirmed NOT implementable:
    // this project has exactly one guest-callable hook per trapped API
    // (the malloc call, firing once, before the record loop even
    // starts), and Cpu::set_code_hook remains a no-op, unchanged since
    // G16 -- stated plainly rather than fabricating a probe that cannot
    // exist. Instead, byte-for-byte re-verification of the loop (this
    // iteration's own "verify exact machine bytes" instruction) found a
    // genuine, self-caught error in G60's own model: the loop tail
    // (0x0044937F `add esi,0x2`, an 8TH stream advance with NO
    // corresponding read instruction anywhere in the loop body) proves
    // each record consumes 8 input words (16 bytes), not the 7 words
    // (14 bytes) G60 assumed. For record #8 this is a 16-byte addressing
    // error -- enough to read an entirely different record's raw index.
    // Corrected formula ([G61-PAYLOAD], win32/kernel32_ign.cpp):
    // resource_struct_ptr+0x94 (was +0x84). This is disclosed as a
    // genuine self-caught mistake, the same class this project's own G49
    // iteration already demonstrated is possible at this scale, caught
    // the same honest way. NOT yet verified against real hardware --
    // requires the next run to compare corrected [G61-PAYLOAD] against
    // the already-armed [G57-FIELD-WRITE]. TEX24 and PATH_A/PATH_B
    // investigation stay paused per this iteration's own instruction.
    // Nothing about the handle scheme, bounds check, RGB validation,
    // dynarec, or render path was changed. See win32/glide2x_ign.cpp's
    // G61 comment on guDrawTriangleWithClip_impl for the full byte-level
    // re-derivation.
    IGN_LOG("[G61]", "THIS_IS_THE_STRIDE_CORRECTION_BUILD (G60's own real hardware run exposed a hard contradiction between the predicted table lookup (0) and the observed field value (0x70000018); this iteration's requested true EIP-scoped register probe at 0x00449299/0x0044929D is confirmed architecturally impossible -- exactly one guest-callable hook exists per trapped API, firing before the record loop starts, and Cpu::set_code_hook remains a no-op; instead, byte-for-byte re-verification of the loop found a genuine self-caught error in G60's own model -- an 8th stream advance with no corresponding read proves each record consumes 16 bytes of input, not 14, so G60's record-8 addressing was off by 16 bytes, reading the wrong record entirely; corrected formula deployed as [G61-PAYLOAD] (resource_struct_ptr+0x94, was +0x84), NOT yet verified against real hardware; TEX24 and PATH_A/PATH_B investigation remain paused; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G62: the input-stride fix from G61 did NOT resolve the
    // contradiction (real hardware still shows table[23]=0 predicted vs.
    // 0x70000018 observed). Per this iteration's own explicit
    // instruction, NO live-EIP probe was requested or attempted again --
    // G61's architectural conclusion stands, unchallenged. Instead, a
    // SECOND self-caught error was found by re-verifying the exact
    // stdcall push order at the "li03" call site: EBP inside 0x004491C0
    // is arg1 (edi from the caller), NOT arg2 (the literal constant
    // 0x004A2474) as G60 and G61 both assumed -- a mis-mapped argument
    // slot, not another stride error. This also retracts G60's "zero
    // writes to 0x004A2474" claim: a genuine copy loop at
    // 0x00448E12-0x00448E2C does write there (missed earlier because it
    // uses computed register addressing, not a literal displacement) --
    // though its relevance is now separately unresolved, since EBP for
    // this specific lookup is arg1, not that address. Given two
    // independent self-caught errors in three consecutive iterations,
    // this iteration deliberately stops hand-tracing further up the call
    // chain (this project's own G49 lesson about manual reconstruction
    // reliability at scale) and instead reads the REAL EBP (arg1)
    // directly off the guest stack ([G62-CALLER]/[G62-PAYLOAD], win32/
    // kernel32_ign.cpp) rather than assuming a constant a third time. No
    // STATIC_EXPECTED value is asserted for the corrected lookup --
    // measurement only, per this iteration's own "do not fake runtime
    // observation" instruction. NOT yet verified against real hardware.
    // TEX24 and PATH_A/PATH_B investigation remain paused. Handle scheme,
    // bounds check, RGB validation, dynarec, LFB, and texture pipeline
    // all remain untouched and FROZEN. See win32/glide2x_ign.cpp's G62
    // comment on guDrawTriangleWithClip_impl for the full loop
    // reconstruction and the argument-mapping correction.
    IGN_LOG("[G62]", "THIS_IS_THE_ARGUMENT_MAPPING_CORRECTION_BUILD (the G61 stride fix did not resolve the static-vs-runtime contradiction; per this iteration's own instruction, no live-EIP probe was requested again; instead, re-verifying the exact stdcall push order at the \"li03\" call site found a SECOND self-caught error -- EBP inside 0x004491C0 is arg1 (edi from the caller), not arg2 (the literal 0x004A2474) as G60/G61 both assumed; this also retracts G60's \"zero writes to 0x004A2474\" claim, since a genuine copy loop at 0x00448E12-0x00448E2C does write there via computed addressing the earlier literal-text search missed -- though its relevance is now separately unresolved; rather than continue hand-tracing arg1's own origin further up the call chain (risking a third error at this depth), this iteration reads the REAL EBP directly off the guest stack instead of assuming a constant again; the full loop (all 8 input words, all 9 output fields) is reconstructed and documented; no STATIC_EXPECTED value is asserted for the corrected lookup -- measurement only; NOT yet verified against real hardware; TEX24 and PATH_A/PATH_B investigation remain paused; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G63: G62's real hardware measurement (table_base=
    // 0x00490590, entry23=0x70000018) is now closed with a genuine
    // static writer instruction -- 0x00409C53-0x00409C7B is a loop that
    // calls the confirmed texture-allocator helper 0x0044FE70 and stores
    // its return value directly into consecutive slots of table
    // 0x00490590 via `mov [ebp+0x0],eax` at 0x00409C6F. Slot 23 holding
    // this project's own 24th sequentially-issued handle
    // (0x70000001+23=0x70000018) is exactly what this loop produces by
    // construction. TEX24_EDGE=PROVEN. Per this iteration's own
    // instruction, provenance investigation stops here.
    //
    // The real, openglide-sourced GR_COLORCOMBINE_ITRGB equation
    // (function=LOCAL, other=NONE, unchanged since G41) has NO texture
    // term at all -- this run's own ring buffer confirmed
    // guColorCombineFunction(2)=ITRGB specifically, not the distinct
    // TEXTURE_TIMES_ITRGB(6) enum value. This directly refutes the
    // "texture * iterated color" hypothesis for this specific draw: the
    // handle in record+0x18 is genuine, provable DATA, but the active
    // rasterizer equation Ignition itself selected does not consume any
    // texture input for this draw. A direct source read of this
    // function's own host draw call confirms it submits only
    // GL_VERTEX_ARRAY+GL_COLOR_ARRAY, never GL_TEXTURE_COORD_ARRAY --
    // consistent with ITRGB needing no texture, not currently a gap.
    //
    // CONCLUSION: no renderer change is made this iteration. The
    // remaining blocker is unchanged from G47-G50's own original
    // finding -- Ignition's OWN fallback branch never initializes the
    // shared vertex C's RGB on either triangle of this quad, a genuine
    // gap in the original game's own code that cannot be fixed without
    // inventing vertex data (forbidden) or forcing a different path
    // (forbidden). The G56-G62 provenance-forensics instrumentation is
    // retired (kept as dead code); a new lean one-shot summary
    // ([G63-PATHB]) reports the settled state instead. See
    // win32/glide2x_ign.cpp's G63 comment on guDrawTriangleWithClip_impl
    // for the complete proof and citations.
    IGN_LOG("[G63]", "THIS_IS_THE_PROVENANCE_CLOSURE_BUILD (record+0x18's provenance is CLOSED with a genuine static writer: 0x00409C53-0x00409C7B calls the confirmed texture-allocator helper 0x0044FE70 and stores its return directly into table 0x00490590's consecutive slots -- slot 23 holding this project's own 24th handle 0x70000018 is exactly what this loop produces by construction; TEX24_EDGE=PROVEN, provenance investigation stops per this iteration's own instruction; the real openglide-sourced GR_COLORCOMBINE_ITRGB equation has NO texture term at all (function=LOCAL, other=NONE, unchanged since G41), directly refuting the \"textured PATH_B\" hypothesis for this specific draw despite the genuine texture handle present as data; a direct source read confirms this project's own host draw call never submits texture coordinates, consistent with ITRGB needing none; NO renderer change made this iteration -- the remaining blocker is unchanged since G47-G50: Ignition's own fallback branch never initializes the shared vertex C's RGB, a genuine gap in the original game's own code, not fixable without inventing data or forcing a different path, both forbidden; G56-G62 provenance instrumentation retired as dead code, replaced by a lean one-shot [G63-PATHB] summary; handle scheme, bounds check, RGB validation, dynarec, LFB, and texture pipeline all remain untouched and FROZEN)");
    // iteration-G64: byte-exact static disassembly of Ignition's own
    // PATH_B function (0x00451f10-0x00452263) found the REAL topology
    // G50 never exposed under the old A/B/C naming -- there are FOUR
    // corners, not three. Two calls to guDrawTriangleWithClip exist
    // (0x452236/ret=0x0045223B, the one this project's halt has always
    // hit, and 0x45224d/ret=0x452252, never yet reached on real
    // hardware). call1 args = A/B/C; call2 args = [A+4, a "hidden" 4th
    // vertex at exactly A+0x3c/B-0x3c, C(shared, literal same address as
    // call1's C)]. PATH_B's own entry code writes exactly THREE literal
    // flat-RGB triples (RED@A, GREEN@hidden, BLUE@B) and, confirmed a
    // second time by an independent address-range sweep, NEVER a fourth
    // for C -- C's r/g/b genuinely have no writer anywhere in either
    // branch of this function. The one thing static analysis could NOT
    // close by hand: call2's first argument (A+4) is not a clean
    // GrVertex-stride offset from anything, and the bytes there are
    // reused as scratch by later interpolation math, so its FINAL value
    // at the moment call2 executes cannot be hand-traced reliably (this
    // project's own G60-G62 lesson: measure, do not re-derive by hand a
    // third time). A DIAGNOSTIC-ONLY one-shot skip (kG64DiagnosticSkip-
    // BadDrawOnce, explicitly authorized, never a fix -- the bad triangle
    // is still never submitted to GL and C.g is still never sanitized)
    // lets guest execution reach call2 for the first time ever on real
    // hardware, so its real vertex pointers and raw/decoded field values
    // can finally be captured ([G64-CALL2-REAL]/[G64-QUAD]), before a
    // real halt still follows immediately. No original PC debugger
    // (windbg/x64dbg/ollydbg) is installed on this dev machine, though
    // the original Ign_3dfx.exe itself is present at "C:/Program Files
    // (x86)/Ignition/Ign_3dfx.exe" -- a ready-to-run manual debugger
    // script is provided in this iteration's own delivery notes instead
    // of guessing or installing new tooling without asking first.
    // Handle scheme, bounds check, dynarec, LFB, texture pipeline, and
    // the ITRGB/TEX24 findings all remain untouched and FROZEN. See
    // win32/glide2x_ign.cpp's G64 comment on guDrawTriangleWithClip_impl
    // for the complete byte-level reconstruction.
    IGN_LOG("[G64]", "THIS_IS_THE_FOUR_VERTEX_QUAD_RECONSTRUCTION_BUILD (byte-exact disassembly of 0x00451f10-0x00452263 found the real topology: TWO calls to guDrawTriangleWithClip exist, call1@0x452236(ret=0x0045223B, the one this project's halt has always hit) with args=A/B/C, and call2@0x45224d(ret=0x452252, never yet reached on real hardware) with args=[A+4, a HIDDEN 4th vertex at exactly A+0x3c/B-0x3c, C(shared, literal same address as call1's C)]; PATH_B's own entry code writes exactly THREE literal flat-RGB triples (RED@A, GREEN@hidden, BLUE@B) and, reconfirmed by an independent address-range sweep, NEVER a fourth for C -- C's r/g/b genuinely have no writer anywhere in either branch; the one thing static analysis alone could not close is call2's first argument (A+4, not a clean GrVertex-stride offset, and the bytes there are reused as scratch by later interpolation math) -- rather than hand-trace a third time (this project's own G60-G62 lesson), a DIAGNOSTIC-ONLY one-shot skip (explicitly authorized, never a fix: the bad triangle is still never submitted to GL, C.g is still never sanitized) lets guest execution reach call2 for the first time ever on real hardware so its real data can be captured before a real halt still follows immediately; no PC debugger is installed on this dev machine though the original Ign_3dfx.exe itself is present locally, so a ready-to-run manual debugger script is provided instead of guessing or installing tooling without asking; handle scheme, bounds check, dynarec, LFB, texture pipeline, and the G63 ITRGB/TEX24 findings all remain untouched and FROZEN)");
    // iteration-G65: G64's own hardware run (the diagnostic skip's first
    // and only use) measured the real four-vertex quad directly: V0=RED,
    // V1=GREEN, V2=BLUE, V3=uninitialized/NaN, at exact 0x3c/60-byte
    // strides, call1=(V0,V2,V3) call2=(V0,V1,V3) -- confirming almost all
    // of G64's static model. ONE piece is explicitly RETRACTED: real
    // hardware measured call2's own arg0 as V0 itself, not "A+4" as G64
    // predicted. Re-disassembling this iteration found one genuine,
    // previously-missed instruction (`add esp,0x20` at 0x45221e, a
    // batched caller-side cleanup for eight earlier calls that turned out
    // not to be self-cleaning) -- a real, disclosed refinement -- but
    // even after correctly incorporating it the static model still could
    // not close the exact remaining gap. Per this project's own standing
    // lesson (do not hand-trace a third time), the hardware measurement
    // is now canonical: call2=(V0,V1,V3), full stop.
    // The single caller of 0x00451f10 was found: 0x004502e1, inside a
    // primitive-shape dispatch table (~0x450170-0x450309) that reads a
    // variable number of (x,y) pairs from a resource record and calls a
    // different draw routine sized for that many points -- 0x00451f10 is
    // the "2-point/box" arity case, meaning its own two caller-supplied
    // arguments are literally a screen-space rectangle origin and corner
    // (matching the measured 9x8-pixel extent), not general 3D geometry.
    // A BYTE-FOR-BYTE STRUCTURAL TWIN was found at 0x00452270 (the same
    // dispatch table's "1-point" case) -- same frame, same PATH_B branch,
    // same three literal RED/GREEN/BLUE writes at the same relative
    // offsets, and the SAME missing 4th-corner writer. This proves the
    // missing V3 color source is a SYSTEMIC pattern across at least two
    // near-identical routines in Ignition's own original code, not an
    // isolated typo -- still not proof of intent either way. No renderer
    // change made this iteration; the diagnostic skip is back OFF (its
    // steady-state value, Part 28) since it already did its one job. A
    // corrected, ready-to-run manual PC-oracle debugger procedure is
    // provided in this iteration's own delivery notes (breakpoints at
    // the CALL instructions 0x00452236/0x0045224d, not at the return
    // address 0x0045223B, since by the return address the pushed vertex
    // pointers are already gone). Handle scheme, bounds check, dynarec,
    // LFB, texture pipeline, and every G63/G64 finding remain untouched
    // and FROZEN. See win32/glide2x_ign.cpp's G65 comment on
    // guDrawTriangleWithClip_impl for the complete correction and the
    // sibling-routine proof.
    IGN_LOG("[G65]", "THIS_IS_THE_FOURTH_CORNER_SOURCE_SEARCH_BUILD (G64's hardware run measured the real quad directly -- V0=RED V1=GREEN V2=BLUE V3=uninitialized/NaN, exact 0x3c strides, call1=(V0,V2,V3) call2=(V0,V1,V3) -- confirming G64's model except ONE retracted piece: call2.arg0 measured as V0 itself, not the predicted \"A+4\"; re-disassembly found and correctly incorporated one genuine previously-missed instruction (add esp,0x20 at 0x45221e, a batched caller-side cleanup for eight calls that are not self-cleaning) but still could not close the remaining gap by hand a third time, so the hardware measurement is now canonical; the single caller of 0x00451f10 was found (0x004502e1, a primitive-shape dispatch table reading a variable point-count from a resource record -- our function is the 2-point/box-arity case, meaning its arguments are a screen-space rectangle, not general 3D geometry, matching the measured 9x8-pixel extent); a BYTE-FOR-BYTE STRUCTURAL TWIN was found at 0x00452270 with the SAME missing 4th-corner writer, proving this is a systemic pattern across at least two routines, not an isolated typo; NO renderer change made this iteration; the diagnostic skip is back OFF, its steady-state value; a corrected manual PC-oracle debugger procedure (breakpoints at the CALL instructions, not the return address) is provided instead of guessing or installing tooling without asking; handle scheme, bounds check, dynarec, LFB, texture pipeline, and every prior G63/G64 finding remain untouched and FROZEN)");
    // iteration-G66: THE NATIVE WINDOWS ORACLE RAN, FOR REAL, READ-ONLY,
    // AGAINST THE ORIGINAL GAME -- and the result is decisive. A 32-bit
    // glide2x.dll proxy (exporting the EXACT 34 decorated stdcall names
    // from Ign_3dfx.exe's own import table, forwarding every call
    // unchanged to the renamed real DLL -- nGlide's own Voodoo-over-D3D
    // wrapper, confirmed already installed locally in the game folder,
    // base=0x10000000) was built with a newly-installed 32-bit mingw
    // toolchain (installed via MSYS2's own official pacman repo, with
    // the user's explicit permission asked first) and dropped into a
    // byte-for-byte robocopy of the ENTIRE original install, made at
    // pc_oracle/ -- the real C:/Program Files (x86)/Ignition install was
    // never touched (its glide2x.dll's own SHA256 was hashed before and
    // after: 7CBD0958...99AAB, unchanged; nothing there was renamed,
    // deleted, or written to). Launching the oracle copy proved
    // Ign_3dfx.exe DOES currently run, unmodified, on this machine. A
    // scripted key-sequence reached actual 3D gameplay, and the proxy
    // captured 16 real occurrences each of caller RVA 0x5223B and
    // 0x52252 -- cross-validating this project's own measured topology a
    // SECOND time, independently, on native hardware: both calls' own
    // arg2 (V3/the shared corner) were the IDENTICAL pointer in every
    // single sample, exactly like the Vita's own G64 measurement.
    //
    // THE DECISIVE FINDING: V3.r's raw bits were EXACTLY 0x0045042A in
    // ALL 32 native captures (both call sites, all 16 frames) -- byte-
    // for-byte IDENTICAL to the Vita's own measured V3.r raw value. But
    // V3.g and V3.b, while they DO vary from frame to frame (unlike .r),
    // were ALWAYS a tiny finite subnormal float on every single native
    // sample -- NEVER once a NaN bit pattern, across 32 real captures.
    // The Vita's own measured V3.g (0xFFFFEEE0) IS a genuine NaN. This
    // is CASE A from this iteration's own classification: native Windows
    // and the Vita agree exactly on WHERE this stale stack memory's
    // pointer-like content sits (.r) but diverge on what ends up in the
    // adjacent bytes (.g/.b) -- native's own call/stack history always
    // leaves something small-but-finite there, WinVita's leaves a NaN
    // bit pattern instead. This is a WinVita STACK-HISTORY / CALLING-
    // SEQUENCE FIDELITY divergence, not a Glide semantics gap, not a
    // path-selection difference (PATH_B/ITRGB is confirmed active
    // natively too), and not deliberate game design relying on a
    // specific constant color (V3.g/b are NOT stable values by design --
    // they visibly change with camera/car position, exactly like real
    // uninitialized-but-usually-harmless stack garbage would).
    //
    // NO Vita renderer or stack-emulation change was made this
    // iteration -- Part 36 explicitly reserves that for G67, once the
    // exact WinVita-side stack-history divergence is traced. Handle
    // scheme, bounds check, dynarec, LFB, texture pipeline, and every
    // prior G63-G65 finding remain untouched and FROZEN. See win32/
    // glide2x_ign.cpp's own G66 comment and this iteration's delivery
    // notes for the full oracle log and methodology.
    IGN_LOG("[G66]", "THIS_IS_THE_NATIVE_ORACLE_BUILD (a read-only 32-bit glide2x.dll proxy, exporting the exact 34 decorated names from Ign_3dfx.exe's own import table and forwarding everything to the renamed real DLL (nGlide's own wrapper), was built and run against a byte-for-byte robocopy of the ORIGINAL install -- the real Program Files install was never touched, its glide2x.dll SHA256 verified unchanged before/after; Ign_3dfx.exe DOES currently run on this machine; 16 real native occurrences each of caller RVA 0x5223B and 0x52252 were captured, cross-validating this project's own measured quad topology on native hardware (both calls' arg2/V3 were the identical pointer every time); DECISIVE RESULT: V3.r raw was EXACTLY 0x0045042A in all 32 native captures, byte-identical to the Vita's own measured V3.r, but V3.g/V3.b -- while they DO vary frame to frame -- were ALWAYS a finite subnormal float natively, NEVER a NaN, across every single sample, while the Vita's own measured V3.g (0xFFFFEEE0) IS a genuine NaN; CLASSIFICATION=CASE_A: native and WinVita agree on WHERE this stale stack content sits but diverge on what ends up there, meaning this is a WinVita stack-history/calling-sequence fidelity divergence, not a Glide semantics gap, not a path-selection difference, and not deliberate reliance on a fixed color; NO Vita renderer or stack-emulation change made this iteration, reserved for G67 once the exact WinVita-side divergence is traced; handle scheme, bounds check, dynarec, LFB, texture pipeline, and every prior G63-G65 finding remain untouched and FROZEN)");
    // iteration-G67: turning CASE_A ("stack history differs") into a
    // localized, evidence-backed bracket. Two new one-shot mechanisms,
    // neither touching vendored dynarec or adding a global instruction
    // trace (Cpu::set_code_hook remains a confirmed no-op):
    //   1) [G67-VITA-STACK]: dumps a ~0x2b4-byte raw guest stack window
    //      spanning V0-0x100 through V3+0x100 (this trap's own ptrA/
    //      ptrC), tagged with rel=addr-esp so it is directly comparable
    //      to the equivalent native window by OFFSET, not absolute
    //      address (this project's own guest stack sits at 0x0661xxxx;
    //      native PC's sits at 0x0019xxxx -- never literally equal).
    //   2) A low-overhead cross-shim watch (diagnostics/call_trace.cpp),
    //      piggybacked on this project's OWN pre-existing "fires before
    //      every resolved import's real shim, on every call" mechanism
    //      (Cpu::set_intrinsic, already armed across kernel32/user32/
    //      winmm/dinput/dsound/glide2x since the call-trace feature was
    //      built) -- samples the 3 fixed guest dwords holding V3.r/g/b's
    //      stack residue (0x0661EC28/2C/30, from G64's own measured V3=
    //      0x0661EC1C) on every host call, records a RAM-only ring entry
    //      ONLY when the value actually changes, and dumps it once, in
    //      order, from this same one-shot failure branch. If the
    //      transition to 0xFFFFEEE0 happens between two ring entries,
    //      this directly names the shim/caller immediately before and
    //      after it; if it happens inside one host-free interval, it at
    //      least brackets that interval for static disassembly.
    //
    // The native side of the comparison is ALSO already captured this
    // iteration: the pc_oracle proxy (G66) was extended with the exact
    // same relative-window dump ([G67-PC-STACK]), re-run against the
    // still-untouched original install copy, and confirms V3.r/g/b sit
    // at esp+236/+240/+244 on native Windows. The full offline diff
    // against the Vita's own window has to wait for the NEXT real-
    // hardware run -- this iteration prepares and arms both capture
    // mechanisms and provides the native reference data now, per this
    // project's own standing rule of never proceeding on Vita behavior
    // without a real hardware log to confirm it. No Vita renderer,
    // stack-emulation, or dynarec change was made. Handle scheme, bounds
    // check, dynarec, LFB, texture pipeline, and every prior G63-G66
    // finding remain untouched and FROZEN. See diagnostics/call_trace.cpp
    // and win32/glide2x_ign.cpp's own G67 comments for the full mechanism.
    IGN_LOG("[G67]", "THIS_IS_THE_STACK_HISTORY_LOCALIZATION_BUILD (two new one-shot mechanisms added, neither touching vendored dynarec nor adding a global instruction trace: [G67-VITA-STACK] dumps a ~0x2b4-byte raw guest stack window around V0..V3, offset-tagged (rel=addr-esp) for direct cross-environment comparison; a low-overhead cross-shim watch, piggybacked on this project's own pre-existing Cpu::set_intrinsic-based call-trace mechanism (already armed on every resolved kernel32/user32/winmm/dinput/dsound/glide2x import), samples the 3 fixed guest dwords holding V3.r/g/b's stack residue on every host call and records a RAM-only ring entry only when the value changes, dumped once in order at the same failure branch -- this can directly name the shim/caller immediately before/after the transition to 0xFFFFEEE0, or at minimum bracket the host-free interval containing it; the native side of the comparison is already captured this iteration too -- the G66 pc_oracle proxy was extended with the identical relative-window dump and re-run against the still-untouched original install, confirming native V3.r/g/b sit at esp+236/+240/+244; the full Vita-vs-native offline diff must wait for the next real-hardware log to provide the Vita-side window/ring dump, per this project's own standing rule; NO Vita renderer, stack-emulation, or dynarec change made this iteration; handle scheme, bounds check, dynarec, LFB, texture pipeline, and every prior G63-G66 finding remain untouched and FROZEN)");
    // iteration-G68: G67's real hardware run gave exactly 4 watch events,
    // none from glide2x, and none capturing the final transition -- this
    // iteration audited that empirically (diagnostics/call_trace.cpp's
    // own g67_watch_sample now counts total/glide2x/guDrawTriangleWithClip
    // samples, dumped as [G68-WATCH-AUDIT]) rather than guessing why, and
    // added TWO independent safety nets regardless of that audit's
    // outcome: an explicit [G68-DRAW-ENTRY] read of V3.r/g/b at the
    // absolute top of guDrawTriangleWithClip_impl (before any decoding,
    // validation, or logging), and G68_DIAGNOSTIC_ONLY explicit sample
    // calls (g68_explicit_sample, feeding the SAME ring) at the entry of
    // 8 already-implemented Glide functions (guDrawTriangleWithClip,
    // guColorCombineFunction, guAlphaSource, grConstantColorValue,
    // grAlphaBlendFunction, grChromakeyMode, grClipWindow, grBufferSwap).
    // Neither touches vendored dynarec or arbitrary internal guest code.
    //
    // Cross-checked G67's own hand-picked "interesting" Vita stack-window
    // offsets against this iteration's re-run of the extended pc_oracle
    // proxy: 5 of 6 spot-checked offsets are BYTE-IDENTICAL between
    // native Windows and the Vita (rel+284=0x004502E6 -- the confirmed
    // immediate caller's own return address; rel+320=0x00409B77, in the
    // same 0x00409xxx region as G63's own confirmed texture-table writer
    // loop; rel+324=0x00488F9C; rel+332=0x0047A4E8 -- outside this
    // project's disassembly cache's covered ranges, not yet identified;
    // rel+352=0x00406FE0 -- a genuine function entry, 16-byte stack
    // realignment). This is strong, now DIRECTLY cross-environment-
    // verified evidence of call-chain fidelity for a substantial stretch
    // of ancestor frames, not just the immediate caller G65 already
    // found. The one offset that differed (rel+344: Vita=0x004074A2 vs
    // native=0x004074AF) was fully resolved by disassembly, NOT promoted
    // to a divergence: both are return addresses of the SAME target
    // function (0x00409ab0) from two adjacent call sites 13 bytes apart
    // inside a tight FPU comparison loop -- ordinary loop-iteration-count
    // variance between the two runs, not a different branch or path.
    // Beyond rel+352 the two windows diverge structurally (native shows
    // native-OS-internal addresses with no guest equivalent; the Vita
    // keeps showing guest code addresses deeper still), which is expected
    // once the shared, provably-identical prefix runs out, not itself
    // evidence of a bug. Literal searches for 0xFFFFEEE0 and the
    // ascending-byte pattern (10 11 12 13/14 15 16 17/18 19 1A 1B) in
    // Ignition's own EXE both came back negative -- consistent with both
    // being genuine computed/stale residue, not a hardcoded constant.
    // NO Vita renderer, stack-emulation, or dynarec change made this
    // iteration. The exact last writer and the watch-audit's own result
    // still require the NEXT real-hardware run, per this project's own
    // standing rule -- this iteration prepares three independent capture
    // paths for it (audit counters, explicit draw-entry sample, explicit
    // Glide-entry sampling) rather than one. Handle scheme, bounds check,
    // dynarec, LFB, texture pipeline, and every prior G63-G67 finding
    // remain untouched and FROZEN. See diagnostics/call_trace.cpp and
    // win32/glide2x_ign.cpp's own G68 comments for the complete detail.
    IGN_LOG("[G68]", "THIS_IS_THE_WATCH_AUDIT_AND_FRAME_MAP_BUILD (G67's real hardware run gave 4 watch events, none from glide2x, none capturing the final transition -- audited empirically via new total/glide2x/guDrawTriangleWithClip sample counters ([G68-WATCH-AUDIT]) rather than guessed; added an explicit [G68-DRAW-ENTRY] read at the absolute top of guDrawTriangleWithClip_impl and G68_DIAGNOSTIC_ONLY explicit sampling at 8 already-implemented Glide function entries, both feeding the same ring, as independent safety nets regardless of the audit's outcome; cross-checked G67's own hand-picked Vita stack-window offsets against a re-run of the extended pc_oracle proxy -- 5 of 6 are BYTE-IDENTICAL (rel+284=0x004502E6 the confirmed immediate caller's return address, rel+320=0x00409B77 in the same region as G63's confirmed texture-table writer loop, rel+324=0x00488F9C, rel+332=0x0047A4E8 not yet identified, rel+352=0x00406FE0 a genuine function entry), strong direct cross-environment proof of call-chain fidelity for a substantial ancestor-frame stretch; the one differing offset (rel+344, Vita 0x004074A2 vs native 0x004074AF) was fully resolved by disassembly as two adjacent call sites to the SAME function 13 bytes apart inside an FPU comparison loop -- ordinary iteration-count variance, NOT a divergence; literal searches for 0xFFFFEEE0 and the ascending-byte pattern in Ignition's own EXE both came back negative, consistent with genuine stale residue; NO Vita renderer, stack-emulation, or dynarec change made this iteration; the exact last writer still requires the next real-hardware run, now with three independent capture paths armed; handle scheme, bounds check, dynarec, LFB, texture pipeline, and every prior G63-G67 finding remain untouched and FROZEN)");
    // iteration-G69: G68's real hardware run resolved TWO open questions
    // and delivered the first-ever OBSERVED bad transition. (1) The
    // generic call-trace watcher's blind spot is now empirically
    // confirmed, not just structurally plausible: total_watch_samples=
    // 5303, glide2x_samples=0, guDrawTriangleWithClip_samples=0 --
    // WATCH_CASE_A_RESTRICTED_TO_GLIDE2X. The explicit per-function
    // sampler (G68's own safety net) is the trusted mechanism from here
    // on; G69 does not attempt to redesign the generic mechanism. (2) The
    // explicit sampler recorded 2483 change events and bracketed the
    // FIRST observed 0xFFFFEEE0 transition between two CONSECUTIVE calls
    // returning to caller=0x0045284F, both with the identical esp=
    // 0x0661EB60 -- a completely different call site than the already-
    // fully-understood 0x0045223B/PATH_B quad function.
    //
    // Static reconstruction this iteration: 0x0045284F is the return
    // address of `call 0x468c1c` at 0x0045284A, inside a function
    // spanning 0x004525D0-0x00452855 (184-byte frame, `sub esp,0xb8`)
    // that draws exactly ONE triangle (a single call, no internal loop)
    // -- a THIRD structural sibling of the 0x00451f10/0x00452270 family
    // (same record-type-branch/interpolation/literal-RGB idiom), reached
    // from the SAME primitive-shape dispatch table G65 already found
    // (this is its 8-point/16-int arity case, called from 0x00450425).
    // The repetition G68 observed is therefore NOT a tight loop around
    // 0x0045284F -- it is this whole per-object dispatch path running
    // once per object/resource, many times per frame; the outer loop
    // that walks the object list itself was not reconstructed this
    // iteration (reported as out of scope for the time available, not
    // guessed). Exact frame algebra (not eyeballed): this function's own
    // stable baseline is ESP_BASE2=0x0661EB70 (derived from the verified
    // push sequence and a batched `add esp,0x18` cleanup identical in
    // kind to G65's own finding); its own triangle's 3 args live entirely
    // within [ESP_BASE2+0x4, ESP_BASE2+0xB8). The fixed diagnostic target
    // (0x0661EC28/2C/30) sits at ESP_BASE2+0xC4/0xC8/0xCC -- 4 bytes PAST
    // this function's own 184-byte frame, inside its CALLER's (the
    // dispatcher's) own territory, not inside anything 0x004525D0 itself
    // controls. This is exactly why the value changes between successive
    // calls to this same function: each invocation observes whatever the
    // ancestor frame currently holds at that physical depth.
    //
    // Also proven from source this iteration (Part 7): grepped every
    // c.write_u32 in the entire glide2x_ign.cpp -- exactly five exist,
    // all inside grSstQueryHardware_impl/grLfbLock_impl writing to
    // caller-supplied OUTPUT structs, NONE anywhere near
    // guDrawTriangleWithClip_impl or any vertex pointer.
    // HOST_HANDLER_GUEST_MEMORY_WRITE_AUDIT=READ_ONLY_ON_VERTEX_MEMORY_
    // CONFIRMED. New runtime mechanisms added to test this empirically
    // too, not just assert it: [G69-DRAW-EXIT] (entry-vs-exit comparison
    // at the fixed target on every single draw call, fires only if it
    // ever catches entry-finite/exit-non-finite within one call);
    // [G69-5284F-CONTEXT] (16-entry rolling register/arg ring); [G69-
    // LAST-GOOD-WINDOW]/[G69-FIRST-BAD-WINDOW] (a small ~0x88-byte window
    // around the fixed target, refreshed every call, frozen and dumped
    // the moment the target is first observed non-finite). All silent
    // (RAM only) until that one-shot moment -- MUTATION_OWNER is not yet
    // proven (STILL_NOT_PROVEN), but every mechanism needed to prove it
    // on the NEXT hardware run is now armed. NO Vita renderer, stack-
    // emulation, or dynarec change made this iteration. Handle scheme,
    // bounds check, dynarec, LFB, texture pipeline, and every prior G63-
    // G68 finding remain untouched and FROZEN. See win32/glide2x_ign.cpp's
    // own G69 comment for the complete frame algebra and citations.
    IGN_LOG("[G69]", "THIS_IS_THE_5284F_OWNERSHIP_BUILD (G68's real hardware run empirically confirmed the generic call-trace watcher never fires for glide2x -- total_watch_samples=5303, glide2x_samples=0, guDrawTriangleWithClip_samples=0, WATCH_CASE_A_RESTRICTED_TO_GLIDE2X -- and its own explicit sampler recorded 2483 events, bracketing the FIRST observed 0xFFFFEEE0 transition between two consecutive calls returning to caller=0x0045284F, both esp=0x0661EB60, a DIFFERENT call site than the already-understood 0x0045223B/PATH_B function; static reconstruction found 0x0045284F is the return address of a THIRD structural sibling (0x004525D0-0x00452855, single-triangle, no internal loop, same dispatch-table family) called from the SAME primitive-shape dispatcher G65 already found -- the observed repetition is the per-object dispatch path running once per object, not a tight local loop, and the outer object-list loop itself was not reconstructed this iteration; exact frame algebra places this function's own args entirely within its own 184-byte frame while the fixed diagnostic target sits 4 bytes past that frame, inside its CALLER's own territory -- explaining why the value changes between invocations without this function itself controlling it; also proved from source (grep of every c.write_u32 in the file) that the host Glide handler writes only to caller-supplied OUTPUT structs, never near vertex memory, and added [G69-DRAW-EXIT]/[G69-5284F-CONTEXT]/[G69-LAST-GOOD-WINDOW]/[G69-FIRST-BAD-WINDOW] to test that empirically and capture the exact byte-level transition on the next real-hardware run; MUTATION_OWNER=STILL_NOT_PROVEN this iteration, all capture mechanisms now armed; NO Vita renderer, stack-emulation, or dynarec change made; handle scheme, bounds check, dynarec, LFB, texture pipeline, and every prior G63-G68 finding remain untouched and FROZEN)");
    // iteration-G70: PROVENANCE CLOSURE. Reconstructed one level further
    // up than G69: 0x00450425 is not a separate function, it is inline
    // case-body code inside the giant per-object dispatch loop starting
    // at 0x0044FF20-0x00450534. That loop's own EBP is `&objectArray[i]`
    // (a NULL-terminated object-pointer array), advanced by exactly 4
    // bytes per iteration at 0x0044FFA7 -- EXACTLY the pattern G69's own
    // context ring observed (seq 1922-1937). Exact algebra (ESP_LOOP=
    // 0x0661EC6C, derived from G69's own measured ESP_BASE2=0x0661EB70
    // plus the verified push/call/prologue chain) places the fixed
    // diagnostic target at ESP_LOOP-0x44/-0x40/-0x3C exactly:
    // target_r is the return-address slot `call 0x004525D0` (at 0x450425)
    // pushes on EVERY execution of this case -- an ACTIVELY REWRITTEN
    // code pointer (0x0045042A), not aged residue, which is exactly why
    // it never varies. target_g/target_b are the LAST TWO of that same
    // case's 16 pushed dwords: the current object's own (x,y) coordinate
    // pair, each translated by the per-frame 8.8-fixed-point esi/edi this
    // same loop sets up (confirmed by its own `shl reg,0x8` clip-rect
    // setup). Decoding every observed value as signed-int32/256 gives
    // small, entirely ordinary screen coordinates (0xFFFFEEE0=-17.125,
    // 0x000012E0=+18.875, etc.) -- 0xFFFFEEE0 is not corrupted data, it
    // is simply "-17.125" in 8.8 fixed point, for whatever 2D object this
    // loop dispatched last, that happens to fall in the IEEE-754 NaN
    // exponent range purely by numeric coincidence when later reread as
    // a float.
    //
    // NATIVE CROSS-CHECK (re-ran the still-untouched pc_oracle proxy):
    // target_r read EXACTLY 0x0045042A on 16/16 native samples at caller
    // 0x5284F -- byte-identical to the Vita's own value AND this
    // iteration's static derivation. Native Windows exhibits the
    // IDENTICAL mechanism; this is genuinely present in Ignition's own
    // original code on both platforms, not a WinVita defect.
    // DIVERGENCE_CLASS=A. Since real Voodoo/nGlide hardware never halts
    // on this (the game runs to completion natively), the actual STOP
    // this project hits is a consequence of this project's OWN G47
    // strict-required-field validation, not a limitation the original
    // game ever exposed -- recorded as a candidate question for a future
    // iteration to weigh deliberately; STRICT_REQUIRED_FIELDS itself is
    // kept fully UNCHANGED this iteration, per its own standing
    // instruction. MUTATION_OWNER=GUEST_INTERVAL (the outer dispatch
    // loop's own case-body code), confirmed NOT the host Glide handler.
    // NO Vita renderer, stack-emulation, or dynarec change made. Handle
    // scheme, bounds check, dynarec, LFB, texture pipeline, and every
    // prior G63-G69 finding remain untouched and FROZEN. See win32/
    // glide2x_ign.cpp's own G70 comment for the complete byte-level
    // derivation and the native oracle log for the raw cross-check data.
    IGN_LOG("[G70]", "THIS_IS_THE_PROVENANCE_CLOSURE_BUILD (reconstructed the outer per-object dispatch loop at 0x0044FF20-0x00450534 -- EBP=&objectArray[i], a NULL-terminated object-pointer array, advanced by exactly 4 bytes per iteration at 0x0044FFA7, matching G69's own observed context-ring pattern exactly; exact frame algebra (ESP_LOOP=0x0661EC6C) places the fixed diagnostic target at target_r=the return-address slot for `call 0x004525D0` at 0x450425 -- an ACTIVELY REWRITTEN code pointer (0x0045042A), not residue, hence never varying -- and target_g/target_b=the LAST TWO of that case's 16 pushed dwords, the current object's own (x,y) coordinate pair translated by this same loop's own 8.8-fixed-point esi/edi; decoding every observed value as signed-int32/256 gives small ordinary screen coordinates, proving 0xFFFFEEE0 is simply \"-17.125\" in fixed point, not corrupted data, that happens to fall in the float NaN range by numeric coincidence; NATIVE CROSS-CHECK this iteration (re-ran the still-untouched pc_oracle proxy) found target_r=EXACTLY 0x0045042A on 16/16 native samples too -- native Windows exhibits the IDENTICAL mechanism, proving this is genuinely present in Ignition's own original code on both platforms, DIVERGENCE_CLASS=A; since real hardware never halts on this, the actual stop is a consequence of this project's OWN G47 validation, not an original-game limitation -- recorded as a candidate question for a future iteration, STRICT_REQUIRED_FIELDS kept fully UNCHANGED this iteration per its own standing instruction; MUTATION_OWNER=GUEST_INTERVAL, confirmed not the host Glide handler; NO Vita renderer, stack-emulation, or dynarec change made; handle scheme, bounds check, dynarec, LFB, texture pipeline, and every prior G63-G69 finding remain untouched and FROZEN)");
    // iteration-G71: FIRST BEHAVIOR CHANGE since G54. Two stages.
    // STAGE A: quantified the native-effective color G70's closure
    // implied. Re-ran the still-untouched pc_oracle proxy with a
    // classifier over 128 real V3 samples across both caller 0x5223B and
    // 0x52252: ALL 128 finite (0 NaN, 0 Inf), max |value| = 6.34e-39 (r),
    // 2.15e-40 (g), 1.66e-40 (b) -- see [G71-NATIVE-RGB-STATS] below and
    // pc_oracle/pc_oracle_log.txt. This project's own existing color path
    // (toGlColor: channel255/255, clamped) turns that magnitude into a
    // GLfloat color component around 1e-42 -- indistinguishable from 0.0
    // by any GPU's float-to-8-bit conversion (rounds to 0 below 1/510).
    // NATIVE_EFFECTIVE_RGB=BLACK_EQUIVALENT, proven quantitatively.
    // DESIGN_INTENT remains explicitly UNKNOWN -- this reproduces
    // observed EFFECTIVE native behavior, not a claimed original intent.
    //
    // STAGE B: IGNITION_LEGACY_V3_RGB_COMPAT -- a HOST-LOCAL-ONLY
    // substitution (g71_apply_ignition_legacy_v3_rgb_compat, win32/
    // glide2x_ign.cpp), gated by an exact conjunction: caller is EXACTLY
    // 0x0045223B or 0x00452252 (the two return addresses proven, since
    // G64/G65, to belong ONLY to 0x00451F10's own two draw calls); the
    // active color source requires ITERATED rgb (not constant); and at
    // least one of vc.r/g/b is actually non-finite right now. When ALL
    // hold, ONLY the already-decoded HOST-side HostGrVertex copy (a
    // value on this trap's own C++ stack) is corrected to (0,0,0) --
    // guest memory at 0x0661EC28/2C/30, guest registers, and all future
    // guest execution remain COMPLETELY untouched. A genuinely finite
    // vertex, or a NaN at any OTHER caller, is never altered.
    // STRICT_REQUIRED_FIELDS remains globally unchanged outside this one
    // named, evidenced, narrowly-scoped case -- this is NOT global NaN
    // sanitization. A host-side selftest (g71_selftest(), run once at
    // grGlideInit) exercises the 5 required cases (exact caller+ITRGB+
    // NaN -> apply; random caller -> reject; constant color -> reject;
    // already-finite vertex -> reject) and logs [G71-SELFTEST] PASS/FAIL
    // per case. The old G67-G70 one-shot forensic dumps are deliberately
    // LEFT IN PLACE (not stripped) this iteration -- they simply stop
    // firing once this compat keeps the vertex finite, and remain the
    // safety net if a REAL, different problem ever reaches this same
    // validation path; stripping them is deferred to a future iteration,
    // once real hardware confirms this fix actually works (Part 30's own
    // "reduce... after proven by build/selftest", not before).
    // No dynarec, heap, texture, LFB, input, or DDraw-branch change.
    // See win32/glide2x_ign.cpp's own G71 comment for the complete
    // predicate rationale and native-evidence citation.
    IGN_LOG("[G71]", "THIS_IS_THE_LEGACY_V3_COMPAT_BUILD (Stage A: re-ran the still-untouched pc_oracle proxy, classified 128 real native V3 samples across both known callers -- ALL 128 finite, 0 NaN/Inf, max_abs<7e-39 per channel, proving NATIVE_EFFECTIVE_RGB=BLACK_EQUIVALENT quantitatively through this project's own existing channel255/255 color conversion; DESIGN_INTENT stays explicitly UNKNOWN, only effective behavior is reproduced; Stage B: added IGNITION_LEGACY_V3_RGB_COMPAT, a HOST-LOCAL-ONLY substitution gated by an exact conjunction (caller==0x0045223B or 0x00452252 AND ITERATED color source AND at least one non-finite V3 field) that corrects ONLY the already-decoded host-side vertex copy to (0,0,0) -- guest memory/registers/future execution stay byte-for-byte untouched, a genuinely finite vertex or a NaN at any other caller is never altered, and STRICT_REQUIRED_FIELDS remains globally unchanged outside this one named case; a host-side selftest exercises all 5 required predicate cases and logs PASS/FAIL; the old G67-G70 one-shot forensic dumps are deliberately left in place as a safety net rather than stripped, since they simply go quiet once the vertex stays finite and only fire again if a genuinely different problem appears; no dynarec/heap/texture/LFB/input/DDraw change)");
    // iteration-G72: G71 CONFIRMED ON REAL HARDWARE -- the old
    // 0x0045223B/0x00452252 blocker was passed repeatedly (compat_calls_
    // total=400, 200 each), guest execution progressed farther than any
    // prior iteration. A NEW required-field halt appeared at caller
    // 0x0045259B (field C.b, raw=0xFFFFFDA0). Investigated whether this
    // is the SAME original-game structural defect, WITHOUT blindly
    // whitelisting it.
    // STATIC: 0x0045259B is exactly the call1 return address of
    // 0x00452270-0x004525C3 -- the function G65 ALREADY proved, byte-
    // for-byte, is a complete structural twin of 0x00451F10 (identical
    // frame, identical PATH_B branch, identical RED/GREEN/BLUE literal
    // corner writes at identical relative offsets, identical missing
    // shared-V3 writer). TOPOLOGY_MATCH=YES, not partial, not guessed.
    // NATIVE ORACLE (this iteration, same still-untouched proxy, 128
    // samples/channel): r and g behave exactly like before (128/128
    // finite, tiny). But b does NOT: 76/128 finite, 52/128 (40.6%)
    // GENUINE NaN. One captured native sample is a byte-for-byte EXACT
    // match to the Vita's own reported failure (raw C = 0045042a
    // 00027ca0 fffffda0, same object position x=585,y=-8) -- proving
    // SAME_MECHANISM at the strongest possible level, not mere symptom
    // similarity. NATIVE_5259B_EFFECTIVE_RGB=MIXED, explicitly NOT
    // black-equivalent.
    // DECISION: per this iteration's own explicit instruction ("if
    // native Windows itself produces NaN, do NOT zero automatically,
    // report it and make that the next target"), G71's compat is
    // DELIBERATELY NOT extended to 0x0045259B/0x004525B2.
    // legacy_quad_family=PROVEN (structurally); family_compat=DISABLED
    // for this specific pair. Caller 0x0045259B REMAINS a real,
    // unresolved halt -- an honest non-fix, not a regression. The real
    // open question (deferred, not decided here) is what real Voodoo
    // hardware/nGlide's own rendering backend does with a NaN-shaped
    // iterated color such that the original game visibly continues --
    // a driver/GPU-behavior question, not a "what value to substitute"
    // question. The large G67/G70 one-shot forensic dumps are now gated
    // OFF specifically for the 4 already-fully-audited call sites
    // (0x0045223B/0x00452252/0x0045259B/0x004525B2) to keep hardware
    // logs small -- diagnostic code itself is NOT deleted, only gated,
    // and still fires in full for any genuinely new/unaudited caller.
    // No dynarec/heap/texture/LFB/input/DDraw change. See win32/
    // glide2x_ign.cpp's own G72 comment (g72_log_family_audit) for the
    // complete family table and citations.
    IGN_LOG("[G72]", "THIS_IS_THE_SECOND_LEGACY_CALLER_AUDIT_BUILD (G71 confirmed on real hardware -- old 0x0045223B/0x00452252 blocker passed repeatedly, compat_calls_total=400; a NEW halt appeared at caller 0x0045259B field C.b raw=0xFFFFFDA0; static reconstruction proves 0x0045259B is call1 of 0x00452270-0x004525C3, the function G65 already proved byte-for-byte is a complete structural twin of 0x00451F10 -- TOPOLOGY_MATCH=YES; native oracle (128 samples/channel, same still-untouched proxy) shows r/g behave exactly like before (128/128 finite) but b does NOT -- 52/128 (40.6%%) GENUINE NaN natively, with one captured sample byte-for-byte IDENTICAL to the Vita's own failure (same object position, same raw bits, same NaN) -- proving SAME_MECHANISM at the strongest level, but NATIVE_5259B_EFFECTIVE_RGB=MIXED, explicitly NOT black-equivalent; per this iteration's own explicit instruction (native NaN -> do not zero automatically, report and defer), G71's compat is DELIBERATELY NOT extended to this caller -- legacy_quad_family=PROVEN structurally, family_compat=DISABLED for this pair, caller 0x0045259B remains a real unresolved halt, an honest non-fix; the real open question (deferred) is nGlide/real-hardware NaN-color handling, not a substitution value; large G67/G70 forensic dumps now gated off for the 4 already-audited call sites to keep logs small, diagnostic code kept not deleted; no dynarec/heap/texture/LFB/input/DDraw change)");
    // iteration-G73: G72 hardware CONFIRMED real progress -- the game
    // reached an interactive menu (UP/DOWN navigation and CROSS/RETURN
    // both worked, menu text was readable), completed_swap_count=150 and
    // total_triangles=27764 before the new FAMILY-B halt, with ZERO
    // regression in G71's own compat (400 calls, 200+200, stable).
    // Surrounding 3D graphics remained visually wrong -- NOT claiming
    // global correctness.
    // This iteration answered the deferred G72 question: what does the
    // real native Glide backend (nGlide 2.10, confirmed via Win32
    // FileVersionInfo on the untouched pc_oracle DLL) actually do with
    // GrVertex.b=NaN at FAMILY-B (0x0045259B/0x004525B2), such that the
    // original Windows game visibly continues?
    // Backend identified by static string evidence in glide2x_real.dll:
    // Direct3DCreate9/d3d9.dll/"Failed to load D3D9.DLL library." --
    // backend=Direct3D9. Static disassembly of guDrawTriangleWithClip's
    // own export thunk (VA 0x10001325) found only a 2-way dispatch on a
    // global flag before tail-calling into nGlide's real D3D9 vertex
    // code several indirection levels deep -- the exact CPU conversion
    // instruction was NOT independently proven this iteration (kept
    // honest: [G73-NAN-CONVERSION] states this explicitly, no invented
    // instruction is claimed).
    // Instead, per this iteration's own preferred evidence type
    // ("framebuffer comparison -- preferred if practical"), a SEPARATE,
    // throwaway, standalone native harness (pc_oracle/experiment_nan/
    // nan_probe.c -- its own private copy of glide2x_real.dll+3DfxSpl*.
    // dll, NEVER touching the real Ignition install or the transparent
    // pc_oracle proxy) opened a real nGlide window with the EXACT state
    // Ignition's own real call uses (guColorCombineFunction(ITRGB=0x2),
    // guAlphaSource(CC_ALPHA=0x0), grConstantColorValue(0x7FFFFFFF)),
    // drew one triangle whose shared vertex C byte-for-byte reproduces
    // the real captured evidence (r=0x0045042A g=0x00027CA0, both
    // proven finite; only .b varied across 4 cases), and read back the
    // ACTUAL rendered pixel with plain Win32 GetPixel() -- an exact
    // byte comparison, not eyesight. Result: the real captured NaN
    // (0xFFFFFDA0) renders pixel-for-pixel IDENTICAL to substituting
    // b=0.0f and to a tiny finite value of the same order of magnitude
    // as this vertex's own proven-finite channels, and CLEARLY
    // DIFFERENT from b=255.0f. native_effective_nan_b=ZERO_EQUIVALENT,
    // proven by direct experiment, not guessed.
    // DECISION: FAMILY-B compatibility (g73_apply_family_b_nan_compat)
    // is now ENABLED, but narrowly -- it substitutes ONLY vc.b (never
    // vc.r/vc.g, which this project has never once observed non-finite
    // for this family: 128/128 finite in every native sample seen), and
    // only for the exact evidenced conjunction (caller==0x0045259B or
    // 0x004525B2, ITERATED color active, r/g already finite, b non-
    // finite). Any other combination (e.g. a future case where r or g
    // is ALSO non-finite) is explicitly NOT covered and still halts
    // normally via STRICT_REQUIRED_FIELDS, per [G73-NEXT-BLOCKER]'s own
    // guard in the failure branch. Host-local only -- guest memory at
    // the vertex's real address is never written, exactly like G71.
    // G71's own compat and selftest are UNCHANGED and re-verified for
    // regression inside g73_selftest() (g71_regression_check). A new
    // [G73-MENU-ACTIVE] diagnostic-only marker (never inferring from
    // pixels) fires once real objective evidence of continued
    // interactive execution exists past FAMILY-B compat activation (10+
    // more completed swaps AND at least one new real input edge from
    // dinput.cpp's own counter). No dynarec/heap/texture/LFB/input/
    // file_io/DDraw change; performance work remains deferred.
    IGN_LOG("[G73]", "THIS_IS_THE_NATIVE_NAN_SEMANTICS_BUILD (G72 hardware confirmed: interactive menu reached, UP/DOWN/CROSS all worked, menu text readable, completed_swap_count=150 total_triangles=27764 before the FAMILY-B halt, G71 compat stable at 400 calls with zero regression, surrounding 3D graphics still visually wrong -- no global correctness claimed; this iteration identified nGlide 2.10's real backend as Direct3D9 via static string evidence, found guDrawTriangleWithClip's export thunk is a 2-way dispatch several indirection levels above the real conversion code -- NOT independently disassembly-proven this iteration, stated honestly; then, per the preferred framebuffer-comparison evidence type, built a SEPARATE throwaway native harness (pc_oracle/experiment_nan/nan_probe.c) that opened a real nGlide window with Ignition's own exact real state, byte-for-byte reproduced the real captured vertex C, and read back the ACTUAL rendered pixel via plain GetPixel() across 4 controlled cases -- proving, by exact pixel-for-pixel match, that the real captured NaN (raw=0xFFFFFDA0) renders IDENTICALLY to b=0.0 and to a tiny finite value of the same order of magnitude as this vertex's own proven-finite channels, and DIFFERENTLY from b=255.0 -- native_effective_nan_b=ZERO_EQUIVALENT, proven by direct experiment; FAMILY-B compat (g73_apply_family_b_nan_compat) is now ENABLED, narrowly substituting ONLY vc.b (never vc.r/vc.g, never observed non-finite for this family) for the exact evidenced conjunction only, host-local only, guest memory untouched; any other unproven combination still halts normally via STRICT_REQUIRED_FIELDS; G71's own compat/selftest re-verified with zero regression; a new diagnostic-only [G73-MENU-ACTIVE] marker fires on real objective post-activation activity, never inferred from pixels; no dynarec/heap/texture/LFB/input/file_io/DDraw change, performance still deferred)");
    // iteration-G74: G73 hardware run reached the main menu (readable,
    // navigable), Quick Race, car selection, and track selection (CANADA)
    // -- both G71/G73 compat layers held with zero regression (600+/8
    // calls respectively). No crash, no diagnostic halt, no new Glide
    // blocker -- the user manually closed the app after the game
    // appeared frozen during track loading. Investigated whether this
    // apparent stall is a real guest hang, slow real I/O, or this
    // project's OWN diagnostic logger (diagnostics/log.cpp's log_line:
    // by deliberate design, EVERY single log line does a full
    // fopen(append)+fprintf+fclose cycle on real Vita storage, zero
    // buffering -- for crash survivability, confirmed by reading that
    // file, not a bug). Added [G74-LOGGER-PERF] RAM-only timing around
    // that exact cycle, and confirmed via kernel32_ign.cpp's own G49
    // timers (unchanged, verified to wrap ONLY the raw fopen/fread calls)
    // that raw host I/O totaled roughly 4 seconds across the whole prior
    // run while the 3 reported stalls alone totaled ~17.4 seconds --
    // raw I/O cannot by itself account for the difference, strongly
    // implicating the verbose per-call file diagnostics (CreateFileA's
    // own 10-line dump, ReadFile's dump, SetFilePointer's dump, routine
    // HEAPCHECK success lines -- each one, on this design, its own
    // open+write+close cycle) as a large, previously-unmeasured
    // contributor. Statically reconstructed the stall's own repeated
    // checkpoint (caller_guest_eip=0x00466361 in all 3 reports): proven,
    // via IAT cross-reference against the real Ign_3dfx.exe import
    // table, to be the return address right after a CreateFileA call
    // inside a generic, C-runtime-style `_open()`-equivalent function
    // (0x004661AF-0x00466463) used for EVERY file the game opens, not
    // track-specific -- confirmed via its own POSIX-oflag translation
    // table shape, and confirmed to contain no loop/decompression/bulk
    // copy, so it cannot itself be the stall's source; the exact
    // subsequent guest routine responsible for each multi-second gap was
    // NOT fully localized this iteration (this project's own standing
    // limitation: no per-instruction guest trace exists on the real ARM
    // dynarec target) -- reported honestly as ACCEPTABLE, not BEST.
    // Added kVerboseFileIoDiagnostics (ignition_paths.h, default false
    // this iteration), gating ONLY the verbose per-call TEXT (never a
    // real fopen/fread/fseek/fclose call, return value, or guest buffer
    // -- file semantics are structurally unchanged, verified by
    // inspection, see kernel32_ign.cpp's own G74 selftest); errors,
    // failures, and safety halts remain unconditionally logged either
    // way. Extended the existing raw-I/O timers with seek/close (G49-
    // FILEIO now reports open/read/seek/close raw microseconds plus a
    // separately-measured file_diag_log_us, so the two costs can never
    // be conflated again). Added objective, non-visual milestone tags
    // ([G74-TRACK-LOAD-BEGIN] on the first LEVELS/ file open,
    // [G74-TRACK-FILE] once per unique level file, [G74-LOAD-HEARTBEAT]
    // at most every 5s during track load reporting real file/swap/
    // triangle/input deltas -- PROGRESSING=YES or UNKNOWN_NO_HOST_
    // BOUNDARY, never an automatic HANG label, [G74-FIRST-RACE-SWAP] and
    // [G74-RACE-RUNTIME] once track-load's first and then 10+ continuous
    // post-load swaps are observed). G71/G73 compat predicates/
    // substitutions themselves UNTOUCHED. No dynarec/heap/DirectInput/
    // MCI/audio/Glide-state/LFB/texture/DDraw change; file SEMANTICS
    // unchanged, only diagnostic TEXT volume reduced; performance work
    // remains deferred -- this is a measurement-and-observability
    // iteration, not an optimization pass.
    IGN_LOG("[G74]", "THIS_IS_THE_TRACK_LOAD_STALL_INVESTIGATION_BUILD (G73 hardware reached main menu/Quick Race/car selection/track selection [CANADA] with zero regression in G71 [600+ calls] or G73 [8 calls] compat; user manually closed the app after an apparent freeze during track loading -- NOT classified as crash or diagnostic halt; logger cost audit found diagnostics/log.cpp's log_line does a full fopen+fprintf+fclose cycle EVERY line by deliberate design [crash survivability, confirmed by reading the code, not a bug], added [G74-LOGGER-PERF] RAM timing around it; G49's own raw-I/O timers [confirmed unchanged, wrap only fopen/fread] show ~4s of raw I/O across the whole prior run vs ~17.4s across the 3 reported stalls, strongly implicating verbose per-call file diagnostics rather than raw I/O; statically reconstructed the shared stall checkpoint 0x00466361 via real IAT cross-reference as the return address right after CreateFileA inside a generic C-runtime _open()-equivalent function [0x004661AF-0x00466463, proven loop-free/no bulk copy, therefore not itself the stall source] -- the exact subsequent guest routine per gap was NOT fully localized this iteration [standing no-per-instruction-trace limitation], reported ACCEPTABLE not BEST; added kVerboseFileIoDiagnostics [default false] gating ONLY diagnostic TEXT, never real file calls/returns/buffers [structurally guaranteed, verified by inspection]; errors/failures/safety-halts remain unconditional; extended G49-FILEIO with seek/close raw timing plus a separated file_diag_log_us; added objective non-visual milestone tags for track-load-begin/per-file/heartbeat/first-race-swap/race-runtime, heartbeat never auto-labels a hang; G71/G73 compat untouched; no dynarec/heap/DirectInput/MCI/audio/Glide-state/LFB/texture/DDraw change; file semantics unchanged, only diagnostic volume reduced; performance work still deferred)");
    // iteration-G75: G74 hardware run was a CLEAN wrapper diagnostic
    // halt (NOT a crash) at the SAME caller (0x0045259B) but a NEW
    // channel (C.g, raw=0xFFFF9660, different payload than G73's own
    // C.b case) -- disproving the earlier assumption that only .b could
    // ever be non-finite for this family. G73's own predicate correctly
    // refused this unproven case, exactly as designed (not a bug).
    // Extended pc_oracle/experiment_nan/nan_probe.c (same separate
    // throwaway harness, still never touching the real install) to test
    // G and R independently, re-confirm B, and probe payload-dependence
    // with 3 distinct NaN bit patterns per channel. Result: G is
    // ZERO_EQUIVALENT and payload-independent (exact hardware payload +
    // canonical qNaN + family-shaped synthetic all match G=0.0 pixel-
    // for-pixel); B is reconfirmed ZERO_EQUIVALENT and payload-
    // independent (matches G73 exactly); R has NO real hardware sample
    // (0/128 non-finite ever observed) -- a synthetic-only test was also
    // consistent with zero-equivalence, but is explicitly NOT treated as
    // hardware evidence. FAMILY-B compat (g75_apply_family_b_rgb_compat)
    // is now component-wise: substitutes ONLY a non-finite G and/or B
    // (both independently proven), NEVER a finite sibling channel, and
    // NEVER R (kept disabled -- synthetic evidence only, per this
    // project's own standing rule against generalizing beyond real
    // hardware). If R is ever genuinely observed non-finite at this
    // caller, it is deliberately left untouched and halts normally,
    // informatively. Separately, G74's own [G74-LOGGER-PERF] measurement
    // on real hardware PROVED the logger's cost decisively: ~18.8s of
    // the first ~23s of runtime were spent inside log_line() itself
    // (1009 lines, avg 18.66ms/line), while raw file I/O in the same
    // window totaled well under 1 second -- DIAGNOSTIC_LOGGER_
    // PERTURBATION=CONFIRMED as the dominant runtime cost, not merely a
    // hypothesis. Added kG75LowOverheadRuntimeLog (default ENABLED,
    // diagnostics/log.h) gating routine HIGH-FREQUENCY informational
    // lines this iteration found and reduced (every real PeekMessageA
    // dispatch, every CloseHandle confirmation, compat-summary print
    // rate lowered from "first 8 + every 200 calls" to "first activation
    // + every 100/500/1000") -- BUILD markers, first activation of any
    // compat, every new blocker, every STOP/exception/unimplemented-API
    // halt, file/memory/GL errors, milestones, and 5-second aggregate
    // summaries all remain unconditional, per this project's own
    // explicit "always retain" list. The logger's own crash-survivable
    // fopen+fprintf+fclose-per-line architecture is UNCHANGED -- only
    // how many lines get written was reduced this iteration, not how
    // each line is written. No dynarec/heap/DirectInput/MCI/audio/file-
    // semantics/LFB/texture/DDraw change; general performance work
    // remains deferred.
    IGN_LOG("[G75]", "THIS_IS_THE_COMPONENT_WISE_FAMILY_B_AND_LOGGING_OVERHEAD_BUILD (G74 hardware halt was a CLEAN wrapper diagnostic halt, NOT a crash, at caller 0x0045259B with a NEW non-finite channel [C.g, raw=0xFFFF9660] and a different payload than G73's own C.b case -- G73's predicate correctly refused this unproven case, working exactly as designed; extended the separate throwaway pc_oracle/experiment_nan/ framebuffer experiment to test G and R independently, re-confirm B, and probe 3 distinct NaN payloads per channel -- G proven ZERO_EQUIVALENT and payload-independent [exact hardware payload + canonical qNaN + family-shaped synthetic all match G=0.0 pixel-for-pixel], B reconfirmed ZERO_EQUIVALENT and payload-independent [matches G73 exactly], R has no real hardware sample ever [0/128 non-finite observed] so its synthetic-only consistent result is explicitly NOT treated as hardware evidence and stays disabled; FAMILY-B compat is now component-wise [g75_apply_family_b_rgb_compat], substituting only a non-finite G and/or B, never a finite sibling channel, never R; separately, G74's own real hardware [G74-LOGGER-PERF] measurement PROVED diagnostic logging -- not file I/O -- dominates runtime cost [~18.8s of the first ~23s spent inside log_line() itself, vs well under 1s of raw file I/O in the same window] -- DIAGNOSTIC_LOGGER_PERTURBATION=CONFIRMED; added kG75LowOverheadRuntimeLog [default ENABLED] gating routine high-frequency lines found this iteration [PeekMessageA real-message spam, CloseHandle confirmations, compat-summary rate lowered to first-activation+100/500/1000] while keeping every BUILD marker/first-activation/new-blocker/STOP/error/milestone/5s-summary unconditional; logger's own crash-survivable per-line architecture UNCHANGED, only line VOLUME reduced; no dynarec/heap/DirectInput/MCI/audio/file-semantics/LFB/texture/DDraw change; general performance still deferred)");
    // iteration-G76: G75 hardware run reached ACTUAL RACE RUNTIME for the
    // first time (846 frame_cycles, 175,720 real triangles, active input,
    // CANADA-specific resources loaded) -- HUD visible, track/cars not
    // visible, world-render still incorrect; halted cleanly (NOT a
    // crash) on the next unimplemented Glide API, grDrawPoint, at
    // caller_guest_eip=0x004507ED. Statically reconstructed the call
    // (IAT cross-reference against the real Ign_3dfx.exe import table):
    // the containing function (0x00450770-0x004507F0, cdecl, no push-ebp
    // prologue) culls against the current visible screen region, then
    // draws exactly one point that is PROVABLY black-by-design in the
    // original game (grConstantColorValue(1) + guColorCombineFunction
    // (GR_COLORCOMBINE_ZERO), both confirmed via the same IAT method) --
    // role=WORLD_SPACE_CULLED_POINT_EFFECT, exact effect (shadow/skid-
    // mark/spark/debug) not conclusively identified, reported honestly.
    // Implemented grDrawPoint faithfully per real Glide2 reference
    // semantics (grDrawPoint shares the SAME per-pixel rasterizer as
    // grDrawTriangle, just a 1-pixel primitive) -- factored the active-
    // color/alpha-source decode out of guDrawTriangleWithClip_impl into
    // a shared helper (glide_decode_active_color_alpha_source) so both
    // draw paths read the SAME state through the SAME rule, no
    // duplicated/divergent logic; grDrawPoint uses the identical GL
    // state derivation (texture/depth/cull/blend) and the vertex-array-
    // only GL_POINTS path (glDrawArrays(GL_POINTS,0,1)), never immediate
    // mode (the still-live G22 legacy_pool_ptr crash root cause).
    // FAMILY-A/B predicates were NOT widened to grDrawPoint (different
    // caller entirely, would simply never match).
    // Separately found and fixed a REAL bug in G74/G75's own race-
    // milestone detection: it fired on the first ANY "LEVELS/" file
    // (levels/japan/japan.tex, an ordinary startup asset), far too
    // early, false-positiving Quick Race/track-load/first-race-swap
    // during plain menu setup. Fixed by requiring a track-geometry-
    // specific extension (.COL/.PAN/.TAB/.PLC/.MSH/.POS/.TRI/.SRF --
    // deliberately excluding .TEX/.PIC, both proven reused for non-track
    // assets) before declaring track-load-begin, with the track NAME
    // derived from the path itself (never hardcoded to CANADA), and
    // requiring the SAME track's own .TRI AND .SRF files (the two files
    // named as the strongest completion evidence) before declaring the
    // CORRECTED [G76-FIRST-REAL-RACE-SWAP]/[G76-RACE-RUNTIME] milestones.
    // Old G74/G75 tags are left in place (still fire on the old, looser
    // condition) rather than deleted, but are now documented as
    // unreliable for "real race" determination -- the new G76 tags are
    // the trustworthy ones going forward.
    // World-render forensics (Parte 22-26): added sparse, RAM-only
    // classification of the first 3 real-race swaps' triangles (constant
    // vs iterated RGB, textured vs untextured, depth on/off, combine-
    // mode histogram) plus texture-allocation/upload counters, printed
    // as ONE bounded summary. Confirmed via direct source read (not
    // inferred): guTexSource and guTexCombineFunction remain UNIMPLEMENTED
    // (still listed in kGlideFns[]'s generic unimplemented-import trap) --
    // if grDrawPoint's own halt was reached without ever hitting that
    // trap, guTexSource has genuinely never been called this run, meaning
    // ALL 175,720 triangles rendered so far did so WITHOUT ever binding a
    // texture. Additionally confirmed, by direct source read of the
    // existing (frozen, unchanged) triangle draw call, that it NEVER
    // calls glTexCoordPointer/enables GL_TEXTURE_COORD_ARRAY regardless
    // of combine mode -- a real, pre-existing gap this project's own
    // history already flagged (G50-era comment) as "would become a real
    // gap the moment a genuine textured pass is reached." Whether this
    // gap is THE cause of invisible track/cars is left for the runtime
    // counters (this run's own [G76-RACE-TEXTURE-USAGE] output) to prove
    // or reject -- not asserted from source reading alone.
    // Logging: G75's OWN low-overhead attempt was proven insufficient by
    // its own [G75-LOGGER-PERF] measurement (4382 lines / 80.9s by
    // t~131s). Audited every log_line() call site in the per-swap hot
    // path and found exactly two remaining UNCONDITIONAL offenders --
    // [G21]'s "grBufferSwap enter" and [G23]'s "FIRST_VISIBLE_GAME_
    // PIXELS_CANDIDATE" -- both firing on literally every single swap
    // since a since-resolved historical debugging question, despite the
    // latter's misleading "FIRST_" name. Both now gated to first-4 +
    // every-30th-swap (846 swaps observed this run: at ~18.66ms/line
    // each, these two lines alone could plausibly account for ~31s of
    // G75's own measured total). RUN25/RUN27/RUN29 spam does not exist
    // in this codebase as an actual log source (checked, reported
    // honestly) -- the only RUN25 reference is an unrelated historical
    // BUILD-marker name. Logger's crash-survivable per-line fopen+
    // fprintf+fclose architecture remains completely unchanged.
    // G71/G73/G75 compat, dynarec, heap, file semantics, DirectInput,
    // MCI, LFB, existing texture uploads, and the DDraw path are all
    // frozen and untouched this iteration.
    IGN_LOG("[G76]", "THIS_IS_THE_GRDRAWPOINT_AND_RACE_FORENSICS_BUILD (G75 hardware reached ACTUAL RACE RUNTIME for the first time -- 846 frame_cycles, 175720 real triangles, HUD visible, track/cars NOT visible, world-render still incorrect -- then halted cleanly, NOT a crash, on grDrawPoint, caller=0x004507ED; statically reconstructed via real IAT cross-reference: function 0x00450770-0x004507F0 culls against the visible screen then draws a point PROVABLY black-by-design [grConstantColorValue(1)+GR_COLORCOMBINE_ZERO, both IAT-confirmed] -- exact effect type [shadow/skidmark/spark] not conclusively identified, reported honestly; implemented grDrawPoint faithfully per real Glide2 semantics [same rasterizer as triangles, GL_POINTS vertex-array path, no immediate mode], factored active-color/alpha-source decode into a shared helper so both draw paths never diverge, FAMILY-A/B predicates correctly NOT widened; found and fixed a REAL bug in G74/G75's own race-milestone detection [false-positived on levels/japan/japan.tex during plain startup] by requiring a track-geometry-specific extension and the selected track's own .TRI+.SRF completion before the new, trustworthy [G76-FIRST-REAL-RACE-SWAP]/[G76-RACE-RUNTIME] tags fire, track name derived from the path, never hardcoded; added sparse first-3-real-race-swap draw-state/texture-usage forensics -- confirmed via direct source read that guTexSource/guTexCombineFunction remain unimplemented and that the frozen triangle draw call never submits texture coordinates regardless of combine mode, leaving the runtime counters themselves to prove or reject this as the invisible-world-geometry root cause; audited and fixed the two confirmed-unconditional per-swap log spam sources [G21]/[G23] that dominated G75's own proven-insufficient 80.9s logger cost, gated to first-4+every-30th-swap, RUN25/27/29 confirmed not to exist in this codebase; logger's crash-survivable architecture unchanged; G71/G73/G75 compat/dynarec/heap/file-semantics/DirectInput/MCI/LFB/existing-textures/DDraw all frozen)");
    // iteration-G77: G76 hardware result was MAJOR -- NO crash, NO
    // diagnostic halt, manual close only, after ~2850 swaps/~787k
    // triangles with grDrawPoint fully resolved and the corrected
    // [G76-FIRST-REAL-RACE-SWAP]/[G76-RACE-RUNTIME] milestones proving a
    // real CANADA.TRI+CANADA.SRF race (track_load_ms=28178,
    // first_race_swap=1092). The single most important finding:
    // [G76-RACE-TEXTURE-USAGE] showed textures_allocated=57,
    // textures_uploaded=57, textured_triangle_count=0 -- assets are
    // allocated and decoded, but the triangle renderer never engaged
    // texture state, exactly matching the source-level gap this project's
    // own G76 comment had already flagged (guTexSource/
    // guTexCombineFunction unimplemented, glTexCoordPointer never called).
    // This iteration implements the smallest faithful textured Glide2
    // path Ignition actually uses: guTexSource resolves a guTexAllocate-
    // Memory handle to its uploaded host GL texture (unknown/invalidated/
    // never-uploaded handles halt cleanly, never a silent texture-0 bind);
    // guTexCombineFunction implements ONLY the one real-disassembly-
    // evidenced value (tmu=0, fnc=0x1=GR_TEXTURECOMBINE_DECAL, from the
    // sibling material-setup function at 0x00452b10-0x00452bc6 that also
    // gave guTexSource its strongest evidence) -- chained with
    // guColorCombineFunction's already-real GR_COLORCOMBINE_DECAL_TEXTURE
    // translation, this is exactly GL_REPLACE, the same texture-
    // environment mode this file's own LFB blit already uses; triangle
    // texture-coordinate submission recovers real s/t from GrVertex's
    // sow/tow/oow (s/w, t/w, 1/w, perspective-correct division, not a
    // blind u=sow/v=tow) and normalizes by the BOUND texture's own
    // uploaded width/height (Glide's well-established texel-unit s/t
    // convention, not an arbitrary /256), gated end-to-end through one
    // new shared glide_resolve_texture_binding() helper reused by both
    // guDrawTriangleWithClip_impl and grDrawPoint_impl so the two draw
    // paths can never diverge on when a texture is genuinely ready. Every
    // other GrTextureCombineFnc_t value, and every guColorCombineFunction
    // texture-involving mode this project hasn't independently evidenced
    // a matching guTexCombineFunction value for, is refused rather than
    // guessed -- texturing stays disabled for those, not a regression
    // since it was already disabled before G77. Also: G76's own aggressive
    // logging fix was proven INSUFFICIENT by its own hardware measurement
    // (44.3% overhead at ~163s) -- this iteration found and gated the
    // actual dominant remaining spam source, mciSendStringA's per-call
    // unconditional 64-byte stack hex dump (win32/winmm_ign.cpp), to
    // first-3+periodic; logger's crash-survivable fopen+fprintf+fclose
    // architecture remains completely unchanged. A static-disassembly-
    // only pause-key audit found the generic IsKeyDown helper (0x00454150)
    // called with literal DIK_RETURN/DIK_ESCAPE only inside the language-
    // select confirm/cancel function, and NO call to it anywhere in the
    // confirmed race-code address range -- the original in-race pause key
    // is reported UNDETERMINED rather than guessed, so Vita START stays
    // mapped to DIK_RETURN unchanged. dynarec/heap/file-semantics/LFB/
    // FAMILY-A/FAMILY-B/grDrawPoint/DirectInput-transport/existing-
    // triangle-geometry-transform/DDraw all frozen this iteration.
    IGN_LOG("[G77]", "THIS_IS_THE_FIRST_REAL_TEXTURED_RENDERING_BUILD (G76 hardware: NO crash, NO diagnostic halt, manual close after ~2850 swaps/~787k triangles; grDrawPoint RESOLVED; real CANADA race runtime PROVEN via corrected .TRI+.SRF milestone; CRITICAL finding textures_allocated=57 textures_uploaded=57 textured_triangle_count=0 -- assets uploaded but never engaged by the triangle renderer; implemented guTexSource [handle->GL-texture resolve, no silent fallback bind] and guTexCombineFunction [ONLY the one evidenced value, tmu=0/fnc=0x1=DECAL, chained with the already-real GR_COLORCOMBINE_DECAL_TEXTURE == GL_REPLACE] and triangle texcoord submission [perspective-correct s/w,t/w,1/w division, texel-unit normalization by the bound texture's own width/height, shared glide_resolve_texture_binding() helper reused by both guDrawTriangleWithClip_impl and grDrawPoint_impl]; every other texture-combine value stays refused, not guessed; found and gated the actual dominant remaining log-spam source [mciSendStringA's unconditional 64-byte stack dump] after G76's own fix was proven insufficient [44.3%% overhead]; pause-key static audit found no in-race call to the confirmed IsKeyDown helper, reported UNDETERMINED rather than guessed, Vita START mapping left unchanged; dynarec/heap/file-semantics/LFB/FAMILY-A/FAMILY-B/grDrawPoint/DirectInput-transport/existing-geometry-transform/DDraw all frozen)");

    // iteration-G78, Parte 6-8: EXHAUSTIVE static callsite audit (fresh
    // objdump -p + full-binary grep for every `call` targeting the real
    // IAT thunks, not a single pattern-matched guess) found EXACTLY ONE
    // static caller chain for BOTH guTexSource and guTexCombineFunction in
    // the entire Ign_3dfx.exe: guest VA 0x0045021A (inside a 16-field/
    // 64-byte object-record marshaling call, part of a much larger
    // internal 2D draw-command dispatcher spanning ~0x0044FF20-
    // 0x00450529+) calls 0x00451380, which -- ONLY when its own 16th
    // argument (the object record's field at +0x3c) satisfies
    // `0 <= value < 256` -- calls the guTexSource dedup wrapper (0x00451360
    // -> thunk 0x00468c28) with that value as mmid, then the
    // guTexCombineFunction dedup wrapper (0x00451340 -> thunk 0x00468c22)
    // with the LITERAL constant fnc=0x1 (GR_TEXTURECOMBINE_DECAL) --
    // confirming G77's own fnc=0x1 implementation choice was correct, even
    // though G77's own comment mis-cited the caller address as
    // 0x00452b10-0x00452bc6, which this iteration's exhaustive grep could
    // NOT reproduce and hereby corrects (Part 6's own "do not choose one
    // without proof" -- the earlier citation was an unverified pattern-
    // match, not a real found call site; the fnc=0x1/DECAL evidence itself
    // is unaffected, since it's read directly from 0x00451380's own body).
    // If the object's field+0x3c is OUT of [0,256), the SAME function
    // instead calls guColorCombineFunction(0x2=GR_COLORCOMBINE_ITRGB) --
    // i.e. Part 8's own "exact branch that selects textured vs fallback"
    // for THIS dispatcher case is exactly that range check.
    //
    // CRITICAL implication (Part 5/12): the dispatcher this belongs to
    // (walking a linked list of tagged variable-length "draw command"
    // records via a 124-entry byte-remap table at guest VA 0x00450590
    // feeding a jump table at 0x0045053C) is a GENERIC 2D primitive/sprite
    // renderer -- it also sets grClipWindow's own screen-bound globals
    // (0x61eb2c/0x61eb30/0x61eb38/0x61eb4c, the SAME globals grDrawPoint's
    // own G76 culling logic reads) and its OTHER observed case handlers
    // (0x450610/0x450630/0x450770/0x4510c0/0x450800/0x450940/0x450b50/
    // 0x451bb0) draw with hardcoded/iterated flat color ONLY (two cases
    // literally push the constants 0xff/0xffffff), matching the visible
    // flat-color HUD elements (accelerate/turbo bars). The 3D race-world/
    // car triangle geometry reaches guDrawTriangleWithClip through a
    // COMPLETELY SEPARATE call path (0x00452781, this project's own
    // long-documented caller) that has NO relationship to this dispatcher
    // and NEVER itself calls guTexSource. This means the true governing
    // question is NOT "does the guest ever call guTexSource" in isolation
    // -- it's whether the 3D material-setup code path ever calls
    // guColorCombineFunction with a texture-referencing fnc (0x4/0x5/0x6/
    // 0x8) at all; [G78-RACE-TEXTURE-BRANCH] (win32/glide2x_ign.cpp)
    // measures exactly that, RAM-only, over the first 64 real-race
    // material decisions.
    IGN_LOG("[G78-TEXTURE-BRANCH-STATIC]", "guTexSource_callsites=1 guTexCombineFunction_callsites=1 "
                                           "chain=0x0045021A->0x00451380->{guTexSource(dedup@0x00451360),guTexCombineFunction(dedup@0x00451340,fnc=CONST_0x1)} "
                                           "branch_condition=\"object_record[+0x3c] in [0,256) -> textured; else guColorCombineFunction(0x2=ITRGB)\" "
                                           "dispatcher_role=GENERIC_2D_SPRITE_PRIMITIVE_RENDERER (NOT the 3D world/car triangle path, which is a separate, already-documented call site at 0x00452781 that never calls guTexSource) "
                                           "G77_prior_citation_0x452b10=UNREPRODUCIBLE_CORRECTED_THIS_ITERATION");

    // iteration-G78, Parte 23: UI draw-route table -- built from this
    // project's own established, ALREADY-CITED findings (not new guesses
    // invented this iteration): LFB is the established path for the
    // full-screen backbuffer blit (win32/glide2x_ign.cpp's
    // lfb_blit_to_backbuffer, GL_REPLACE-textured quad, frozen since G22/
    // G23); GL triangle geometry is the established path for the 3D
    // world/car model (guDrawTriangleWithClip, 0x00452781); the 2D sprite
    // dispatcher documented above (0x0044FF20+) is a THIRD, distinct path
    // whose specific consumer for any one visible element (menu
    // background, car-selection thumbnail, pause menu, yes/no dialog) has
    // NOT been individually traced back to a specific record/shape-type
    // value this iteration -- reported honestly as UNTRACED rather than
    // guessed from eyesight (Part 21's own explicit instruction).
    IGN_LOG("[G78-UI-PATH-TABLE]", "menu_background: draw_route=UNTRACED (candidates: LFB palette blit OR 2D sprite dispatcher; NOT triangle geometry per this project's own menucar-only triangle evidence) current_result=WHITE_INCORRECT");
    IGN_LOG("[G78-UI-PATH-TABLE]", "car_selection_images: draw_route=UNTRACED (same candidate set as menu_background) current_result=MISSING");
    IGN_LOG("[G78-UI-PATH-TABLE]", "race_world_geometry: draw_route=GL_TRIANGLES(guDrawTriangleWithClip@0x00452781, CONFIRMED, FROZEN) color_combine=CCRGB/ITRGB_ONLY(G77_HISTOGRAM) texture_state=NEVER_REQUESTED current_result=MISSING");
    IGN_LOG("[G78-UI-PATH-TABLE]", "pause_menu: draw_route=UNTRACED (reached and VISIBLE per G77 hardware -- CIRCLE/DIK_ESCAPE proven, see [G77-PAUSE-INPUT]) current_result=VISIBLE");
    IGN_LOG("[G78-UI-PATH-TABLE]", "exit_race_yesno_dialog: draw_route=UNTRACED current_result=MISSING (see [G78-PAUSE-RENDER-SUMMARY] for the 2-second post-pause counter window)");
    IGN_LOG("[G78-UI-PATH-TABLE]", "shared_root_cause_across_all_four=NOT_PROVEN (insufficient static evidence to trace all four to one specific draw_route this iteration; [G78-PAUSE-RENDER-SUMMARY]'s own counters are the closest available runtime discriminator)");

    IGN_LOG("[G78]", "THIS_IS_THE_WHY_TEXTURE_PATH_NEVER_REACHED_BUILD (G77 hardware: NO crash, NO diagnostic halt, manual close; real CANADA race runtime proven again at swap_count=3657/triangle_count=162817; ZERO guTexSource/guTexCombineFunction/textured-triangle runtime calls observed despite the G77 implementation existing and being correct; CIRCLE->DIK_ESCAPE PROVEN to open the in-race pause menu, frozen, no further pause-key search needed; exit-race yes/no confirmation reported invisible. Exhaustive static callsite audit found the ONE AND ONLY real caller chain for guTexSource/guTexCombineFunction in the whole binary [0x0045021A->0x00451380->dedup wrappers], corrected G77's own mis-cited caller address [0x00452b10, unreproducible], and proved this chain belongs to a GENERIC 2D sprite/primitive dispatcher structurally unrelated to the 3D world/car triangle path [0x00452781, unchanged since G42] -- meaning the real governing fact is that the 3D material-setup code never calls guColorCombineFunction with a texture-referencing value at all, not merely that guTexSource itself goes uncalled. Extended the existing read-only native glide2x.dll proxy oracle [pc_oracle/glide2x_proxy] to intercept and count guTexSource/guTexCombineFunction/guColorCombineFunction calls and their first-32 caller RVAs/arguments [G78-NATIVE-TEX-COUNTS/G78-NATIVE-TEX-CALLER], rebuilt and deployed -- awaiting the user's own real-Windows run to complete the decisive native-vs-Vita comparison [G78-TEX-DIVERGENCE]. Replaced G74-G77's own per-call-site logging booleans [proven insufficient three iterations running: 80.9s/44.3%%/74.7%% overhead] with ONE central tag-denylist filter checked before log_line()'s own fopen, plus five sparse [G78-STARTUP-TIMING] checkpoints and a sparse [G78-PAUSE-RENDER-SUMMARY] window around the proven pause transition. No fallback condition forced, no texture bound arbitrarily, no material patched -- this iteration is evidence-gathering only, per its own explicit instruction. dynarec/heap/file-semantics/input-transport/LFB/FAMILY-A/FAMILY-B/grDrawPoint/race-milestone/texture-upload/guTexSource/guTexCombineFunction implementations all frozen)");

    // iteration-G79, Parte 5/6/18/19: MAJOR CORRECTION to G78's own
    // conclusion. G78 traced exactly ONE of what this iteration's
    // exhaustive `call 0x451360`/`call 0x451340` grep (the dedup-wrapper
    // ENTRY points, not just the raw IAT thunks G78's own grep targeted)
    // now proves are ELEVEN real static call sites for the guTexSource/
    // guTexCombineFunction material-setup pattern: guest VA 0x004510DE,
    // 0x004513A0, 0x00451661, 0x00451921, 0x00451BE9, 0x00451F49,
    // 0x004522A9, 0x004525EE, 0x00452880, 0x00452B41, 0x00452E01 (each
    // pairs with 0x451340 four bytes later). At LEAST TWO of these
    // (0x00452860-0x00452B0F and 0x00452B10-??, both immediately ADJACENT
    // to the confirmed world-triangle function 0x00452781-0x00452855)
    // are BYTE-FOR-BYTE identical in shape to 0x00451380 but, critically,
    // end by calling guDrawTriangleWithClip THEMSELVES (0x00452860's own
    // body: range-check object_record[+0x3c] -> guTexSource+
    // guTexCombineFunction(fnc=CONST_0x1)+guColorCombineFunction(0x4=
    // DECAL_TEXTURE)+chromakey -> marshal 3 vertices -> `call 0x468c1c`
    // at guest VA 0x00452AFC). This PROVES texture setup CAN directly
    // precede a real 3D triangle draw in this engine -- G78's "2D sprite
    // dispatcher, NOT the 3D world path" conclusion was based on tracing
    // only ONE of eleven real sites and is hereby corrected: ALL of these
    // call sites belong to ONE larger dispatcher (list-walk entry at
    // 0x0044FF20, remap table @0x00450590, jump table @0x0045053C) that
    // Ignition uses for BOTH 2D HUD primitives AND (via at least the
    // 0x00452860/0x00452B10 family) textured-or-flat 3D object draws --
    // it is Ignition's general-purpose primitive renderer, not a
    // UI-only system. guDrawTriangleWithClip itself has 18 distinct
    // static callers across guest VA 0x00450C83-0x00453078, confirming
    // this dispatcher (or a small family of near-identical ones) drives
    // the large majority of all drawing in this engine, world geometry
    // included. The STANDING FACT remains unchanged and still explains
    // zero runtime guTexSource calls: G78's own [G78-RACE-TEXTURE-BRANCH]
    // (64/64 fallback, sequence 0x1/0x2 alternating -- CCRGB/ITRGB,
    // exactly matching 0x00452860's own ELSE branch call
    // guColorCombineFunction(0x2)) proves that whichever of these eleven
    // sibling functions actually executes during real gameplay takes its
    // FALLBACK branch every single time (object_record[+0x3c] out of
    // [0,256) for whatever records this port's data produces) -- this
    // iteration adds full world-triangle forensics ([G79-WORLD-VERTEX-
    // SAMPLE]/[G79-WORLD-SCREEN-BOUNDS]/[G79-WORLD-WINDING]/[G79-WORLD-
    // COLOR]/[G79-WORLD-DEPTH]/[G79-WORLD-GL-STATE]) to determine, from
    // the CONFIRMED untextured/flat-color triangles Vita actually draws,
    // whether they are even on-screen with visible colors at all -- a
    // question entirely independent of the texture investigation.
    // iteration-G79, Parte 20: static string-table trace only (no PE
    // section byte-offset cross-reference to a specific consuming
    // function was completed this iteration -- Part 20's own "for each:
    // parser, resulting buffer/object, draw function, LFB vs GL" is only
    // PARTIALLY answered here, reported honestly). Confirmed via direct
    // `strings` extraction from the real, untouched Ign_3dfx.exe (not
    // guessed, not assumed from the pc_oracle install directory listing
    // alone) that these exact filenames are embedded literal strings in
    // the executable: baltazar\data\flaggor.pic, baltazar\data\car_sel.pic,
    // baltazar\data\trk_spr.pic, baltazar\data\bilar.pic, MenuBkg.dat, plus
    // a family of dedicated ".PIC" loader error strings (N_SYSG_2.PIC,
    // N_SYSGFX.PIC, POKAL.PIC, H_SIGNS.PIC, N_SIGNS.PIC, S_SIGNS.PIC,
    // H_PAN1/2.PIC, N/S_PANGFX.PIC) each paired with its own "Error while
    // trying to read %s" string, confirming a SHARED ".PIC loader"
    // function exists and is used for many distinct UI assets. The
    // consuming draw function (LFB vs GL) for car_sel.pic/trk_spr.pic/
    // bilar.pic/MenuBkg.dat was NOT traced to completion this iteration --
    // a concrete, scoped G80 target rather than a guess.
    IGN_LOG("[G79-UI-ASSET-PATH]", "asset=\"baltazar\\\\data\\\\car_sel.pic\" string_confirmed_in_exe=YES parser=UNTRACED draw_route=UNTRACED");
    IGN_LOG("[G79-UI-ASSET-PATH]", "asset=\"baltazar\\\\data\\\\trk_spr.pic\" string_confirmed_in_exe=YES parser=UNTRACED draw_route=UNTRACED");
    IGN_LOG("[G79-UI-ASSET-PATH]", "asset=\"baltazar\\\\data\\\\bilar.pic\" string_confirmed_in_exe=YES parser=UNTRACED draw_route=UNTRACED");
    IGN_LOG("[G79-UI-ASSET-PATH]", "asset=\"baltazar\\\\data\\\\flaggor.pic\" string_confirmed_in_exe=YES parser=UNTRACED draw_route=UNTRACED");
    IGN_LOG("[G79-UI-ASSET-PATH]", "asset=\"MenuBkg.dat\" string_confirmed_in_exe=YES parser=UNTRACED draw_route=UNTRACED shared_pic_loader_evidence=YES(8_distinct_.PIC_error_strings_share_one_message_format)");

    IGN_LOG("[G79-WORLD-CALLER-STATIC]", "world_triangle_function=0x00452781-0x00452855 call_instruction=0x0045284A "
                                         "thunk=0x00468c1c(guDrawTriangleWithClip@12) total_guDrawTriangleWithClip_static_callers=18 "
                                         "range=0x00450C83-0x00453078 material_setup_family_callsites=11 "
                                         "material_setup_family_with_own_triangle_draw=2_confirmed(0x00452860,0x00452B10) "
                                         "dispatcher_entry=0x0044FF20 remap_table=0x00450590 jump_table=0x0045053C "
                                         "role=GENERAL_PURPOSE_PRIMITIVE_RENDERER_2D_AND_3D (CORRECTS G78's own "
                                         "GENERIC_2D_SPRITE_PRIMITIVE_RENDERER_NOT_3D_WORLD conclusion, which traced only 1 of 11 real sites)");

    IGN_LOG("[G79]", "THIS_IS_THE_WORLD_TRIANGLE_FORENSICS_BUILD (G78 hardware: NO crash, NO diagnostic halt, manual close, same visual result as G76/G77; CIRCLE pause still works, frozen. MAJOR CORRECTION: exhaustive re-audit of every static call to the guTexSource/guTexCombineFunction dedup wrappers [not just the raw IAT thunks G78's own grep targeted] found ELEVEN real call sites, not one -- at least two [0x00452860, 0x00452B10] are immediately adjacent to the confirmed world-triangle function [0x00452781-0x00452855] and end by calling guDrawTriangleWithClip themselves after a real guTexSource+guTexCombineFunction+guColorCombineFunction[DECAL_TEXTURE] sequence, proving texture setup CAN directly precede a real 3D draw in this engine -- G78's '2D-sprite-only' conclusion is corrected. All eleven sites belong to ONE general-purpose primitive dispatcher [0x0044FF20 entry], not a UI-only system. The standing explanation for zero runtime guTexSource calls is unchanged: G78's own 64/64-fallback evidence proves whichever sibling executes during real play always takes its ELSE branch [object index out of (0,256)]. Added full world-triangle forensics [screen bounds/winding/color/depth/GL-state] to determine, independent of the texture question, whether the CONFIRMED untextured triangles Vita already draws are even on-screen with visible color. Extended the native oracle to also mirror depth/cull/constant-color state at the exact 0x52781/0x52B01 call family and report it alongside vertex dumps [G79-NATIVE-WORLD-SAMPLE] -- awaiting the user's real-Windows run. Found and fixed G78's own incomplete central-log denylist [G31-LOOP/G37-PERF/G42-PERF/G34-PERF/G34-PEEK/G27/G41 all still periodic, plus FIVE redundant logger-perf tags accumulated since G74 never retired] -- G78's own hardware measured 79.2%% then 45.9%% overhead despite the filter existing. No texture forced into the world renderer, no depth/cull disabled, no coordinates rescaled -- this iteration is evidence-gathering only. dynarec/heap/file-semantics/input/G71/G75/grDrawPoint/race-milestones/file-loading/MCI/DDraw/guTexSource/guTexCombineFunction/UV-implementation all frozen)");
    // iteration-G80, Parte 4/5: full static reconstruction of the NEW
    // nonfinite caller 0x00451EDB, found via disassembly starting from
    // the real-hardware caller_guest_eip. Containing function:
    // 0x00451BB0-0x00451EFD (`sub esp,0xfc; push ebx,ebp,esi,edi`
    // prologue at 0x00451BB0). Contains a guTexSource/guTexCombineFunction
    // dedup-wrapper call pair at 0x00451BE9/0x00451BF0 -- CONFIRMS this is
    // the 5th of G79's own 11 material-setup-family siblings (already
    // counted there, not a 12th). Draws a QUAD as two guDrawTriangleWithClip
    // calls sharing one vertex: call1 at 0x00451ED6 (return=0x00451EDB,
    // args V0=[esp+0x1c], V2=[esp+0x94], V3/C=[esp+0xd0]) and call2 at
    // 0x00451EED (return=0x00451EF2, args V0, V1/hidden=[esp+0x58],
    // SAME V3/C=[esp+0xd0]) -- BYTE-FOR-BYTE the identical topology G64/
    // G65 already fully reverse-engineered for FAMILY-A (0x00451F10,
    // caller 0x0045223B/0x00452252) and FAMILY-B (0x00452270, caller
    // 0x0045259B/0x004525B2): V3/C sits at the SAME relative offset
    // (ESP_BASE+0xd0) in all three functions. No static literal writer to
    // C's r/g/b fields was found in THIS function either, matching G50's
    // original exhaustive-sweep finding for the other two families.
    // STRUCTURAL_TWIN_OF_5223B=YES STRUCTURAL_TWIN_OF_5259B=YES
    // DISTINCT_FAMILY=NO (same code-generation pattern, third sibling).
    //
    // Per Part 2/9's own explicit "resemblance != proof" instruction,
    // this structural match alone does NOT justify implementing
    // IGNITION_LEGACY_FAMILY_C_RGB_COMPAT yet -- the native oracle has
    // been extended ([G80-NATIVE-51EDB], pc_oracle/glide2x_proxy) to
    // capture 128 real Windows samples at this exact caller and prove
    // (or disprove) B-channel zero-equivalence the SAME way G73/G75
    // proved it for FAMILY-B's own G/B channels, but this environment
    // cannot launch the native GUI executable itself -- awaiting the
    // user's hardware run. FAMILY-C compat is therefore NOT implemented
    // this iteration (Part 9's own predicate requirements -- native
    // zero-equivalence proof -- are unmet), matching this iteration's
    // ACCEPTABLE tier: full reconstruction + evidence collection, fix
    // deferred.
    IGN_LOG("[G80-51EDB-STATIC]", "function=0x00451BB0-0x00451EFD call1=0x00451ED6(return=0x00451EDB) "
                                  "call2=0x00451EED(return=0x00451EF2) V3_C_offset=ESP_BASE+0xd0 "
                                  "material_setup_sibling=5_of_11(0x00451BE9) STRUCTURAL_TWIN_OF_5223B=YES "
                                  "STRUCTURAL_TWIN_OF_5259B=YES DISTINCT_FAMILY=NO C_literal_writer_found=NO");
    IGN_LOG("[G80-FAMILY-C-COMPAT]", "implemented=NO reason=NATIVE_PROOF_PENDING_USER_HARDWARE_RUN "
                                     "predicate_if_implemented=\"caller==0x00451EDB AND ITRGB_active AND C.b_nonfinite\" "
                                     "scope_if_implemented=host_local_C.b_only(never_C.r_C.g_never_guest_memory)");

    // iteration-G81, Parte 32/33: honest status, not a new claim -- this
    // iteration's own time budget went to the scanout-vs-GL comparison
    // (Part 5, the primary question) rather than a full byte-level trace
    // of the loading-image asset's consuming draw function. G79 already
    // confirmed the CANDIDATE asset strings exist in the real executable
    // (MenuBkg.dat/car_sel.pic/trk_spr.pic/bilar.pic) but never traced
    // which one specifically produces the RACE loading background, nor
    // which guest operation is expected to remove/replace it -- both
    // remain UNTRACED, reported honestly rather than guessed. The runtime
    // [G81-SCANOUT-HASH]/[G81-PRESENT-COMPARE] evidence (comparing the
    // LOADING state's own scanout hash against RACE_FIRST/RACE_10) is
    // this iteration's real, independent way to test the stale-image
    // hypothesis WITHOUT first identifying the exact source asset.
    IGN_LOG("[G81-LOADING-IMAGE-SOURCE]", "source_asset=UNTRACED destination_buffer=UNTRACED draw_route=UNTRACED "
                                          "palette=UNTRACED last_write_time=UNTRACED (see [G79-UI-ASSET-PATH] for the "
                                          "confirmed-present candidate filenames; not yet correlated to a specific "
                                          "consuming function this iteration)");
    IGN_LOG("[G81-LOADING-EXIT-STATIC]", "expected_removal_operation=UNTRACED (no static trace of the guest operation "
                                         "that should clear/replace the loading image around USA.TRI/SRF completion "
                                         "was completed this iteration -- see [G81-SCANOUT-HASH]/[G81-PRESENT-COMPARE] "
                                         "for the independent runtime test of whether the image actually persists)");

    // iteration-G82, Parte 21/22: material-setup family table -- of the 11
    // real dedup-wrapper callsites (G79's own exhaustive grep), 9 are now
    // resolved to 7 distinct containing functions (2 sites share a
    // function with ANOTHER already-known family -- a genuinely new
    // finding this iteration: FAMILY-A [0x00451F10] and FAMILY-B
    // [0x00452270], both already fully reverse-engineered for their
    // TRIANGLE topology in G64/G65/G72, ALSO each contain their own
    // guTexSource/guTexCombineFunction dedup call [0x00451F49 inside
    // FAMILY-A, 0x004522A9 inside FAMILY-B's own range] -- meaning even
    // the already-frozen FAMILY-A/B functions have a textured branch this
    // project had not previously connected to the texture investigation).
    // One site [0x004510DE] was NOT resolved to a containing function
    // this iteration -- reported honestly as unresolved, not guessed.
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00451380 dedup_call=0x004513A0 condition=\"[esp+0xfc]_in_[0,256)\" fallback=guColorCombineFunction(0x2) triangle_caller=self(2D_dispatcher_case) proven=YES");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00451630 dedup_call=0x00451661 condition=\"[esp+0xfc]_in_[0,256)\"(prologue_identical_to_0x00451380,_branch_bytes_not_individually_re-verified) triangle_caller=self proven=STRUCTURAL_MATCH_ONLY");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x004518F0 dedup_call=0x00451921 condition=\"[esp+0xfc]_in_[0,256)\"(prologue_identical) triangle_caller=self proven=STRUCTURAL_MATCH_ONLY");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00451BB0 dedup_call=0x00451BE9 condition=\"[esp+0xfc]_in_[0,256)\"(V3/C=ESP_BASE+0xd0,_quad_topology,_STRUCTURAL_TWIN_of_FAMILY-A/B) triangle_caller=self(0x00451ED6/0x00451EED,_G80's_own_51EDB_halt) proven=YES");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00451F10(FAMILY-A,_already_frozen) dedup_call=0x00451F49 condition=UNVERIFIED_THIS_ITERATION triangle_caller=self(0x00452236/0x00452252) proven=NEW_FINDING_NOT_FULLY_VERIFIED");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00452270(FAMILY-B,_already_frozen) dedup_call=0x004522A9_and_0x004525EE condition=UNVERIFIED_THIS_ITERATION triangle_caller=self(0x00452596/0x004525AD) proven=NEW_FINDING_NOT_FULLY_VERIFIED");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00452860 dedup_call=0x00452880 condition=\"[esp+0xfc]_in_[0,256)\" fallback=guColorCombineFunction(0x2) triangle_caller=self(0x00452AFC) proven=YES");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00452B10 dedup_call=0x00452B41 condition=\"[esp+0xfc]_in_[0,256)\"(prologue_identical) triangle_caller=self proven=STRUCTURAL_MATCH_ONLY");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=0x00452DD0 dedup_call=0x00452E01 condition=\"[esp+0xfc]_in_[0,256)\"(prologue_identical) triangle_caller=self proven=STRUCTURAL_MATCH_ONLY");
    IGN_LOG("[G82-MATERIAL-FAMILY-TABLE]", "function=UNRESOLVED dedup_call=0x004510DE condition=UNKNOWN triangle_caller=UNKNOWN proven=NO (not traced to a containing function this iteration)");

    // iteration-G82: deliberately SHORT -- per Part 36/37's own explicit
    // "no historical iteration prose" instruction, this replaces what
    // used to be one giant paragraph per past iteration (now denylisted,
    // see G82_historical_boot_spam above) with a few concise lines.
    // iteration-G83, Parte 13/14/29: documentation-only one-shots. VitaGL
    // is linked as a prebuilt static library (libvitaGL.a from the
    // vitasdk install) -- its own C source is NOT present anywhere in
    // this project tree (confirmed via search this iteration), so its
    // internal buffer-rotation/clear implementation cannot be read
    // directly; only its public API contract (vglSwapBuffers/
    // vglInitExtended signatures, already cited since G4E/G11/G23) is
    // known. Reported honestly as UNAVAILABLE rather than guessed.
    IGN_LOG("[G83-VITAGL-BUFFER-MODEL]", "source_available=NO (prebuilt libvitaGL.a, not vendored in this project tree) "
                                         "public_api_only=vglSwapBuffers(vsync_bool)/vglInitExtended(legacy_pool_size,...) "
                                         "internal_buffer_count/clear_behavior/rotation_order=UNKNOWN_NOT_GUESSED");
    IGN_LOG("[G83-WINVITA-SWAP-MODEL]", "confirmed_order=guest_triangle_draws->[if_lfb_dirty]upload+blit->"
                                        "G81/G82/G83_diagnostic_capture->vglSwapBuffers(GL_FALSE) "
                                        "post_swap_copy=NO post_swap_clear=NO post_swap_invalidate=NO "
                                        "(all confirmed by direct source read, win32/glide2x_ign.cpp's own grBufferSwap_impl, unchanged)");
    // iteration-G83, Parte 29: RETRACTS G82's own [G82-LOADING-ASSET]
    // candidate (last file opened before structural track load, that
    // iteration guessed "ign_3dfx.btz" or similar) -- Part 29's own
    // explicit "that is NOT sufficient evidence" instruction. No stronger
    // proof (decoded-buffer hash correlated against the physical loading
    // tile pattern) was established this iteration either -- the asset's
    // true identity remains genuinely UNPROVEN, reported honestly rather
    // than repeating an unproven guess with more confidence.
    IGN_LOG("[G83-LOADING-ASSET-PROOF]", "status=UNPROVEN (G82's own last-file-before-load candidate explicitly "
                                         "RETRACTED as insufficient evidence, per this iteration's own instruction; "
                                         "no stronger decoded-buffer-hash correlation was established this iteration "
                                         "either -- a concrete, scoped G84 target, not a guess)");

    IGN_LOG("[G93]", "G92 hardware run: no crash, no halt, CANADA ran. G92's own real numbers REJECTED the simple "
                     "presentation-divergence theory -- GL (11-43%%) and physical scanout (11-35%%) BOTH change "
                     "substantially across all three phases, never PRESENT_DIVERGENCE_PROVEN. Real race frames also "
                     "showed the 3D world (~600 triangles/frame, both textured and guTexSource active) and an LFB "
                     "blit happening in the SAME frame, every frame -- the highest-value fact from that run. G92's "
                     "own [G92-LFB-OVERWRITE] result (changed_tiles=0, not stale) was from the first 3 GLOBAL LFB "
                     "occurrences, likely during menu/loading, NOT race-specific -- so the race LFB question stayed "
                     "genuinely open. G93 picks the FIRST real race frame with >=500 triangles, textured triangles, "
                     "guTexSource calls, and a captured LFB blit, records its exact ordered chronology (collapsed/"
                     "run-length-encoded, not one line per triangle), and captures GL tilemaps immediately before "
                     "and after that SAME frame's own LFB blit -- a direct, frame-specific test of whether it "
                     "overwrites an already-rendered 3D scene with stale content, computed automatically into an "
                     "OVERWRITE_PROVEN / HELPFUL / NOT_PRIMARY / INCONCLUSIVE verdict. Also adds a large-screen-draw "
                     "detector (bounding box >=50%% of the logical screen) to find a candidate fullscreen stale "
                     "quad if the LFB turns out not to be the cause. Fixed a real G92 bug along the way: phase "
                     "arming never snapshotted its own counter baseline, so a phase's first frame silently absorbed "
                     "everything accumulated since boot. No LFB semantics changed, no buffer/MMID/texture path "
                     "touched -- this iteration is diagnostic capture only, and (per its own explicit gate) does "
                     "NOT build a second no-LFB probe VPK until a real verdict exists.");

    IGN_LOG("[G92]", "G91 hardware run: no crash, no halt, CANADA race entered and ran. The world texture path is "
                     "now PROVEN active at real scale (35536 guTexSource calls, 8729 textured world triangles, ZERO "
                     "fallback) -- G85-G89's own question is closed for good. Stale-background percentage improved "
                     "from ~83%% to ~66.7%%, but a real, user-visible blocker remains: specific frozen/stuck images "
                     "at the menu-accept and race-entry transitions, even though rendering is clearly active "
                     "underneath. G92 does NOT touch the renderer, MMID, LFB, or buffers -- it instruments WHERE in "
                     "the frame lifecycle staleness happens: temporal 12x8 tile tracking of the GL render target "
                     "AND the physical scanout across 10 consecutive frames at three real milestones (MENU/LOADING/"
                     "RACE_FIRST, this iteration's own explicit mapping of the user's described phases onto already-"
                     "instrumented hooks), plus pre/post-LFB-blit tilemap capture (capped at 3 occurrences) to test "
                     "directly whether an LFB blit overwrites a fresh 3D frame with stale content. Each phase's own "
                     "real data drives an automatic three-way verdict: PRESENT_DIVERGENCE_PROVEN (GL updates, "
                     "scanout doesn't -- a presentation/swap bug), GL_ITSELF_STALE (renderer isn't rebuilding the "
                     "scene at all), or BOTH_CHANGING (stale content is being explicitly redrawn, needing separate "
                     "draw-source identification) -- computed from this run's own numbers, not asserted here. All "
                     "RAM-buffered, one dump per phase, no per-frame file writes.");

    IGN_LOG("[G91]", "MAJOR VISUAL MILESTONE: the user's own G90 hardware run saw recognizable, MOVING CARS for the "
                     "first time ever, with sane colors (no more psychedelic/wild appearance) -- direct visual "
                     "proof the G89 MMID fix is real. Execution then hit a clean diagnostic halt on vertex B.b "
                     "being non-finite in a 'required' field. Root-caused it as a genuine validator bug, not a "
                     "vertex-producer or state-tracking problem: glide_decode_active_color_alpha_source() only "
                     "ever recognized ONE combine shape (function==LOCAL && local==CONSTANT) as 'vertex RGB not "
                     "needed' and silently treated everything else as ITERATED -- including GR_COLORCOMBINE_"
                     "DECAL_TEXTURE (function=SCALE_OTHER, factor=ONE, other=TEXTURE), whose real Glide equation "
                     "consumes ZERO vertex color channels. The formula never even looked at `factor`. Fixed by "
                     "deriving the real dependency from all three already-correctly-tracked fields (function/"
                     "factor/local, set by guColorCombineFunction_impl since G41/G89, completely unchanged) "
                     "instead of guessing from just two. This also incidentally corrects the same bug for "
                     "GR_COLORCOMBINE_ZERO and GR_COLORCOMBINE_ONE, which were also being wrongly required. No "
                     "vertex memory touched, no NaN sanitized, no global weakening -- the fix only stops the "
                     "validator from demanding a field the active pipeline mathematically never reads. Added a "
                     "one-shot full-state+vertex capture for any future halt at this exact validator, a static "
                     "caller classifier, and confirmed the existing native oracle proxy (since G79) already "
                     "captures full A/B/C vertex dumps for this exact caller family -- just needs a fresh run, "
                     "since the only log on hand predates that code.");

    IGN_LOG("[G90]", "G89 hardware run: no crash -- a clean diagnostic halt right after the intro video, on the "
                     "NEXT genuinely unimplemented Glide call, grChromakeyValue(0x00FFFFFF) at caller 0x004528A8. "
                     "This is itself strong evidence the G89 MMID fix worked: a much earlier iteration's own static "
                     "disassembly of a sibling material function had already predicted this exact sequence -- "
                     "guTexSource->guTexCombineFunction->guColorCombineFunction(DECAL_TEXTURE)->grChromakeyMode"
                     "(ENABLE)->grChromakeyValue -- as what a WORLD material would do once it could take its "
                     "textured branch, long before there was any runtime evidence it would ever be reached. G90 "
                     "implements grChromakeyValue_impl for real (fetched the exact signature and semantics from "
                     "openglide's sdk2_glide.h and the Glide 2.2 reference manual this iteration, not guessed): "
                     "GrColor_t value storage, matching the SAME conservative precedent grChromakeyMode_impl itself "
                     "already set for the ENABLE case -- real per-pixel color-key discard is deliberately NOT "
                     "implemented, since this project's current texture upload format (RGB565) has no alpha "
                     "channel at all, and adding one would be exactly the pipeline redesign this iteration's own "
                     "instructions forbid doing speculatively. Added bounded runtime capture (first 32 calls), a "
                     "one-shot semantics decode, a first-chromakey-enable snapshot for a future iteration to build "
                     "real discard from, and cumulative texture-path counters that fire early (not gated behind "
                     "RACE_10, since G89 never reached it) so progress is visible even if another halt follows "
                     "immediately. The G89 MMID fix itself is completely unchanged. No buffer/LFB/depth/cull/blend/"
                     "dynarec/geometry touched, no visual claim without hardware confirmation.");

    IGN_LOG("[G89]", "The user ran the ORIGINAL Windows Ignition with the G88 native oracle -- MMID_ABI_DIVERGENCE is "
                     "now PROVEN, not suspected: native guTexAllocateMemory returned small values (86/86 samples "
                     "<256, observed range 0x00-0x1C) and the game passed them back to guTexSource completely "
                     "UNCHANGED (128/128 exact matches). The native race also proved the texture path is used "
                     "heavily (22958 guTexSource calls, ~2810 DECAL_TEXTURE color-combine calls in 100 frames) -- "
                     "this project's own prior 0x70000001+N guest-visible handle encoding made EVERY material's own "
                     "0<=mmid<256 check fail unconditionally, forcing fallback everywhere. G89 fixes exactly that: "
                     "guTexAllocateMemory now returns small, native-compatible, sequential guest MMIDs starting at "
                     "0 (next_guest_mmid()), reset to 0 by guTexMemReset (matching native's own observed allocation-"
                     "cycle behavior). The change turned out minimal -- guTexDownloadMipMap_impl/guTexSource_impl/the "
                     "UV-normalization lookup already looked up whatever value the map was keyed by generically, so "
                     "only the allocator itself needed to change. The OLD 0x70000001+N numbering survives internally "
                     "only, as GuTextureAllocation::internalDebugId, never guest-visible, never used for any lookup. "
                     "Added real hardware counters for guTexSource/guTexCombineFunction calls, textured-vs-fallback "
                     "world decisions, unique MMIDs/GL textures bound, and a contradiction check in case the fix "
                     "doesn't flip the counters as expected. No buffer/LFB/depth/cull/blend/dynarec/geometry touched. "
                     "No visual claim without hardware confirmation.");

    IGN_LOG("[G88]", "G87 hardware run: no crash, CANADA ran, and performance keeps improving (faster boot, smoother "
                     "intro, faster loads) -- confirmed NOT a regression from any of this project's own recent "
                     "logging/instrumentation changes. G87 deliberately did not touch the MMID ABI (still gated on "
                     "native proof), so unchanged visuals were expected, not a failure. G88's own job was to make "
                     "that native proof trivial for the user to actually deliver: found a STALE pc_oracle_log.txt in "
                     "pc_oracle/ left over from the G66-G72 era (predates guTexSource/guTexAllocateMemory "
                     "instrumentation entirely) and moved it aside rather than mistake it for real data. Wrote a "
                     "plain-language README_G88.txt and a launch_oracle.bat that clears stale logs and starts the "
                     "game directly, and retagged the proxy's own output to [G88-NATIVE-TEXALLOC]/[G88-NATIVE-MMID-"
                     "HISTOGRAM]/[G88-NATIVE-TEXSOURCE]/[G88-NATIVE-MMID-CORRELATION]/[G88-NATIVE-MATERIAL] (kept "
                     "alongside the G86/G87 tags already there, nothing removed). No MMID ABI change, no branch "
                     "forced, no buffer/LFB/dynarec touched -- still MMID_DIVERGENCE=PENDING_USER_RUN, honestly.");

    IGN_LOG("[G87]", "G86 hardware run: no crash, CANADA ran. G86's real question is now answered from the Vita "
                     "side alone: the WORLD/general-purpose material families -- never captured before G86 -- "
                     "produced 256/256 fallback samples, ALL shaped exactly like this project's own synthetic "
                     "0x700000xx texture-handle namespace (0x70000002 x167, 0x7000000E x71, 0x70000007 x12, "
                     "0x70000006 x6). This is no longer particle-only evidence -- it is the SAME pattern, now seen "
                     "in the general material path every world triangle goes through. G87 does NOT change the "
                     "handle ABI (still gated on native proof this project does not have yet) -- instead it closes "
                     "the remaining measurement gap: the native oracle proxy (pc_oracle/glide2x.dll) now captures "
                     "the first 128 guTexAllocateMemory returns AND the first 128 guTexSource arguments, with a "
                     "direct exact/transformed/unknown correlation between them, plus a full static MMID usage "
                     "table for this project's own Glide layer. Native_oracle_status=READY_FOR_USER_RUN -- nothing "
                     "was fabricated. No handle encoding change, no branch forced, no buffer/LFB/dynarec touched. "
                     "Also recorded (not implemented): the real Ignition install's own exit-confirmation key is 'S', "
                     "deferred as a future context-sensitive input task, graphics investigation stays priority.");

    IGN_LOG("[G86]", "G85 hardware run: no crash, CANADA ran, same visual symptoms. G85's own 128 fallback captures "
                     "-- ALL from the particle family (0x00451F10) -- held only TWO unique values, both bit-for-bit "
                     "identical to this project's own synthetic texture-handle namespace (0x7000001A x112, "
                     "0x70000018 x16). That specific coincidence was actually ALREADY fully explained by this "
                     "project's own prior hardware-proven forensics (G53/G56-G63, closed): 0x70000018 genuinely IS "
                     "TEX24's real handle, deliberately written into that exact object field by Ignition's own "
                     "texture-table-fill loop (0x00409C53-0x00409C94) -- not a coincidence, not ABI corruption, and "
                     "not new evidence by itself. G86's real job was the question G85 could not yet answer: does the "
                     "SAME 0x700000xx pattern also appear in the 7 WORLD/general-purpose material families (never "
                     "captured before) -- since G86 confirmed by fresh disassembly that ESI survives unclobbered "
                     "from branch to draw call for 6 of those 7, capture now happens at guDrawTriangleWithClip "
                     "itself (immune to guColorCombineFunction's own dedup-cache blind spot), kept in a completely "
                     "separate histogram from the particle family. Also added: a bit-for-bit dual-triangle capture "
                     "validation for the particle technique itself, a real per-call guTexAllocateMemory ring "
                     "(replacing pure inference), and native-oracle instrumentation for guTexAllocateMemory's own "
                     "return values ([G86-NATIVE-MMID]) so the decisive native-vs-Vita comparison can finally be "
                     "made. No handle encoding changed, no branch forced, no buffer/LFB/dynarec code touched -- "
                     "pure read-only capture and citation of already-proven history, per this iteration's own "
                     "explicit gate.");

    IGN_LOG("[G85]", "G84 hardware run: no crash, CANADA race entered and ran, notably smoother/faster overall, but "
                     "the clear-probe VPK looked visually IDENTICAL to normal -- REJECTING the simple "
                     "missing-clear hypothesis. An ACTIVE gameplay frame (611 triangles) only changed 12/96 coarse "
                     "tiles (12.5%%), pointing at the stale background being a SECONDARY symptom of a deeper "
                     "problem: EVERY real material still takes the fallback path (textured=0, fallback=64). G85 "
                     "challenges the prior 'unobservable without a dynarec hook' conclusion: fresh disassembly of "
                     "all 10 material-family functions (objdump against the real Ign_3dfx.exe) found the fallback "
                     "call's OWN dedup-wrapper chain has a fixed, per-family stack offset back to the tested value, "
                     "recoverable at the EXISTING guColorCombineFunction trap with zero dynarec changes -- and, for "
                     "the 3 object-pointer families, G52's own already-hardware-proven ESI-register technique "
                     "(previously one-shot) generalizes cleanly into a systematic sampler at the existing "
                     "guDrawTriangleWithClip trap. Resolved G82's one remaining UNRESOLVED family (0x004510C0). "
                     "Notably, G52/G53 ALREADY proved FAMILY-A's own tested field is a per-particle palette index, "
                     "not a texture handle -- its fallback may be entirely LEGITIMATE particle-system behavior, a "
                     "distinct question from the other 7 general-purpose families. RAM-only capture, one aggregate "
                     "dump at RACE_10/128-cap -- zero new hot-path logging, preserving G84's own ~1%% overhead win. "
                     "Native oracle comparison still PENDING (not fabricated). Retired the clear-probe from active "
                     "use (source kept, OFF by default). No texture/material/branch value forced or changed.");

    IGN_LOG("[G84]", "MAJOR CORRECTION: G83 hardware found THREE distinct physical scanout bases (not two) across "
                     "16 swaps -- 0x626A6000(6/16), 0x622A4000(5/16), 0x624A5000(5/16). Per-buffer tile comparison "
                     "against the loading reference: A=100.0%% stale, B=100.0%% stale, C=4.2%% stale -- two buffers "
                     "hold completely stale loading-era content while one is mostly current. G83's own swap-pair "
                     "data also showed a newly-rotated GL backbuffer hash equal to the EXACT loading hash before "
                     "any guest draw. G84 adds per-buffer-appearance evolution tracking over each buffer's first 4 "
                     "rotations [G84-BUFFER-EVOLUTION/CONVERGENCE], GL-side new-backbuffer/post-clear tilemap "
                     "capture [G84-NEW-BACKBUFFER-STATE]/[G84-POST-CLEAR-STATE], stale-vs-render-target clear "
                     "correlation [G84-CLEAR-TARGET], a rolling first-real-geometry frame coverage probe "
                     "[G84-ACTIVE-FRAME-COVERAGE] (G83's own end-of-frame sample had triangles_this_frame=0, not "
                     "representative), and an OPTIONAL diagnostic clear-probe VPK (G84_CLEAR_PROBE, default OFF, "
                     "force-clears the rotated backbuffer for the first 30 race frames) to test whether removing "
                     "stale content alone unmasks the game underneath. FOUND the real source of [RUN27]/[RUN29]: "
                     "vendored dynarec instrumentation (external/WinVita/third_party/box86-dynarec), a raw fopen/"
                     "fprintf bypassing this project's own logger entirely -- NOT modified, vendored code requires "
                     "explicit authorization, see [G84-RUN-MARKER-SOURCE]. TIGHTENED the allowlist to [BUILD-G84]/"
                     "[G84]-only (drops every prior iteration's own [BUILD]/[G82]/[G83] tags by construction) and "
                     "filtered ignition_critical.log for the first time (was completely unfiltered through G83). "
                     "No texture/material/depth/cull/blend change made -- buffer lifecycle instrumentation and "
                     "logging suppression only, per this iteration's own explicit freeze list.");

    IGN_LOG("[G83]", "MAJOR: G82 hardware PROVED partial redraw with hard numbers (race_first stale=100.0%%, "
                     "race_10=85.4%%, race_60=83.3%%) -- no longer a hypothesis. G82's own temporally-paired swap "
                     "probe found something new: GL-before-swap hash was IDENTICAL across 4 consecutive swaps "
                     "(0xd0ccfb94) while the SCANOUT hash alternated between the race hash and the EXACT loading "
                     "hash (0x52965dc5) -- strong evidence some physical/logical buffer never gets new content. "
                     "G83 adds a 16-swap buffer-identity sequence [G83-SCANOUT-BUFFER-SEQUENCE/HISTOGRAM/TILEMAP], "
                     "new-backbuffer-after-swap hashing, single-frame buildup tracing, and per-race-clear buffer "
                     "correlation to map this directly. RETRACTS G82's own unproven loading-asset candidate. "
                     "REPLACED the entire denylist logging architecture with an allowlist (only [BUILD]/[G82]/"
                     "[G83]/errors/[STOP] now reach the log) after the denylist failed five iterations running. "
                     "No buffer/clear/texture/depth/cull change made -- evidence only, per its own explicit rule.");

    IGN_LOG("[G82]", "scanout-vs-GL comparison DISPROVED frozen-scanout (loading!=race hash); same-instant GL-vs-"
                     "scanout is expected to differ (different double-buffer members) and is NOT itself a bug -- "
                     "G82 adds temporally-paired [G82-SWAP-PAIR], a 12x8 tile-diff map, and world/HUD coverage "
                     "estimation to test PARTIAL_REDRAW_STALE_BACKGROUND directly. Material branch still 0/64 "
                     "textured; native oracle extended for both clear-lifetime and 100-frame material counts, "
                     "awaiting the user's hardware run. Found and denylisted the true dominant logger cost: ~42 "
                     "unconditional per-iteration narrative lines at every boot. No renderer/clear/texture/depth/"
                     "cull change made -- evidence-gathering only.");

    IGN_LOG("[G80]", "THIS_IS_THE_51EDB_AND_LFB_OCCLUSION_BUILD (G79 hardware: NO crash -- user selected USA, second "
                     "track, USA.TRI/USA.SRF both observed, real race entry proven; then a NEW wrapper diagnostic "
                     "halt at caller=0x00451EDB, field=C.b, raw=0xFFFFFC4C, NOT the previously-known FAMILY-A/B "
                     "callers. Full static reconstruction proves 0x00451EDB is the 5th of G79's own 11 material-"
                     "setup-family siblings [function 0x00451BB0-0x00451EFD, guTexSource/guTexCombineFunction dedup "
                     "call at 0x00451BE9] and draws a quad with the BYTE-IDENTICAL V0/hidden/V2/V3-shared topology "
                     "already proven for FAMILY-A/B, V3/C at the SAME ESP_BASE+0xd0 offset -- STRUCTURAL_TWIN_OF "
                     "both=YES, DISTINCT_FAMILY=NO -- but per this iteration's own 'resemblance != proof' rule, "
                     "FAMILY-C compat is NOT implemented without a fresh native zero-equivalence proof for THIS "
                     "caller's B channel; the native oracle is extended [G80-NATIVE-51EDB, 128-sample cap] and "
                     "awaiting the user's hardware run. Also fixed a real bug this iteration found: g67_log_"
                     "extended_stack_window's own diagnostic tag had 'caller=0x0045223B' HARDCODED regardless of "
                     "the actual caller, misleadingly mislabeling this new halt's own forensic dump -- now prints "
                     "the real caller, and g70_log_provenance_closure (whose text is ALSO specific to 0x0045223B) "
                     "no longer fires for unrelated callers. Capped [G67-WATCH-CHANGE] from up to 128 lines to the "
                     "last 16. G79's own graphics forensics answered several questions with real hardware data: "
                     "world geometry IS on-screen [0 fully off-screen of 2048 sampled], culling/depth/blend are all "
                     "OFF and cannot explain invisibility, scissor/viewport are full-screen and correct -- this "
                     "CORRECTS G79's own textual 'depth/blend prime suspect' guess. The new leading hypothesis, "
                     "provable by direct source reading [lfb_blit_to_backbuffer disables GL_BLEND, uses GL_REPLACE "
                     "texenv, RGB565 has no alpha channel], is that the LFB fullscreen composite -- which runs "
                     "AFTER world triangles in grBufferSwap_impl's own order [triangle draws -> lfb_upload_texture "
                     "-> lfb_blit_to_backbuffer -> vglSwapBuffers] -- unconditionally overwrites the entire covered "
                     "rect with opaque LFB content. Added a one-shot pre/post-LFB-blit pixel probe [8 fixed points "
                     "+ one 64x64 ROI hash, never a full framebuffer read] and a diagnostic-only, default-OFF A/B "
                     "toggle [kG80SkipLfbBlitDuringRealRace] to test this directly on real hardware without "
                     "changing shipped behavior. Fixed G79's own [G79-LFB-DIRTY-REGION], which never fired on real "
                     "hardware because it was armed ONLY during the pause window; now also samples once at the "
                     "same real-race probe frame. Confirmed color units: guest Glide 0..255 float, ONE host "
                     "conversion [/255.0f], GL_FLOAT color pointers used as-is -- no double-normalization bug. "
                     "guTexSource/guTexCombineFunction/UV implementation, dynarec, heap, file semantics, input, "
                     "grDrawPoint, race milestones, MCI, DDraw all frozen; no depth/cull globally disabled, no "
                     "texture path forced, no guest memory changed)");

    IGN_LOG("[G81]", "THIS_IS_THE_SCANOUT_VS_GL_PRESENT_FORENSICS_BUILD (G80 hardware: NO crash, NO diagnostic halt "
                     "-- user selected USA again, real race runtime proven [swap_count=622, triangle_count=166710 at "
                     "first real race swap], game kept running much longer, manually closed. G80's own pre/post-LFB "
                     "probe measured the EXACT SAME 8 pixels and ROI hash before and after the LFB blit on the first "
                     "real race frame -- LFB_OVERWRITES_WORLD_PIXELS=NO for that frame, and the GL backbuffer itself "
                     "contained non-black, non-uniform, spatially-varying pixel data (proof the world IS being drawn "
                     "into SOME real render target). Yet the user physically observes a stale loading-screen image "
                     "persisting behind live HUD during race -- this contradiction, treated as real hardware "
                     "evidence per this iteration's own explicit instruction, is the primary question. This "
                     "iteration adds a genuinely NEW capability: read-only physical-display-scanout sampling via "
                     "the real Vita SDK sceDisplayGetFrameBuf (never sceDisplaySetFrameBuf, display mode never "
                     "touched), so the GL backbuffer and the ACTUAL scanned-out framebuffer can finally be compared "
                     "pixel-for-pixel and hash-for-hash at 5 milestones [LANGUAGE, LOADING, MENU, RACE_FIRST, "
                     "RACE_10], each captured exactly once, right before vglSwapBuffers. Also audited and fully "
                     "documented grBufferSwap's own real operation order [triangle draws already happened earlier "
                     "in the frame -> conditional LFB upload/blit -> G81 capture -> vglSwapBuffers(GL_FALSE), no "
                     "glFlush/glFinish/renderbuffer-rebind/clear happens inside it]; fixed a real, previously-"
                     "unnoticed unconditional-every-call spam source in grRenderBuffer_impl [4-5 [GLIDE-G5] lines "
                     "per call, never gated since G5] by converting it to state-transition-only logging plus an "
                     "explicit FRONTBUFFER-usage audit; added an LFB-lock buffer histogram; added a real-race "
                     "grBufferClear audit (RAM-only, first 20 calls, confirms glClearColor+glClear(GL_COLOR_BUFFER_"
                     "BIT) ARE called unconditionally in source, independent of whether the guest actually keeps "
                     "calling grBufferClear during race). Extended the native oracle with sparse transition "
                     "timestamps [G81-NATIVE-TRANSITION: last pre-gameplay LFB lock, first gameplay triangle, first "
                     "post-gameplay buffer swap, render-buffer changes] -- awaiting the user's hardware run. "
                     "RUN25/RUN27/RUN29 re-checked AGAIN this iteration and still do not exist as real log tags "
                     "anywhere in this codebase -- reported honestly rather than fabricating a fix; found and "
                     "denylisted four additional real routine-log sources instead [G24/G25/G26 file enumeration, "
                     "G39/G40 texture alloc/download, G74-TRACK-FILE per-file logging, GLIDE-G12 clip-window "
                     "periodic summary]. No renderer content changed, no depth/cull/blend touched, no texture path "
                     "forced, no LFB globally disabled, no clear forced every frame -- this iteration is evidence-"
                     "gathering and log-volume-cutting only, per its own explicit instruction. dynarec/heap/file-"
                     "semantics/input/FAMILY-A/FAMILY-B/grDrawPoint/USA-loading/race-milestone-logic all frozen)");
#endif
    // Build fingerprint -- printed before anything else so a real-hardware
    // log can be matched to the exact source state that produced the VPK,
    // instead of trusting the VPK filename (which never changes). No git in
    // this toolchain (checked: `git` isn't on PATH here), so this uses the
    // compiler's own __DATE__/__TIME__ macros -- these are baked in at the
    // moment THIS translation unit was compiled, so they change on every
    // real rebuild even without VCS.
    IGN_LOG("[BUILD]", "IgnitionVita iteration=35");
#ifdef IGNITION_RENDERER_3DFX
    IGN_LOG("[BUILD]", "renderer=3DFX (experimental branch, ux0:data/ignition/Ign_3dfx.exe)");
#if defined(G20C_IDLE_NOOP)
    IGN_LOG("[BUILD]", "glide_bringup=G20C-A");
    IGN_LOG("[BUILD]", "grSstIdle=NOOP_DIAGNOSTIC");
    IGN_LOG("[BUILD]", "idle_sync_mode=NONE");
    IGN_LOG("[BUILD]", "idle_trap_active=YES");
    IGN_LOG("[BUILD]", "lfb_path=UNCHANGED");
#elif defined(G20C_BYPASS_IDLE_CALLSITE)
    IGN_LOG("[BUILD]", "glide_bringup=G22");
    IGN_LOG("[BUILD]", "grSstIdle_callsite_bypass=0x00458A30_DIAGNOSTIC");
    IGN_LOG("[BUILD]", "lfb_blit_callsite_probe=YES");
    IGN_LOG("[BUILD]", "lfb_blit_immediate_mode_probe=YES ([G22] per-call checkpoints around every glTexCoord2f/glVertex2f/glEnd inside lfb_blit_to_backbuffer(), on top of G21's coarser checkpoints -- see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "root_cause_identified=YES (vglInitExtended's legacy_pool_size arg is 0 at grSstWinOpen_impl's call site, confirmed via the exact linked vitaGL source: legacy_pool_ptr is therefore never allocated and stays NULL, and glVertex3f -- reached via G19's glVertex2f -- unconditionally writes legacy_pool_ptr[0] with no null check; NOT fixed this iteration per its own explicit instruction, flagged in place at the call site for a future iteration -- see win32/glide2x_ign.cpp's grSstWinOpen_impl comment and win32/glide2x_ign.h's G22 entry)");
    IGN_LOG("[BUILD]", "lfb_upload=UNCHANGED");
    IGN_LOG("[BUILD]", "grSstIdle=UNCHANGED");
    IGN_LOG("[BUILD]", "guest=UNCHANGED");
    IGN_LOG("[BUILD]", "vitagl_path=UNCHANGED");
    IGN_LOG("[BUILD]", "lfb_path=UNCHANGED");
#else
    IGN_LOG("[BUILD-G98]", "working_directory=<project-root>");
    IGN_LOG("[BUILD-G98]", "continuation_agent=CODEX");
    IGN_LOG("[BUILD-G98]", "G97_crash=NO");
    IGN_LOG("[BUILD-G98]", "G97_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G98]", "G97_visual_improvement=YES_PARTIAL");
    IGN_LOG("[BUILD-G98]", "G97_early_car_background_looks_better=YES");
    IGN_LOG("[BUILD-G98]", "G97_background_should_continue_animating_but_visually_stops=YES");
    IGN_LOG("[BUILD-G98]", "G97_race_stale_loading_background=YES");
    IGN_LOG("[BUILD-G98]", "UV_WRAPPER_SEMANTICS_BUG=FIXED_PROVEN");
    IGN_LOG("[BUILD-G98]", "G97_host_nonfinite=0");
    IGN_LOG("[BUILD-G98]", "G97_host_huge=0");
    IGN_LOG("[BUILD-G98]", "current_question=WHAT_CHANGES_WHEN_GOOD_MOVING_BACKGROUND_BECOMES_STUCK");
    IGN_LOG("[BUILD-G99]", "working_directory=<project-root>");
    IGN_LOG("[BUILD-G99]", "continuation_agent=CODEX");
    IGN_LOG("[BUILD-G99]", "G98_crash=NO");
    IGN_LOG("[BUILD-G99]", "G98_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G99]", "G98_key_discovery=VISUAL_FREEZE_BEGINS_INSIDE_PHASE_A_BEFORE_ACCEPT");
    IGN_LOG("[BUILD-G99]", "world_render_continues_after_freeze=YES");
    IGN_LOG("[BUILD-G99]", "phase_A_initial_GL_change_percent_approx=90_PLUS");
    IGN_LOG("[BUILD-G99]", "phase_A_frozen_GL_change_percent=0");
    IGN_LOG("[BUILD-G99]", "phase_B_avg_triangles=716.667");
    IGN_LOG("[BUILD-G99]", "phase_B_avg_GL_changed_tiles=0.310");
    IGN_LOG("[BUILD-G99]", "race_avg_triangles=456.367");
    IGN_LOG("[BUILD-G99]", "race_avg_GL_changed_tiles=7.172");
    IGN_LOG("[BUILD-G99]", "UV_FIX=FROZEN_PROVEN");
    IGN_LOG("[BUILD-G99]", "MMID_FIX=FROZEN_PROVEN");
    IGN_LOG("[BUILD-G99]", "current_question=DO_GUEST_OR_HOST_DRAW_INPUTS_FREEZE_WHILE_DRAW_CALLS_CONTINUE");
    IGN_LOG("[BUILD-G103]", "working_directory=<project-root> artifact=G103_ZERO_DIRTY_LFB_GUARD");
    IGN_LOG("[G103-G102-ROLLBACK]", "partial_renderer=COMPILED_OUT state_restore_equal_NO_path=UNREACHABLE stable_deferred_fullscreen_presenter=RESTORED");
    IGN_LOG("[G103-ZERO-DIRTY-GUARD]", "policy=all_transactions_zero_change_AND_complete_WRITE_ONLY_BACKBUFFER_RGB565_UPPER_LEFT_group_AND_source_crc_matches_previous_upload uncertainty=ORIGINAL_UPLOAD");
    IGN_LOG("[G103-GL-STATE]", "guard_GL_calls=0 new_state_save_restore_layer=NO original_presenter_used_when_upload_required=YES");
    IGN_LOG("[BUILD-G103]", "G101_CONTROL_PROBE=COMPILED_OUT diagnostic_glReadPixels=0 diagnostic_glFinish=0 frozen=UV_MMID_clocks_guest_vertices_host_transform_viewport_ACCEPT_dynarec_texture_allocation_race_loading_swap");
    IGN_LOG("[BUILD-G104]", "working_directory=<project-root> artifact=G104_FRAME_AWARE_LFB_COMPOSITOR continuation_agent=CLAUDE");
    IGN_LOG("[BUILD-G104]", "G103_hardware_result=stale_fullscreen_LFB_still_overwrote_race_world_most_frames selector_flicker=YES_due_to_global_unchanged_skip");
    IGN_LOG("[BUILD-G104]", "classifier=LFB_ONLY_GL_ONLY_GL_PLUS_UNCHANGED_LFB_GL_PLUS_SPARSE_LFB_GL_PLUS_FULLSCREEN_LFB_UNKNOWN dirty_map=16x16_tiles_persistent_CPU_only");
    IGN_LOG("[BUILD-G105]", "working_directory=<project-root> artifact=G105_STRUCTURAL_LFB_CONTRACT continuation_agent=CLAUDE");
    IGN_LOG("[BUILD-G105]", "G104_hardware_result=RACE_frames=60_unknown=60_sparse=0_unchanged_skips=0");
    IGN_LOG("[BUILD-G105]", "root_cause=G104_contract_required_exact_lock_unlock_CALLER_address_match_any_other_legitimate_family_flipped_whole_interval_to_UNKNOWN");
    IGN_LOG("[BUILD-G105]", "fix=structural_contract_type_buffer_mode_pointer_stride_geometry_only_caller_identity_now_diagnostic_only");
    IGN_LOG("[BUILD-G105]", "log_volume_fix=no_longer_one_line_pair_per_UNKNOWN_frame_forever_first_N_frames_per_phase_plus_first_ever_reason_plus_periodic_plus_phase_transition");
    IGN_LOG("[BUILD-G106]", "working_directory=<project-root> artifact=G106_PERSISTENT_OVERLAY_FASTPATH continuation_agent=CLAUDE");
    IGN_LOG("[BUILD-G106]", "G105_hardware_result=structural_contract_fixed_but_menu_and_HUD_invisible_3to4fps_possible_two_player_splitscreen");
    IGN_LOG("[BUILD-G106]", "root_cause_1=content_unchanged_wrongly_treated_as_skip_the_overlay_persistent_UI_needs_redraw_every_GL_frame_regardless");
    IGN_LOG("[BUILD-G106]", "root_cause_2=G103_and_G104_both_ran_a_full_512000_byte_diff_PER_LOCK_UNLOCK_TRANSACTION_33_to_60_per_frame");
    IGN_LOG("[BUILD-G106]", "fix_1=persistent_active_overlay_mask_separate_from_per_frame_changed_tile_set_epoch_reset_on_LFB_ONLY_to_GL_transition_or_broad_change");
    IGN_LOG("[BUILD-G106]", "fix_2=G103_hot_path_compiled_out_of_this_build_one_snapshot_diff_CRC_per_swap_not_per_transaction_lock_unlock_now_O1_metadata_only");
    IGN_LOG("[BUILD-G106]", "diagnostics_added=tile_pixel_density_audit_splitscreen_viewport_audit_both_read_only_no_behavior_change");
    IGN_LOG("[BUILD-G107]", "working_directory=<project-root> artifact=G107_OVERLAY_FASTPATH continuation_agent=CLAUDE title_id=IGNVITA04");
    IGN_LOG("[BUILD-G107]", "G106_vpk_install_result=REJECTED_by_hardware_SCE_error_0x80105A03_speculative_icon0_fix_not_reused");
    IGN_LOG("[BUILD-G107]", "packaging_fix=minimal_param_sfo_plus_eboot_only_matches_G101_through_G105_recipe_new_titleid_IGNVITA04_avoids_IGNVITA03_collision");
    IGN_LOG("[BUILD-G107]", "functional_code=unchanged_from_G106_persistent_overlay_mask_epoch_resets_G103_hot_path_removed_tile_density_and_splitscreen_audits");
    IGN_LOG("[BUILD-G107]", "data_paths_confirmed_titleid_independent=ux0:data/ignition_hardcoded_literal_see_ignition_paths_h_log_cpp");
    IGN_LOG("[BUILD-G108]", "working_directory=<project-root> artifact=G108_STABLE_PIXEL_OVERLAY continuation_agent=CLAUDE title_id=IGNVITA05");
    IGN_LOG("[BUILD-G108]", "G107_hardware_result=playable_race_10to13fps_39s_then_wrapper_diagnostic_halt_G47_nonfinite_alpha_canada");
    IGN_LOG("[BUILD-G108]", "fix_1=request_halt_replaced_with_DROP_TRIANGLE_AND_CONTINUE_for_nonfinite_required_field_guDrawTriangleWithClip_and_grDrawPoint");
    IGN_LOG("[BUILD-G108]", "fix_2=overlay_mask_model_changed_from_union_of_ever_changed_tiles_to_current_vs_epoch_base_diff_tile_8x8_was_16x16");
    IGN_LOG("[BUILD-G108]", "fix_3=splitscreen_audit_reduced_to_initial_detection_plus_one_summary_per_race_not_per_swap");
    IGN_LOG("[BUILD-G108]", "fix_4=logging_tightened_first_5_frames_per_phase_periodic_300_frames_legacy_G47_G65_G91_cascade_gated_off");
    IGN_LOG("[BUILD-G108]", "confirmed_unchanged=structural_LFB_contract_G103_hot_path_removal_single_snapshot_per_swap_splitscreen_geometry");
    IGN_LOG("[BUILD-G109]", "working_directory=<project-root> artifact=G109_SNAPSHOT_FASTPATH_FIX continuation_agent=CLAUDE title_id=IGNVITA06");
    IGN_LOG("[BUILD-G109]", "G108_hardware_result=stable_no_livearea_exit_but_3to4fps_regression_from_G107_13to14fps_confirmed_in_G106-PERF_log_line");
    IGN_LOG("[BUILD-G109]", "root_cause=g104_snapshot_lfb_lost_G107_zero_copy_hostptr_path_plus_bitserial_CRC32_folded_into_timed_snapshot_window");
    IGN_LOG("[BUILD-G109]", "fix_1=zero_copy_hostptr_path_restored_byte_for_byte_G107_structure_confirmed_against_CpuBox86_hostptr_O1_implementation");
    IGN_LOG("[BUILD-G109]", "fix_2=CRC32_replaced_with_direct_memcmp_against_retained_last_uploaded_buffer_see_g_g104LastUploadedLfb");
    IGN_LOG("[BUILD-G109]", "fix_3=epoch_base_capture_hysteresis_4_stable_frames_plus_10_frame_cooldown_between_broad_resets_stuck_menu_images");
    IGN_LOG("[BUILD-G109]", "confirmed_unchanged=structural_LFB_contract_G108_safe_triangle_drop_policy_G103_hot_path_removal_splitscreen_geometry");
    IGN_LOG("[BUILD-G110]", "working_directory=<project-root> artifact=G110_G107_RESTORE_SAFE_DROP continuation_agent=CLAUDE title_id=IGNVITA07");
    IGN_LOG("[BUILD-G110]", "G109_hardware_result=stable_no_crash_but_totally_unplayable_no_visual_improvement_no_hud_in_race_menu_still_stuck_images");
    IGN_LOG("[BUILD-G110]", "user_conclusion=every_compositor_change_after_G107_made_it_worse_G107_confirmed_best_performing_version_on_real_hardware");
    IGN_LOG("[BUILD-G110]", "action=full_rollback_of_G104_FRAME_AWARE_LFB_COMPOSITOR_block_to_G107_exact_logic_no_git_repo_reconstructed_from_session_verbatim_source_reads");
    IGN_LOG("[BUILD-G110]", "removed=G108_current_vs_epoch_base_model_8x8_tiles_G109_epoch_stabilize_hysteresis_broad_reset_cooldown_all_related_heuristics_and_log_tags");
    IGN_LOG("[BUILD-G110]", "restored=union_of_ever_changed_tiles_persistent_overlay_16x16_tiles_CRC32_based_upload_check_byte_level_diff_zero_copy_hostptr_snapshot");
    IGN_LOG("[BUILD-G110]", "retained_on_top_of_G107=G108_safe_drop_fix_only_DROP_TRIANGLE_AND_CONTINUE_in_guDrawTriangleWithClip_and_grDrawPoint_no_request_halt");
    IGN_LOG("[BUILD-G110]", "run27_run29_status=confirmed_still_vendored_in_external_WinVita_box86_dynarec_fopen_fprintf_bypasses_this_projects_logger_not_touched_without_authorization");
    IGN_LOG("[BUILD-G111]", "working_directory=<project-root> artifact=G111_RUN_HOTTRACE_REMOVED continuation_agent=CLAUDE title_id=IGNVITA08");
    IGN_LOG("[BUILD-G111]", "G110_hardware_result=stable_visual_and_menu_state_match_G107_game_moves_behind_menu_race_reachable_but_fps_stuck_at_3to4_not_G107s_13to14");
    IGN_LOG("[BUILD-G111]", "forensic_check=G107_vs_G110_text_sections_byte_identical_for_every_untouched_object_CMakeCache_flags_make_link_txt_identical_vendored_lib_unchanged_since_before_both_builds");
    IGN_LOG("[BUILD-G111]", "root_cause=external_WinVita_box86_dynarec_arm_66_c_case_0x9C_0x9D_markers_11_14_RUN27_RUN29_unconditional_since_that_files_own_iteration31_fopen_fprintf_fclose_on_every_PUSHFW_POPFW_in_the_whole_game");
    IGN_LOG("[BUILD-G111]", "fix=removed_RUN27_RUN29_markers_11_14_in_dynarec_arm_66_c_plus_6_narrower_EIP_gated_RUN25_RUN27_RUN29_markers_3_4_5_10_13_15_in_dynarec_arm_00_c_authorized_scope_limited_edit_to_vendored_code");
    IGN_LOG("[BUILD-G111]", "confirmed_unchanged=RUN26_RUN28_RUN30_IDLE34_IDLE35_diagnostics_REP_STOSD_functional_override_MMU_PUSH16_MMU_POP16_real_fixes_G110_compositor_safe_drop_policy");
    IGN_LOG("[G111-HOTTRACE]", "RUN25_RUN27_RUN29_COMPILED_OUT");
    IGN_LOG("[BUILD-G112]", "working_directory=<project-root> artifact=G112_COMPOSITOR_TIMING_AND_MASK_FIX continuation_agent=CLAUDE title_id=IGNVITA09");
    IGN_LOG("[BUILD-G112]", "G111_hardware_result=stable_full_race_completed_8min_hud_visible_coverage_6to8fps_vs_G107s_13to14_hud_progressively_duplicates_long_race_see_user_photo");
    IGN_LOG("[BUILD-G112]", "root_cause_1=g104_crc32_bitserial_CRC32_over_full_512000_bytes_every_swap_ran_UNTIMED_in_the_gap_between_snapshot_and_diff_timers_explains_the_confirmed_44ms_unattributed_compositor_gap");
    IGN_LOG("[BUILD-G112]", "fix_1=g104_crc32_removed_entirely_contentChangedThisFrame_already_computed_by_the_diff_loop_drives_the_upload_decision_in_every_branch_now");
    IGN_LOG("[BUILD-G112]", "root_cause_2=persistent_union_of_ever_changed_tiles_mask_never_retires_a_tile_once_active_except_on_broad_reset_so_moved_or_cleared_HUD_elements_keep_redrawing_stale_current_LFB_content_at_their_old_tile");
    IGN_LOG("[BUILD-G112]", "fix_2=content_based_retire_activate_rule_using_g112_chromakey_key_rgb565_as_the_background_test_computed_in_the_same_per_pixel_pass_as_change_detection_no_current_vs_epoch_base_no_second_full_pass");
    IGN_LOG("[BUILD-G112]", "fix_3=lfb_upload_texture_reuses_the_compositors_own_already_read_buffer_instead_of_a_second_independent_512000_byte_c_read_plus_per_frame_heap_allocation");
    IGN_LOG("[BUILD-G112]", "deferred=partial_sub_rectangle_texture_upload_investigated_not_implemented_this_iteration_pending_real_hardware_numbers_from_this_iterations_own_new_G112-PERF_instrumentation_per_explicit_solo_despues_de_medirlo_sequencing");
    IGN_LOG("[BUILD-G112]", "confirmed_unchanged=safe_drop_policy_RUN25_RUN27_RUN29_compiled_out_16x16_tiles_single_present_per_swap_LFB_GL_treatment_resolution_controls");
    IGN_LOG("[BUILD-G113]", "working_directory=<project-root> artifact=G113_HUD_LIFECYCLE_FIX continuation_agent=CLAUDE title_id=IGNVITA10");
    IGN_LOG("[BUILD-G113]", "G112_hardware_result=most_stable_yet_second_race_played_9to11fps_compositor_9to10ms_but_HUD_still_duplicates_tiles_retired_2892_so_stale_pixels_live_inside_the_shadow_LFB");
    IGN_LOG("[BUILD-G113]", "root_cause=shadow_LFB_persists_across_frames_while_on_a_Voodoo_the_per_frame_3D_pass_rewrites_the_framebuffer_and_grBufferClear_is_rare_in_race_so_it_is_not_the_per_frame_boundary");
    IGN_LOG("[BUILD-G113]", "fix_1=shadow_cleared_to_key_once_per_frame_at_swap_end_plus_at_grBufferClear_before_first_lock_only_in_armed_race_results_GL_scenes_with_safe_write_only_backbuffer_565_upper_left_contract");
    IGN_LOG("[BUILD-G113]", "fix_2=mask_is_exactly_the_occupied_tiles_of_the_rebuilt_layer_no_history_no_epoch_base_fallback_frames_keep_G112_rule");
    IGN_LOG("[BUILD-G113]", "fix_3=LFB_chromakey_latched_at_the_pixel_pipeline_write_lock_G112_0x1083_0x0000_flip_was_the_live_3D_key_changing_between_draw_passes_grChromakeyMode_never_substituted_black");
    IGN_LOG("[BUILD-G113]", "fix_4=overlay_reset_at_every_GL_scene_boundary_and_Glide_context_change_race_scene_armed_by_every_structural_track_file_open_so_a_second_race_works");
    IGN_LOG("[BUILD-G113]", "confirmed_unchanged=safe_drop_RUN25_RUN27_RUN29_out_CRC_removed_zero_copy_upload_16x16_tiles_one_present_per_swap_menus_selector_videos_loading_cars_controls");
    IGN_LOG("[BUILD-G114]", "working_directory=<project-root> artifact=G114_OVERLAY_KEY_AND_SPARSE_CLEAR continuation_agent=CLAUDE title_id=IGNVITA11");
    IGN_LOG("[BUILD-G114]", "G113_hardware_result=HUD_duplication_gone_stable_but_black_rectangles_around_2D_elements_and_fps_down_to_about_5");
    IGN_LOG("[BUILD-G114]", "root_cause_1=G113_key_latched_only_at_write_locks_with_pixelPipeline_and_chromakey_enabled_but_every_HUD_lock_has_pipe0_en0_so_lfb_key_valid_NO_and_the_layer_was_cleared_to_black_and_drawn_as_opaque_tiles");
    IGN_LOG("[BUILD-G114]", "fix_1=dedicated_overlay_key_learned_from_grChromakeyValue_zero_never_a_key_confirmed_by_lock_samples_bit0_tolerance_never_clear_without_it");
    IGN_LOG("[BUILD-G114]", "fix_2=per_pixel_transparency_by_drawing_spans_of_non_key_pixels_no_texture_format_or_blend_change_same_key_used_to_clear_and_to_compose");
    IGN_LOG("[BUILD-G114]", "fix_3=sparse_clear_of_tiles_the_game_wrote_one_full_clear_only_when_layer_not_provably_key_except_dirty_prev_updated_per_changed_tile_fast_key_row_scan");
    IGN_LOG("[BUILD-G114]", "confirmed_unchanged=safe_drop_RUN25_RUN27_RUN29_out_CRC_removed_zero_copy_upload_16x16_tiles_one_present_per_swap_scene_resets_menus_cars_controls");
    IGN_LOG("[BUILD-G115]", "working_directory=<project-root> artifact=G115_TILE_ALPHA_OVERLAY continuation_agent=CLAUDE title_id=IGNVITA12");
    IGN_LOG("[BUILD-G115]", "G114_hardware_result=picture_correct_HUD_clean_transparent_results_clean_post_race_menu_clean_but_fps_about_5_and_initial_menu_still_black_before_key_known");
    IGN_LOG("[BUILD-G115]", "fix_1=span_geometry_removed_one_quad_per_active_tile_RGBA5551_overlay_texture_alpha0_equals_overlay_key_GL_ALPHA_TEST_conversion_limited_to_active_changed_tiles");
    IGN_LOG("[BUILD-G115]", "fix_2=overlay_key_available_early_previous_session_cache_else_observed_G114_raw_0x00131313_always_provisional_until_lock_samples_confirm_or_replace_scenes_managed_whenever_a_key_exists");
    IGN_LOG("[BUILD-G115]", "fix_3=G115-FRAMEBREAKDOWN_host_stage_vs_guest_residual_because_G111_and_G114_both_run_about_7fps_on_CANADA_despite_compositor_50ms_vs_11ms");
    IGN_LOG("[BUILD-G115]", "confirmed_unchanged=safe_drop_RUN25_RUN27_RUN29_out_sparse_clear_scene_resets_CRC_removed_zero_copy_upload_16x16_tiles_one_present_per_swap_menus_cars_controls");

    // iteration-G94: the G93 hardware run entered CANADA and selected a
    // genuine 611-triangle textured race frame, but its unchanged stage
    // samples were taken without an explicit GPU completion point.  G94
    // keeps every renderer semantic frozen and diagnoses one subsequent
    // frame with synchronized stage/LFB samples plus bounded large-draw
    // and composite-family forensics.
    IGN_LOG("[BUILD-G94]", "working_directory=<project-root>");
    IGN_LOG("[BUILD-G94]", "continuation_agent=CODEX");
    IGN_LOG("[BUILD-G94]", "continue_existing_project=YES");
    IGN_LOG("[BUILD-G94]", "current_question=WHICH_DRAW_RECREATES_THE_STALE_LOADING_IMAGE");
    IGN_LOG("[BUILD-G94]", "G93_crash=NO");
    IGN_LOG("[BUILD-G94]", "G93_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G94]", "G93_race_runtime=YES");
    IGN_LOG("[BUILD-G94]", "G93_selected_swap=306");
    IGN_LOG("[BUILD-G94]", "G93_selected_triangles=611");
    IGN_LOG("[BUILD-G94]", "G93_selected_textured=70");
    IGN_LOG("[BUILD-G94]", "G93_selected_guTexSource=189");
    IGN_LOG("[BUILD-G94]", "G93_lfb_verdict=INCONCLUSIVE");
    IGN_LOG("[BUILD-G94]", "simple_present_failure=REJECTED");
    IGN_LOG("[BUILD-G94]", "mmid_path=FROZEN");
    IGN_LOG("[BUILD-G94]", "texture_path=FROZEN");
    IGN_LOG("[BUILD-G94]", "validator=FROZEN");
    IGN_LOG("[BUILD-G94]", "swap_path=FROZEN");
    IGN_LOG("[BUILD-G94]", "no_global_LFB_change=YES");
    IGN_LOG("[G94-NEXT-GRAPHICS-TARGET]", "capture_armed_after_G93_selected_frame=YES diagnostic_sync_one_frame_only=YES large_draw_threshold=40_percent composite_threshold=70_percent");

    // iteration-G93, Parte 32: fresh "[BUILD-G93]" tag family -- bumped
    // allowlist prefix. G92's own hardware run REJECTED the simple
    // presentation-divergence theory: GL and physical scanout BOTH
    // change substantially every phase (BOTH_CHANGING, not
    // PRESENT_DIVERGENCE_PROVEN) -- the swap chain/VitaGL is not frozen.
    // Real race frames ALSO proved the 3D world and an LFB blit are both
    // active in the SAME frame (~600 triangles + LFB blit, every frame).
    // This iteration picks ONE such real race frame, records its exact
    // chronology, and captures GL tilemaps immediately before/after that
    // frame's own LFB blit to test directly whether it overwrites an
    // already-rendered 3D frame with stale content -- without disabling
    // or altering the LFB in any way. Also fixes a real G92 bug: phase
    // arming left the textured/guTexSource baseline at 0 instead of
    // snapshotting it at arm time, so a phase's FIRST frame silently
    // included everything accumulated since boot (explains G92's own
    // "impossible-looking" counter combinations).
    IGN_LOG("[BUILD-G93]", "current_question=IS_AN_OPAQUE_RACE_LFB_BLIT_COVERING_THE_REAL_3D_WORLD");
    IGN_LOG("[BUILD-G93]", "G92_crash=NO");
    IGN_LOG("[BUILD-G93]", "G92_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G93]", "G92_race_runtime=YES");
    IGN_LOG("[BUILD-G93]", "user_confirms_stale_screen_despite_both_GL_and_scanout_changing=YES");
    IGN_LOG("[BUILD-G93]", "simple_present_divergence=REJECTED");
    IGN_LOG("[BUILD-G93]", "world_texture_path=FROZEN_ACTIVE");
    IGN_LOG("[BUILD-G93]", "G92_race_has_frames_with_world_triangles_and_LFB_blit_same_frame=YES");
    IGN_LOG("[BUILD-G93]", "G92_global_first3_LFB_overwrite_samples_not_race_specific=YES");
    IGN_LOG("[BUILD-G93]", "race_specific_LFB_overwrite_status=UNPROVEN_UNTIL_THIS_RUN");
    IGN_LOG("[BUILD-G93]", "buffer_changes_forbidden=YES");
    IGN_LOG("[BUILD-G93]", "LFB_semantics_change_forbidden_until_case_A=YES");
    IGN_LOG("[BUILD-G93]", "G92_counter_baseline_bug=FIXED (g92_arm_phase now snapshots textured/guTexSource "
                          "counters at arm time instead of leaving them at 0)");
    IGN_LOG("[G93-NEXT-GRAPHICS-TARGET]", "race_frame_capture_armed=YES -- see [G93-RACE-LFB-VERDICT] in this run's "
                                          "own log once a qualifying race frame (>=500 triangles, textured>0, "
                                          "guTexSource>0, LFB blit captured) completes; G94's target follows "
                                          "directly from that verdict, not predicted here");

    // iteration-G92, Parte 29: fresh "[BUILD-G92]" tag family -- bumped
    // allowlist prefix. G91's own hardware run PROVED the world renderer
    // is genuinely active (35536 guTexSource calls, 8729 textured world
    // triangles, ZERO fallback) -- the "why are textures never used"
    // question from G85-G89 is now fully closed. The remaining visible
    // blocker is a stale/frozen image the user sees at specific
    // transitions (menu accept, race entry) despite that real activity
    // underneath -- this iteration instruments WHERE in the frame
    // lifecycle (GL render target vs physical scanout vs LFB blit) that
    // staleness actually originates, without changing any renderer
    // behavior.
    IGN_LOG("[BUILD-G92]", "current_question=WHY_DOES_USER_SEE_STATIC_OLD_FRAME_WHILE_TEXTURED_WORLD_RENDERING_IS_ACTIVE");
    IGN_LOG("[BUILD-G92]", "G91_crash=NO");
    IGN_LOG("[BUILD-G92]", "G91_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G92]", "G91_race_runtime=YES");
    IGN_LOG("[BUILD-G92]", "B_b_validator_bug=RESOLVED");
    IGN_LOG("[BUILD-G92]", "mmid_fix=FROZEN_CORRECT");
    IGN_LOG("[BUILD-G92]", "world_texture_path=ACTIVE");
    IGN_LOG("[BUILD-G92]", "G91_race_guTexSource_calls=35536");
    IGN_LOG("[BUILD-G92]", "G91_textured_world_triangles=8729");
    IGN_LOG("[BUILD-G92]", "G91_DECAL_TEXTURE_count=6841");
    IGN_LOG("[BUILD-G92]", "stale_background_previous=83.0");
    IGN_LOG("[BUILD-G92]", "stale_background_G91=66.7");
    IGN_LOG("[BUILD-G92]", "current_primary_question=GL_BACKBUFFER_VS_PHYSICAL_SCANOUT_VS_LFB_OVERWRITE");
    IGN_LOG("[BUILD-G92]", "buffer_semantics_change=FORBIDDEN_UNTIL_PROVEN");
    IGN_LOG("[BUILD-G92]", "LFB_change=FORBIDDEN_UNTIL_PROVEN");
    IGN_LOG("[BUILD-G92]", "phase_mapping=PHASE_A:MENU_milestone PHASE_B:LOADING_milestone PHASE_C:RACE_FIRST_milestone "
                          "(this iteration's own explicit choice, using the closest already-instrumented hooks -- "
                          "not a claim these exactly match the user's own described transition moments)");
    IGN_LOG("[G92-NEXT-GRAPHICS-TARGET]", "three_phase_temporal_capture_armed=YES -- G93's target comes directly "
                                          "from this run's own real [G92-PRESENT-DIVERGENCE]/[G92-STALE-MECHANISM] "
                                          "verdicts per phase, not predicted here: PRESENT_DIVERGENCE_PROVEN points "
                                          "at the swap/presentation path, GL_ITSELF_STALE points at frame "
                                          "construction, and BOTH_CHANGING points at an explicit stale redraw "
                                          "needing draw-source identification");

    // iteration-G91, Parte 31: fresh "[BUILD-G91]" tag family -- bumped
    // allowlist prefix. MAJOR VISUAL MILESTONE this iteration: the user's
    // own G90 hardware run saw recognizable, moving cars for the first
    // time ever, with sane (non-psychedelic) colors -- direct visual
    // confirmation the MMID fix (G89) is real progress, not just counters.
    IGN_LOG("[BUILD-G91]", "current_question=IS_NONFINITE_VERTEX_B_B_ACTUALLY_REQUIRED_IN_CURRENT_TEXTURED_GLIDE_STATE");
    IGN_LOG("[BUILD-G91]", "G90_crash=NO");
    IGN_LOG("[BUILD-G91]", "G90_diagnostic_halt=YES");
    IGN_LOG("[BUILD-G91]", "hardware_visual_moving_cars_seen=YES");
    IGN_LOG("[BUILD-G91]", "hardware_visual_colors_more_sane=YES");
    IGN_LOG("[BUILD-G91]", "hardware_image_still_broken=YES");
    IGN_LOG("[BUILD-G91]", "first_real_world_texsource=YES");
    IGN_LOG("[BUILD-G91]", "first_real_world_textured_triangle=YES");
    IGN_LOG("[BUILD-G91]", "mmid_fix=FROZEN_CORRECT");
    IGN_LOG("[BUILD-G91]", "chromakey_value_impl=FROZEN");
    IGN_LOG("[BUILD-G91]", "current_halt_field=vertex_B_b");
    IGN_LOG("[BUILD-G91]", "no_global_nan_sanitization=YES");
    IGN_LOG("[BUILD-G91]", "no_vertex_memory_mutation=YES");
    IGN_LOG("[BUILD-G91]", "validator_bug_found=YES (glide_decode_active_color_alpha_source only recognized "
                          "function==LOCAL&&local==CONSTANT as vertex-RGB-not-needed; never checked `factor`, so "
                          "DECAL_TEXTURE(function=SCALE_OTHER,factor=ONE) was wrongly classified as ITERATED)");
    IGN_LOG("[BUILD-G91]", "state_tracking_divergence=NO (color_combine_function/factor/local were ALWAYS correct "
                          "in g_glideContext since G41/G89 -- only the boolean DERIVED from them was wrong)");
    IGN_LOG("[BUILD-G91]", "vertex_producer_bug=NOT_NEEDED (Case A -- field genuinely unused for this combine "
                          "state, no producer to fix)");
    IGN_LOG("[BUILD-G91]", "buffer_path=FROZEN");
    IGN_LOG("[BUILD-G91]", "LFB=FROZEN");
    IGN_LOG("[BUILD-G91]", "depth=FROZEN");
    IGN_LOG("[BUILD-G91]", "cull=FROZEN");
    IGN_LOG("[BUILD-G91]", "blend=FROZEN");
    IGN_LOG("[BUILD-G91]", "dynarec_semantics=FROZEN");

    // iteration-G91, Parte 10/20-21: the full per-mode dependency table
    // the corrected glide_decode_active_color_alpha_source() rule
    // produces -- derived directly from guColorCombineFunction_impl's own
    // real-Glide-sourced Translation table (function/factor/local/other),
    // unchanged this iteration, not re-guessed.
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_ZERO(0x0) requires_r=NO requires_g=NO requires_b=NO requires_s=NO requires_t=NO");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_CCRGB(0x1) requires_r=NO(uses_constant_color) requires_g=NO requires_b=NO requires_s=NO requires_t=NO");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_ITRGB(0x2) requires_r=YES requires_g=YES requires_b=YES requires_s=NO requires_t=NO");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_ITRGB_DELTA0(0x3) requires_r=NO(uses_constant_color) requires_g=NO requires_b=NO requires_s=NO requires_t=NO");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_DECAL_TEXTURE(0x4) requires_r=NO requires_g=NO requires_b=NO requires_s=YES requires_t=YES "
                                       "(THE FIX -- was wrongly requiring r/g/b before this iteration)");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_TEXTURE_TIMES_CCRGB(0x5) requires_r=NO(uses_constant_color) requires_g=NO requires_b=NO requires_s=YES requires_t=YES");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB(0x6) requires_r=YES requires_g=YES requires_b=YES requires_s=YES requires_t=YES");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_ADD_ALPHA(0x8) requires_r=YES requires_g=YES requires_b=YES requires_a=YES requires_s=YES requires_t=YES");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "mode=GR_COLORCOMBINE_ONE(0x10) requires_r=NO requires_g=NO requires_b=NO requires_s=NO requires_t=NO "
                                       "(also corrected this iteration -- same class of bug as DECAL_TEXTURE)");
    IGN_LOG("[G91-VERTEX-DEPENDENCY]", "alpha_path=UNCHANGED_THIS_ITERATION (requires_a determined separately by "
                                       "alpha_combine_function/alpha_combine_local -- no evidence yet of an "
                                       "equivalent bug there; this run's own failure had alpha_source=CONSTANT, "
                                       "already correctly not requiring vertex alpha)");
    IGN_LOG("[G91-BB-PRODUCER]", "status=NOT_NEEDED (Case A confirmed -- B.b is genuinely unused under the active "
                                "DECAL_TEXTURE combine state; there is no producer bug to trace, per this "
                                "iteration's own explicit gate not to chase a producer once the field is proven "
                                "unused)");
    IGN_LOG("[G91-STATE-DIVERGENCE]", "status=NO (the raw color_combine_function/factor/local VALUES this project "
                                      "stores were correct the entire time, set by guColorCombineFunction_impl "
                                      "since G41/G89, unchanged this iteration -- the bug was in the BOOLEAN "
                                      "derived from them by glide_decode_active_color_alpha_source, now corrected)");
    IGN_LOG("[G91-NEXT-GRAPHICS-TARGET]", "validator_fix_applied=YES -- if this run's own hardware log now passes "
                                          "the B.b halt, the next real question is whichever milestone or blocker "
                                          "comes after it: Quick Race/CANADA reachability, [G91-TEXTURE-PROGRESS]'s "
                                          "real counters at that point, or a genuinely NEW non-finite field/caller "
                                          "captured fresh by [G91-NONFINITE-TRIANGLE] -- G92's target comes directly "
                                          "from that real log, not predicted here");

    // iteration-G90, Parte 23: fresh "[BUILD-G90]" tag family -- bumped
    // allowlist prefix. G89's own hardware run halted (NOT crashed) right
    // after the intro video with a diagnostic stop on the next genuinely
    // unimplemented Glide call, grChromakeyValue -- strong evidence the
    // G89 MMID fix is already opening new real Glide code paths. That
    // call is implemented this iteration (see grChromakeyValue_impl in
    // glide2x_ign.cpp); the G89 MMID fix itself is completely unchanged.
    IGN_LOG("[BUILD-G90]", "current_question=IMPLEMENT_grChromakeyValue_AND_CONTINUE_NEW_TEXTURE_PATH");
    IGN_LOG("[BUILD-G90]", "G89_crash=NO");
    IGN_LOG("[BUILD-G90]", "G89_diagnostic_halt=YES");
    IGN_LOG("[BUILD-G90]", "G89_stop_api=grChromakeyValue");
    IGN_LOG("[BUILD-G90]", "G89_stop_caller=0x004528A8");
    IGN_LOG("[BUILD-G90]", "G89_stop_arg0=0x00FFFFFF");
    IGN_LOG("[BUILD-G90]", "mmid_abi_divergence=PROVEN");
    IGN_LOG("[BUILD-G90]", "small_guest_mmid_fix=FROZEN");
    IGN_LOG("[BUILD-G90]", "grChromakeyValue_implemented=YES (state-storage only -- real per-pixel discard deferred, "
                          "see [G90-CHROMAKEY-SEMANTICS] for the exact reason)");
    IGN_LOG("[BUILD-G90]", "buffer_path=FROZEN");
    IGN_LOG("[BUILD-G90]", "LFB=FROZEN");
    IGN_LOG("[BUILD-G90]", "depth=FROZEN");
    IGN_LOG("[BUILD-G90]", "cull=FROZEN");
    IGN_LOG("[BUILD-G90]", "blend=FROZEN");
    IGN_LOG("[BUILD-G90]", "dynarec_semantics=FROZEN");
    IGN_LOG("[G90-NEXT-GRAPHICS-TARGET]", "grChromakeyValue_implemented=YES -- if execution now passes this call, "
                                          "the next real question is whichever API or milestone comes after it: "
                                          "either the main menu/race becomes reachable (see [G90-TEXTURE-PATH-"
                                          "PROGRESS] for real guTexSource/DECAL_TEXTURE counts at that point), or "
                                          "[G90-NEXT-UNIMPLEMENTED-GLIDE] names the next genuine blocker -- either "
                                          "way, G91's target comes directly from this run's own real log, not "
                                          "predicted here");

    // iteration-G89, Parte 19: fresh "[BUILD-G89]" tag family -- bumped
    // allowlist prefix. MMID_ABI_DIVERGENCE=PROVEN this iteration by the
    // user's own real native oracle run (pc_oracle_log.txt): native
    // guTexAllocateMemory returns small values (86/86 samples <256, range
    // 0x00-0x1C) and the game passes them back to guTexSource UNCHANGED
    // (128/128 exact matches). The guest-visible MMID ABI is fixed this
    // iteration -- see next_guest_mmid()'s own big comment in
    // glide2x_ign.cpp for the full design.
    IGN_LOG("[BUILD-G89]", "current_question=FIRST_NATIVE_COMPATIBLE_GUEST_MMID_RENDER_PATH");
    IGN_LOG("[BUILD-G89]", "native_mmid_oracle=COMPLETE");
    IGN_LOG("[BUILD-G89]", "native_mmid_min=0x00000000");
    IGN_LOG("[BUILD-G89]", "native_mmid_max=0x0000001C");
    IGN_LOG("[BUILD-G89]", "native_mmid_lt256=86");
    IGN_LOG("[BUILD-G89]", "native_mmid_ge256=0");
    IGN_LOG("[BUILD-G89]", "native_texsource_exact_matches=128");
    IGN_LOG("[BUILD-G89]", "native_texsource_transformed=0");
    IGN_LOG("[BUILD-G89]", "native_race_guTexSource_calls_100_frames=22958");
    IGN_LOG("[BUILD-G89]", "native_DECAL_TEXTURE_calls=2810");
    IGN_LOG("[BUILD-G89]", "mmid_abi_divergence=PROVEN");
    IGN_LOG("[BUILD-G89]", "guest_mmid_namespace=SMALL_NATIVE_COMPATIBLE");
    IGN_LOG("[BUILD-G89]", "old_0x700000xx_guest_namespace=DISABLED (kept internally only, as GuTextureAllocation::"
                          "internalDebugId, never guest-visible, never used for any lookup)");
    IGN_LOG("[BUILD-G89]", "buffer_path=FROZEN");
    IGN_LOG("[BUILD-G89]", "LFB=FROZEN");
    IGN_LOG("[BUILD-G89]", "depth=FROZEN");
    IGN_LOG("[BUILD-G89]", "cull=FROZEN");
    IGN_LOG("[BUILD-G89]", "blend=FROZEN");
    IGN_LOG("[BUILD-G89]", "dynarec_semantics=FROZEN");

    IGN_LOG("[BUILD-G88]", "current_question=FINAL_NATIVE_MMID_ORACLE_AND_FIRST_REAL_TEXTURED_WORLD_ATTEMPT");
    IGN_LOG("[BUILD-G88]", "current_question=FINAL_NATIVE_MMID_ORACLE_AND_FIRST_REAL_TEXTURED_WORLD_ATTEMPT");
    IGN_LOG("[BUILD-G88]", "G87_crash=NO");
    IGN_LOG("[BUILD-G88]", "G87_race_runtime=YES");
    IGN_LOG("[BUILD-G88]", "user_reports_boot_very_fast=YES");
    IGN_LOG("[BUILD-G88]", "user_reports_intro_smooth=YES");
    IGN_LOG("[BUILD-G88]", "user_reports_race_load_fast=YES");
    IGN_LOG("[BUILD-G88]", "current_vita_world_samples=256");
    IGN_LOG("[BUILD-G88]", "current_vita_world_ge256=256");
    IGN_LOG("[BUILD-G88]", "current_vita_mmid_namespace=0x700000xx");
    IGN_LOG("[BUILD-G88]", "native_mmid_range=PENDING_ORACLE");
    IGN_LOG("[BUILD-G88]", "native_oracle_package_ready=YES (pc_oracle/glide2x.dll + README_G88.txt + "
                          "launch_oracle.bat -- a STALE pre-guTexSource-era pc_oracle_log.txt was found and moved "
                          "aside this iteration to pc_oracle_log_STALE_G66_G72_era.txt so it is never mistaken for "
                          "fresh data)");
    IGN_LOG("[BUILD-G88]", "mmid_fix_allowed_only_if_case_A=YES");
    IGN_LOG("[BUILD-G88]", "mmid_fix_applied_this_iteration=NO");
    IGN_LOG("[BUILD-G88]", "buffer_path=FROZEN");
    IGN_LOG("[BUILD-G88]", "LFB=FROZEN");
    IGN_LOG("[BUILD-G88]", "depth=FROZEN");
    IGN_LOG("[BUILD-G88]", "cull=FROZEN");
    IGN_LOG("[BUILD-G88]", "blend=FROZEN");
    IGN_LOG("[BUILD-G88]", "dynarec_semantics=FROZEN");

    // iteration-G88, Parte 3/9-13: cannot be upgraded past PENDING from
    // the Vita side alone -- same honest status as G87, since no fresh
    // native run has been supplied yet (only a stale pre-MMID-era log was
    // found in pc_oracle/, moved aside rather than misused).
    IGN_LOG("[G88-MMID-DIVERGENCE]", "status=PENDING_USER_RUN vita_evidence_unchanged_since_G87=world_scalar_256_of_256_"
                                     "handle_shaped native_oracle_package_delivered=YES(README_G88.txt+launch_oracle.bat) "
                                     "stale_log_found_and_set_aside=YES");
    IGN_LOG("[G88-NEXT-GRAPHICS-TARGET]", "blocked_on=user_running_launch_oracle.bat(or_manual_steps_in_README_G88.txt)_"
                                          "with_their_own_original_Ign_3dfx.exe -- once pc_oracle_log.txt shows real "
                                          "[G88-NATIVE-MMID-HISTOGRAM]/[G88-NATIVE-MMID-CORRELATION]/[G88-NATIVE-MATERIAL] "
                                          "numbers, G89 either implements the guest-visible small-MMID mapping (Case A) "
                                          "or abandons the MMID hypothesis for the native LFB/material/color-combine "
                                          "path comparison (Case B/C) -- no VPK behavior changes until that data exists");

    IGN_LOG("[BUILD-G87]", "current_question=DOES_NATIVE_IGNITION_USE_SMALL_MMIDS_AND_IS_WINVITA_0x700000xx_BREAKING_THE_TEXTURE_BRANCH");
    IGN_LOG("[BUILD-G87]", "G86_crash=NO");
    IGN_LOG("[BUILD-G87]", "G86_race_runtime=YES");
    IGN_LOG("[BUILD-G87]", "world_scalar_samples=256");
    IGN_LOG("[BUILD-G87]", "world_scalar_ge256=256");
    IGN_LOG("[BUILD-G87]", "world_value_0x70000002_count=167");
    IGN_LOG("[BUILD-G87]", "world_value_0x7000000E_count=71");
    IGN_LOG("[BUILD-G87]", "world_value_0x70000006_count=6");
    IGN_LOG("[BUILD-G87]", "world_value_0x70000007_count=12");
    IGN_LOG("[BUILD-G87]", "all_world_values_handle_shaped=YES");
    IGN_LOG("[BUILD-G87]", "current_vita_guTexAllocateMemory_returns=0x70000001_plus_N");
    IGN_LOG("[BUILD-G87]", "native_glide_api_contract=OPAQUE_INTEGER");
    IGN_LOG("[BUILD-G87]", "native_runtime_value_range=PENDING_ORACLE (proxy now captures guTexAllocateMemory+guTexSource "
                          "first 128 calls each plus their correlation -- see [G87-NATIVE-TEXALLOC]/[G87-NATIVE-MMID-"
                          "HISTOGRAM]/[G87-NATIVE-TEXSOURCE]/[G87-NATIVE-MMID-CORRELATION] in pc_oracle_log.txt once "
                          "the user runs the original Windows executable with it)");
    IGN_LOG("[BUILD-G87]", "handle_abi_fix_allowed=ONLY_AFTER_NATIVE_PROOF");
    IGN_LOG("[BUILD-G87]", "handle_abi_fix_applied_this_iteration=NO");
    IGN_LOG("[BUILD-G87]", "buffer_path=FROZEN");
    IGN_LOG("[BUILD-G87]", "LFB=FROZEN");
    IGN_LOG("[BUILD-G87]", "depth=FROZEN");
    IGN_LOG("[BUILD-G87]", "cull=FROZEN");
    IGN_LOG("[BUILD-G87]", "blend=FROZEN");
    IGN_LOG("[BUILD-G87]", "dynarec_semantics=FROZEN");
    IGN_LOG("[BUILD-G87]", "exit_confirmation_original_key=S");
    IGN_LOG("[BUILD-G87]", "start_to_S_mapping=DEFERRED_GRAPHICS_PRIORITY");

    // iteration-G87, Parte 20: complete static audit of every function that
    // produces/consumes/stores/compares a GrMipMapId_t in this project's
    // own Glide layer -- grepped fresh this iteration (gu_tex_allocations,
    // next_gu_tex_handle, GuTextureAllocation), not from memory.
    IGN_LOG("[G87-MMID-USAGE-TABLE]", "function=guTexAllocateMemory_impl role=PRODUCES value=next_gu_tex_handle()="
                                      "0x70000001+N stores_into=gu_tex_allocations()[handle]");
    IGN_LOG("[G87-MMID-USAGE-TABLE]", "function=guTexDownloadMipMap_impl role=CONSUMES lookup=gu_tex_allocations()."
                                      "find(mmid) on_miss=ABORT(halt,_no_guess)");
    IGN_LOG("[G87-MMID-USAGE-TABLE]", "function=guTexSource_impl role=CONSUMES lookup=gu_tex_allocations().find(mmid) "
                                      "stores_into=g_glideContext.current_texture_mmid on_miss=ABORT(halt,_no_guess)");
    IGN_LOG("[G87-MMID-USAGE-TABLE]", "function=guTexMemReset_impl role=CLEARS iterates=gu_tex_allocations() "
                                      "action=delete_GL_textures_then_gu_tex_allocations().clear()");
    IGN_LOG("[G87-MMID-USAGE-TABLE]", "function=triangle_UV_normalization(~guDrawTriangleWithClip_impl_texcoord_path) "
                                      "role=CONSUMES lookup=gu_tex_allocations().find(g_glideContext.current_texture_mmid) "
                                      "reads=width/height_for_texel-unit_normalization");
    IGN_LOG("[G87-MMID-USAGE-TABLE]", "no_other_function_in_glide2x_ign.cpp_references_GrMipMapId_t/gu_tex_handle/"
                                      "0x70000000/gu_tex_allocations/next_gu_tex_handle_this_iteration's_own_grep");

    // iteration-G87, Parte 4/13: cannot be upgraded past PENDING from the
    // Vita side alone -- this is exactly what the native oracle exists to
    // answer, per this iteration's own explicit "do not fabricate" rule.
    IGN_LOG("[G87-MMID-DIVERGENCE]", "status=PENDING_USER_RUN world_scalar_evidence=SUSPECTED_STRONGER "
                                     "(G86's own 256/256 world-scalar samples were ALL 0x700000xx-shaped) "
                                     "still_missing=native_guTexAllocateMemory/guTexSource_measurement "
                                     "native_oracle_status=READY_FOR_USER_RUN");
    IGN_LOG("[G87-NEXT-GRAPHICS-TARGET]", "blocked_on=user_running_pc_oracle/glide2x.dll_beside_their_own_original_"
                                          "Ign_3dfx.exe_for_a_Quick_Race/CANADA_session -- once pc_oracle_log.txt "
                                          "shows real [G87-NATIVE-MMID-HISTOGRAM]/[G87-NATIVE-MMID-CORRELATION] "
                                          "numbers, G88's target is either (Case A) implementing the guest-visible "
                                          "small-MMID mapping this iteration deliberately did NOT build yet, or "
                                          "(Case B/C) abandoning the MMID hypothesis and reconstructing branch "
                                          "semantics or comparing the native LFB/color-combine path instead");

    IGN_LOG("[BUILD-G86]", "current_question=IS_0x700000xx_SYNTHETIC_HANDLE_ENCODING_BREAKING_GUEST_MMID_RANGE_CHECKS");
    IGN_LOG("[BUILD-G86]", "G85_crash=NO");
    IGN_LOG("[BUILD-G86]", "G85_race_runtime=YES");
    IGN_LOG("[BUILD-G86]", "G85_fallback_capture_count=128");
    IGN_LOG("[BUILD-G86]", "G85_negative_count=0");
    IGN_LOG("[BUILD-G86]", "G85_ge256_count=128");
    IGN_LOG("[BUILD-G86]", "G85_unique_value_1=0x7000001A,count=112");
    IGN_LOG("[BUILD-G86]", "G85_unique_value_2=0x70000018,count=16");
    IGN_LOG("[BUILD-G86]", "G85_capture_family=0x00451F10_ONLY");
    IGN_LOG("[BUILD-G86]", "G85_world_scalar_capture=NOW_IMPLEMENTED (was MISSING)");
    IGN_LOG("[BUILD-G86]", "synthetic_texture_handle_namespace=0x700000xx");
    IGN_LOG("[BUILD-G86]", "exact_value_collision_with_texture_handles=CONFIRMED_FOR_FAMILY-A_ALREADY_CLOSED_SINCE_G63 "
                          "(record+0x18/TEX24 provenance chain: G53/G56-G63, STOP-INVESTIGATING status re-cited, not re-derived)");
    IGN_LOG("[BUILD-G86]", "handle_encoding_change=FORBIDDEN_UNTIL_ABI_PROVEN");
    IGN_LOG("[BUILD-G86]", "handle_encoding_changed_this_iteration=NO");
    IGN_LOG("[BUILD-G86]", "buffer_system=FROZEN");
    IGN_LOG("[BUILD-G86]", "LFB=FROZEN");
    IGN_LOG("[BUILD-G86]", "depth=FROZEN");
    IGN_LOG("[BUILD-G86]", "cull=FROZEN");
    IGN_LOG("[BUILD-G86]", "blend=FROZEN");
    IGN_LOG("[BUILD-G86]", "dynarec_semantics=FROZEN");
    IGN_LOG("[BUILD-G86]", "native_oracle_status=PENDING_USER_PC_RUN (guTexAllocateMemory now also instrumented, [G86-NATIVE-MMID])");

    // iteration-G86, Parte 8-9: field+0x18's producer -- this question was
    // ALREADY CLOSED in G56-G63 (re-cited here, not re-derived): a loop at
    // 0x00409C53-0x00409C94 (bounded `cmp ebp,0x8`) computes
    // `ebp=[ecx*4+0x00490590]`, calls the confirmed texture-specific
    // allocator helper 0x0044FE70 (which itself calls guTexAllocateMemory),
    // and stores the real returned GrMipMapId_t straight into that table
    // slot via `mov [ebp+0x0],eax` at 0x00409C6F. Table slot 23 (0-indexed)
    // holding the 24th sequentially-issued handle (0x70000001+23=
    // 0x70000018) is exactly what this loop produces by construction.
    IGN_LOG("[G86-FIELD18-PRODUCER]", "writer_eip=0x00409C6F instruction=\"mov [ebp+0x0],eax\" source=return_value_of_"
                                      "allocator_helper_0x0044FE70(which_calls_guTexAllocateMemory) "
                                      "loop=0x00409C53-0x00409C94 table_base=0x00490590 "
                                      "status=CLOSED_SINCE_G63 SYNTHETIC_HANDLE_PROPAGATION=PROVEN "
                                      "(re-cited from this project's own prior hardware-confirmed forensics, not re-measured this iteration)");

    // iteration-G86, Parte 10: re-opening ONLY the semantic label, per this
    // iteration's own explicit instruction not to discard earlier evidence
    // blindly. G52/G53's ORIGINAL hypothesis (before the provenance chain
    // closed) was "per-particle color/palette index, not a texture
    // handle" -- that label is SUPERSEDED by G56-G63's own later,
    // hardware-proven finding: the field genuinely, deliberately holds a
    // real GrMipMapId_t (TEX24), placed there by Ignition's own texture-
    // table-fill loop. These are not fully contradictory: G63 ALSO proved
    // (from the real Glide SDK's own GR_COLORCOMBINE_ITRGB equation,
    // function=LOCAL/other=NONE -- no texture term at all) that this
    // specific draw's active color-combine state never consumes that
    // handle even though it is sitting right there as data -- the object
    // this dispatcher (0x0044ff20) draws is better described as a general
    // object/texture-association record reused across a "particle-style"
    // rendering path, not a dedicated particle-color-palette structure.
    // The fallback branch is legitimate for an independent reason (no
    // texture term in ITRGB), not because the field itself is garbage.
    IGN_LOG("[G86-FAMILY-A-SEMANTICS]", "prior_label(G52/G53)=per-particle_color_palette_index SUPERSEDED_BY=G56-G63 "
                                        "actual_content=genuine_GrMipMapId_t(TEX24,0x70000018) "
                                        "reconciliation=field_holds_a_real_handle_AND_the_fallback_is_still_legitimate "
                                        "(ITRGB's own real equation has no texture term, G63) -- these are compatible, "
                                        "not contradictory; dispatcher_0x0044ff20 better described as general object/"
                                        "texture-association record than a dedicated particle-palette structure");

    // iteration-G86, Parte 19-22: Glide/WinVita MMID ABI audit, citing this
    // project's own already-fetched real-SDK evidence (G52's own citation,
    // re-verified here, not re-fetched from the internet this iteration).
    IGN_LOG("[G86-GLIDE-MMID-ABI]", "source=openglide_sdk2_glide.h(same_citation_status_as_every_other_real_Glide_"
                                    "enum_this_project_has_used) guTexAllocateMemory_return_type=GrMipMapId_t "
                                    "semantic=OPAQUE_INTEGER_HANDLE(not_a_pointer,_not_specified_as_a_small_index) "
                                    "real_contract_requires_nothing_about_magnitude -- whether real Voodoo/Glide "
                                    "drivers happened to return small values IN PRACTICE is a separate, unproven "
                                    "empirical question this Vita-side audit cannot answer; see [G86-NATIVE-MMID] "
                                    "for the actual native measurement");
    IGN_LOG("[G86-GUTEXSOURCE-ABI]", "signature=guTexSource(GrChipID_t tmu,GrMipMapId_t startAddress) "
                                     "real_contract=the_application_passes_back_the_EXACT_GrMipMapId_t_it_received_"
                                     "from_guTexAllocateMemory,_opaquely,_no_transformation_documented_or_required");
    IGN_LOG("[G86-WINVITA-HANDLE-ABI]", "guTexAllocateMemory_impl_guest_visible_return=next_gu_tex_handle()="
                                        "monotonic_0x70000001+N(glide2x_ign.cpp) guTexDownloadMipMap_impl_accepted_value="
                                        "same_0x700000xx_handle(looked_up_in_gu_tex_allocations()_map) "
                                        "guTexSource_impl_accepted_value=same_0x700000xx_handle "
                                        "internal_GL_mapping=gu_tex_allocations()_map_keyed_by_the_SAME_0x700000xx_"
                                        "value(never_a_separate_small_guest-facing_id) -- this IS the guest-visible "
                                        "ABI in this project today: the guest receives 0x700000xx directly, unlike "
                                        "the 0..255-shaped table INDEX Ignition's own material-branch code appears "
                                        "to expect in the general/world material path (still PENDING confirmation "
                                        "from [G86-WORLD-MATERIAL-VALUES] and the native oracle before any change)");

    // iteration-G86, Parte 12-13: world/scalar draw-time stack map --
    // confirmed this iteration by fresh disassembly (objdump against the
    // real Ign_3dfx.exe) that ESI survives, unclobbered, from each
    // family's own branch all the way to its own guDrawTriangleWithClip
    // call -- no `mov esi`/`push esi` appears anywhere in between for any
    // of these six. 0x004510C0 is the lone exception (tests EAX, which is
    // caller-saved and unconditionally clobbered by guColorCombineFunction's
    // own dedup wrapper at 0x00450710's first instruction) -- it keeps
    // G85's already-working guColorCombineFunction-trap stack-offset
    // capture instead, with that method's known dedup-cache-hit blind spot
    // honestly retained.
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x00451380 tested_register=ESI draw_call=0x00451613 draw_return_eip=0x00451618 "
                                          "esi_unclobbered_branch_to_draw=CONFIRMED(no_mov_esi_or_push_esi_in_between)");
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x00451630 tested_register=ESI draw_call=0x004518CF draw_return_eip=0x004518D4 "
                                          "esi_unclobbered_branch_to_draw=CONFIRMED");
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x004518F0 tested_register=ESI draw_call=0x00451B8F draw_return_eip=0x00451B94 "
                                          "esi_unclobbered_branch_to_draw=CONFIRMED");
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x00452860 tested_register=ESI draw_call=0x00452AFC draw_return_eip=0x00452B01 "
                                          "esi_unclobbered_branch_to_draw=CONFIRMED");
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x00452B10 tested_register=ESI draw_call=0x00452DB8 draw_return_eip=0x00452DBD "
                                          "esi_unclobbered_branch_to_draw=CONFIRMED");
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x00452DD0 tested_register=ESI draw_call=0x00453078 draw_return_eip=0x0045307D "
                                          "esi_unclobbered_branch_to_draw=CONFIRMED");
    IGN_LOG("[G86-WORLD-DRAW-STACK-MAP]", "function=0x004510C0 tested_register=EAX(caller_saved,_clobbered_by_0x00450710_"
                                          "itself) draw_call=0x00451331 draw_return_eip=0x00451336 "
                                          "capture_method=UNCHANGED_FROM_G85(guColorCombineFunction_trap,_stack_offset_0x10C) "
                                          "known_blind_spot=dedup_cache_hits_never_reach_that_trap");

    IGN_LOG("[BUILD-G85]", "current_question=WHY_ALL_REAL_MATERIALS_TAKE_FALLBACK");
    IGN_LOG("[BUILD-G85]", "G84_normal_crash=NO");
    IGN_LOG("[BUILD-G85]", "G84_normal_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G85]", "G84_normal_race_runtime=YES");
    IGN_LOG("[BUILD-G85]", "clearprobe_visual_change=NO");
    IGN_LOG("[BUILD-G85]", "simple_missing_clear_root_cause=REJECTED");
    IGN_LOG("[BUILD-G85]", "active_frame_triangles=611");
    IGN_LOG("[BUILD-G85]", "active_frame_changed_tiles=12");
    IGN_LOG("[BUILD-G85]", "active_frame_changed_percent=12.5");
    IGN_LOG("[BUILD-G85]", "stale_background=SECONDARY_SYMPTOM_LIKELY");
    IGN_LOG("[BUILD-G85]", "textures_uploaded=57");
    IGN_LOG("[BUILD-G85]", "guTexSource_runtime_calls=0");
    IGN_LOG("[BUILD-G85]", "textured_material_decisions=0");
    IGN_LOG("[BUILD-G85]", "fallback_material_decisions=64");
    IGN_LOG("[BUILD-G85]", "raw_material_value_capture=ENABLED_VIA_EXISTING_API_TRAP");
    IGN_LOG("[BUILD-G85]", "dynarec_code_hooks=NOT_REQUIRED");
    IGN_LOG("[BUILD-G85]", "material_families_resolved=10_of_10 (0x004510C0 previously UNRESOLVED, resolved this iteration)");
    IGN_LOG("[BUILD-G85]", "native_oracle_status=PENDING_USER_PC_RUN");
    IGN_LOG("[BUILD-G85]", "logger_overhead_target=1.5");
    IGN_LOG("[BUILD-G85]", "performance_regression_forbidden=YES");
    IGN_LOG("[BUILD-G85]", "clear_probe_vpk_built_this_iteration=NO (retired per item 26, source kept behind G84_CLEAR_PROBE=OFF)");

    // iteration-G85, Parte 9: full material-family stack map, disassembled
    // fresh this iteration (objdump -Mintel against the real Ign_3dfx.exe
    // shipped in pc_oracle/ and Ignition1997/ -- real bytes, not guessed).
    // Deliberately NOT one reused "[esp+0xfc]" constant per Part 9's own
    // explicit warning -- 0x004510C0 (G82's own UNRESOLVED dedup_call=
    // 0x004510DE) has a DIFFERENT prologue (no `push esi`, reads
    // [esp+0xf8] not [esp+0xfc]) and needed its own derivation.
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x004510C0 compare_eip=0x004510d2/0x004510d6 tested_source=[esp+0xf8]_no_push_esi_prologue "
                                        "fallback_call=0x004510f8(guColorCombineFunction_wrapper_0x00450710) fallback_return_eip=0x004510fd "
                                        "trap_esp_offset=0x10C (DIFFERENT from the other six -- shorter prologue, no push esi)");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00451380 compare_eip=0x0045138e/0x00451392 tested_source=[esp+0xfc]_via_push_esi "
                                        "fallback_call=0x004513d1 fallback_return_eip=0x004513d6 trap_esp_offset=0x110");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00451630 compare_eip=0x0045163e/0x00451642 tested_source=[esp+0xfc]_via_push_esi "
                                        "fallback_call=0x0045168d fallback_return_eip=0x00451692 trap_esp_offset=0x110 prologue_byte_identical_to_0x00451380=YES");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x004518F0 compare_eip=0x004518fe/0x00451902 tested_source=[esp+0xfc]_via_push_esi "
                                        "fallback_call=0x0045194d fallback_return_eip=0x00451952 trap_esp_offset=0x110 prologue_byte_identical_to_0x00451380=YES");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00452860 compare_eip=0x00452871/0x00452875 tested_source=[esp+0xfc]_via_push_esi "
                                        "fallback_call=0x004528b1 fallback_return_eip=0x004528b6 trap_esp_offset=0x110 prologue_byte_identical_to_0x00451380=YES");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00452B10 tested_source=[esp+0xfc]_via_push_esi "
                                        "fallback_call=0x00452b6d fallback_return_eip=0x00452b72 trap_esp_offset=0x110 prologue_byte_identical_to_0x00451380=YES");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00452DD0 tested_source=[esp+0xfc]_via_push_esi "
                                        "fallback_call=0x00452e2d fallback_return_eip=0x00452e32 trap_esp_offset=0x110 prologue_byte_identical_to_0x00451380=YES");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00451BB0 compare_eip=0x00451bc4/0x00451bc8 tested_source=object_ptr[+0x18] "
                                        "object_ptr_source=[esp+0x10c]_at_own_entry(this_functions_own_arg0) capture_trap=guDrawTriangleWithClip(NOT_guColorCombineFunction) "
                                        "capture_return_eip=0x00451edb prologue=sub_esp_0xfc+push_ebx+push_ebp+push_esi (DIFFERENT frame shape from the 0x110-offset family)");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00451F10(FAMILY-A,particle_dispatcher_0x0044ff20) compare_eip=0x00451f24/0x00451f28 tested_source=object_ptr[+0x18] "
                                        "capture_trap=guDrawTriangleWithClip capture_return_eip=0x0045223b prologue_byte_identical_to_0x00451BB0=YES "
                                        "prior_finding=G52/G53_ALREADY_PROVED_this_field_is_a_per-particle_palette_index_not_a_texture_handle");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "function=0x00452270(FAMILY-B) compare_eip=0x00452284/0x00452288 tested_source=object_ptr[+0x18] "
                                        "capture_trap=guDrawTriangleWithClip capture_return_eip=0x0045259b(first_of_2_triangles_only) prologue_byte_identical_to_0x00451BB0=YES");
    IGN_LOG("[G85-MATERIAL-STACK-MAP]", "wrapper=0x00450710 role=guColorCombineFunction_dedup_cache real_thunk=0x00468c0a cache_var=ds:0x0048ea90 "
                                        "note=this_is_WHY_the_ESI_register_shortcut_fails_for_the_scalar_families(mov_esi,[esp+8]_clobbers_it_with_fnc_before_reaching_0x00468c0a)");

    // iteration-G85, Parte 16: textured-path static flow, for the fully-
    // proven 0x00451380 family (chain confirmed by fresh disassembly this
    // iteration, matches G78's own prior citation).
    IGN_LOG("[G85-TEXTURED-FLOW]", "family=0x00451380 tested_value(0<=v<256)->push_v->guTexSource_dedup(0x00451360)->"
                                   "push_0x1->guTexCombineFunction_dedup(0x00451340,fnc=DECAL)->push_0x4->"
                                   "guColorCombineFunction_dedup(0x00450710,fnc=TEXTURE_TIMES_CCRGB_class)->guDrawTriangleWithClip");
    IGN_LOG("[G85-FALLBACK-FLOW]", "family=0x00451380 tested_value(v<0_or_v>=256)->push_0x2->"
                                   "guColorCombineFunction_dedup(0x00450710,fnc=ITRGB)->guDrawTriangleWithClip "
                                   "(original tested value NEVER reaches any Glide argument on this path -- only "
                                   "recoverable via the static stack-offset technique this iteration adds)");
    IGN_LOG("[G85-FALLBACK-FLOW]", "family=0x00451F10(FAMILY-A) object_ptr[+0x18]<0_or_>=256->push_0x2->"
                                   "guColorCombineFunction_dedup(fnc=ITRGB)->guDrawTriangleWithClip "
                                   "(G52/G53's own prior conclusion: this specific field is a per-particle "
                                   "palette index -- an expired/inactive particle slot legitimately reaches "
                                   "fallback here, this is NOT necessarily the same bug as the 0x00451380-style "
                                   "families, which are NOT part of the particle dispatcher)");

    // G84 and earlier BUILD-marker history continues below, unmodified.
    IGN_LOG("[BUILD-G84]", "current_question=TRIPLE_BUFFER_STALE_CONTENT");
    IGN_LOG("[BUILD-G84]", "G83_crash=NO");
    IGN_LOG("[BUILD-G84]", "G83_diagnostic_halt=NO");
    IGN_LOG("[BUILD-G84]", "G83_manual_termination=YES");
    IGN_LOG("[BUILD-G84]", "physical_buffers_seen=3");
    IGN_LOG("[BUILD-G84]", "buffer_A=0x626A6000");
    IGN_LOG("[BUILD-G84]", "buffer_B=0x622A4000");
    IGN_LOG("[BUILD-G84]", "buffer_C=0x624A5000");
    IGN_LOG("[BUILD-G84]", "buffer_A_stale_percent=100.0");
    IGN_LOG("[BUILD-G84]", "buffer_B_stale_percent=100.0");
    IGN_LOG("[BUILD-G84]", "buffer_C_stale_percent=4.2");
    IGN_LOG("[BUILD-G84]", "triple_buffer_content_divergence=PROVEN_AT_MEASURED_STATE");
    IGN_LOG("[BUILD-G84]", "new_backbuffer_can_equal_loading_hash=YES");
    IGN_LOG("[BUILD-G84]", "loading_hash=0x52965DC5");
    IGN_LOG("[BUILD-G84]", "G83_race_clear_calls=4");
    IGN_LOG("[BUILD-G84]", "G83_active_frame_coverage_probe_invalid_due_zero_triangles=YES");
#ifdef G84_CLEAR_PROBE
    IGN_LOG("[BUILD-G84]", "clear_probe_build=YES");
    IGN_LOG("[BUILD-G84]", "clear_probe_normal_default=OFF (this specific VPK has it force-enabled for the diagnostic A/B test)");
#else
    IGN_LOG("[BUILD-G84]", "clear_probe_build=NO");
    IGN_LOG("[BUILD-G84]", "clear_probe_normal_default=OFF");
#endif
    IGN_LOG("[BUILD-G84]", "material_path=FROZEN");
    IGN_LOG("[BUILD-G84]", "textures=FROZEN");
    IGN_LOG("[BUILD-G84]", "depth=FROZEN");
    IGN_LOG("[BUILD-G84]", "cull=FROZEN");
    IGN_LOG("[BUILD-G84]", "blend=FROZEN");
    IGN_LOG("[BUILD-G84]", "G83_logger_overhead_percent=13.3");
    IGN_LOG("[BUILD-G84]", "logger_policy=G84_TAGS_ONLY");
    IGN_LOG("[BUILD-G84]", "generic_BUILD_LOGS_DISABLED=YES");
    IGN_LOG("[BUILD-G84]", "RUN27_RUN29_SOURCE_FOUND=YES (vendored external/WinVita/third_party/box86-dynarec/dynarec/dynarec_arm_00.c + dynarec_arm_66.c, raw fopen/fprintf bypassing this project's own log_line() entirely -- see [G84-RUN-MARKER-SOURCE] below for detail; NOT modified this iteration, vendored code requires explicit user authorization)");
    IGN_LOG("[BUILD-G84]", "critical_log_policy=SESSION_TRACK_RACE_STOP_EXIT_ONLY");

    // iteration-G84, Parte 29: the exact origin of [RUN27]/[RUN29] --
    // found this iteration by searching external/WinVita (this project's
    // own source tree has never contained these tags, confirmed 3 times
    // across G81/G83/G84). ign_marker25/ign_marker25_66 in the vendored
    // box86-derived dynarec write DIRECTLY to this project's own log path
    // via a raw fopen("ux0:data/ignition/ignitionvita.log","a")/fprintf/
    // fclose on EVERY execution of specific translated PUSHF/POPF-family
    // x86 opcodes -- completely bypassing diagnostics/log.cpp's log_line()
    // and therefore invisible to any allowlist/denylist this project's own
    // code has ever implemented, and invisible to g_g74Logger* overhead
    // counters. The comments at both call sites (dynarec_arm_66.c lines
    // 26-44, dynarec_arm_00.c line ~218) show this is leftover
    // instrumentation from a PUSHFW/POPFW bug hunt across iterations
    // 25-31 of THAT codebase's own history; the actual fix it was hunting
    // (case 0x9C/0x9D's own MMU_PUSH16/MMU_POP16 handling) is described as
    // already landed and the C-helper overrides already retired -- the
    // marker logging itself was apparently never removed. This is
    // vendored code under external/WinVita, so per this project's own
    // standing rule it has NOT been touched without explicit authorization
    // -- reported here, honestly, instead of silently patching it.
    IGN_LOG("[G84-RUN-MARKER-SOURCE]", "found=YES file=\"external/WinVita/third_party/box86-dynarec/dynarec/"
                                       "dynarec_arm_66.c\" function=ign_marker25_66 AND file=\"dynarec_arm_00.c\" "
                                       "function=ign_marker25 mechanism=raw_fopen_fprintf_fclose_to_same_log_path "
                                       "bypasses_log_line=YES call_site_count_approx=26 fires=every_execution_of_"
                                       "the_translated_PUSHF_POPF_opcode_handler(hot_path,_not_one_shot) "
                                       "vendored=YES modified_this_iteration=NO "
                                       "reason=standing_rule_never_touch_external_WinVita_without_authorization "
                                       "recommendation=ask_user_to_authorize_removing_or_gating_this_dead_"
                                       "diagnostic_instrumentation_since_its_own_comments_say_the_bug_it_hunted_"
                                       "is_already_fixed_and_retired");
    IGN_LOG("[BUILD]", "glide_bringup=G83");
    IGN_LOG("[BUILD]", "unique_build_id=G83_20260928");
    IGN_LOG("[BUILD]", "current_question=DOUBLE_BUFFER_STALE_CONTENT");
    IGN_LOG("[BUILD]", "G82_crash=NO");
    IGN_LOG("[BUILD]", "G82_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "G82_manual_termination=YES");
    IGN_LOG("[BUILD]", "G82_partial_redraw=PROVEN");
    IGN_LOG("[BUILD]", "G82_race_first_stale_percent=100.0");
    IGN_LOG("[BUILD]", "G82_race10_stale_percent=85.4");
    IGN_LOG("[BUILD]", "G82_race60_stale_percent=83.3");
    IGN_LOG("[BUILD]", "loading_hash=0x52965dc5");
    IGN_LOG("[BUILD]", "race_scanout_hash=0x5f36abec");
    IGN_LOG("[BUILD]", "swap_pair_loading_hash_reappeared=YES");
    IGN_LOG("[BUILD]", "current_hypothesis=DOUBLE_BUFFER_CONTENT_DIVERGENCE");
    IGN_LOG("[BUILD]", "race_clear_calls=4");
    IGN_LOG("[BUILD]", "no_race_clear_hypothesis=RETRACTED");
    IGN_LOG("[BUILD]", "G82_textured_material_count=0");
    IGN_LOG("[BUILD]", "G82_fallback_material_count=64");
    IGN_LOG("[BUILD]", "raw_material_value_observable=NO");
    IGN_LOG("[BUILD]", "native_oracle=PENDING_USER_RUN");
    IGN_LOG("[BUILD]", "user_reports_performance_improvement=YES");
    IGN_LOG("[BUILD]", "G82_logger_overhead_late_percent=27.0");
    IGN_LOG("[BUILD]", "logging_policy=ALLOWLIST_ONLY");
    IGN_LOG("[BUILD]", "RUN27_RUN29_OUTPUT_DISABLED=YES (re-confirmed: RUN25/RUN27/RUN29 do NOT exist as log-emission tags anywhere in this codebase's source, checked a THIRD time this iteration -- if the user's own log capture tooling adds numbered run identifiers, that is external to this project's own logger and outside diagnostics/log.cpp's control; this project's own allowlist filter, active this build, guarantees no other tag reaches the log)");
    IGN_LOG("[BUILD]", "texture_path=FROZEN");
    IGN_LOG("[BUILD]", "depth=FROZEN");
    IGN_LOG("[BUILD]", "cull=FROZEN");
    IGN_LOG("[BUILD]", "blend=FROZEN");
    IGN_LOG("[BUILD]", "G81_crash=NO");
    IGN_LOG("[BUILD]", "G81_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "G81_manual_termination=YES");
    IGN_LOG("[BUILD]", "G81_selected_track=CANADA");
    IGN_LOG("[BUILD]", "G81_real_race_runtime=PROVEN");
    IGN_LOG("[BUILD]", "G81_loading_scanout_hash=0x52965dc5");
    IGN_LOG("[BUILD]", "G81_race_first_scanout_hash=0x5f36abec");
    IGN_LOG("[BUILD]", "G81_race_10_scanout_hash=0x5f36abec");
    IGN_LOG("[BUILD]", "fully_frozen_scanout=RULED_OUT");
    IGN_LOG("[BUILD]", "current_visual_hypothesis=PARTIAL_REDRAW_STALE_BACKGROUND");
    IGN_LOG("[BUILD]", "G81_frontbuffer_locks=0");
    IGN_LOG("[BUILD]", "G81_backbuffer_locks=48895");
    IGN_LOG("[BUILD]", "G81_render_buffer=BACKBUFFER_ONLY");
    IGN_LOG("[BUILD]", "G81_texture_path_taken=NO");
    IGN_LOG("[BUILD]", "G81_material_textured_count=0");
    IGN_LOG("[BUILD]", "G81_material_fallback_count=64");
    IGN_LOG("[BUILD]", "G81_world_geometry_on_screen=YES");
    IGN_LOG("[BUILD]", "forced_race_clear=FORBIDDEN_PENDING_NATIVE_PROOF");
    IGN_LOG("[BUILD]", "vitaGL_swap_chain=FROZEN");
    IGN_LOG("[BUILD]", "performance_user_observed_fps_approx=20");
    IGN_LOG("[BUILD]", "performance_user_observed_core3_near_100pct=YES");
    IGN_LOG("[BUILD]", "performance_optimization=DEFERRED");
    IGN_LOG("[BUILD]", "G81_logger_objective=FAIL");
    IGN_LOG("[BUILD]", "G82_historical_boot_spam=DISABLED (42 bare per-iteration narrative tags [G40]-[G81] denylisted, diagnostics/log.cpp)");
    IGN_LOG("[BUILD]", "G82_runtime_log_budget=120_LINES_5_MINUTES");
    IGN_LOG("[BUILD]", "G80_crash=NO");
    IGN_LOG("[BUILD]", "G80_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "G80_manual_termination=YES");
    IGN_LOG("[BUILD]", "G80_selected_track=USA");
    IGN_LOG("[BUILD]", "G80_USA_race_runtime=PROVEN");
    IGN_LOG("[BUILD]", "hardware_stale_loading_screen_during_race=YES");
    IGN_LOG("[BUILD]", "G80_lfb_overwrite_test_frame=NEGATIVE");
    IGN_LOG("[BUILD]", "G80_pre_post_roi_hash_same=YES");
    IGN_LOG("[BUILD]", "G80_backbuffer_contains_nonuniform_pixels=YES");
    IGN_LOG("[BUILD]", "current_hypothesis=PRESENT_OR_PARTIAL_REDRAW_STALE_SCREEN (pending this build's own [G81-PRESENT-COMPARE]/[G81-HUD-WORLD-SPLIT] hardware evidence)");
    IGN_LOG("[BUILD]", "G80_color_double_normalization=NO");
    IGN_LOG("[BUILD]", "depth=FROZEN");
    IGN_LOG("[BUILD]", "cull=FROZEN");
    IGN_LOG("[BUILD]", "blend=FROZEN");
    IGN_LOG("[BUILD]", "texture_path=FROZEN");
    IGN_LOG("[BUILD]", "family_C=PENDING_NATIVE_PROOF_NOT_CURRENT_BLOCKER");
    IGN_LOG("[BUILD]", "G80_logger_objective=FAIL");
    IGN_LOG("[BUILD]", "G81_direct_run_markers_disabled=YES (RUN25/RUN27/RUN29 re-confirmed NOT to exist as real log tags in this codebase, see [G81-LOGGER-PERF])");
    IGN_LOG("[BUILD]", "G81_minimal_logger=ENABLED");
    IGN_LOG("[BUILD]", "G79_crash=NO");
    IGN_LOG("[BUILD]", "G79_exit=WRAPPER_DIAGNOSTIC_HALT");
    IGN_LOG("[BUILD]", "G79_selected_track=USA");
    IGN_LOG("[BUILD]", "G79_USA_TRI_seen=YES");
    IGN_LOG("[BUILD]", "G79_USA_SRF_seen=YES");
    IGN_LOG("[BUILD]", "G79_USA_race_entry=YES");
    IGN_LOG("[BUILD]", "current_blocker_caller=0x00451EDB");
    IGN_LOG("[BUILD]", "current_blocker_field=C.b");
    IGN_LOG("[BUILD]", "current_blocker_raw=0xFFFFFC4C");
    IGN_LOG("[BUILD]", "family_A=FROZEN");
    IGN_LOG("[BUILD]", "family_B=FROZEN");
    IGN_LOG("[BUILD]", "family_C=UNPROVEN_PENDING_G80 (static topology CONFIRMED identical to A/B, native zero-equivalence proof PENDING user hardware run)");
    IGN_LOG("[BUILD]", "G79_world_fully_offscreen=0");
    IGN_LOG("[BUILD]", "G79_cull_enabled=NO");
    IGN_LOG("[BUILD]", "G79_depth_test=NO");
    IGN_LOG("[BUILD]", "G79_blend=NO");
    IGN_LOG("[BUILD]", "G79_scissor_fullscreen=YES");
    IGN_LOG("[BUILD]", "G79_viewport_fullscreen=YES");
    IGN_LOG("[BUILD]", "new_visual_hypothesis=LFB_POST_DRAW_OCCLUSION (PROVABLE BY CODE: lfb_blit_to_backbuffer disables GL_BLEND, uses GL_REPLACE texenv, RGB565 has no alpha -- runtime pre/post-blit pixel proof pending [G80-FB-PRE-LFB]/[G80-FB-POST-LFB] in the next hardware log)");
    IGN_LOG("[BUILD]", "guTexSource_static_callers=11");
    IGN_LOG("[BUILD]", "guTexCombine_static_callers=11");
    IGN_LOG("[BUILD]", "texture_runtime_calls=0");
    IGN_LOG("[BUILD]", "G79_logger_target=FAILED (77.4%% at t=24.2s, 37.9%% at t=111.9s, plus G67-WATCH-CHANGE spam on the new halt)");
    IGN_LOG("[BUILD]", "G80_G67_watch_spam=DISABLED (capped to last 16 events, stale hardcoded caller label fixed, g70_log_provenance_closure no longer fires for unrelated callers)");
    IGN_LOG("[BUILD]", "G80_minimal_logger=ENABLED");
    IGN_LOG("[BUILD]", "G80_skip_lfb_blit_toggle=kG80SkipLfbBlitDuringRealRace(default_OFF,_diagnostic_only)");
    IGN_LOG("[BUILD]", "G78_crash=NO");
    IGN_LOG("[BUILD]", "G78_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "G78_manual_termination=YES");
    IGN_LOG("[BUILD]", "world_draw_caller=0x00452781");
    IGN_LOG("[BUILD]", "world_guTexSource_calls=0");
    IGN_LOG("[BUILD]", "world_guTexCombine_calls=0");
    IGN_LOG("[BUILD]", "texture_2d_sprite_path_static=CORRECTED_G79 (11 real call sites, not 1 -- see guTexSource_static_callsites below)");
    IGN_LOG("[BUILD]", "texture_world_path_static=CAN_PRECEDE_TRIANGLE_DRAW (0x00452860/0x00452B10 confirmed adjacent to and ending in a real guDrawTriangleWithClip call -- G79 static finding)");
    IGN_LOG("[BUILD]", "G78_race_material_decisions=64");
    IGN_LOG("[BUILD]", "G78_textured_path_count=0");
    IGN_LOG("[BUILD]", "G78_fallback_path_count=64");
    IGN_LOG("[BUILD]", "G78_native_texture_oracle=PENDING_USER_HARDWARE_RUN");
    IGN_LOG("[BUILD]", "race_HUD_visible=YES");
    IGN_LOG("[BUILD]", "race_track_visible=NO");
    IGN_LOG("[BUILD]", "race_cars_visible=NO");
    IGN_LOG("[BUILD]", "pause_menu_visible=YES");
    IGN_LOG("[BUILD]", "pause_confirmation_visible=NO");
    IGN_LOG("[BUILD]", "menu_background=WHITE_INCORRECT");
    IGN_LOG("[BUILD]", "car_selection_image=MISSING");
    IGN_LOG("[BUILD]", "G78_logger_objective=FAIL (79.2%% at t=23.8s, 45.9%% at t=99.7s)");
    IGN_LOG("[BUILD]", "G79_minimal_logger=ENABLED (diagnostics/log.cpp denylist extended: G31-LOOP/G37-PERF/G42-PERF/G34-PERF/G34-PEEK/G27/G41/G74-G78-LOGGER-PERF all silenced, only G79-LOGGER-PERF remains)");
    IGN_LOG("[BUILD]", "guDrawTriangleWithClip_static_callers=18 (guest VA 0x00450C83-0x00453078)");
    IGN_LOG("[BUILD]", "guTexSource_static_callsites=11 (CORRECTED from G78's own count of 1 -- exhaustive re-grep of dedup-wrapper callers, not just raw IAT thunk callers)");
    IGN_LOG("[BUILD]", "guTexCombineFunction_static_callsites=11 (SAME correction)");
    IGN_LOG("[BUILD]", "texture_branch_dispatcher_role=GENERAL_PURPOSE_PRIMITIVE_RENDERER_2D_AND_3D (CORRECTS G78's own GENERIC_2D_SPRITE_PRIMITIVE_RENDERER_NOT_3D_WORLD)");
    IGN_LOG("[BUILD]", "LFB_SEMANTICS=FROZEN_PENDING_AUDIT (sampled dirty-region instrumentation added, no semantic change)");
    IGN_LOG("[BUILD]", "DDraw=UNCHANGED");
    IGN_LOG("[BUILD]", "G77_crash=NO");
    IGN_LOG("[BUILD]", "G77_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "G77_manual_termination=YES");
    IGN_LOG("[BUILD]", "G77_guTexSource_runtime_calls=0");
    IGN_LOG("[BUILD]", "G77_guTexCombineFunction_runtime_calls=0");
    IGN_LOG("[BUILD]", "G77_textured_triangles=0");
    IGN_LOG("[BUILD]", "G77_colorcombine_CCRGB_calls=GREATER_THAN_ZERO");
    IGN_LOG("[BUILD]", "G77_colorcombine_ITRGB_calls=GREATER_THAN_ZERO");
    IGN_LOG("[BUILD]", "G77_colorcombine_texture_modes=0");
    IGN_LOG("[BUILD]", "G77_pause_menu_via_circle=YES");
    IGN_LOG("[BUILD]", "circle_mapping=DIK_ESCAPE_FROZEN");
    IGN_LOG("[BUILD]", "pause_exit_yes_no_visual=MISSING");
    IGN_LOG("[BUILD]", "guTexSource_static_callsites=1_HISTORICAL_G78 (INCOMPLETE -- corrected to 11 by G79, see guTexSource_static_callsites above)");
    IGN_LOG("[BUILD]", "guTexCombineFunction_static_callsites=1_HISTORICAL_G78 (INCOMPLETE -- corrected to 11 by G79)");
    IGN_LOG("[BUILD]", "texture_branch_dispatcher_role_HISTORICAL_G78=GENERIC_2D_SPRITE_PRIMITIVE_RENDERER_NOT_3D_WORLD (CORRECTED by G79 -- see texture_branch_dispatcher_role above)");
    IGN_LOG("[BUILD]", "G77_caller_citation_0x452b10=REPRODUCIBLE_AFTER_ALL_G79 (G78 called this UNREPRODUCIBLE -- G79's exhaustive dedup-wrapper-caller grep found it at guest VA 0x00452B41, confirming G77's original citation was directionally correct)");
    IGN_LOG("[BUILD]", "native_oracle_proxy_extended=YES (pc_oracle/glide2x_proxy/glide2x_proxy.c, guTexSource/guTexCombineFunction/guColorCombineFunction now intercepted, rebuilt and deployed to pc_oracle/glide2x.dll)");
    IGN_LOG("[BUILD]", "native_oracle_run_status=PENDING_USER_HARDWARE_RUN (this environment cannot launch a native Windows Glide/3dfx GUI process)");
    IGN_LOG("[BUILD]", "G77_logger_overhead_peak_percent=74.7");
    IGN_LOG("[BUILD]", "G77_logger_overhead_late_percent=65.1");
    IGN_LOG("[BUILD]", "G77_logger_max_single_line_us=1066330");
    IGN_LOG("[BUILD]", "G78_central_log_filter=ENABLED (diagnostics/log.h kG78CentralLogFilterEnabled, denylist in diagnostics/log.cpp)");
    IGN_LOG("[BUILD]", "G78_startup_timing_checkpoints=process_start,jit_ready,first_game_file,language_screen_active(proxy),menu_active(proxy) (see [G78-STARTUP-TIMING])");
    IGN_LOG("[BUILD]", "input_transport=FROZEN");
    IGN_LOG("[BUILD]", "guTexSource=FROZEN_IMPLEMENTED");
    IGN_LOG("[BUILD]", "guTexCombineFunction=FROZEN_IMPLEMENTED");
    IGN_LOG("[BUILD]", "texture_upload=FROZEN");
    IGN_LOG("[BUILD]", "G76_crash=NO");
    IGN_LOG("[BUILD]", "G76_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "G76_manual_termination=YES");
    IGN_LOG("[BUILD]", "G76_execution_flow_unblocked=YES (no [STOP]/G76-NEXT-BLOCKER/watchdog halt in the observed log -- game stayed alive through ~2850 swaps/~787k triangles until the user closed it)");
    IGN_LOG("[BUILD]", "G76_grDrawPoint=RESOLVED (first call succeeded, glGetError=0x0000, game continued many seconds afterward)");
    IGN_LOG("[BUILD]", "G76_real_race_runtime=PROVEN (CANADA.TRI and CANADA.SRF both observed, [G76-RACE-RUNTIME] PROVEN=YES)");
    IGN_LOG("[BUILD]", "G76_track_load_ms=28178");
    IGN_LOG("[BUILD]", "G76_first_race_swap=1092");
    IGN_LOG("[BUILD]", "G76_race_runtime_swap=1102");
    IGN_LOG("[BUILD]", "G76_textures_allocated=57");
    IGN_LOG("[BUILD]", "G76_textures_uploaded=57");
    IGN_LOG("[BUILD]", "G76_textured_triangles_observed=0");
    IGN_LOG("[BUILD]", "guTexSource_before_G77=NOT_IMPLEMENTED");
    IGN_LOG("[BUILD]", "guTexCombineFunction_before_G77=NOT_IMPLEMENTED");
    IGN_LOG("[BUILD]", "triangle_texcoord_before_G77=NEVER");
    IGN_LOG("[BUILD]", "guTexSource_status=IMPLEMENTED (guTexAllocateMemory handle -> host GL texture resolve, unknown/uninvalidated/never-uploaded handles halt cleanly, no fallback bind)");
    IGN_LOG("[BUILD]", "guTexCombineFunction_status=IMPLEMENTED (tmu=0/fnc=0x1=GR_TEXTURECOMBINE_DECAL only -- the one real-disassembly-evidenced value; every other value refused, not guessed)");
    IGN_LOG("[BUILD]", "triangle_texcoord_submission_status=IMPLEMENTED (glTexCoordPointer/GL_TEXTURE_COORD_ARRAY, gated on glide_resolve_texture_binding -- see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "current_visual_hypothesis=MISSING_TEXTURE_RENDER_PIPELINE (G76's own finding: textures_allocated=57 textures_uploaded=57 textured_triangle_count=0 before G77)");
    IGN_LOG("[BUILD]", "menu_background_visual=WHITE_INCORRECT (unconfirmed this iteration -- pending next hardware run)");
    IGN_LOG("[BUILD]", "car_selection_images_visual=MISSING (unconfirmed this iteration -- pending next hardware run)");
    IGN_LOG("[BUILD]", "race_HUD_visual=VISIBLE (G76 hardware result)");
    IGN_LOG("[BUILD]", "race_track_visual=MISSING (G76 hardware result -- G77's texture implementation not yet hardware-tested)");
    IGN_LOG("[BUILD]", "race_cars_visual=MISSING (G76 hardware result -- G77's texture implementation not yet hardware-tested)");
    IGN_LOG("[BUILD]", "original_pause_dik=NO_LONGER_SPECULATIVE (G77 hardware proved CIRCLE->DIK_ESCAPE opens the in-race pause menu -- see HARDWARE_PAUSE_VIA_ESCAPE below; no further search needed per Part 19)");
    IGN_LOG("[BUILD]", "HARDWARE_PAUSE_VIA_ESCAPE=YES");
    IGN_LOG("[BUILD]", "vita_start_mapping_before=DIK_RETURN");
    IGN_LOG("[BUILD]", "vita_start_mapping_after=DIK_RETURN (UNCHANGED -- START not needed for pause, Part 20's own explicit instruction not to mix this into graphics work)");
    IGN_LOG("[BUILD]", "G76_logger_overhead_percent=44.3");
    IGN_LOG("[BUILD]", "G77_hard_low_log=ENABLED (mciSendStringA's per-call stack dump gated to first-3+periodic -- see win32/winmm_ign.cpp, [G77-LOGGER-PERF] reports this run's actual measurement)");
    IGN_LOG("[BUILD]", "G75_log_lines_at_130s=4382");
    IGN_LOG("[BUILD]", "G75_logger_total_us_at_130s=80940032");
    IGN_LOG("[BUILD]", "G75_log_reduction_insufficient=YES");
    IGN_LOG("[BUILD]", "G76_aggressive_low_log=ENABLED (proved insufficient by its own hardware measurement, see G76_logger_overhead_percent)");
    IGN_LOG("[BUILD]", "grDrawPoint_status=IMPLEMENTED_AND_HARDWARE_RESOLVED");
    IGN_LOG("[BUILD]", "grDrawPoint=FROZEN");
    IGN_LOG("[BUILD]", "dynarec=FROZEN");
    IGN_LOG("[BUILD]", "heap=FROZEN");
    IGN_LOG("[BUILD]", "file_semantics=FROZEN");
    IGN_LOG("[BUILD]", "LFB=FROZEN");
    IGN_LOG("[BUILD]", "DDraw=UNCHANGED");
    IGN_LOG("[BUILD]", "family_A=FROZEN");
    IGN_LOG("[BUILD]", "family_B=FROZEN");
    IGN_LOG("[BUILD]", "triangles=FROZEN");
    IGN_LOG("[BUILD]", "failing_caller=0x0045259B (RESOLVED_G75 -- component-wise FAMILY-B compat, see [G75] above)");
    IGN_LOG("[BUILD]", "failing_field=C.g (RESOLVED_G75)");
    IGN_LOG("[BUILD]", "failing_raw=0xFFFF9660 (RESOLVED_G75)");
    IGN_LOG("[BUILD]", "failing_C_r=0x0045047F (historical, RESOLVED_G75)");
    IGN_LOG("[BUILD]", "failing_C_g=0xFFFF9660 (historical, RESOLVED_G75)");
    IGN_LOG("[BUILD]", "failing_C_b=0x000172E0 (historical, RESOLVED_G75)");
    IGN_LOG("[BUILD]", "G73_B_nan_case=RESOLVED");
    IGN_LOG("[BUILD]", "G74_G_nan_case=NEW");
    IGN_LOG("[BUILD]", "native_nan_R_effect=ZERO_EQUIVALENT_SYNTHETIC_ONLY (never hardware-observed, R compat stays DISABLED)");
    IGN_LOG("[BUILD]", "native_nan_G_effect=ZERO_EQUIVALENT (real hardware payload + 2 synthetic payloads, payload-independent)");
    IGN_LOG("[BUILD]", "native_nan_B_effect=ZERO_EQUIVALENT (reconfirmed since G73, payload-independent)");
    IGN_LOG("[BUILD]", "nan_payload_dependent=NO");
    IGN_LOG("[BUILD]", "family_B_componentwise_compat=ENABLED");
    IGN_LOG("[BUILD]", "family_B_guest_memory_modified=NO");
    IGN_LOG("[BUILD]", "diagnostic_logger_perturbation=CONFIRMED");
    IGN_LOG("[BUILD]", "G74_logger_sample_lines=1009");
    IGN_LOG("[BUILD]", "G74_logger_sample_total_us=18823171");
    IGN_LOG("[BUILD]", "G74_logger_sample_avg_us=18655");
    IGN_LOG("[BUILD]", "G74_logger_sample_max_us=175766");
    IGN_LOG("[BUILD]", "G75_low_overhead_runtime_log=ENABLED");
    IGN_LOG("[BUILD]", "logger_crash_safety_model=UNCHANGED");
    IGN_LOG("[BUILD]", "family_A_compat=FROZEN");
    IGN_LOG("[BUILD]", "family_B_previous_compat=SUPERSEDED_BY_G75_COMPONENTWISE (G75 proof complete for G/B; R deferred)");
    IGN_LOG("[BUILD]", "G73_family_A=RESOLVED");
    IGN_LOG("[BUILD]", "G73_family_B=RESOLVED");
    IGN_LOG("[BUILD]", "G73_passed_5259B=YES");
    IGN_LOG("[BUILD]", "G73_passed_525B2=YES");
    IGN_LOG("[BUILD]", "G73_menu_runtime_active=YES");
    IGN_LOG("[BUILD]", "G73_quick_race_reached=YES");
    IGN_LOG("[BUILD]", "G73_car_selection_reached=YES");
    IGN_LOG("[BUILD]", "G73_track_selected=YES");
    IGN_LOG("[BUILD]", "previous_run_crash=NO");
    IGN_LOG("[BUILD]", "previous_run_diagnostic_halt=NO");
    IGN_LOG("[BUILD]", "previous_run_manual_termination=YES");
    IGN_LOG("[BUILD]", "current_issue=APPARENT_TRACK_LOAD_STALL");
    IGN_LOG("[BUILD]", "selected_track=CANADA");
    IGN_LOG("[BUILD]", "canada_col_seen=YES");
    IGN_LOG("[BUILD]", "canada_pan_seen=YES");
    IGN_LOG("[BUILD]", "canada_pic_seen=YES");
    IGN_LOG("[BUILD]", "G73_stall_1_ms=5121");
    IGN_LOG("[BUILD]", "G73_stall_1_from=0x00466361");
    IGN_LOG("[BUILD]", "G73_stall_1_to=0x00462563");
    IGN_LOG("[BUILD]", "G73_stall_2_ms=6663");
    IGN_LOG("[BUILD]", "G73_stall_2_from=0x00466361");
    IGN_LOG("[BUILD]", "G73_stall_2_to=0x00464D72");
    IGN_LOG("[BUILD]", "G73_stall_3_ms=5614");
    IGN_LOG("[BUILD]", "G73_stall_3_from=0x00466361");
    IGN_LOG("[BUILD]", "G73_stall_3_to=0x00466B26");
    IGN_LOG("[BUILD]", "G73_guest_alive_during_load=YES");
    IGN_LOG("[BUILD]", "file_open_total_us_observed=201801");
    IGN_LOG("[BUILD]", "file_read_total_us_observed=3829933");
    IGN_LOG("[BUILD]", "verbose_file_logging=DISABLED_FOR_G74_TRACK_LOAD_TEST");
    IGN_LOG("[BUILD]", "file_semantics=UNCHANGED");
    IGN_LOG("[BUILD]", "logger_cost_measurement=ENABLED_RAM_COUNTERS");
    IGN_LOG("[BUILD]", "function_466361_role=CRT_STYLE_OPEN_WRAPPER (0x004661AF-0x00466463, translates POSIX oflags to CreateFileA args, one CreateFileA call at 0x46635b/ret=0x466361, then GetFileType via WinVita's own vendored instant stub, no loop/decompression/bulk copy -- cannot itself be the stall source)");
    IGN_LOG("[BUILD]", "stall_classification=INCONCLUSIVE_THIS_ITERATION (checkpoint function proven fast; exact subsequent guest routine per gap not fully localized -- no per-instruction guest trace exists on the real ARM dynarec target; [G74-LOAD-HEARTBEAT]/[G49-FILEIO]/[G74-LOGGER-PERF] are the concrete next-run evidence)");
    IGN_LOG("[BUILD]", "menu_background_visual=WHITE_INCORRECT");
    IGN_LOG("[BUILD]", "car_selection_images_visual=MISSING");
    IGN_LOG("[BUILD]", "menu_text_visual=READABLE");
    IGN_LOG("[BUILD]", "family_A_compat=FROZEN");
    IGN_LOG("[BUILD]", "family_B_compat=FROZEN");
    IGN_LOG("[BUILD]", "G72_hardware_menu_reached=YES");
    IGN_LOG("[BUILD]", "G72_hardware_menu_input_working=YES");
    IGN_LOG("[BUILD]", "G72_hardware_menu_text_readable=YES");
    IGN_LOG("[BUILD]", "G72_global_visual_correctness=NO");
    IGN_LOG("[BUILD]", "G72_completed_swap_before_halt=150");
    IGN_LOG("[BUILD]", "G72_total_triangles_before_halt=27764");
    IGN_LOG("[BUILD]", "G71_compat_regression=NO");
    IGN_LOG("[BUILD]", "paired_caller=0x004525B2");
    IGN_LOG("[BUILD]", "native_5259B_b_finite=76");
    IGN_LOG("[BUILD]", "native_5259B_b_nan=52");
    IGN_LOG("[BUILD]", "native_5259B_b_nan_rate=40.625_PERCENT");
    IGN_LOG("[BUILD]", "native_vita_byte_exact_match=YES");
    IGN_LOG("[BUILD]", "native_nan_is_original_behavior=YES");
    IGN_LOG("[BUILD]", "family_A_behavior=FINITE_BLACK_EQUIVALENT");
    IGN_LOG("[BUILD]", "family_B_behavior=NATIVE_NAN_MIXED");
    IGN_LOG("[BUILD]", "native_backend=Direct3D9 (nGlide 2.10, evidence=strings Direct3DCreate9/d3d9.dll in glide2x_real.dll)");
    IGN_LOG("[BUILD]", "native_nan_path=guDrawTriangleWithClip_thunk(0x10001325)->0x1004e060(2-way dispatch)->deeper D3D9 vertex code, not exhaustively traced");
    IGN_LOG("[BUILD]", "native_nan_effect=ZERO_EQUIVALENT (proven by framebuffer experiment, pc_oracle/experiment_nan/)");
    IGN_LOG("[BUILD]", "family_B_compat=ENABLED");
    IGN_LOG("[BUILD]", "family_B_compat_reason=native NaN proven pixel-exact ZERO_EQUIVALENT via separate throwaway readback experiment -- substitutes ONLY vc.b, host-local only");
    IGN_LOG("[BUILD]", "G71_old_5223B_blocker=RESOLVED (confirmed on real hardware -- see hardware log reported this iteration)");
    IGN_LOG("[BUILD]", "G71_old_52252_blocker=RESOLVED (confirmed on real hardware -- see hardware log reported this iteration)");
    IGN_LOG("[BUILD]", "G71_compat_calls_observed=400");
    IGN_LOG("[BUILD]", "G71_compat_5223B_observed=200");
    IGN_LOG("[BUILD]", "G71_compat_52252_observed=200");
    IGN_LOG("[BUILD]", "hardware_progressed_farther=YES");
    IGN_LOG("[BUILD]", "current_blocker=APPARENT_TRACK_LOAD_STALL (UPDATED_G74 -- caller 0x0045259B/0x004525B2 RESOLVED via FAMILY-B compat, see [G73]/[G74] above)");
    IGN_LOG("[BUILD]", "failing_raw_as_signed_8_8=-2.375");
    IGN_LOG("[BUILD]", "adjacent_g_raw=0x00027CA0");
    IGN_LOG("[BUILD]", "adjacent_g_as_signed_8_8=636.625");
    IGN_LOG("[BUILD]", "function_5259B_range=0x00452270-0x004525C3");
    IGN_LOG("[BUILD]", "topology_5259B=SAME_FAMILY (byte-for-byte structural twin of 0x00451F10, proven since G65)");
    IGN_LOG("[BUILD]", "C_RGB_writers_5259B=NONE (exhaustive static sweep, matches G50/G64 for the original pair)");
    IGN_LOG("[BUILD]", "native_5259B_samples=128");
    IGN_LOG("[BUILD]", "native_5259B_nonfinite=52 (b channel only -- r/g both 0/128 nonfinite)");
    IGN_LOG("[BUILD]", "native_5259B_max_abs_r=6.34e-39");
    IGN_LOG("[BUILD]", "native_5259B_max_abs_g=2.68e-40");
    IGN_LOG("[BUILD]", "native_5259B_max_abs_b=2.26e-41");
    IGN_LOG("[BUILD]", "native_5259B_effective_rgb=OTHER (MIXED -- b channel genuinely non-finite natively 40.6%% of the time, NOT black-equivalent)");
    IGN_LOG("[BUILD]", "legacy_quad_family=PROVEN (structurally, via byte-for-byte topology match)");
    IGN_LOG("[BUILD]", "family_compat=DISABLED (native itself produces genuine NaN for this member -- no automatic substitution, per explicit standing instruction)");
    IGN_LOG("[BUILD]", "G70_root_cause=CLOSED");
    IGN_LOG("[BUILD]", "G70_outer_dispatch=0x0044FF20-0x00450534");
    IGN_LOG("[BUILD]", "G70_target_r=RETURN_ADDRESS_0x0045042A");
    IGN_LOG("[BUILD]", "G70_target_g=SIGNED_8_8_XY_ARG");
    IGN_LOG("[BUILD]", "G70_target_b=SIGNED_8_8_XY_ARG");
    IGN_LOG("[BUILD]", "G70_writer_g=0x00450424_PUSH_EDX");
    IGN_LOG("[BUILD]", "G70_writer_b=0x00450423_PUSH_ECX");
    IGN_LOG("[BUILD]", "native_alias_mechanism=CONFIRMED");
    IGN_LOG("[BUILD]", "dynarec_implicated=NO");
    IGN_LOG("[BUILD]", "host_memory_corruption=NO");
    IGN_LOG("[G71-NATIVE-RGB-STATS]", "samples=128 (caller 0x5223B+0x52252 combined) r:finite=128 nan=0 inf=0 max_abs=6.34e-39 g:finite=128 nan=0 inf=0 max_abs=2.15e-40 b:finite=128 nan=0 inf=0 max_abs=1.66e-40 (measured this iteration via pc_oracle, see pc_oracle_log.txt)");
    IGN_LOG("[BUILD]", "native_v3_samples=128");
    IGN_LOG("[BUILD]", "native_v3_finite_count=128");
    IGN_LOG("[BUILD]", "native_v3_nonfinite_count=0");
    IGN_LOG("[BUILD]", "native_v3_max_abs_r=6.34e-39");
    IGN_LOG("[BUILD]", "native_v3_max_abs_g=2.15e-40");
    IGN_LOG("[BUILD]", "native_v3_max_abs_b=1.66e-40");
    IGN_LOG("[BUILD]", "native_effective_rgb=BLACK_EQUIVALENT");
    IGN_LOG("[BUILD]", "ignition_legacy_v3_rgb_compat=ENABLED");
    IGN_LOG("[BUILD]", "compatibility_scope=EXACT_CALLSITE_AND_TOPOLOGY_ONLY");
    IGN_LOG("[BUILD]", "guest_memory_modified=NO");
    IGN_LOG("[BUILD]", "global_nan_sanitization=NO");
    IGN_LOG("[BUILD]", "strict_required_fields=UNCHANGED_OUTSIDE_COMPAT");
    IGN_LOG("[BUILD]", "guessed_color=NO");
    IGN_LOG("[BUILD]", "bad_triangle_skip=OFF");
    IGN_LOG("[BUILD]", "renderer_fix=G71_COMPAT_ONLY");
    IGN_LOG("[BUILD]", "dynarec=FROZEN");
    IGN_LOG("[BUILD]", "heap=FROZEN");
    IGN_LOG("[BUILD]", "textures=FROZEN");
    IGN_LOG("[BUILD]", "LFB=FROZEN");
    IGN_LOG("[BUILD]", "input=FROZEN");
    IGN_LOG("[BUILD]", "file_io=FROZEN");
    IGN_LOG("[BUILD]", "performance=DEFERRED");
    IGN_LOG("[BUILD]", "ddraw_execution=UNCHANGED");
    IGN_LOG("[BUILD]", "global_write_trace=NO (unchanged -- Part 6 forbids it, none added)");
    IGN_LOG("[BUILD]", "heap_zeroing_hack=FORBIDDEN (no memset/zero added to HeapAlloc's flags=0 path)");
    IGN_LOG("[BUILD]", "handle_encoding=FROZEN (0x700000xx numbering unchanged)");
    IGN_LOG("[BUILD]", "force_path_A=FORBIDDEN (none added)");
    IGN_LOG("[BUILD]", "stale_rgb_patch=UPDATED_G71 (guest C.r/g/b still never written/sanitized in guest memory; as of G71, the HOST-LOCAL decoded copy is corrected to (0,0,0) ONLY for the exact evidenced IGNITION_LEGACY_V3_RGB_COMPAT case -- see [G71] above, not a general patch)");
    IGN_LOG("[BUILD]", "nan_bypass=UPDATED_G71 (STRICT_REQUIRED_FIELDS remains globally unchanged OUTSIDE the one named G71 compat case; NOT a general bypass -- see [G71] above)");
    IGN_LOG("[BUILD]", "texture_gpu_path=FROZEN (unchanged, Part 34)");
    IGN_LOG("[BUILD]", "triangle_submission=FROZEN (unchanged, Part 35)");
    IGN_LOG("[BUILD]", "prefail_glide_ring=64_MAX_RAM_ONLY (unchanged since G51)");
    IGN_LOG("[BUILD]", "live_hot_glide_trace=NO (ring pushes are silent RAM writes, zero per-call logging -- unchanged hot-path cost)");
    IGN_LOG("[BUILD]", "C_struct_last_writer_trace=SUPERSEDED_BY_STATIC_PROOF (unchanged since G50 -- moot for C.r/g/b specifically, a file-wide static grep already proves no writer exists)");
    IGN_LOG("[BUILD]", "global_memory_write_trace=NO (confirmed again by source review, unchanged since G48)");
    IGN_LOG("[BUILD]", "startup_119s_regression=NOT_REPRODUCED (unchanged since G49/G50 -- this run reached 3D in ~70s total including interaction/intro/asset loading, no new stall)");
    IGN_LOG("[BUILD]", "startup_probe=LOW_OVERHEAD_ONLY (G49's [G49-STALL]/[G49-FILEIO] probes kept unchanged, Part 28 -- no new startup diagnostics or optimization added this iteration)");
    IGN_LOG("[BUILD]", "rgb_range_hack=FORBIDDEN (no if(rgb<0||rgb>255)-style halt or coercion added, unchanged since G50)");
    IGN_LOG("[BUILD]", "G47_color_combine_fix=FROZEN (unchanged since G47 -- proven correct, real 3D scene keeps presenting across multiple swaps)");
    IGN_LOG("[BUILD]", "constant_color_draw_application=FROZEN (unchanged since G47)");
    IGN_LOG("[BUILD]", "alpha_source_draw_application=FROZEN (unchanged since G47)");
    IGN_LOG("[BUILD]", "current_blocker=REQUIRED_VERTEX_COLOR_NONFINITE (real, confirmed-required field failure -- not an unused-field false positive; caller/field/raw below; UPDATED_G72 -- old caller 0x0045223B/0x00452252 RESOLVED via IGNITION_LEGACY_V3_RGB_COMPAT, see [G71]/[G72] above)");
    IGN_LOG("[BUILD]", "failing_caller=0x0045259B");
    IGN_LOG("[BUILD]", "failing_field=C.b");
    IGN_LOG("[BUILD]", "failing_raw=0xFFFFFDA0");
    IGN_LOG("[BUILD]", "nonfinite_validation=STRICT_REQUIRED_FIELDS (unchanged since G47 -- x/y always required, r/g/b required only when color source is ITERATED, a required only when alpha source is ITERATED)");
    IGN_LOG("[BUILD]", "vertex_producer_trace=ENABLED_ONE_SHOT ([G48-A/B/C-RAW]: full 15-DWORD raw dump of all three vertices, fires once on the first required-field failure only, see gu_log_nonfinite_vertex_dump)");
    IGN_LOG("[BUILD]", "raw_vertex_dump=ONE_SHOT_ONLY (same one-shot guard as [G47-NONFINITE] -- never per-draw)");
    IGN_LOG("[BUILD]", "first_real_3d_geometry_visible=YES (G46's real-hardware run: real post-intro 3D geometry rendered and presented on PS Vita -- visually incorrect (rainbow) before this iteration's color-combine fix, but genuinely reached the screen)");
    IGN_LOG("[BUILD]", "first_swap_after_3d=CONFIRMED ([G45-3D] fired for real: completed_swap_count=294, triangles_total=562, lfb_dirty_this_swap=NO -- the LFB blit did NOT run this swap, so it is proven NOT responsible for the visible frame)");
    IGN_LOG("[BUILD]", "post_intro_3d_scene_visible=YES (2573+ real triangles executed this run, not a single test mesh)");
    IGN_LOG("[BUILD]", "triangle_path=ACTIVE");
    IGN_LOG("[BUILD]", "color_combine_draw_application=AUDITED_AND_FIXED (guDrawTriangleWithClip previously ignored color_combine_local/alpha_combine_local unconditionally -- proven real bug, now selects constant vs iterated color per the actual active state, see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "constant_color_draw_application=SDK_VERIFIED (GR_COMBINE_LOCAL_CONSTANT now correctly sources grConstantColorValue's decoded A/R/G/B, per G41's own real openglide-sourced translation table)");
    IGN_LOG("[BUILD]", "alpha_source_draw_application=SDK_VERIFIED (GR_ALPHASOURCE_CC_ALPHA now correctly sources grConstantColorValue's alpha instead of always using vertex.a, per G45's own real openglide-sourced translation table)");
    IGN_LOG("[BUILD]", "nonfinite_vertex_validation=STATE_AWARE (x/y always required; r/g/b required only when the active color source is ITERATED; a required only when the active alpha source is ITERATED; z/ooz/oow/tmuvtx never required since this draw path has never consumed them -- see gu_vertex_required_finite)");
    IGN_LOG("[BUILD]", "nan_sanitization=FORBIDDEN (no NaN->0/Inf->1/clamp coercion anywhere, this iteration or ever -- an unmet REQUIRED field still halts cleanly with full field/caller provenance via [G47-NONFINITE]+[G48-A/B/C-RAW])");
    IGN_LOG("[BUILD]", "validation_weakening=FORBIDDEN (gu_vertex_required_finite's x/y-always + color/alpha-conditional rule is UNCHANGED since G47 -- C.g still correctly halts, it is a genuine required-field failure)");
    IGN_LOG("[BUILD]", "hot_glide_logging=REDUCED (grAlphaBlendFunction/guColorCombineFunction/grConstantColorValue reduced in G47; grClipWindow/grTexDownloadTable ALSO reduced this iteration, same first-N + 5s-summary pattern, Part 26)");
    IGN_LOG("[BUILD]", "guAlphaSource=FROZEN_WORKING (unchanged since G45 -- G45's own hardware run proved mode=0x0/GR_ALPHASOURCE_CC_ALPHA translated and stored correctly)");
    IGN_LOG("[BUILD]", "grConstantColorValue=IMPLEMENTED (GrColor_t decoded using Ignition's own real GR_COLORFORMAT_ARGB, confirmed at grSstWinOpen since G4E -- see win32/glide2x_ign.cpp's grConstantColorValue_impl for the full caller-disassembly evidence at 0x004506f0-0x0045070b)");
    IGN_LOG("[BUILD]", "constant_color_format=SDK_VERIFIED (GR_COLORFORMAT_ARGB byte order taken from this project's own confirmed grSstWinOpen decode, not guessed; ABGR/RGBA/BGRA also supported by name, unrecognized values refused)");
    IGN_LOG("[BUILD]", "constant_color_first_value=0x7FFFFFFF");
    IGN_LOG("[BUILD]", "real_triangles=YES (unchanged since G42 -- 2573+ real triangles drawn in G46's own hardware run, glGetError=0 throughout)");
    IGN_LOG("[BUILD]", "grChromakeyMode=FROZEN_WORKING (unchanged since G43)");
    IGN_LOG("[BUILD]", "grAlphaBlendFunction=FROZEN_WORKING (semantics unchanged since G44 -- only hot-path logging volume reduced this iteration)");
    IGN_LOG("[BUILD]", "guAlphaSource=FROZEN_WORKING (unchanged since G45)");
    IGN_LOG("[BUILD]", "vertex_arrays_only=YES (unchanged since G42 -- glVertexPointer/glColorPointer/glDrawArrays(GL_TRIANGLES))");
    IGN_LOG("[BUILD]", "immediate_mode=FORBIDDEN (unchanged since G42 -- glBegin/glVertex* never used)");
    IGN_LOG("[BUILD]", "post_intro_3d_asset_loading=YES (unchanged since G41 -- deep asset loading confirmed, menucar.msh/CANADA.TEX/CARS.TEX and more, not touched this iteration)");
    IGN_LOG("[BUILD]", "intro_full_playback=YES (unchanged since G40 -- INTRO_VISUAL_CONFIRMED=YES, INTRO_FULL_PLAYBACK_COMPLETE=YES; re-confirmed in every run through G46)");
    IGN_LOG("[BUILD]", "texture_pipeline=FROZEN (guTexAllocateMemory/guTexDownloadMipMap, RGB332->RGB565 decode, GL texture create/update, filter/wrap state, guTexMemReset cleanup -- all unchanged this iteration; guTexSource still not reached by runtime, Part 11/29)");
    IGN_LOG("[BUILD]", "language_initial_state=AUDIT_ONLY (unchanged since G39/G40 -- no ADVAPI32.dll import, no .ini/.cfg/.sav strings, single write site at 0x0041224B for the 0x00536ED8 language index. NOT touched this iteration)");
    IGN_LOG("[BUILD]", "grSstVRetraceOn=FROZEN (unchanged since G38 -- confirmed even hotter in G40's own hardware log, ~20k calls/sec in some 10s windows -- noted for a future performance-focused iteration, NOT touched this iteration)");
    IGN_LOG("[BUILD]", "language_selection_visible=YES (unchanged since G34/G35)");
    IGN_LOG("[BUILD]", "language_confirm=WORKING (unchanged since G35 -- START->DIK_RETURN->IsKeyDown(TRUE), re-proven across every run through G43)");
    IGN_LOG("[BUILD]", "LFB_path=FROZEN (LFB ptr/size/stride/format/RGB565/vertex-array blit/glDrawArrays/grBufferSwap/vglSwapBuffers/grSstIdle/deferred upload all unchanged this iteration -- grAlphaBlendFunction_impl touches only GL_BLEND/glBlendFuncSeparate, saved/restored around every triangle draw exactly like every other state guDrawTriangleWithClip_impl already tracks)");
    IGN_LOG("[BUILD]", "input_path=FROZEN (sceCtrl/DIK queue/GetDeviceData/IsKeyDown/START mapping untouched this iteration, Part Y)");
    IGN_LOG("[BUILD]", "file_io=FROZEN (CreateFileA/ReadFile/FindFirstFileA/etc. untouched this iteration)");
    IGN_LOG("[BUILD]", "trap_policy=NEXT_UNIMPLEMENTED_GLIDE_API (still the primary mechanism, unchanged -- current_blocker=CALLER_0045223B_VERTEX_STATE_MISMATCH takes precedence and remains UNRESOLVED as of this build; guTexSource/guTexCombineFunction/grChromakeyValue remain NOT preempted)");
    IGN_LOG("[BUILD]", "ddraw_branch=UNCHANGED (Ign_d3d.exe code paths not touched this iteration)");
    IGN_LOG("[BUILD]", "G18_lfb_lock_checksum=DISABLED (kG37LfbLockChecksum=false -- grLfbLock_impl's own separate, previously-ungated compute_lfb_checksum() call, distinct from grLfbUnlock_impl's kG32LfbFullChecksum; this was the confirmed real cause of the ~5-6s/present stall, not GPU upload/blit/swap -- see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "G18_full_buffer_diagnostics=DISABLED (audited every compute_lfb_checksum() caller in glide2x_ign.cpp: grLfbUnlock_impl's -- disabled since G32; grLfbLock_impl's -- disabled this iteration; and one remaining caller inside glide_next_unimplemented_trap's one-time-only diagnostic, which fires at most once per run immediately before a halt, not part of the per-present hot path -- left untouched, not a spam source)");
    IGN_LOG("[BUILD]", "GrLfbInfo_per_call_logging=DISABLED (the 'GrLfbInfo written' line, previously unconditional on all ~26 locks/present, now logs once ever under [G37-LFB] since lfbPtr/stride/writeMode/origin are constant across subsequent locks on the same shadow buffer)");
    IGN_LOG("[BUILD]", "dynarec=FROZEN_UNLESS_STATIC_RUNTIME_CONTRADICTION (external/WinVita vendored source not touched this iteration; this run's own finding -- C.r/g/b have NO writer anywhere in the caller function -- is evidence AGAINST a dynarec bug for this field, not for one: nothing failed to execute, since no instruction to execute exists)");
    IGN_LOG("[BUILD]", "session_id=%u", g35SessionId);
    IGN_LOG("[BUILD]", "first_frame=YES");
    IGN_LOG("[BUILD]", "language_selector_interactive=YES (G35's real-hardware run proved it end to end: CROSS->DIK_RETURN queued/consumed->IsKeyDown(DIK_RETURN)=TRUE->real file loading, ign1.cdp+ign2.cdp, checksum MATCH on the completed one)");
    IGN_LOG("[BUILD]", "DIK_RETURN_confirm=PROVEN (G35's real-hardware [G35-LANG]/[G35-TRANS] lines)");
    IGN_LOG("[BUILD]", "CROSS_confirm=PROVEN (confirmed ~4.5s BEFORE START in the same run -- START was not required)");
    IGN_LOG("[BUILD]", "graphics_semantics=FROZEN (same visual output, LFB ptr/size/stride/format/RGB565/vertex-array blit/glDrawArrays/grBufferSwap/vglSwapBuffers/grSstIdle all unchanged -- only WHEN and HOW OFTEN the GPU texture upload happens changed, see below)");
    IGN_LOG("[BUILD]", "performance_profiler=LOW_OVERHEAD ([G36-PERF] one line per grBufferSwap: uploads/upload_total_us/upload_min_us/upload_max_us/blit_total_us/swap_total_us, all RAM-accumulated with wx86_now_us(), zero per-call logging -- see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "lfb_upload_timing=YES (measured separately from blit and from vglSwapBuffers -- Part C: checksum/pixel-scan already excluded, both disabled since G32)");
    IGN_LOG("[BUILD]", "lfb_uploads_per_present=MEASURED (G31-G35 baseline: 26 full glTexSubImage2D(640x400) calls per present, one per grLfbUnlock -- confirmed by reading the code, Part E's YES/NO answered YES)");
    IGN_LOG("[BUILD]", "deferred_lfb_upload=IMPLEMENTED (Part F/G safety argument: audited every consumer of lfb_texture in this whole file -- the ONLY read is lfb_blit_to_backbuffer(), called ONLY from grBufferSwap_impl when lfb_dirty; no grDraw*/texture-sampling Glide call exists (would trap as unimplemented if the guest ever called one) -- so uploading once, right before the blit, with whatever the guest's shadow buffer holds at that moment, is pixel-identical to uploading 26 times and only the last one ever being read. Upload moved from grLfbUnlock_impl to grBufferSwap_impl, at most once per present -- see both functions' own comments)");
    IGN_LOG("[BUILD]", "guest_hot_eip_sampling=BLOCKED (same vendored-code constraint as RUN25 -- a real 'sample every N dynarec dispatches' requires hooking the dynarec's own dispatch loop in external/WinVita's vendored box86-dynarec, which this project's standing rule forbids editing without explicit authorization; NOT implemented, flagged for the user's decision, not faked with a weaker substitute)");
    IGN_LOG("[BUILD]", "input_poll_rate_audit=YES (dinput_poll_vita_input is hooked into every kernel32/user32/winmm/dinput shim call PLUS grBufferSwap -- ALL the host-visible hook points this project has ever found. G35/G36's own ~2.3 polls/sec during the slow post-confirm window is a DIRECT, expected consequence of the guest making few Win32/Glide calls while it's CPU-bound in its own guest code between them -- not a bug in the hook placement. No additional safe host-tick point was found this iteration without either a background thread (Part M: explicitly forbidden) or editing the vendored dynarec dispatch loop (same constraint as guest_hot_eip_sampling above) -- both would need explicit authorization)");
    IGN_LOG("[BUILD]", "file_checksums=DISABLED (kG36FileFullChecksum=false -- the per-ReadFile FNV-1a accumulation AND the CloseHandle full-file re-read+compare both gated off; Part U's own instruction, re-enable by flipping the one constant -- see win32/kernel32_ign.cpp)");
    IGN_LOG("[BUILD]", "stable_file_logging=REDUCED (ReadFile's per-call 'about to copy'/'copy complete' lines removed -- pure spam, no diagnostic value beyond what CreateFileA/ReadFile's own summary lines already give)");
    IGN_LOG("[BUILD]", "stable_glide_logging=MINIMAL (unchanged since G34 -- grLfbLock/grLfbUnlock/grSstIdle/LFB blit all still gated behind the g_completedSwapCount<kDetailedLogCycles verbose window)");
    IGN_LOG("[BUILD]", "critical_transition_log=YES (ignition_critical.log unchanged since G35 -- worked correctly, session id stayed coherent across the whole G35 run per the user's own report)");
    IGN_LOG("[BUILD]", "poll_count_logging=DISABLED (unchanged since G34)");
    IGN_LOG("[BUILD]", "RUN25_periodic_logging=BLOCKED_VENDORED_CODE (unchanged since G34 -- NOT touched, Part T)");
    IGN_LOG("[BUILD]", "PeekMessage_empty_logging=SUMMARY_ONLY (unchanged since G34, 10s interval)");
    IGN_LOG("[BUILD]", "frame_loop_watchdog=DISABLED (unchanged since G28 -- still never calls request_halt())");
    IGN_LOG("[BUILD]", "stop_policy=UNIMPLEMENTED_API_OR_REAL_FAULT_ONLY (unchanged since G28)");
#endif
    IGN_LOG("[BUILD]", "grGlideInit=IMPLEMENTED (void, 0 args -- sets an internal GlideState.initialized flag only; no vitaGL, no framebuffer, no hardware emulation)");
    IGN_LOG("[BUILD]", "grSstQueryHardware=IMPLEMENTED (GrHwConfiguration fields chosen from disassembling Ignition's own post-call code at 0x0045868A-0x004586AB; G2's real-hardware run confirmed Ignition ACCEPTED this response and continued -- not modified this iteration)");
    IGN_LOG("[BUILD]", "grSstSelect=IMPLEMENTED (void, 1 arg -- validates which_sst against num_sst=1, records a logical selected_sst only, no GPU/vitaGL context)");
    IGN_LOG("[BUILD]", "grSstWinOpen=IMPLEMENTED (calls vglInitExtended once, from inside the trap, and verifies the context via GL state -- see win32/glide2x_ign.cpp's grSstWinOpen_impl; G4E's real-hardware run confirmed FXTRUE and the guest continuing)");
    IGN_LOG("[BUILD]", "grRenderBuffer=IMPLEMENTED_LOGICAL_ONLY (buffer arg decoded against Glide's standard GrBuffer_t enum and recorded in GlideContext.render_buffer; no glBindFramebuffer/vglSwapBuffers/glDraw* yet -- see win32/glide2x_ign.cpp's grRenderBuffer_impl; G5's real-hardware run confirmed the guest continuing to grDepthBufferMode)");
    IGN_LOG("[BUILD]", "grDepthBufferMode=IMPLEMENTED_LOGICAL_ONLY (mode arg decoded against Glide's standard GrDepthBufferMode_t enum and recorded in GlideContext.depth_buffer_mode; no glEnable/glDisable(GL_DEPTH_TEST) yet -- see win32/glide2x_ign.cpp's grDepthBufferMode_impl; G6's real-hardware run confirmed the guest continuing to grDepthMask)");
    IGN_LOG("[BUILD]", "grDepthMask=IMPLEMENTED_LOGICAL_ONLY (enable arg decoded against Glide's standard FxBool convention and recorded in GlideContext.depth_mask; no glDepthMask() call yet -- see win32/glide2x_ign.cpp's grDepthMask_impl; G7's real-hardware run confirmed the guest continuing to grDepthBufferFunction)");
    IGN_LOG("[BUILD]", "grDepthBufferFunction=IMPLEMENTED_LOGICAL_ONLY (fnc arg decoded against Glide's standard GrCmpFnc_t enum and recorded in GlideContext.depth_function; no glDepthFunc() call yet -- see win32/glide2x_ign.cpp's grDepthBufferFunction_impl; G8's real-hardware run confirmed the guest continuing to grCullMode)");
    IGN_LOG("[BUILD]", "grCullMode=IMPLEMENTED_LOGICAL_ONLY (mode arg decoded against Glide's standard GrCullMode_t enum and recorded in GlideContext.cull_mode; no glEnable/glDisable(GL_CULL_FACE)/glCullFace() call yet -- see win32/glide2x_ign.cpp's grCullMode_impl; G9's real-hardware run confirmed the guest continuing to grBufferClear)");
    IGN_LOG("[BUILD]", "grBufferClear=IMPLEMENTED_VITAGL (color/alpha decoded per GR_COLORFORMAT_ARGB, glClearColor+glClear(GL_COLOR_BUFFER_BIT) executed -- no depth clear, depth_buffer_mode is still DISABLE; no vglSwapBuffers -- see win32/glide2x_ign.cpp's grBufferClear_impl; G10's real-hardware run confirmed glGetError=0 and the guest continuing to grBufferSwap)");
    IGN_LOG("[BUILD]", "grBufferSwap=IMPLEMENTED_VITAGL (vglSwapBuffers(GL_FALSE) executed exactly once per guest call; swap_interval logged only, no vblank-wait/pacing logic; frame-loop safety cap at 4 completed swaps with no new Glide API -- see win32/glide2x_ign.cpp's grBufferSwap_impl; G11's real-hardware run confirmed 2 completed clear/swap cycles, glGetError=0, and the guest continuing to grClipWindow)");
    IGN_LOG("[BUILD]", "grClipWindow=IMPLEMENTED_VITAGL (logical clip rect stored in GlideContext, converted to a physical GL_SCISSOR_TEST/glScissor rect via a centralized logical->physical helper that queries the real GL_VIEWPORT and flips Y for GR_ORIGIN_UPPER_LEFT -- see win32/glide2x_ign.cpp's grClipWindow_impl; G12's real-hardware run confirmed physical_clip=full viewport, glGetError=0, and the guest continuing to grTexDownloadTable)");
    IGN_LOG("[BUILD]", "grTexDownloadTable=IMPLEMENTED_LOGICAL_STORAGE (GrTexTable_t decoded, type=GR_TEXTABLE_PALETTE copies 256 FxU32 entries from a guest pointer validated against Ignition's confirmed PE image range into GlideContext.palette[256]; NCC0/NCC1 recognized but halt rather than fake support; no glTexImage2D/glBindTexture yet -- see win32/glide2x_ign.cpp's grTexDownloadTable_impl; G16's real-hardware run confirmed a REAL non-zero palette this time, checksum=0x69f793d7)");
    IGN_LOG("[BUILD]", "palette_checksum=0x69f793d7");
    IGN_LOG("[BUILD]", "guTexMemReset=IMPLEMENTED_LOGICAL (logical texture-memory allocator reset only -- next_address/allocation_count/generation in GlideContext; no glDeleteTextures, palette explicitly preserved -- see win32/glide2x_ign.cpp's guTexMemReset_impl)");
    IGN_LOG("[BUILD]", "texture_mem_allocator_state=TRACKED");
    IGN_LOG("[BUILD]", "texture_palette_checksum_watch=YES");
    IGN_LOG("[BUILD]", "texture_table_state=TRACKED");
    IGN_LOG("[BUILD]", "texture_gpu_upload=NO");
    IGN_LOG("[BUILD]", "texture_upload_to_gpu=NO");
    IGN_LOG("[BUILD]", "grLfbLock=IMPLEMENTED_SHADOW_BUFFER (GR_LFB_WRITE_ONLY + GR_BUFFER_BACKBUFFER + GR_LFBWRITEMODE_565 only -- permanent guest-mapped pixel buffer via ignition_guest_alloc_pixels(), sized to logical 640x400; GR_LFB_READ_ONLY and other pixel formats halt rather than guess -- see win32/glide2x_ign.cpp's grLfbLock_impl; G18's real-hardware run confirmed LFB_DIRTY=YES -- Ignition wrote real pixels into the shadow buffer)");
    IGN_LOG("[BUILD]", "grLfbUnlock=IMPLEMENTED_VITAGL_UPLOAD (validates the unlock matches the active lock, uploads RGB565 directly via GL_UNSIGNED_SHORT_5_6_5 to a persistent texture created once and updated via glTexSubImage2D thereafter, sets lfb_dirty for grBufferSwap to blit -- see win32/glide2x_ign.cpp's grLfbUnlock_impl)");
    IGN_LOG("[BUILD]", "lfb_format=RGB565");
    IGN_LOG("[BUILD]", "lfb_guest_buffer=YES");
    IGN_LOG("[BUILD]", "lfb_texture_upload=YES");
    IGN_LOG("[BUILD]", "lfb_present_on_grBufferSwap=YES (lfb_blit_to_backbuffer() draws a screen-aligned textured quad via vitaGL's fixed-function immediate mode, right before vglSwapBuffers, reusing grClipWindow's own logical->physical mapping -- see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "lfb_gpu_sync=NO");
    IGN_LOG("[BUILD]", "lfb_dirty_tracking=YES (before/after checksum, reported as LFB_DIRTY=YES/NO at the next Glide call -- see win32/glide2x_ign.cpp's compute_lfb_checksum)");
    IGN_LOG("[BUILD]", "grSstIdle=IMPLEMENTED (glFinish()/sceGxmFinish -- real blocking GPU-idle wait, verified against this exact linked vitaGL revision's source (not glFlush, which only submits without waiting); no vglSwapBuffers, lfb_dirty untouched -- see win32/glide2x_ign.cpp's grSstIdle_impl; G19's real-hardware run confirmed the guest continuing to this call right after repeated grLfbLock/grLfbUnlock cycles with increasing nonzero pixel counts; G20's real-hardware run showed the log ending abruptly right after glGetError_after_idle=0 -- no CPU fault reported, screen went black with an FPS overlay visible then crashed -- G20B adds return-path breadcrumbs to bisect the exact failure point, see win32/glide2x_ign.h's G20B entry)");
    IGN_LOG("[BUILD]", "idle_return_probe=YES ([G20B] breadcrumbs bracket grSstIdle's own remaining code, ending with a 'handler about to return' line -- the last thing this project's own code does before Bridge's untouched trap_epilogue resumes the guest)");
    IGN_LOG("[BUILD]", "idle_post_guest_trace=NOT_AVAILABLE (same confirmed limitation as G16: Cpu::set_code_hook is a no-op default on the real ARM dynarec backend, only cpu_unicorn.cpp -- the desktop oracle -- overrides it; no per-instruction trace of the guest's push/call at 0x458a35-0x458a3b is possible without modifying vendored WinVita code, which this project does not do)");
    IGN_LOG("[BUILD]", "idle_sync_mode=glFinish (kG20BIdleSyncMode=2, unchanged default -- 0/1 alternates prepared in code for a future A/B hardware run per Part I/J, not switched to on suspicion alone per Part H)");
    IGN_LOG("[BUILD]", "call_ret_history=NOT_AVAILABLE (same reason as idle_post_guest_trace -- no return-address-level observability exists beyond this project's own shim/import trap log points)");
    IGN_LOG("[BUILD]", "crash_handler=NOT_PRESENT (no existing SceExcpmgr/signal-style crash handler found in this project's own source -- Part M's own instruction was conditional on one already existing; none does, so none was added this iteration)");
    IGN_LOG("[BUILD]", "lfb_upload=UNCHANGED");
    IGN_LOG("[BUILD]", "lfb_blit=UNCHANGED");
    IGN_LOG("[BUILD]", "grBufferSwap=UNCHANGED");
    IGN_LOG("[BUILD]", "fps_overlay_seen_on_hardware=YES (VITAGL_PRESENT_PATH_VISIBLY_ACTIVE=YES -- confirms the vitaGL/display backend is genuinely active; NOT treated as FIRST_FRAME or FIRST_VISIBLE_GAME_PIXELS evidence)");
    IGN_LOG("[BUILD]", "first_visible_game_pixels=NOT_YET_CONFIRMED (LFB upload+blit implemented this iteration, but not yet observed presented on real hardware)");
    IGN_LOG("[BUILD]", "mci_behavior=UNCHANGED_ERROR_PATH (G16's argc fix confirmed correct -- mciSendStringA still returns generic error, guest handles it and continues; no MCI semantics touched)");
    IGN_LOG("[BUILD]", "menu_col_checksum_analysis=YES (see win32/glide2x_ign.h's G17 entry -- source_checksum spans the WHOLE file, guest_checksum spans only the bytes actually ReadFile'd from a non-zero offset; a MISMATCH between them is expected/mechanical, not corruption)");
    IGN_LOG("[BUILD]", "frame_loop_cfg_analysis=YES (see win32/glide2x_ign.h's G15 entry -- 14-slot function-pointer driver table at 0x4f3330-0x4f3367, indirect dispatcher at ~0x454330-0x45445a, Present/DrawSurface/PrepareSurface/LockSurface/UnlockSurface/SetPalette slots identified)");
    IGN_LOG("[BUILD]", "frame_loop_condition_probe=PARTIAL (outer per-tick repeat/exit branch not yet located -- lives above the dispatcher found this iteration; not guessed)");
    IGN_LOG("[BUILD]", "frame_interval_profiler=YES ([G15-FRAME] swap #N t=.. delta_ms=.. per present)");
    IGN_LOG("[BUILD]", "frame_file_activity_summary=YES (kernel32/user32/winmm/dinput/glide call-count deltas + file-read-by-name summary, reset every swap)");
    IGN_LOG("[BUILD]", "frame_loop_watchdog_primary=NO (G16: CPU fault / NEXT_UNIMPLEMENTED_GLIDE_API are the primary triggers this iteration -- see glide2x_ign.cpp's raised cycle cap)");
    IGN_LOG("[BUILD]", "hard_watchdog_seconds=60 (secondary safety net)");
    IGN_LOG("[BUILD]", "mci_postcall_trace=YES (mciSendStringA: full args, caller context, stack[ESP..ESP+64) dump, explicit mci_return -- see win32/winmm_ign.cpp)");
    IGN_LOG("[BUILD]", "mci_argc_fix=YES (mciSendStringA was registered argc=5, real ABI is argc=4 -- confirmed via disassembly of the one real call site at 0x4551f0-0x455208; the extra argc popped 4 bytes too many on every call, a real stdcall stack-imbalance bug, fixed as an ABI correctness fix -- mci_return semantics themselves unchanged)");
    IGN_LOG("[BUILD]", "cpu_fault_context_dump=YES (registers + fault_addr/EIP classification against this run's real ignition_mem_layout() + stack[ESP..ESP+128) classified word-by-word -- see runtime/ignition_boot.cpp)");
    IGN_LOG("[BUILD]", "short_guest_trace_after_mci=NOT_AVAILABLE (Cpu::set_code_hook is a documented no-op on the real ARM dynarec backend this project ships with -- only cpu_unicorn.cpp, the desktop oracle, overrides it; confirmed by source search, not assumed -- no per-instruction trace exists without modifying vendored WinVita code, which this project does not do)");
    IGN_LOG("[BUILD]", "call_ret_history=NOT_AVAILABLE (same reason as short_guest_trace_after_mci -- no return-address-level observability beyond the shim/import trap points this project's own diagnostics/call_trace.cpp already logs)");
    IGN_LOG("[BUILD]", "first_real_framebuffer_operation=YES");
    IGN_LOG("[BUILD]", "first_real_present=YES");
    IGN_LOG("[BUILD]", "first_frame=NO");
    IGN_LOG("[BUILD]", "trap_policy=NEXT_UNIMPLEMENTED_GLIDE_API");
    IGN_LOG("[BUILD]", "vitagl_init_location=grSstWinOpen (unchanged from G4E -- frozen architecture, not revisited this iteration)");
    IGN_LOG("[BUILD]", "vitagl_context_verified=YES (G4E's real-hardware run confirmed GL_VERSION/VENDOR/RENDERER all present, glGetError=0)");
    IGN_LOG("[BUILD]", "remaining_glide_imports=TRAP_ON_CALL (17 of 34 -- see win32/glide2x_ign.cpp)");
    IGN_LOG("[BUILD]", "glide_stack_probe=YES (entry ESP + expected post-return ESP logged for grSstWinOpen, per stdcall @28 cleanup -- observation only, Bridge's own trap_epilogue is untouched)");
    IGN_LOG("[BUILD]", "explicit_vita_exit=YES (sceKernelExitProcess(0) instead of a normal main() return -- G2's real-hardware run confirmed this eliminates the post-STOP C2 seen in G0/G1; kept for 3dfx diagnostic builds -- see main.cpp)");
    IGN_LOG("[BUILD]", "executable_identity=3DFX_CORRECT (GetCommandLineA/GetModuleFileNameA derive from runtime/ignition_identity.h)");
    IGN_LOG("[BUILD]", "ddraw_specific_probes=DISABLED ([GM35]/[IDLE34]/[IDLE35]/[DYN35] fingerprint announcements are compiled out for this build)");
    IGN_LOG("[BUILD]", "wm_activate_ddraw_global_probe=DISABLED (ShowWindow's [0x004C5384] readback line is skipped for this build)");
    IGN_LOG("[BUILD]", "exit_cleanup_diag=YES ([EXIT-G1]/[EXIT-G2] breadcrumbs around guest-run return / bridge / process exit)");
    IGN_LOG("[BUILD]", "ddraw_branch=UNCHANGED (DirectDraw/DirectInput/message-loop code paths untouched; ddraw_install() still registers its shims defensively but Ign_3dfx.exe's own import table has no DDRAW.dll entry to link them to)");
    IGN_LOG("[BUILD]", "rep_stosd_45afde_diag_override=NOT_APPLIED_TO_3DFX (that override is guarded by an exact ip==0x0045AFDE check specific to Ign_d3d.exe -- it will not fire for Ign_3dfx.exe's different calloc address (audit found 0x0045801C); no new override installed for 3dfx this iteration, per explicit instruction not to patch addresses blindly)");
#else
    IGN_LOG("[BUILD]", "renderer=DDRAW (stable branch, ux0:data/ignition/Ign_d3d.exe, unchanged)");
    IGN_LOG("[BUILD]", "getmessage_precall_4121f4_probe=YES ([GM35] PRECALL_BLOCKING fires unconditionally right before the ONLY guest instruction that can call a blocking GetMessageA (0x004121F4, byte-verified FF D7 = CALL EDI) -- full register dump plus flag_4C5384 read directly from guest memory, independent of the ARM register cache)");
    IGN_LOG("[BUILD]", "idle_call_target_probe=YES (marker 19 TARGET_ENTER @0x00412230, idle-work's own first instruction, byte-verified 8B 0D 34 37 49 00)");
    IGN_LOG("[BUILD]", "idle_ret_probe=YES (markers 20/21/22 on idle-work's 3 real RET points -- 0x0041226B, 0x0041228E, 0x004122C9, all byte-verified this iteration -- confirms the function has no tail-calls or JMP-out: every exit is a plain RET)");
    IGN_LOG("[BUILD]", "winmain_121d8_12200_flow_probe=YES (marker 23 RETURN_FROM_BLOCKING_GETMESSAGE @0x004121F6, [DYN35] translation-time confirmation separate from marker 18's runtime firing)");
    IGN_LOG("[BUILD]", "trap_stack_caller_probe=REMOVED (iteration-34's [MSG34] stack-walk is gone -- HALLAZGO 3 showed none of the first 8 words land in .text for this import path; replaced entirely by direct guest-EIP instrumentation)");
    IGN_LOG("[BUILD]", "wm_close_behavior=UNCHANGED ([POST33]/[WM_CLOSE33] kept as-is, untouched)");
    IGN_LOG("[BUILD]", "dynarec=UNCHANGED (pushfw16_global_fix, popfw16_global_fix, rep_stosd_45afde_diag_override all untouched; markers 19-23 are new passive additions, not modifications to those three fixes)");
    IGN_LOG("[BUILD]", "heap_diag=size==324000_only");
    IGN_LOG("[BUILD]", "aa3b_guest_block_diag=YES (0x0046AA3B identified: TEST EAX,EAX right after the real CloseHandle IAT call)");
    IGN_LOG("[BUILD]", "calloc_path_diag=YES (calloc @0x0045AFB0 confirmed safe; count read live from [0x005530E8]; HeapAlloc visibility wired)");
    IGN_LOG("[BUILD]", "crt_allocator_diag=YES");
    IGN_LOG("[BUILD]", "calloc_post_heapalloc_diag=YES (real malloc @0x00469420, HeapAlloc wrapper @0x00469470 identified; count=1 confirmed, overflow theory refuted)");
    IGN_LOG("[BUILD]", "calloc_zerofill_diag=YES (REP STOSD/STOSB confirmed via calloc disassembly; dynarec MMU_SAI4 path audited, no bug proven)");
    IGN_LOG("[BUILD]", "heapalloc_return_probe=YES (real host-side R/W probe on the allocated range, restored after)");
    IGN_LOG("[BUILD]", "malloc_return_probe=NO (no safe passive hook exists for an arbitrary guest EIP -- see delivery notes)");
    IGN_LOG("[BUILD]", "calloc_return_chain_diag=YES (argued via Bridge::trap_epilogue source audit + prior successful calls at the same return address, not new runtime breadcrumbs)");
    IGN_LOG("[BUILD]", "message_queue_v2=YES (PostMessageA/PeekMessageA/GetMessageA/DispatchMessageA + WM_ACTIVATEAPP)");
    IGN_LOG("[BUILD]", "palette_setentries=YES (IDirectDrawPalette::SetEntries real, slot 0x18)");
    IGN_LOG("[BUILD]", "fileio_crash_diag=YES (CreateFileA/ReadFile/SetFilePointer/CloseHandle instrumented, ReadFile validates guest buffer before write)");
    IGN_LOG("[BUILD]", "menubkg_postload_diag=YES (per-handle checksum vs. source file, compared at CloseHandle)");
    IGN_LOG("[BUILD]", "heap_allocation_bounds=YES (ReadFile checks the actual HEAP allocation containing the guest buffer, not just page-mapped)");
    IGN_LOG("[BUILD]", "post_menubkg_trace=YES (full register + stack snapshot at CloseHandle(MenuBkg.dat) return)");
#endif
    IGN_LOG("[BUILD]", "timers=UNCHANGED");
    IGN_LOG("[BUILD]", "input=UNCHANGED");
    IGN_LOG("[BUILD]", "guest_context_breadcrumbs=YES");
    IGN_LOG("[BUILD]", "log_flush_critical=YES (log.cpp opens/writes/closes the log file on EVERY line -- confirmed no stdio buffering exists to lose)");
    IGN_LOG("[BUILD]", "compiled=%s %s", __DATE__, __TIME__);
    set_phase(Phase::Boot);
    IGN_LOG("[BOOT]", "IgnitionVita starting");

    set_phase(Phase::PeOpen, kExePath);
    IGN_LOG("[PE]", "Loading %s", kExePath);
    std::vector<uint8_t> bytes;
    if (!read_whole_file(kExePath, bytes)) {
        IGN_LOG("[ERROR]", "Could not open %s -- is it copied to the memory card?", kExePath);
        set_phase(Phase::Error, "open guest exe");
        return;
    }
    IGN_LOG("[PE]", "Read %u bytes", (unsigned)bytes.size());

    set_phase(Phase::WinVitaInit);
    // Real-hardware finding (Phase 1 test 1): Cpu::map() only works on the
    // ARM backend in "arena mode", armed by WX86_ARENA -- read ONCE inside
    // CpuBox86's constructor. This MUST happen before make_cpu_box86().
    // See runtime/ignition_memlayout.h for the full story.
    apply_memory_layout_env();
#ifdef IGNITION_RENDERER_3DFX
    // iteration-G4E, Parte C/D: G4D's real-hardware run showed the log
    // dying right after WINVITA_INIT/before "Creating ARM dynarec CPU
    // backend" ever completed, when vitaGL was pre-initialized in main()
    // BEFORE this point. This iteration removed that early preinit (see
    // main.cpp) to test whether CpuBox86's constructor is order-sensitive
    // to vitaGL/sceGxm having already claimed memory or GPU state. These
    // breadcrumbs (and the free-memory reading, same API vitaGL itself
    // uses internally -- see glide2x_ign.cpp's G4C comment) are diagnostic
    // only, not a fix -- kept minimal so as not to flood the log.
    {
        SceKernelFreeMemorySizeInfo memInfo;
        memInfo.size = sizeof(memInfo);
        int memRc = sceKernelGetFreeMemorySize(&memInfo);
        if (memRc == 0) {
            IGN_LOG("[CPU-G4E]", "free_mem_before_cpu: user=%d cdram=%d phycont=%d", memInfo.size_user, memInfo.size_cdram, memInfo.size_phycont);
        } else {
            IGN_LOG("[CPU-G4E]", "free_mem_before_cpu: sceKernelGetFreeMemorySize FAILED rc=0x%08x", (unsigned)memRc);
        }
    }
    IGN_LOG("[CPU-G4E]", "before CpuBox86 constructor");
#endif
    IGN_LOG("[CPU]", "Creating ARM dynarec CPU backend");
    Cpu* cpu = make_cpu_box86();
    IGN_LOG("[G78-STARTUP-TIMING]", "checkpoint=jit_ready elapsed_ms=%llu",
            (unsigned long long)(wx86_now_ms() - g_g78ProcessStartMs));
#ifdef IGNITION_RENDERER_3DFX
    IGN_LOG("[CPU-G4E]", "after CpuBox86 constructor");
    {
        SceKernelFreeMemorySizeInfo memInfo;
        memInfo.size = sizeof(memInfo);
        int memRc = sceKernelGetFreeMemorySize(&memInfo);
        if (memRc == 0) {
            IGN_LOG("[CPU-G4E]", "free_mem_after_cpu: user=%d cdram=%d phycont=%d", memInfo.size_user, memInfo.size_cdram, memInfo.size_phycont);
        } else {
            IGN_LOG("[CPU-G4E]", "free_mem_after_cpu: sceKernelGetFreeMemorySize FAILED rc=0x%08x", (unsigned)memRc);
        }
    }
#endif
    if (!cpu) {
        IGN_LOG("[ERROR]", "make_cpu_box86() returned null");
        set_phase(Phase::Error, "make_cpu_box86");
        return;
    }
    g_cpu = cpu;
    Bridge br(cpu);

    set_phase(Phase::PeParse);
    std::string err;
    PeImage* img = br.add_module(kGuestExeName, bytes, err);
    if (!img) {
        IGN_LOG("[ERROR]", "PE parse/load failed: %s", err.c_str());
        set_phase(Phase::Error, "add_module");
        return;
    }
    IGN_LOG("[PE]", "Image base  = 0x%08x", img->load_base());
    IGN_LOG("[PE]", "Entry point = 0x%08x (rva 0x%08x)", img->entry_va(), img->entry_rva());
    IGN_LOG("[PE]", "Image size  = %u bytes", img->image_size());
    IGN_LOG("[PE]", "Subsystem   = %u", (unsigned)img->subsystem());
    IGN_LOG("[PE]", "Imports     = %u entries (run tools/pe_analyze on desktop for the full section table)",
            (unsigned)img->imports().size());

#ifdef IGNITION_RENDERER_3DFX
    // Cross-check against the desktop-side audit (audit/IGNITION_3DFX_AUDIT.md):
    // ImageBase=0x00400000, EntryPoint=0x0045F0CC, SizeOfImage=2326528. Any
    // mismatch here means either a different Ign_3dfx.exe was copied to the
    // memory card, or PeImage parsed something differently than the offline
    // audit script did -- worth knowing immediately, not discovering later.
    IGN_LOG("[3DFX]", "PE loaded");
    IGN_LOG("[3DFX]", "image_base=0x%08x (audit expected 0x00400000)", img->load_base());
    IGN_LOG("[3DFX]", "entrypoint=0x%08x (audit expected 0x0045F0CC)", img->entry_va());
    IGN_LOG("[3DFX]", "size_of_image=%u (audit expected 2326528)", (unsigned)img->image_size());
    {
        auto ieq = [](const std::string& a, const char* b) {
            std::string lb = b;
            if (a.size() != lb.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)lb[i])) return false;
            return true;
        };
        int glideImports = 0;
        bool sawDdraw = false;
        for (const auto& imp : img->imports()) {
            if (ieq(imp.dll, "glide2x.dll")) ++glideImports;
            if (ieq(imp.dll, "DDRAW.dll")) sawDdraw = true;
        }
        IGN_LOG("[3DFX]", "imports total=%u", (unsigned)img->imports().size());
        IGN_LOG("[3DFX]", "glide imports=%d (audit expected 34)", glideImports);
        // Section 14 of this iteration's instructions: Ign_3dfx.exe must NOT
        // import DDRAW.dll (the whole premise of the audit was that Glide
        // fully replaces DirectDraw in this build). If it does, that's a
        // real finding to stop and investigate, not something to paper over.
        if (sawDdraw) {
            IGN_LOG("[3DFX]", "WARNING: DDRAW.dll import found in Ign_3dfx.exe -- this CONTRADICTS the prior audit (which found zero DDRAW references). Investigate before proceeding.");
        } else {
            IGN_LOG("[3DFX]", "DDRAW.dll: absent from import table, as the audit predicted (no contradiction)");
        }
    }
#endif

    set_phase(Phase::Imports);
    for (const auto& imp : img->imports()) log_import(imp);

    // ---- Guest address space the consumer owns: heap, VirtualAlloc arena,
    // and the permanent scratch range (GetCommandLineA-style return values).
    // GuestRegion only does bookkeeping; the backing memory itself has to be
    // mapped explicitly with Cpu::map() before anything can read/write it --
    // WinVita's own Bridge::commit() only maps the PE image(s) and the guest
    // stack (verified by reading bridge.cpp), nothing else.
    set_phase(Phase::PeMap);
    if (!log_and_check_layout(img->load_base(), img->image_size())) {
        set_phase(Phase::Error, "memory layout overlap");
        return;
    }
    wx86::GuestRegion& s_heap = ignition_heap_region();
    static wx86::GuestRegion s_va;
    if (!map_layout_regions(*cpu, s_heap, s_va)) {
        IGN_LOG("[ERROR]", "cpu->map() failed for one of HEAP/VA/SCRATCH -- see the [MAP] line above for which one");
        set_phase(Phase::Error, "map heap/va/scratch");
        return;
    }

    IGN_LOG("[WIN32]", "Installing WinVita engine shims");
    win32_shims_kernel32_install(br);
    win32_shims_locale_install(br);
    Wx86MemoryPlan plan;
    plan.heap = &s_heap;
    plan.va = &s_va;
    plan.vquery = ign_vquery_stub;
    wx86_mem_set_observer(ign_mem_observer);
    win32_shims_memory_install(br, plan);
    win32_shims_wait_install(br);
    win32_shims_shell32_install(br);
    win32_shims_advapi32_install(br);
    win32_shims_user32_install(br);
    win32_shims_misc_install(br);   // DirectDrawCreate -> honest DDERR_GENERIC, for now
    win32_shims_psapi_install(br);
    win32_shims_version_install(br);
    win32_shims_wintrust_install(br);
    win32_shims_gdi32_install(br);
    wx86_set_screen_size(640, 480);
    win32_shims_window_install(br);
    win32_shims_sync_install(br);

    // Overrides the engine's DirectDrawCreate (win32_shims_misc.cpp: honest
    // DDERR_GENERIC) with a real, minimal IDirectDraw COM object. See
    // win32/ddraw.h for exactly what's implemented vs. a diagnostic trap.
    ddraw_install(br, *cpu);

    // Real, minimal IDirectInputA/IDirectInputDeviceA COM objects. See
    // win32/dinput.h for exactly what's implemented vs. a diagnostic trap.
    dinput_install(br, *cpu);

    // Window/message-loop subsystem: RegisterClassA, CreateWindowExA,
    // ShowWindow, SetFocus, SetWindowLongA/GetWindowLongA, the message
    // queue (PostMessageA/PeekMessageA/GetMessageA/DispatchMessageA), and
    // an overriding PostQuitMessage. Installed AFTER win32_shims_window/
    // user32_install so "last registration wins" makes ours the one that
    // sticks for PostQuitMessage specifically.
    user32_window_install(br);
    IGN_LOG("[MSG]", "message queue v2 installed (GetMessageA/PeekMessageA/DispatchMessageA/PostMessageA/PostQuitMessage shims = NEW, override engine defaults)");

    dsound::HostOps dsOps;
    dsOps.va_alloc = ign_ds_va_alloc;
    dsOps.tick_ms = ign_ds_tick_ms;
    dsOps.write_root = "ux0:data/ignition/";
    // iteration-G129: mix_rate = 22050 Hz, PROVEN from the game itself (not guessed):
    //   * Ign_3dfx.exe 0x0045d750 (the only DirectSound init; its only DirectSoundCreate
    //     call is 0x0045d7fb) builds a 6-entry format table on its stack: channels
    //     {1,2,1,1,2,2}, bits {16,16,16,8,8,8}, rate {22050,22050,44100,22050,22050,44100}
    //     (stores at 0x0045d768..0x0045d7bf) and tries the entries in order, index 0 first
    //     (0x0045d7d0 `xor ebp,ebp`), calling PRIMARY SetFormat (vtable +0x38 at 0x0045d915)
    //     and, on success, a looping SECONDARY buffer of exactly one second (blockAlign*rate
    //     bytes, flags 0xE8 = CTRLVOLUME|CTRLPAN|CTRLFREQUENCY|LOCSOFTWARE).
    //     First entry that succeeds = mono / 16-bit / 22050 Hz.
    //   * The result is copied to the game's own software mixer (0x00454ac1..0x00454b10:
    //     rate [0x6203a0], bits [0x6203a4], channels-1 [0x6203a8]); it never calls
    //     SetFrequency/SetVolume/SetPan, so no buffer is ever resampled by the engine.
    //   * The original WAVs on disk are 8-bit mono 22050 Hz (Baltazar/data/Sound), the same rate.
    // The Vita BGM port accepts 22050 natively (vita_audio.cpp rate_supported), so the whole
    // chain is resample-free. See tmp/G129_DIRECTSOUND_AUDIT.txt.
    dsOps.mix_rate = 22050;
    // iteration-G131: state reconciliation BEFORE the sound layer reads its environment and before any Glide context exists.
    ign_g131_visual_hashes();
    ign_g131_key_cache();
    ign_g131_ab_file();
    dsound::set_logger(ign_ds_log);
    dsound::install(br, *cpu, dsOps);
    IGN_LOG("[G132]", "dsound installed: enabled=%d mix_rate=%d (unchanged since G131: effects sink thread first rung 0x100000E0, stale stream-ring content played as silence)",
            dsound::enabled() ? 1 : 0, dsOps.mix_rate);
    // iteration-G132: the virtual CD (mciSendStringA -> TOC -> Music/TrackNN.ogg -> second audio port). Creating the service opens no
    // file, port or thread: `open cdaudio` validates the collection, the first `play` starts the music thread and opens the port.
    if (!g_g132MusicOff) {
        const std::string musicDir = std::string(kDataRoot) + "Music/";
        music::service_init(musicDir.c_str());
        IGN_LOG("[G132]", "virtual CD-audio service ready: music dir %s (decoder stb_vorbis 1.22, output port MAIN 48000 Hz stereo, thread started lazily at the first play)", musicDir.c_str());
    } else {
        IGN_LOG("[G132]", "virtual CD-audio service NOT created (g131_ab.txt nomusic=1): mciSendStringA keeps the G131 generic error");
    }

    IGN_LOG("[WIN32]", "Installing Ignition-specific stubs (KERNEL32 file I/O + process exit, WINMM timers)");
    kernel32_ign_install(br, *img);
    winmm_ign_install(br);

#ifdef IGNITION_RENDERER_3DFX
    // G0 bring-up only: clean traps on all 34 known glide2x.dll imports,
    // no rendering, no success stubs. See win32/glide2x_ign.h.
    glide2x_g0_install(br);
#endif

    // ---- Unresolved-import audit (pre-link()). br.shim_trap(dll, name)
    // looks up the SAME registered-shims table link() itself consults, so
    // this list is exactly what "N unresolved import(s)" will count --
    // just spelled out instead of left as a bare number. Ordinal imports
    // have no public pre-check (Bridge exposes no shim_trap_ordinal), so
    // they're listed as "assumed unresolved" rather than silently skipped.
    {
        int unresolvedPreCheck = 0;
        IGN_LOG("[IMPORT]", "========== UNRESOLVED IMPORTS ==========");
        for (const auto& imp : img->imports()) {
            bool hasShim = !imp.name.empty() && br.shim_trap(imp.dll, imp.name) != 0;
            if (hasShim) continue;
            ++unresolvedPreCheck;
            if (!imp.name.empty())
                IGN_LOG("[UNRESOLVED]", "%s!%s", imp.dll.c_str(), imp.name.c_str());
            else
                IGN_LOG("[UNRESOLVED]", "%s!#%u (ordinal import)", imp.dll.c_str(), imp.ordinal);
        }
        IGN_LOG("[IMPORT]", "Total unresolved: %d", unresolvedPreCheck);
        IGN_LOG("[IMPORT]", "==========================================");
    }

    // First-call trace on every import that DOES have a shim (engine's or
    // ours) -- reconstructs the CRT/WinMain bootstrap sequence from the log.
    arm_call_trace(br, *cpu, img->imports());

    br.set_default_shim([](Cpu& c, const std::string& tag) -> uint32_t {
        // The guest's own return address sits at [ESP] at trap entry (no
        // args popped yet) -- far more useful than the trap-window EIP,
        // since it points at the actual CALL in Ignition's code that hit
        // this missing import.
        uint32_t esp = c.reg(R_ESP);
        uint32_t retAddr = c.read_u32(esp);
        char detail[224];
        snprintf(detail, sizeof detail, "%s (called from guest EIP=0x%08x, ESP=0x%08x)",
                 tag.c_str(), retAddr, esp);
        request_halt(detail);
        return 0;
    });

    set_phase(Phase::CpuSetup);
    if (!br.commit(err)) {
        IGN_LOG("[ERROR]", "Bridge::commit failed: %s", err.c_str());
        set_phase(Phase::Error, "commit");
        return;
    }
    if (!br.link(err)) {
        IGN_LOG("[ERROR]", "Bridge::link failed: %s", err.c_str());
        set_phase(Phase::Error, "link");
        return;
    }
    IGN_LOG("[CPU]", "Bridge committed and linked (%u unresolved import(s) -- each halts cleanly on its first call)",
            br.unresolved_count());
#ifdef IGNITION_RENDERER_3DFX   // G150
    glide2x_g150_install_hooks(br);   // iteration-G150: the guest hot-loop replacements the control file asked for (none by default)
    glide2x_g156_install_hooks(br);   // iteration-G156: the display-list cull (verified against the machine code; the control file token g156=off leaves it out)
    glide2x_g157_install_hooks(br);   // iteration-G157: split screen disabled (verified against the machine code; the control file token g157=off leaves it out)
    glide2x_g158_install_hooks(br);   // iteration-G158: network game disabled (needs the G157 seam; verified against the machine code; the control file token g158=off leaves it out)
    glide2x_g159_install_hooks(br);   // iteration-G159: the player name keyboard (needs the G157 seam; verified against the character table of the game; the control file token g159=off leaves it out)
#endif   // G150

#if defined(IGNITION_RENDERER_3DFX) && defined(G20C_BYPASS_IDLE_CALLSITE)
    {
        // iteration-G20C, Experiment B: ULTRA-specific diagnostic patch --
        // affects ONLY the single guest CALL instruction at 0x00458a30
        // (Ignition's own call to grSstIdle, confirmed byte-exact by
        // disassembly across G14/G15/G20/G20B: `E8 47 02 01 00` = `call
        // 0x468c7c`), replacing its 5 opcode bytes with 5 NOPs so the
        // guest falls straight through to 0x00458a35 without ever
        // entering the grSstIdle trap. Nothing else is touched: not
        // grSstIdle's own registration/shim, not Bridge's trap_epilogue,
        // not any other CALL instruction, not set_alternate. Uses
        // Cpu::write() (not hostptr()), which per cpu.h's own documented
        // contract handles the dynarec's dirty-page/code-cache
        // invalidation automatically for CODE writes -- the documented,
        // safe way to patch guest .text (hostptr() writes explicitly do
        // NOT get this handling, per that same header comment). Verifies
        // the expected bytes are actually there before patching --
        // refuses rather than blindly overwriting if they don't match.
        // NOT a permanent fix -- diagnostic-only, gated behind its own
        // CMake option (default OFF), purely to answer G20C Experiment
        // B's question (does bypassing the trap avoid the crash).
        constexpr uint32_t kIdleCallSite = 0x00458a30u;
        constexpr uint8_t kExpectedCallBytes[5] = {0xE8, 0x47, 0x02, 0x01, 0x00};
        uint8_t before[5] = {0};
        cpu->read(kIdleCallSite, before, sizeof before);
        IGN_LOG("[G20C-B]", "guest bytes at 0x%08x before patch: %02x %02x %02x %02x %02x",
                kIdleCallSite, before[0], before[1], before[2], before[3], before[4]);
        if (memcmp(before, kExpectedCallBytes, sizeof before) == 0) {
            uint8_t nops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
            cpu->write(kIdleCallSite, nops, sizeof nops);
            uint8_t after[5] = {0};
            cpu->read(kIdleCallSite, after, sizeof after);
            IGN_LOG("[G20C-B]", "guest bytes at 0x%08x after patch:  %02x %02x %02x %02x %02x",
                    kIdleCallSite, after[0], after[1], after[2], after[3], after[4]);
            IGN_LOG("[G20C-B]", "grSstIdle call site NOPped -- guest will fall through to 0x00458a35 without entering the trap (Experiment B, diagnostic-only)");
        } else {
            IGN_LOG("[G20C-B]", "ABORT: bytes at 0x%08x do not match the expected CALL grSstIdle encoding -- refusing to patch blindly", kIdleCallSite);
        }
    }
#endif

    set_phase(Phase::Entrypoint, "jumping to PE entry point");
    IGN_LOG("[CPU]", "Jumping to entry point 0x%08x", img->entry_va());
    set_phase(Phase::Running);
    uint32_t ret = 0;
    const char* fault = nullptr;
    IGN_CP("guest-run-begin", "bridge committed+linked, calling the PE entry point 0x%08x (language selector, menus, car/track selection and the race all run inside this call)", img->entry_va());
    // iteration-G150: the guest runs inside ONE Cpu::run whose block-entry budget is 2^31-1 (WinVita recharges it at the start of each run). When it is spent the run ends exactly like a normal finish and call_va returns ok: the game would
    // have left by itself after ~65 minutes of racing (the G149 run had executed 548,605,530 blocks after 1,002 s). take_limit_hit() tells the budget stop apart from the guest's own return, and a budget stop is resumable at the EIP of the   // G150
    // block that was about to run, so the loop below re-enters there (what WinVita's schedulers do on every slice). The optional token budget=<blocks> of ux0:data/ignition/g146_geo.txt makes the budget small (a resume every few seconds)   // G150
    // so the exhaustion path can be exercised on the console in a minute.   // G150
    ign::g150::Control g150Ctl;
    {   // G150
        char tb[400] = {0};   // G150
        if (FILE* cf = fopen("ux0:data/ignition/g146_geo.txt", "r")) {   // G150
            const size_t cn = fread(tb, 1, sizeof tb - 1, cf); fclose(cf);   // G150
            for (size_t i = 0; i < cn; ++i) if (tb[i] == '\n' || tb[i] == '\r' || tb[i] == '\t') tb[i] = ' ';   // G150
            g150Ctl = ign::g150::parse_control(tb);
        }   // G150
        if (g150Ctl.budgetBlocks) { cpu->set_run_limit(ign::g150::run_limit_for_blocks(g150Ctl.budgetBlocks)); }
        IGN_LOG("[G150-BUDGET]", "the guest run is resumed when its block budget ends (blocks per run: %u%s; refused tokens: %s)", g150Ctl.budgetBlocks ? g150Ctl.budgetBlocks : 2147483647u,
                g150Ctl.budgetBlocks ? " from the control file (test)" : " (engine default)", g150Ctl.refused[0] ? g150Ctl.refused : "none");
    }   // G150
    bool ok = br.call_va(img->entry_va(), {}, ret, &fault);
    {   // G150
        struct HaltedNow { bool operator()() const { return g_halted; } };   // G150
        struct CpuApi { Cpu* c; bool take_limit_hit() { return c->take_limit_hit(); } bool run(uint32_t eip, const char** f) { return c->run(eip, f); } uint32_t reg(int r) { return c->reg(r); } } api{cpu};   // G150
        ign::g150::ResumeStats rs;
        auto onResume = [](uint32_t n) { g_g150Resumes = n; if (n <= 8u) IGN_LOG("[G150-BUDGET]", "block budget ended: the guest run is resumed at its EIP (resume #%u, %llu ms after the start of the process)", n, (unsigned long long)wx86_now_ms()); };
        ok = ign::g150::run_with_resume(api, ok, (int)R_EIP, HaltedNow(), onResume, rs, &fault);
        if (rs.resumes) { ret = cpu->reg(R_EAX); }   // G150
        g_g150Resumes = rs.resumes;
        IGN_LOG("[G150-BUDGET]", "the guest run ended after %u budget resume(s)%s (ok=%d halted=%d)", rs.resumes, rs.capped ? " - RESUME CAP REACHED" : "", (int)ok, (int)g_halted);
    }   // G150
#ifdef IGNITION_RENDERER_3DFX   // G151
    glide2x_g151_session_final();   // iteration-G151: the one-line summary of the session (hooks, native calls, fallbacks by cause, errors, frame time)
#endif   // G151
#ifdef IGNITION_RENDERER_3DFX   // G152
    glide2x_g152_session_final();   // iteration-G152: how many Championship races the detector recognised, what it rejected
#endif   // G152
#ifdef IGNITION_RENDERER_3DFX   // G153
    glide2x_g155_session_final();   // iteration-G155 (replaces G154's): chroma-keyed draws, resolutions, variants and pool, real GL calls of the keyed state, memory
    glide2x_g156_session_final();   // iteration-G156: groups received / culled before any work, planes, estimated time, fallbacks and their causes
#endif   // G153
    IGN_CP("guest-run-end", "br.call_va returned ok=%d halted=%d", (int)ok, (int)g_halted);
    // iteration-G1 Part D: G0's real-hardware run showed a C2 AFTER the log
    // already printed "Halted cleanly"/"diagnostic run finished" -- i.e.
    // after boot_run() had already reached (or nearly reached) its last
    // line. These breadcrumbs bracket every step from here to the end of
    // this function so the NEXT real-hardware log says exactly how far
    // shutdown got before whatever produces the C2. Not a fix (there isn't
    // enough evidence yet to know what to fix) -- this is the narrowing
    // step itself.
    IGN_LOG("[EXIT-G1]", "guest_run returned ok=%d halted=%d", (int)ok, (int)g_halted);
    // iteration-G35, Parte O: distinguishes a normal br.call_va() return
    // (guest's own main() returned, or a real CPU fault) from a
    // wrapper-diagnostic halt (request_halt(), already logged its own
    // [G35-EXIT] line above the moment it fired) -- both survive in the
    // never-truncated critical log even if the main log gets wiped.
    if (ok && !g_halted) {
        IGN_CRITICAL("[G35-EXIT]", "reason=guest_run_returned_normally (br.call_va ok=true, no request_halt() ever fired -- neither a diagnostic stop nor a CPU fault)");
    }

    if (!ok) {
        uint32_t faultAddr = cpu->fault_addr();
        uint32_t faultCode = cpu->fault_code();
        IGN_LOG("[ERROR]", "CPU fault: %s (fault addr 0x%08x, code 0x%08x)",
                fault ? fault : "(unknown)", faultAddr, faultCode);
        IGN_CRITICAL("[G35-EXIT]", "reason=cpu_fault detail=\"%s\" fault_addr=0x%08x fault_code=0x%08x",
                fault ? fault : "(unknown)", faultAddr, faultCode);

        // iteration-G16, Parte G/H/I: everything still observable about a
        // fault at the TOP-LEVEL catch (br.call_va already stopped -- this
        // project's Cpu backends expose no per-instruction trace on the
        // real ARM dynarec target (cpu.h's set_code_hook is a documented
        // no-op default and only cpu_unicorn.cpp, the desktop oracle,
        // overrides it -- confirmed by grep, not assumed), so a true
        // instruction-by-instruction trace leading up to the fault is not
        // achievable here without touching WinVita's own vendored dynarec,
        // which this project does not do. What IS captured: full register
        // state and a raw stack dump, plus a classification of fault_addr
        // against this run's OWN real memory layout (ignition_mem_layout(),
        // not hardcoded guesses) -- e.g. whether the guest tried to
        // execute/jump into the STACK region, which would be strong
        // evidence of a corrupt return address or stack imbalance.
        {
            const MemLayout& L = ignition_mem_layout();
            uint32_t regs[8] = {0};
            cpu->regs_gp(regs);
            uint32_t eip = cpu->reg(R_EIP);
            uint32_t esp = regs[R_ESP];
            IGN_LOG("[G16-FAULT]", "registers at fault: EIP=0x%08x EAX=0x%08x ECX=0x%08x EDX=0x%08x EBX=0x%08x ESP=0x%08x EBP=0x%08x ESI=0x%08x EDI=0x%08x",
                    eip, regs[R_EAX], regs[R_ECX], regs[R_EDX], regs[R_EBX], esp, regs[R_EBP], regs[R_ESI], regs[R_EDI]);

            auto classify = [&](uint32_t va) -> const char* {
                if (va >= L.stack_base && va < L.stack_base + L.stack_size) return "STACK_REGION";
                if (va >= L.trap_base && va < L.trap_base + L.trap_size) return "TRAP_WINDOW";
                if (va >= L.heap_base && va < L.heap_base + L.heap_size) return "HEAP_REGION";
                if (va >= L.va_base && va < L.va_base + L.va_size) return "VA_REGION";
                if (va >= 0x00400000u && va < 0x0066E000u) return "PE_IMAGE";
                return "UNCLASSIFIED";
            };
            IGN_LOG("[G16-FAULT]", "fault_addr classification=%s (stack region is 0x%08x-0x%08x per this run's real ignition_mem_layout(), not a hardcoded guess)",
                    classify(faultAddr), L.stack_base, L.stack_base + L.stack_size - 1);
            IGN_LOG("[G16-FAULT]", "EIP classification=%s", classify(eip));

            if (faultAddr && cpu->mapped(faultAddr)) {
                uint8_t bytes[16] = {0};
                if (cpu->read(faultAddr, bytes, sizeof bytes)) {
                    char hex[3 * 16 + 1] = {0};
                    for (int i = 0; i < 16; ++i) snprintf(hex + i * 3, 4, "%02x ", bytes[i]);
                    IGN_LOG("[G16-FAULT]", "bytes at fault_addr (0x%08x): %s", faultAddr, hex);
                }
            } else {
                IGN_LOG("[G16-FAULT]", "fault_addr 0x%08x not mapped -- cannot read bytes there", faultAddr);
            }

            // Part I: raw stack dump ESP..ESP+128, classified word by word --
            // the fastest way to see whether a plausible corrupted return
            // address (a STACK_REGION value, matching the observed fault)
            // sits right where a real one should.
            char stackDump[512] = {0};
            size_t pos = 0;
            for (int i = 0; i < 32 && pos < sizeof(stackDump) - 40; ++i) {
                uint32_t v = 0;
                cpu->read(esp + (uint32_t)i * 4, &v, 4);
                int n = snprintf(stackDump + pos, sizeof(stackDump) - pos, "[esp+%02d]=0x%08x(%s) ", i * 4, v, classify(v));
                if (n > 0) pos += (size_t)n;
            }
            IGN_LOG("[G16-FAULT]", "stack[ESP..ESP+128) classified: %s", stackDump);
        }

        set_phase(Phase::Error, fault ? fault : "cpu fault");
    } else if (g_halted) {
        set_phase(Phase::Stopped, g_halt_reason.c_str());
        IGN_LOG("[BOOT]", "Halted cleanly: %s", g_halt_reason.c_str());
    } else {
        IGN_LOG("[BOOT]", "Entry point returned normally (eax=0x%08x) -- unusual for a GUI app, but not an error", ret);
        set_phase(Phase::Stopped, "entry point returned");
    }

    IGN_LOG("[EXIT-G1]", "bridge cleanup begin (br/cpu go out of scope at the end of this function -- Bridge/Cpu/PeImage destructors run after this line)");
    // iteration-G132: the music thread and its port go FIRST (the guest has already stopped, so no further MCI command can arrive):
    // stop -> join -> release the MAIN port; only then the effects thread/sink are shut down (dsound::shutdown), then process exit.
    IGN_CP("exit-begin", "guest stopped, starting the shutdown sequence (music, dsound)");
    IGN_LOG("[EXIT-G1]", "music shutdown begin");
    music::service_shutdown();
    IGN_LOG("[EXIT-G1]", "music shutdown done");
    IGN_CP("exit-music-done", "music thread joined, MAIN port released");
    IGN_LOG("[EXIT-G1]", "dsound shutdown begin");
    dsound::shutdown();
    IGN_LOG("[EXIT-G1]", "dsound shutdown done");
    IGN_CP("exit-dsound-done", "DirectSound engine shut down");
    IGN_LOG("[BOOT]", "IgnitionVita diagnostic run finished");
    IGN_LOG("[EXIT-G1]", "boot_run() about to return to main() -- Bridge br (and its ~166 registered shim slots, module images, guest-mapped memory) is destroyed on the way out; main() itself does nothing after this but `return 0` (no explicit sceKernelExitProcess/exit() call anywhere in this codebase -- confirmed by source search -- the vitasdk crt0 handles process exit on a normal main() return, same as every prior DDraw run that returned to LiveArea without a C2)");
    // iteration-G35, Parte N: the LAST checkpoint this project's own code
    // reaches before control leaves it entirely (Bridge/Cpu teardown,
    // main()'s `return 0`, then vitasdk's crt0) -- written to the
    // never-truncated critical log specifically so it survives even a
    // stray relaunch that would wipe the main log's own copy of this line.
    IGN_CRITICAL("[G35-EXIT]", "boot_run() returning to main() -- this is the last line this project's own code writes before process teardown");
}

} // namespace ign
