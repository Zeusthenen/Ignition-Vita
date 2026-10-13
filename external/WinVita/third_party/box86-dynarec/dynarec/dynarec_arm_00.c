#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "bridge.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynarec_arm.h"
#include "dynarec_arm_private.h"
#include "arm_printer.h"
#include "custommem.h"

#include "dynarec_arm_functions.h"
#include "dynarec_arm_helper.h"
#include "dynarec_arm_signtag.h"

int isRetX87Wrapper(wrapper_t fun);
void emit_signal(x86emu_t* emu, int sig, void* addr, int code);
void emit_div0(x86emu_t* emu, void* addr, int code);

// IgnitionVita iteration-33: message-queue depth, written by
// source/win32/user32_window.cpp's sync_queue_depth() (a different
// translation unit and language -- this vendored file can't link against
// its C++/STL). See ign_loop33_marker() below.
extern volatile uint32_t g_ign_msg_queue_depth;

// ============================================================================
// IgnitionVita iteration-31 -- register-cache architecture note (found the
// hard way: this exact misunderstanding caused a real, demonstrated bug in
// the iteration-28/29/30 diagnostic overrides). Documented here so it isn't
// rediscovered the same way again.
//
// x86 registers are cached DIRECTLY in fixed physical ARM registers for the
// life of a translated block (arm_emitter.h): xEAX=r4, xECX=r5, xEDX=r6,
// xEBX=r7, xESP=r8, xEBP=r9, xESI=r10, xEDI=r11, xFlags=r12. These are the
// LIVE values the JIT-generated code actually computes with; they are NOT
// automatically synchronized to the x86emu_t struct (`emu->regs[]` /
// `emu->eflags`) at every instruction -- only on an explicit STM (flush
// live ARM regs -> struct) or LDM (load struct -> live ARM regs), typically
// wrapped around a native C callout (CALL_S/call_c), since a plain BLX only
// preserves r4-r11 by the callee's own AAPCS-mandated prologue/epilogue,
// and NEVER preserves r12 (an explicitly volatile/scratch register in
// AAPCS) -- which is exactly why every marker in this file flushes xFlags
// alongside the others.
//
// THE TRAP: STM/LDM are real ARM "multiple register" instructions. They
// transfer the registers named in their bitmask SEQUENTIALLY, in ascending
// register-number order, to/from CONSECUTIVE words starting at the base
// address -- with NO GAP for a register that's absent from the mask. They
// do NOT address each register at its own fixed offset into the struct.
// The register layout above was deliberately chosen so that regs[8] (32
// bytes: _AX,_CX,_DX,_BX,_SP,_BP,_SI,_DI, matching r4..r11) followed by
// `eflags` (r12) lines up with a FULL r4-r12 STM/LDM with zero gaps -- but
// the instant a mask skips a register in the middle of that range (e.g.
// `(1<<xESP)|(1<<xFlags)`, skipping r9/r10/r11), every register AFTER the
// gap silently lands 1+ words short of its real struct offset, quietly
// corrupting an unrelated field and/or reading one back. This is exactly
// what broke the iteration-28 PUSHFW override (xESP never made it back
// into the live register), iteration-29's REP STOSD override (live xEDI
// came back as 0), and iteration-30's POPFW override (read stack data 2
// bytes away from where PUSHFW had actually written it). Fixed in
// iteration 31 by always using the FULL, contiguous 9-register mask when a
// diagnostic override's STM/LDM needs to touch more than one register (see
// the REP STOSD override below), and, better yet, by not needing STM/LDM
// at all for PUSHFW/POPFW anymore (see dynarec_arm_66.c's case 0x9C/0x9D
// -- native ARM emission via the existing MMU_PUSH16/MMU_POP16 primitives,
// which manipulate the live xESP/xFlags registers directly).
// ============================================================================

