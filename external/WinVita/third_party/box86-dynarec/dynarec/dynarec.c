#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <setjmp.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "tools/bridge_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "threads.h"
#ifdef DYNAREC
#include "dynablock.h"
#include "dynablock_private.h"
#include "bridge.h"
#include "custommem.h"
void x86test_check(x86emu_t* ref, uintptr_t ip);
#endif

// D2Vita preemption seam (implemented in src/dynarec86/dyn86.c)
extern int dyn86_should_break(void);

#ifdef ARM
void arm_prolog(x86emu_t* emu, void* addr) EXPORTDYN;
void arm_epilog() EXPORTDYN;
void arm_epilog_fast() EXPORTDYN;
#endif

#ifdef DYNAREC
#ifdef HAVE_TRACE
#include "elfloader.h"
uintptr_t getX86Address(dynablock_t* db, uintptr_t arm_addr);
#endif
extern uintptr_t dyn86_eiptrap;
extern uintptr_t dyn86_eiptrap2;
__attribute__((weak)) uint32_t d2rt_sw_seq = 0;   // strong def in sched_cooperative.cpp
// D2Vita transfer ring: last 64 control transfers {src x86 block, target, esp}
// through the funnel. Armed by D2_XFERTRACE (needs D2_NOLINK=1 so ALL transfers
// funnel here). Dumped at fault via dyn86_dump_xfer() — finds the last valid
// block before a RET/JMP to a garbage (stack) target.
int dyn86_xfertrace = -1;
static uint32_t g_xf[64][3];
static int g_xfi = 0;
dynablock_t* FindDynablockFromNativeAddress(void* p);
void dyn86_dump_xfer(void)
{
    if(dyn86_xfertrace<=0) return;
    printf_log(LOG_NONE, "=== [xfer] last control transfers (src_block -> target  esp) ===\n");
    for(int k=0;k<64;k++){ int i=(g_xfi+k)&63; if(g_xf[i][0]||g_xf[i][1])
        printf_log(LOG_NONE, "  [xfer] %08x -> %08x  esp=%08x\n", g_xf[i][0], g_xf[i][1], g_xf[i][2]); }
}
/* Journal du moteur (src/platform/vita_host.h) : reference FORTE, meme
 * bibliotheque. Portait le prefixe du premier consommateur en lien faible. */
