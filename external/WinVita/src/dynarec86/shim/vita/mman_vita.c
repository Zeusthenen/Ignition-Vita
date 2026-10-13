/* src/dynarec86/shim/vita/mman_vita.c — mmap façade over Vita memblocks.
 * See sys/mman.h in this directory for the contract.
 *
 * PROT_EXEC requests come only from Box86's AllocDynarecMap (RWX chunks for
 * emitted ARM code): served from the VM domain (sceKernelAllocMemBlockForVM),
 * the one mechanism homebrew has for runnable generated code. The domain is
 * opened once and kept open — Box86 interleaves emission and execution, and
 * per-block Open/Close would serialize every translation.  VM sizes are
 * rounded to 1 MiB (hardware alloc granularity for the VM type); plain RW
 * requests round to 4 KiB USER_RW blocks.
 */
#include "sys/mman.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv : knob D2_VMBRACKET */
#include <string.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>

/* Registry of live blocks: munmap frees whole blocks by exact base (that is
 * the only pattern custommem.c uses), and the cache-sync needs base+uid. */
#define DYN86_MAXBLK 256
/* Engine log sink (platform/vita_host.h). Declared HERE rather than
 * included: this unit is C, the header is C++. STRONG reference -- the
 * symbol is defined by the same library under the same target guard. A weak
 * reference tied to one consumer's name would silently do nothing for a
 * differently-named port, and a failed allocation MUST reach the log: there
 * is no stderr on hardware, and a silent MAP_FAILED just looks like the app
 * closing at launch. */
void wx86_vita_progress_c(const char* msg);
/* `pool`: sub-block of the JIT pool. `uid` stays the POOL's uid -- VM-domain
 * sync (dyn86_vita_clear_cache) needs it, and a placeholder uid there makes
 * the sync fail silently: emitted code never becomes executable and boot
 * dies right after the pool is reserved. */
typedef struct { void* base; size_t size; SceUID uid; int vm; int pool; } Blk;
static Blk g_blk[DYN86_MAXBLK];
/* Registry lock: mmap() is reached under TWO different upstream locks --
 * customMalloc holds mutex_blocks (RW blocks), AllocDynarecMap holds
 * mutex_dynarec (VM blocks) -- so two threads can be in here CONCURRENTLY.
 * The slot scan + insert (and munmap's find + clear) mutate g_blk with no
 * lock of their own: a slot collision unregisters a live block, and a block
 * missing from the registry makes dyn86_vita_clear_cache return WITHOUT
 * syncing = stale icache for freshly emitted code. qemu never sees this
 * (real kernel mmap, no registry). Leaf lock, cold paths (chunk creation,
 * per-translation cache sync). pte handles the lazy-init sentinel. */
static pthread_mutex_t g_blk_mx = PTHREAD_MUTEX_INITIALIZER;

/* --- VM domain: PER-THREAD open ---
 * sceKernelOpenVMDomain grants VM-domain write access via the DACR, which
 * lives in each thread's saved CPU context -- the open is effective only for
 * the CALLING thread. Opening it ONCE globally covers only whichever thread
 * performed the first VM mmap (the boot main). Under cooperative scheduling
 * a single thread ever writes JIT memory, so that was stable; under a native
 * scheduler, the first worker thread that translates code writes the arena
 * with a virgin DACR and takes a deterministic write-permission Data abort
 * in allocBlock -- not corruption, since the fault is the very first write
 * into an otherwise healthy, freshly mapped VM chunk. qemu (real kernel
 * mmap, no domain) and Vita3K (no DACR emulation) never exercise this path;
 * only real hardware does.
 * Fix: every thread that may WRITE JIT memory calls this before its first
 * write (mmap covers the allocating thread; the native scheduler calls it
 * at main+runner entry). Idempotent per thread; a failure is logged only
 * once per thread to avoid log spam from a repeat-open quirk. */
