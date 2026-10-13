// Console services for the Vita (see vita_host.h).
#include "platform/vita_host.h"

// This entire file only exists on-target: off console there are no cores to
// place threads on, no Sony clock, and no durable log to maintain. Same
// convention as platform/vita_audio.cpp.
#ifdef __vita__

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>

// ===========================================================================
//  DURABLE PROGRESS LOG
// ===========================================================================
// Static lock with a constant initializer, so no lazy init and no
// __cxa_guard_acquire — the class of trap the build script's `nm` guard
// exists to catch.
static pthread_mutex_t g_progress_mx = PTHREAD_MUTEX_INITIALIZER;

void wx86_vita_progress(const char* msg) {
    // No path means no log. Guessing one would let two ports write to the
    // same file, exactly what the lock below exists to prevent. This only
    // happens if a port omitted the definition, in which case the link
    // fails — the right moment to find out.
    if (!wx86_vita_progress_path) return;
    pthread_mutex_lock(&g_progress_mx);
    FILE* f = std::fopen(wx86_vita_progress_path, "a");
    if (f) {
        unsigned s = (unsigned)(sceKernelGetProcessTimeWide() / 1000000ull);
        std::fprintf(f, "[%4u.%02us] ", s, 0u);
        std::fputs(msg, f); std::fputc('\n', f); std::fclose(f);
    }
    pthread_mutex_unlock(&g_progress_mx);
}

// C-linkage alias so the dynarec's C code (weak reference) can reach it.
extern "C" void wx86_vita_progress_c(const char* m) { wx86_vita_progress(m); }

// ===========================================================================
//  REAL SLEEP
// ===========================================================================
// The monotonic clock belongs in runtime/host_clock.h since it's meaningful
// off console too; keeping a copy here would duplicate it in the engine.
void wx86_vita_sleep_ms(uint32_t ms) { if (ms) sceKernelDelayThread(ms * 1000u); }

// ===========================================================================
//  HOST THREAD PLACEMENT ACROSS USER CORES
// ===========================================================================
namespace {
struct CoreEnt { char nom[14]; int uid; unsigned wanted; int rc; };
// Sized with headroom above what a single consumer typically registers — a
// thread that isn't registered disappears from the one line that shows which
// thread lives on which core.
CoreEnt g_core[16];
int     g_coreN = 0;

// Read once. Default is "222".
const char* core_scheme() {
    static const char* s_sch = nullptr;
    static bool s_done = false;
    if (!s_done) {
        s_done = true;
        const char* e = getenv("WX86_COEURS");
        if (!e) e = getenv("D2_COEURS");
        bool ok = e && e[0] && e[1] && e[2] && !e[3];
        for (int i = 0; ok && i < 3; ++i) if (e[i] < '0' || e[i] > '3') ok = false;
        if (e && e[0] && !ok) {
            char m[120];
            std::snprintf(m, sizeof m,
                "coeurs: WX86_COEURS=\"%s\" INVALIDE (3 chiffres 0..3 attendus) — repartition par defaut 222", e);
            wx86_vita_progress(m);
        }
        s_sch = ok ? e : "222";
    }
    return s_sch;
}
} // namespace

int wx86_vita_core_mask(int who) {
    if (who < 0 || who > 2) return SCE_KERNEL_CPU_MASK_USER_2;
    const char c = core_scheme()[who];
    // Heartbeat on USER_0 is accepted but flagged: this kernel is RUN-TO-BLOCK,
    // so an anti-starvation heartbeat sharing a core with guest runner threads
    // won't get scheduled once a runner stops blocking — exactly when it's
    // needed. A silently inert safety net is the worst failure mode here.
    if (who == 2 && c == '0') {
        static bool s_said = false;
        if (!s_said) { s_said = true; wx86_vita_progress(
            "coeurs: ATTENTION battement anti-famine demande sur USER_0 — sous RUN-TO-BLOCK"
            " le filet ne sera PAS elu quand un runner invite cesse de bloquer (filet inerte)"); }
    }
    // Digit `3` requests the fourth core (0x00080000). If the kernel refuses
    // it, the pin's rc is published in the "cores:" line and the thread stays
    // where it was. Don't rely on it until that line's probe reports rc0 on
    // hardware.
    return c == '0' ? SCE_KERNEL_CPU_MASK_USER_0
         : c == '1' ? SCE_KERNEL_CPU_MASK_USER_1
         : c == '3' ? WX86_CPU_MASK_USER_3
                    : SCE_KERNEL_CPU_MASK_USER_2;
}

