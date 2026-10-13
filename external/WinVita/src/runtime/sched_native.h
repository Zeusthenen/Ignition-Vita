// src/runtime/sched_native.h — the native backend of ThreadScheduler.
//
// One pthread per guest thread (pte on Vita => real sceKernel threads), all
// pinned to ONE core for now (stage 1), OS preemption. The GIL (gil.h) is
// the scheduler lock: every method here runs with it held (methods are
// called from shims, which run under the trap-dispatch gil::Guard). wait()
// blocks with pthread_cond_wait on the GIL mutex. Wake-up is a full re-poll
// of every blocked thread in id order — the EXACT mirror of the cooperative
// pick_ready(), so Waitable semantics (auto-reset consumption, IOCP guest
// out-param writes, wake ordering) carry over verbatim.
//
// CONTRACT: under native, notify() after ANY Waitable mutation is MANDATORY —
// there is no polling fallback (coop's pick_ready re-polled every switch and
// forgave a forgotten notify; native sleeps forever). Every shim must notify.
#pragma once
#include "runtime/guest_thread.h"
#include "runtime/cpu.h"
#include "runtime/bridge.h"
#include "runtime/gil.h"

#include <memory>
#include <vector>
#include <atomic>
#include <pthread.h>

namespace d2rt {

// Starvation-avoidance net: constants for the mechanism and its detector.
// The corresponding env knobs (D2_FAMINE, D2_FAMINE_WINDOW_MS,
// D2_FAMINE_NAP_US) are parsed by rt_boot's native branch, which calls
// arm_famine() BEFORE run().
constexpr uint32_t kFamineNapUsDefault    = 1000; // nap length = pte's sched_yield floor (~1ms)
constexpr uint32_t kFamineLogEvery        = 8;    // per FLOW (detections, naps): 1st one logged, then 1 in 8
constexpr uint32_t kFamineWindowMsDefault = 200;  // detection window — also bounds de-starvation latency
constexpr uint32_t kFamineExpiredMarginMs = 50;   // margin before counting a deadline "expired"
// GIL observability: past this acquisition age, a window skipped on a failed
// trylock is no longer "a shim making progress" but a holder not giving the
// GIL back — the heartbeat NAMES it (one line, and STRICTLY nothing else: no
// wake, no poke, no extra nap).
constexpr uint32_t kGilStuckMs = 2000;
// MAXIMUM cadence of the "window skipped (GIL busy)" summary line. Bounded
// in TIME, not by a modulo: skips per second depend on the detection window
// (5/s at a 200ms window, 50/s at a 20ms one), so a modulo would give a
// different log rate per knob. 5s = at most 12 lines per minute regardless
// of the window, under the watchdog's own cadence (10s): the log can never
// stay silent for more than 5s while the net is giving up, and it still
// can't flood boot_progress.
constexpr uint32_t kFamineSkipReportMs = 5000;
// MAXIMUM number of runners detailed in the "run=" field (the rest are
// summarized as "+k": truncating silently would make the field lie). 10
// fits on the alive: line and covers a typical session's guest thread count.
constexpr uint32_t kVivacityMaxThreads = 10;

class NativeScheduler : public ThreadScheduler {
public:
    NativeScheduler(Cpu* cpu, Bridge* br,
                    uint32_t stack_region, uint32_t stack_each, uint32_t tib_region);

    GuestThread* set_main(uint32_t entry, uint32_t stack_top, uint32_t tib) override;
    GuestThread* create_thread(uint32_t entry, uint32_t param,
                               uint32_t stack_size, bool suspended) override;
    void resume(GuestThread* t) override;
    GuestThread* current() override;
    void yield() override;             // brief GIL release + sched_yield
    uint32_t wait(Waitable* w, uint32_t timeout_ms) override;
    void wait_noresult(Waitable* w, uint32_t timeout_ms) override;
    uint32_t wait_timeout_result(Waitable* w, uint32_t timeout_ms,
                                 uint32_t eax_on_timeout) override;
    void notify(Waitable* w) override;
    void exit_current(uint32_t code) override;
    void run() override;               // runs MAIN on the calling host thread
    int live_count() override;

