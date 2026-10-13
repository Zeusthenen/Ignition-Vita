/* src/dynarec86/dyn86_intrin.h — table-driven recognition of native
 * intrinsics at translation time. Generalizes the mechanism proven by
 * D2_MEMINTRIN.
 * ------------------------------------------------------------------------
 *
 * WHY THIS MODULE EXISTS
 *
 * A hook (alternate + trap) round-trips through the dispatcher on every
 * call, which can cost more than just letting a small, hot guest function
 * run its own translated body -- so hooking is a poor fit for small,
 * frequently-called functions. D2_MEMINTRIN proved that a direct native call
 * emitted at translation time (no trap, no dispatcher) works at scale
 * instead.
 *
 * This module extracts that mechanism from memintrin and makes it
 * table-driven, so a new target doesn't need a bespoke hook-vs-native
 * tradeoff analysis.
 *
 * WHAT THE TRANSLATOR EMITS, when a block BEGINS at a registered VA:
 *
 *     mov r1, xESP                  ; guest ESP (stack arguments)
 *     bl  <helper>                  ; r0 = xEmu, contract: int fn(emu, esp)
 *     cmp r0, #0
 *     beq <original translated body> ; faithful fallback: nothing was done
 *     [ldr xREG, [xEmu, regs[REG]]] ; for each register in `regmask`
 *     <ret_to_epilog | retn_to_epilog(retn)>
 *
 * No trap, no dispatcher: a direct native call from translated code, just
 * like box86's div32/imul8 helpers.
 *
 * WHAT MAKES BLOCK-START RECOGNITION VALID
 *
 * It only captures a function if EVERY entry creates a block that BEGINS at
 * its first byte. For each target, disassembly must confirm there is:
 *   - no direct `jmp` to the entry (that would be a tail call: the block
 *     wouldn't restart there) -- a direct `call` is fine;
 *   - no jump INTO the body (past the first byte).
 * Data references (a vtable slot) are not a problem: an indirect call also
 * creates a block at the entry point. A verification script
 * (tools/verif_intrin_capture.py) checks these properties by disassembly
 * before a target is registered.
 *
 * SAFETY. The helper returns 0 as soon as it isn't SURE -- the guest then
 * does the work with its own translated code, untouched. The fallback is not
 * an error path, it is the contract: anything the native side can't
 * reproduce identically must be refused.
 *
 * INPUTS: `inmask` IS NOT OPTIONAL.
 * The 8 x86 registers live PERMANENTLY in r4-r11 and are only stored into
 * `emu` AT THE BLOCK EPILOGUE (arm_epilog.S: `stm r0,{r4-r12,r14}`). A helper
 * that reads `emu->regs[...]` without the corresponding bit set in `inmask`
 * therefore reads a STALE value -- the one from the last return to the
 * dispatcher, not the value at the current call. memintrin didn't have this
 * problem: it only reads the guest STACK, via `esp`. A client of this module
 * can take arguments in registers instead, which is exactly why `inmask` is
 * a mandatory field rather than an option: a helper reading a register
 * outside `inmask` silently reads garbage.
 *
 * REGISTERS AND FLAGS. The native path writes ONLY the registers declared in
 * `regmask` (reloaded from `emu` by the emitted code). Every other register
 * keeps the value it had at entry -- CONSERVATIVE when the guest body used to
 * clobber it, WRONG if a caller reads a register the guest body clobbered
 * observably. This is why `regmask` must list every register the guest body
 * modifies AND that a caller may read, including the calling convention's
 * "volatile" registers: disassembly decides this, not the calling
 * convention. FLAGS are left intact (box86's deferred-flags state stays
 * valid) wherever the guest body used to destroy them -- more conservative,
 * not less.
 *
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#ifndef DYN86_INTRIN_H_
#define DYN86_INTRIN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Helper contract: return 1 if it SERVED the call (the emitted code then
 * executes RET), 0 to FALL BACK to the original translated body.
 *   emu = x86emu_t* (r0, dynarec invariant)
 *   esp = guest ESP AT ENTRY: [esp] = return address, [esp+4] = first stack
 *         argument, etc. */
typedef int (*dyn86_intrin_fn)(void* emu, uint32_t esp);

/* Register indices for `regmask` -- same values as regs.h (_AX.._DI). One bit
 * per register to RELOAD from emu after the call. */
#define DYN86_IR_AX (1u<<0)
#define DYN86_IR_CX (1u<<1)
#define DYN86_IR_DX (1u<<2)
#define DYN86_IR_BX (1u<<3)
#define DYN86_IR_SP (1u<<4)
#define DYN86_IR_BP (1u<<5)
#define DYN86_IR_SI (1u<<6)
#define DYN86_IR_DI (1u<<7)

typedef struct dyn86_intrin_s {
    uintptr_t         va;       /* guest VA of the entry point (0 = free slot) */
    dyn86_intrin_fn   fn;
    uint16_t          retn;     /* bytes popped by RET (stdcall); 0 = bare RET */
    uint16_t          inmask;   /* registers STORED to emu BEFORE the call    */
    uint16_t          regmask;  /* registers reloaded from emu AFTER the call */
    const char*       name;     /* for the arming-proof line */
    unsigned long long calls;   /* entries into the helper                    */
    unsigned long long served;  /* of which served (calls - served = fallbacks) */
} dyn86_intrin_t;

#define DYN86_INTRIN_MAX 16

/* 0 = nothing emitted (default, byte-for-byte the original code)
 * 1 = serve natively
 * 2 = PLUMBING ONLY: the helper is called and ALWAYS returns 0, so the guest
 *     still runs its translated body. Every client MUST honor this mode.
 *     It is the only way to separate a port's two costs:
 *        mode 2 - baseline   = cost of the PLUMBING
 *        mode 1 - mode 2     = benefit of the native BODY
 *     Without it, a regression can't tell whether the native code is slow or
 *     the round trip itself is eating the gain. */
extern int dyn86_intrin_on;
extern int dyn86_intrin_n;
extern dyn86_intrin_t dyn86_intrin_tbl[DYN86_INTRIN_MAX];

/* Registers a target. Must be called AFTER the PE is loaded (VAs are not
 * constants: the guest executable may be relocated) and BEFORE the first
 * translation. Returns 1 if the entry was added, 0 if the table is full or
 * the VA is already registered. Does not arm anything by itself: see
 * dyn86_intrin_on. */
int dyn86_intrin_add(uintptr_t va, dyn86_intrin_fn fn, uint16_t retn,
                     uint16_t inmask, uint16_t regmask, const char* name);

/* Consulted AT TRANSLATION TIME, once per block start. Returns NULL if the VA
 * isn't a target or the mechanism is off. The table is frozen before the
 * first translation, so this predicate is PURE: passes 2 and 3 emit exactly
 * the same thing (box86's divergence detector requires this). */
const dyn86_intrin_t* dyn86_intrin_find(uintptr_t va);

/* Arming-proof line, published by the periodic status window. Returns the
 * number of bytes written, with a distinct marker when nothing is armed --
 * a silent counter proves nothing if the run never reaches a final report,
 * so this must surface periodically rather than only at the end. */
int dyn86_intrin_report(char* out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* DYN86_INTRIN_H_ */