void wx86_vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc) {
    if (g_coreN >= (int)(sizeof g_core / sizeof g_core[0])) return;
    CoreEnt& e = g_core[g_coreN++];
    std::snprintf(e.nom, sizeof e.nom, "%s", nom ? nom : "?");
    e.uid = uid; e.wanted = wanted; e.rc = pin_rc;
}

// Number of host threads registered so far. Used as context before creating
// a thread: an exhausted thread quota and exhausted memory can't be told
// apart from sceKernelCreateThread's rc alone.
int wx86_vita_core_count() { return g_coreN; }

extern "C" int wx86_vita_pin_self(int mask, unsigned* relu) {
    const SceUID me = sceKernelGetThreadId();
    const int rc = sceKernelChangeThreadCpuAffinityMask(me, mask);
    const int r  = sceKernelGetThreadCpuAffinityMask(me);
    if (relu) *relu = (r < 0) ? 0u : (unsigned)r;
    return rc;
}

extern "C" int wx86_vita_pin_thread(int thread_uid, int mask, unsigned* relu) {
    const int rc = sceKernelChangeThreadCpuAffinityMask((SceUID)thread_uid, mask);
    // Read back only if the caller asked for it — see vita_host.h.
    if (relu) { const int r = sceKernelGetThreadCpuAffinityMask((SceUID)thread_uid);
                *relu = (r < 0) ? 0u : (unsigned)r; }
    return rc;
}

namespace {
// Fourth-core probe, run once on the first 10-second window.
//
// It creates a thread but never starts it, so it only measures what the
// kernel ACCEPTS (rc of ChangeThreadCpuAffinityMask + mask read back), never
// whether a core actually executes something. A thread started on a
// nonexistent core would never get scheduled, and cleaning it up would need
// a bounded WaitThreadEnd plus an abandon — a needless risk for a question
// the rc already answers. `activeCpuMask` is read for reference, since it's
// the kernel's own count of active cores.
//
// Three masks are tried: USER_2 (a witness mask known to be accepted —
// without it, an `rc<0` on 0x80000 wouldn't be distinguishable from a broken
// probe), 0x00080000 (the 4th core alone), and 0x000F0000 (all four).
void core_probe_cpu3() {
    SceKernelSystemInfo si; std::memset(&si, 0, sizeof si); si.size = sizeof si;
    const int rsys = sceKernelGetSystemInfo(&si);
    SceUID th = sceKernelCreateThread("probe_c3", [](SceSize, void*) -> int { return 0; },
                                      0x10000100, 0x1000, 0, 0, nullptr);
    if (th < 0) {
        char m[112];
        std::snprintf(m, sizeof m,
            "coeurs: sonde 4e coeur — CreateThread KO (rc=0x%08x), activeCpuMask=0x%08x (rc=0x%08x)",
            (unsigned)th, rsys < 0 ? 0u : (unsigned)si.activeCpuMask, (unsigned)rsys);
        wx86_vita_progress(m);
        return;
    }
    const unsigned masks[3] = { SCE_KERNEL_CPU_MASK_USER_2, WX86_CPU_MASK_USER_3, 0x000F0000u };
    int  rc[3]; unsigned relu[3];
    for (int i = 0; i < 3; ++i) {
        rc[i]   = sceKernelChangeThreadCpuAffinityMask(th, (int)masks[i]);
        relu[i] = (unsigned)sceKernelGetThreadCpuAffinityMask(th);
    }
    sceKernelDeleteThread(th);          // never started, so Delete alone is enough
    char m[224];
    std::snprintf(m, sizeof m,
        "coeurs: sonde 4e coeur — activeCpuMask=0x%08x (rc=0x%08x) |"
        " temoin 0x40000 rc=0x%08x relu=0x%x | 0x80000 rc=0x%08x relu=0x%x |"
        " 0xF0000 rc=0x%08x relu=0x%x",
        rsys < 0 ? 0u : (unsigned)si.activeCpuMask, (unsigned)rsys,
        (unsigned)rc[0], relu[0], (unsigned)rc[1], relu[1], (unsigned)rc[2], relu[2]);
    wx86_vita_progress(m);
    // A mask that reads back as 0 proves nothing on this firmware — only the
    // rc distinguishes accepted from refused, and the 0x40000 witness confirms
    // whether the probe itself works.
    if (rc[0] < 0)
        wx86_vita_progress("coeurs: sonde 4e coeur NON CONCLUANTE — le temoin USER_2 lui-meme est refuse");
    else if (rc[1] >= 0)
        wx86_vita_progress("coeurs: 4e coeur (0x80000) ACCEPTE par le noyau — reste a prouver qu'un fil y COURT"
                           " (chiffre 3 de WX86_COEURS, puis lire le champ d= de la ligne coeurs:)");
    else
        wx86_vita_progress("coeurs: 4e coeur (0x80000) REFUSE par le noyau — trois coeurs user, pas quatre");
}
} // namespace

