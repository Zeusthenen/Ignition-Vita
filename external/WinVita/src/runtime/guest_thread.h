// src/runtime/guest_thread.h — guest (x86) threading abstraction.
//
// Two backends, ONE API:
//   GuestThread + ThreadScheduler → CooperativeScheduler (debug: deterministic,
//   single-runner, save/restore x86 context on one CPU) | NativeVitaScheduler
//   (final: one real sceKernelThread per guest thread, running its own
//   X86Context in parallel).
//
// Design rules (so the native backend drops in without API changes):
//   * Each GuestThread owns an independent X86Context, stack, and TIB/TLS.
//   * NO assumption that only one x86 thread runs at a time — call sites use
//     the scheduler's wait()/wake()/yield(), never a global "the CPU".
//   * Kernel sync objects (event/mutex/semaphore/IOCP/thread) are Waitables
//     with real semantics; the scheduler blocks/wakes threads on them.
//   * The dynarec cache, block chaining/invalidation and x86↔ARM bridges must
//     be thread-safe (locks live in those modules, not here) — this header only
//     fixes the threading contract.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace d2rt {

// Full architectural state of one guest thread. gpr[] indices match Reg
// (R_EAX..R_EDI); fs_base is this thread's TIB linear address.
struct X86Context {
    uint32_t gpr[8] = {0,0,0,0,0,0,0,0};
    uint32_t eip = 0;
    uint32_t eflags = 0x202;
    uint32_t fs_base = 0;   // per-thread TIB
    // Per-thread FPU/SIMD state (opaque blob, Box86 backend: x87 stack +
    // control/status/tags, MMX, XMM, mxcsr, long-double/int64 shadows).
    // Quantum preemption lands mid-computation where this state is live;
    // sharing one copy across guest threads corrupts whichever thread
    // resumes (stale FPU state surfaces as corrupted float results or a
    // wild jump in guest code). fpu_valid=false = thread has not run yet
    // -> backend loads its pristine power-on FPU state instead.
    uint8_t fpu[768] = {0};
    bool fpu_valid = false;
};

class ThreadScheduler;  // fwd

class GuestThread {
public:
    uint32_t id = 0;
    X86Context ctx;
    uint32_t stack_base = 0, stack_top = 0;   // [base, top) guest stack region
    uint32_t tib = 0;                          // this thread's TIB region (4 KiB)
    uint32_t entry = 0, param = 0;             // thread proc + arg
    uint32_t exit_code = 0;
    uint64_t slices = 0;                       // scheduler slices consumed (diagnostics)
    uint64_t prof_ns = 0;                      // time in cpu->run() (PROF_COUNTERS builds)

    enum class State : int { New, Ready, Running, Blocked, Finished };
    State state = State::New;

    // What this thread is blocked on (opaque; the scheduler owns wake-up).
    void* wait_obj = nullptr;
    uint64_t wake_deadline = 0;   // for timed waits (0 = infinite)
    bool wake_writes_eax = true;  // false: wake must not clobber the shim's return value
                                  // (peek-throttle wait — PeekMessage already returned 0)
    uint32_t wake_timeout_eax = 0x102;  // EAX a timed-out wake writes. WAIT_TIMEOUT fits
                                        // WaitForSingleObject-style shims; BOOL-returning
                                        // ones (GetQueuedCompletionStatus) need 0=FALSE —
                                        // 0x102 there reads as TRUE and the caller
                                        // processes a phantom completion packet.
    bool started = false;         // has the entry stub been set up on first run
    void* native = nullptr;       // NativeScheduler per-thread extension (NT)
};

// A kernel object a thread can wait on and that can be signaled: event, mutex,
// semaphore, thread handle, IO-completion-port. Real semantics; thread-safe in
// the native backend (the object holds its own lock there).
class Waitable {
public:
    virtual ~Waitable() = default;
    // Try to satisfy the wait without blocking. Return true if acquired/signaled
    // (auto-reset objects consume their signal here). Called under the scheduler.
    virtual bool try_acquire(GuestThread* t) = 0;
    // Non-consuming probe: would try_acquire succeed right now? Needed by
    // WaitForMultipleObjects(bWaitAll): the old all-path consumed objects one
    // by one and failed on the first unready one — auto-reset events and
    // semaphore counts were eaten and lost. Conservative default (false) so an
    // un-overridden type can never fake readiness; every concrete K-type in
    // rt_boot overrides it.
    virtual bool ready(GuestThread* t) { (void)t; return false; }
    // Value the guest sees as the wait return (EAX) after a successful acquire —
    // WAIT_OBJECT_0(0) for a single object, WAIT_OBJECT_0+i for a multi-wait.
    virtual uint32_t result() const { return 0; }
    // Human tag for logs.
    virtual const char* kind() const = 0;
};

