// src/runtime/cpu.h — x86-32 CPU backend interface for the D2 runtime.
//
// Two backends implement this:
//   * CpuUnicorn  (desktop dev/test + differential oracle)   — cpu_unicorn.cpp
//   * CpuDynarec  (PS Vita ARMv7, the shipping engine)        — cpu_dynarec_arm.cpp (WIP)
//
// The bridge (bridge.h) is backend-agnostic: it registers a "trap" address
// range; whenever guest execution transfers control into that range, the
// backend halts, invokes Cpu::Trap, and resumes at whatever EIP the trap
// handler leaves in the register file. That single mechanism carries both
// the x86→native import calls and the "call finished" sentinel.

#pragma once
#include <cstdint>
#include <functional>

namespace d2rt {

// Subset of x86 register indices.
enum Reg {
    R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI, R_EIP, R_EFLAGS,
    R_COUNT
};

enum Prot { P_R = 1, P_W = 2, P_X = 4, P_RW = 3, P_RWX = 7 };

// Called when guest control enters the trap range. `trap_va` is the address
// jumped to. The handler mutates CPU state (via the Cpu&) — typically it
// reads args off the guest stack, performs the native call, writes EAX/EDX,
// and sets EIP to the guest return address. Returns:
//   true  → resume emulation at the (possibly new) EIP
//   false → stop the run entirely (used by the final-return sentinel)
struct Cpu;
using TrapFn = std::function<bool(Cpu& cpu, uint32_t trap_va)>;

struct Cpu {
    virtual ~Cpu() = default;

    // Memory. `init` may be null (zero-filled). Region must not overlap.
    virtual bool map(uint32_t va, uint32_t size, const void* init, int prot) = 0;
    virtual bool protect(uint32_t va, uint32_t size, int prot) = 0;
    virtual bool read(uint32_t va, void* dst, uint32_t n) = 0;
    virtual bool write(uint32_t va, const void* src, uint32_t n) = 0;
    // Direct host view of guest memory (Box86: va+membase; null if the backend
    // has no stable host mapping — callers MUST fall back to read()/write()).
    // For DATA buffers only: writing code through this bypasses the dynarec
    // dirty-page tracking (write() handles that; hostptr writes must never
    // touch guest bytes that may have been translated).
    // `n` = how many bytes the caller will touch: backends that can tell refuse
    // (null) a range escaping the guest arena, so bulk shim I/O can never memcpy
    // into a host object behind a wild guest pointer.
    virtual void* hostptr(uint32_t va, uint32_t n = 1) { (void)va; (void)n; return nullptr; }
    // Stable pointer to the guest EIP storage (diagnostics: the Vita watchdog
    // samples it racily to name where a hung guest is spinning). Null if the
    // backend has no stable register file.
    virtual const uint32_t* ip_ptr() const { return nullptr; }
    // Is `va` inside a map()ed guest region? Backends that can't tell return
    // true (their read/write already fail soft on unmapped addresses).
    virtual bool mapped(uint32_t va) { (void)va; return true; }

    // Invalidate any translated (dynarec) code covering [va, va+size): the next
    // execution re-hashes/re-translates from the current guest bytes. The honest
    // hook for FlushInstructionCache / VirtualProtect(PAGE_EXECUTE_*) so a
    // well-behaved self-patcher (Storm, a dynamically loaded module) never runs
    // a stale translation. Mirrors the write() barrier. No-op on backends
    // without a code cache. Same guest-VA keying as read()/write().
    virtual void invalidate_code(uint32_t va, uint32_t size) { (void)va; (void)size; }

    // Unconditionally DISCARDS translations of [va, va+size) — not just marks
    // them dirty.
    //
    // invalidate_code() follows box86's SMC contract: it only acts if the
    // range is ENTIRELY under PROT_DYNAREC (isprotectedDB returns 0 as soon as
    // one page isn't). That's fine for a small self-patch, but it's a silent
    // no-op for what a full image reload needs: replacing a multi-MB exe
    // where only a few pages were translated would leave the new process
    // executing the PREVIOUS image's dynablocks at the same addresses. Hence
    // this method, which unconditionally destroys the range's dynablocks.
    // Expensive: call only on an image change.
    virtual void discard_code(uint32_t va, uint32_t size) { invalidate_code(va, size); }