// Published for each host thread:
//   v=<n>   requested core (0/1/2/3), derived from the mask
//   rc      sceKernelChangeThreadCpuAffinityMask's rc (0 = ok)
//   m=<hex> mask read back by sceKernelGetThreadCpuAffinityMask
//   c=<n>   currentCpuId (sceKernelGetThreadInfo)
//   d=<n>   lastExecutedCpuId — the field that actually settles it
//
// Caveat: on this firmware, sceKernelGetThreadCpuAffinityMask reads back
// 0x00000000 even for a thread pinned successfully. So "m=0x0" is not proof
// of failure — the line also publishes the request's rc and, more
// importantly, lastExecutedCpuId, which says which physical core the thread
// actually ran on.
void wx86_vita_core_window_line() {
    if (!g_coreN) return;
    static bool s_probed = false;
    if (!s_probed) { s_probed = true; core_probe_cpu3(); }
    char buf[224]; int used = std::snprintf(buf, sizeof buf, "coeurs:");
    int inline_n = 0;
    for (int i = 0; i < g_coreN; ++i) {
        const CoreEnt& e = g_core[i];
        int want = (e.wanted & SCE_KERNEL_CPU_MASK_USER_0) ? 0
                 : (e.wanted & SCE_KERNEL_CPU_MASK_USER_1) ? 1
                 : (e.wanted & SCE_KERNEL_CPU_MASK_USER_2) ? 2
                 : (e.wanted & WX86_CPU_MASK_USER_3)       ? 3 : -1;
        int relu = (e.uid > 0) ? sceKernelGetThreadCpuAffinityMask(e.uid) : 0;
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        int rti = (e.uid > 0) ? sceKernelGetThreadInfo(e.uid, &ti) : -1;
        char one[96];
        std::snprintf(one, sizeof one, " %s=v%d/rc%d/m0x%x/c%d/d%d",
                      e.nom, want, e.rc < 0 ? -1 : 0, (unsigned)relu,
                      rti < 0 ? -1 : (int)ti.currentCpuId,
                      rti < 0 ? -1 : (int)ti.lastExecutedCpuId);
        const int need = (int)std::strlen(one);
        if (used + need >= (int)sizeof buf - 1 || inline_n >= 3) {
            wx86_vita_progress(buf);
            used = std::snprintf(buf, sizeof buf, "coeurs:"); inline_n = 0;
        }
        std::memcpy(buf + used, one, (size_t)need + 1); used += need; ++inline_n;
    }
    if (used > 7) wx86_vita_progress(buf);
}

#else  // ---------------------------------------------------------------------
//  OFF CONSOLE: the log exists but does nothing
// ---------------------------------------------------------------------------
// The engine's generic body (bridge, cpu_box86, both schedulers, the memory
// mapper) logs its progress, and that body also compiles for the qemu/desktop
// harness, where there is no console and no durable file to maintain.
//
// These two no-ops stand in for all of the portability needed: callers link
// strongly with no null check, and off console nothing happens.
//
// wx86_vita_progress_path is deliberately not read here, so a port has
// nothing to define for its desktop build to link.

void wx86_vita_progress(const char*) {}
extern "C" void wx86_vita_progress_c(const char*) {}

#endif // __vita__
