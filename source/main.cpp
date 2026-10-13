// source/main.cpp — deliberately tiny. All real work is in
// runtime/ignition_boot.cpp; this file only exists so main() has somewhere
// to live and the Vita app has a memory budget generous enough for a PE
// image, its guest heap/VA arenas, and the dynarec's JIT pool.
#include "runtime/ignition_boot.h"
#include "diagnostics/log.h"

#ifdef IGNITION_RENDERER_3DFX
#include "win32/glide2x_ign.h"
#endif
#ifdef IGNITION_RENDERER_3DFX   // G160
#include "runtime/g160_clock_runtime.h"   // G160: the default clock profile (minimum ARM 444 / BUS 222 / GPU 222 / XBAR 166), applied once here and restored once below
#include "runtime/g161_pacer.h"           // G161: the 30 FPS limiter summary
#endif   // G160

#ifdef IGNITION_RENDERER_3DFX
// iteration-G2 Part C: G1's real-hardware run showed a C2 that appears
// AFTER the log already reached "main() returning 0" -- i.e. after this
// project's own code has finished. Guarded strictly to the experimental
// 3dfx build (never the stable DDraw branch): instead of a normal `return
// 0` (which hands off to vitasdk's own crt0 exit path), explicitly call
// the real, header-declared Vita API for ending a process
// (psp2/kernel/processmgr.h: `int sceKernelExitProcess(int res);`,
// confirmed present and backed by libSceProcessmgr_stub.a on this
// toolchain -- verified this iteration, not assumed). If the C2
// disappears, the normal-return crt0 path is implicated for this specific
// build; if it persists, the C2 happens before the process can exit
// cleanly by any method, which is a different, narrower problem.
#include <psp2/kernel/processmgr.h>
#endif

extern "C" {
// VitaSDK convention: this exact symbol name/type sizes the newlib heap
// used for HOST-side allocations only (std::vector/std::string/std::map
// inside Bridge and PeImage -- a few hundred KB at most for ~114 imports
// and the ~900 KB Ign_d3d.exe). Kept small on purpose: the guest memory
// arena (runtime/ignition_memlayout.h, WX86_ARENA=160 MiB) and the dynarec's
// JIT pool (up to 32 MiB) are SEPARATE sceKernelAllocMemBlock allocations
// competing for the same real budget, and this is the one of the three that
// genuinely doesn't need to be generous.
unsigned int sceLibcHeapSize = 16 * 1024 * 1024;
}

// Mandatory per platform/vita_host.h: the engine's own internal progress log
// (JIT pool reservation, VM domain open/close, memblock failures) links to
// this symbol UNCONDITIONALLY and fires before our own diagnostics/log.h is
// even initialized. Kept separate from ignitionvita.log on purpose: if OUR
// logger has a bug, this one still captures whatever the engine itself saw
// on the way down.
//
// Deliberately NOT inside an `extern "C" { ... }` block: a `const` object
// declared that way keeps C++'s default INTERNAL linkage (the braces only
// set language linkage, not storage class), so vita_host.cpp's `extern`
// reference to it would fail at link time. The single-line form below is
// the one WinVita's own header comment documents for exactly this reason.
//
// G134: the path moved into logs/ (the SAME literal-const form as before; no writable buffer). The folder is created by the first
// statement of main() below, i.e. before WinVita runs anything. In the (unexpected) fallback case where the folder cannot be created
// this file simply cannot be written; ignitionvita.log / ignition_critical.log fall back to the legacy folder explicitly in
// ign::logs_prepare() and say so in the first checkpoint.
extern "C" const char* const wx86_vita_progress_path = "ux0:data/ignition/logs/boot.txt";