    uint32_t read_u32(uint32_t va) { uint32_t v = 0; read(va, &v, 4); return v; }
    void     write_u32(uint32_t va, uint32_t v) { write(va, &v, 4); }

    // Registers.
    virtual uint32_t reg(int r) = 0;
    virtual void set_reg(int r, uint32_t v) = 0;

    // Segment base for FS (Windows TIB/TEB). Default no-op; Unicorn overrides.
    virtual void set_fs_base(uint32_t base) { (void)base; }

    // True if set_fs_base() gives each guest thread a real per-thread FS base
    // (dynarec backend: segs_offs[_FS]); false when FS is emulated at a fixed
    // linear address (Unicorn: the scheduler swaps the TIB page at linear 0).
    virtual bool fs_base_is_direct() const { return false; }


    // Per-instruction callback (debug tracing). Called with the EIP about to
    // execute. Default no-op; Unicorn installs a UC_HOOK_CODE.
    virtual void set_code_hook(std::function<void(uint32_t)> fn) { (void)fn; }

    // Memory-write callback (debug). Called with (addr, size, value). Default
    // no-op; Unicorn installs a UC_HOOK_MEM_WRITE.
    virtual void set_write_hook(std::function<void(uint32_t,int,uint32_t)> fn) { (void)fn; }

    // Per-thread context save/restore — a threading backend swaps guest threads
    // by saving the running thread's X86 state and loading another's. Default
    // impl uses reg()/set_reg(); backends may override for speed.
    virtual void save_context(struct X86Context& c);
    virtual void load_context(const struct X86Context& c);

    // Convenience stack ops on the guest ESP.
    uint32_t pop_u32() { uint32_t esp = reg(R_ESP); uint32_t v = read_u32(esp); set_reg(R_ESP, esp + 4); return v; }
    void     push_u32(uint32_t v) { uint32_t esp = reg(R_ESP) - 4; write_u32(esp, v); set_reg(R_ESP, esp); }
    // Read the k-th cdecl stack argument (0-based), given ESP already points
    // at the return address (i.e. just after the call, before prologue).
    uint32_t arg(uint32_t k) { return read_u32(reg(R_ESP) + 4 + 4 * k); }

    // Batched read of the eight general registers (R_EAX..R_EDI) into out[].
    // Reading them one at a time via reg() pays a per-thread state resolution
    // on every call; grouping avoids that. Safe because reading GP registers
    // is pure. R_EFLAGS is deliberately excluded: reading it would materialize
    // Box86's deferred flags (UpdateFlags), which is a side effect and has no
    // place in a speculative bulk read.
    //
    // Default implementation is the old register-by-register code, so a
    // backend that doesn't override this stays correct unchanged.
    virtual void regs_gp(uint32_t* out) {
        for (int r = R_EAX; r <= R_EDI; ++r) out[r] = reg(r);
    }

    // ---- Batched trap entry/exit -------------------------------------------
    // Handling registers one at a time (2 reg() + 3 set_reg() per trap) pays a
    // per-thread state resolution on every call; this path is hot enough that
    // batching them matters.
    //
    // Default implementation has the same effect as the old per-register
    // code, so a backend that doesn't override it (e.g. CpuUnicorn, the
    // verification oracle) keeps identical semantics — that's what makes this
    // interface change safe.
    virtual uint32_t trap_retaddr() { return read_u32(reg(R_ESP)); }
    // esp_add: what `ret [imm]` pops off the stack (4, plus 4*argc for stdcall).
    // eip: the final destination, already resolved by the caller.
    virtual void trap_epilogue(uint32_t eax, uint32_t esp_add, uint32_t eip) {
        set_reg(R_EAX, eax);
        set_reg(R_ESP, reg(R_ESP) + esp_add);
        set_reg(R_EIP, eip);
    }