    void set_no_preempt(bool) override;        // no-op + one-shot log
    void request_shutdown() override;
    GuestThread* thread_by_id(uint32_t id) override;
    std::vector<uint32_t> live_thread_ids() override;
    int live_thread_count() override;
    void dump_threads_to(void(*sink)(const char*)) override;   // watchdog: trylock
    void set_fault_dispatcher(FaultFn f) override { fault_disp_ = std::move(f); }
    // Stored but UNUSED for now: the sink is the cooperative backend's
    // mechanism for refreshing the guest-inlined GetTickCount page when the
    // VIRTUAL clock advances. Native has no virtual clock — the tick page is
    // served real-time by tick_real through the shims (rt_boot disables the
    // guest-inline tick under native).
    void set_time_sink(std::function<void(uint64_t)> f) override { time_sink_ = std::move(f); }
    const char* stop_reason() const override { return stop_reason_; }

    // ---- Second core: which guest thread may run elsewhere -----------------
    // The engine knows no module names and has no built-in notion of which
    // guest thread is the "server" thread: the only thing it knows is that
    // thread's guest ENTRY address, supplied by the consumer at setup. 0
    // (default) = nobody, i.e. the single-core topology, byte for byte.
    // Configured once at setup, never guessed by the engine: it just
    // compares against `t->entry`.
    void set_server_thread_entry(uint32_t guest_entry_va) { server_entry_ = guest_entry_va; }
    uint32_t server_thread_entry() const { return server_entry_; }

