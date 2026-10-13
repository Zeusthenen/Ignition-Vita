/* D2Vita shim implementations for the Box86 dynarec extraction.
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 * Provides the minimal runtime surface the extracted dynarec links against.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "debug.h"
#include "box86context.h"
#include "regs.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86emu.h"
#include "x86run.h"
#include "signals.h"
#include "wrapper.h"
#include "threads.h"
#include "my_cpuid.h"

box86context_t *my_context = 0;

int arm_v8 = 0;     // Cortex-A9 is ARMv7: no ARMv8 VRINT/etc extensions

int checkUnlockMutex(void* m)
{
    (void)m;
    return 0;   // single-threaded PoC: nothing to unlock
}

static x86emu_t* g_thread_emu = 0;
x86emu_t* thread_get_emu(void) { return g_thread_emu; }
void thread_set_emu(x86emu_t* emu) { g_thread_emu = emu; }

/* ---- interpreter stub: no interpreter in the PoC ---- */
int Run(x86emu_t *emu, int step)
{
    (void)step;
    fprintf(stderr, "[dynarec86] Run() interpreter stub hit at EIP=%p — aborting emulation\n", (void*)emu->ip.dword[0]);
    emu->quit = 1;
    emu->error |= ERR_UNIMPL;
    return 0;
}

void PltResolver(x86emu_t* emu) { (void)emu; }

void x86Syscall(x86emu_t *emu) { emu->quit = 1; emu->error |= ERR_UNIMPL; }
void x86Int3(x86emu_t* emu)    { emu->quit = 1; emu->error |= ERR_UNIMPL; }

x86emu_t* x86emu_fork(x86emu_t* emu, int forktype)
{
    (void)forktype;
    emu->quit = 1;
    emu->error |= ERR_UNIMPL;
    return emu;
}

int my_setcontext(x86emu_t* emu, void* ucp)
{
    (void)ucp;
    emu->quit = 1;
    emu->error |= ERR_UNIMPL;
    return 0;
}

/* ---- signals: no guest signal delivery, just stop the emu ---- */
void emit_signal(x86emu_t* emu, int sig, void* addr, int code)
{
    fprintf(stderr, "[dynarec86] emit_signal(%d) at addr=%p code=%d — stopping\n", sig, addr, code);
    emu->quit = 1;
    emu->error |= ERR_ILLEGAL;
}
void emit_div0(x86emu_t* emu, void* addr, int code)
{
    (void)addr; (void)code;
    emu->quit = 1;
    emu->error |= ERR_DIVBY0;
}
void emit_interruption(x86emu_t* emu, int num, void* addr)
{
    (void)num; (void)addr;
    emu->quit = 1;
    emu->error |= ERR_UNIMPL;
}

/* ---- dynarec self-test harness: disabled (box86_dynarec_test==0) ---- */
void x86test_init(x86emu_t* ref, uintptr_t ip) { (void)ref; (void)ip; }
void x86test_check(x86emu_t* ref, uintptr_t ip) { (void)ref; (void)ip; }

int isRetX87Wrapper(wrapper_t fun) { (void)fun; return 0; }

/* ---- cpuid: minimal leaf 0/1 answers ---- */
/* Leaf-1 feature bits, defined ONCE. IsProcessorFeaturePresent
   (win32_shims_kernel32.cpp) derives its answers from these instead of
   choosing its own -- two views of the same CPU that disagree (cpuid
   reporting SSE2, the API saying no) are exactly the kind of inconsistency
   that gives an emulator away. */
#define WX86_CPUID1_EDX ((1<<0)|(1<<15)|(1<<23)|(1<<24)|(1<<25)|(1<<26)) /* FPU CMOV MMX FXSR SSE SSE2 */
#define WX86_CPUID1_ECX (0)                                             /* no SSE3+ */
void wx86_cpuid_features(uint32_t* edx, uint32_t* ecx)
{
    if(edx) *edx = WX86_CPUID1_EDX;
    if(ecx) *ecx = WX86_CPUID1_ECX;
}

void my_cpuid(x86emu_t* emu, uint32_t tmp32u)
{
    static int dbg=-1; if(dbg<0){ dbg=(getenv("WX86_CPUIDLOG")?getenv("WX86_CPUIDLOG"):getenv("D2_CPUIDLOG"))?1:0; }
    emu->regs[_AX].dword[0] = 0;
    switch(tmp32u) {
        case 0x0:
            emu->regs[_AX].dword[0] = 0x1; // max level
            emu->regs[_BX].dword[0] = 0x756e6547; // Genu
            emu->regs[_DX].dword[0] = 0x49656e69; // ineI
            emu->regs[_CX].dword[0] = 0x6c65746e; // ntel
            break;
        case 0x1:
            // Report a Pentium III (family 6, model 8, stepping 1). D2 1.14d
            // (2016 build) indexes a CPU-type string table by family; family 4
            // (486) hits an invalid/-1 slot in its table and its CPU-info
            // logger then strlen()s the -1 pointer -> crash. Family 6 is a slot
            // every D2 build has. 1.13c only *prints* this string, so its
            // frames/switches stay byte-identical.
            emu->regs[_AX].dword[0] = 0x00000681; // family 6, model 8, stepping 1
            emu->regs[_BX].dword[0] = 0;
            emu->regs[_CX].dword[0] = WX86_CPUID1_ECX;   // no SSE3+
            emu->regs[_DX].dword[0] = WX86_CPUID1_EDX;   // FPU CMOV MMX FXSR SSE SSE2
            break;
        default:
            emu->regs[_BX].dword[0] = 0;
            emu->regs[_CX].dword[0] = 0;
            emu->regs[_DX].dword[0] = 0;
            break;
    }
    if(dbg) fprintf(stderr,"[cpuid] leaf=0x%x -> eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
        tmp32u, emu->regs[_AX].dword[0], emu->regs[_BX].dword[0],
        emu->regs[_CX].dword[0], emu->regs[_DX].dword[0]);
}

uint64_t ReadTSC(x86emu_t* emu)
{
    (void)emu;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

/* ---- flat segmentation: offsets are set directly by the host ---- */
uintptr_t GetSegmentBaseEmu(x86emu_t* emu, int seg)
{
    /* mark the serial valid so the emitted grab_fsdata fast path (serial!=0
       -> read segs_offs directly) engages after the first access; the host
       (CpuBox86::set_fs_base) updates segs_offs and keeps serial non-zero */
    emu->segs_serial[seg] = 1;
    return emu->segs_offs[seg];
}

/* parity lookup table (from Box86 src/emu/x86emu.c, MIT) */
static uint32_t x86emu_parity_tab[8] =
{
    0x96696996,
    0x69969669,
    0x69969669,
    0x96696996,
    0x69969669,
    0x96696996,
    0x96696996,
    0x69969669,
};

void dynarec86_setup_emu_helpers(x86emu_t* emu)
{
    emu->x86emu_parity_tab = x86emu_parity_tab;
    emu->eflags.x32 = 0x202;    // default flags
    // sbiidx: scaled-index helpers — [4] points to a zero reg
    for(int i=0; i<8; ++i)
        emu->sbiidx[i] = &emu->regs[i];
    emu->sbiidx[4] = &emu->zero;
}
