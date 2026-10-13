/* src/dynarec86/dyn86_memintrin.h — recognizes the guest CRT's memcpy/memset
 * at translation time and serves them natively, without a trap.
 * ------------------------------------------------------------------------
 * A hook (alternate + trap) round-trips through the dispatcher; for a
 * function as small and as frequently called as memcpy/memset, that round
 * trip can cost more than just letting the guest run its own translated
 * body. So instead of a trap, the translator recognizes the block that
 * BEGINS at the memcpy/memset entry point and emits a direct native call in
 * its place:
 *     mov r1, xESP                  ; guest ESP
 *     bl  dyn86_mi_copy/dyn86_mi_set ; C helper (r0 = emu)
 *     cmp r0, #0
 *     beq <original translated body> ; faithful fallback: nothing was done
 *     ldr xEAX, [xEmu, #regs[_AX]]  ; EAX = dst, written by the helper
 *     <ret_to_epilog>               ; normal dynarec RET
 * No trap, no dispatcher: a direct native call from translated code, just
 * like box86's div32/imul8 helpers.
 *
 * SAFETY / FALLBACK. The helper REFUSES (returns 0, the guest does the work)
 * when:
 *   - the knob isn't armed or the address hasn't been published;
 *   - size > dyn86_mi_maxn (16 MiB);
 *   - source or destination is outside the ARENA ([0, dyn86_mi_span)), when
 *     the arena is known -- the same guard as the shim's arena_check();
 *   - PROFILE mode (2) is active: it measures and always returns 0.
 * Overlap is NOT a refusal case: the guest CRT's memcpy body is the shared
 * memcpy/memmove body (backward branch when dst > src && dst < src+n), i.e.
 * exactly memmove() semantics -- so the native side calls memmove().
 *
 * WHAT THE NATIVE PATH DOES NOT REPRODUCE: the flags and ECX/EDX/ESI/EDI
 * clobbers the guest body leaves behind. cdecl declares those volatile, and
 * every call site is compiler-generated code, so that is safe. Flags are
 * left intact (box86's deferred-flags state stays valid) wherever the guest
 * body would have destroyed them -- more conservative, not less.
 *
 * This recognition scheme only works if every call site is a genuine `call`
 * to the function's first byte: no incoming `jmp` (which would resume
 * mid-block), no jump target inside the body, and no data/indirect
 * reference bypassing it. Verify this by disassembly before relying on it
 * for a new guest binary.
 *
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#ifndef DYN86_MEMINTRIN_H_
#define DYN86_MEMINTRIN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = off (default, byte-for-byte the original code)
 * 1 = serve natively
 * 2 = PROFILE only: measure the distribution then return 0 (guest does everything) */
extern int dyn86_memintrin;

/* Guest VAs of the two entry points, published by rt_boot AFTER the PE is
 * loaded (the guest executable may be relocated: these are not constants).
 * 0 = no recognition for that function. */
extern uintptr_t dyn86_mi_cpy_va, dyn86_mi_set_va;

/* Guard-rail bounds: extent of the guest arena (0 = unknown, guard disabled,
 * falls back to the original behavior) and max size served. */
extern uint32_t dyn86_mi_span;
extern uint32_t dyn86_mi_maxn;

/* Counters (final report). served/fallback are CALLS, not blocks. */
extern unsigned long long dyn86_mi_cpy_served, dyn86_mi_cpy_fb, dyn86_mi_cpy_bytes;
extern unsigned long long dyn86_mi_set_served, dyn86_mi_set_fb, dyn86_mi_set_bytes;
extern unsigned long long dyn86_mi_cpy_calls,  dyn86_mi_set_calls;
/* Fallback reasons (diagnostic): 0 = out of arena, 1 = size, 2 = profile. */
extern unsigned long long dyn86_mi_rej[4];

/* Measured distribution (mode 2 AND mode 1: counting costs just two adds).
 * hist[k] = calls whose size is in [2^(k-1), 2^k); hist[0] = n==0.
 * align[a] = calls where (dst|src) & 3 == a (0 = both word-aligned). */
#define DYN86_MI_HBITS 26
extern unsigned long long dyn86_mi_cpy_hist[DYN86_MI_HBITS], dyn86_mi_set_hist[DYN86_MI_HBITS];
extern unsigned long long dyn86_mi_cpy_align[4];
extern unsigned long long dyn86_mi_cpy_overlap;   /* overlapping dst/src */
extern unsigned long long dyn86_mi_cpy_small;     /* n < 16 */
extern unsigned long long dyn86_mi_cpy_maxseen;   /* largest size seen */
/* Calls (and bytes) the GUEST already handles via its own SSE2 fast path --
 * `_VEC_memzero` for memset, a movdqa-based copy for memcpy. my_cpuid
 * (shim_impl.c) reports SSE2, so the CRT's SSE2 capability flag is armed and
 * these branches are live: box86 translates their `movdqa` to NEON, 16 bytes
 * per instruction. This counter says whether a native port still has
 * headroom on LARGE transfers. [0]=calls, [1]=bytes. Counted in PROFILE mode
 * only. */
extern uint32_t dyn86_mi_sse2_va;                 /* VA of the guest's SSE2 capability flag (0 = unknown) */
extern unsigned long long dyn86_mi_cpy_sse[2], dyn86_mi_set_sse[2];

/* --- Cross-check oracle D2_MEMVERIFY (same pattern as D2_DCCVERIFY /
 * D2_RLEVERIFY) ---
 * When armed, the helper does NOT serve the call: it snapshots the source
 * (or fill value) into a host buffer, replaces the return address on the
 * GUEST stack with a trap, and returns 0 -- so the guest performs the copy
 * itself, with its own translated code. At the trap, dyn86_mi_verify_ret()
 * compares byte-for-byte what the guest wrote against what the native path
 * would have written, then returns the original return address (the shim
 * does a redirect_next to it). Only one call can be in flight at a time;
 * others are counted as "skipped". */
extern int      dyn86_mi_verify;        /* D2_MEMVERIFY=1 */
extern uint32_t dyn86_mi_verify_trap;   /* VA of the return trap, set by rt_boot */
extern unsigned long long dyn86_mi_ver_n, dyn86_mi_ver_bad, dyn86_mi_ver_skip;
uint32_t dyn86_mi_verify_ret(void);     /* compares, then returns the return address */

/* Published from rt_boot / CpuBox86. */
void dyn86_mi_arm(int mode, uintptr_t cpy_va, uintptr_t set_va);
void dyn86_mi_set_sse2va(uint32_t va);
void dyn86_mi_set_span(uint32_t span);
void dyn86_mi_arm_verify(uint32_t trap_va);

/* The two helpers called BY THE EMITTED CODE. r0 = emu, r1 = guest ESP.
 * Return 1 = served (EAX written to emu->regs[0]), 0 = fallback. */
int dyn86_mi_copy(void* emu, uint32_t esp);
int dyn86_mi_set (void* emu, uint32_t esp);

#ifdef __cplusplus
}
#endif
#endif /* DYN86_MEMINTRIN_H_ */
