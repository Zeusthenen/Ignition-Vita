// src/runtime/cpu_box86.cpp — Box86-dynarec-backed x86-32 CPU (ARM only).
//
// Implements the same Cpu contract as cpu_unicorn.cpp on top of the vendored
// Box86 dynarec core (third_party/box86-dynarec + src/dynarec86/shim).
// Memory model is IDENTITY: guest VA == host address (map() uses MAP_FIXED),
// which is exactly how Box86 itself runs x86 code.
//
// Trap mechanism: set_trap() maps the [lo,hi) window and fills every 16-byte
// slot (the Bridge allocates trap VAs 16 bytes apart, sentinel included) with
// Box86's "exit" bridge stub: CC 'S' 'C' + 4 zero bytes. The dynarec compiles
// that to emu->quit=1 + EIP=<slot>; this backend's run() loop detects EIP inside the
// window, invokes the TrapFn (guest state is at a clean instruction boundary,
// ESP still pointing at the pushed return address — same contract as the
// Unicorn fetch-unmapped hook), then resumes at whatever EIP the handler set.
// This round-trips through the dynarec prolog/epilog on every native call.
//
// Preemption is implemented via the block-entry budget: every
// translated block's prologue decrements emu->dyn86_budget (recharged per
// slice in run()) and exits DynaRun resumable at the block's start when it
// expires; request_stop() zeroes the budget (plus dyn86_request_stop's flag
// for the LinkNext seam, which covers not-yet-translated targets). Blocks
// stay direct-linked. Residual limitation: a loop contained in a SINGLE
// dynablock never re-enters a prologue and cannot be preempted this way
// (guest threads that yield via import traps are unaffected).
//
// TODO (deferred):
//  * SMC: protectDB write-protects pages holding translated code; Box86
//    normally catches the SIGSEGV of a guest self-write and invalidates.
//    No segv handler is installed here yet; host-side Cpu::write() does call
//    unprotectDB() first, so bridge-side writes (IAT patches...) are safe.
//  * single instance: the Box86 core has one global my_context; only one
//    CpuBox86 may exist per process (asserted in make_cpu_box86).
//
// Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
#ifdef __arm__

#include <cstdint>
#include "runtime/guest_thread.h"   // X86Context (per-thread FPU blob)
#include "runtime/prof_map.h" // address-family map for the profiler (provided by the port)
#include "runtime/cpu.h"    // MUST be included before the Box86 headers:
                            // Box86's regs.h #defines R_EAX & friends.
#include "runtime/gil.h"    // GIL guard at the trap dispatch (inert under the cooperative backend)
#include "runtime/trapcnt.h"// per-slot acquisition counter: the intrinsic path
                            // bypasses the Bridge, so this is where it must be
                            // counted (dependency-free header)
#ifndef __vita__            // Vita delivers no POSIX signals; the SIGSEGV diag
#include <csignal>          // handler is a desktop/qemu debug aid only.
#include <ucontext.h>
#endif
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>      // timing for contended lock acquisitions (D2_FILSTAT)
#include <pthread.h>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

// Box86 build-mode macros (the vendored headers branch on them).
#ifndef DYNAREC
#define DYNAREC
#endif
#ifndef ARM
#define ARM
#endif

extern "C" {
#include "debug.h"
#include "box86context.h"
#include "regs.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "custommem.h"
#include "dynablock.h"
#include "dynarec/dynablock_private.h"
#include "dyn86.h"
#include "dyn86_memintrin.h"    // native memcpy/memset intrinsics (D2_MEMINTRIN)

void dynarec86_setup_emu_helpers(x86emu_t* emu);    // shim_impl.c
}

// Box86's regs.h macros would shadow d2rt's enum Reg constants (the enum was
// already parsed inside cpu.h, so only *uses* below need the macros gone).
#undef R_EAX
#undef R_ECX
#undef R_EDX
#undef R_EBX
#undef R_ESP
#undef R_EBP
#undef R_ESI
#undef R_EDI
#undef R_EIP
#undef R_EFLAGS

// D2_EIPPROF sampling profiler (see run()). Buckets are C-visible so rt_boot
// can print them; base is the guest image base (set once the exe is mapped).
// Must stay at FILE scope: inside d2rt's anonymous namespace, GCC 15
// (VitaSDK) gives internal linkage priority and mangles these symbols despite
// `extern "C"`, so the Vita link fails with `undefined reference to
// d2rt_eipprof_*` (GCC 13 in the qemu build accepts it either way).
extern "C" {
    uint32_t d2rt_eipprof_on = 0;
    uint32_t d2rt_eipprof_base = 0;
    uint64_t d2rt_eipprof[8] = {0};
    uint64_t d2rt_eipprof_sub[32] = {0};    // 0x0f0000 + n*0x1000
    uint64_t d2rt_eipprof_sub2[32] = {0};   // 0x0d0000 + n*0x1000
    uint64_t d2rt_eipprof_fn[16] = {0};     // 0x0fa000 + n*0x100 (function-level zoom)
    uint64_t d2rt_eipprof_sub3[32] = {0};   // (reserved)
    uint64_t d2rt_eipprof_all[96] = {0};    // n*0x8000 across the whole .text
    // Exact histogram of sampled EIPs (= block start addresses). The 32 KiB
    // buckets say "where", not "what": porting a function needs its address,
    // not its slice. Open-addressed hash table; ~10k samples for 8192 slots,
    // so saturation doesn't happen in practice — and if it did, the sample
    // would be DROPPED, never reattributed to another address.
    uint32_t d2rt_eipprof_key[8192] = {0};
    uint64_t d2rt_eipprof_hit[8192] = {0};

    // ---- D2_TIMEPROF: time-based profiling of guest code -------------------
    // D2_EIPPROF samples on expiry of a BLOCK-count budget: its percentages
    // are shares of block ENTRIES, not of time, and it over-represents short,
    // frequently-called functions. Here the budget is servo-controlled so
    // sampling intervals are equal in TIME instead.
    //
    // What this does NOT fix: the servo only equalizes interval DURATION; it
    // doesn't change WHICH block gets sampled inside an interval — the sample
    // is still drawn at a fixed block count. The sampled address therefore
    // stays distributed proportionally to ENTRY FREQUENCY, same as
    // D2_EIPPROF.
    //
    // What it adds anyway: far more samples and distinct addresses, so
    // rankings beyond the top ~20 become usable, and periodic aliasing (a
    // fixed sampling period beating against a periodic per-frame workload)
    // is avoided.
    //
    // Cost: significant at a small target interval (e.g. 250us) — never use
    // a small interval in a speed-sensitive run. A larger target (e.g.
    // 1000us) cuts the cost roughly 4x.
    //
    // A true time profiler would sample the PC at chosen instants; not
    // possible here without a cycle counter (PMCCNTR, inaccessible from Vita
    // userland without kernel-mode PMUSERENR) — the guest EIP lives in r14
    // and is only written to memory at block epilogues.
    uint32_t d2rt_timeprof_on = 0;       // 0 = off; else target interval in us
    uint32_t d2rt_timeprof_base = 0;
    uint64_t d2rt_tp_key[8192] = {0};    // guest address (0 = empty slot)
    uint64_t d2rt_tp_hit[8192] = {0};
    uint64_t d2rt_tp_bucket[8] = {0};    // same families as d2rt_eipprof
    uint64_t d2rt_tp_samples = 0;        // samples RETAINED
    uint64_t d2rt_tp_susp = 0;           // intervals rejected (thread suspended)
    uint64_t d2rt_tp_us = 0;             // time covered by the samples
    uint64_t d2rt_tp_blocks = 0;         // blocks covered (for the average budget)

    // ---- D2_LAGWATCH: sample ring for attributing a stall -------------------
    // One-off stalls are a different problem from throughput: they are RARE
    // events, happen while the user is playing, and can't be replayed.
    // D2_FRAMEPROF already knows WHEN they happen and attributes HOST-side
    // causes (JIT translation, I-cache sync, file reads, decompression,
    // thread switches). What's missing is WHICH GUEST function was running
    // during the stall.
    // This ring keeps recent samples (timestamp, EIP, thread). When a frame
    // exceeds the threshold, rt_boot looks up the samples that fall within
    // that frame and reports the dominant addresses.
    // Cost is that of the underlying sampler (D2_TIMEPROF) alone, so it is
    // set by its interval — LAGWATCH defaults to a larger interval to keep
    // the overhead low.
    // Limitation: a guest loop contained in a SINGLE dynablock never
    // re-enters a prologue and so is NEVER sampled. A stall of this kind
    // shows up as zero samples, which is itself informative, not silence.
    uint32_t d2rt_lag_on = 0;            // 1 = ring is being fed
    #define D2RT_LAG_RING 2048u
    uint64_t d2rt_lag_t[D2RT_LAG_RING]  = {0};   // timestamp (us)
    uint32_t d2rt_lag_ip[D2RT_LAG_RING] = {0};   // guest EIP
    uint32_t d2rt_lag_w = 0;                     // write cursor, monotonic
}

// These definitions must stay at GLOBAL scope. Inside this file's anonymous
// namespace, `extern "C"` does not give them external linkage — the symbol
// keeps its mangled name (_ZN4d2rt12_GLOBAL__N_113d2rt_b5_callsE) and rt_boot
// can no longer find it. g++ accepts this silently; the Vita toolchain
// (arm-vita-eabi) rejects it at link time — same source, different verdict
// per toolchain.
#if defined(D2_TLSCOUNT) || defined(D2_B5CENSUS)
extern "C" { unsigned long long d2_tls_hits = 0; }
#endif
// dynablock.c's counter of translations that produced no block -- in practice,
// the JIT pool refusing memory. Read (never written) here to tell an exhausted
// pool apart from a genuinely unimplemented opcode when naming a fault.
extern "C" uint32_t dyn86_fill_fail;

extern "C" {
    unsigned long long d2rt_b5_calls  = 0;   // entries into try_intrinsic
    unsigned long long d2rt_b5_loads  = 0;   // intrinsics-table lookups
    unsigned long long d2rt_b5_hits   = 0;   // intrinsics actually serviced
    unsigned long long d2rt_b5_emutls = 0;   // per-thread state resolutions inside a serviced call
    unsigned int       d2rt_b5_index_on = 0; // 1 = direct-index path armed
    unsigned int       d2rt_b5_direct_n = 0; // slots covered by the direct index
    unsigned int       d2rt_b5_direct_lo = 0;// VA of slot index 0
}