/* --- BRACKET Close+Open: DEFAULT ON, kill switch D2_VMBRACKET=0 ---------
 * The VitaSDK header doc comment for this function pair is backwards. Two
 * independent primary sources agree with each other, and against the header:
 *   - the reference HENkaku/vitasdk dynarec gist: Open = "set domain to be
 *     writable by user", Close = "set domain back to read-only";
 *   - vita-luajit (hyln9) src/lj_mcode.c, the only real published Vita JIT:
 *       mcode_setprot(..., MCPROT_RX)  -> sceKernelCloseVMDomain()
 *       mcode_setprot(..., MCPROT_RWX) -> sceKernelOpenVMDomain()
 * So Open = RWX (superset), Close = RX: "ldrb OK, then strb faulting at the
 * same byte" is exactly the signature of a closed domain. vita-luajit calls
 * the pair thousands of times -- toggling is the NOMINAL usage; "open once
 * and keep open" (this file's original approach) is the exception.
 *
 * Measured on real hardware: the DACR write permission is scoped to the
 * THREAD, not the process or the core, and Close only disarms its own
 * caller -- so bracketing Close+Open around a write opens no unsafe window
 * for other threads. Armed, this lets the native scheduler run stably;
 * without it, a translating worker thread dies shortly after start.
 *
 * Hence DEFAULT ON (D2_FAMINE convention, see tools/rt_boot.cpp: the
 * variable's ABSENCE means armed; an explicit '0' is the kill switch). The
 * original "open once" mode is kept only as an A/B kill switch on hardware:
 * D2_VMBRACKET=0. Both modes stamp themselves in boot_progress -- never a
 * silent mode change.
 * Neither qemu (no domain) nor Vita3K (HLE returns 0 for every thread)
 * exercise this knob: only real hardware can judge it, which is why this
 * file only compiles for __vita__.
 * A lock around the pair is mandatory: without it, two threads racing
 * (Close/Close/Open/Open) can leave the second Open refused and the first
 * thread unarmed. */
static pthread_mutex_t g_vmdom_mx = PTHREAD_MUTEX_INITIALIZER;

void dyn86_vita_open_vm_thread(void) {
    static __thread int t_vm_open = 0;
    if (t_vm_open) return;
    t_vm_open = 1;
    static int bracket = -1;   /* benign race: both threads compute the same value */
    /* Default ON: the variable's ABSENCE arms the bracket (D2_FAMINE
     * convention). Only an explicit '0' falls back to the original mode. */
    if (bracket < 0) { const char* e = getenv("WX86_VMBRACKET"); if (!e) e = getenv("D2_VMBRACKET"); bracket = (e && *e == '0') ? 0 : 1; }
    int rcC = 0, rc;
    if (bracket) {
        pthread_mutex_lock(&g_vmdom_mx);
        rcC = sceKernelCloseVMDomain();
        rc  = sceKernelOpenVMDomain();
        pthread_mutex_unlock(&g_vmdom_mx);
    } else {
        rc = sceKernelOpenVMDomain();
    }
    static int announced = 0;   /* one line for the console log, first thread only */
    if (!announced) {
        announced = 1;
        {
            /* Stamp BOTH outcomes (doctrine: never a silent mode change) --
             * the log always states which mode ran. Wording is conditional:
             * an rc<0 must not read as "armed" or "active". */
            char m[160];
            snprintf(m, sizeof m,
                     rc < 0 ? (bracket ? "vm-domain: BRACKET arme mais EN ECHEC (premier rc=0x%08x)"
                                       : "vm-domain: BRACKET DESACTIVE (D2_VMBRACKET=0) - mode legacy open-unique EN ECHEC (premier rc=0x%08x)")
                            : (bracket ? "vm-domain: BRACKET close+open arme (premier rc=0x%08x)"
                                       : "vm-domain: BRACKET DESACTIVE (D2_VMBRACKET=0) - mode legacy open-unique (premier rc=0x%08x)"), (unsigned)rc);
            wx86_vita_progress_c(m);
        }
    } else if (rc < 0) {
        /* a per-thread failure is exactly the crash we are fixing: say it */
        char m[160];
        if (bracket)
            snprintf(m, sizeof m, "vm-domain: BRACKET close=0x%08x puis open rc=0x%08x sur un fil ecrivain JIT",
                     (unsigned)rcC, (unsigned)rc);
        else
            snprintf(m, sizeof m, "vm-domain: [legacy D2_VMBRACKET=0] OpenVMDomain rc=0x%08x sur un fil ecrivain JIT",
                     (unsigned)rc);
        wx86_vita_progress_c(m);
    }
}
/* Live JIT (VM) and RW bytes held by this façade. Exposed (non-static) so the
 * watchdog heartbeat can chart the translation cache's real footprint: the JIT
 * shares the user budget with the guest arena, and starving it is a silent
 * perf collapse (fill_fail storm -> retranslation every visit). Any future
 * budget reshuffle must keep a floor >= the measured in-game peak + margin. */