    // ---- Combined entry+exit for a stdcall trap with argc=0, in one call ---
    // trap_retaddr() + trap_epilogue() each pay a per-thread state resolution,
    // and the backend has no way to know two separate calls belong to the
    // same trap. For a trap whose returned value doesn't depend on the return
    // address, the stack read and the EAX/ESP/EIP writes can share one
    // resolution instead.
    //
    // Default implementation preserves the old code's effect and observable
    // order (read ESP, read [ESP], then the three writes), so a backend that
    // doesn't override it stays correct for any caller.
    //
    // Precondition (same as trap_retaddr()): ESP points at the return address
    // (right after the `call`, no arguments pushed — argc=0).
    virtual void trap_ret0(uint32_t eax) {
        const uint32_t esp = reg(R_ESP);
        const uint32_t ret = read_u32(esp);
        set_reg(R_EAX, eax);
        set_reg(R_ESP, esp + 4);
        set_reg(R_EIP, ret);
    }

    // Install the trap handler and the [lo,hi) guest VA range that triggers it.
    virtual void set_trap(uint32_t lo, uint32_t hi, TrapFn fn) = 0;

    // Stop the current run() cleanly from inside a hook (cooperative preemption).
    // The run returns as if it stopped normally, resumable at the next EIP.
    // NATIVE-backend contract: shutdown-grade and MONOTONIC — dyn86's g_stop is
    // never cleared in native mode (dyn86_set_native gates the slice_begin
    // reset), so after one call every LinkNext seam diverts to the epilog
    // before translating, forever, and the async quit=1 broadcast can make
    // ANOTHER thread's DynaRun exit with no break marker (looks like a clean
    // stop). Call it ONLY for process shutdown — never for an ordinary thread
    // exit (ExitThread/TerminateThread have their own paths).
    virtual void request_stop() {}

    // Native instruction budget for the NEXT run() (0 = unlimited). Backends
    // that support it stop after ~n guest instructions WITHOUT a per-insn
    // hook (Unicorn: uc_emu_start count — 10-50x faster than a code hook).
    // take_limit_hit() reports (and clears) whether the last run() ended by
    // exhausting that budget rather than by trap/sentinel/yield.
    virtual void set_run_limit(uint64_t) {}
    virtual bool take_limit_hit() { return false; }

    // Anti-starvation valve: zeroes one emu's block budget (handle from
    // thread_emu_create, or nullptr for the base emu) to force it out of
    // translated code at the next prologue — on-demand preemption.
    // Contract: never touches quit or g_stop — those are monotonic in native
    // mode and reserved for shutdown (see request_stop above); nudge is
    // repeatable and has no effect on guest semantics (it only shortens a
    // time slice). Benign RMW race assumed: the dyn86 prologue does a
    // load-decrement-store on the budget, so a zero written between the load
    // and the store is lost — caught at the next starvation-detection pass.
    // Caller must hold the GIL and iterate the append-only emu registry (same
    // discipline as request_stop under the GIL). No-op by default (backends
    // without a block budget).
    virtual void nudge_thread_budget(void* emu) { (void)emu; }