    // Watchdog heartbeat: total wake deliveries (the native "switches").
    const uint64_t* wakes_ptr() const { return &wakes_total_; }
    // ---- Starvation-avoidance net: arming + observability -------------------
    // Called BEFORE run() by rt_boot's native branch. Stores the config and
    // arms fam_on_; the heartbeat pthread is started by run() and stopped
    // (flag + join) BEFORE the runners' bounded join. frames = the game's
    // frame counter, supplied by the caller, read volatile. 0 => defaults.
    void arm_famine(const volatile int* frames, uint32_t window_ms, uint32_t nap_us);
    bool     fam_armed()      const { return fam_on_.load(std::memory_order_relaxed); }
    uint32_t fam_detections() const { return fam_detections_.load(std::memory_order_relaxed); }
    uint32_t fam_naps()       const { return fam_naps_.load(std::memory_order_relaxed); }
    uint32_t fam_gil_skips()  const { return fam_gil_skips_.load(std::memory_order_relaxed); }
    // Pointers for the Vita watchdog's alive: line (field fam=<detections>/
    // <naps>, NATIVE ONLY — coop passes null and the field is absent, not a
    // lying zero). 32-bit read, tear-free on ARM32 — same heartbeat regime as
    // wakes_ptr().
    const uint32_t* fam_detections_ptr() const {
        static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
                      "atomic<u32> doit être lisible comme un u32 nu (watchdog)");
        return reinterpret_cast<const uint32_t*>(&fam_detections_);
    }
    const uint32_t* fam_naps_ptr() const { return reinterpret_cast<const uint32_t*>(&fam_naps_); }
    // ---- GIL observability — lock-free reads --------------------------------
    // The holder's guest thread id comes from gil::Probe::owner_tag, resolved
    // by gil::probe() from the STACK MARK published by lock() (see gil.h: no
    // more pthread_self() on the trap path). Threads that may take the GIL
    // register their stack here: main in run(), runners in runner(). Label 0
    // = unregistered or ambiguous thread, printed as "host".
    // DECLARED stack window, below the registration point. Always a STRICT
    // SUBSET of the real stack (gil.h):
    //   * runner: 256 KiB stack (pthread_attr_setstacksize in create_thread),
    //     registered a few hundred bytes below the top — 240 KiB leaves
    //     16 KiB of margin;
    //   * main: 4 MiB on Vita (sceUserMainThreadStackSize), 8 MiB by default
    //     on the host, and run() is called at shallow stack depth.
    static constexpr uintptr_t kRunnerStackDeclared = 240 * 1024;
    static constexpr uintptr_t kMainStackDeclared   = 512 * 1024;
    // Formats the "gil=..." field of the watchdog's alive: line. Returns
    // bytes written, 0 when the GIL is inactive (coop: field ABSENT, not a
    // lying zero — same convention as the fam= field). Called by the
    // watchdog thread ONLY: its statics (a lower-bound duration at that
    // thread's own cadence) have no other writer.
    int gil_field(char* out, unsigned n);
    // ---- Per-runner liveness (starvation vs. deadlock) ----------------------
    // Formats the "run=<id>:<state>:<blocks>[,...]" field of the alive: line
    // (and of the starvation heartbeat's lines, the only such window
    // available under qemu).
    // <blocks> = translated blocks executed since boot
    // (Cpu::thread_emu_blocks), MONOTONE: it moves as soon as the thread runs
    // any translated code, shim or not.
    // <state> = R Running, B Blocked, P Ready, N New.
    // READ, between TWO successive samples while frames= is frozen:
    //   * one counter moves            => STARVATION (someone is running,
    //                                     the others make no progress);
    //   * none move, all states B      => everyone is WAITING. Healthy if
    //                                     something else must happen first
    //                                     (network I/O), a DEADLOCK if
    //                                     nobody can signal anymore;
    //   * none move, one R             => that thread is stuck INSIDE a shim
    //                                     (see gil=/dans=).
    // These call for opposite fixes, hence the field. It does NOT claim to
    // tell a healthy wait from a deadlock by itself: the state published next
    // to the counter supplies the missing half.
    // Returns bytes written, 0 = nothing to say (coop, or a backend without a
    // block counter) — field ABSENT rather than a lying zero, like fam=/gil=;
    // "run=?" when there is no room (never a field that silently vanishes).
    // Lock-free (flat_ snapshot), callable from both the watchdog thread and
    // the starvation heartbeat.
    int vivacity_field(char* out, unsigned n);
    // ---- Per-thread block census (stage 2, multi-core decision) -----------
    // Same counter as vivacity_field, but CUMULATIVE and over ALL threads,
    // Finished ones INCLUDED. That's the difference that matters:
    // vivacity_field skips finished threads because it answers "who is
    // making progress NOW"; here the question is "who did work SINCE BOOT",
    // and a thread that did all its work and then exited must still show up,
    // or the measurement would lie by omission exactly when it matters most.
    //
    // WHAT IT DECIDES: spreading work across cores only helps if several
    // guest threads actually execute translated code. If one thread carries
    // nearly all the blocks, spreading threads across three cores changes
    // nothing — the hot thread stays alone on its own core. The "max=NN%"
    // field is therefore the decision number.
    //
    // Called at TEARDOWN, from the main thread, once no runner is still
    // running: no lock needed, the counters have stopped moving.
    // Returns bytes written; 0 if the backend has no block counter (field
    // ABSENT rather than a lying zero, like fam=/gil=/run=).
    int blocks_census(char* out, unsigned n);
    // Polled GIL-free (rt_boot's d2rt_poll_gilfree: short timeout, re-check)
    // to re-test shutdown between 250ms slices — without this, a thread
    // parked 15s inside a ::poll could outlive teardown's bounded join (2s)
    // and re-enter shims AFTER main's static destructors ran (use-after-free).
    // Volatile read without the GIL, intentional (single-core for now).
    bool shutdown_requested() const { return shutdown_; }
    uint64_t wake_signal_ = 0, wake_timeout_ = 0;   // parity with coop's counters

private:
    // Per-thread native extension, owned via GuestThread::native.
    struct NT {
        // Runner's host thread. STAYS 0 for main (which never goes through
        // pthread_create): nothing reads it for observability anymore now
        // that GIL-holder identity is a stack mark (gil.h) — do not put a
        // pthread_self() back here. Teardown's bounded join skips main via
        // is_main, before it even looks at this field.
        pthread_t       pth{};
        pthread_cond_t  cv;
        void*           emu = nullptr;      // Cpu::thread_emu_create handle
        bool            woken = false;      // wake_check_all delivered
        uint64_t        t_signal_us = 0;    // D2_WAKEPROF: timestamp of the pthread_cond_signal call
        uint32_t        delivered_eax = 0;
        bool            is_main = false;
        bool            joined = false;
        // ---- Starvation-avoidance net ----
        // SceUID of the underlying kernel thread (int32_t: the psp2 headers
        // aren't visible here). Set in runner() on Vita
        // (sceKernelGetThreadId), 0 elsewhere and for main. Diagnostics only
        // for now; a priority-boost fallback would also need it.
        int32_t         sce_uid = 0;
        // Deadline of an in-flight TIMED wait, on the now_ms()/CLOCK_MONOTONIC
        // base (NOT the cond_timedwait's absolute REALTIME value — the
        // starvation detector compares against now_ms()). Set by wait_common
        // before blocking, cleared after; two writes per wait, under the GIL.
        // 0 = no timed wait in flight.
        uint64_t        deadline_ms = 0;
        // Last starvation epoch acknowledged by THIS thread (0 = never).
        // Written and read only by the runner itself, outside the GIL —
        // single-writer.
        uint32_t        fam_ack = 0;
        // Has this thread already logged a nap? Each thread's 1st nap is
        // logged in addition to the global kFamineLogEvery modulo — otherwise
        // a thread could stay hidden behind the modulo. Same single-writer
        // regime as fam_ack (the runner itself, outside the GIL).
        bool            fam_napped = false;
        // The runner has given up its host thread for good (no more guest
        // code can run on this thread's stack). Set under the GIL just
        // before the last unlock. Required condition for stack/TIB
        // recycling: Finished state alone isn't enough — TerminateThread
        // sets it FROM THE OUTSIDE while the target may still be running.
        bool            runner_done = false;
    };
    static void* runner_tramp(void* guest_thread);
    void runner(GuestThread* t);            // worker body (GIL taken inside)
    // ---- Second core: pin a designated "server" guest thread (a guest
    // thread like any other) on ANOTHER core — WX86_COEUR_SERVEUR=<c> (1..3 =
    // USER_c; absent or 0 = the default topology, everything on USER_0).
    // Thread identification: by guest ENTRY ADDRESS, which the CONSUMER
    // supplies via set_server_thread_entry() (the engine knows no modules),
    // or by guest id (WX86_COEUR_SERVEUR_ID=<n>, takes priority if given).
    // Under qemu there are no USER cores: the same decision becomes a HOST
    // affinity when WX86_QEMU_MONOCOEUR=<cpu> emulates the console topology
    // (all runners + main on <cpu>, the server on <cpu>+c). Without these
    // knobs, no affinity call is made at all.
    bool is_server_thread(GuestThread* t);
    void pin_runner(GuestThread* t, bool is_main);   // called BY THE THREAD ITSELF, at entry
    void run_guest(GuestThread* t);         // seed + cpu run + finish (GIL held at entry/exit)
    void seed_first_run(GuestThread* t);
    void finish_thread(GuestThread* t, bool ok, const char* fault);
    void wake_check_all();                  // pick_ready() mirror; GIL held
    uint32_t wait_common(Waitable* w, uint32_t timeout_ms, uint32_t eax_on_timeout);
    NT* nt(GuestThread* t) { return static_cast<NT*>(t->native); }
    uint64_t now_ms() const;