unsigned int dyn86_jit_cur = 0;   /* bytes in VM (PROT_EXEC) blocks */
/* JIT POOL RESERVED AT STARTUP.
 * The JIT cache and the newlib heap used to draw from the SAME free-memory
 * reserve, each growing independently. Whichever asked last would lose --
 * and when it's the JIT, the guest thread DIES, since this build has no
 * interpreter fallback (shim_impl.c: Run() sets quit=1).
 * The pool is now reserved IN ONE SHOT on first need, then sub-allocated:
 *   - the heap can no longer starve the JIT, or vice versa;
 *   - an insufficient budget shows up AT STARTUP, not ten minutes in;
 *   - box86 never returns its chunks (FreeDynarecMap only frees WITHIN a
 *     chunk), so one bump allocation is exactly equivalent to the previous
 *     behavior, minus the race.
 * If the reservation fails, this falls back to the old block-by-block path:
 * no regression possible. */
unsigned int  dyn86_jitpool_size = 0, dyn86_jitpool_used = 0;   /* for the gauge, summed across segments */
unsigned int dyn86_rw_cur  = 0;   /* bytes in plain RW blocks */

/* JIT pool split into MULTIPLE 16 MiB segments. A single larger
 * sceKernelAllocMemBlockForVM request is refused by the kernel
 * (sce=0x80024B0B, SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW): 16 MiB is a KERNEL
 * CAP PER VM BLOCK, not a choice made by this project. So one larger block
 * doesn't exist as an option; several 16 MiB blocks do.
 * Safe by construction: every link between translated blocks is an INDIRECT
 * branch on a full 32-bit address (CreateJmpNext = LDR_literal+BX;
 * D2_CALLRET's ret_to_epilog/retn_to_epilog = BX after a check, or jump
 * table + BX) -- never a direct, range-limited B/BL. So the distance between
 * segments costs nothing: same instruction, same cost, whether the target is
 * next door or at the other end of the address space.
 *
 * WX86_JITPOOL_MB/D2_JITPOOL_MB = size of EACH segment (capped at 16; the
 * kernel refuses more anyway). WX86_JITPOOL_SEGS/D2_JITPOOL_SEGS = target
 * segment count (default 2, so 32 MiB of total pool). A segment is opened
 * LAZILY, only once the previous one is full -- never all at once at boot. */
#define JITPOOL_MAX_SEGS 8
typedef struct { void* base; size_t size, used; SceUID uid; } JitSeg;
static JitSeg    g_jitseg[JITPOOL_MAX_SEGS];
static int       g_jitseg_n = 0;          /* segments actually opened */
static unsigned  g_jitseg_cap_mb = 0;     /* target size per segment, 0 = not read yet */
static unsigned  g_jitseg_max = 0;        /* target segment count */
static int       g_jitseg_refused = 0;    /* a request failed: stop asking */

/* Tries to open ONE more segment. Returns 0 without touching the kernel if
 * the configured cap is already reached OR a previous attempt already
 * failed (no hammering the kernel on every new PROT_EXEC block request). */
