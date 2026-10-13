/* src/dynarec86/dyn86.c — see dyn86.h.
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "dyn86.h"
#include "bridge.h"          /* table hasAlternate/getAlternate (alt_table.c) */

static volatile int g_stop = 0;
static uint32_t g_quantum = 0;      /* chains per slice; 0 = periodic break off */
static uint32_t g_chains = 0;       /* chains consumed in the current slice */
static int g_break = 0;             /* last emu exit came from the seam (1|2) */

/* fastmmu single global guest->host delta (see shim/debug.h). 0 = identity:
 * every MMU emission site collapses to the original validated code path.
 * Must be set BEFORE any translation happens (low 24 bits zero). */
uintptr_t dyn86_membase = 0;
void dyn86_set_membase(uintptr_t base) { dyn86_membase = base; }

/* --- D2_JITPROFILE: counters + clock (contract in dyn86.h) --------------- */
int dyn86_jitprof = 0;
// Dynarec return-address prediction (D2_CALLRET=1). Defined HERE rather than
// in debug.h: the translator reads it AT TRANSLATION TIME, so a single binary
// carries both arms of the A/B. Default 0 keeps the original behavior.
int box86_dynarec_callret = 0;
/* Translator expansion (dynarec_arm.c): bytes emitted / x86 bytes. */
unsigned long long dyn86_emit_arm_bytes = 0, dyn86_emit_x86_bytes = 0, dyn86_emit_blocks = 0;
// D2_FORWARD=<n> (default 256 = original box86 value) and D2_BUDGETTAIL=<0|1>
// (default 0 = original prologue placement). Defined here for the same
// reason as callret: the translator reads them AT TRANSLATION TIME, so a
// single binary carries every arm of the A/B. Armed from tools/rt_boot.cpp.
int box86_dynarec_forward = 256;
/* D2_NOPEND: relax the forced flags archiving at block end.
 * Contract, modes and the mode-3 warning: dynarec_arm_pass.c.
 * Defined HERE like the other translation knobs: a single binary carries
 * every arm of the A/B. Default 0 keeps box86 behavior byte-for-byte.
 * These count translation SITES, not executions. */
int dyn86_nopend = 0;
unsigned long dyn86_nopend_dropped = 0, dyn86_nopend_kept = 0, dyn86_nopend_seen = 0;
/* Deferred-archiving sites, counted AT TRANSLATION TIME (all passes):
 *   need = a consumer in the block needs it (X_PEND)  -> legitimate
 *   pess = gen_flags == 0, nobody needs it here        -> pessimistic
 *   cut  = of which removed by D2_NOPEND=3 */
unsigned long dyn86_pendor0_need = 0, dyn86_pendor0_pess = 0, dyn86_pendor0_cut = 0;
unsigned long dyn86_pendor0_pess3 = 0, dyn86_pendor0_cut3 = 0;
int box86_dynarec_budgettail = 0;

/* --- D2_SIGNTAG: "complemented pointer, discriminated by its sign bit" ---
 * Full contract and safety proofs:
 * third_party/box86-dynarec/dynarec/dynarec_arm_signtag.h.
 * Defined HERE (compiled into one unit): the translator reads these AT
 * TRANSLATION TIME, so a single binary carries both arms of the A/B.
 * Default 0 keeps the original code byte-for-byte. */
int dyn86_signtag = 0;
unsigned long dyn86_signtag_seen = 0;   /* patterns recognized (read-only tally) */
unsigned long dyn86_signtag_done = 0;   /* patterns rewritten onto bit 30  */

/* --- fastmmu: move the base ADD out of the critical path ------------------
 * Full contract, enumerated forms and safety proofs:
 * third_party/box86-dynarec/dynarec/dynarec_arm_mmu.h.
 * Defined HERE (compiled into one unit) and not in debug.h: the translator
 * reads these AT TRANSLATION TIME, so a single binary carries both arms of
 * each A/B. Default 0 keeps the original code byte-for-byte.
 * Both are inert without an arena (dyn86_membase == 0): a benchmark without
 * D2ARENA proves nothing about them. */