    Cpu* cpu_; Bridge* br_;
    uint32_t server_entry_ = 0;             // second core: server thread's guest entry address (0 = none)
    uint32_t stack_region_, stack_each_, stack_next_;
    uint32_t tib_region_, tib_next_;
    std::vector<std::unique_ptr<GuestThread>> threads_;   // mutated under GIL only
    // Flat snapshot for the lock-free watchdog reader (same pattern as the
    // cooperative scheduler).
    static constexpr uint32_t kFlatMax = 128;
    GuestThread* flat_[kFlatMax] = {nullptr};
    std::atomic<uint32_t> flat_n_{0};
    GuestThread* main_ = nullptr;
    uint32_t next_id_ = 1;
    FaultFn fault_disp_;
    std::function<void(uint64_t)> time_sink_;
    // volatile is sufficient for now: every runner is pinned on ONE core (no
    // multi-core visibility to guarantee); move to atomics in stage 2.
    volatile bool shutdown_ = false;
    // Watchdog reads this lock-free via wakes_ptr() from another core (ARM32:
    // a 64-bit load can tear) — heartbeat semantics, torn reads tolerated.
    uint64_t wakes_total_ = 0;
    const char* stop_reason_ = "";
    // Set once a thread has faulted. Teardown must not overwrite the fault
    // reason with "main exited": a fault reported as a normal exit is a
    // diagnostic that lies, and it costs hours the next time someone reads
    // the log of a crash that "did not happen".
    bool faulted_ = false;
    bool no_preempt_warned_ = false;
    bool stack_overrun_warned_ = false;     // one-shot TIB-overrun warning
    uint32_t stacks_reused_ = 0;            // stacks/TIBs of finished threads, reused
    uint32_t thread_cap_ = 64;              // fail CreateThread cleanly past this