static int jitpool_grow(void) {
    if (g_jitseg_refused) return 0;
    if (!g_jitseg_cap_mb) {
        const char* e = getenv("WX86_JITPOOL_MB"); if (!e) e = getenv("D2_JITPOOL_MB");
        unsigned mb = e ? (unsigned)atoi(e) : 16u;
        if (mb > 16u) mb = 16u;   /* kernel cap per VM block */
        g_jitseg_cap_mb = mb ? mb : 16u;
        const char* es = getenv("WX86_JITPOOL_SEGS"); if (!es) es = getenv("D2_JITPOOL_SEGS");
        unsigned segs = es ? (unsigned)atoi(es) : 2u;
        if (segs < 1) segs = 1;
        if (segs > JITPOOL_MAX_SEGS) segs = JITPOOL_MAX_SEGS;
        g_jitseg_max = segs;
    }
    if (g_jitseg_n >= (int)g_jitseg_max) return 0;
    /* Step the request down instead of giving up on the first refusal.
     * A full-size segment is refused as soon as the rest of the process has
     * eaten the budget, and the old code then set g_jitseg_refused and never
     * asked again -- for anything, at any size. The pool froze at whatever it
     * had, and since this build has no interpreter, the first block that could
     * not be translated killed the guest thread outright (crash signature
     * SMRD2J34ZXU2A55I: 52 of the 62 claims received from 0.1.6, at ~45 min of
     * play, after "JIT: segment 2/2 de 16 Mo REFUSE" and 130 refused 2 MiB
     * fallbacks). Taking the 4 MiB that ARE free beats taking nothing. */
    SceUID u = -1, last_rc = 0; void* pb = 0; size_t want = 0;
    for (unsigned mb = g_jitseg_cap_mb; mb >= 1u; mb >>= 1) {
        want = (size_t)mb << 20;
        u = sceKernelAllocMemBlockForVM("dyn86_jitpool", want);
        if (u >= 0 && sceKernelGetMemBlockBase(u, &pb) >= 0 && pb) break;
        last_rc = u;                     /* the kernel's own code, for the log */
        if (u >= 0) sceKernelFreeMemBlock(u);
        u = -1; pb = 0;
    }
    if (!pb) {
        g_jitseg_refused = 1;
        char m[208];
        snprintf(m, sizeof m,
            "JIT: segment %d/%u REFUSE de %u Mo jusqu'a 1 Mo (sce=0x%08x) — piscine figee a %u Mo,"
            " repli bloc-par-bloc au-dela",
            g_jitseg_n + 1, g_jitseg_max, g_jitseg_cap_mb, (unsigned)last_rc, dyn86_jitpool_size >> 20);
        wx86_vita_progress_c(m);
        return 0;
    }
    g_jitseg[g_jitseg_n].base = pb; g_jitseg[g_jitseg_n].size = want; g_jitseg[g_jitseg_n].used = 0;
    g_jitseg[g_jitseg_n].uid = u;
    ++g_jitseg_n;
    dyn86_jitpool_size += (unsigned int)want;
    /* Say what the pool IS and what it was asked to be. The old line read
     * "segment 1/2 de 16 Mo reserve (piscine totale 16 Mo)", which a reader
     * takes for 32 MiB of pool with the first half open -- while the second
     * half may well be impossible, as it was on every 0.1.6 console that
     * reported. The remaining segments are opened on demand, and a refusal
     * then is fatal to the guest thread, so the target is worth printing. */
    { char m[208];
        snprintf(m, sizeof m,
            "JIT: segment %d/%u de %u Mo reserve (piscine %u Mo ; cible %u Mo, le reste ouvert a la demande)",
            g_jitseg_n, g_jitseg_max, (unsigned)(want >> 20), dyn86_jitpool_size >> 20,
            g_jitseg_cap_mb * g_jitseg_max);
        wx86_vita_progress_c(m); }
    return 1;
}