int main() {
    // iteration-G134: the very first action. Nothing runs before main() (no static constructors in the logger); this creates
    // ux0:data/ignition/logs/ (an existing folder is fine) and rotates the previous generation, THEN the first checkpoints are written.
    const bool logsInFolder = ign::logs_prepare();
    IGN_CP("main-start", "IgnitionVita G134_LOGGER_CRASH_RECOVERY base=G132_COMPLETE_STABLE_BASE (first statement of main(); the folder had to exist before anything could be logged)");
    {
        char d[300];
        ign::logs_describe(d, sizeof d);
        IGN_CP("logs-ready", "%s ok=%d", d, logsInFolder ? 1 : 0);
    }
#if defined(IGNITION_RENDERER_3DFX) && defined(G4B_VITAGL_SELFTEST)
    // iteration-G4B/G4C, Parte C/I: while vitaGL's context is unverified,
    // this build does NOT run Ignition at all -- it only answers "does
    // vitaGL produce a real, working context on this VPK, from the real
    // main thread, standalone?" (see glide2x_ign.cpp's
    // glide2x_g4b_vitagl_selftest for the full rationale, including G4C's
    // correction of G4B's return-code interpretation). Toggle the
    // G4B_VITAGL_SELFTEST CMake option off to get the normal guest-running
    // 3dfx build back.
    ign::log_init();   // normally done by boot_run() -- this path never calls it
    IGN_LOG("[BUILD]", "IgnitionVita iteration=35");
    IGN_LOG("[BUILD]", "renderer=3DFX (experimental branch, ux0:data/ignition/Ign_3dfx.exe)");
    IGN_LOG("[BUILD]", "glide_bringup=G4C");
    IGN_LOG("[BUILD]", "grGlideInit=IMPLEMENTED");
    IGN_LOG("[BUILD]", "grSstQueryHardware=IMPLEMENTED");
    IGN_LOG("[BUILD]", "grSstSelect=IMPLEMENTED");
    IGN_LOG("[BUILD]", "grSstWinOpen=WAITING_FOR_VERIFIED_VGL_CONTEXT (not exercised this build -- guest is not run while G4B_VITAGL_SELFTEST=ON)");
    IGN_LOG("[BUILD]", "explicit_vita_exit=YES");
    IGN_LOG("[BUILD]", "ddraw_branch=UNCHANGED");
    // vitagl_return_semantics_verified/vitagl_selftest are logged from
    // glide2x_g4b_vitagl_selftest() itself, right before init begin.
    bool selftestOk = ign::glide2x_g4b_vitagl_selftest();
    IGN_LOG("[EXIT-G4B]", "selftest result=%s -- guest NOT run this build (G4B_VITAGL_SELFTEST=ON)", selftestOk ? "PASS" : "FAIL");
    sceKernelExitProcess(0);
    return 0;
#endif
    // iteration-G4E, Parte A/M/N: G4D pre-initialized vitaGL right here,
    // before boot_run() -- and G4D's real-hardware run showed the log
    // dying right after WINVITA_INIT, before "Creating ARM dynarec CPU
    // backend" completed. That preinit call is REMOVED (not just
    // disabled) this iteration to test whether it was interfering with
    // CpuBox86's construction; vitaGL now only ever gets initialized from
    // inside grSstWinOpen_impl (glide2x_ign.cpp), once Ignition itself
    // actually asks for a graphics context -- see runtime/ignition_boot.cpp's
    // [CPU-G4E] breadcrumbs for the before/after-constructor evidence this
    // iteration gathers either way.
#ifdef IGNITION_RENDERER_3DFX   // G160
    ign::g160_apply_clocks();   // G160: once, after the log folder exists and before vitaGL / GXM / the guest are initialised (policy and log line: win32/g160_clocks.h)
#endif   // G160
    ign::boot_run();
    IGN_CP("boot-run-returned", "boot_run() returned to main()");
#ifdef IGNITION_RENDERER_3DFX   // G160
    ign::g161_pacer_summary();    // G161: [RELEASE-FPS-SUMMARY], once (idempotent)
    ign::g160_restore_clocks();   // G160: once (idempotent), the components the port changed and nobody else touched since, before the exit sequence below
#endif   // G160
#ifdef IGNITION_RENDERER_3DFX
    // iteration-G117: breadcrumbs that survive the log allowlist -- proof that
    // boot_run() returned to main() and that the pre-existing exit call below
    // (unchanged) is about to run.
    IGN_LOG("[G117-EXIT]", "main(): boot_run() returned -- calling the existing sceKernelExitProcess(0) now");
    IGN_CRITICAL("[G117-EXIT]", "main(): boot_run() returned -- calling the existing sceKernelExitProcess(0) now");
    IGN_LOG("[EXIT-G2]", "invoking explicit Vita process exit now (sceKernelExitProcess(0), diagnostic test for the post-STOP C2)");
    IGN_CP("process-exit", "normal exit: calling sceKernelExitProcess(0) now");
    sceKernelExitProcess(0);
    // Not expected to reach here -- sceKernelExitProcess ends the process.
    // If this line DOES log, exitProcess itself returned instead of
    // exiting, which is its own finding.
    IGN_LOG("[EXIT-G2]", "sceKernelExitProcess returned instead of exiting -- unexpected");
    return 0;
#else
    // iteration-G1 Part D: last breadcrumb before the vitasdk crt0 takes
    // over process exit on this normal `return`. Unchanged for the stable
    // DDraw branch -- the explicit-exit experiment above is 3dfx-only.
    IGN_LOG("[EXIT-G1]", "main() returning 0");
    IGN_CP("process-exit", "normal exit: main() returning 0");
    return 0;
#endif
}