// ---- Address-family map for the profiler (runtime/prof_map.h) ----------
// At GLOBAL scope for the same reason as the counters above: these two
// accessors are engine symbols, and the inline helpers need to see them from
// the anonymous namespace below.
static Wx86ProfMap g_profMap;
void wx86_prof_set_map(const Wx86ProfMap& m) { g_profMap = m; }
const Wx86ProfMap& wx86_prof_map() { return g_profMap; }

// Classification itself lives in prof_map.h (inline functions) so a desktop
// build can exercise it too — this translation unit only compiles for
// ARM/Vita. Here only the three detail windows are wired to their counters.
// They don't need to check which family matched: the windows are disjoint,
// so testing "rva falls in this window" alone gives the same counts as
// checking family-then-range would.
static inline void wx86_prof_zooms(uint32_t rva) {
    const Wx86ProfMap& m = g_profMap;
    int k;
    if ((k = wx86_prof_zoom(m.zoom_4k_a, 12, 32, rva)) >= 0) ++d2rt_eipprof_sub[k];
    if ((k = wx86_prof_zoom(m.zoom_4k_b, 12, 32, rva)) >= 0) ++d2rt_eipprof_sub2[k];
    if ((k = wx86_prof_zoom(m.zoom_256,   8, 16, rva)) >= 0) ++d2rt_eipprof_fn[k];
}