static Blk* blk_find(const void* p) {
    for (int i = 0; i < DYN86_MAXBLK; ++i)
        if (g_blk[i].base && (const char*)p >= (const char*)g_blk[i].base
                          && (const char*)p <  (const char*)g_blk[i].base + g_blk[i].size)
            return &g_blk[i];
    return 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)flags; (void)fd; (void)offset;
    if (!length) { errno = EINVAL; return MAP_FAILED; }

    pthread_mutex_lock(&g_blk_mx);
    int slot = -1;
    for (int i = 0; i < DYN86_MAXBLK; ++i) if (!g_blk[i].base) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_blk_mx); errno = ENOMEM; return MAP_FAILED; }

    SceUID uid;
    int vm = (prot & PROT_EXEC) != 0;
    size_t size;
    if (vm) {
        size = (length + 0xFFFFFu) & ~(size_t)0xFFFFFu;      /* 1 MiB */
        /* These two guardrails are DISABLED BY DEFAULT (0 = inert). Refusing
         * a JIT allocation is normally safe in box86: mmap returns
         * MAP_FAILED, FillBlock fails, dynablock.c frees the block and falls
         * back to interpreting that address. This build has NO interpreter
         * (shim_impl.c's Run() stub just sets quit=1 and kills the guest
         * thread), so a refused JIT allocation kills the guest exactly as
         * surely as running out of memory would -- it doesn't make things
         * safer. Kept as diagnostic knobs (D2_JITRESERVE_KB, D2_JITMAX_MB)
         * for if/when an interpreter exists; until then the real fix is
         * giving the JIT more memory, not capping it. */
        {
            static unsigned cap_mb = 0, said = 0, said2 = 0;
            if (!cap_mb) { const char* e = getenv("WX86_JITMAX_MB"); if (!e) e = getenv("D2_JITMAX_MB");
                           cap_mb = e ? (unsigned)atoi(e) : 0u; }   /* 0 = DISABLED */
            /* An absolute cap alone is NOT enough: system RAM can be
             * exhausted by something other than the JIT while the JIT is
             * still well under its own cap, triggering a storm of allocation
             * failures and a hang before death. An absolute limit protects
             * nothing when a neighbor eats the budget -- what needs bounding
             * is what's LEFT.
             *
             * Hence a floor on remaining user RAM as well. Refusing here is
             * the GRACEFUL path (interpreter fallback, "fail=" counter
             * climbs); letting the system itself refuse is the FATAL path. */
            static unsigned res_kb = 0;
            if (!res_kb) { const char* e = getenv("WX86_JITRESERVE_KB"); if (!e) e = getenv("D2_JITRESERVE_KB");
                           res_kb = e ? (unsigned)atoi(e) : 0u; }   /* 0 = DISABLED, see below */
            SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
            if (res_kb && sceKernelGetFreeMemorySize(&fi) >= 0 &&
                (long)(fi.size_user >> 10) - (long)(size >> 10) < (long)res_kb) {
                pthread_mutex_unlock(&g_blk_mx);
                if (!said2) { said2 = 1; char m[160];
                    snprintf(m, sizeof m,
                        "JIT BRIDE : reserve systeme atteinte (libre %d Ko, plancher %u Ko) — interpretation en repli, voir fail=",
                        (int)(fi.size_user >> 10), res_kb);
                    wx86_vita_progress_c(m); }
                errno = ENOMEM; return MAP_FAILED;
            }
            if (cap_mb && dyn86_jit_cur + size > (size_t)cap_mb << 20) {
                pthread_mutex_unlock(&g_blk_mx);
                if (!said) { said = 1; char m[152];
                    snprintf(m, sizeof m,
                        "JIT PLAFONNE a %u Mo (en cours %u Mo) — interpretation en repli, voir fail= ; D2_JITMAX_MB pour changer",
                        cap_mb, (unsigned)(dyn86_jit_cur >> 20));
                    wx86_vita_progress_c(m); }
                errno = ENOMEM; return MAP_FAILED;
            }
        }
        /* --- pool: 16 MiB segments, growing on demand --- */
        {
            JitSeg* s = (g_jitseg_n > 0) ? &g_jitseg[g_jitseg_n - 1] : 0;
            if (!(s && s->used + size <= s->size)) {
                s = jitpool_grow() ? &g_jitseg[g_jitseg_n - 1] : 0;
            }
            if (s && s->used + size <= s->size) {
                void* p = (char*)s->base + s->used;
                s->used += size;
                dyn86_jitpool_used += (unsigned int)size;
                /* Pressure, announced while there is still room. The pool only
                 * ever grows (nothing evicts a translated block), so saturation
                 * is visible ten minutes before it turns fatal -- whereas the
                 * "fail=" counter on the alive: line only moves once blocks are
                 * already being refused, which on the reported sessions all
                 * happened inside the watchdog's 10 s blind window. */
                if (!g_jitseg_refused && g_jitseg_n >= (int)g_jitseg_max && dyn86_jitpool_size) {
                    static unsigned said_pct = 0;
                    unsigned pct = (unsigned)((uint64_t)dyn86_jitpool_used * 100u / dyn86_jitpool_size);
                    unsigned step = pct >= 95u ? 95u : pct >= 90u ? 90u : pct >= 75u ? 75u : 0u;
                    if (step > said_pct) { said_pct = step;
                        char m[176];
                        snprintf(m, sizeof m,
                            "JIT: piscine a %u%% (%u/%u Mo, %u segments) — rien n'est jamais evince,"
                            " un bloc non traduisible tue le fil invite",
                            pct, dyn86_jitpool_used >> 20, dyn86_jitpool_size >> 20, (unsigned)g_jitseg_n);
                        wx86_vita_progress_c(m); } }
                dyn86_jit_cur += (unsigned int)size;
                g_blk[slot].base = p;   g_blk[slot].size = size;
                g_blk[slot].uid  = s->uid;   /* the SEGMENT's uid: required by the VM sync */
                g_blk[slot].vm   = 1;
                g_blk[slot].pool = 1;              /* do NOT return to the kernel on munmap */
                pthread_mutex_unlock(&g_blk_mx);
                dyn86_vita_open_vm_thread();
                return p;
            }
        }
        uid = sceKernelAllocMemBlockForVM("dyn86_jit", size);
    } else {
        size = (length + 0xFFFu) & ~(size_t)0xFFFu;          /* 4 KiB */
        uid = sceKernelAllocMemBlock("dyn86_rw", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                     size, 0);
    }
    if (uid < 0) {
        pthread_mutex_unlock(&g_blk_mx);
        {
            SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
            int frc = sceKernelGetFreeMemorySize(&fi);
            char m[144];
            snprintf(m, sizeof m, "mmap FAIL %s %u KB: sce=0x%08x free user=%d KB cdram=%d KB phycont=%d KB",
                     vm ? "VM" : "RW", (unsigned)(size >> 10), (unsigned)uid,
                     frc < 0 ? -1 : fi.size_user >> 10,
                     frc < 0 ? -1 : fi.size_cdram >> 10,
                     frc < 0 ? -1 : fi.size_phycont >> 10);
            wx86_vita_progress_c(m);
        }
        errno = ENOMEM; return MAP_FAILED;
    }
    if (vm) dyn86_jit_cur += (unsigned int)size; else dyn86_rw_cur += (unsigned int)size;
    if (size >= (64u << 20)) {   /* big blocks: confirm on HW */
        char m[96];
        snprintf(m, sizeof m, "memblock %s %u MB ok (uid=0x%08x)",
                 vm ? "VM" : "RW", (unsigned)(size >> 20), (unsigned)uid);
        wx86_vita_progress_c(m);
    }

    void* base = 0;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        pthread_mutex_unlock(&g_blk_mx);
        sceKernelFreeMemBlock(uid); errno = ENOMEM; return MAP_FAILED;
    }
    if (vm) dyn86_vita_open_vm_thread();   /* per-thread — see the comment above */

    g_blk[slot].base = base; g_blk[slot].size = size;
    g_blk[slot].uid = uid;   g_blk[slot].vm = vm;  g_blk[slot].pool = 0;
    pthread_mutex_unlock(&g_blk_mx);
    /* mmap(MAP_ANONYMOUS) -- the desktop/qemu path this was validated against --
     * always hands back ZERO-filled pages. sceKernelAllocMemBlock does NOT: it
     * may return stale/garbage bytes. Guest code relies on the zero baseline
     * Windows itself guarantees (VirtualAlloc MEM_COMMIT, BSS, and a fresh
     * heap all read as zero), so a non-zeroed arena makes the guest read
     * garbage where it expects NUL -- which can surface as a guest-side
     * fatal error on Vita that never reproduces on qemu. Zero non-exec
     * blocks (the guest arena/heap) to restore mmap-ANON parity. JIT (vm)
     * blocks are fully overwritten by the emitter, so they need no clearing. */
    if (!vm) memset(base, 0, size);
    return base;
}