// Backend-agnostic scheduler. Cooperative and native both implement this; call
// sites (the Win32 sync shims, CreateThread) use only this interface.
class ThreadScheduler {
public:
    virtual ~ThreadScheduler() = default;

    // Create a runnable guest thread (suspended if `suspended`). Allocates its
    // stack + TIB via the provided allocators at construction time.
    virtual GuestThread* create_thread(uint32_t entry, uint32_t param,
                                       uint32_t stack_size, bool suspended) = 0;
    virtual void resume(GuestThread*) = 0;

    virtual GuestThread* current() = 0;

    // Cooperative: switch to another ready thread. Native: sched_yield / spin.
    virtual void yield() = 0;

    // Block the current thread until `w` is acquirable (or timeout ms elapses;
    // 0xFFFFFFFF = infinite). Immediate acquire returns w->result()
    // (WAIT_OBJECT_0+index for a multi-wait). Cooperative backend: the blocked
    // path returns a placeholder 0 and the REAL result is written into the
    // saved context's EAX on wake. Native backend: always returns the real
    // outcome (w->result() on acquire, 0x102 WAIT_TIMEOUT on timeout).
    virtual uint32_t wait(Waitable* w, uint32_t timeout_ms) = 0;

    // Signal: make blocked waiters re-evaluate `w` (auto-reset consumes once).
    virtual void notify(Waitable* w) = 0;

    // Mark the current thread finished with exit_code and never return to it.
    virtual void exit_current(uint32_t code) = 0;

    // Run the scheduler until every thread is Finished (or deadlocked). Used by
    // the cooperative backend as the main driver; the native backend blocks the
    // launching thread until all guest threads exit.
    virtual void run() = 0;

    // Diagnostics.
    virtual int live_count() = 0;

    // ---- Extended contract: everything the Win32 shims call.
    // Timed wait whose wake must NOT decide the shim's return value (the shim
    // already returned it — e.g. the real-clock PeekMessage throttle).
    virtual void wait_noresult(Waitable* w, uint32_t timeout_ms) = 0;
    // Timed wait with a custom timeout return value. RETURNS the value the
    // shim must return as EAX. Cooperative backend: returns a placeholder —
    // the real EAX is written into the saved context on wake. Native backend:
    // blocks and returns the REAL outcome (w->result() on acquire,
    // eax_on_timeout on timeout). Shims MUST `return` this value.
    virtual uint32_t wait_timeout_result(Waitable* w, uint32_t timeout_ms,
                                         uint32_t eax_on_timeout) = 0;
    // Loader-lock semantics around DllMain (see sched_cooperative.h). Native
    // backend: no-op (the hazards it guarded — shared emu, hot slots_ growth —
    // are fixed by design there).
    virtual void set_no_preempt(bool on) = 0;
    virtual void request_shutdown() = 0;
    virtual GuestThread* thread_by_id(uint32_t id) = 0;
    virtual std::vector<uint32_t> live_thread_ids() = 0;
    virtual int live_thread_count() = 0;
    virtual void dump_threads_to(void(*sink)(const char*)) = 0;
    virtual GuestThread* set_main(uint32_t entry, uint32_t stack_top, uint32_t tib) = 0;
    using FaultFn = std::function<int(GuestThread*, uint32_t, uint32_t)>;
    virtual void set_fault_dispatcher(FaultFn f) = 0;
    virtual void set_time_sink(std::function<void(uint64_t)> f) = 0;
    virtual const char* stop_reason() const = 0;
    int current_id() { GuestThread* t = current(); return t ? (int)t->id : -1; }
};

} // namespace d2rt