int dyn86_mmufold = 0;      /* D2_MMUFOLD=1: absolute addressings */
int dyn86_mmustack = 0;     /* D2_MMUSTACK=<mask>: 1 = POP, 2 = PUSH, 3 = both */
/* Translation counters (SITES, not executions), counted in PASS 3 only.
 * Without them, an A/B with identical output can't tell "the change is
 * neutral" apart from "the change touched nothing". */
unsigned long dyn86_mmu_folds = 0;        /* absolute addressings FOLDED       */
unsigned long dyn86_mmu_sites = 0;        /* GETED* sites routed through geted  */
unsigned long dyn86_mmu_foldable = 0;     /* of which ABSOLUTE form = FOLDABLE  */
unsigned long dyn86_mmu_st_heads = 0;     /* PUSH/POP r32 chain heads           */
unsigned long dyn86_mmu_st_links = 0;     /* chain links (one fewer ADD)        */
unsigned long dyn86_mmu_st_pop = 0;       /* of which POP: one fewer LINK       */
unsigned long dyn86_mmu_st_push = 0;      /* of which PUSH: a single INSTRUCTION */
unsigned long dyn86_mmu_st_rej_pos = 0;   /* rejected: code emitted in between   */
unsigned long dyn86_mmu_st_rej_pred = 0;  /* rejected: jump target/barrier       */
unsigned long dyn86_mmu_st_rej_kind = 0;  /* rejected: other family/register     */
uint64_t dyn86_jp_epoch_us = 0;
uint64_t dyn86_jp_clock_bias_ns = 0;
uint64_t dyn86_jp_run_us = 0;

static uint64_t jp_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t dyn86_jp_now_us(void) { return jp_now_ns() / 1000ull; }

/* A bracketed measurement (t0 = read, work, t1 = read) also charges the cost
 * of the first clock read itself. On Vita that read is a kernel call, i.e. of
 * the SAME order as the sub-microsecond thing being measured — left in, it
 * would inflate the lookup number by an order of magnitude. Measure the cost
 * of one read here, once, and subtract it from every sample. Reported in the
 * dump so the correction stays auditable. */
static void jp_calibrate(void)
{
    const int N = 2048;
    uint64_t acc = 0;
    for (int i = 0; i < N; ++i) {
        uint64_t a = jp_now_ns();
        uint64_t b = jp_now_ns();
        if (b > a) acc += b - a;
    }
    dyn86_jp_clock_bias_ns = acc / (uint64_t)N;
}

void dyn86_init(void)
{
    /* Nothing to capture anymore (the jump-table default was only needed by
     * the retired poison-based preemption); kept as the documented init hook
     * in case future platform state needs a pre-translation capture point. */

    /* D2_JITPROFILE: read ONCE here, so the hot paths test a plain int and
     * never call getenv(). dyn86_init() runs after the Vita env.txt parse
     * (rt_boot main -> d2vita_platform_init -> ... -> CpuBox86 ctor) and
     * before the first translation -- the correct origin point for the
     * profile clock. */
    {
        /* D2_FRAMEPROF (per-frame attribution, rt_boot) reads the SAME counters,
         * so it must arm them too, or its deltas are all zero. */
        const char* e = getenv("WX86_JITPROFILE"); if (!e) e = getenv("D2_JITPROFILE");
        const char* f = getenv("WX86_FRAMEPROF"); if (!f) f = getenv("D2_FRAMEPROF");
        dyn86_jitprof = ((e && e[0] && e[0] != '0') || (f && f[0] && f[0] != '0')) ? 1 : 0;
        if (dyn86_jitprof) { jp_calibrate(); dyn86_jp_epoch_us = dyn86_jp_now_us(); }
    }
}

void dyn86_set_stop(int v) { g_stop = v; }

/* Native-scheduler mode (see dyn86.h): slice_begin keeps a pending stop. */
static int g_native = 0;
void dyn86_set_native(int on) { g_native = on; }

int dyn86_stop_requested(void) { return g_stop; }

void dyn86_set_quantum(uint32_t n)
{
    /* Quantum is now enforced by the block-entry budget emitted in every
     * translated block's prologue (see dynarec_arm_pass.c): the budget lives
     * in emu->dyn86_budget, recharged per slice by CpuBox86::run(). No more
     * jump-table poisoning — blocks stay DIRECT-LINKED under the scheduler. */
    g_quantum = n;
}