    // ---- Native-scheduler backend support (one x86emu per guest thread) ----
    // Create an independent emu sharing the process context (block cache,
    // custommem). Null = backend has no multi-emu support (Unicorn): the
    // native scheduler refuses to start on it.
    // Registry contract: append-only, never freed (request_stop iterates it
    // from arbitrary GIL holders and thread_emu_ip pointers are handed to the
    // lock-free watchdog reader — freeing would race both; the leak is bounded
    // by the scheduler's thread cap). Call under the runtime GIL, or before
    // any second runner exists.
    virtual void* thread_emu_create() { return nullptr; }
    // Bind `emu` as the CURRENT HOST THREAD's register file: every reg()/
    // set_reg()/run()/save/load on this host thread now targets it. nullptr
    // rebinds the base emu. TLS — affects only the calling host thread.
    virtual void thread_emu_bind(void* emu) { (void)emu; }
    // Racily-sampleable EIP storage of one emu (per-thread watchdog dumps).
    virtual const uint32_t* thread_emu_ip(void* emu) { (void)emu; return nullptr; }
    // Per-thread liveness (starvation/deadlock diagnostics for the native
    // backend). Writes to *out the number of translated blocks executed by
    // this thread's emu since boot — MONOTONIC (mod 2^32: only DIFFERENCES
    // are used, and an unsigned difference stays exact across wraparound).
    // Derived from the block budget, which every translated block's prologue
    // already decrements: it moves as soon as the thread runs translated
    // code, even a loop that never crosses a shim, and costs nothing extra on
    // the hot path (one add per slice, not per block).
    // Why monotonic rather than the raw budget: the anti-starvation valve
    // zeroes every Running thread's budget on each detection pass, so two
    // consecutive reads of a thread running at a steady rate would show the
    // same value ("blocks since the last nudge") — and the rule "no counter
    // moves => deadlock" would then flag the thread STARVING the others as
    // the deadlocked one.
    // Read is racy (same regime as thread_emu_ip) but ORDERED: the
    // implementation reads the total, THEN the arm flag, THEN the budget, and
    // the writer disarms before accumulating (order commented in place in
    // cpu_box86.cpp). Consequence, and this is the field's contract: the only
    // possible wrong reading is an UNDER-estimate over a window of a few
    // instructions, NEVER a forward jump that would misread as "this thread
    // is racing ahead." Any reimplementation must preserve this ordering.
    // Returns false when the backend has no block counter (field ABSENT, not
    // a lying zero). nullptr = the base emu (main).
    virtual bool thread_emu_blocks(void* emu, uint32_t* out) { (void)emu; (void)out; return false; }
    // Per-thread trap density and GIL contention — traps = entries into the
    // trap window (one GIL acquisition each), cont = contended acquisitions
    // (trylock failed), wait_us = cumulative wait time on those acquisitions.
    // Counted only under D2_FILSTAT=1; returns false when the backend doesn't
    // count (field ABSENT, not a lying zero). Read is racy (same regime as
    // thread_emu_blocks). nullptr = the base emu.
    virtual bool thread_emu_filstat(void* emu, uint32_t* traps, uint32_t* cont, uint32_t* wait_us) {
        (void)emu; (void)traps; (void)cont; (void)wait_us; return false; }

    // Translation-time GUEST->GUEST redirect: the dynarec sends call/jmp targets
    // equal to `from` to `to`, WITHOUT modifying guest memory. Lets a known
    // function (e.g. the palette blit) run a native shim while the guest .text
    // stays byte-identical to disk (pristine mode). No-op on backends without it.
    virtual void set_alternate(uint32_t from, uint32_t to) { (void)from; (void)to; }

    // Warden-safe "intrinsic" for one OS-shim trap slot. When guest control
    // enters `slot_va`, the backend calls fn(cpu, slot_va) at the dynarec trap
    // dispatch BEFORE the generic Bridge handler. fn returns true when it fully
    // serviced the call (EAX/ESP/EIP already set) — the backend resumes without
    // ever entering the Bridge/std::function shim path; false defers to the
    // normal handler. The guest observes NOTHING new (same IAT, same GetProcAddress
    // result, same registers/memory) — this only makes servicing the trap cheaper.
    // Captures every call site (IAT- and GetProcAddress-resolved alike) because
    // they all resolve to the same trap VA. No-op default. (docs/perf)
    using IntrinsicFn = bool(*)(Cpu& cpu, uint32_t slot_va);
    virtual void set_intrinsic(uint32_t slot_va, IntrinsicFn fn) { (void)slot_va; (void)fn; }
    // Runtime A/B toggle. D2_DISABLE_INTRINSICS=1 forces the historical trap path
    // from the start; this lets a self-test flip it per call for state parity.
    virtual void set_intrinsics_enabled(bool on) { (void)on; }

    // Run starting at `eip` until a trap returns false or a fault occurs.
    // Returns true on clean stop (sentinel), false on fault; `fault` gets a
    // reason string pointer (static) when it returns false.
    virtual bool run(uint32_t eip, const char** fault) = 0;

    // Last fault address (valid after run() returns false).
    virtual uint32_t fault_addr() const = 0;

    // Windows exception code for the last fault (valid after run() returns
    // false), for the SEH dispatcher. 0 = not classifiable / not SEH-eligible
    // (e.g. an emulator gap) -> caller terminates as before. Backends that can
    // tell div0 (0xC0000094) / illegal (0xC000001D) / AV (0xC0000005) override.
    virtual uint32_t fault_code() const { return 0; }
};

Cpu* make_cpu_box86();     // cpu_box86.cpp (ARM only; single instance)

} // namespace d2rt