void wx86_vita_progress_c(const char* msg);
// D2Vita eiptrap silent ring: every chain to dyn86_eiptrap records the live
// register file here (no I/O — printing at the race window suppresses the race);
// dyn86_dump_eipring() is called by rt_boot's Crash.txt hook at a Fog Halt.
static uint32_t g_eipring[32][16];
static int g_eipi = 0;
void dyn86_dump_eipring(void)
{
    fprintf(stderr, "=== [eipring] last %d chains to %08x (total seen: see #) ===\n",
            g_eipi < 32 ? g_eipi : 32, (uint32_t)dyn86_eiptrap);
    for(int k=0;k<32;k++){ int i=(g_eipi+k)&31; uint32_t* R=g_eipring[i]; if(!R[0]) continue;
        char m[224];
        snprintf(m, sizeof m, "[eipring#%u%s] ret=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x esp=%08x ebp=%08x args: %08x %08x %08x %08x %08x sw=%u",
                 R[0]&0x7FFFFFFFu,(R[0]&0x80000000u)?"B":"A",R[1],R[2],R[3],R[4],R[5],R[6],R[7],R[8],R[9],
                 R[10],R[11],R[12],R[13],R[14],R[15]);
        fprintf(stderr, "%s\n", m);
        wx86_vita_progress_c(m);
    }
    fflush(stderr);
}
void* LinkNext(x86emu_t* emu, uintptr_t addr, void* x2)
{
    // D2Vita preemption seam: chains to not-yet-translated targets (and, with
    // D2_NOLINK=1, ALL chains) funnel through here; dyn86_should_break()
    // (explicit stop flag) diverts to the epilog with quit set, leaving emu
    // resumable at EIP=addr. The periodic quantum lives in the block-entry
    // budget, not here (see src/dynarec86/dyn86.h).
    if(dyn86_should_break()) {
        emu->quit = 1;
        return arm_epilog;
    }
    // D2Vita transfer ring: record {src block, target, esp} for the fault dump.
    if(dyn86_xfertrace<0) dyn86_xfertrace = getenv("D2_XFERTRACE")?1:0;
    if(dyn86_xfertrace) {
        dynablock_t* sb = FindDynablockFromNativeAddress((void*)((uintptr_t)x2-8));
        g_xf[g_xfi][0] = sb?(uint32_t)(uintptr_t)sb->x86_addr:0;
        g_xf[g_xfi][1] = (uint32_t)addr;
        g_xf[g_xfi][2] = R_ESP;
        g_xfi = (g_xfi+1)&63;
    }
    // D2_EIPTRAP diag: report who chains into the suspect address (wild jump
    // through a corrupted function pointer lands here first).
    if((dyn86_eiptrap && addr==dyn86_eiptrap) || (dyn86_eiptrap2 && addr==dyn86_eiptrap2)) {
        static int hits = 0;
        static int cap = -1;
        if(cap<0){ const char* e=getenv("D2_EIPTRAP_N"); cap=e?atoi(e):4; }
        ++hits;
        // Always record into the silent ring (see dyn86_dump_eipring above);
        // the stderr prints below stay capped by D2_EIPTRAP_N (0 = fully silent).
        {
            uint32_t esp0 = R_ESP, r0 = 0;
            if(getProtection((uintptr_t)esp0)) r0 = *(uint32_t*)DYN86_G2H(esp0);
            uint32_t* R = g_eipring[g_eipi & 31]; g_eipi++;
            R[0]=(uint32_t)hits | (addr==dyn86_eiptrap2 ? 0x80000000u : 0u); R[1]=r0; R[2]=R_EAX; R[3]=R_ECX; R[4]=R_EDX;
            R[5]=R_EBX; R[6]=R_ESI; R[7]=R_EDI; R[8]=esp0; R[9]=R_EBP;
            // Top 6 stack dwords after the return address ([esp+4..+0x18]): the
            // pushed arguments of the trapped call — lets the ring capture the
            // full argument vector for any trapped call site.
            {
                if(getProtection((uintptr_t)esp0) && getProtection((uintptr_t)esp0+28)){
                    uint32_t* q = (uint32_t*)DYN86_G2H(esp0+4);
                    R[10]=q[0]; R[11]=q[1]; R[12]=q[2]; R[13]=q[3]; R[14]=q[4];
                } else R[10]=R[11]=R[12]=R[13]=R[14]=0xDEADDEADu;
                R[15]=d2rt_sw_seq;   // scheduler-event timestamp (which slice)
            }
        }
        if(hits<=cap) {
            dynablock_t* db = FindDynablockFromNativeAddress((void*)((uintptr_t)x2-8));
            // Registers are LIVE only in a diag build where arm_next.S STMs the
            // register file into the emu before LinkNext; stale otherwise.
            // Every guest deref below is gated on getProtection(guest va): sparse
            // layouts have unmapped holes below any fixed bound, and a raw read
            // there host-faults inside this very diagnostic (seen live).
            uint32_t esp = R_ESP, ret0 = 0;
            if(getProtection((uintptr_t)esp)) ret0 = *(uint32_t*)DYN86_G2H(esp);
            char m[224];
            snprintf(m, sizeof m, "[eiptrap#%d] to %08x from blk %08x ret=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x esp=%08x ebp=%08x",
                     hits, (uint32_t)addr, db?(uint32_t)(uintptr_t)db->x86_addr:0, ret0,
                     R_EAX, R_ECX, R_EDX, R_EBX, R_ESI, R_EDI, esp, R_EBP);
            fprintf(stderr, "%s\n", m);
            wx86_vita_progress_c(m);
            if(getProtection((uintptr_t)esp) && getProtection((uintptr_t)esp+28)) {
                uint32_t* s = (uint32_t*)DYN86_G2H(esp);
                fprintf(stderr, "  [stack] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7]);
            }
            const struct { const char* n; uint32_t v; } RG[4] = {
                {"ecx",R_ECX},{"ebx",R_EBX},{"eax",R_EAX},{"esi",R_ESI}};
            for(int k=0;k<4;k++){ uint32_t b=RG[k].v;
                if(b && getProtection((uintptr_t)b) && getProtection((uintptr_t)b+60)) {
                    uint32_t* p = (uint32_t*)DYN86_G2H(b);
                    for(int r=0;r<2;r++)
                        fprintf(stderr, "  [@%s+%02x] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                RG[k].n, r*32, p[r*8+0],p[r*8+1],p[r*8+2],p[r*8+3],
                                p[r*8+4],p[r*8+5],p[r*8+6],p[r*8+7]);
                }
            }
            fflush(stderr);
        }
    }
    #ifdef HAVE_TRACE
    if(!addr) {
        x2-=8;  // actual PC is 2 instructions ahead
        dynablock_t* db = FindDynablockFromNativeAddress(x2);
        void* x86addr = db?(void*)getX86Address(db, (uintptr_t)x2):NULL;
        const char* pcname = getAddrFunctionName((uintptr_t)x86addr);
        printf_log(LOG_NONE, "Warning, jumping to NULL address from %p (db=%p, x86addr=%p/%s)\n", x2, db, x86addr, pcname);
    }
    #endif
    void * jblock;
    dynablock_t* block = NULL;
    if(hasAlternate((void*)addr)) {
        printf_log(LOG_INFO, "Jmp address has alternate: %p", (void*)addr);
        addr = (uintptr_t)getAlternate((void*)addr);
        R_EIP = addr;
        printf_log(LOG_INFO, " -> %p\n", (void*)addr);
        block = DBGetBlock(emu, addr, 1);
    } else
        block = DBGetBlock(emu, addr, 1);
    if(!block) {
        // no block, let link table as is...
        #ifdef HAVE_TRACE
        if(LOG_INFO<=box86_dynarec_log) {
            if(checkInHotPage(addr)) {
                dynarec_log(LOG_INFO, "Not trying to run a block from a Hotpage at %p\n", (void*)addr);
            } else {
                dynablock_t* db = FindDynablockFromNativeAddress(x2-4);
                elfheader_t* h = FindElfAddress(my_context, (uintptr_t)x2-4);
                dynarec_log(LOG_INFO, "Warning, jumping to a no-block address %p from %p (db=%p, x86addr=%p(elf=%s))\n", (void*)addr, x2-4, db, db?(void*)getX86Address(db, (uintptr_t)x2-4):NULL, h?ElfName(h):"(none)");
            }
        }
        #endif
        //tableupdate(arm_epilog, addr, table);
        return arm_epilog;
    }
    if(!block->done) {
        // not finished yet... leave linker
        //tableupdate(arm_linker, addr, table);
        return arm_epilog;
    }
    if(!(jblock=block->block)) {
        // null block, but done: go to epilog, no linker here
        return arm_epilog;
    }
    //dynablock_t *father = block->father?block->father:block;
    // D2Vita eiptrap per-call funnel: put the trapped address's jump-table entry
    // back on the block's jmpnext stub so EVERY future chain to it re-enters
    // LinkNext (stock behaviour only traps the FIRST entry, before the direct
    // link is published). Diagnostic-only; costs one arm_next bounce per call
    // for that single address.
    if(((dyn86_eiptrap && addr==dyn86_eiptrap) || (dyn86_eiptrap2 && addr==dyn86_eiptrap2)) && block->jmpnext) {
        uintptr_t* je = (uintptr_t*)getJumpTableAddress(addr);
        if(je) *je = (uintptr_t)block->jmpnext;
    }
    return jblock;
}
#endif

#if defined(__vita__)
#define JUMPBUFF jmp_buf         /* matches x86emu_private.h (newlib compat) */
#elif defined(ANDROID)
#define JUMPBUFF sigjmp_buf
#else
#define JUMPBUFF struct __jmp_buf_tag
#endif

void DynaCall(x86emu_t* emu, uintptr_t addr)
{
    uint32_t old_esp = R_ESP;
    uint32_t old_ebx = R_EBX;
    uint32_t old_edi = R_EDI;
    uint32_t old_esi = R_ESI;
    uint32_t old_ebp = R_EBP;
    uint32_t old_eip = R_EIP;
    // save defered flags
    defered_flags_t old_df = emu->df;
    uint32_t old_op1 = emu->op1;
    uint32_t old_op2 = emu->op2;
    uint32_t old_res = emu->res;
    // uc_link
    i386_ucontext_t* old_uc_link = emu->uc_link;
    emu->uc_link = NULL;

    PushExit(emu);
    R_EIP = addr;
    emu->df = d_none;
    DynaRun(emu);
    emu->quit = 0;  // reset Quit flags...
    emu->df = d_none;
    emu->uc_link = old_uc_link;
    if(emu->flags.quitonlongjmp && emu->flags.longjmp) {
        if(emu->flags.quitonlongjmp==1)
            emu->flags.longjmp = 0;   // don't change anything because of the longjmp
    } else {
        // restore defered flags
        emu->df = old_df;
        emu->op1 = old_op1;
        emu->op2 = old_op2;
        emu->res = old_res;
        // and the old registers
        R_EBX = old_ebx;
        R_EDI = old_edi;
        R_ESI = old_esi;
        R_EBP = old_ebp;
        R_ESP = old_esp;
        R_EIP = old_eip;  // and set back instruction pointer
    }
}

int my_setcontext(x86emu_t* emu, void* ucp);
int DynaRun(x86emu_t* emu)
{
    // prepare setjump for signal handling
    JUMPBUFF jmpbuf[1] = {0};
    int skip = 0;
    JUMPBUFF *old_jmpbuf = emu->jmpbuf;
    uintptr_t old_savesp = emu->xSPSave;
    emu->flags.jmpbuf_ready = 0;

    while(!(emu->quit)) {
        // D2Vita perf: the sigsetjmp arming (upstream: signal-handler recovery)
        // is DEAD WEIGHT here — nothing in this tree sets flags.need_jmpbuf or
        // siglongjmps to emu->jmpbuf (the FillBlock cancel path uses its own
        // dynarec_jmpbuf, dynablock.c). One sigsetjmp per DynaRun re-entry =
        // one sigprocmask syscall per OS-shim trap (~30-100k/s). Verified by
        // grep across third_party/ + src/runtime/: zero setters, zero longjmp
        // users. `skip` stays 0 (its only role was resuming after a longjmp).
        (void)jmpbuf;
        if(emu->flags.need_jmpbuf)
            emu->flags.need_jmpbuf = 0;

#ifndef DYNAREC
        Run(emu, 0);
#else
        if(!box86_dynarec)
            Run(emu, 0);
        else {
            dynablock_t* block = (skip || ACCESS_FLAG(F_TF))?NULL:DBGetBlock(emu, R_EIP, 1);
            // D2Vita residual TOCTOU (sequel to the DBGetBlock retry, see the
            // comment there): the lost-mark path (MarkDynablock: db->done=0,
            // under mutex_prot only) can still zero done BETWEEN DBGetBlock's
            // "valid block" break and the gate below. Upstream falls back to
            // the interpreter; here Run() is an aborting stub, and unlike the
            // hot block->block chain (LinkNext -> epilog -> re-lookup) this
            // re-entry path has no graceful fallback. Same transient-vs-
            // permanent argument as in DBGetBlock: re-lookup, bounded by the
            // same constant — exhaustion (or NULL: permanent FillBlock
            // failure, F_TF/skip single-step) still reaches the honest abort
            // below. No lock is held while spinning (DynaRun runs GIL-free;
            // DBGetBlock takes and releases its own).
            int dyn86_rerun = 0;
            while(block && !(block->done && block->block) && dyn86_rerun++<DYN86_DBGET_RETRY) {
                sched_yield();
                block = DBGetBlock(emu, R_EIP, 1);
            }
            if(!block || !block->block || !block->done) {
                skip = 0;
                // no block, of block doesn't have DynaRec content (yet, temp is not null)
                // Use interpreter (should use single instruction step...)
                dynarec_log(LOG_DEBUG, "%04d|Running Interpreter @%p, emu=%p\n", GetTID(), (void*)R_EIP, emu);
                if(box86_dynarec_test)
                    emu->test.clean = 0;
                Run(emu, 1);
            } else {
                dynarec_log(LOG_DEBUG, "%04d|Running DynaRec Block @%p (%p) of %d x86 insts (hash=0x%x) emu=%p\n", GetTID(), (void*)R_EIP, block->block, block->isize, block->hash, emu);
                // block is here, let's run it!
                arm_prolog(emu, block->block);
            }
            if(emu->fork) {
                int forktype = emu->fork;
                emu->quit = 0;
                emu->fork = 0;
                emu = x86emu_fork(emu, forktype);
            }
            if(emu->quit && emu->uc_link) {
                emu->quit = 0;
                my_setcontext(emu, emu->uc_link);
            }
        }
#endif
        if(emu->flags.need_jmpbuf)
            emu->quit = 0;
    }
    // clear the setjmp
    emu->jmpbuf = old_jmpbuf;
    emu->xSPSave = old_savesp;
    return 0;   // D2Vita: missing return in upstream Box86 (non-void function)
}