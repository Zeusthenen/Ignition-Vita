/* src/dynarec86/dyn86.h — public D2Vita API of the Box86-derived dynarec
 * platform layer (implemented in dyn86.c, linked into libdynarec86.a).
 *
 * Preemption model (cooperative + periodic, deterministic — no signals):
 *
 *  A. Periodic quantum (scheduler mode): every translated block's emitted
 *     prologue decrements emu->dyn86_budget (see dynarec_arm_pass.c); on
 *     expiry the block stores ip = block start, sets emu->dyn86_bbreak=2 and
 *     quit=1 and exits via arm_epilog — the emu leaves DynaRun exactly
 *     resumable at that block's entry. CpuBox86::run() recharges the budget
 *     each slice (set_run_limit), so the quantum is counted in BLOCK ENTRIES
 *     while blocks stay DIRECT-LINKED (no per-chain funnel overhead).
 *     Deterministic: same code path -> same schedule.
 *
 *  B. Explicit stop: dyn86_request_stop() raises a volatile flag, and the
 *     caller (CpuBox86::request_stop) also zeroes emu->dyn86_budget so the
 *     next block entry exits. Chains to not-yet-translated targets still pass
 *     through LinkNext, whose patched seam (tools/extract_box86.sh) calls
 *     dyn86_should_break(), sets emu->quit and returns the epilog — the emu
 *     exits DynaRun resumable at EIP = the pending jump target.
 *
 * After a LinkNext seam break dyn86_take_break() returns non-zero once (a
 * budget break instead surfaces as emu->dyn86_bbreak) — this is how CpuBox86
 * distinguishes "preempted but resumable" from a clean sentinel/trap stop.
 *
 * Limitation (documented): a loop contained in a SINGLE dynablock never
 * re-enters a block prologue and thus cannot be preempted this way; D2 code
 * yields via import calls (traps) long before that matters.
 *
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#ifndef DYN86_H_
#define DYN86_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ~64k chains ~= 1-3M guest instructions per slice (a chain occurs every
 * ~15-50 instructions in D2-like code) — same order as the 300k-instruction
 * quantum the Unicorn scheduler hook uses, with LinkNext overhead <0.1%. */
#define DYN86_QUANTUM_DEFAULT 65536u

/* D2Vita transfer ring dump (D2_XFERTRACE=1 + D2_NOLINK=1): prints the last 64
 * control transfers {src block -> target, esp} — call from a fault handler to
 * find the last valid block before a RET/JMP to garbage. No-op if not armed. */
void dyn86_dump_xfer(void);

/* D2Vita eiptrap silent ring (diag): dump the last 32 recorded chains to the
 * D2_EIPTRAP address — regs are live only in a diag build where arm_next.S
 * STMs the register file. Called by rt_boot's Crash.txt hook at a Fog Halt. */
void dyn86_dump_eipring(void);

/* One-time platform-layer init. Call ONCE, right after
 * init_custommem_helper() and before any translation. */
void dyn86_init(void);

/* --- D2_JITPROFILE: dynarec cost profile (opt-in, docs/perf/jitprofile.md) ---
 *
 * Armed ONCE in dyn86_init() from the environment. When it is 0 every counter
 * below stays untouched and the only cost is one global load + not-taken
 * branch on paths that already do a hash lookup — nothing is added to the
 * emitted ARM code, to block linking, to the budget prologue or to the
 * scheduler. Off => behaviour identical to a build without this block.
 *
 * INCLUSIVE / EXCLUSIVE (read this before computing any percentage):
 *   dyn86_jp_run_us      INCLUSIVE  — wall time inside cpu->run(): translated
 *                                     ARM code + JIT + lookups + icache sync +
 *                                     OS-shim bodies executed inside the slice.
 *   dyn86_fill_ns        INCLUSIVE of dyn86_sync_us (the __clear_cache of the
 *                                     freshly emitted block runs inside it).
 *   dyn86_sync_us        EXCLUSIVE leaf (Vita only; see mman_vita.c).
 *   dyn86_jp_lookup_ns   EXCLUSIVE of translation (the FillBlock time of a
 *                                     sampled call is subtracted) — SAMPLED,
 *                                     extrapolate with calls/samples.
 * So: run - jit - lookup = "pure execution + traps", and total - run = host
 * side (presentation, scheduler, idle). Never add run + jit + lookup.
 *
 * NOT measurable here (documented gap): the jump-table dispatch emitted INSIDE
 * the ARM code (indirect jumps/returns index box86_jmptbl directly). Only the
 * C-level lookups — DynaRun's entry DBGetBlock and the LinkNext funnel — pass
 * through dyn86_jp_lookup_*. Instrumenting the emitted dispatch would change
 * code generation, which this profiler is forbidden to do.
 */