// ============================================================================
// IgnitionVita iteration-23 -- translation-time-only diagnostic.
//
// iteration-22's runtime override (guard `ip == 0x0045AFDE`, native call_c()
// callout replacing REP STOSD's ARM loop) is REMOVED here, per this
// iteration's explicit instruction: real hardware showed [REP22] BEGIN never
// fired, so the guard's premise needs re-checking before layering anything
// else on top of it. dynarec00() is back to generating the exact stock
// translation for every instruction, unconditionally.
//
// What's added instead is pure HOST-side logging inside the C translator
// itself -- these printf/fopen calls run on the ARM CPU while it is
// COMPILING x86 blocks into ARM code (i.e. host execution time), and emit
// ZERO additional ARM instructions into the generated block. This cannot
// change guest behavior even in principle: it's not code the guest ever
// executes, it's the compiler's own bookkeeping. Two things are logged:
//   1. Every instruction translated with `ip` inside [0x0045AFB0,0x0045B020)
//      (calloc) or [0x00469470,0x004694C0) (the HeapAlloc wrapper) -- one
//      line per instruction, at COMPILE time, not per execution.
//   2. Extra detail specifically when opcode==0xAB is seen in that range,
//      confirming (or refuting) whether the REP prefix was really active
//      and what `ip`/`addr` actually hold at that point -- the exact
//      question this iteration exists to answer.
// "Translated" is NOT "executed": a block can be compiled once and then
// run many times, zero times if a fault happens first, or the dynablock
// could already be cached from an earlier compile far above in the log.
// These lines only prove the guest's control flow reached far enough for
// the TRANSLATOR to process that address at some point -- see the
// [DYN23]/[DYN23-STOSD]/[DYN23-HEAPRET] tags themselves for exactly what
// each one claims.
static void ign_dyn23_log(const char* tag, const char* fmt, ...)
{
    FILE* lf = fopen("ux0:data/ignition/ignitionvita.log", "a");
    if (!lf) return;
    fprintf(lf, "%s ", tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fprintf(lf, "\n");
    fclose(lf);
}

// IgnitionVita iteration-25/26 -- passive return-chain markers.
//
// iteration 25 localized the crash to the 13-byte span [0x0045AFC5,
// 0x0045AFD2) -- marker=4 (M4) fired, marker=5 (M5, CLD) never did.
// iteration 26 subdivided that span further: marker=7 (TEST EAX,EAX,
// 0x0045AFCB) fired, marker=8 (PUSH EDI, 0x0045AFD1) never did -- crash
// now localized to the 6-byte span [0x0045AFCB, 0x0045AFD1).
// iteration 27 subdivides THAT span, byte-verified instruction by
// instruction (real bytes re-read from Ign_d3d.exe this iteration, not
// recalled): 85 C0 | 74 1C | 66 9C ->
//   (10) = 0x0045AFCD  JZ 0x0045AFEB (branch NOT expected to be taken --
//                       EAX was non-NULL at M4/marker7)
//   (11) = 0x0045AFCF  PUSHFW (66 9C -- 16-bit operand-size-prefixed
//                       PUSHF; dispatches through dynarec_arm_66.c's
//                       OWN case 0x9C, a different translator function
//                       from this file's plain 32-bit case 0x9C). Prime
//                       suspect: that handler emits a RAW STRHB_IMM8
//                       using xESP directly as the ARM base register,
//                       with no dyn86_membase translation and no ESP
//                       adjust -- see the marker call site added in
//                       dynarec_arm_66.c this iteration for the full
//                       source-level finding. This marker fires (if it
//                       fires) BEFORE that store executes, so seeing it
//                       land while marker 8 never does would pin the
//                       crash to that exact STRHB_IMM8.
// M1(1)/M2(2) (malloc-2arg/1arg return markers) are DISABLED as of
// iteration 27 (fired on every single malloc/calloc call in the game --
// pure noise now that the return chain up to M4 is proven). The `ip`
// checks for them are removed from case 0x83 below; kMarkerEip[1]/[2]
// are kept only so existing marker IDs don't shift.
//
// iteration 28: marker 11 fired on real hardware immediately before the
// crash (confirming the PUSHFW suspicion), so this iteration installs a
// diagnostic-only REPLACEMENT for that ONE instruction (0x0045AFCF, see
// dynarec_arm_66.c's case 0x9C and ign_pushfw28_override()) instead of a
// passive marker, plus one new passive marker:
//   (12) = 0x0045AFDE  immediately before REP STOSD -- does NOT touch REP
//                       in any way, purely observes whether the override
//                       let execution get this far.
//
// iteration 29: the PUSHFW fix (28) proved on real hardware that execution
// reaches 0x0045AFDE (REP STOSD) with EDI/ECX/EAX exactly as calloc's
// disassembly predicts, then hard-crashes. Audited the stock REP STOSD
// translation (case 0xAB below) first: MMU_SAI4 IS membase-translated
// correctly (unlike the PUSHFW bug), so this is a different, still-
// unidentified problem. Installs a second diagnostic-only REPLACEMENT,
// scoped to this ONE instruction (see ign_repstosd29_override() above),
// plus three new passive markers on the real, byte-verified instructions
// immediately after it:
//   (13) = 0x0045AFE0  MOV ECX,[EBP+0C] -- first instruction after REP
//                       STOSD; reaching it proves the override returned
//                       control correctly.
//   (14) = 0x0045AFE9  POPFW -- restores the flags saved by the PUSHFW
//                       this project already fixed (lives in
//                       dynarec_arm_66.c's case 0x9D).
//   (15) = 0x0045AFF2  RET -- calloc's own return; reaching it means the
//                       whole function completed successfully.
//
// These do NOT replace any instruction (except the two explicit,
// exact-EIP-scoped overrides noted above): each other call site sits
// BEFORE the real opcode decode for its case, with NO `break`, so the
// original instruction is still decoded and emitted immediately after,
// completely unchanged. The guard is `ip == <exact address>` inside a
// specific case (0x83 for M4, 0x8B for M3, marker 9 and marker 13, 0xFC
// for M5, 0x89 for marker 6, 0x85 for marker 7, 0x57 for marker 8, 0x74
// for marker 10, 0xAB/REP-STOSD block for marker 12 and the REP29
// override, 0xC3 for marker 15) -- every one of those opcodes is common
// elsewhere in the game, but the `ip` comparison means this can only ever
// fire for these exact instruction occurrences. Marker 11 and marker 14
// live in dynarec_arm_66.c (see that file); the REP STOSD override
// (marker 12 and ign_repstosd29_override()) is entirely in THIS file,
// since REP STOSD has no 0x66 prefix.
//
// iteration 31: PUSHFW (marker 11's instruction) and POPFW (marker 14's
// instruction) are no longer diagnostic overrides -- both are now GLOBAL
// fixes in dynarec_arm_66.c's case 0x9C/0x9D, so markers 11/14 now fire
// UNCONDITIONALLY (every 66-9C/66-9D in the whole game), not just at
// Ignition's two specific EIPs. REP STOSD (marker 12) is still an
// EIP-scoped diagnostic override -- no ARM-level root cause has been
// proven for it yet (MMU_SAI4's own translation audited as correct), so
// it stays scoped rather than claiming a generic fix that isn't proven.
//
// Mechanism: same call_c()/CALL_S() native callout as every prior
// iteration's helper (proven safe, matches Box86's own CPUID handler). EIP
// is deliberately left OUT of the STM/LDM mask: this helper never reads or
// writes emu->ip, so there's nothing to save.
// iteration-27 correction: an earlier version of this diagnostic made this
// function non-static so dynarec_arm_66.c (marker 11) could call it
// directly. That broke the link: build.sh compiles this file 4 times (one
// per multi-pass `step`) into 4 distinct .o members of libwinx86_vita.a,
// and a non-static ign_marker25 became 4 colliding GLOBAL symbols ("multiple
// definition of `ign_marker25'"). Reverted to `static` -- each pass keeps
// its own private copy, exactly like every other per-file static in this
// translator. Marker 11 in dynarec_arm_66.c uses its own small, separate
// static logging function instead (see that file) rather than sharing
// this one across translation units.
static void ign_marker25(x86emu_t* emu, uint32_t markerId)
{
    static const uint32_t kMarkerEip[24] = {
        0, 0x0046944D, 0x00469410, 0x0045AE10, 0x0045AFC5, 0x0045AFD2,
        0x0045AFC8, 0x0045AFCB, 0x0045AFD1, 0x0045AFEB,
        0x0045AFCD, 0x0045AFCF, 0x0045AFDE,
        0x0045AFE0, 0x0045AFE9, 0x0045AFF2, 0x0045AFEE,
        0x004121DF, 0x004121E4,
        0x00412230, 0x0041226B, 0x0041228E, 0x004122C9, 0x004121F6
    };
    uint32_t esp = emu->regs[_SP].dword[0];
    const char* espRange = (esp >= 0x06420000u && esp <= 0x0661FFFFu) ? "in-range" : "OUT-OF-RANGE";
    // IgnitionVita iteration-G111: the markerId ranges that used to map to
    // "[RUN25]"/"[RUN27]"/"[RUN29]" (1-5, 10-11, 13-15) have NO remaining
    // call site in this file (see case 0x74/0x83/0x8B/0xC3/0xFC above) --
    // those tags were this project's hottest log producers (raw fopen/
    // fprintf bypassing the game's own logger) and their call sites were
    // removed entirely, not just gated. Narrowed this ternary to only the
    // markerId values still actually passed, so the retired tag strings no
    // longer exist in the binary at all (not just unreachable).
    const char* tag = (markerId <= 9) ? "[RUN26]" :
                       (markerId == 12) ? "[RUN28]" :
                       (markerId <= 16) ? "[RUN30]" :
                       (markerId <= 18) ? "[IDLE34]" : "[IDLE35]";
    ign_dyn23_log(tag, "marker=%u EIP=0x%08x ESP=0x%08x(%s) EAX=0x%08x EBX=0x%08x ECX=0x%08x "
                  "EDX=0x%08x ESI=0x%08x EDI=0x%08x EBP=0x%08x",
                  markerId, kMarkerEip[markerId], esp, espRange,
                  emu->regs[_AX].dword[0], emu->regs[_BX].dword[0], emu->regs[_CX].dword[0],
                  emu->regs[_DX].dword[0], emu->regs[_SI].dword[0], emu->regs[_DI].dword[0],
                  emu->regs[_BP].dword[0]);
}

// IgnitionVita iteration-29: diagnostic-only replacement for the ONE guest
// REP STOSD at 0x0045AFDE, now that iteration 28 proved on real hardware
// that fixing PUSHFW @ 0x0045AFCF lets execution reach here with EDI/ECX/
// EAX exactly as expected (EDI=0x010810C0, ECX=0x00013C68=81000, EAX=0).
//
// Audit of the STOCK translation (case 0xAB below, F3-prefixed) before
// writing this: it uses MMU_SAI4(STRAI_REG_LSL_IMM5, STR_IMM9, xEAX, xEDI,
// x3, 0, x14), which -- unlike the PUSHFW bug -- IS the correct pattern:
// it computes a translated host scratch register (scr = xEDI + membase)
// and stores THROUGH that, while advancing xEDI (the GUEST address) by
// the untranslated add. Same shape as the known-good MMU_PUSH1/MMU_PUSH16.
// So this is NOT the same class of bug as PUSHFW -- whatever crashes here
// is still unidentified (could be the B_MARK/MARK loop-branch encoding at
// this trip count, could be something else). This override sidesteps the
// question entirely for THIS one instruction by doing the identical
// architectural effect from native C, through the same guest->host
// translation math (host = guest + dyn86_membase) as every working MMU_*
// helper, in chunks, with per-chunk breadcrumbs.
static void ign_repstosd29_override(x86emu_t* emu)
{
    uint32_t edi = emu->regs[_DI].dword[0];
    uint32_t ecx = emu->regs[_CX].dword[0];
    uint32_t eax = emu->regs[_AX].dword[0];
    uint32_t totalBytes = ecx * 4u;
    // Known bounds of the one allocation this override is scoped to
    // (already validated this session: base+size fits, first/last dword
    // writable). Re-used here rather than re-deriving from GuestRegion,
    // per this iteration's "no volver a auditar heap" instruction.
    const uint32_t kAllocBase = 0x010810C0u;
    const uint32_t kAllocSize = 324000u;

    ign_dyn23_log("[REP29]", "BEGIN EIP=0x0045AFDE EDI=0x%08x ECX=%u(0x%08x) EAX=0x%08x bytes=%u",
                  edi, ecx, ecx, eax, totalBytes);

    if (edi < kAllocBase || (uint64_t)edi + (uint64_t)totalBytes > (uint64_t)kAllocBase + (uint64_t)kAllocSize) {
        ign_dyn23_log("[REP29]", "STOP invalid range EDI=0x%08x bytes=%u alloc=0x%08x..0x%08x",
                      edi, totalBytes, kAllocBase, kAllocBase + kAllocSize - 1u);
        return; // diagnostic-only clean stop: EDI/ECX left exactly as they were
    }

    const uint32_t kChunkDwords = 1024u; // 4096 bytes/chunk
    uint32_t remaining = ecx;
    uint32_t curEdi = edi;
    uint32_t chunkNum = 0;
    while (remaining > 0u) {
        uint32_t n = (remaining > kChunkDwords) ? kChunkDwords : remaining;
        uint32_t* hostPtr = (uint32_t*)((uintptr_t)curEdi + dyn86_membase);
        for (uint32_t i = 0; i < n; i++) hostPtr[i] = eax;
        curEdi += n * 4u;
        remaining -= n;
        chunkNum++;
        if (chunkNum == 1u || chunkNum == 8u || chunkNum == 16u || chunkNum == 32u ||
            chunkNum == 48u || chunkNum == 64u || remaining == 0u) {
            ign_dyn23_log("[REP29]", remaining == 0u ? "final_chunk=%u EDI=0x%08x remaining_dwords=%u"
                                                       : "chunk=%u EDI=0x%08x remaining_dwords=%u",
                          chunkNum, curEdi, remaining);
        }
    }

    emu->regs[_DI].dword[0] = curEdi;
    emu->regs[_CX].dword[0] = 0u;
    // EAX/EBX/EDX/ESI/EBP/ESP and all flags intentionally untouched.

    // iteration-30: re-read straight from the emu struct (not the local
    // curEdi) to distinguish "the struct write never stuck" from "the
    // struct write stuck but the ARM register cache lost it before
    // marker 13's STM captured it" -- see the iteration-30 delivery notes
    // for why this differential matters.
    ign_dyn23_log("[REP29]", "COMPLETE EDI=0x%08x(local) EDI_restruct=0x%08x ECX=0 next_EIP=0x0045AFE0",
                  curEdi, emu->regs[_DI].dword[0]);
}

// IgnitionVita iteration-33: passive per-iteration probe on WinMain's
// message loop head (VA 0x004121A2, byte-verified `A1 84 53 4C 00` = MOV
// EAX,[0x004C5384]). Exhaustively confirmed via a full .text scan (only 3
// references to this address exist in the whole binary: this read, the
// WM_ACTIVATEAPP handler's write at 0x0041238F, and an unrelated read
// inside the clock-update helper at 0x004124C9) that there is exactly ONE
// writer, and real hardware already showed it sets the flag to 1 and
// stays there. This marker exists to find out, empirically, whether the
// loop instance that ultimately calls the blocking GetMessageA (ip
// 0x004121F4) ever actually sees flag==0, or whether that call is
// reached some other way entirely -- resolving that ambiguity needs
// runtime evidence, not more static reasoning.
//
// Reads guest memory directly (dyn86_membase-translated, same math as
// every MMU_* helper) rather than depending on any ARM register cache
// state, and never writes anything -- so unlike the iteration-28/29/30
// overrides, this needs no STM/LDM around its CALL_S at all: nothing to
// flush in, nothing to flush back out.
//
// Logging is capped (first 16 iterations, then transition-only) per this
// iteration's explicit low-noise requirement -- this loop could spin
// indefinitely once truly idling, and every iteration writing to
// ux0:data over FIOS would be its own performance problem.
static void ign_loop33_marker(x86emu_t* emu)
{
    static uint32_t s_iter = 0;
    static uint32_t s_lastFlag = 0xFFFFFFFFu;   // sentinel: force first log
    ++s_iter;

    uint32_t flag = 0;
    if (dyn86_membase) {
        flag = *(uint32_t*)((uintptr_t)0x004C5384u + dyn86_membase);
    }
    uint32_t queueDepth = g_ign_msg_queue_depth;
    const char* path = flag ? "ACTIVE" : "INACTIVE";

    int shouldLog = (s_iter <= 16u) || (flag != s_lastFlag);
    if (shouldLog) {
        ign_dyn23_log("[LOOP33]", "iter=%u flag_4C5384=0x%08x queue_depth=%u path=%s",
                      s_iter, flag, queueDepth, path);
    }
    s_lastFlag = flag;
}

// IgnitionVita iteration-35: priority marker at 0x004121F4 (`FF D7` =
// CALL EDI, the ONLY guest instruction in the whole binary that can call
// GetMessageA and actually block -- confirmed exhaustively in iteration
// 34: GetMessageA's IAT slot 0x0064C450 is loaded into EDI exactly once,
// at 0x00412192, and every CALL EDI while that value holds is either
// 0x004121C9 (Peek-confirmed, never blocks) or this one). Fires
// unconditionally before the real CALL executes -- if this line is
// MISSING from a real-hardware log, the STOP is NOT coming from this
// instruction at all, definitively ruling it out instead of assuming it.
// Reads the flag directly from guest memory (same as ign_loop33_marker),
// independent of whatever the ARM register cache thinks -- this is the
// direct, decisive answer to "is flag_4C5384 really 0 here" instead of
// inferring it from the branch that was taken to arrive.
static void ign_gm35_precall(x86emu_t* emu)
{
    uint32_t flag = 0;
    if (dyn86_membase) {
        flag = *(uint32_t*)((uintptr_t)0x004C5384u + dyn86_membase);
    }
    ign_dyn23_log("[GM35]", "PRECALL_BLOCKING EIP=0x004121F4 EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x "
                  "ESI=0x%08x EDI=0x%08x EBP=0x%08x ESP=0x%08x flag_4C5384=0x%08x",
                  emu->regs[_AX].dword[0], emu->regs[_BX].dword[0], emu->regs[_CX].dword[0],
                  emu->regs[_DX].dword[0], emu->regs[_SI].dword[0], emu->regs[_DI].dword[0],
                  emu->regs[_BP].dword[0], emu->regs[_SP].dword[0], flag);
}
// ============================================================================

uintptr_t dynarec00(dynarec_arm_t* dyn, uintptr_t addr, uintptr_t ip, int ninst, int* ok, int* need_epilog)
{
    uint8_t nextop, opcode;
    uint8_t gd, ed;
    int8_t i8;
    int32_t i32, j32, tmp;
    uint8_t u8;
    uint8_t gb1, gb2, eb1, eb2;
    uint32_t u32;
    uint8_t wback, wb1, wb2;
    int fixedaddress;
    int lock;
    int cacheupd;

    opcode = F8;

    // iteration-23's generic per-instruction [DYN23] probe REMOVED here
    // (section 23's instruction): it already answered its question --
    // confirmed instruction_start=0x0045AFDE, opcode(prefix)=0xF3, and
    // (inside case 0xAB below) addr_after=0x0045AFE0. Table, from that real
    // hardware log:
    //   stage                        value
    //   ---------------------------------------
    //   instruction start (= ip)     0x0045AFDE   (the F3 byte -- ip is
    //                                              fixed once per dynarec00()
    //                                              call, set by the caller
    //                                              in dynarec_arm_pass.c:
    //                                              `ip = addr;`, and is NEVER
    //                                              reassigned inside this
    //                                              function -- confirmed by
    //                                              reading the source, not
    //                                              assumed)
    //   after consuming F3 (prefix)  0x0045AFDF   (addr, not ip)
    //   when case 0xAB begins        ip is STILL 0x0045AFDE; addr is 0x0045AFE0
    //   after consuming AB (opcode)  0x0045AFE0   (addr == next guest EIP)
    // So iteration 22's `ip == 0x0045AFDE` guard was comparing the RIGHT
    // variable against the RIGHT value -- iteration 23's own probe, using
    // that exact same `ip` parameter at that exact same code location,
    // printed 0x0045afde. The "ip had already advanced" theory does not
    // hold up against this evidence. The guard condition itself was never
    // the bug; iteration 24 keeps it, and separately instruments whether
    // the guarded branch is even reached at RUNTIME (not just emitted at
    // translation time) to find out what actually happened.

    MAYUSE(eb1);
    MAYUSE(eb2);
    MAYUSE(tmp);
    MAYUSE(j32);
    MAYUSE(lock);
    MAYUSE(cacheupd);

    switch(opcode) {
        case 0x00:
            INST_NAME("ADD Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_add8(dyn, ninst, x1, x2, x14, x3, (wb1 && (wback==x3))?1:0);
            EBBACK;
            break;
        case 0x01:
            INST_NAME("ADD Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_add32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x02:
            INST_NAME("ADD Gb, Eb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_add8(dyn, ninst, x1, x2, x3, x14, 0);
            GBBACK;
            break;
        case 0x03:
            INST_NAME("ADD Gd, Ed");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_add32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x04:
            INST_NAME("ADD AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_add8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x05:
            INST_NAME("ADD EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_add32c(dyn, ninst, xEAX, i32, x3, x14);
            break;
        case 0x06:
            INST_NAME("PUSH ES");
            MOVW(x1, offsetof(x86emu_t, segs[_ES]));
            LDRH_REG(x2, xEmu, x1);
            MMU_PUSH1(x2, x3);
            break;
        case 0x07:
            INST_NAME("POP ES");
            MOVW(x1, offsetof(x86emu_t, segs[_ES]));
            MMU_POP1(x2, x3);
            STRH_REG(x2, xEmu, x1);
            MOVW(x1, 0);
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, segs_serial[_ES]));
            break;
        case 0x08:
            INST_NAME("OR Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_or8(dyn, ninst, x1, x2, x14, x2);
            EBBACK;
            break;
        case 0x09:
            INST_NAME("OR Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_or32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x0A:
            INST_NAME("OR Gb, Eb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_or8(dyn, ninst, x1, x2, x3, x14);
            GBBACK;
            break;
        case 0x0B:
            INST_NAME("OR Gd, Ed");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_or32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x0C:
            INST_NAME("OR AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_or8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x0D:
            INST_NAME("OR EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_or32c(dyn, ninst, xEAX, i32, x3, x14);
            break;

        case 0x0F:
            addr = dynarec0F(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0x10:
            INST_NAME("ADC Eb, Gb");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_adc8(dyn, ninst, x1, x2, x14, x3, (wb1 && (wback==x3))?1:0);
            EBBACK;
            break;
        case 0x11:
            INST_NAME("ADC Ed, Gd");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_adc32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x12:
            INST_NAME("ADC Gb, Eb");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_adc8(dyn, ninst, x1, x2, x14, x3, 0);
            GBBACK;
            break;
        case 0x13:
            INST_NAME("ADC Gd, Ed");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_adc32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x14:
            INST_NAME("ADC AL, Ib");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_adc8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x15:
            INST_NAME("ADC EAX, Id");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_adc32c(dyn, ninst, xEAX, i32, x3, x14);
            break;

        case 0x18:
            INST_NAME("SBB Eb, Gb");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_sbb8(dyn, ninst, x1, x2, x14, x3, (wb1 && (wback==x3))?1:0);
            EBBACK;
            break;
        case 0x19:
            INST_NAME("SBB Ed, Gd");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_sbb32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x1A:
            INST_NAME("SBB Gb, Eb");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_sbb8(dyn, ninst, x1, x2, x3, x14, 0);
            GBBACK;
            break;
        case 0x1B:
            INST_NAME("SBB Gd, Ed");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_sbb32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x1C:
            INST_NAME("SBB AL, Ib");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_sbb8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x1D:
            INST_NAME("SBB EAX, Id");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_sbb32c(dyn, ninst, xEAX, i32, x3, x14);
            break;
        case 0x1E:
            INST_NAME("PUSH DS");
            MOVW(x1, offsetof(x86emu_t, segs[_DS]));
            LDRH_REG(x2, xEmu, x1);
            MMU_PUSH1(x2, x3);
            break;
        case 0x1F:
            INST_NAME("POP DS");
            MOVW(x1, offsetof(x86emu_t, segs[_DS]));
            MMU_POP1(x2, x3);
            STRH_REG(x2, xEmu, x1);
            MOVW(x1, 0);
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, segs_serial[_DS]));
            break;
        case 0x20:
            INST_NAME("AND Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_and8(dyn, ninst, x1, x2, x14, x2);
            EBBACK;
            break;
        case 0x21:
            INST_NAME("AND Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_and32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x22:
            INST_NAME("AND Gb, Eb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_and8(dyn, ninst, x1, x2, x3, x14);
            GBBACK;
            break;
        case 0x23:
            INST_NAME("AND Gd, Ed");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_and32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x24:
            INST_NAME("AND AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_and8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x25:
            INST_NAME("AND EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_and32c(dyn, ninst, xEAX, i32, x3, x14);
            break;
        case 0x26:
            INST_NAME("ES:");
            // ignored
            break;
        case 0x27:
            INST_NAME("DAA");
            MESSAGE(LOG_DUMP, "Need Optimization DAA\n");
            READFLAGS(X_AF|X_CF);
            SETFLAGS(X_ALL, SF_SET_DF);
            UXTB(x1, xEAX, 0);
            CALL_(daa8, x1, 0);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x28:
            INST_NAME("SUB Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_sub8(dyn, ninst, x1, x2, x14, x3, (wb1 && (wback==x3))?1:0);
            EBBACK;
            break;
        case 0x29:
            INST_NAME("SUB Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_sub32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x2A:
            INST_NAME("SUB Gb, Eb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_sub8(dyn, ninst, x1, x2, x3, x14, 0);
            GBBACK;
            break;
        case 0x2B:
            INST_NAME("SUB Gd, Ed");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_sub32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x2C:
            INST_NAME("SUB AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_sub8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x2D:
            INST_NAME("SUB EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_sub32c(dyn, ninst, xEAX, i32, x3, x14);
            break;
        case 0x2E:
            INST_NAME("CS:");
            // ignored
            break;
        case 0x2F:
            INST_NAME("DAS");
            MESSAGE(LOG_DUMP, "Need Optimization DAS\n");
            READFLAGS(X_AF|X_CF);
            SETFLAGS(X_ALL, SF_SET_DF);
            UXTB(x1, xEAX, 0);
            CALL_(das8, x1, 0);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x30:
            INST_NAME("XOR Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_xor8(dyn, ninst, x1, x2, x14, x2);
            EBBACK;
            break;
        case 0x31:
            INST_NAME("XOR Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_xor32(dyn, ninst, ed, gd, x3, x14);
            WBACK;
            break;
        case 0x32:
            INST_NAME("XOR Gb, Eb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_xor8(dyn, ninst, x1, x2, x3, x14);
            GBBACK;
            break;
        case 0x33:
            INST_NAME("XOR Gd, Ed");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            emit_xor32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x34:
            INST_NAME("XOR AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            emit_xor8c(dyn, ninst, x1, u8, x3, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0x35:
            INST_NAME("XOR EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            emit_xor32c(dyn, ninst, xEAX, i32, x3, x14);
            break;
        case 0x36:
            INST_NAME("SS:");
            // ignored
            break;
        case 0x37:
            INST_NAME("AAA");
            MESSAGE(LOG_DUMP, "Need Optimization AAA\n");
            READFLAGS(X_AF);
            SETFLAGS(X_ALL, SF_SET_DF);
            UXTH(x1, xEAX, 0);
            CALL_(aaa16, x1, 0);
            BFI(xEAX, x1, 0, 16);
            break;
        case 0x38:
            INST_NAME("CMP Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x1);
            GETGB(x2);
            emit_cmp8(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0x39:
            INST_NAME("CMP Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETEDH(x1);
            emit_cmp32(dyn, ninst, ed, gd, x3, x14);
            break;
        case 0x3A:
            INST_NAME("CMP Gb, Eb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETEB(x2);
            GETGB(x1);
            emit_cmp8(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0x3B:
            INST_NAME("CMP Gd, Ed");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGD;
            GETEDH(x2);
            emit_cmp32(dyn, ninst, gd, ed, x3, x14);
            break;
        case 0x3C:
            INST_NAME("CMP AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            u8 = F8;
            UXTB(x1, xEAX, 0);
            if(u8) {
                MOVW(x2, u8);
                emit_cmp8(dyn, ninst, x1, x2, x3, x14);
            } else {
                emit_cmp8_0(dyn, ninst, x1, x3, x14);
            }
            break;
        case 0x3D:
            INST_NAME("CMP EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            if(i32) {
                MOV32(x2, i32);
                emit_cmp32(dyn, ninst, xEAX, x2, x3, x14);
            } else {
                emit_cmp32_0(dyn, ninst, xEAX, x3, x14);
            }
            break;
        case 0x3E:
            INST_NAME("DS:");
            // ignored
            break;
        case 0x3F:
            INST_NAME("AAS");
            MESSAGE(LOG_DUMP, "Need Optimization AAS\n");
            READFLAGS(X_AF);
            SETFLAGS(X_ALL, SF_SET_DF);
            UXTH(x1, xEAX, 0);
            CALL_(aas16, x1, 0);
            BFI(xEAX, x1, 0, 16);
            break;
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
            INST_NAME("INC reg");
            SETFLAGS(X_ALL&~X_CF, SF_SUBSET_PENDING);
            gd = xEAX+(opcode&0x07);
            emit_inc32(dyn, ninst, gd, x3, x14);
            break;
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
            INST_NAME("DEC reg");
            SETFLAGS(X_ALL&~X_CF, SF_SUBSET_PENDING);
            gd = xEAX+(opcode&0x07);
            emit_dec32(dyn, ninst, gd, x3, x14);
            break;
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
            // IgnitionVita iteration-26: marker 8 (0x0045AFD1, `PUSH EDI` --
            // reaching this proves TEST EAX,EAX/JZ (not taken) and the
            // 0x66-prefixed PUSHFW right before it all executed without
            // crashing; only CLD @0x0045AFD2 remains unconfirmed after this).
            if (ip == 0x0045AFD1u) {
                MOV32(x1, 8u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("PUSH reg");
            gd = xEAX+(opcode&0x07);
            if(gd==xESP) {
                /* PUSH ESP passe par un AUTRE registre de travail (x2) : il ne
                 * chaine pas, et il coupe la chaine en cours (verrou V4,
                 * dynarec_arm_mmu.h). L'etat est remis a neuf par le fait
                 * meme que mmust_ninst n'est pas mis a jour ici. */
                MOV_REG(x1, gd);
                MMU_PUSH1(x1, x2);
            } else {
                MMU_PUSH1_CH(gd, x1);   /* D2_MMUSTACK : maillon chainable */
            }
            break;
        case 0x58:
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
            // IgnitionVita iteration-30: marker 16 (0x0045AFEE, "POP EDI"
            // -- byte-verified, the epilogue's own restore of the CALLER's
            // EDI, unrelated to the local PUSH EDI/POP EDI bracket around
            // the zero-fill). Passive, shared case (0x58-0x5F covers POP
            // for all 8 registers), guarded by exact `ip` so it only ever
            // fires for this one real occurrence.
            if (ip == 0x0045AFEEu) {
                MOV32(x1, 16u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("POP reg");
            gd = xEAX+(opcode&0x07);
            #ifdef RPI2
            if(gd==xESP) {
                MMU_POP1(x1, x2);
                MOV_REG(gd, x1);
            } else {
                MMU_POP1(gd, x1);    
            }
            #else
            /* POP ESP est EXCLU de la chaine (verrou V4) : apres lui ESP vaut
             * une valeur lue en memoire, l'invariant scr = ESP+base tombe. */
            if(gd==xESP) {
                MMU_POP1(gd, x1);
            } else {
                MMU_POP1_CH(gd, x1);    /* D2_MMUSTACK : maillon chainable */
            }
            #endif
            break;
        case 0x60:
            INST_NAME("PUSHAD");
            MOV_REG(x1, xESP);
            // cannot use PUSH (STMdb!) because order of regs are reversed!
            //PUSH(x1, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESP)|(1<<xEBP)|(1<<xESI)|(1<<xEDI));
            MMU_PUSH1(xEAX, x2);
            MMU_PUSH1(xECX, x2);
            MMU_PUSH1(xEDX, x2);
            MMU_PUSH1(xEBX, x2);
            MMU_PUSH1(x1, x2);
            MMU_PUSH1(xEBP, x2);
            MMU_PUSH1(xESI, x2);
            MMU_PUSH1(xEDI, x2);
            break;
        case 0x61:
            INST_NAME("POPAD");
            //MOV_REG(x1, xESP);
            //POP(x1, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESP)|(1<<xEBP)|(1<<xESI)|(1<<xEDI));
            MMU_POP1(xEDI, x2);
            MMU_POP1(xESI, x2);
            MMU_POP1(xEBP, x2);
            ADD_IMM8(xESP, xESP, 4);    //POP(xESP, (1<<xESP));
            MMU_POP1(xEBX, x2);
            MMU_POP1(xEDX, x2);
            MMU_POP1(xECX, x2);
            MMU_POP1(xEAX, x2);
            //MOV_REG(xESP, x1);
            break;
        case 0x62:
            INST_NAME("BOUND Gd, Ed");
            nextop = F8;
            GETED;
            // no bound test for now
            break;

        case 0x64:
            addr = dynarecFS(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0x65:
            addr = dynarecGS(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0x66:
            addr = dynarec66(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0x67:
            addr = dynarec67(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0x68:
            INST_NAME("PUSH Id");
            i32 = F32S;
            if(PK(0)==0xC3) {
                MESSAGE(LOG_DUMP, "PUSH then RET, using indirect\n");
                MOV32(x3, addr-4 + dyn86_membase);  // fastmmu: addr-4 is guest code
                LDR_IMM9(x1, x3, 0);
                MMU_PUSH1(x1, x2);
            } else {
                MOV32(x3, i32);
                MMU_PUSH1(x3, x1);
            }
            break;
        case 0x69:
            INST_NAME("IMUL Gd, Ed, Id");
            SETFLAGS(X_ALL, SF_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            i32 = F32S;
            MOV32(x14, i32);
            UFLAG_IF {
                SMULL(x3, gd, x14, ed);
                UFLAG_OP1(x3);
                UFLAG_RES(gd);
                UFLAG_DF(x3, d_imul32);
            } else {
                MUL(gd, ed, x14);
            }
            break;
        case 0x6A:
            INST_NAME("PUSH Ib");
            i32 = F8S;
            MOV32(x3, i32);
            MMU_PUSH1(x3, x1);
            break;
        case 0x6B:
            INST_NAME("IMUL Gd, Ed, Ib");
            SETFLAGS(X_ALL, SF_PENDING);
            nextop = F8;
            GETGD;
            GETED;
            i32 = F8S;
            MOV32(x14, i32);
            UFLAG_IF {
                SMULL(x3, gd, x14, ed);
                UFLAG_OP1(x3);
                UFLAG_RES(gd);
                UFLAG_DF(x3, d_imul32);
            } else {
                MUL(gd, ed, x14);
            }
            break;

        #define GO(GETFLAGS, NO, YES, F)                                \
            READFLAGS(F);                                               \
            i8 = F8S;                                                   \
            BARRIER(BARRIER_MAYBE);                                     \
            JUMP(addr+i8, 1);                                           \
            GETFLAGS;                                                   \
            if(dyn->insts[ninst].x86.jmp_insts==-1 ||                   \
                CHECK_CACHE()) {                                        \
                /* out of the block */                                  \
                i32 = dyn->insts[ninst].epilog-(dyn->arm_size+8);       \
                Bcond(NO, i32);                                         \
                if(dyn->insts[ninst].x86.jmp_insts==-1) {               \
                    if(!(dyn->insts[ninst].x86.barrier&BARRIER_FLOAT))  \
                        fpu_purgecache(dyn, ninst, 1, x1, x2, x3);      \
                    jump_to_next(dyn, addr+i8, 0, ninst);               \
                } else {                                                \
                    CacheTransform(dyn, ninst, cacheupd, x1, x2, x3);   \
                    i32 = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8);\
                    Bcond(c__, i32);                                    \
                }                                                       \
            } else {                                                    \
                /* inside the block */                                  \
                i32 = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8);    \
                Bcond(YES, i32);                                        \
            }

        /* D2_SIGNTAG : voir dynarec_arm_signtag.h. Sur les sites ou le Jcc
         * discrimine un POINTEUR COMPLEMENTE d'un deplacement par le BIT DE
         * SIGNE (motif TEST rX,rX / JGE|JNS / NOT rX), on branche sur le
         * BIT 30 au lieu du bit 31 : c'est ce qui rend l'idiome correct quand
         * la memoire invitee vit au-dessus de 2 Gio (D2LAYOUT=haut). Hors
         * motif reconnu ou knob eteint, l'expansion est GO() mot pour mot. */
        #define GOSIGN(GETFLAGS, NO, YES, F)                            \
        {   d2sign_t d2s = d2sign_analyze(dyn, ninst);                  \
            const int d2sd = (dyn86_signtag && d2s.ok) ? 1 : 0;         \
            D2SIGN_TALLY(d2s, d2sd);                                    \
            if(!d2sd) { GO(GETFLAGS, NO, YES, F) }                      \
            else {                                                      \
                READFLAGS(F);                                           \
                i8 = F8S;                                               \
                BARRIER(BARRIER_MAYBE);                                 \
                JUMP(addr+i8, 1);                                       \
                TSTS_IMM8_ROR(xEAX + d2s.reg, 1, 1);   /* rX & 0x40000000 */ \
                if(dyn->insts[ninst].x86.jmp_insts==-1 ||               \
                    CHECK_CACHE()) {                                    \
                    i32 = dyn->insts[ninst].epilog-(dyn->arm_size+8);   \
                    Bcond(NO, i32);                                     \
                    if(dyn->insts[ninst].x86.jmp_insts==-1) {           \
                        if(!(dyn->insts[ninst].x86.barrier&BARRIER_FLOAT)) \
                            fpu_purgecache(dyn, ninst, 1, x1, x2, x3);  \
                        jump_to_next(dyn, addr+i8, 0, ninst);           \
                    } else {                                            \
                        CacheTransform(dyn, ninst, cacheupd, x1, x2, x3); \
                        i32 = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8); \
                        Bcond(c__, i32);                                \
                    }                                                   \
                } else {                                                \
                    i32 = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8); \
                    Bcond(YES, i32);                                    \
                }                                                       \
            }                                                           \
        }

        case 0x70:
            INST_NAME("JO ib");
            //F_OF is 1<<11, so 2048, so 0b10 ROL 10 so 0b01 ROR 11*2
            GO( TSTS_IMM8_ROR(xFlags, 0b10, 0x0b)
                , cEQ, cNE, X_OF)
            break;
        case 0x71:
            INST_NAME("JNO ib");
            GO( TSTS_IMM8_ROR(xFlags, 0b10, 0x0b)
                , cNE, cEQ, X_OF)
            break;
        case 0x72:
            INST_NAME("JC ib");
            GO( TSTS_IMM8(xFlags, 1<<F_CF)
                , cEQ, cNE, X_CF)
            break;
        case 0x73:
            INST_NAME("JNC ib");
            GO( TSTS_IMM8(xFlags, 1<<F_CF)
                , cNE, cEQ, X_CF)
            break;
        case 0x74:
            // IgnitionVita iteration-G111: marker 10 (RUN27, 0x0045AFCD)
            // removed -- this `if (ip==...)`-gated native call_c()/STM/LDM
            // was passive diagnostic-only logging (see the retired
            // ign_marker25() comment block at the top of this file for the
            // full history) confirmed NOT to affect the real JZ translation
            // below (GO() still emits it unchanged). RUN25/RUN27/RUN29 were
            // flooding ignitionvita.log via raw fopen/fprintf that bypasses
            // this project's own logger and is invisible to its overhead
            // accounting -- removed at the producer per explicit
            // authorization, real dynarec logic otherwise untouched.
            INST_NAME("JZ ib");
            GO( TSTS_IMM8(xFlags, 1<<F_ZF)
                , cEQ, cNE, X_ZF)
            break;
        case 0x75:
            INST_NAME("JNZ ib");
            GO( TSTS_IMM8(xFlags, 1<<F_ZF)
                , cNE, cEQ, X_ZF)
            break;
        case 0x76:
            INST_NAME("JBE ib");
            GO( TSTS_IMM8(xFlags, (1<<F_CF)|(1<<F_ZF))
                , cEQ, cNE, X_CF|X_ZF)
            break;
        case 0x77:
            INST_NAME("JNBE ib");
            GO( TSTS_IMM8(xFlags, (1<<F_CF)|(1<<F_ZF))
                , cNE, cEQ, X_CF|X_ZF)
            break;
        case 0x78:
            INST_NAME("JS ib");
            GO( TSTS_IMM8(xFlags, 1<<F_SF)
                , cEQ, cNE, X_SF)
            break;
        case 0x79:
            INST_NAME("JNS ib");
            GOSIGN( TSTS_IMM8(xFlags, 1<<F_SF)
                , cNE, cEQ, X_SF)
            break;
        case 0x7A:
            INST_NAME("JP ib");
            GO( TSTS_IMM8(xFlags, 1<<F_PF)
                , cEQ, cNE, X_PF)
            break;
        case 0x7B:
            INST_NAME("JNP ib");
            GO( TSTS_IMM8(xFlags, 1<<F_PF)
                , cNE, cEQ, X_PF)
            break;
        case 0x7C:
            INST_NAME("JL ib");
            GO( XOR_REG_LSL_IMM5(x1, xFlags, xFlags, F_OF-F_SF);
                TSTS_IMM8_ROR(x1, 0b10, 0x0b)
                , cEQ, cNE, X_SF|X_OF)
            break;
        case 0x7D:
            INST_NAME("JGE ib");
            GOSIGN( XOR_REG_LSL_IMM5(x1, xFlags, xFlags, F_OF-F_SF);
                TSTS_IMM8_ROR(x1, 0b10, 0x0b)
                , cNE, cEQ, X_SF|X_OF)
            break;
        case 0x7E:
            INST_NAME("JLE ib");
            GO( XOR_REG_LSL_IMM5(x1, xFlags, xFlags, F_OF-F_SF);
                ORR_REG_LSL_IMM5(x1, x1, xFlags, F_OF-F_ZF);
                TSTS_IMM8_ROR(x1, 0b10, 0x0b)
                , cEQ, cNE, X_SF|X_OF|X_ZF)
            break;
        case 0x7F:
            INST_NAME("JG ib");
            GO( XOR_REG_LSL_IMM5(x1, xFlags, xFlags, F_OF-F_SF);
                ORR_REG_LSL_IMM5(x1, x1, xFlags, F_OF-F_ZF);
                TSTS_IMM8_ROR(x1, 0b10, 0x0b)
                , cNE, cEQ, X_SF|X_OF|X_ZF)
            break;
        #undef GOSIGN
        #undef GO
        case 0x80:
        case 0x82:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0: //ADD
                    INST_NAME("ADD Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_add8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 1: //OR
                    INST_NAME("OR Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_or8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 2: //ADC
                    INST_NAME("ADC Eb, Ib");
                    READFLAGS(X_CF);
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_adc8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 3: //SBB
                    INST_NAME("SBB Eb, Ib");
                    READFLAGS(X_CF);
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_sbb8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 4: //AND
                    INST_NAME("AND Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_and8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 5: //SUB
                    INST_NAME("SUB Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_sub8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 6: //XOR
                    INST_NAME("XOR Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_xor8c(dyn, ninst, x1, u8, x2, x14);
                    EBBACK;
                    break;
                case 7: //CMP
                    INST_NAME("CMP Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    if(u8) {
                        MOVW(x2, u8);
                        emit_cmp8(dyn, ninst, x1, x2, x3, x14);
                    } else {
                        emit_cmp8_0(dyn, ninst, x1, x3, x14);
                    }
                    break;
            }
            break;
        case 0x81:
        case 0x83:
            // IgnitionVita iteration-25: passive return-chain markers (M1,
            // M2, M4 -- see ign_marker25()'s comment block near the top of
            // this file). Guarded by exact `ip`, so this can never fire for
            // any of the thousands of OTHER `ADD/SUB/CMP/... r/m32,imm8`
            // instructions in the game that also decode through case 0x83.
            // NO `break` -- falls through to the untouched original
            // decoding immediately below, so the real instruction still
            // executes exactly as it always did.
            // IgnitionVita iteration-G111: marker 4 (RUN25) removed -- see
            // the case-0x74 comment above for the full rationale.
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0: //ADD
                    if(opcode==0x81) {
                        INST_NAME("ADD Ed, Id");
                    } else {
                        INST_NAME("ADD Ed, Ib");
                    }
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_add32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 1: //OR
                    if(opcode==0x81) {INST_NAME("OR Ed, Id");} else {INST_NAME("OR Ed, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_or32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 2: //ADC
                    if(opcode==0x81) {INST_NAME("ADC Ed, Id");} else {INST_NAME("ADC Ed, Ib");}
                    READFLAGS(X_CF);
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_adc32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 3: //SBB
                    if(opcode==0x81) {INST_NAME("SBB Ed, Id");} else {INST_NAME("SBB Ed, Ib");}
                    READFLAGS(X_CF);
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_sbb32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 4: //AND
                    if(opcode==0x81) {INST_NAME("AND Ed, Id");} else {INST_NAME("AND Ed, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_and32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 5: //SUB
                    if(opcode==0x81) {INST_NAME("SUB Ed, Id");} else {INST_NAME("SUB Ed, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_sub32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 6: //XOR
                    if(opcode==0x81) {INST_NAME("XOR Ed, Id");} else {INST_NAME("XOR Ed, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    emit_xor32c(dyn, ninst, ed, i32, x3, x14);
                    WBACK;
                    break;
                case 7: //CMP
                    if(opcode==0x81) {INST_NAME("CMP Ed, Id");} else {INST_NAME("CMP Ed, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEDH(x1);
                    if(opcode==0x81) i32 = F32S; else i32 = F8S;
                    if(i32) {
                        MOV32(x2, i32);
                        emit_cmp32(dyn, ninst, ed, x2, x3, x14);
                    } else {
                        emit_cmp32_0(dyn, ninst, ed, x3, x14);
                    }
                    break;
            }
            break;
        case 0x84:
            INST_NAME("TEST Eb, Gb");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop=F8;
            GETEB(x1);
            GETGB(x2);
            emit_test8(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0x85:
            // IgnitionVita iteration-26: marker 7 (0x0045AFCB, `TEST EAX,EAX`
            // -- the NULL check on calloc's fresh pointer, immediately
            // before the JZ that decides whether to skip the zero-fill).
            if (ip == 0x0045AFCBu) {
                MOV32(x1, 7u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            // IgnitionVita iteration-34: marker 18 (0x004121E4, "TEST
            // EAX,EAX" right after "CALL 0x412230" returns from idle-work
            // -- EAX here IS idle-work's return value). Pairs with marker
            // 17 (ENTER) at 0x004121DF: if 17 fires but 18 never does,
            // idle-work itself never returned -- a decisive finding.
            if (ip == 0x004121E4u) {
                // iteration-35 section 3-4: translation-time confirmation,
                // SEPARATE from the runtime marker below -- proves the
                // guard is reached during JIT compilation (i.e. this
                // exact block gets emitted at all), independent of
                // whether the GUEST ever executes it. If [DYN35] appears
                // in the log but [IDLE34] marker=18 never does, that
                // means the block compiles but this guest EIP is never
                // reached at runtime (same distinction iteration 24 used
                // for REP STOSD).
                ign_dyn23_log("[DYN35]", "marker emitted @ 0x004121E4 (translation pass)");
                MOV32(x1, 18u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            // IgnitionVita iteration-35: marker 23, "RETURN_FROM_BLOCKING
            // _GETMESSAGE" (0x004121F6, the TEST EAX,EAX right after CALL
            // EDI @ 0x004121F4). Currently we STOP before this could ever
            // fire (GetMessageA halts the run on an empty queue instead of
            // returning), but it's wired for the day that changes.
            if (ip == 0x004121F6u) {
                MOV32(x1, 23u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("TEST Ed, Gd");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop=F8;
            GETGD;
            GETEDH(x1);
            emit_test32(dyn, ninst, ed, gd, x3, x14);
            break;
        case 0x86:
            INST_NAME("(LOCK)XCHG Eb, Gb");
            // Do the swap
            nextop = F8;
            if(MODREG) {
                GETGB(x14);
                ed = (nextop&7);
                eb1 = xEAX+(ed&3);
                eb2 = ((ed&4)>>2);
                UXTB(x1, eb1, eb2);
                // do the swap 14 -> ed, 1 -> gd
                BFI(gb1, x1, gb2*8, 8);
                BFI(eb1, x14, eb2*8, 8);
            } else {
                GETGB(x14);
                SMDMB();
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 0, 0, 0, LOCK_LOCK);
                MMU_ADDR(ed, x2);
                MARKLOCK;
                // do the swap with exclusive locking
                LDREXB(x1, ed);
                // do the swap 14 -> strb(ed), 1 -> gd
                STREXB(x3, x14, ed);
                CMPS_IMM8(x3, 0);
                B_MARKLOCK(cNE);
                BFI(gb1, x1, gb2*8, 8);
                SMDMB();
            }
            break;
        case 0x87:
            INST_NAME("(LOCK)XCHG Ed, Gd");
            nextop = F8;
            if(MODREG) {
                GETGD;
                GETED;
                if(gd!=ed) {
                    XOR_REG_LSL_IMM5(gd, gd, ed, 0);
                    XOR_REG_LSL_IMM5(ed, gd, ed, 0);
                    XOR_REG_LSL_IMM5(gd, gd, ed, 0);
                }
            } else {
                GETGD;
                SMDMB();
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 0, 0, 1, LOCK_LOCK);
                MMU_ADDR(ed, x2);
                if(!fixedaddress) {
                    TSTS_IMM8(ed, 3);
                    B_MARK(cNE);
                }
                if(!fixedaddress || (fixedaddress && !(fixedaddress&3))) {
                    MARKLOCK;
                    LDREX(x1, ed);
                    STREX(x3, gd, ed);
                    CMPS_IMM8(x3, 0);
                    B_MARKLOCK(cNE);
                }
                if(!fixedaddress) {
                    B_MARK2(c__);
                    MARK;
                }
                if(!fixedaddress || (fixedaddress && (fixedaddress&3))) {
                    LDR_IMM9(x1, ed, 0);
                    STR_IMM9(gd, ed, 0);
                }
                if(!fixedaddress) {
                    MARK2;
                }
                SMDMB();
                MOV_REG(gd, x1);
            }
            break;
        case 0x88:
            INST_NAME("MOV Eb, Gb");
            nextop = F8;
            gd = (nextop&0x38)>>3;
            gb2 = ((gd&4)>>2);
            gb1 = xEAX+(gd&3);
            if(gb2) {
                gd = x14;
                UXTB(gd, gb1, gb2);
            } else {
                gd = gb1;   // no need to extract
            }
            if(MODREG) {
                ed = (nextop&7);
                eb1 = xEAX+(ed&3);  // Ax, Cx, Dx or Bx
                eb2 = ((ed&4)>>2);    // L or H
                BFI(eb1, gd, eb2*8, 8);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 4095, 0, 0, &lock);
                MMU_ADDR(ed, x2);
                STRB_IMM9(gd, ed, fixedaddress);
                SMWRITELOCK(lock);
            }
            break;
        case 0x89:
            // IgnitionVita iteration-26: marker 6 (0x0045AFC8, `MOV [EBP-4],EAX`
            // -- spills the fresh calloc pointer to calloc's own stack
            // temp, immediately after M4). Passive, non-replacing, same
            // pattern as iteration 25's markers.
            if (ip == 0x0045AFC8u) {
                MOV32(x1, 6u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("MOV Ed, Gd");
            nextop=F8;
            GETGD;
            if(MODREG) {   // reg <= reg
                MOV_REG(xEAX+(nextop&7), gd);
            } else {                    // mem <= reg
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 4095, 0, 0, &lock);
                MMU_ADDR(ed, x2);
                STR_IMM9(gd, ed, fixedaddress);
                SMWRITELOCK(lock);
            }
            break;
        case 0x8A:
            INST_NAME("MOV Gb, Eb");
            nextop = F8;
            gd = (nextop&0x38)>>3;
            gb1 = xEAX+(gd&3);
            gb2 = ((gd&4)>>2);
            if(MODREG) {
                    wback = (nextop&7);
                    wb2 = (wback>>2);
                    wback = xEAX+(wback&3);
                    if(wb2) {
                        UXTB(x14, wback, wb2);
                        ed = x14;
                    } else {
                        ed = wback;
                    }
                } else {
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress, 4095, 0, 0, &lock);
                    MMU_ADDR(wback, x3);
                    SMREADLOCK(lock);
                    LDRB_IMM9(x14, wback, fixedaddress);
                    ed = x14;
                }
            BFI(gb1, ed, gb2*8, 8);
            break;
        case 0x8B:
            // IgnitionVita iteration-25: marker 3 (entry to calloc's
            // allocator-dispatch function, 0x0045AE10). Same passive,
            // non-replacing pattern as the case-0x83 markers above.
            // IgnitionVita iteration-G111: marker 3 (RUN25) removed -- see
            // the case-0x74 comment above for the full rationale.
            // IgnitionVita iteration-26: marker 9 (0x0045AFEB, the JZ-at-
            // 0x0045AFCD's own branch TARGET -- reached only if EAX WAS
            // treated as NULL/zero and the zero-fill path was skipped. EAX
            // was 0x010810c0 (nonzero) at M4/marker7, so this firing would
            // itself be direct evidence of a flags/branch bug in the
            // dynarec, not a crash cause).
            if (ip == 0x0045AFEBu) {
                MOV32(x1, 9u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            // IgnitionVita iteration-29: marker 13 (0x0045AFE0, "MOV ECX,
            // [EBP+0C]" -- byte-verified real next instruction after REP
            // STOSD @ 0x0045AFDE, reloading the size for the trailing
            // REP STOSB remainder). Reaching this proves the REP STOSD
            // override completed and control returned to the right place.
            // IgnitionVita iteration-G111: marker 13 (RUN29) removed -- see
            // the case-0x74 comment above for the full rationale.
            // IgnitionVita iteration-35: marker 19, "TARGET_ENTER"
            // (0x00412230, byte-verified "8B 0D 34 37 49 00" = MOV ECX,
            // [0x00493734] -- idle-work's own first instruction, the
            // target of the CALL at 0x004121DF/marker 17).
            if (ip == 0x00412230u) {
                MOV32(x1, 19u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("MOV Gd, Ed");
            nextop=F8;
            GETGD;
            if(MODREG) {   // reg <= reg
                MOV_REG(gd, xEAX+(nextop&7));
            } else {                    // mem <= reg
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 4095, 0, 0, &lock);
                MMU_ADDR(ed, x2);
                SMREADLOCK(lock);
                LDR_IMM9(gd, ed, fixedaddress);
            }
            break;
        case 0x8C:
            INST_NAME("MOV Ed, Seg");
            nextop=F8;
            MOV32(x3, offsetof(x86emu_t, segs[(nextop&0x38)>>3]));
            if(MODREG) {   // reg <= seg
                LDRH_REG(xEAX+(nextop&7), xEmu, x3);
            } else {                    // mem <= seg
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 0, 0, 0, NULL);
                MMU_ADDR(ed, x2);
                LDRH_REG(x3, xEmu, x3);
                STRH_IMM8(x3, ed, fixedaddress);
                SMWRITE2();
            }
            break;
        case 0x8D:
            INST_NAME("LEA Gd, Ed");
            nextop=F8;
            GETGD;
            if(MODREG) {   // reg <= reg? that's an invalid operation
                DEFAULT;
            } else {                    // mem <= reg
                addr = geted(dyn, addr, ninst, nextop, &ed, gd, &fixedaddress, 0, 0, 0, NULL);
                if(gd!=ed) {    // it's sometimes used as a 3 bytes NOP
                    MOV_REG(gd, ed);
                }
            }
            break;
        case 0x8E:
            INST_NAME("MOV Seg,Ew");
            nextop = F8;
            if(MODREG) {
                ed = xEAX+(nextop&7);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 255, 0, 0, NULL);
                MMU_ADDR(ed, x2);
                LDRH_IMM8(x1, ed, fixedaddress);
                ed = x1;
            }
            MOV32(x2, offsetof(x86emu_t, segs[(nextop&0x38)>>3]));
            STRH_REG(ed, xEmu, x2);
            if(((nextop&0x38)>>3)==_FS) {
                // update default_fs, hack for wine
                MOV32(x2, &default_fs);
                STRH_IMM8(ed, x2, 0);
            }
            MOVW(x1, 0);
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, segs_serial[(nextop&0x38)>>3]));
            break;
        case 0x8F:
            INST_NAME("POP Ed");
            nextop = F8;
            if(MODREG) {
                MMU_POP1(xEAX+(nextop&7), x1);
            } else {
                MMU_POP1(x2, x3); // so this can handle POP [ESP] and maybe some variant too
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, 4095, 0, 0, NULL);
                MMU_ADDR(ed, x1);
                if(ed==xESP) {
                    STR_IMM9(x2, ed, fixedaddress);
                } else {
                    // complicated to just allow a segfault that can be recovered correctly
                    SUB_IMM8(xESP, xESP, 4);
                    STR_IMM9(x2, ed, fixedaddress);
                    ADD_IMM8(xESP, xESP, 4);
                }
            }
            break;
        case 0x90:
            INST_NAME("NOP");
            break;
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
            INST_NAME("XCHG EAX, Reg");
            gd = xEAX+(opcode&0x07);
            MOV_REG(x2, xEAX);
            MOV_REG(xEAX, gd);
            MOV_REG(gd, x2);
            break;
        case 0x98:
            INST_NAME("CWDE");
            SXTH(xEAX, xEAX, 0);
            break;
        case 0x99:
            INST_NAME("CDQ");
            MOV_REG_ASR_IMM5(xEDX, xEAX, 0);    // 0 in ASR means only bit #31 everywhere
            break;

        case 0x9B:
            INST_NAME("FWAIT");
            break;
        case 0x9C:
            INST_NAME("PUSHF");
            READFLAGS(X_ALL);
            MMU_PUSH1(xFlags, x2);
            break;
        case 0x9D:
            INST_NAME("POPF");
            SETFLAGS(X_ALL, SF_SET_NODF);
            MMU_POP1(xFlags, x2);
            MOV32(x1, 0x3F7FD7);
            AND_REG_LSL_IMM5(xFlags, xFlags, x1, 0);
            ORR_IMM8(xFlags, xFlags, 2, 0);
            SET_DFNONE(x1);
            if(box86_wine || 1) {    // should this be done all the time?
                TSTS_IMM8_ROR(xFlags, 0x1, 12); // 0x100 == F_TF
                B_NEXT(cEQ);
                // go to epilog, TF should trigger at end of next opcode, so using Interpretor only
                jump_to_epilog(dyn, addr, 0, ninst);
            }
            break;
        case 0x9E:
            INST_NAME("SAHF");
            SETFLAGS(X_CF|X_PF|X_AF|X_ZF|X_SF, SF_SUBSET);
            SET_DFNONE(x1);	
            BIC_IMM8(xFlags, xFlags, 0b11010101, 0);
            UXTB(x1, xEAX, 1);
            AND_IMM8(x1, x1, 0b11010101);
            ORR_REG_LSL_IMM5(xFlags, xFlags, x1, 0);
            break;
        case 0x9F:
            INST_NAME("LAHF");
            READFLAGS(X_CF|X_PF|X_AF|X_ZF|X_SF);
            BFI(xEAX, xFlags, 8, 8);
            break;
        case 0xA0:
            INST_NAME("MOV AL, Ob");
            u32 = F32;
            MOV32(x2, u32 + dyn86_membase);  /* fastmmu: moffs folded at translation */
            if(isLockAddress(u32)) lock=1; else lock = 0;
            SMREADLOCK(lock);
            LDRB_IMM9(x2, x2, 0);
            BFI(xEAX, x2, 0, 8);
            break;
        case 0xA1:
            // IgnitionVita iteration-33: marker at WinMain's loop head
            // (0x004121A2). See ign_loop33_marker()'s comment above for
            // the full rationale. No STM/LDM needed (pure observation,
            // reads guest memory directly, touches no x86 register),
            // no `break`/skip -- the real MOV EAX,[0x4C5384] still runs
            // immediately after, completely unchanged.
            if (ip == 0x004121A2u) {
                CALL_S(ign_loop33_marker, -1, 0);
            }
            INST_NAME("MOV EAX, Od");
            u32 = F32;
            MOV32(x2, u32 + dyn86_membase);  /* fastmmu: moffs folded at translation */
            if(isLockAddress(u32)) lock=1; else lock = 0;
            SMREADLOCK(lock);
            LDR_IMM9(xEAX, x2, 0);
            break;
        case 0xA2:
            INST_NAME("MOV Ob, AL");
            u32 = F32;
            MOV32(x2, u32 + dyn86_membase);  /* fastmmu: moffs folded at translation */
            if(isLockAddress(u32)) lock=1; else lock = 0;
            STRB_IMM9(xEAX, x2, 0);
            SMWRITELOCK(lock);
            break;
        case 0xA3:
            INST_NAME("MOV Od, EAX");
            u32 = F32;
            MOV32(x2, u32 + dyn86_membase);  /* fastmmu: moffs folded at translation */
            if(isLockAddress(u32)) lock=1; else lock = 0;
            STR_IMM9(xEAX, x2, 0);
            SMWRITELOCK(lock);
            break;
        case 0xA4:
            INST_NAME("MOVSB");
            GETDIR(x3, 1);
            SMREAD();
            MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x1, xESI, x3, 0, x14);
            MMU_SAI4(STRBAI_REG_LSL_IMM5, STRB_IMM9, x1, xEDI, x3, 0, x14);
            SMWRITE();
            break;
        case 0xA5:
            INST_NAME("MOVSD");
            GETDIR(x3, 4);
            SMREAD();
            MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x1, xESI, x3, 0, x14);
            MMU_SAI4(STRAI_REG_LSL_IMM5, STR_IMM9, x1, xEDI, x3, 0, x14);
            SMWRITE();
            break;
        case 0xA6:
            INST_NAME("CMPSB");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            GETDIR(x3, 1);
            MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x1, xESI, x3, 0, x14);
            MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x2, xEDI, x3, 0, x14);
            emit_cmp8(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0xA7:
            INST_NAME("CMPSD");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            GETDIR(x3, 4);
            MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x1, xESI, x3, 0, x14);
            MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x2, xEDI, x3, 0, x14);
            emit_cmp32(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0xA8:
            INST_NAME("TEST AL, Ib");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            UXTB(x1, xEAX, 0);
            u8 = F8;
            MOVW(x2, u8);
            emit_test8(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0xA9:
            INST_NAME("TEST EAX, Id");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            i32 = F32S;
            MOV32(x2, i32);
            emit_test32(dyn, ninst, xEAX, x2, x3, x14);
            break;
        case 0xAA:
            INST_NAME("STOSB");
            GETDIR(x3, 1);
            MMU_SAI4(STRBAI_REG_LSL_IMM5, STRB_IMM9, xEAX, xEDI, x3, 0, x14);
            SMWRITE();
            break;
        case 0xAB:
            INST_NAME("STOSD");
            GETDIR(x3, 4);
            MMU_SAI4(STRAI_REG_LSL_IMM5, STR_IMM9, xEAX, xEDI, x3, 0, x14);
            SMWRITE();
            break;
        case 0xAC:
            INST_NAME("LODSB");
            GETDIR(x3, 1);
            MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x1, xESI, x3, 0, x14);
            BFI(xEAX, x1, 0, 8);
            break;
        case 0xAD:
            INST_NAME("LODSD");
            GETDIR(x3, 4);
            MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, xEAX, xESI, x3, 0, x14);
            break;
        case 0xAE:
            INST_NAME("SCASB");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            GETDIR(x3, 1);
            UXTB(x1, xEAX, 0);
            MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x2, xEDI, x3, 0, x14);
            emit_cmp8(dyn, ninst, x1, x2, x3, x14);
            break;
        case 0xAF:
            INST_NAME("SCASD");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            GETDIR(x3, 4);
            MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x2, xEDI, x3, 0, x14);
            emit_cmp32(dyn, ninst, xEAX, x2, x3, x14);
            break;
        case 0xB0:
        case 0xB1:
        case 0xB2:
        case 0xB3:
            INST_NAME("MOV xL, Ib");
            u8 = F8;
            MOVW(x1, u8);
            gb1 = xEAX+(opcode&3);
            BFI(gb1, x1, 0, 8);
            break;
        case 0xB4:
        case 0xB5:
        case 0xB6:
        case 0xB7:
            INST_NAME("MOV xH, Ib");
            u8 = F8;
            MOVW(x1, u8);
            gb1 = xEAX+(opcode&3);
            BFI(gb1, x1, 8, 8);
            break;
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF:
            INST_NAME("MOV Reg, Id");
            gd = xEAX+(opcode&7);
            i32 = F32S;
            MOV32(gd, i32);
            break;

        case 0xC0:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("ROL Eb, Ib");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_rol8c(dyn, ninst, x1, u8&0x1f, x2, x14);
                    EBBACK;
                    break;
                case 1:
                    INST_NAME("ROR Eb, Ib");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    emit_ror8c(dyn, ninst, x1, u8&0x1f, x2, x14);
                    EBBACK;
                    break;
                case 2:
                    INST_NAME("RCL Eb, Ib");
                    MESSAGE(LOG_DUMP, "Need Optimization RCL\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    GETEB(x1);
                    u8 = F8;
                    MOVW(x2, u8);
                    CALL_(rcl8, ed, (1<<x3));
                    EBBACK;
                    break;
                case 3:
                    INST_NAME("RCR Eb, Ib");
                    MESSAGE(LOG_DUMP, "Need Optimization RCR\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    GETEB(x1);
                    u8 = F8;
                    MOVW(x2, u8);
                    CALL_(rcr8, ed, (1<<x3));
                    EBBACK;
                    break;
                case 4:
                case 6:
                    INST_NAME("SHL Eb, Ib");
                    if(geted_ib(dyn, addr, ninst, nextop)&0x1f) {
                        SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                        GETEB(x1);
                        u8 = (F8)&0x1f;
                        emit_shl8c(dyn, ninst, ed, u8, x2, x14);
                        EBBACK;
                    } else {
                        FAKEED;
                        F8;
                    }
                    break;
                case 5:
                    INST_NAME("SHR Eb, Ib");
                    if(geted_ib(dyn, addr, ninst, nextop)&0x1f) {
                        SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                        GETEB(x1);
                        u8 = (F8)&0x1f;
                        emit_shr8c(dyn, ninst, ed, u8, x2, x14);
                        EBBACK;
                    } else {
                        FAKEED;
                        F8;
                    }
                    break;
                case 7:
                    INST_NAME("SAR Eb, Ib");
                    if(geted_ib(dyn, addr, ninst, nextop)&0x1f) {
                        SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                        GETSEB(x1);
                        u8 = (F8)&0x1f;
                        emit_sar8c(dyn, ninst, ed, u8, x2, x14);
                        EBBACK;
                    } else {
                        FAKEED;
                        F8;
                    }
                    break;
            }
            break;
        case 0xC1:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("ROL Ed, Ib");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETED;
                    u8 = (F8)&0x1f;
                    emit_rol32c(dyn, ninst, ed, u8, x3, x14);
                    if(u8) { WBACK; }
                    break;
                case 1:
                    INST_NAME("ROR Ed, Ib");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETED;
                    u8 = (F8)&0x1f;
                    emit_ror32c(dyn, ninst, ed, u8, x3, x14);
                    if(u8) { WBACK; }
                    break;
                case 2:
                    INST_NAME("RCL Ed, Ib");
                    MESSAGE(LOG_DUMP, "Need Optimization RCL\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    GETEDW(x14, x1);
                    u8 = F8;
                    MOVW(x2, u8);
                    CALL_(rcl32, ed, (1<<x14));
                    WBACK;
                    break;
                case 3:
                    INST_NAME("RCR Ed, Ib");
                    READFLAGS(X_CF);
                    GETEDW(x14, x1);
                    u8 = F8&0x1f;
                    if(u8==1) {
                        SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                        IFX(X_PEND) {
                            STR_IMM9(ed, xEmu, offsetof(x86emu_t, op1));
                            MOVW(x3, u8);
                            STR_IMM9(x3, xEmu, offsetof(x86emu_t, op2));
                            SET_DF(x3, d_rcr32);
                        } else IFX(X_ALL) {
                            SET_DFNONE(x3);
                        }
                        IFX(X_OF) {
                            XOR_REG_LSR_IMM8(x3, xFlags, ed, 31);
                            BFI(xFlags, x3, F_OF, 1);
                        }
                        MOVS_REG_LSR_IMM5(x3, xFlags, 1);    // load CC (F_CF==0) into ARM CF
                        MOVS_REG_RRX(ed, ed);
                        IFX(X_PEND) {
                            STR_IMM9(ed, xEmu, offsetof(x86emu_t, res));
                        }
                        IFX(X_CF) {
                            ORR_IMM8_COND(cCS, xFlags, xFlags, 1, 0);
                            BFC_COND(cCC, xFlags, F_CF, 1);

                        }
                        WBACK;
                    } else if(u8>1) {
                        MESSAGE(LOG_DUMP, "Need Optimization RCR\n");
                        SETFLAGS(X_OF|X_CF, SF_SET_DF);
                        MOVW(x2, u8);
                        CALL_(rcr32, ed, (1<<x14));
                        WBACK;
                    }
                    break;
                case 4:
                case 6:
                    INST_NAME("SHL Ed, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETED;
                    u8 = (F8)&0x1f;
                    emit_shl32c(dyn, ninst, ed, u8, x3, x14);
                    WBACK;
                    break;
                case 5:
                    INST_NAME("SHR Ed, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETED;
                    u8 = (F8)&0x1f;
                    emit_shr32c(dyn, ninst, ed, u8, x3, x14);
                    if(u8) {
                        WBACK;
                    }
                    break;
                case 7:
                    INST_NAME("SAR Ed, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETED;
                    u8 = (F8)&0x1f;
                    emit_sar32c(dyn, ninst, ed, u8, x3, x14);
                    if(u8) {
                        WBACK;
                    }
                    break;
            }
            break;
        case 0xC2:
            INST_NAME("RETN");
            //SETFLAGS(X_ALL,_NODF SF_SET_NODF);    // Hack, set all flags (to an unknown state...)
            READFLAGS(X_PEND);  // lets play safe here too
            fpu_purgecache(dyn, ninst, 1, x1, x2, x3); // using next, even if there no next
            i32 = F16;
            retn_to_epilog(dyn, ninst, i32);
            *need_epilog = 0;
            *ok = 0;
            break;
        case 0xC3:
            // IgnitionVita iteration-29: marker 15 (0x0045AFF2, calloc's
            // own RET, byte-verified). Passive, does not affect the real
            // RET translation below in any way.
            // IgnitionVita iteration-G111: marker 15 (RUN29) removed -- see
            // the case-0x74 comment above for the full rationale.
            // IgnitionVita iteration-35: idle-work's own 3 RET points
            // (byte-verified this iteration: 0x41226B, 0x41228E, 0x4122C9
            // -- the function has NO tail-calls/JMP-out, every exit is a
            // plain RET). Markers 20/21/22 -- if 19 (TARGET_ENTER) fires
            // but NONE of these three ever do, idle-work's call tree does
            // not return via any of its own known RETs (a real, decisive
            // finding: either a deeper callee never returns either, or
            // control leaves via something other than these 3 addresses).
            if (ip == 0x0041226Bu) {
                MOV32(x1, 20u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            if (ip == 0x0041228Eu) {
                MOV32(x1, 21u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            if (ip == 0x004122C9u) {
                MOV32(x1, 22u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("RET");
            // SETFLAGS(X_ALL,_NODF SF_SET_NODF);    // Hack, set all flags (to an unknown state...)
            // ^^^ that hack break PlantsVsZombies and GOG Setup under wine....
            READFLAGS(X_PEND);  // so instead, force the defered flags, so it's not too slow, and flags are not lost
            fpu_purgecache(dyn, ninst, 1, x1, x2, x3); // using next, even if there no next
            ret_to_epilog(dyn, ninst);
            *need_epilog = 0;
            *ok = 0;
            break;

        case 0xC6:
            INST_NAME("MOV Eb, Ib");
            nextop=F8;
            if(MODREG) {   // reg <= u8
                u8 = F8;
                ed = (nextop&7);
                eb1 = xEAX+(ed&3);  // Ax, Cx, Dx or Bx
                eb2 = (ed&4)>>2;    // L or H
                MOVW(x3, u8);
                BFI(eb1, x3, eb2*8, 8);
            } else {                    // mem <= u8
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, 4095, 0, 0, &lock);
                MMU_ADDR(ed, x1);
                u8 = F8;
                MOVW(x3, u8);
                STRB_IMM9(x3, ed, fixedaddress);
                SMWRITELOCK(lock);
            }
            break;
        case 0xC7:
            INST_NAME("MOV Ed, Id");
            nextop=F8;
            if(MODREG) {   // reg <= i32
                i32 = F32S;
                ed = xEAX+(nextop&7);
                MOV32(ed, i32);
            } else {                    // mem <= i32
                addr = geted(dyn, addr, ninst, nextop, &ed, x2, &fixedaddress, 4095, 0, 0, &lock);
                MMU_ADDR(ed, x2);
                i32 = F32S;
                MOV32(x3, i32);
                STR_IMM9(x3, ed, fixedaddress);
                SMWRITELOCK(lock);
            }
            break;

        case 0xC9:
            INST_NAME("LEAVE");
            MOV_REG(xESP, xEBP);
            MMU_POP1(xEBP, x2);
            break;

        case 0xCC:
            SETFLAGS(X_ALL, SF_SET_NODF);    // Hack, set all flags (to an unknown state...)
            NOTEST(x1, x14);
            if(PK(0)=='S' && PK(1)=='C') {
                addr+=2;
                BARRIER(BARRIER_FLOAT);
                INST_NAME("Special Box86 instruction");
                if((PK(0)==0) && (PK(1)==0) && (PK(2)==0) && (PK(3)==0))
                {
                    addr+=4;
                    MESSAGE(LOG_DEBUG, "Exit x86 Emu\n");
                    MOV32(x14, ip+1+2);
                    SMEND();
                    STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                    MOVW(x1, 1);
                    STR_IMM9(x1, xEmu, offsetof(x86emu_t, quit));
                    *ok = 0;
                    *need_epilog = 1;
                } else {
                    MESSAGE(LOG_DUMP, "Native Call to %s\n", GetNativeName(GetNativeFnc(ip)));
                    SMEND();
                    x87_forget(dyn, ninst, x3, x14, 0);
                    if((box86_log<2) && !cycle_log) {   // call the wrapper directly
                        uintptr_t ncall[2]; // to avoid BUSERROR!!!
                        memcpy(ncall,  (void*)addr, 2*sizeof(void*));   // the wrapper + function
                        addr+=8;
                        MOV32(xEIP, addr);
                        MOV_REG(x3, xEIP);
                        MOV32(x1, ncall[1]);
                        STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        CALL_S((void*)ncall[0], -1, (1<<x3));
                        LDM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                    } else {
                        // use x86Int3 to have trace
                        MOV32(xEIP, ip+1); // read the 0xCC
                        addr+=4+4;
                        STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        CALL_S(x86Int3, -1, 0);
                        LDM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        MOV32(x3, ip+1+2+4+4); // expected return address
                    }
                    CMPS_REG_LSL_IMM5(xEIP, x3, 0);
                    B_MARK(cNE);
                    LDR_IMM9(x1, xEmu, offsetof(x86emu_t, quit));
                    CMPS_IMM8(x1, 1);
                    B_NEXT(cNE);
                    MARK;
                    jump_to_epilog(dyn, 0, xEIP, ninst);
                }
            } else {
                INST_NAME("INT 3");
                // check if TRAP signal is handled
                LDR_IMM9(x1, xEmu, offsetof(x86emu_t, context));
                MOV32(x2, offsetof(box86context_t, signals[SIGTRAP]));
                LDR_REG_LSL_IMM5(x3, x1, x2, 0);
                CMPS_IMM8(x3, 0);
                B_NEXT(cEQ);
                STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                MOV32(xEIP, addr);
                STR_IMM9(xEIP, xEmu, offsetof(x86emu_t, ip));
                MOV32(x1, SIGTRAP);
                MOV32(x2, addr);
                MOV32(x3, 128);
                CALL(emit_signal, -1, 0);
                break;
            }
            break;
        case 0xCD:
            SETFLAGS(X_ALL, SF_SET_NODF);    // Hack, set all flags (to an unknown state...)
            SKIPTEST(x1, x14);
            SMEND();
            u8 = F8;
            if(u8==0x80) {
                INST_NAME("Syscall");
                BARRIER(BARRIER_FLOAT);
                MOV32(xEIP, ip+2);
                STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                CALL_S(x86Syscall, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                MOVW(x2, addr);
                CMPS_REG_LSL_IMM5(x14, x2, 0);
                B_MARK(cNE);    // jump to epilog, if IP is not what is expected
                LDR_IMM9(x1, xEmu, offsetof(x86emu_t, quit));
                CMPS_IMM8(x1, 1);
                B_NEXT(cNE);
                MARK;
                jump_to_epilog(dyn, 0, xEIP, ninst);
            } else {
                INST_NAME("INT Ib");
                if(box86_wine && u8==0x2D && 0) {
                    MESSAGE(LOG_DEBUG, "Hack for wine/int 2d\n");
                } else {
                    DEFAULT;
                }
            }
            break;

        case 0xCF:
            INST_NAME("IRET");
            SETFLAGS(X_ALL, SF_SET_NODF);    // Not a hack, EFLAGS are restored
            BARRIER(BARRIER_FLOAT);
            iret_to_epilog(dyn, ninst);
            *need_epilog = 0;
            *ok = 0;
            break;
        case 0xD0:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("ROL Eb, 1");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETEB(x1);
                    emit_rol8c(dyn, ninst, ed, 1, x2, x14);
                    EBBACK;
                    break;
                case 1:
                    INST_NAME("ROR Eb, 1");
                    MOVW(x2, 1);
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETEB(x1);
                    emit_ror8c(dyn, ninst, ed, 1, x2, x14);
                    EBBACK;
                    break;
                case 2:
                    INST_NAME("RCL Eb, 1");
                    MESSAGE(LOG_DUMP, "Need Optimization RCL 8b\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    MOVW(x2, 1);
                    GETEB(x1);
                    CALL_(rcl8, x1, (1<<x3));
                    EBBACK;
                    break;
                case 3:
                    INST_NAME("RCR Eb, 1");
                    MESSAGE(LOG_DUMP, "Need Optimization RCR 8b\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    MOVW(x2, 1);
                    GETEB(x1);
                    CALL_(rcr8, x1, (1<<x3));
                    EBBACK;
                    break;
                case 4:
                case 6:
                    INST_NAME("SHL Eb, 1");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETEB(x1);
                    emit_shl8c(dyn, ninst, ed, 1, x2, x14);
                    EBBACK;
                    break;
                case 5:
                    INST_NAME("SHR Eb, 1");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETEB(x1);
                    emit_shr8c(dyn, ninst, ed, 1, x2, x14);
                    EBBACK;
                    break;
                case 7:
                    INST_NAME("SAR Eb, 1");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETSEB(x1);
                    emit_sar8c(dyn, ninst, ed, 1, x2, x14);
                    EBBACK;
                    break;
            }
            break;

        case 0xD1:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("ROL Ed, 1");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETED;
                    emit_rol32c(dyn, ninst, ed, 1, x3, x14);
                    WBACK;
                    break;
                case 1:
                    INST_NAME("ROR Ed, 1");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET_PENDING);
                    GETED;
                    emit_ror32c(dyn, ninst, ed, 1, x3, x14);
                    WBACK;
                    break;
                case 2:
                    INST_NAME("RCL Ed, 1");
                    MESSAGE(LOG_DUMP, "Need Optimization RCL\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    MOVW(x2, 1);
                    GETEDW(x14, x1);
                    CALL_(rcl32, ed, (1<<x14));
                    WBACK;
                    break;
                case 3:
                    INST_NAME("RCR Ed, 1");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SUBSET);
                    GETED;
                    IFX(X_OF) {
                        XOR_REG_LSR_IMM8(x3, xFlags, ed, 31);
                        BFI(xFlags, x3, F_OF, 1);
                    }
                    MOVS_REG_LSR_IMM5(x3, xFlags, 1);    // load CC (F_CF==0) into ARM CF
                    MOVS_REG_RRX(ed, ed);
                    IFX(X_PEND) {
                        STR_IMM9(ed, xEmu, offsetof(x86emu_t, res));
                    }
                    IFX(X_CF) {
                        ORR_IMM8_COND(cCS, xFlags, xFlags, 1, 0);
                        BFC_COND(cCC, xFlags, F_CF, 1);

                    }
                    WBACK;
                    break;
                case 4:
                case 6:
                    INST_NAME("SHL Ed, 1");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETED;
                    emit_shl32c(dyn, ninst, ed, 1, x3, x14);
                    WBACK;
                    break;
                case 5:
                    INST_NAME("SHR Ed, 1");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETED;
                    emit_shr32c(dyn, ninst, ed, 1, x3, x14);
                    WBACK;
                    break;
                case 7:
                    INST_NAME("SAR Ed, 1");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    GETED;
                    emit_sar32c(dyn, ninst, ed, 1, x3, x14);
                    WBACK;
                    break;
            }
            break;
        case 0xD2:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("ROL Eb, CL");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET);
                    if(box86_dynarec_safeflags>1)
                        MAYSETFLAGS();
                    UFLAG_IF {
                        TSTS_IMM8(xECX, 0x1f);
                        B_NEXT(cEQ);
                    }
                    AND_IMM8(x2, xECX, 0x7);
                    RSB_IMM8(x2, x2, 8);
                    GETEB(x1);
                    BFI(ed, ed, 8, 8);
                    MOV_REG_LSR_REG(ed, ed, x2);
                    EBBACK;
                    UFLAG_IF {  // calculate flags directly
                        CMPS_IMM8(x2, 7);
                            MOV_REG_LSR_IMM5_COND(cEQ, x3, ed, 7);
                            ADD_REG_LSL_IMM5_COND(cEQ, x3, x3, ed, 0);
                            BFI_COND(cEQ, xFlags, x3, F_OF, 1);
                        BFI(xFlags, ed, F_CF, 1);
                        UFLAG_DF(x2, d_none);
                    }
                    break;
                case 1:
                    INST_NAME("ROR Eb, CL");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET);
                    if(box86_dynarec_safeflags>1)
                        MAYSETFLAGS();
                    UFLAG_IF {
                        TSTS_IMM8(xECX, 0x1f);
                        B_NEXT(cEQ);
                    }
                    AND_IMM8(x2, xECX, 0x7);
                    GETEB(x1);
                    BFI(ed, ed, 8, 8);
                    MOV_REG_LSR_REG(ed, ed, x2);
                    EBBACK;
                    UFLAG_IF {  // calculate flags directly
                        CMPS_IMM8(x2, 1);
                            MOV_REG_LSR_IMM5_COND(cEQ, x2, ed, 6); // x2 = d>>30
                            XOR_REG_LSR_IMM8_COND(cEQ, x2, x2, x2, 1); // x2 = ((d>>30) ^ ((d>>30)>>1))
                            BFI_COND(cEQ, xFlags, x2, F_OF, 1);
                        MOV_REG_LSR_IMM5(x2, ed, 7);
                        BFI(xFlags, x2, F_CF, 1);
                        UFLAG_DF(x2, d_none);
                    }
                    break;
                case 2:
                    INST_NAME("RCL Eb, CL");
                    MESSAGE(LOG_DUMP, "Need Optimization RCL 8b\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    AND_IMM8(x2, xECX, 0x1f);
                    GETEB(x1);
                    CALL_(rcl8, x1, (1<<x3));
                    EBBACK;
                    break;
                case 3:
                    INST_NAME("RCR Eb, CL");
                    MESSAGE(LOG_DUMP, "Need Optimization RCR 8b\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    AND_IMM8(x2, xECX, 0x1f);
                    GETEB(x1);
                    CALL_(rcr8, x1, (1<<x3));
                    EBBACK;
                    break;
                case 4:
                case 6:
                    INST_NAME("SHL Eb, CL");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    if(box86_dynarec_safeflags>1)
                        MAYSETFLAGS();
                    UFLAG_IF {
                        ANDS_IMM8(x2, xECX, 0x1f);
                        B_NEXT(cEQ);
                    } else {
                        AND_IMM8(x2, xECX, 0x1f);
                    }
                    GETEB(x1);
                    emit_shl8(dyn, ninst, ed, x2, x2, x14);
                    EBBACK;
                    break;
                case 5:
                    INST_NAME("SHR Eb, CL");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    if(box86_dynarec_safeflags>1)
                        MAYSETFLAGS();
                    UFLAG_IF {
                        ANDS_IMM8(x2, xECX, 0x1f);
                        B_NEXT(cEQ);
                    } else {
                        AND_IMM8(x2, xECX, 0x1f);
                    }
                    GETEB(x1);
                    emit_shr8(dyn, ninst, ed, x2, x2, x14);
                    EBBACK;
                    break;
                case 7:
                    INST_NAME("SAR Eb, CL");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    if(box86_dynarec_safeflags>1)
                        MAYSETFLAGS();
                    UFLAG_IF {
                        ANDS_IMM8(x2, xECX, 0x1f);
                        B_NEXT(cEQ);
                    } else {
                        AND_IMM8(x2, xECX, 0x1f);
                    }
                    GETSEB(x1);
                    emit_sar8(dyn, ninst, ed, x2, x2, x14);
                    EBBACK;
                    break;
            }
            break;
        case 0xD3:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("ROL Ed, CL");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET);
                    AND_IMM8(x3, xECX, 0x1f);
                    TSTS_REG_LSL_IMM5(x3, x3, 0);
                    B_MARK2(cEQ);
                    RSB_IMM8(x3, x3, 0x20);
                    GETED;
                    MOV_REG_ROR_REG(ed, ed, x3);
                    WBACK;
                    UFLAG_IF {  // calculate flags directly
                        CMPS_IMM8(x3, 31);
                        B_MARK(cNE);
                            MOV_REG_LSR_IMM5(x1, ed, 31);
                            ADD_REG_LSL_IMM5(x1, x1, ed, 0);
                            BFI(xFlags, x1, F_OF, 1);
                        MARK;
                        BFI(xFlags, ed, F_CF, 1);
                        UFLAG_DF(x2, d_none);
                    }
                    MARK2;
                    break;
                case 1:
                    INST_NAME("ROR Ed, CL");
                    SETFLAGS(X_OF|X_CF, SF_SUBSET);
                    AND_IMM8(x3, xECX, 0x1f);
                    TSTS_REG_LSL_IMM5(x3, x3, 0);
                    B_MARK2(cEQ);
                    GETED;
                    MOV_REG_ROR_REG(ed, ed, x3);
                    WBACK;
                    UFLAG_IF {  // calculate flags directly
                        CMPS_IMM8(x3, 1);
                        B_MARK(cNE);
                            MOV_REG_LSR_IMM5(x2, ed, 30); // x2 = d>>30
                            XOR_REG_LSR_IMM8(x2, x2, x2, 1); // x2 = ((d>>30) ^ ((d>>30)>>1))
                            BFI(xFlags, x2, F_OF, 1);
                        MARK;
                        MOV_REG_LSR_IMM5(x2, ed, 31);
                        BFI(xFlags, x2, F_CF, 1);
                        UFLAG_DF(x2, d_none);
                    }
                    MARK2;
                    break;
                case 2:
                    INST_NAME("RCL Ed, CL");
                    MESSAGE(LOG_DUMP, "Need Optimization RCL\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    AND_IMM8(x2, xECX, 0x1f);
                    GETEDW(x14, x1);
                    CALL_(rcl32, ed, (1<<x14));
                    WBACK;
                    break;
                case 3:
                    INST_NAME("RCR Ed, CL");
                    MESSAGE(LOG_DUMP, "Need Optimization RCR\n");
                    READFLAGS(X_CF);
                    SETFLAGS(X_OF|X_CF, SF_SET_DF);
                    AND_IMM8(x2, xECX, 0x1f);
                    GETEDW(x14, x1);
                    CALL_(rcr32, ed, (1<<x14));
                    WBACK;
                    break;
                case 4:
                case 6:
                    INST_NAME("SHL Ed, CL");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    AND_IMM8(x3, xECX, 0x1f);
                    GETED;
                    emit_shl32(dyn, ninst, ed, x3, x3, x14);
                    WBACK;
                    break;
                case 5:
                    INST_NAME("SHR Ed, CL");
                    SETFLAGS(X_ALL, SF_SET_PENDING);    // some flags are left undefined
                    AND_IMM8(x3, xECX, 0x1f);
                    GETED;
                    emit_shr32(dyn, ninst, ed, x3, x3, x14);
                    WBACK;
                    break;
                case 7:
                    INST_NAME("SAR Ed, CL");
                    SETFLAGS(X_ALL, SF_PENDING);
                    AND_IMM8(x3, xECX, 0x1f);
                    GETED;
                    UFLAG_OP12(ed, x3);
                    MOV_REG_ASR_REG(ed, ed, x3);
                    WBACK;
                    UFLAG_RES(ed);
                    UFLAG_DF(x3, d_sar32);
                    break;
            }
            break;
        case 0xD4:
            INST_NAME("AAM Ib");
            SETFLAGS(X_ALL, SF_SET_DF);
            UBFX(x1, xEAX, 0, 8);    // load AL
            u8 = F8;
            MOVW(x2, u8);
            CALL_(aam16, x1, 0);
            BFI(xEAX, x1, 0, 16);
            break;
        case 0xD5:
            INST_NAME("AAD Ib");
            SETFLAGS(X_ALL, SF_SET_DF);
            UBFX(x1, xEAX, 0, 16);    // load AX
            u8 = F8;
            MOVW(x2, u8);
            CALL_(aad16, x1, 0);
            BFI(xEAX, x1, 0, 16);
            break;
        case 0xD6:
            INST_NAME("SALC");
            READFLAGS(X_CF);
            TSTS_IMM8(xFlags, 1<<F_CF);
            MOVW_COND(cEQ, x3, 0x00);
            MOVW_COND(cNE, x3, 0xFF);
            BFI(xEAX, x3, 0, 8);
            break;
        case 0xD7:
            INST_NAME("XLAT");
            UBFX(x1, xEAX, 0, 8);    // x1 = AL
            SMREAD();
            // C8 (deep review): xEBX is a GUEST address — dereferencing it raw
            // reads host memory one membase below the table. Translate into a
            // scratch first (no-op under identity, MMU_ED picks xEBX back).
            MMU_G2H(x2, xEBX);
            LDRB_REG_LSL_IMM5(x1, MMU_ED(x2, xEBX), x1, 0); //x1 = byte ptr[EBX+AL]
            BFI(xEAX, x1, 0, 8);
            break;
        case 0xD8:
            addr = dynarecD8(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xD9:
            addr = dynarecD9(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xDA:
            addr = dynarecDA(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xDB:
            addr = dynarecDB(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xDC:
            addr = dynarecDC(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xDD:
            addr = dynarecDD(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xDE:
            addr = dynarecDE(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        case 0xDF:
            addr = dynarecDF(dyn, addr, ip, ninst, ok, need_epilog);
            break;
        #define GO(NO, YES)                                             \
            BARRIER(BARRIER_MAYBE);                                     \
            JUMP(addr+i8, 1);                                           \
            if(dyn->insts[ninst].x86.jmp_insts==-1 ||                   \
                CHECK_CACHE()) {                                        \
                /* out of the block */                                  \
                i32 = dyn->insts[ninst].epilog-(dyn->arm_size+8);       \
                Bcond(NO, i32);                                         \
                if(dyn->insts[ninst].x86.jmp_insts==-1) {               \
                    if(!(dyn->insts[ninst].x86.barrier&BARRIER_FLOAT))  \
                        fpu_purgecache(dyn, ninst, 1, x1, x2, x3);      \
                    jump_to_next(dyn, addr+i8, 0, ninst);               \
                } else {                                                \
                    CacheTransform(dyn, ninst, cacheupd, x1, x2, x3);   \
                    i32 = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8);\
                    Bcond(c__, i32);                                    \
                }                                                       \
            } else {                                                    \
                /* inside the block */                                  \
                i32 = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8);    \
                Bcond(YES, i32);                                        \
            }
        case 0xE0:
            INST_NAME("LOOPNZ");
            READFLAGS(X_ZF);
            i8 = F8S;
            SUBS_IMM8(xECX, xECX, 1);
            B_NEXT(cEQ);    // CX is 0, no LOOP
            TSTS_IMM8(xFlags, 1<<F_ZF);
            GO(cNE, cEQ);
            break;
        case 0xE1:
            INST_NAME("LOOPZ");
            READFLAGS(X_ZF);
            i8 = F8S;
            SUBS_IMM8(xECX, xECX, 1);
            B_NEXT(cEQ);    // CX is 0, no LOOP
            TSTS_IMM8(xFlags, 1<<F_ZF);
            GO(cEQ, cNE);
            break;
        case 0xE2:
            INST_NAME("LOOP");
            i8 = F8S;
            SUBS_IMM8(xECX, xECX, 1);
            GO(cEQ, cNE);
            break;
        case 0xE3:
            INST_NAME("JECXZ");
            i8 = F8S;
            TSTS_REG_LSL_IMM5(xECX, xECX, 0);
            GO(cNE, cEQ);
            break;
        #undef GO

        case 0xE8:
            // IgnitionVita iteration-34: marker 17 (0x004121DF, the "CALL
            // 0x412230" that invokes idle-work from WinMain's ACTIVE-path
            // loop, right after PeekMessageA came back empty). Passive, no
            // `break`/skip -- the real CALL rel32 decode below still runs
            // unchanged, still targets whatever it targets.
            if (ip == 0x004121DFu) {
                MOV32(x1, 17u);
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_marker25, -1, 0);
                LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
            }
            INST_NAME("CALL Id");
            i32 = F32S;
            u32 = (uintptr_t)getAlternate((void*)(addr+i32));
            if(u32==0) {
                #if STEP == 3
                printf_log(LOG_INFO, "Warning, CALL to 0x0 at %p (%p)\n", (void*)addr, (void*)(addr-1));
                #endif
            }
            #if STEP < 2
            if (i32==0)
                tmp = dyn->insts[ninst].pass2choice = 1;
            else if ((getProtection(u32)&PROT_READ) && (PKa(u32+0)==0x8B) && (((PKa(u32+1))&0xC7)==0x04) && (PKa(u32+2)==0x24) && (PKa(u32+3)==0xC3))
                tmp = dyn->insts[ninst].pass2choice = 2;
            else if(isNativeCall(dyn, u32, &dyn->insts[ninst].natcall, &dyn->insts[ninst].retn))
                tmp = dyn->insts[ninst].pass2choice = 3;
            else 
                tmp = dyn->insts[ninst].pass2choice = 0;
            #else
                tmp = dyn->insts[ninst].pass2choice;
            #endif
            switch(tmp) {
                case 1:
                    //SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags to "dont'care" state
                    MESSAGE(LOG_DUMP, "Hack for Call 0\n");
                    SKIPTEST(x1, x14);
                    MOV32(xEIP, addr);
                    MMU_PUSH1(xEIP, x2);
                    break;
                case 2:
                    //SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags to "dont'care" state
                    MESSAGE(LOG_DUMP, "Hack for Call x86.get_pc_thunk.reg\n");
                    SKIPTEST(x1, x14);
                    u8 = PK(i32+1);
                    gd = xEAX+((u8&0x38)>>3);
                    MOV32(gd, addr);
                    break;
                case 3:
                    SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags to "dont'care" state
                    SKIPTEST(x1, x14);
                    BARRIER(BARRIER_FULL);
                    BARRIER_NEXT(BARRIER_FULL);
                    MOV32(x2, addr);
                    MMU_PUSH1(x2, x3);
                    MESSAGE(LOG_DUMP, "Native Call to %s (retn=%d)\n", GetNativeName(GetNativeFnc(dyn->insts[ninst].natcall-1)), dyn->insts[ninst].retn);
                    // calling a native function
                    if(isRetX87Wrapper(*(wrapper_t*)(dyn->insts[ninst].natcall+2))) {
                        // return value will be on the stack, so the stack depth needs to be updated
                        x87_purgecache(dyn, ninst, 0, x3, x14, x1);
                    }
                    if((box86_log<2) && !cycle_log && dyn->insts[ninst].natcall) {   // call the wrapper directly
                        uintptr_t ncall[2];
                        memcpy(ncall,  (void*)(dyn->insts[ninst].natcall+2), 2*sizeof(void*));   // the wrapper + function
                        MOV32(xEIP, dyn->insts[ninst].natcall+2+4+4);
                        MOV_REG(x3, xEIP);
                        MOV32(x1, ncall[1]);
                        STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        CALL_S((void*)ncall[0], -1, (1<<x3));
                        LDM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                    } else {
                        MOV32(xEIP, dyn->insts[ninst].natcall); // read the 0xCC already
                        STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        CALL_S(x86Int3, -1, 0);
                        LDM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        MOV32(x3, dyn->insts[ninst].natcall+2+4+4);
                    }
                    CMPS_REG_LSL_IMM5(xEIP, x3, 0);
                    B_MARK(cNE);    // Not the expected address, exit dynarec block
                    MMU_POP1(xEIP, x2);   // pop the return address
                    if(dyn->insts[ninst].retn) {
                        ADD_IMM8(xESP, xESP, dyn->insts[ninst].retn);
                    }
                    MOV32(x3, addr);
                    CMPS_REG_LSL_IMM5(xEIP, x3, 0);
                    B_MARK(cNE);    // Not the expected address again
                    LDR_IMM9(x1, xEmu, offsetof(x86emu_t, quit));
                    CMPS_IMM8(x1, 1);
                    B_NEXT(cNE);    // not quitting, so lets continue
                    MARK;
                    jump_to_epilog(dyn, 0, xEIP, ninst);
                    break;
                default:
                    if((box86_dynarec_safeflags>1) || (ninst && dyn->insts[ninst-1].x86.set_flags)) {
                        READFLAGS(X_PEND);  // that's suspicious
                    } else {
                        SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags to "dont'care" state
                    }
                    // regular call
                    /*if(box86_dynarec_callret && box86_dynarec_bigblock>1) {
                        BARRIER(BARRIER_FULL);
                        BARRIER_NEXT(BARRIER_FULL);
                    } else {
                        BARRIER(BARRIER_FLOAT);
                        *need_epilog = 0;
                        *ok = 0;
                    }*/
                    MOV32(x2, addr);
                    fpu_purgecache(dyn, ninst, 1, x1, x3, x14);
                    MMU_PUSH1(x2, x3);
                    if(box86_dynarec_callret) {
                        SET_HASCALLRET();
                        // Push actual return address
                        if(addr < (dyn->start+dyn->isize)) {
                            // there is a next...
                            j32 = (dyn->insts)?(dyn->insts[ninst].epilog-(dyn->arm_size)-8):0;
                            MESSAGE(LOG_NONE, "\tCALLRET set return to +%di\n", j32>>2);
                            ADR(c__, x14, j32);
                        } else {
                            MESSAGE(LOG_NONE, "\tCALLRET set return to Jmptable(%p)\n", (void*)addr);
                            j32 = getJumpTableAddress(addr);
                            MOV32(x14, j32);
                            LDR_IMM9(x14, x14, 0);
                        }
                        PUSH(xSP, (1<<x2)|(1<<x14));
                    } else {
                        *need_epilog = 0;
                        *ok = 0;
                    }
                    if(u32==0) {   // self modifying code maybe? so use indirect address fetching
                        MOV32(x14, addr-4);
                        LDR_IMM9(x14, x14, 0);
                        jump_to_next(dyn, 0, x14, ninst);
                    } else
                        jump_to_next(dyn, u32, 0, ninst);
                    break;
            }
            break;
        case 0xE9:
        case 0xEB:
            BARRIER(BARRIER_MAYBE);
            if(opcode==0xE9) {
                INST_NAME("JMP Id");
                i32 = F32S;
            } else {
                INST_NAME("JMP Ib");
                i32 = F8S;
            }
            JUMP((uintptr_t)getAlternate((void*)(addr+i32)), 0);
            if(dyn->insts[ninst].x86.jmp_insts==-1) {
                // out of the block
                fpu_purgecache(dyn, ninst, 1, x1, x2, x3);
                jump_to_next(dyn, (uintptr_t)getAlternate((void*)(addr+i32)), 0, ninst);
            } else {
                // inside the block
                CacheTransform(dyn, ninst, CHECK_CACHE(), x1, x2, x3);
                tmp = dyn->insts[dyn->insts[ninst].x86.jmp_insts].address-(dyn->arm_size+8);
                if(tmp==-4) {
                    NOP;
                } else {
                    Bcond(c__, tmp);
                }
            }
            *need_epilog = 0;
            *ok = 0;
            break;
        
        case 0xEC:                      /* IN AL, DX */
        case 0xED:                      /* IN EAX, DX */
        case 0xEE:                      /* OUT DX, AL */
        case 0xEF:                      /* OUT DX, EAX */
            INST_NAME(opcode==0xEC?"IN AL, DX":(opcode==0xED?"IN EAX, DX":(opcode==0xEE?"OUT DX? AL":"OUT DX, EAX")));
            SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags in "don't care" state
            STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESP)|(1<<xEBP)|(1<<xESI)|(1<<xEDI)|(1<<xFlags));
            MOV32(xEIP, ip);
            STR_IMM9(xEIP, xEmu, offsetof(x86emu_t, ip));
            CALL(arm_priv, -1, 0);
            LDR_IMM9(xEIP, xEmu, offsetof(x86emu_t, ip));
            jump_to_epilog(dyn, 0, xEIP, ninst);
            *need_epilog = 0;
            *ok = 0;
            break;

        case 0xF0:
            addr = dynarecF0(dyn, addr, ip, ninst, ok, need_epilog);
            break;

        case 0xF2:                      /* REPNZ prefix */
        case 0xF3:                      /* REPZ prefix */
            nextop = F8;
            if(nextop==0x0F) {
                if(opcode==0xF3) {
                    addr = dynarecF30F(dyn, addr, ip, ninst, ok, need_epilog);
                } else {
                    addr = dynarecF20F(dyn, addr, ip, ninst, ok, need_epilog);
                }
            } else if(nextop==0x66) {
                nextop = F8;
                switch(nextop) {
                    case 0xA5:
                        INST_NAME("REP MOVSW");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3, 2);
                        SMREAD();
                        MARK;
                        MMU_SAI3(LDRHAI_REG_LSL_IMM5, LDRH_IMM8, x1, xESI, x3, x14);
                        MMU_SAI3(STRHAI_REG_LSL_IMM5, STRH_IMM8, x1, xEDI, x3, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        SMWRITE();
                        break;
                    case 0xAB:
                        INST_NAME("REP STOSW");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3, 2);
                        MARK;
                        MMU_SAI3(STRHAI_REG_LSL_IMM5, STRH_IMM8, xEAX, xEDI, x3, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        SMWRITE();
                        break;
                    case 0xAF:
                        if(opcode==0xF2) {INST_NAME("REPNZ SCASW");} else {INST_NAME("REPZ SCASW");}
                        MAYSETFLAGS();
                        SETFLAGS(X_ALL, SF_SET_PENDING);
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3, 2);
                        UXTH(x1, xEAX, 0); // get lower 16bits
                        MARK;
                        MMU_SAI3(LDRHAI_REG_LSL_IMM5, LDRH_IMM8, x2, xEDI, x3, x14);
                        CMPS_REG_LSL_IMM5(x1, x2, 0);
                        if(opcode==0xF2) {
                            B_MARK2(cEQ);
                        } else {
                            B_MARK2(cNE);
                        }
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        B_MARK3(c__);
                        // done, finish with cmp test
                        MARK2;
                        SUB_IMM8(xECX, xECX, 1);
                        MARK3;
                        emit_cmp16(dyn, ninst, x1, x2, x3, x14);
                        break;
                    default:
                        INST_NAME("F2/F3 66 ...");
                        DEFAULT;
                }
            } else {
                // DF=0, increment addresses, DF=1 decrement addresses
                switch(nextop) {
                    case 0x70:
                    case 0x71:
                    case 0x72:
                    case 0x73:
                    case 0x74:
                    case 0x75:
                    case 0x76:
                    case 0x77:
                    case 0x78:
                    case 0x79:
                    case 0x7A:
                    case 0x7B:
                    case 0x7C:
                    case 0x7D:
                    case 0x7E:
                    case 0x7F:
                    case 0x80:
                    case 0x9C:
                    case 0xE8:
                    case 0xE9:
                    case 0xEB:
                        if(opcode==0xF2) {INST_NAME("BND");} else {INST_NAME("F3");}
                        --addr; // put back opcode
                        break;
                    case 0x90:
                        INST_NAME("PAUSE");
                        YIELD(c__);
                        break;
                    case 0xC3:
                        INST_NAME("(REPZ) RET");
                        if(box86_dynarec_safeflags>1) {
                            READFLAGS(X_PEND);
                        } else {
                            SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags to "dont'care" state
                        }
                        BARRIER(BARRIER_FLOAT);
                        ret_to_epilog(dyn, ninst);
                        *need_epilog = 0;
                        *ok = 0;
                        break;
                    case 0xA4:
                        INST_NAME("REP MOVSB");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        SMREAD();
                        GETDIR(x3, 1);
                        // D2Vita fix: the 4-byte fast path below batches 4 bytes as
                        // one dword, stepping by dir<<2. That is correct only FORWARD
                        // (DF=0): with DF=1 (x3=-1) the 4 bytes come out in reverse
                        // order (wrong RESULT, not a crash — invisible unless a guest
                        // runs `std; rep movsb`, e.g. a backward memmove). Force the
                        // per-byte loop when DF=1. Upstream Box86 bug — report it.
                        CMPS_IMM8(x3, 1);
                        B_MARK(cNE);
                        SUBS_REG_LSL_IMM5(x2, xEDI, xESI, 0);
                        SUBS_IMM8(x2, x2, 4);
                        B_MARK(cCC);
                        MARK2;
                        SUBS_IMM8(x2, xECX, 4);
                        B_MARK(cCC);
                        MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x1, xESI, x3, 2, x14);
                        MMU_SAI4(STRAI_REG_LSL_IMM5, STR_IMM9, x1, xEDI, x3, 2, x14);
                        MOV_REG(xECX, x2);
                        B_MARK2(cNE);
                        B_MARK3(c__);
                        MARK;
                        MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x1, xESI, x3, 0, x14);
                        MMU_SAI4(STRBAI_REG_LSL_IMM5, STRB_IMM9, x1, xEDI, x3, 0, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        MARK3;
                        SMWRITE();
                        // done
                        break;
                    case 0xA5:
                        INST_NAME("REP MOVSD");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,4);
                        SMREAD();
                        MARK;
                        MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x1, xESI, x3, 0, x14);
                        MMU_SAI4(STRAI_REG_LSL_IMM5, STR_IMM9, x1, xEDI, x3, 0, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        SMWRITE();
                        // done
                        break;
                    case 0xA6:
                        if(opcode==0xF2) {INST_NAME("REPNZ CMPSB");} else {INST_NAME("REPZ CMPSB");}
                        MAYSETFLAGS();
                        SETFLAGS(X_ALL, SF_SET_PENDING);
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,1);
                        MARK;
                        MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x1, xESI, x3, 0, x14);
                        MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x2, xEDI, x3, 0, x14);
                        CMPS_REG_LSL_IMM5(x1, x2, 0);
                        if(opcode==0xF2) {
                            B_MARK2(cEQ);
                        } else {
                            B_MARK2(cNE);
                        }
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        B_MARK3(c__);   // go past sub ecx, 1
                        // done, finish with cmp test
                        MARK2;
                        SUB_IMM8(xECX, xECX, 1);
                        MARK3;
                        emit_cmp8(dyn, ninst, x1, x2, x3, x14);
                        break;
                    case 0xA7:
                        if(opcode==0xF2) {INST_NAME("REPNZ CMPSD");} else {INST_NAME("REPZ CMPSD");}
                        MAYSETFLAGS();
                        SETFLAGS(X_ALL, SF_SET_PENDING);
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,4);
                        MARK;
                        MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x1, xESI, x3, 0, x14);
                        MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x2, xEDI, x3, 0, x14);
                        CMPS_REG_LSL_IMM5(x1, x2, 0);
                        if(opcode==0xF2) {
                            B_MARK2(cEQ);
                        } else {
                            B_MARK2(cNE);
                        }
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        B_MARK3(c__);   // go past sub ecx, 1
                        // done, finish with cmp test
                        MARK2;
                        SUB_IMM8(xECX, xECX, 1);
                        MARK3;
                        emit_cmp32(dyn, ninst, x1, x2, x3, x14);
                        break;
                    case 0xAA:
                        INST_NAME("REP STOSB");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,1);
                        MARK;
                        MMU_SAI4(STRBAI_REG_LSL_IMM5, STRB_IMM9, xEAX, xEDI, x3, 0, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        SMWRITE();
                        break;
                    case 0xAB:
                        INST_NAME("REP STOSD");
                        // IgnitionVita iteration-25: REP24/DYN24 override
                        // REMOVED at that time -- iteration 24 had proven
                        // this instruction was never reached at runtime.
                        // Iteration 28 (fixing the separate PUSHFW bug)
                        // changed that: execution now reaches here, and
                        // then hard-crashes. Iteration 29 installs a new,
                        // separate diagnostic override below, scoped to
                        // this exact ip, after first firing marker 12
                        // (0x0045AFDE, passive) to confirm arrival. For
                        // every OTHER ip, this stays the stock Box86
                        // translation, completely unmodified.
                        if (ip == 0x0045AFDEu) {
                            MOV32(x1, 12u);
                            STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                            CALL_S(ign_marker25, -1, 0);
                            LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));

                            // Iteration-29 diagnostic override: ONLY this
                            // exact guest instruction. See
                            // ign_repstosd29_override()'s comment above for
                            // the full rationale. Skips the stock
                            // TSTS/B_NEXT/MARK/MMU_SAI4/B_MARK loop below
                            // entirely for this one occurrence and
                            // substitutes the native chunked helper, which
                            // does the identical architectural effect
                            // (81000 dword stores, EDI+=324000, ECX=0)
                            // through the same guest->host translation.
                            // `break` here means every OTHER REP STOSD in
                            // the game still falls through to the
                            // completely unmodified code below --
                            // global_rep_behavior is unchanged.
                            //
                            // iteration-31 fix: the STM/LDM masks here used
                            // to be sparse ((1<<xEAX)|(1<<xECX)|(1<<xEDI)
                            // and (1<<xECX)|(1<<xEDI)) -- ARM's STM/LDM
                            // pack registers SEQUENTIALLY from the base
                            // address in ascending register-number order,
                            // NOT at each register's fixed struct offset,
                            // so a mask with gaps (xEDI=r11 is far from
                            // xEAX=r4/xECX=r5) silently landed xEDI's
                            // value at the WRONG struct offset (EDX's
                            // slot) on the way in, and on the way out the
                            // LDM loaded live xEDI from offset 4 (ECX's
                            // slot, which the override had just zeroed)
                            // instead of offset 28 (EDI's real slot) --
                            // exactly why marker 13 logged EDI=0x00000000
                            // right after "[REP29] COMPLETE
                            // EDI=0x010D0260". Switched to the SAME full,
                            // contiguous 9-register mask every marker uses
                            // (r4..r12, no gaps) -- this always round-trips
                            // ALL of EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI/Flags
                            // correctly regardless of which ones the
                            // callee actually touches.
                            INST_NAME("REP STOSD [IGN29 diag override @0x0045AFDE]");
                            STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                            CALL_S(ign_repstosd29_override, -1, 0);
                            LDM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                            break;
                        }
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,4);
                        MARK;
                        MMU_SAI4(STRAI_REG_LSL_IMM5, STR_IMM9, xEAX, xEDI, x3, 0, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        SMWRITE();
                        break;
                    case 0xAC:
                        INST_NAME("REP LODSB");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,1);
                        MARK;
                        MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x1, xESI, x3, 0, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        BFI(xEAX, x1, 0, 8);
                        break;
                    case 0xAD:
                        INST_NAME("REP LODSD");
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,4);
                        MARK;
                        MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, xEAX, xESI, x3, 0, x14);
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        break;
                    case 0xAE:
                        if(opcode==0xF2) {INST_NAME("REPNZ SCASB");} else {INST_NAME("REPZ SCASB");}
                        MAYSETFLAGS();
                        SETFLAGS(X_ALL, SF_SET_PENDING);
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,1);
                        UXTB(x1, xEAX, 0);
                        MARK;
                        MMU_SAI4(LDRBAI_REG_LSL_IMM5, LDRB_IMM9, x2, xEDI, x3, 0, x14);
                        CMPS_REG_LSL_IMM5(x1, x2, 0);
                        if(opcode==0xF2) {
                            B_MARK2(cEQ);
                        } else {
                            B_MARK2(cNE);
                        }
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        B_MARK3(c__);
                        // done, finish with cmp test
                        MARK2;
                        SUB_IMM8(xECX, xECX, 1);
                        MARK3;
                        emit_cmp8(dyn, ninst, x1, x2, x3, x14);
                        break;
                    case 0xAF:
                        if(opcode==0xF2) {INST_NAME("REPNZ SCASD");} else {INST_NAME("REPZ SCASD");}
                        MAYSETFLAGS();
                        SETFLAGS(X_ALL, SF_SET_PENDING);
                        TSTS_REG_LSL_IMM5(xECX, xECX, 0);
                        B_NEXT(cEQ);    // end of loop
                        GETDIR(x3,4);
                        MARK;
                        MMU_SAI4(LDRAI_REG_LSL_IMM5, LDR_IMM9, x2, xEDI, x3, 0, x14);
                        CMPS_REG_LSL_IMM5(xEAX, x2, 0);
                        if(opcode==0xF2) {
                            B_MARK2(cEQ);
                        } else {
                            B_MARK2(cNE);
                        }
                        SUBS_IMM8(xECX, xECX, 1);
                        B_MARK(cNE);
                        B_MARK3(c__);
                        // done, finish with cmp test
                        MARK2;
                        SUB_IMM8(xECX, xECX, 1);
                        MARK3;
                        emit_cmp32(dyn, ninst, xEAX, x2, x3, x14);
                        break;
                    default:
                        INST_NAME("F2/F3 ...");
                        DEFAULT;
                }
            }
            break;
            
        case 0xF5:
            INST_NAME("CMC");
            READFLAGS(X_CF);
            SETFLAGS(X_CF, SF_SUBSET);
            XOR_IMM8(xFlags, xFlags, 1<<F_CF);
            break;
        case 0xF6:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                case 1:
                    INST_NAME("TEST Eb, Ib");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    u8 = F8;
                    MOVW(x2, u8);
                    emit_test8(dyn, ninst, x1, x2, x3, x14);
                    break;
                case 2:
                    INST_NAME("NOT Eb");
                    if(MODREG) {
                        wback = (nextop&7);
                        wb2 = (wback>>2);
                        wback = xEAX+(wback&3);
                        XOR_IMM8_ROR(wback, wback, 0xff, wb2?12:0);
                    } else {
                        GETEB(x1);
                        MVN_REG_LSL_IMM5(x1, x1, 0);
                        EBBACK;
                    }
                    break;
                case 3:
                    INST_NAME("NEG Eb");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEB(x1);
                    emit_neg8(dyn, ninst, x1, x2, x14);
                    EBBACK;
                    break;
                case 4:
                    INST_NAME("MUL AL, Ed");
                    SETFLAGS(X_ALL, SF_PENDING);
                    GETEB(x1);
                    UBFX(x2, xEAX, 0, 8);
                    MUL(x1, x1, x2);
                    BFI(xEAX, x1, 0, 16);
                    UFLAG_RES(x1);
                    UFLAG_DF(x2, d_mul8);
                    break;
                case 5:
                    INST_NAME("IMUL AL, Eb");
                    MESSAGE(LOG_DUMP, "Need Optimization IMUL 8b\n");
                    SETFLAGS(X_ALL, SF_PENDING);
                    GETEB(x1);
                    STM(xEmu, (1<<xEAX));
                    CALL(imul8, -1, 0);
                    LDM(xEmu, (1<<xEAX));
                    break;
                case 6:
                    INST_NAME("DIV Eb");
                    MESSAGE(LOG_DUMP, "Need Optimization DIV 8b\n");
                    SETFLAGS(X_ALL, SF_SET_DF);
                    GETEB(x1);
                    STM(xEmu, (1<<xEAX));
                    CALL(div8, -1, 0);
                    LDM(xEmu, (1<<xEAX));
                    CHECK_DIV0();
                    break;
                case 7:
                    INST_NAME("IDIV Eb");
                    MESSAGE(LOG_DUMP, "Need Optimization IDIV 8b\n");
                    SETFLAGS(X_ALL, SF_SET_DF);
                    GETEB(x1);
                    STM(xEmu, (1<<xEAX));
                    CALL(idiv8, -1, 0);
                    LDM(xEmu, (1<<xEAX));
                    CHECK_DIV0();
                    break;
            }
            break;
        case 0xF7:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                case 1:
                    INST_NAME("TEST Ed, Id");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEDH(x1);
                    i32 = F32S;
                    MOV32(x2, i32);
                    emit_test32(dyn, ninst, ed, x2, x3, x14);
                    break;
                case 2:
                    INST_NAME("NOT Ed");
                    GETED;
                    MVN_REG_LSL_IMM5(ed, ed, 0);
                    WBACK;
                    break;
                case 3:
                    INST_NAME("NEG Ed");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETED;
                    emit_neg32(dyn, ninst, ed, x3, x14);
                    WBACK;
                    break;
                case 4:
                    INST_NAME("MUL EAX, Ed");
                    SETFLAGS(X_ALL, SF_PENDING);
                    GETED;
                    UMULL(xEDX, xEAX, xEAX, ed);
                    UFLAG_RES(xEAX);
                    UFLAG_OP1(xEDX);
                    UFLAG_DF(x2, d_mul32);
                    break;
                case 5:
                    INST_NAME("IMUL EAX, Ed");
                    SETFLAGS(X_ALL, SF_PENDING);
                    GETED;
                    SMULL(xEDX, xEAX, xEAX, ed);
                    UFLAG_RES(xEAX);
                    UFLAG_OP1(xEDX);
                    UFLAG_DF(x2, d_imul32);
                    break;
                case 6:
                    INST_NAME("DIV Ed");
                    SETFLAGS(X_ALL, SF_SET_NODF);
                    if(ninst && dyn->insts && (nextop==0xF0)
                       && dyn->insts[ninst-1].x86.addr
                       && *(uint8_t*)DYN86_G2H(dyn->insts[ninst-1].x86.addr)==0xB8
                       && *(uint32_t*)DYN86_G2H(dyn->insts[ninst-1].x86.addr+1)==0) {
                        // hack for some protection that check a divide by zero actualy trigger a div0 exception
                        MESSAGE(LOG_INFO, "Divide by 0 hack\n");
                        STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xEIP)|(1<<xFlags));
                        MOV32(xEIP, addr);
                        STR_IMM9(xEIP, xEmu, offsetof(x86emu_t, ip));
                        MOV_REG(x1, xEIP);
                        MOV32(x2, 0);
                        CALL(emit_div0, -1, 0);
                        // emit_div0 always raises: leave the block here too,
                        // rather than falling through into the next
                        // instruction of a block the fault has ended.
                        CHECK_DIV0();
                    } else {
                        if(arm_div && ninst && dyn->insts 
                        && dyn->insts[ninst-1].x86.addr
                        && (*(uint8_t*)DYN86_G2H(dyn->insts[ninst-1].x86.addr)==0x31
                            || *(uint8_t*)DYN86_G2H(dyn->insts[ninst-1].x86.addr)==0x33)
                        && *(uint8_t*)DYN86_G2H(dyn->insts[ninst-1].x86.addr+1)==0xD2) {
                            // previous instruction is XOR EDX, EDX, so its 32bits / 32bits
                            SET_DFNONE(x2); // flags are undefined
                            GETED;
                            UDIV(x2, xEAX, ed); // x1 = xEAX / ed
                            MLS(xEDX, x2, ed, xEAX);  // x14 = xEAX mod ed (i.e. xEAX - x1*ed)
                            MOV_REG(xEAX, x2);
                        } else {
                            GETEDH(x1);
                            if(arm_div) {
                                CMPS_IMM8(xEDX, 0);
                                B_MARK(cEQ);
                            }
                            if(ed!=x1) {MOV_REG(x1, ed);}
                            STM(xEmu, (1<<xEAX) | (1<<xECX) | (1<<xEDX));
                            CALL(div32, -1, 0);
                            LDM(xEmu, (1<<xEAX) | (1<<xECX) | (1<<xEDX));
                            CHECK_DIV0();
                            if(arm_div) {
                                B_NEXT(c__);
                                MARK;
                                UDIV(x2, xEAX, ed); // x2 = xEAX / ed
                                MLS(xEDX, x2, ed, xEAX);  // x14 = xEAX mod ed (i.e. xEAX - x2*ed)
                                MOV_REG(xEAX, x2);
                                SET_DFNONE(x2); // flags are undefined
                            }
                        }
                    }
                    break;
                case 7:
                    INST_NAME("IDIV Ed");
                    SETFLAGS(X_ALL, SF_SET_DF);
                    if(arm_div && ninst && dyn->insts
                       &&  dyn->insts[ninst-1].x86.addr
                       && *(uint8_t*)DYN86_G2H(dyn->insts[ninst-1].x86.addr)==0x99) {
                        // previous instruction is CDQ, so its 32bits / 32bits
                        SET_DFNONE(x2); // flags are undefined
                        GETED;
                        SDIV(x2, xEAX, ed); // x1 = xEAX / ed
                        MLS(xEDX, x2, ed, xEAX);  // x14 = xEAX mod ed (i.e. xEAX - x1*ed)
                        MOV_REG(xEAX, x2);
                    } else {
                        GETEDH(x1);
                        if(arm_div) {
                            // check if a 32bits division is enough
                            CMPS_IMM8(xEDX, 0); // compare to 0
                            TSTS_IMM8_ROR_COND(cEQ, xEAX, 0b10, 1);   // also test that xEAX is not signed!
                            B_MARK(cEQ);
                            CMNS_IMM8(xEDX, 1); // compare to FFFFFFFF if not 0
                            B_MARK2(cNE);
                            TSTS_IMM8_ROR(xEAX, 0b10, 1);   // also test that xEAX is signed!
                            B_MARK(cNE);
                            MARK2;
                        }
                        if(ed!=x1) {MOV_REG(x1, ed);}
                        STM(xEmu, (1<<xEAX) | (1<<xECX) | (1<<xEDX));
                        CALL(idiv32, -1, 0);
                        LDM(xEmu, (1<<xEAX) | (1<<xECX) | (1<<xEDX));
                        CHECK_DIV0();
                        if(arm_div) {
                            B_NEXT(c__);
                            MARK;
                            SDIV(x2, xEAX, ed); // x2 = xEAX / ed
                            MLS(xEDX, x2, ed, xEAX);  // x14 = xEAX mod ed (i.e. xEAX - x2*ed)
                            MOV_REG(xEAX, x2);
                            SET_DFNONE(x2); // flags are undefined
                        }
                    }
                    break;
            }
            break;
        case 0xF8:
            INST_NAME("CLC");
            SETFLAGS(X_CF, SF_SUBSET);
            SET_DFNONE(x1);
            BFC(xFlags, F_CF, 1);
            break;
        case 0xF9:
            INST_NAME("STC");
            SETFLAGS(X_CF, SF_SUBSET);
            SET_DFNONE(x1);
            ORRS_IMM8(xFlags, xFlags, 1, 0);
            break;
        case 0xFA:
        case 0xFB:
            INST_NAME(opcode==0xFA?"STI":"CLI");
            SETFLAGS(X_ALL, SF_SET_NODF);    // Hack to set flags in "don't care" state
            STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESP)|(1<<xEBP)|(1<<xESI)|(1<<xEDI)|(1<<xFlags));
            MOV32(xEIP, ip);
            STR_IMM9(xEIP, xEmu, offsetof(x86emu_t, ip));
            CALL(arm_priv, -1, 0);
            LDR_IMM9(xEIP, xEmu, offsetof(x86emu_t, ip));
            jump_to_epilog(dyn, 0, xEIP, ninst);
            *need_epilog = 0;
            *ok = 0;
            break;
        case 0xFC:
            // IgnitionVita iteration-25: marker 5 (calloc's own CLD,
            // 0x0045AFD2 -- immediately before the REP STOSD that iteration
            // 24 proved is never reached). Same passive pattern.
            // IgnitionVita iteration-G111: marker 5 (RUN25) removed -- see
            // the case-0x74 comment above for the full rationale.
            INST_NAME("CLD");
            BFC(xFlags, F_DF, 1);
            break;
        case 0xFD:
            INST_NAME("STD");
            MOVW(x1, 1);
            BFI(xFlags, x1, F_DF, 1);
            break;

        case 0xFE:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("INC Eb");
                    SETFLAGS(X_ALL&~X_CF, SF_SUBSET_PENDING);
                    GETEB(x1);
                    emit_inc8(dyn, ninst, x1, x2, x14);
                    EBBACK;
                    break;
                case 1:
                    INST_NAME("DEC Eb");
                    SETFLAGS(X_ALL&~X_CF, SF_SUBSET_PENDING);
                    GETEB(x1);
                    emit_dec8(dyn, ninst, x1, x2, x14);
                    EBBACK;
                    break;
                default:
                    INST_NAME("Grp5 Ed");
                    DEFAULT;
            }
            break;
        case 0xFF:
            // IgnitionVita iteration-35: [GM35] PRECALL_BLOCKING, guarded
            // to the ONE real occurrence (0x004121F4, byte-verified
            // `FF D7` = CALL EDI). No `break`/skip -- the real CALL Ed
            // decode below (case 2 of the nested switch) still runs
            // completely unchanged immediately after; this only observes.
            if (ip == 0x004121F4u) {
                // Full contiguous r4-r12 mask, per the iteration-31
                // register-cache-coherency lesson: a sparse mask would
                // silently land at the wrong struct offsets. This marker
                // only READS the struct (ign_gm35_precall never writes),
                // so no LDM is needed afterward.
                STM(xEmu, (1<<xEAX)|(1<<xECX)|(1<<xEDX)|(1<<xEBX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP)|(1<<xFlags));
                CALL_S(ign_gm35_precall, -1, 0);
            }
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0: // INC Ed
                    INST_NAME("INC Ed");
                    SETFLAGS(X_ALL&~X_CF, SF_SUBSET_PENDING);
                    GETED;
                    emit_inc32(dyn, ninst, ed, x3, x14);
                    WBACK;
                    break;
                case 1: //DEC Ed
                    INST_NAME("DEC Ed");
                    SETFLAGS(X_ALL&~X_CF, SF_SUBSET_PENDING);
                    GETED;
                    emit_dec32(dyn, ninst, ed, x3, x14);
                    WBACK;
                    break;
                case 2: // CALL Ed
                    INST_NAME("CALL Ed");
                    PASS2IF((box86_dynarec_safeflags>1) ||
                        ((ninst && dyn->insts[ninst-1].x86.set_flags)
                        || ((ninst>1) && dyn->insts[ninst-2].x86.set_flags)), 1)
                    {
                        READFLAGS(X_PEND);          // that's suspicious
                    } else {
                        SETFLAGS(X_ALL, SF_SET_NODF);    //Hack to put flag in "don't care" state
                    }
                    GETEDH(xEIP);
                    if(box86_dynarec_callret && box86_dynarec_bigblock>1) {
                        BARRIER(BARRIER_FULL);
                        BARRIER_NEXT(BARRIER_FULL);
                    } else {
                        BARRIER(BARRIER_FLOAT);
                        *need_epilog = 0;
                        *ok = 0;
                    }
                    MOV32(x2, addr);
                    if(box86_dynarec_callret) {
                        SET_HASCALLRET();
                        // Push actual return address
                        if(addr < (dyn->start+dyn->isize)) {
                            // there is a next...
                            j32 = (dyn->insts)?(dyn->insts[ninst].epilog-(dyn->arm_size)-8):0;
                            MESSAGE(LOG_NONE, "\tCALLRET set return to +%di\n", j32>>2);
                            ADR(c__, x3, j32);
                        } else {
                            MESSAGE(LOG_NONE, "\tCALLRET set return to Jmptable(%p)\n", (void*)addr);
                            j32 = getJumpTableAddress(addr);
                            MOV32(x3, j32);
                            LDR_IMM9(x3, x3, 0);
                        }
                        PUSH(xSP, (1<<x2)|(1<<x3));
                    }
                    MMU_PUSH1(x2, x3);
                    jump_to_next(dyn, 0, ed, ninst);
                    break;
                case 4: // JMP Ed
                    INST_NAME("JMP Ed");
                    READFLAGS(X_PEND);
                    BARRIER(BARRIER_FLOAT);
                    GETEDH(xEIP);
                    jump_to_next(dyn, 0, ed, ninst);
                    *need_epilog = 0;
                    *ok = 0;
                    break;
                case 6: // Push Ed
                    INST_NAME("PUSH Ed");
                    GETED;
                    MMU_PUSH1(ed, x3);  // GETED never lands ed in x3
                    break;

                default:
                    INST_NAME("Grp5 Ed");
                    DEFAULT;
            }
            break;

        default:
            DEFAULT;
    }
 
     return addr;
}