/* Always direct-link: preemption no longer needs the LinkNext funnel.
 * D2_NOLINK=1 (diag): keep every chain in the LinkNext funnel so a
 * block-level trace sees ALL transitions (slower). */
int dyn86_link_direct(void) {
    static int nolink = -1;
    if (nolink < 0) nolink = (getenv("WX86_NOLINK") != NULL || getenv("D2_NOLINK") != NULL) ? 1 : 0;
    return !nolink;
}

int dyn86_should_break(void)
{
    /* Only the stop request matters here now (LinkNext still runs for
     * not-yet-translated targets); the quantum lives in the block budget. */
    if (g_stop) { g_break = 1; return 1; }
    return 0;
}

void dyn86_slice_begin(void)
{
    if (!g_native) g_stop = 0;
    g_chains = 0;
    g_break = 0;
}

int dyn86_take_break(void)
{
    int b = g_break;
    g_break = 0;
    return b;
}

void dyn86_request_stop(void)
{
    /* The caller (CpuBox86::request_stop) also zeroes emu->dyn86_budget so the
     * next block entry exits — no jump-table poisoning needed anymore. */
    g_stop = 1;
}

/* D2_INLINETRAP (inline-translating the CC 'S' 'C' exit stub into a native
 * call, skipping the epilog/prolog round trip) is not faster in practice,
 * even fully armed. Do not reintroduce it without a new cost model: the
 * assumption that motivated it -- "every block transition pays a ~283-cycle
 * cache miss" -- no longer holds. */

static int g_protectdb = 0;     /* see dyn86.h: OFF until a segv handler exists */
void dyn86_set_protectdb(int on) { g_protectdb = on; }
int dyn86_protectdb(void) { return g_protectdb; }

/* The alternates table (GUEST -> GUEST redirection applied at translation
 * time) lives in src/dynarec86/alt_table.c: a leaf unit with no dynarec
 * dependency, so it can be exercised by a desktop test. Its declarations
 * stay in bridge.h. */

/* ---- 64-bit x87 accesses to GUEST memory at "parity" sites ----
 * getedparity() assumes the address is 8-byte aligned and box86 emits
 * LDRD/STRD. If that assumption is wrong at runtime, unaligned LDRD/STRD are
 * UNPREDICTABLE on Cortex-A9 (silent corruption) while qemu just accepts
 * them -- exactly the profile of a HW-only, layout-sensitive crash. These
 * helpers do 2 safe 32-bit accesses instead, AND log every unaligned host
 * address with its call site. h = host address. */
#include <string.h>
volatile uint32_t dyn86_unal_total = 0, dyn86_unal_cnt = 0, dyn86_unal_last = 0, dyn86_unal_site = 0;
static inline void dyn86_unal_note(uint32_t h, uint32_t site) {
    dyn86_unal_total++;
    if (h & 7u) { dyn86_unal_cnt++; dyn86_unal_last = h; dyn86_unal_site = site; }
}
void dyn86_st64_site1(void* emu, uint32_t h, uint32_t lo, uint32_t hi) {
    (void)emu; dyn86_unal_note(h, 1);
    memcpy((void*)(uintptr_t)h, &lo, 4); memcpy((char*)(uintptr_t)h + 4, &hi, 4);
}
void dyn86_st64_site3(void* emu, uint32_t h, uint32_t lo, uint32_t hi) {
    (void)emu; dyn86_unal_note(h, 3);
    memcpy((void*)(uintptr_t)h, &lo, 4); memcpy((char*)(uintptr_t)h + 4, &hi, 4);
}
uint64_t dyn86_ld64_site2(void* emu, uint32_t h) {
    (void)emu; dyn86_unal_note(h, 2);
    uint32_t lo, hi; memcpy(&lo, (const void*)(uintptr_t)h, 4); memcpy(&hi, (const char*)(uintptr_t)h + 4, 4);
    return ((uint64_t)hi << 32) | lo;
}
uint32_t dyn86_unal_stat(int k) {
    switch (k) { case 0: return dyn86_unal_total; case 1: return dyn86_unal_cnt;
                 case 2: return dyn86_unal_last;  default: return dyn86_unal_site; }
}
