/* D2Vita shim replacing Box86's src/include/debug.h
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 * All logging is compiled out; all tunables are compile-time constants
 * chosen for a Cortex-A9 (PS Vita) / qemu-arm target.
 */
#ifndef __DEBUG_H_
#define __DEBUG_H_
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct box86context_s box86context_t;

/* ---- log levels ---- */
#define LOG_NONE 0
#define LOG_INFO 1
#define LOG_DEBUG 2
#define LOG_NEVER 3
#define LOG_VERBOSE 3

#define ftrace stderr

#define printf_log(L, ...)   do { } while(0)
#define printf_dump(L, ...)  do { } while(0)
#define printf_dlsym(L, ...) do { } while(0)
#define dynarec_log(L, ...)  do { } while(0)

#define EXPORT
#define EXPORTDYN

/* ---- fastmmu: single global guest->host delta -------------------------------
 * The PS Vita cannot map memory at chosen virtual addresses (memblock VAs are
 * kernel-assigned, all >= 0x80000000), while D2 requires its own layout
 * (reloc-stripped Game.exe at 0x400000, Fog pool validator rejects pointers
 * >= 0x80000000). So the guest keeps a compact D2-native address space
 * [0, span) and every dereference adds dyn86_membase (host block base).
 * Constraints: low 24 bits of dyn86_membase must be 0 (single-ADD encodable
 * as ARM imm8 ror 8); 0 means identity (emits nothing -> the validated
 * qemu/Linux path stays bit-identical). C-side derefs use DYN86_G2H. */
extern uintptr_t dyn86_membase;
#define DYN86_G2H(a)  ((uintptr_t)(a) + dyn86_membase)
/* Inverse path: host address -> guest address. Used for diagnostics (naming
 * the faulting x86 instruction via the instsize table). */
#define DYN86_H2G(a)  ((uint32_t)((uintptr_t)(a) - dyn86_membase))

/* ---- allocators: plain libc ---- */
#define box_malloc      malloc
#define box_realloc     realloc
#define box_calloc      calloc
#define box_free        free
#define box_memalign(a,s) aligned_alloc(a,s)
#define box_strdup      strdup