    // ---- Starvation-avoidance net: mechanism only (poke + nap at the
    // translation seam). The detector/heartbeat writes fam_epoch_/fam_on_/
    // fam_detections_/fam_gil_skips_. As long as fam_on_ is false and
    // fam_epoch_ is 0, everything is inert: the spontaneous expiry of the
    // 0x7FFFFFFF budget keeps today's "spurious resume" behavior, with no
    // nap (the epoch is already acknowledged by construction). Atomics:
    // runners read outside the GIL, and the heartbeat runs on another core
    // on Vita.
    std::atomic<uint32_t> fam_epoch_{0};      // starvation epoch, 0 = never; ++ by the heartbeat under a GIL trylock
    // CONTRACTUAL ORDER: the heartbeat MUST publish fam_epoch_ BEFORE poking
    // the budgets — the translation seam reads the EPOCH, not the budget; if
    // a poke happened before the publish, a thread mid-seam could miss both
    // (budget reloaded by the slice, epoch not seen yet), self-healing only
    // at the next window.
    std::atomic<bool>     fam_on_{false};     // armed by the heartbeat (D2_FAMINE) — default: net off
    std::atomic<uint32_t> fam_detections_{0}; // heartbeat's detection count
    std::atomic<uint32_t> fam_naps_{0};       // naps taken (incremented outside the GIL by fam_note_nap)
    std::atomic<uint32_t> fam_gil_skips_{0};  // windows skipped on a failed GIL trylock
    std::atomic<uint32_t> fam_gil_stuck_{0};  // windows skipped WITH a stuck holder — separate log stream
    // Bounded summary of the "failed GIL trylock" branch: timestamp of the
    // last summary published, and count of skips already summarized. Written
    // and read by the heartbeat thread ONLY — no atomic needed.
    uint64_t fam_skip_last_ms_ = 0;
    uint32_t fam_skip_last_n_  = 0;
    uint32_t fam_nap_us_ = kFamineNapUsDefault;  // nap duration (knob D2_FAMINE_NAP_US)
    void fam_nap();                           // sceKernelDelayThread / nanosleep — call OUTSIDE the GIL
    void fam_note_nap(GuestThread* t, uint32_t eip);  // counter + rate-limited log

    // ---- Starvation-detector heartbeat. Started by run() if fam_on_,
    // stopped (flag + join) before the runners' bounded join. Host (qemu): a
    // pthread (fam_tramp/fam_th_). Vita: a RAW Sce thread instead of a pte
    // one — pte creates its threads at priority 191, the LEAST urgent, and
    // discards sceKernelStartThread's return code, which would leave this
    // heartbeat silently never running. Explicit priority 0x10000100, pinned
    // to USER_2 by the CREATOR before start, with create/pin/start return
    // codes all checked (see run()).
    static void* fam_tramp(void* self);
    void fam_beat();                          // detection loop — public to both thread-creation paths
    pthread_t fam_th_{};                      // host only
#ifdef __vita__
    int32_t fam_sce_th_ = -1;                 // SceUID of the raw Sce heartbeat thread
#endif
    bool fam_started_ = false;                // heartbeat started by run()
    std::atomic<bool> fam_stop_{false};       // stop flag (teardown)
    const volatile int* fam_frame_ = nullptr; // frame counter (caller-owned), read volatile
    uint32_t fam_window_ms_ = kFamineWindowMsDefault;
};

// Backref for runner_tramp (a plain pthread start_routine has one void* and it
// carries the GuestThread*). Defined in sched_native.cpp; assigned by
// make_scheduler right after `new NativeScheduler(...)`, BEFORE any
// create_thread.
extern NativeScheduler* g_native_sched;

} // namespace d2rt