extern int dyn86_jitprof;                /* 0 = everything below is dead */
extern uint64_t dyn86_jp_epoch_us;       /* monotonic origin of the profile */
extern uint64_t dyn86_jp_clock_bias_ns;  /* measured cost of one clock read */
extern uint64_t dyn86_jp_run_us;         /* accumulated by the scheduler */
uint64_t dyn86_jp_now_us(void);          /* same monotonic clock everywhere */

/* Sample 1 lookup out of DYN86_JP_SAMPLE (power of two: masked, not divided). */
#define DYN86_JP_SAMPLE 64u

extern uint64_t dyn86_jp_lookup_calls;   /* C-level dynablock lookups */
extern uint64_t dyn86_jp_lookup_hits;    /* served by an existing valid block */
extern uint64_t dyn86_jp_lookup_miss;    /* had to translate (new or again) */
extern uint64_t dyn86_jp_created;        /* NEW blocks published */
extern uint64_t dyn86_jp_recompiles;     /* known blocks re-translated (hash) */
extern uint64_t dyn86_jp_invalid;        /* blocks invalidated / freed */
extern uint64_t dyn86_jp_x86_insns;      /* x86 instructions translated */
extern uint64_t dyn86_jp_arm_bytes;      /* ARM bytes emitted */
extern uint64_t dyn86_jp_lookup_ns;      /* SAMPLED lookup time (bias removed) */
extern uint64_t dyn86_jp_lookup_smp;     /* samples actually taken */

/* Raise/clear the explicit stop flag (volatile int). */
void dyn86_set_stop(int v);

/* fastmmu: single global guest->host delta (low 24 bits must be 0; 0 =
 * identity). Set BEFORE any translation. See shim/debug.h. */
void dyn86_set_membase(uintptr_t base);
int dyn86_stop_requested(void);

/* Legacy knob (chain-quantum era). The live quantum is the block-entry
 * budget in emu->dyn86_budget, recharged per slice by CpuBox86::run(). */
void dyn86_set_quantum(uint32_t n);

/* 1 while direct block linking is allowed (quantum disabled). Called by the
 * patched addJumpTableIfDefault in custommem.c. */
int dyn86_link_direct(void);

/* Called by the patched LinkNext on every chain: non-zero => set quit and
 * exit to the epilog (explicit stop or quantum expiry). */
int dyn86_should_break(void);

/* Native-scheduler mode: dyn86_slice_begin must NOT clear a pending stop —
 * under the native backend a run() begins per THREAD START, and clearing the
 * global would erase a shutdown broadcast racing with a thread launch. */
void dyn86_set_native(int on);

/* Reset per-slice state (chain counter, stop flag, pending break marker —
 * the stop flag is preserved in native mode, see dyn86_set_native).
 * CpuBox86::run() calls this on entry. */
void dyn86_slice_begin(void);

/* Non-zero (once; self-clearing) if the last emu exit was caused by the
 * LinkNext seam: 1 = explicit stop, 2 = quantum expiry. */
int dyn86_take_break(void);

/* Raise the explicit stop flag (the caller also zeroes emu->dyn86_budget so
 * the next block entry exits — see CpuBox86::request_stop). */
void dyn86_request_stop(void);

/* SMC write-protection of translated pages (Box86 protectDB). Default OFF:
 * no SIGSEGV handler is ported yet, so a protected page would turn a guest
 * self-write into a hard crash. Enabling it requires that handler (later
 * lot). Escalation ladder if a boot misbehaves without it: (1) this stays
 * off and blocks may go stale on SMC; (2) hash-validation-on-entry for the
 * offending module; (3) the minimal segv handler + ON. */
void dyn86_set_protectdb(int on);
int dyn86_protectdb(void);

/* Translation-time GUEST->GUEST redirect for a known function (pristine mode):
 * the translator sends call/jmp targets equal to `from` to `to`, with NO
 * modification of guest memory. See bridge.h getAlternate, and
 * src/dynarec86/alt_table.c for the table's contract. */
void dyn86_set_alternate(uintptr_t from, uintptr_t to);
/* Count of alternates set, and how many were lost. The table has no fixed
 * bound: `lost` only increases when an allocation fails, existing so a
 * failure is never silent. */
int dyn86_alt_count(void);
int dyn86_alt_lost(void);

/* Log of unaligned 64-bit x87 accesses (parity sites).
 * k: 0 = total, 1 = unaligned, 2 = last host address, 3 = site */
uint32_t dyn86_unal_stat(int k);

#ifdef __cplusplus
}
#endif

#endif /* DYN86_H_ */