/* ---- tunables, frozen as constants ---- */
static const int box86_log = 0;
static const int box86_dump = 0;
static const int box86_dynarec_log = 0;
static const int box86_dynarec = 1;
static const uintptr_t box86_pagesize = 4096;
static const uintptr_t box86_load_addr = 0;
static const int box86_showbt = 0;
static const int box86_maxcpu = 0;
static const int box86_maxcpu_immutable = 0;
static const int box86_dynarec_dump = 0;
static const int box86_dynarec_trace = 0;
static const int box86_dynarec_forced = 0;
static int box86_dynarec_largest __attribute__((unused)) = 0;
static const int box86_dynarec_bigblock = 2;   // build larger blocks across cond. branches (fewer transitions)
// D2_BUDGETTAIL=1: move the block-budget-expiry check out of the block
// prologue and place it at the block tail instead (see dynarec_arm_pass.c).
// Same code, different placement, letting the common-case branch skip over
// it. Default 0 keeps the original placement.
extern int box86_dynarec_budgettail;
// D2_FORWARD=<n>: max size of the gap a block may bridge forward to keep
// extending (dynarec_arm_pass.c, "forward extend" branch). Default 256
// matches upstream box86 behavior.
// Read by the translator at translation time (like box86_dynarec_callret
// below), so a single binary can carry both arms of an A/B: already
// translated blocks keep their shape, later ones pick up a changed value.
// Larger = longer blocks = fewer block transitions (~283 cycles each on the
// Vita), but more translated code, adding I-cache and jump-table pressure.
// The net effect is not predictable from qemu; only hardware measurement
// settles it.
extern int box86_dynarec_forward;
static const int box86_dynarec_strongmem = 0;
static const int box86_dynarec_x87double = 0;
// safeflags: x86 FLAGS conservatism at RET/RETN, upstream box86 value.
// Do not turn this into a runtime switch: the body of READFLAGS(A) is guarded
// by `if(((A)!=X_PEND && ...) ...)`, and X_PEND is 0x80 -- with A == X_PEND
// the condition is false at compile time, so both RET/RETN sites emit
// nothing regardless of the flag's value. Other call sites test `> 1`, also
// dead at the default. box86's deferred flags are already lazy at returns;
// there is nothing to gain from a toggle here.
static const int box86_dynarec_safeflags = 1;
/* Translator expansion counters: ARM bytes emitted, x86 bytes translated, blocks. */
extern unsigned long long dyn86_emit_arm_bytes, dyn86_emit_x86_bytes, dyn86_emit_blocks;
// D2_CALLRET=1: return-address prediction -- the dynarec pushes the pair
// (x86 return address, ARM target) on the ARM stack at CALL and checks it at
// RET, skipping the jump-table walk.
// Safe under cooperative threads: each thread owns its own x86emu_t, and
// arm_prolog/arm_epilog save and restore xSPSave on every DynaRun entry/exit,
// so an early block exit (budget expiry, trap, block splice) can never
// desync the prediction stack.
// The value is read by the translator at the moment it translates a block
// (like box86_dynarec_forward above), not per call -- so a single running
// binary can carry both old and new behavior: already-translated blocks keep
// their shape, blocks translated after the value changes (e.g. via env.txt)
// pick up the new one. Default 0 keeps the original behavior.
extern int box86_dynarec_callret;
// D2_SIGNTAG=1: reworks the "complemented pointer, discriminated by its sign
// bit" idiom to discriminate on bit 30 instead. Only meaningful under
// D2LAYOUT=haut (guest memory above 2 GiB); rt_boot refuses to arm it
// otherwise. Contract and proofs:
// third_party/box86-dynarec/dynarec/dynarec_arm_signtag.h.
extern int dyn86_signtag;
extern unsigned long dyn86_signtag_seen;    // patterns recognized (read-only tally)
extern unsigned long dyn86_signtag_done;    // patterns rewritten
// D2_MMUFOLD=1 / D2_MMUSTACK=1: two ways to remove the fastmmu base ADD from
// the critical path of a guest memory access. Same rule as callret: read by
// the translator at translation time, default 0 keeps the original code.
// Contract, enumerated forms and safety proofs:
// third_party/box86-dynarec/dynarec/dynarec_arm_mmu.h.
extern int dyn86_mmufold;
extern int dyn86_mmustack;
extern unsigned long dyn86_mmu_folds;        // absolute addressings folded
extern unsigned long dyn86_mmu_sites;        // GETED* call sites routed through geted
extern unsigned long dyn86_mmu_foldable;     // of which absolute form: foldable
extern unsigned long dyn86_mmu_st_heads;     // PUSH/POP r32 chain heads
extern unsigned long dyn86_mmu_st_links;     // chain links
extern unsigned long dyn86_mmu_st_pop;       // of which POP: one fewer link
extern unsigned long dyn86_mmu_st_push;      // of which PUSH: single instruction
extern unsigned long dyn86_mmu_st_rej_pos;   // rejected: code emitted in between
extern unsigned long dyn86_mmu_st_rej_pred;  // rejected: jump target / barrier
extern unsigned long dyn86_mmu_st_rej_kind;  // rejected: other family/register
static const uintptr_t box86_nodynarec_start = 0;
static const uintptr_t box86_nodynarec_end = 0;
static const int box86_dynarec_fastnan = 1;
static const int box86_dynarec_fastround = 1;
static const int box86_dynarec_hotpage = 0;
static const int box86_dynarec_wait = 1;
static const int box86_dynarec_fastpage = 0;
static const int box86_dynarec_bleeding_edge = 1;
static const int box86_dynarec_missing = 0;
static const int box86_dynarec_test = 0;
static const int box86_dynarec_jvm = 0;
static const int box86_dynarec_tbb = 0;
/* ARM hw caps: Cortex-A9 = VFPv3-D32 + NEON, no idiv, no crypto */
static const int arm_vfp = 3;
static const int arm_swap = 1;
static const int arm_div = 0;
static const int arm_aes = 0;
static const int arm_pmull = 0;
static const int box86_libcef = 0;
static const int box86_sdl2_jguid = 0;
static const int dlsym_error = 0;
static const int cycle_log = 0;
static const int trace_xmm = 0;
static const int trace_emm = 0;
static const int box86_nosandbox = 0;
static const int box86_malloc_hack = 0;
static const int box86_sse_flushto0 = 0;
static const int box86_x87_no80bits = 0;
static const int allow_missing_libs = 0;
static const int box86_prefer_wrapped = 0;
static const int box86_prefer_emulated = 0;
static const int box86_steam = 0;
static const int box86_wine = 0;
static const int box86_musl = 0;
static const int box86_showsegv = 0;
static const int box86_mutex_aligned = 0;
static const uintptr_t trace_start = 0, trace_end = 0;
static char* const trace_func = 0;
static const uint64_t start_cnt = 0;
static const uintptr_t fmod_smc_start = 0, fmod_smc_end = 0;
static const uint32_t default_fs = 0;
static const int jit_gdb = 0;
static int box86_mapclean __attribute__((unused)) = 0;

#endif /* __DEBUG_H_ */