int munmap(void* addr, size_t length) {
    (void)length;
    pthread_mutex_lock(&g_blk_mx);
    Blk* b = blk_find(addr);
    if (!b || b->base != addr) { pthread_mutex_unlock(&g_blk_mx); errno = EINVAL; return -1; }
    if (b->vm) { if (dyn86_jit_cur >= b->size) dyn86_jit_cur -= (unsigned int)b->size; }
    else       { if (dyn86_rw_cur  >= b->size) dyn86_rw_cur  -= (unsigned int)b->size; }
    SceUID uid = b->uid;
    const int was_pool = b->pool;
    memset(b, 0, sizeof *b);
    pthread_mutex_unlock(&g_blk_mx);
    /* Pool sub-block: the kernel only knows about the whole pool, and box86
     * never returns its chunks anyway. Only the registry entry is released
     * here -- freeing the uid would destroy the ENTIRE pool. */
    if (!was_pool) sceKernelFreeMemBlock(uid);
    return 0;
}

int mprotect(void* addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;   /* see header: intentionally a no-op on Vita */
}

uint64_t dyn86_sync_us = 0;   /* cumulative cache-sync wall time (watchdog) */

void dyn86_vita_clear_cache(void* beg, void* end) {
    /* Snapshot the block under the registry lock (a concurrent mmap may be
     * scanning/inserting); the sync itself runs unlocked on the copy — VM
     * blocks are only ever freed at teardown. */
    pthread_mutex_lock(&g_blk_mx);
    Blk* bp = blk_find(beg);
    Blk bcopy; if (bp) bcopy = *bp;
    pthread_mutex_unlock(&g_blk_mx);
    Blk* b = bp ? &bcopy : 0;
    if (!b || !b->vm) return;                    /* non-VM: nothing to sync */
    /* Sync only the pages the emitter touched. A whole-block sync is a 2 MiB
     * dcache clean + icache invalidate: ~25 ms per translated block on the
     * A9, which turned every first visit of new code into a freeze (zone
     * loads = minutes-long translation storms). Page-rounded sub-range,
     * whole-block fallback if the kernel rejects it. */
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    uintptr_t base = (uintptr_t)b->base, top = base + b->size;
    uintptr_t lo = (uintptr_t)beg & ~(uintptr_t)0xFFF;
    uintptr_t hi = ((uintptr_t)end + 0xFFF) & ~(uintptr_t)0xFFF;
    if (lo < base) lo = base;
    if (hi > top)  hi = top;
    if (hi <= lo || sceKernelSyncVMDomain(b->uid, (void*)lo, (SceSize)(hi - lo)) < 0)
        sceKernelSyncVMDomain(b->uid, b->base, b->size);
    dyn86_sync_us += sceKernelGetProcessTimeWide() - t0;
}