namespace d2rt {
namespace {

// Diagnostic SIGSEGV/SIGBUS handler: with the identity memory model a guest
// wild access is a host fault — print the guest state (EIP & friends) before
// dying so the crash is attributable. Async-signal-unsafe printf is fine for
// a terminal diagnostic. This is also the seed of the future SMC handler.
extern "C" x86emu_t* dyn86_diag_emu = nullptr;
// Crash-log fd (rt_boot opens $D2WRITE/crash.log and sets this). diag_segv mirrors
// a concise one-line summary here with async-signal-safe write() BEFORE the risky
// guest-stack scans below — so a host SIGSEGV leaves a durable post-mortem line
// even if the scan itself re-faults. -1 = no crash log (write() is skipped).
extern "C" int dyn86_crash_fd = -1;
static uintptr_t g_mb;   // guest->host membase (defined once; H(va)=va+g_mb). Fwd for diag_segv.
#ifndef __vita__
static void diag_segv(int sig, siginfo_t* si, void* uctx) {
    x86emu_t* e = dyn86_diag_emu;
    uintptr_t pc = 0, lr = 0, sp = 0, r0 = 0, r1 = 0, r2 = 0;
    uintptr_t live[8] = {0}; bool live_ok = false;
#if defined(__arm__)
    if (uctx) {
        auto& mc = ((ucontext_t*)uctx)->uc_mcontext;
        pc = mc.arm_pc; lr = mc.arm_lr; sp = mc.arm_sp;
        r0 = mc.arm_r0; r1 = mc.arm_r1; r2 = mc.arm_r2;
        // box86/ARM convention: r4..r11 ARE the eight live x86 registers
        // (EAX ECX EDX EBX ESP EBP ESI EDI). emu->ip/emu->regs are only
        // synced at block boundaries — mid-dynablock they're STALE, while
        // these eight ARM registers are exact at the faulting instruction.
        live[0]=mc.arm_r4; live[1]=mc.arm_r5; live[2]=mc.arm_r6; live[3]=mc.arm_r7;
        live[4]=mc.arm_r8; live[5]=mc.arm_r9; live[6]=mc.arm_r10; live[7]=mc.arm_fp;
        live_ok = true;
    }
#endif
    if (live_ok)
        fprintf(stderr, "\n[cpu_box86] x86 VIVANTS (r4..r11) EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x",
                (unsigned)live[0],(unsigned)live[1],(unsigned)live[2],(unsigned)live[3],
                (unsigned)live[4],(unsigned)live[5],(unsigned)live[6],(unsigned)live[7]);
    // Durable crash line FIRST (before the guest-stack scans, which may re-fault).
    if (dyn86_crash_fd >= 0) {
        char cb[224];
        int n = snprintf(cb, sizeof cb,
            "CRASH host-sig=%d fault-addr=%p host-pc=%p guest-EIP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
            sig, si ? si->si_addr : nullptr, (void*)pc,
            e?e->ip.dword[0]:0, e?e->regs[0].dword[0]:0, e?e->regs[1].dword[0]:0, e?e->regs[2].dword[0]:0,
            e?e->regs[3].dword[0]:0, e?e->regs[4].dword[0]:0, e?e->regs[5].dword[0]:0,
            e?e->regs[6].dword[0]:0, e?e->regs[7].dword[0]:0);
        if (n > 0) { ssize_t w = write(dyn86_crash_fd, cb, (size_t)n); (void)w; fsync(dyn86_crash_fd); }
    }
    fprintf(stderr, "\n[cpu_box86] host signal %d at addr=%p host-pc=%p lr=%p sp=%p r0=%p r1=%p r2=%p",
            sig, si ? si->si_addr : nullptr, (void*)pc, (void*)lr, (void*)sp, (void*)r0, (void*)r1, (void*)r2);
    // which guest block was executing? (host-pc inside a dynarec chunk)
    if (dynablock_t* db = FindDynablockFromNativeAddress((void*)pc)) {
        fprintf(stderr, " | dynablock x86=%p size=%d hostoff=+0x%x",
                db->x86_addr, db->x86_size, (unsigned)(pc - (uintptr_t)db->block));
        // EXACT x86 address of the faulting instruction, derived by walking
        // the dynablock's instsize table rather than printing emu->ip (which
        // is only synced at block boundaries). For each translated x86
        // instruction, instsize gives its x86 size and its native size in
        // words: walk both until the host PC is bracketed. An entry value of
        // 15 means "the next one continues" (box86's 4-bit encoding).
        if (db->instsize && db->x86_addr) {
            uintptr_t x86a = (uintptr_t)db->x86_addr, arma = (uintptr_t)db->block;
            int i = 0;
            while (db->instsize[i].x86 || db->instsize[i].nat) {
                int xs = 0, as = 0;
                do { xs += db->instsize[i].x86; as += db->instsize[i].nat * 4; ++i; }
                while (db->instsize[i-1].x86 == 15 || db->instsize[i-1].nat == 15);
                if (pc >= arma && pc < arma + (uintptr_t)as) {
                    fprintf(stderr, " x86-insn=0x%08x", (unsigned)DYN86_H2G(x86a));
                    if (dyn86_crash_fd >= 0) { char xb[64];
                        int xn = snprintf(xb, sizeof xb, "CRASH x86-insn=0x%08x\n", (unsigned)DYN86_H2G(x86a));
                        if (xn > 0) { ssize_t w = write(dyn86_crash_fd, xb, (size_t)xn); (void)w; } }
                    break;
                }
                arma += as; x86a += xs;
            }
        }
    }
    // a few return-address candidates off the host stack (caller of the faulting fn)
    if (sp) {
        fprintf(stderr, "\n[cpu_box86] host stack:");
        for (int i = 0; i < 24; ++i) {
            uintptr_t v = ((uintptr_t*)sp)[i];
            if (v >= 0x76000000 && v < 0x76400000) fprintf(stderr, " [%d]=%p", i, (void*)v);
        }
    }
    // NOTE: emu->ip is only synced at block boundaries/STM points — treat as approximate.
    if (e)
        fprintf(stderr, " | guest EIP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x",
                e->ip.dword[0], e->regs[0].dword[0], e->regs[1].dword[0], e->regs[2].dword[0],
                e->regs[3].dword[0], e->regs[4].dword[0], e->regs[5].dword[0],
                e->regs[6].dword[0], e->regs[7].dword[0]);
    // Guest call chain: scan the guest stack for return addresses (values in a
    // loaded-module code range). H(va)=va+g_mb, so the guest stack is directly
    // readable in the host address space. Env D2_CODELO/HI override the range.
    if (e) {   // g_mb may be 0 (identity mmap) — gesp+g_mb is still the host ptr
        uint32_t gesp = e->regs[4].dword[0];
        uint32_t lo = 0x01900000, hi = 0x02200000;   // compact-layout module window (9 MiB), sized for a forced-relocation guest image; override via D2_CODELO/HI
        if (const char* s = getenv("WX86_CODELO") ? getenv("WX86_CODELO") : getenv("D2_CODELO")) lo = (uint32_t)strtoul(s, nullptr, 16);
        if (const char* s = getenv("WX86_CODEHI") ? getenv("WX86_CODEHI") : getenv("D2_CODEHI")) hi = (uint32_t)strtoul(s, nullptr, 16);
        const uint32_t* gs = (const uint32_t*)(uintptr_t)(gesp + g_mb);
        // Raw stack words: the faulting function's args (a -1 %s pointer + the
        // adjacent format-string pointer are visible here). ASCII-annotate any
        // word that points at a printable guest string.
        fprintf(stderr, "\n[cpu_box86] guest stack @%08x:", gesp);
        for (int i = 0; i < 16; ++i) {
            uint32_t v = gs[i];
            fprintf(stderr, " %08x", v);
            const char* p = (const char*)(uintptr_t)(v + g_mb);
            if (v > 0x1000 && v < 0x60000000) {
                char buf[20]; int n = 0; bool printable = true;
                for (; n < 16; ++n) { char c = p[n]; if (c == 0) break;
                    if (c < 0x20 || c > 0x7e) { printable = false; break; } buf[n] = c; }
                if (printable && n >= 3) { buf[n] = 0; fprintf(stderr, "(\"%s\")", buf); }
            }
        }
        // Return-address chain (text range only).
        fprintf(stderr, "\n[cpu_box86] guest call chain:");
        for (int i = 0, shown = 0; i < 256 && shown < 12; ++i) {
            uint32_t v = gs[i];
            if (v >= lo && v < hi) { fprintf(stderr, " %08x", v); ++shown; }
        }
    }
    fprintf(stderr, "\n");
    _exit(139);
}
#endif // !__vita__


// fastmmu: guest->host delta (env WX86_MEMBASE, falls back to D2MEMBASE; hex,
// low 24 bits zero; 0 = identity). The guest keeps its validated memory
// layout; host pages live at va+g_mb — the exact Vita memory model (memblock
// VAs are kernel-assigned). g_mb is forward-declared above (before
// diag_segv); initialized here.
//
// Engine log. An early fatal exit MUST leave a line: on hardware there is no
// stderr, and a silent _exit reads as "the application just closes at
// launch" with no way to tell why.
//
// The logging service belongs to the engine (platform/vita_host.h): on
// console it writes the durable line, off console it's a no-op. The call is
// DIRECT and strongly linked — a generic engine must not hard-wire a
// specific consumer's symbol name as a weak-reference fallback, since a port
// whose symbols don't carry that prefix would get a silently muted log with
// no link error to flag it.
#include "platform/vita_host.h"
static inline void* H(uint32_t va) { return (void*)((uintptr_t)va + g_mb); }
// Evaluated LAZILY, not as a pre-main static: on Vita the knob arrives via
// env.txt, which platform_init applies AFTER static init, so a pre-main
// getenv could never see it. -1 = not yet checked.
static int g_memGuardV = -1;
static inline bool mem_guard(){ if(g_memGuardV<0) g_memGuardV = (std::getenv("WX86_MEMGUARD")||std::getenv("D2_MEMGUARD"))?1:0; return g_memGuardV!=0; }
// Single-arena mode (the exact Vita model): ONE host block covers the whole
// guest span [0, arena_size); membase = block base (kernel/host-assigned, not
// chosen); map()/set_trap() no longer host-mmap per region — the block already
// backs them. On Vita this block is one sceKernelAllocMemBlockForVM; here it is
// one lazy mmap (overcommit), proving the model without needing the compressed
// hardware layout yet. Enabled by WX86_ARENA=<hex bytes> (falls back to D2ARENA).
static bool     g_arena = false;
// (D2_EIPPROF counters: defined at FILE SCOPE, see above `namespace d2rt`)
#define g_eipProf        d2rt_eipprof_on
#define g_eipProfBase    d2rt_eipprof_base
#define g_eipProfBuckets d2rt_eipprof
// Usable guest span behind the single block ([0, g_arena_span)). 0 = unknown
// (sparse/identity layouts): the check below is then skipped.
static uint32_t g_arena_span = 0;
static uint64_t g_arenaViol = 0;
// One cheap compare per shim-side deref. A guest VA past the block would alias
// host memory with no fault; name it (durably, once per 64) and refuse instead.
static inline bool arena_check(uint32_t va, uint32_t n, const char* what) {
    if (!g_arena_span) return true;                       // unknown span: legacy behaviour
    if ((uint64_t)va + n <= (uint64_t)g_arena_span) return true;
    if (++g_arenaViol <= 64) {
        fprintf(stderr, "[cpu_box86] C3 OUT-OF-ARENA %s va=0x%08x n=%u span=0x%08x\n", what, va, n, g_arena_span);
        { char m[128];
            snprintf(m, sizeof m, "C3: acces hors arene %s va=%08x n=%u (span=%08x)", what, va, n, g_arena_span);
            wx86_vita_progress_c(m); }
    }
    return false;
}

class CpuBox86;
static CpuBox86* g_self = nullptr;     // single instance (asserted in make_cpu_box86)

// Native backend: per-HOST-thread current emu. Zero (=> base emu_) on the
// main thread and under the cooperative backend — the single-emu behavior is
// simply the TLS default, not a mode test.
#if defined(D2_TLSCOUNT) || defined(D2_B5CENSUS)
#define D2_TLSCOUNT_HIT() (++d2_tls_hits)
// Counts calls to E(), incremented BEFORE the g_multi_emu shortcut. The two
// counters measure different things, and both are needed:
//   * d2_tls_hits = per-thread state resolutions ACTUALLY paid in THIS run.
//     Under the cooperative backend the shortcut keeps it at zero, so it
//     says nothing about the production (native-scheduler) regime.
//   * d2_e_calls  = calls to E(), i.e. the number of resolutions the
//     PRODUCTION regime (D2SCHED=native, where g_multi_emu is true by
//     construction) would pay for the same guest work. This is the counter
//     to use for a census, because it is SCHEDULER-INDEPENDENT: a
//     deterministic bench under the cooperative backend gives the same
//     number as the native bench for the same scenario, which d2_tls_hits
//     cannot.
// No per-call cost is derived here: this counter is a COUNT, and the actual
// cost per resolution has to be measured on real hardware, not under qemu.
extern "C" { unsigned long long d2_e_calls = 0; }
#define D2_ECALL() (++d2_e_calls)
#else
#define D2_TLSCOUNT_HIT() ((void)0)
#define D2_ECALL()        ((void)0)
#endif
// ---- Census counters (measurement build only: -DD2_B5CENSUS, never shipped) ----
// Exactly what each counter measures:
//   calls  = entries into try_intrinsic (one per guest->host trap, whether
//            or not the fast path services it);
//   loads  = accesses to the intrinsics TABLE (one per probe iteration on
//            the legacy linear-scan path; zero on the direct-index path
//            when the slot is outside the indexed window);
//   hits   = calls where the intrinsic returned true (so the trap never
//            enters the bridge);
//   emutls = per-thread state resolutions (E()) charged inside the
//            serviced intrinsic's body. This is a COUNT, not an estimate:
//            E() is the only site that becomes __emutls_get_address on
//            Vita, and it is instrumented at the source. It is 0 under the
//            cooperative backend BY CONSTRUCTION (the g_multi_emu shortcut
//            makes E() a no-op on TLS), so this counter is only meaningful
//            under D2SCHED=native.
// Always DEFINED (so rt_boot can print them unconditionally), but only
// INCREMENTED under D2_B5CENSUS: an unconditional ++ on this path would add
// real, measurable overhead on every call — exactly what this build flag
// exists to avoid.
// (kept at global scope: same linkage reason as above.)
// D2_B5INDEX=1 (or D2_B5=1): DIRECT index into the intrinsics table instead
// of the linear probe. Default is the plain linear-scan path.
static bool g_b5index = false;
#ifdef D2_B5CENSUS
// Scale test for the census (measurement build only — this code doesn't
// exist in the shipped binary). D2_B5SCALE=N repeats, per serviced
// intrinsic, N-1 EXTRA times exactly the work the two counters claim to
// measure: one table lookup and one per-thread state resolution (E()). The
// counters must then increase by (N-1) per service — a counter that doesn't
// move isn't measuring what it claims to.
// NO SIDE EFFECTS: reading ikey_[] and regs[R_ESP] is pure, and the result
// goes into a global sink that is never read back.
static uint32_t g_b5scale = 1;
extern "C" { unsigned long long d2rt_b5_sink = 0; }
#endif
// Rollback switch for emutls optimizations — D2_NOEMUOPT=1.
// Under the native backend, the only emutls optimization still active in the
// hot loop is HOISTING: E() resolved once per time slice instead of on every
// access (the g_multi_emu shortcut only helps the cooperative backend; under
// native the flag is true and the plain path runs unchanged). This flag
// disables that hoist, and restores the per-trap write of t_fault_addr that
// was removed for the same reason. Purpose: an A/B comparison without
// leaving D2SCHED=native.
// Read once: a getenv() per time slice would cost more than what it measures.
static bool g_noEmuOpt = false;
// D2_FILSTAT=1: per-thread trap density and GIL contention
// (x86emu_t::dyn86_traps / dyn86_gilcont / dyn86_gilwait_us). Read once.
// Default OFF: the trap path keeps a bare gil::lock() plus one global bool
// check (the pattern used by every knob in this file). Also armed by
// D2_COEUR_SERVEUR.
static bool g_filstat = false;
static __thread x86emu_t* t_emu = nullptr;
// D2_TIMEPROF per-thread servo state. Kept at file scope rather than a class
// static member: a `static __thread` member needs an out-of-class
// definition, and d2rt's anonymous namespace would mangle the symbol (same
// trap as the linkage note at the top of this file).
static __thread uint64_t tp_last_us = 0;
static __thread int      tp_budget  = 0;
// False until a per-thread emu exists (i.e. while t_emu is null everywhere).
// Set only once, before any runner's pthread_create: the happens-before edge
// from pthread_create is enough on its own, so relaxed ordering here is just
// hygiene, not a load-bearing precaution.
static std::atomic<bool> g_multi_emu{false};
// Fault/preempt state is per-thread: with N host threads running run()
// concurrently, a worker's fault must not clobber the main thread's pending
// one.
static __thread int         t_preempt = 0;
static __thread const char* t_fault = nullptr;
static __thread uint32_t    t_fault_addr = 0;
static __thread uint32_t    t_fault_err = 0;

// GIL acquisition at the trap window, with PER-THREAD counting under
// D2_FILSTAT=1. Without the knob: exactly gil::Guard (bare lock/unlock). With
// it: try the lock first; on failure the acquisition is CONTENDED — count it
// and time the blocking lock (clock_gettime ONLY on this rare path, never on
// an uncontended acquisition). The counters live in the thread's emu: that's
// what the starvation-watch "fils:" line reads (thread_emu_filstat).
static inline uint32_t filstat_now_us() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
}
struct TrapGuard {
    bool a;
    explicit TrapGuard(x86emu_t& e) {
        a = gil::active(); if (!a) return;
        if (!g_filstat) { gil::lock(); return; }
        ++e.dyn86_traps;
        if (gil::try_lock()) return;
        ++e.dyn86_gilcont;
        const uint32_t t0 = filstat_now_us();
        gil::lock();
        e.dyn86_gilwait_us += filstat_now_us() - t0;
    }
    ~TrapGuard() { if (a) gil::unlock(); }
    TrapGuard(const TrapGuard&) = delete; TrapGuard& operator=(const TrapGuard&) = delete;
};

class CpuBox86 : public Cpu {
public:
    CpuBox86() {
        // emutls-optimization rollback flag, read ONCE: env.txt is already
        // loaded by the time this constructor runs.
        g_noEmuOpt = getenv("WX86_NOEMUOPT") || getenv("D2_NOEMUOPT");
        {   const char* fs = getenv("WX86_FILSTAT"); if (!fs) fs = getenv("D2_FILSTAT");
            const char* cs = getenv("WX86_COEUR_SERVEUR"); if (!cs) cs = getenv("D2_COEUR_SERVEUR");
            g_filstat = (fs && fs[0] && !(fs[0]=='0' && !fs[1]))
                     || (cs && cs[0] && !(cs[0]=='0' && !cs[1])); }
        // B5 flag: read once (a getenv per trap would cost more than what it
        // measures). "0" explicitly means OFF, same convention as INLINEHOT.
        {   auto on = [](const char* n){ const char* v = getenv(n);
                                        return v && v[0] && !(v[0]=='0' && !v[1]); };
            g_b5index = on("WX86_B5INDEX") || on("D2_B5INDEX") || on("WX86_B5") || on("D2_B5");
            d2rt_b5_index_on = g_b5index ? 1u : 0u; }
#ifdef D2_B5CENSUS
        if (const char* sc = getenv("WX86_B5SCALE") ? getenv("WX86_B5SCALE") : getenv("D2_B5SCALE")) {
            unsigned long v = strtoul(sc, nullptr, 10);
            if (v >= 1 && v <= 1024) g_b5scale = (uint32_t)v;
        }
#endif
        if (g_noEmuOpt)
            wx86_vita_progress_c("emutls: OPTIMISATIONS DESACTIVEES (D2_NOEMUOPT=1) — "
                                 "E() resolu a chaque acces, t_fault_addr reecrit par trap");
        const char* as = getenv("WX86_ARENA"); if (!as) as = getenv("D2ARENA");
        if (as) {
            // 16 MiB (0x1000000) is a KERNEL CEILING per VM memory block on
            // this platform, not a project choice: requesting a single block
            // larger than that fails with SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW
            // regardless of how much free memory remains.
            //
            // Known latent bug: jitpool_reserve() (mman_vita.c) sets
            // g_jitpool_tried=1 BEFORE it knows whether the allocation
            // succeeded. If that allocation is refused for any reason, the
            // JIT pool is disarmed for the rest of the session with no
            // retry — the only trace is the REFUSED log line. Not fixed
            // here: fixing it changes behavior, not just diagnostics.
            //
            // The arena's alignment slack (a few MB) is real waste with no
            // zero-cost fix: recovering it would mean shrinking a different,
            // already-calibrated margin elsewhere.
            uint64_t sz = strtoull(as, nullptr, 16);
            // Page-granular size: only the guest->host DELTA needs 16 MiB
            // alignment, and D2ARENA already carries that slack. Rounding the
            // SIZE itself up to 16 MiB too would ask the kernel for several
            // MB more than necessary — on real hardware that can be the
            // difference between booting and failing.
            sz = (sz + 0xFFFull) & ~0xFFFull;
            // Reserve only the alignment slack actually needed. D2ARENA's
            // 16 MiB of margin exists solely so membase can be rounded up to
            // a 16 MiB multiple (the ADD imm8-ror-8 encoding constraint);
            // paying the full 16 MiB on every allocation is wasteful when
            // the slack actually needed is usually much smaller.
            //
            // The exact slack depends on the base address the kernel happens
            // to hand back, which itself depends on prior allocations — so a
            // fixed guess can under- or over-shoot. This matters: if the
            // arena ends up too tight, the JIT pool can't grow, and since
            // there is no interpreter fallback (shim_impl.c), a refused
            // block kills the guest thread outright. Arena sizing is not a
            // comfort tuning — it decides whether the guest survives.
            //
            // So the allocator is probed with a small block first; the base
            // it returns gives the EXACT alignment slack needed, and the
            // real request is need+that-slack. Falling back to need+16 MiB
            // (the old fixed calculation) remains the last resort.
            const uint64_t need = (sz > 0x1000000ull) ? sz - 0x1000000ull : sz;
            void* blk = MAP_FAILED;
            {
                uint64_t slack = 0x1000000ull;          // default = old fixed calculation
                void* probe = mmap(nullptr, 0x100000, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
                if (probe != MAP_FAILED) {
                    const uintptr_t pb = (uintptr_t)probe;
                    slack = (uint64_t)((((pb + 0xFFFFFFu) & ~(uintptr_t)0xFFFFFFu) - pb));
                    munmap(probe, 0x100000);
                }
                for (int att = 0; att < 2 && blk == MAP_FAILED; ++att) {
                    const uint64_t try_sz = need + slack;
                    void* t = mmap(nullptr, try_sz, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
                    if (t == MAP_FAILED) { slack = 0x1000000ull; continue; }
                    const uintptr_t tb = (uintptr_t)t;
                    const uintptr_t mb = (tb + 0xFFFFFFu) & ~(uintptr_t)0xFFFFFFu;
                    if ((uint64_t)(tb + try_sz - mb) >= need) { blk = t; sz = try_sz; }
                    else { munmap(t, try_sz); slack = 0x1000000ull; }
                }
            }
            if (blk == MAP_FAILED)      // last resort: exactly the old fixed calculation
                blk = mmap(nullptr, sz, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
            if (blk == MAP_FAILED) { fprintf(stderr, "[cpu_box86] D2ARENA mmap %llx failed\n",
                                             (unsigned long long)sz);
                { char m[112];
                    snprintf(m, sizeof m, "FATAL: arena alloc failed (%llu MB) — see the mmap FAIL line above",
                             (unsigned long long)(sz >> 20));
                    wx86_vita_progress_c(m); }
                _exit(2); }
            // membase must have zero low 24 bits (single-ADD imm8-ror-8). mmap is
            // page-aligned; round the *guest→host delta* up to 16 MiB and rely on
            // NORESERVE slack (the arena is oversized by one granule for this).
            uintptr_t base = (uintptr_t)blk;
            g_mb = (base + 0xFFFFFFu) & ~(uintptr_t)0xFFFFFFu;
            g_arena = true;
            // With a single block, H(va)=va+membase NEVER faults — a wild
            // guest pointer silently aliases whatever host object sits there
            // (the JIT cache, the newlib heap), so corruption surfaces far
            // from its cause. Record the span so shim-side derefs can be
            // range-checked (see g_arena_span / arena_check below).
            g_arena_span = (uint32_t)((base + sz > g_mb) ? (base + sz - g_mb) : 0);
            // Logs the alignment waste directly: the probe above only
            // reduces it, so this is the only way to see how much survives
            // on a given run.
            { char m[152];
                snprintf(m, sizeof m, "arene: base=%p membase=%p span=%u Mo reserve=%llu Mo perdu-alignement=%u Ko",
                         (void*)base, (void*)g_mb, (unsigned)(g_arena_span>>20),
                         (unsigned long long)(sz>>20), (unsigned)((g_mb-base)>>10));
                wx86_vita_progress_c(m); }
            dyn86_set_membase(g_mb);
            // Same span for the memcpy/memset intrinsics guard
            // (dyn86_memintrin.h): the native helper writes to guest memory
            // without going through write()/arena_check, so it must carry
            // the SAME bound, or a wild pointer would alias the host heap.
            dyn86_mi_set_span(g_arena_span);
            fprintf(stderr, "[cpu_box86] arena: block=%p size=0x%llx membase=0x%lx (Vita single-block model)\n",
                    blk, (unsigned long long)sz, (unsigned long)g_mb);
        } else if (const char* mbs = getenv("WX86_MEMBASE") ? getenv("WX86_MEMBASE")
                                                            : getenv("D2MEMBASE")) {
            g_mb = strtoul(mbs, nullptr, 16);
            if (g_mb & 0xFFFFFFu) { fprintf(stderr, "[cpu_box86] D2MEMBASE low 24 bits must be 0\n"); _exit(2); }
            dyn86_set_membase(g_mb);
            fprintf(stderr, "[cpu_box86] fastmmu membase=0x%08x\n", (unsigned)g_mb);
        }
        g_self = this;
        std::memset(&ctx_, 0, sizeof ctx_);
        pthread_mutex_init(&ctx_.mutex_dyndump, nullptr);
        pthread_mutex_init(&ctx_.mutex_lock, nullptr);
        my_context = &ctx_;
        init_custommem_helper(&ctx_);
        dyn86_init();                  // capture the pristine jump-table default
        std::memset(&emu_, 0, sizeof emu_);
        emu_.context = &ctx_;
        dynarec86_setup_emu_helpers(&emu_);
        emu_.df = d_none;
        dyn86_diag_emu = &emu_;
#ifndef __vita__
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = diag_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
#endif
    }

    // Current-emu accessor: the TLS binding if a native runner bound one,
    // else the base emu — every register/run/save path below goes through it.
    // D2_TLSCOUNT: measurement-only build (never shipped, never the
    // default). Counts t_emu resolutions, i.e. EXACTLY what becomes a
    // __emutls_get_address call on Vita (vitasdk toolchain --disable-tls).
    // A call to reg() or set_reg() counts as exactly ONE (its 3 static call
    // sites are ALTERNATIVE paths: i<=7, i==8, i==9), so this counter is a
    // count of emutls calls, not an estimate. It's a
    // LOWER BOUND on the per-trap total: bridge.cpp's t_* (t_redirect_eip,
    // t_yield_pending) and sched_native's t_cur add to it and are not
    // counted here.
    // ---- The shortcut that removes the emutls chain under the cooperative backend ----
    //
    // Why this exists: under the cooperative backend (the shipped default),
    // E() is called several times per Bridge trap, and each call resolves
    // t_emu. On Vita, the toolchain's TLS chain (--disable-tls) is
    // __emutls_get_address -> pthread_getspecific -> pte_osTlsGetValue ->
    // sceKernelGetTLSAddr — an inter-module call, not a cheap TLS read.
    //
    // THE INVARIANT THAT MAKES THE SHORTCUT SAFE, proven in three steps:
    //   1. t_emu is only ever made non-null by thread_emu_bind(emu != 0);
    //   2. the ONLY caller that binds a non-null emu is
    //      NativeScheduler::runner (sched_native.cpp:501) — main binds
    //      nullptr (:902);
    //   3. a per-thread emu can only come from thread_emu_create(), and the
    //      only call site outside NativeScheduler is the probe at
    //      rt_boot.cpp:2744, which is INSIDE the D2SCHED=native branch.
    // Therefore: as long as thread_emu_create() has never been called,
    // t_emu is nullptr on ALL threads, and `t_emu ? *t_emu : emu_` equals
    // emu_ by construction. The shortcut doesn't choose a different value:
    // it avoids ASKING for a value whose answer is already known.
    //
    // Cost of the shortcut: one relaxed global load and a branch, far
    // cheaper than the TLS chain it replaces. Under the native backend,
    // nothing changes — the flag is true and the old path runs unchanged.
    //
    // The invariant isn't just asserted: thread_emu_bind VERIFIES it (below),
    // on a cold path, and aborts loudly if it's ever violated.
    x86emu_t& E() {
        D2_ECALL();
        if (!g_multi_emu.load(std::memory_order_relaxed)) return emu_;
        D2_TLSCOUNT_HIT(); return t_emu ? *t_emu : emu_;
    }
    const x86emu_t& E() const {
        D2_ECALL();
        if (!g_multi_emu.load(std::memory_order_relaxed)) return emu_;
        D2_TLSCOUNT_HIT(); return t_emu ? *t_emu : emu_;
    }

    bool map(uint32_t va, uint32_t size, const void* init, int prot) override {
        uint32_t a = va & ~0xFFFu;
        uint32_t end = (va + size + 0xFFFu) & ~0xFFFu;
        // Arena mode: the single block already backs [0, arena) — no per-region
        // mmap (exactly how a Vita VM memblock works: one allocation, addressed
        // by offset). Otherwise host-mmap this region RWX at H(a).
        if (!g_arena) {
            void* p = mmap(H(a), end - a, PROT_READ|PROT_WRITE|PROT_EXEC,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED || p != H(a)) {
                fprintf(stderr, "[DEBUG map] va=0x%x size=%u a=0x%x end=0x%x H(a)=%p errno=%d(%s) p=%p\n",
                        va, size, a, end, H(a), errno, strerror(errno), p);
                return false;
            }
        }
        if (init) std::memcpy(H(va), init, size);
        setProtection(a, end - a, host_prot(prot));    // custommem bookkeeping
        return true;
    }
    bool protect(uint32_t va, uint32_t size, int prot) override {
        updateProtection(va, size, host_prot(prot));   // keeps PROT_DYNAREC bits
        return true;
    }
    bool read(uint32_t va, void* dst, uint32_t n) override {
        // D2_MEMGUARD: a shim handed a bad guest pointer would fault __memcpy_neon
        // deep in the host (uncatchable box86 SIGSEGV). Under the guard, verify the
        // page is mapped first; if not, log the address and hand back zeros so the
        // run survives and the culprit address is visible instead of a core dump.
        // custommem's protection map is keyed by GUEST address (setProtection in
        // map(), and the dynarec's getProtection/protectDB all use guest VAs), so
        // query `va` — NOT H(va). Under membase!=0 (Vita), keying on H(va)
        // instead would find nothing and make every guarded read return zeros.
        if (mem_guard() && getProtection((uintptr_t)va) == 0) {
            static int rn = 0;
            if (rn++ < 64) std::fprintf(stderr,
                "  [box86 guard] shim read of UNMAPPED guest 0x%08x (n=%u) -> zeros\n", va, n);
            std::memset(dst, 0, n);
            return false;
        }
        if (!arena_check(va, n, "read")) { std::memset(dst, 0, n); return false; }
        std::memcpy(dst, (const void*)H(va), n);
        return true;
    }
    bool write(uint32_t va, const void* src, uint32_t n) override {
        // A translated page is host-write-protected (protectDB); lift it and
        // mark the affected dynablocks dirty before the host-side write.
        if (!arena_check(va, n, "write")) return false;
        if (isprotectedDB(va, n)) unprotectDB(va, n, 1);
        std::memcpy(H(va), src, n);
        return true;
    }
    // Honest dynarec invalidation for FlushInstructionCache / VirtualProtect:
    // mark any translated block in the range needtest, exactly like the write()
    // barrier does. Re-hash on next entry -> re-translate if the bytes changed.
    void invalidate_code(uint32_t va, uint32_t size) override {
        if (size == 0) size = 1;
        if (isprotectedDB(va, size)) unprotectDB(va, size, 1);
    }
    // See runtime/cpu.h: unconditional DESTRUCTION, not a marking that's
    // conditioned on isprotectedDB. cleanDBFromAddressRange is keyed by
    // GUEST ADDRESS (see dynarec/dynarec_arm_functions.c:399).
    void discard_code(uint32_t va, uint32_t size) override {
        if (size == 0) return;
        cleanDBFromAddressRange((uintptr_t)va, (size_t)size, 1 /* destroy */);
    }
    // Callers use hostptr for bulk shim I/O (ReadFile, gzero, present) — a
    // wild VA there would memcpy straight into a host object. Null makes every
    // caller fall back to read()/write(), which are themselves range-checked.
    void* hostptr(uint32_t va, uint32_t n = 1) override {
        if (!arena_check(va, n, "hostptr")) return nullptr;
        return H(va); }
    // custommem tracks every map()ed region via setProtection — zero means no
    // guest region covers this address (a raw garbage pointer from the guest).
    bool mapped(uint32_t va) override { return getProtection((uintptr_t)va) != 0; }   // guest-keyed
    const uint32_t* ip_ptr() const override { return &E().ip.dword[0]; }

    uint32_t reg(int r) override {
        if (r >= R_EAX && r <= R_EDI) return E().regs[r].dword[0];
        if (r == R_EIP) return E().ip.dword[0];
        if (r == R_EFLAGS) {
            UpdateFlags(&E());         // materialize Box86's deferred flags
            return E().eflags.x32;
        }
        return 0;
    }
    void set_reg(int r, uint32_t v) override {
        if (r >= R_EAX && r <= R_EDI) { E().regs[r].dword[0] = v; return; }
        if (r == R_EIP) { E().ip.dword[0] = v; return; }
        if (r == R_EFLAGS) {
            E().eflags.x32 = v;
            E().df = d_none;           // the value is now authoritative
        }
    }

    // Batched overrides (see cpu.h): ONE emu resolution instead of five.
    // R_ESP falls within the general-register range (cpu.h, enum Reg), so
    // e.regs[R_ESP] is the same slot that set_reg(R_ESP, ...) writes.
    // read_u32 resolves nothing itself: it goes through read()/hostptr.
    // Batched read of the eight general registers (see cpu.h): ONE emu
    // resolution.
    void regs_gp(uint32_t* out) override {
        const x86emu_t& e = E();
        for (int r = R_EAX; r <= R_EDI; ++r) out[r] = e.regs[r].dword[0];
    }
    uint32_t trap_retaddr() override {
        return read_u32(E().regs[R_ESP].dword[0]);
    }
    void trap_epilogue(uint32_t eax, uint32_t esp_add, uint32_t eip) override {
        x86emu_t& e = E();
        e.regs[R_EAX].dword[0]  = eax;
        e.regs[R_ESP].dword[0] += esp_add;
        e.ip.dword[0]           = eip;
    }
    // B5: stdcall trap with argc=0 — ONE emu resolution for both the
    // return-address read AND the three writes, instead of two (one per
    // trap_retaddr(), one per trap_epilogue()). read_u32 resolves nothing
    // itself: it goes through read()/hostptr, never E().
    void trap_ret0(uint32_t eax) override {
        x86emu_t& e = E();
        const uint32_t esp = e.regs[R_ESP].dword[0];
        const uint32_t ret = read_u32(esp);
        e.regs[R_EAX].dword[0] = eax;
        e.regs[R_ESP].dword[0] = esp + 4;
        e.ip.dword[0]          = ret;
    }

    void set_fs_base(uint32_t base) override {
        E().segs_offs[_FS] = base;
        E().segs_serial[_FS] = 1;      // emitted FS path: serial!=0 -> use offs
    }
    bool fs_base_is_direct() const override { return true; }

    // Scheduler slice budget. Unicorn counts native instructions; here the
    // unit is BLOCK ENTRIES — every translated block's emitted prologue
    // decrements emu->dyn86_budget (recharged in run()) and exits resumable
    // when it expires. Same ~8 guest insns/block scale as the old LinkNext
    // chains, but blocks stay DIRECT-LINKED (no arm_next round-trip per
    // transition). Residual limitation: a loop inside ONE dynablock never
    // re-enters the prologue and cannot be preempted this way.
    void set_run_limit(uint64_t n) override {
        limit_blocks_ = n ? (uint32_t)((n / 8) ? (n / 8) : 1) : 0;
    }
    // True for both break causes (chain-quantum expiry AND request_stop) —
    // deliberately mirroring CpuUnicorn, which also reports limit_hit_ for a
    // request_stop() that lands while a budget is set. The scheduler treats
    // both identically: slice over, thread still Ready.
    bool take_limit_hit() override { int p = t_preempt; t_preempt = 0; return p != 0; }

    // Warden-safe per-slot fast path (see docs/perf, Tier 1). This check runs
    // on EVERY guest->host crossing (thousands per frame), not just the few
    // that hit — so a miss must be cheap.
    // Open-ADDRESSED hash table (linear probing), frozen after warm-up: one
    // read in the common case.
    //   * key = the slot's VA; 0 = empty slot. A trap VA is NEVER 0 (the
    //     bridge allocates them from trap_base_, always > 0), so 0 is a safe
    //     sentinel.
    //   * hash = va >> 4: the bridge spaces slots 16 bytes apart, so this
    //     division yields CONSECUTIVE indices — the best possible spread, no
    //     clustering.
    //   * the table is sized at 4x the max number of intrinsics (16), so at
    //     most 25% full: probing stops at an empty slot within one or two
    //     steps, and CANNOT loop forever (there's always a gap).
    // Semantics unchanged: same set of slots, same function called.
    static constexpr uint32_t kIntrinMax  = 16;
    static constexpr uint32_t kIntrinMask = 63;   // 64-slot table (>= 4x kIntrinMax)
    // ---- B5: the DIRECT INDEX (D2_B5INDEX=1), and why it saves work --------
    // The linear probe above pays, on EVERY guest->host trap, a read of
    // ikey_[] — a cache line — for the sole purpose of discovering that the
    // slot is NOT an intrinsic, which is true for the overwhelming majority
    // of them.
    // The bridge allocates slots every 16 bytes starting at trap_base_, so
    // the set of registered intrinsics occupies a single CONTIGUOUS VA
    // window (narrow: two slots today). An unsigned subtraction and one
    // comparison test membership with NO MEMORY ACCESS AT ALL: a slot below
    // idir_lo_ wraps to a huge integer and is rejected by that same
    // comparison. Inside the window, the index is exact: no more probing, no
    // more key comparison, and a SINGLE table instead of two (ikey_ and ifn_
    // were two separate cache lines on a hit).
    // If the window exceeded kDirectMax slots, the index is NOT armed and the
    // probe path takes over: idir_n_ == 0 rejects everything (see
    // rebuild_direct).
    static constexpr uint32_t kDirectMax = 256;   // 256 slots = 4 KiB of VA

    inline bool try_intrinsic(uint32_t slot) {
        if (no_intrinsics_) return false;
#ifdef D2_B5CENSUS
        ++d2rt_b5_calls;
        const unsigned long long tls0 = d2_tls_hits;
#endif
        bool served = false;
        if (g_b5index) {
            const uint32_t d = (slot - idir_lo_) >> 4;   // unsigned: below lo wraps to huge
            if (d < idir_n_) {
#ifdef D2_B5CENSUS
                ++d2rt_b5_loads;
#endif
                if (IntrinsicFn fn = idir_[d]) {
                    // SAME ACCOUNTING AS THE LEGACY PATH (do not remove): a
                    // slot serviced here never goes back through
                    // Bridge::trap_handler, so this is the only place it can
                    // be counted; if fn returns false, the trap falls
                    // through to the bridge, which counts it there instead.
                    served = fn(*this, slot);
                    if (served) d2rt::trapcnt::bump(slot);
                }
            }
        } else {
            uint32_t i = (slot >> 4) & kIntrinMask;
            for (;;) {
#ifdef D2_B5CENSUS
                ++d2rt_b5_loads;
#endif
                uint32_t k = ikey_[i];
                if (k == slot) {
                    // Count ONLY when the intrinsic actually served the
                    // call: a slot serviced here never goes back through
                    // Bridge::trap_handler, so this is the only place it can
                    // be counted; if fn returns false, the trap falls
                    // through to the bridge, which counts it there instead
                    // (otherwise it would be counted twice).
                    served = ifn_[i](*this, slot);
                    if (served) d2rt::trapcnt::bump(slot);
                    break;
                }
                if (!k) break;                     // empty slot => not present
                i = (i + 1) & kIntrinMask;
            }
        }
#ifdef D2_B5CENSUS
        if (served) {
            for (uint32_t x = 1; x < g_b5scale; ++x) {     // scale test
                ++d2rt_b5_loads;
                d2rt_b5_sink += ikey_[(slot >> 4) & kIntrinMask];
                d2rt_b5_sink += E().regs[R_ESP].dword[0];  // one resolution per iteration
            }
            ++d2rt_b5_hits; d2rt_b5_emutls += d2_tls_hits - tls0;
        }
#endif
        return served;
    }
    void set_intrinsic(uint32_t va, IntrinsicFn fn) override {
        if (!va) return;                           // 0 is the "empty slot" sentinel
        uint32_t i = (va >> 4) & kIntrinMask;
        for (uint32_t probe = 0; probe <= kIntrinMask; ++probe, i = (i + 1) & kIntrinMask) {
            if (ikey_[i] == va) { ifn_[i] = fn; rebuild_direct(); return; }   // replace existing
            if (!ikey_[i]) {
                if (intrin_n_ >= (int)kIntrinMax) return;        // respects the kIntrinMax cap
                ikey_[i] = va; ifn_[i] = fn; ++intrin_n_; rebuild_direct(); return;
            }
        }
    }
    // Rebuilds the direct index from the hash table — a COLD path (a couple
    // of calls at startup). The hash table remains the source of truth: the
    // index is only a projection of it, so both paths serve EXACTLY the same
    // set of slots and the same function.
    void rebuild_direct() {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i <= kIntrinMask; ++i)
            if (ikey_[i]) { if (ikey_[i] < lo) lo = ikey_[i]; if (ikey_[i] > hi) hi = ikey_[i]; }
        idir_n_ = 0; d2rt_b5_direct_n = 0;
        if (!hi) return;
        const uint32_t span = ((hi - lo) >> 4) + 1;
        if (span > kDirectMax) return;             // window too wide: index NOT armed
        idir_lo_ = lo;
        for (uint32_t d = 0; d < span; ++d) idir_[d] = nullptr;
        for (uint32_t i = 0; i <= kIntrinMask; ++i)
            if (ikey_[i]) idir_[(ikey_[i] - lo) >> 4] = ifn_[i];
        idir_n_ = span;
        d2rt_b5_direct_n = span; d2rt_b5_direct_lo = lo;
    }
    void set_intrinsics_enabled(bool on) override { no_intrinsics_ = !on; }

    void set_alternate(uint32_t from, uint32_t to) override { dyn86_set_alternate(from, to); }
    void set_trap(uint32_t lo, uint32_t hi, TrapFn fn) override {
        trap_lo_ = lo; trap_hi_ = hi; trap_fn_ = std::move(fn);
        if (!trap_mapped_) {
            if (!g_arena) {
                void* p = mmap(H(lo), hi - lo, PROT_READ|PROT_WRITE|PROT_EXEC,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
                if (p == MAP_FAILED || p != H(lo)) return;
            }
            // one Box86 exit stub (CC 'S' 'C' 00 00 00 00) per 16-byte slot —
            // the Bridge allocates trap VAs 16 bytes apart (sentinel included)
            for (uint32_t s = lo; s < hi; s += 16) {
                uint8_t* b = (uint8_t*)H(s);
                b[0] = 0xCC; b[1] = 'S'; b[2] = 'C';
            }
            setProtection(lo, hi - lo, PROT_READ|PROT_EXEC);
            trap_mapped_ = true;
        }
    }

    void request_stop() override {
        // Zero EVERY emu's budget: the NEXT block entry's prologue exits
        // resumable at that block's start. dyn86_request_stop keeps the flag
        // for LinkNext (not-yet-translated targets). Native shutdown must
        // reach all runners AND the base/main emu, whichever host thread
        // calls this; `quit` also covers "not currently running".
        // Remaining limitation: a loop inside ONE dynablock can't be stopped.
        dyn86_request_stop();
        // Account for the current slice BEFORE zeroing the budget — same
        // discipline as nudge_thread_budget below, and for the same reason,
        // but it was missing here. Without it, zeroing the budget makes
        // thread_emu_blocks()'s `b > 0` guard false, so any reader AFTER
        // shutdown sees only dyn86_blocks, missing the current slice's
        // delta. This has no visible effect under the cooperative backend
        // (slices are short and already accumulated); under NATIVE the
        // budget is 0x7FFFFFFF and never expires on its own, so the current
        // slice IS the entire work the thread has done since it last woke
        // up — omitting it here would silently understate the thread's true
        // block count after shutdown.
        save_slice(&emu_);
        emu_.dyn86_budget = 0; emu_.quit = 1;
        for (x86emu_t* e : emus_) { save_slice(e); e->dyn86_budget = 0; e->quit = 1; }
    }

    // Saves the current slice into the cumulative total, and DISARMS it so
    // it isn't counted twice. Factored out of nudge_thread_budget, which
    // establishes and documents the ordering.
    // LOAD-BEARING ORDER: (1) disarm, which cancels the delta an observer
    // would read, THEN (2) accumulate. A read that lands between the two
    // UNDER-estimates by one slice; the reverse order would show a jump,
    // misread as "this thread is racing ahead" — the wrong sense. `volatile`:
    // this order must survive the optimizer.
    static void save_slice(x86emu_t* e) {
        volatile int*      pa = &e->dyn86_armed;
        volatile uint32_t* pn = &e->dyn86_blocks;
        const int b = __atomic_load_n(&e->dyn86_budget, __ATOMIC_RELAXED);
        const int a = *pa;
        *pa = 0;                                                     // (1)
        if (b > 0 && a >= b) *pn = *pn + (uint32_t)(a - b);          // (2)
    }

    // Anti-starvation valve: mirrors the budget-zeroing in request_stop
    // above, but WITHOUT quit or g_stop (those are monotonic, shutdown-only
    // — see the contract in cpu.h). Relaxed atomic store: the block prologue
    // does a load-decrement-store on dyn86_budget; a zero written between
    // its load and its store is lost — benign, caught at the next
    // starvation-detection pass. Caller must hold the GIL (emus_ registry is
    // append-only).
    void nudge_thread_budget(void* emu) override {
        x86emu_t* e = emu ? (x86emu_t*)emu : &emu_;   // nullptr = base emu (main)
        // Accounts for the slice BEFORE breaking it (per-thread liveness,
        // cpu.h): the poke destroys the information "how many blocks has
        // this thread executed", so it must be saved here, and nowhere else.
        // ORDER: (1) armed <- current budget, which cancels the delta an
        // observer would read, (2) accumulate, (3) the poke itself. A read
        // that lands between (1) and (2) UNDER-estimates by one slice; no
        // ordering here can produce a +2^31 jump, which would misread as
        // "this thread is racing ahead" — the wrong sense.
        // (same `volatile` accesses, same reason as in run()'s ordering:
        // disarm -> accumulate -> poke must survive the optimizer.)
        save_slice(e);                                               // (1) disarm + (2) save
        __atomic_store_n(&e->dyn86_budget, 0, __ATOMIC_RELAXED);     // (3) the poke itself
    }

    // ---- Native-scheduler support: one x86emu_t per guest thread --------
    void* thread_emu_create() override {
        // Arms the TLS path of E() (full argument above E()). Set HERE and
        // not in thread_emu_bind: creation NECESSARILY precedes binding, so
        // the flag is true before any thread can observe a non-null t_emu.
        // Under the cooperative backend this function is never called — the
        // probe at rt_boot.cpp:2744 is inside the D2SCHED=native branch — so
        // the flag stays false for the whole run.
        g_multi_emu.store(true, std::memory_order_relaxed);
        x86emu_t* e = new x86emu_t();
        std::memset(e, 0, sizeof *e);
        e->context = &ctx_;
        dynarec86_setup_emu_helpers(e);
        e->df = d_none;
        // FPU: inherit the BASE emu's current state — matches the
        // cooperative backend's first-slice "inherit" semantics. Never a
        // zero blob.
        X87Blob b; blob_from_emu(b, emu_); blob_to_emu(*e, b);
        emus_.push_back(e);            // caller holds the GIL (native ctor path)
        return e;
    }
    void thread_emu_bind(void* e) override {
        // GUARDS the invariant in E(): binding a NON-NULL emu while the flag
        // is false would mean a per-thread emu appeared without going
        // through thread_emu_create — and E() would then hand the BASE emu
        // to a thread that has its own, i.e. silent, total corruption. This
        // is a COLD path (once per guest thread), so the guard is free; and
        // it ABORTS LOUDLY instead of drifting silently.
        if (e && !g_multi_emu.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "FATAL: thread_emu_bind(non nul) sans thread_emu_create — invariant E() rompu\n");
            wx86_vita_progress_c("FATAL: invariant E() rompu (bind sans create)");
            std::abort();
        }
        t_emu = (x86emu_t*)e;
    }
    const uint32_t* thread_emu_ip(void* e) override {
        return e ? &((x86emu_t*)e)->ip.dword[0] : &emu_.ip.dword[0];
    }
    // Per-thread liveness (see cpu.h): translated blocks executed since
    // boot, MONOTONIC. Derived from the block budget (decremented by every
    // translated block's prologue): zero cost on the hot path, and it moves
    // even when the thread crosses no trap — which is what distinguishes a
    // spin in translated code from a genuinely blocked thread. nullptr =
    // base emu (main).
    // `volatile`: the writers are generated code plus a relaxed atomic
    // store; the formal race disappears for zero extra instructions.
    bool thread_emu_filstat(void* e, uint32_t* traps, uint32_t* cont, uint32_t* wait_us) override {
        if (!g_filstat) return false;                 // field ABSENT without the knob
        const x86emu_t* em = e ? (const x86emu_t*)e : &emu_;
        const volatile uint32_t* pt = &em->dyn86_traps;
        const volatile uint32_t* pc = &em->dyn86_gilcont;
        const volatile uint32_t* pw = &em->dyn86_gilwait_us;
        if (traps) *traps = *pt; if (cont) *cont = *pc; if (wait_us) *wait_us = *pw;
        return true;
    }
    bool thread_emu_blocks(void* e, uint32_t* out) override {
        x86emu_t* em = e ? (x86emu_t*)e : &emu_;
        const volatile uint32_t* pn = &em->dyn86_blocks;
        const volatile int*      pa = &em->dyn86_armed;
        const volatile int*      pb = &em->dyn86_budget;
        // LOAD-BEARING READ ORDER (cpu.h): total, THEN armed, THEN budget.
        // Paired with "disarm before accumulating" on the writer side, this
        // forbids double-counting a slice, and so forbids any forward jump.
        uint32_t n = *pn; int a = *pa; int b = *pb;
        // budget <= 0: slice exhausted or thread poked — the delta is already in n.
        if (out) *out = (b > 0 && a >= b) ? n + (uint32_t)(a - b) : n;
        return true;
    }

    // NATIVE NOTE: called OUTSIDE gil::Guard, with single-writer counters —
    // safe under the cooperative backend only; unreachable under native
    // (budget is armed infinite, bbreak never fires). Don't enable
    // D2_EIPPROF+native without rethinking this.
    // D2_EIPPROF: CPU-time sampler. IMPORTANT — sampling happens ONLY on
    // block-budget expiry, i.e. when the thread has actually consumed its
    // slice computing. Sampling on resume instead heavily over-represents
    // code that yields often, such as a message/wait loop that burns no CPU.
    void eipprof_sample(uint32_t ip) {
        uint32_t rva = ip - g_eipProfBase;
        ++g_eipProfBuckets[wx86_prof_family(g_profMap, rva)];
        wx86_prof_zooms(rva);
        // FULL-RANGE histogram: 96 buckets of 32 KiB covering the whole
        // .text (0..0x300000). The named zones above cover only a fraction
        // of real time — this scans without assumptions.
        if ((rva >> 15) < 96) ++d2rt_eipprof_all[rva >> 15];
        { uint32_t h = (ip * 2654435761u) >> 19;           // 13 bits -> 8192 cases
          for (int p = 0; p < 24; ++p) {
              uint32_t s = (h + p) & 8191u;
              if (!d2rt_eipprof_hit[s]) { d2rt_eipprof_key[s] = ip; d2rt_eipprof_hit[s] = 1; break; }
              if (d2rt_eipprof_key[s] == ip) { ++d2rt_eipprof_hit[s]; break; }
          } }
    }

    // ---- D2_TIMEPROF: sampling UNIFORM IN TIME ------------------------------
    // Called at the same point as eipprof_sample (budget expiry), the only
    // moment emu->ip is current: between epilogues, EIP lives in r14 and
    // memory is stale.
    //
    // SERVO. The real interval is measured and the next step's budget is
    // corrected to target `d2rt_timeprof_on` microseconds. The correction
    // factor is bounded to [1/4, 4] per step, so it converges within a few
    // steps without oscillating when a scene's per-block cost changes
    // abruptly.
    //
    // SUSPENDED INTERVALS. Under D2SCHED=native the thread can be preempted
    // from the core between two expiries: the measured interval then
    // includes time this thread was NOT computing, and attributing it to the
    // sampled address would be a lie. Beyond 8x the target, the sample is
    // REJECTED and counted separately (`suspended=`). An instrument that
    // hides what it discarded proves nothing.
    void timeprof_sample(uint32_t ip, int consumed) {
        const uint64_t now = dyn86_jp_now_us();
        const uint64_t target = d2rt_timeprof_on;
        if (!tp_last_us) { tp_last_us = now; tp_budget = 2048; return; }
        const uint64_t dt = now - tp_last_us;
        tp_last_us = now;
        if (!dt) return;                      // below clock resolution

        // Correction du budget : viser `target` us par intervalle.
        {   int nb = tp_budget;
            if (dt * 4 < target)      nb = tp_budget * 4;
            else if (dt * 2 < target) nb = tp_budget * 2;
            else if (dt > target * 4) nb = tp_budget / 4;
            else if (dt > target * 2) nb = tp_budget / 2;
            if (nb < 32)      nb = 32;
            if (nb > 1000000) nb = 1000000;
            tp_budget = nb; }

        if (dt > target * 8) { ++d2rt_tp_susp; return; }   // thread was suspended

        ++d2rt_tp_samples;
        d2rt_tp_us += dt;
        d2rt_tp_blocks += (uint64_t)(consumed > 0 ? consumed : 0);

        const uint32_t rva = ip - d2rt_timeprof_base;
        ++d2rt_tp_bucket[wx86_prof_family(g_profMap, rva)];
        if(d2rt_lag_on) {                 // stall-attribution ring
            const uint32_t w = d2rt_lag_w;
            d2rt_lag_t[w % D2RT_LAG_RING] = now;
            d2rt_lag_ip[w % D2RT_LAG_RING] = ip;
            d2rt_lag_w = w + 1;
        }
        { uint32_t h = (ip * 2654435761u) >> 19;
          for (int p = 0; p < 24; ++p) {
              uint32_t s = (h + p) & 8191u;
              if (!d2rt_tp_hit[s]) { d2rt_tp_key[s] = ip; d2rt_tp_hit[s] = 1; break; }
              if (d2rt_tp_key[s] == ip) { ++d2rt_tp_hit[s]; break; }
          } }
    }

    bool run(uint32_t eip, const char** fault) override {
        dyn86_slice_begin();           // clear stop flag + break marker (native
                                       // mode: dyn86_set_native keeps the stop)
        // ---- The thread's emu, resolved ONCE for the whole slice ------------
        // WHY: under the native backend, each access through the accessor is
        // a __emutls_get_address -> pthread_getspecific -> pte_osTlsGetValue
        // -> sceKernelGetTLSAddr chain — an inter-module call, not a cheap
        // TLS read. Disassembly of the shipped binary counted FOURTEEN such
        // calls in run()'s body alone, SIX of them per loop iteration — so
        // six per GIL acquisition, since the Guard further down is taken
        // INSIDE the loop.
        //
        // WHY THIS IS CORRECT, and not just a cache: t_emu is written only by
        // thread_emu_bind, which has exactly TWO callers (sched_native.cpp
        // l.501 and l.902), both BEFORE run_guest — i.e. before any run() on
        // this host thread. Under the cooperative backend the accessor
        // returns emu_, a member. In both cases it names the SAME object for
        // the entire duration of this run() activation: this doesn't cache a
        // value, it names a reference.
        //
        // WHAT THE REFERENCE DOES NOT CHANGE: e.quit and e.dyn86_bbreak are
        // written by OTHER threads (request_stop). A reference only holds an
        // ADDRESS, never a copy of the content: every read still goes to
        // memory, exactly as before. And DynaRun remains an opaque external
        // call the compiler cannot see through (no -flto on this file,
        // build_rt_boot_vpk.sh l.94). Re-read this note if -flto is ever
        // enabled here.
        //
        // DEBT TO WATCH: if thread_emu_bind ever became callable from inside
        // a shim body, this hoist would become wrong AND SILENT. The guard
        // against that isn't an assert (it would only live under
        // -DD2_TLSCOUNT, never in the shipped binary): it's the NUMBER OF
        // CALLERS, to re-verify whenever thread_emu_bind is touched.
        // Pointer PINNED for the slice, or nullptr when the rollback flag is
        // set — every access then goes back through E(), i.e. through the
        // full emutls chain, exactly as before this optimization.
        x86emu_t* const pin_ = g_noEmuOpt ? nullptr : &E();
        auto EMU = [&]() -> x86emu_t& { return pin_ ? *pin_ : E(); };
        EMU().ip.dword[0] = eip;
        EMU().error = 0;                   // per-slice reset, per-EMU state: this
                                       // runner's fault must not ghost into a
                                       // later slice on the same emu
        // Recharge the block-entry preemption budget for this slice.
        // Per-thread liveness (cpu.h): the ending slice is accounted for
        // here, and `armed` remembers the recharged value — so the reader
        // never has to assume any 0x7FFFFFFF constant (which would become
        // wrong the day native arms a real limit via set_run_limit).
        // armed == 0 <=> slice already accounted for by nudge_thread_budget
        // (or this is the very first entry); budget <= 0 <=> the slice was
        // consumed to the end (preempted by budget expiry): it counts in
        // full.
        //
        // WRITE ORDER — this is what makes the field honest. The reader
        // (thread_emu_blocks) reads blocks, THEN armed, THEN budget, without
        // a lock. So this DISARMS BEFORE accumulating: while armed is 0, no
        // delta is added to the total a reader sees, so no reader can count
        // the just-finished slice TWICE. A reader caught in the window
        // under-estimates by one slice; it never sees a forward jump — a
        // jump would misread as "this thread is racing ahead," the wrong
        // sense (starvation reported where there was a stop). The reverse
        // order would make that jump possible, for a few instructions per
        // slice.
        // `volatile` access: forbids the compiler from reordering these four
        // writes among themselves. Zero cost (once per SLICE), and under
        // native all threads run on a single core (USER_0), so there's no
        // extra hardware reordering to cover.
        {   volatile int*      pa = &EMU().dyn86_armed;
            volatile uint32_t* pn = &EMU().dyn86_blocks;
            volatile int*      pb = &EMU().dyn86_budget;
            const int a = *pa, b = *pb;
            *pa = 0;                                                    // (1) no more delta to read
            if (a > 0) *pn = *pn + (uint32_t)(a - (b > 0 ? b : 0));     // (2) the slice is saved
            // D2_TIMEPROF: use the budget the servo computed for the
            // current thread, not the scheduling slice's.
            const int fresh = d2rt_timeprof_on ? (tp_budget ? tp_budget : 2048)
                            : (limit_blocks_ ? (int)limit_blocks_ : 0x7FFFFFFF);
            *pb = fresh;                                                // (3) this slice's budget
            *pa = fresh;                                                // (4) deltas resume
        }
        EMU().dyn86_bbreak = 0;
        for (;;) {
            EMU().quit = 0;
            const uint32_t start = EMU().ip.dword[0];
            DynaRun(&EMU());
            uint32_t ip = EMU().ip.dword[0];
            // Block-budget expiry: preempted, exactly resumable at EIP =
            // the start of the block that was about to run.
            if (EMU().dyn86_bbreak) { EMU().dyn86_bbreak = 0; t_preempt = 2;
                // NOT a time profile. The budget decrements on every BLOCK
                // ENTRY (prologue emitted), so sampling is uniform in block
                // entries, not time. A short, frequently-called function is
                // massively over-represented; a long loop that fits in a
                // single block is under-counted. Treat this profiler's
                // percentages as a rough entry-frequency ranking, not a cost
                // ranking — the two can disagree by an order of magnitude.
                if (g_eipProf) eipprof_sample(ip);   // block-entry-based profile
                if (d2rt_timeprof_on)                // time-based profile
                    timeprof_sample(ip, tp_budget ? tp_budget : 2048);
                return true; }
            // LinkNext seam break (request_stop seen at a not-yet-linked seam):
            // preempted but resumable at EIP = the pending chain target.
            if (int b = dyn86_take_break()) { t_preempt = b; return true; }
            if (EMU().error) {
                t_fault_addr = ip;
                t_fault_err = EMU().error;     // ERR_UNIMPL=1 / ERR_DIVBY0=2 / ERR_ILLEGAL=4
                // Name the JIT-pool death. An exhausted pool surfaces here as
                // ERR_UNIMPL, because a block that could not be translated
                // lands in shim_impl.c's Run() stub, which has no interpreter
                // to fall back to and just sets quit + ERR_UNIMPL. Reported as
                // the generic dynarec fault it is indistinguishable from a
                // genuinely unimplemented opcode -- and it is by far the more
                // common of the two in the field. dyn86_fill_fail tells them
                // apart: it counts translations that produced no block.
                t_fault = (t_fault_err == 1 && dyn86_fill_fail)
                        ? "box86 dynarec fault (piscine JIT saturee, aucun bloc traduit disponible)"
                        : "box86 dynarec fault (unimplemented/illegal/div0)";
                if (fault) *fault = t_fault;
                return false;
            }
            uint32_t slot = ip & ~0xFu;    // exit stub leaves EIP inside the slot
            if (slot >= trap_lo_ && slot < trap_hi_ && trap_fn_) {
                TrapGuard gg(EMU());       // native: serialize shims/intrinsics; equivalent to gil::Guard without D2_FILSTAT
                // No "t_fault_addr = slot" here. That used to be a write to a
                // __thread variable, i.e. a full emutls chain
                // (__emutls_get_address -> pthread_getspecific ->
                // pte_osTlsGetValue -> sceKernelGetTLSAddr, inter-module) ON
                // EVERY TRAP.
                // NOTHING IS LOST: every reader of fault_addr() is on a FAULT
                // path (sched_native.cpp:430/435/442/453,
                // sched_cooperative.cpp:310/314, rt_boot.cpp:7194), and the
                // only "return false" in run() is the fault branch above,
                // which sets t_fault_addr = ip itself. If a fault happens
                // INSIDE a shim, the next line has already published the
                // slot into e.ip, so cpu_->reg(R_EIP) — printed side by side
                // with faultAddr at each of those sites — carries the same
                // information.
                if (g_noEmuOpt) t_fault_addr = slot;   // per-trap write RESTORED (rollback path)
                EMU().ip.dword[0] = slot;      // consistent state, like CpuUnicorn
                if (try_intrinsic(slot)) continue;   // fast path: never enters the Bridge
                bool resume = trap_fn_(*this, slot);
                if (!resume) return true;  // sentinel: clean finish
                continue;                  // resume at handler-set EIP
            }
            // quit without error outside the trap window: clean stop
            return true;
        }
    }
    uint32_t fault_addr() const override { return t_fault_addr; }
    // Precise Windows exception code for the SEH dispatcher. UNIMPL is
    // an emulator gap, NOT a guest fault -> return 0 so the dispatcher skips it
    // (a __except "succeeding" on an emulator gap would mask it and diverge from
    // real hardware). NOTE: some genuine guest AVs arrive tagged ERR_ILLEGAL via
    // emit_signal(SIGSEGV) in generated code, so they surface as ILLEGAL_INSTRUCTION.
    uint32_t fault_code() const override {
        if (t_fault_err & 2) return 0xC0000094u;   // ERR_DIVBY0  -> INTEGER_DIVIDE_BY_ZERO
        if (t_fault_err & 4) return 0xC000001Du;   // ERR_ILLEGAL -> ILLEGAL_INSTRUCTION
        return 0;                                  // ERR_UNIMPL / unknown: terminate as before
    }

    // Per-thread FPU/SIMD context. One x86emu_t serves every guest thread, so
    // x87/MMX/XMM state must ride in the thread's X86Context across switches:
    // quantum preemption stops threads mid-computation where that state is
    // live (cooperative waits sit at Win32 call boundaries where the x87
    // stack is empty by ABI — the leak only bites on preemption, which is why
    // the virtual clock's fixed interleavings never tripped it and realclock
    // gameplay corrupted sprite decodes / switch dispatches).
    struct X87Blob {
        x87control_t cw; x87flags_t sw;
        mmx87_regs_t x87[8], mmx[8];
        uint32_t top; int fpu_stack; uint32_t fpu_tags;
        fpu_ld_t fpu_ld[8]; fpu_ll_t fpu_ll[8];
        sse_regs_t xmm[8]; mmxcontrol_t mxcsr;
    };
    static void blob_from_emu(X87Blob& b, const x86emu_t& e) {
        b.cw=e.cw; b.sw=e.sw;
        std::memcpy(b.x87,e.x87,sizeof b.x87); std::memcpy(b.mmx,e.mmx,sizeof b.mmx);
        b.top=e.top; b.fpu_stack=e.fpu_stack; b.fpu_tags=e.fpu_tags;
        std::memcpy(b.fpu_ld,e.fpu_ld,sizeof b.fpu_ld); std::memcpy(b.fpu_ll,e.fpu_ll,sizeof b.fpu_ll);
        std::memcpy(b.xmm,e.xmm,sizeof b.xmm); b.mxcsr=e.mxcsr;
    }
    static void blob_to_emu(x86emu_t& e, const X87Blob& b) {
        e.cw=b.cw; e.sw=b.sw;
        std::memcpy(e.x87,b.x87,sizeof b.x87); std::memcpy(e.mmx,b.mmx,sizeof b.mmx);
        e.top=b.top; e.fpu_stack=b.fpu_stack; e.fpu_tags=b.fpu_tags;
        std::memcpy(e.fpu_ld,b.fpu_ld,sizeof b.fpu_ld); std::memcpy(e.fpu_ll,b.fpu_ll,sizeof b.fpu_ll);
        std::memcpy(e.xmm,b.xmm,sizeof b.xmm); e.mxcsr=b.mxcsr;
    }
public:
    void save_context(X86Context& c) override {
        Cpu::save_context(c);
        static_assert(sizeof(X87Blob) <= sizeof(c.fpu), "X86Context::fpu too small");
        X87Blob b; blob_from_emu(b, E());
        std::memcpy(c.fpu, &b, sizeof b); c.fpu_valid = true;
    }
    void load_context(const X86Context& c) override {
        Cpu::load_context(c);
        // First slice of a never-run thread (fpu_valid=false): INHERIT the
        // current emu's FPU state — this is the long-validated semantics.
        // Loading an all-zero "pristine" blob here instead is a known
        // regression: cw=0 means x87 precision control = single and
        // tags != TAGS_EMPTY, so x87-heavy guest code (e.g. sprite decoding)
        // goes subtly wrong on every thread's first slice — including MAIN,
        // whose pre-scheduler CRT state (cw=0x27F) would get clobbered. A
        // correct power-on blob would use reset_fpu() semantics (cw=0x37F,
        // tags=TAGS_EMPTY, mxcsr=0x1F80) applied to WORKER threads only, and
        // must be validated on hardware before it ships; until then, inherit.
        if (c.fpu_valid) {
            X87Blob b; std::memcpy(&b, c.fpu, sizeof b);
            blob_to_emu(E(), b);
        }
    }

private:
    static int host_prot(int p) {
        int hp = 0;
        if (p & P_R) hp |= PROT_READ;
        if (p & P_W) hp |= PROT_WRITE;
        if (p & P_X) hp |= PROT_EXEC;
        return hp;
    }

    box86context_t ctx_{};
    x86emu_t emu_{};
    // Per-guest-thread emus created by thread_emu_create (native backend only;
    // empty under coop). Append-only; request_stop broadcasts to all of them.
    std::vector<x86emu_t*> emus_;
    uint32_t limit_blocks_ = 0;        // per-slice block budget (0 = unlimited)
    uint32_t trap_lo_ = 0, trap_hi_ = 0;
    bool trap_mapped_ = false;
    TrapFn trap_fn_;
    // Per-slot Warden-safe intrinsics (docs/perf, Tier 1): a tiny fixed table of
    // {trap VA -> inline handler}. Only a few ultra-hot trivial imports qualify.
    // Open addressing (see try_intrinsic): ikey_[i]==0 => empty slot.
    uint32_t    ikey_[kIntrinMask + 1] = {};
    IntrinsicFn ifn_ [kIntrinMask + 1] = {};
    int    intrin_n_ = 0;                          // occupancy (capped at kIntrinMax)
    // B5: DIRECT-INDEX projection of the table above (see rebuild_direct).
    // idir_n_ == 0 => index not armed: the `d < idir_n_` comparison rejects
    // everything, including before any registration.
    uint32_t    idir_lo_ = 0;
    uint32_t    idir_n_  = 0;
    IntrinsicFn idir_[kDirectMax] = {};
    bool   no_intrinsics_ = std::getenv("WX86_DISABLE_INTRINSICS") || std::getenv("D2_DISABLE_INTRINSICS");
    // Fault/preempt state lives in the t_* __thread vars (top of file): with N
    // native runners it is per-HOST-thread, never shared members.
};

CpuBox86* g_instance = nullptr;

} // namespace

Cpu* make_cpu_box86() {
    if (g_instance) return nullptr;    // Box86 core is single-instance (global my_context)
    g_instance = new CpuBox86();
    return g_instance;
}

} // namespace d2rt

#endif // __arm__
