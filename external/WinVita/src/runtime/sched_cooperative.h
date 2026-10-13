// src/runtime/sched_cooperative.h — deterministic single-runner scheduler.
//
// Debug backend of the GuestThread abstraction: one Unicorn CPU, N guest
// threads, context save/restore on switch. A running thread yields by calling
// Bridge::request_yield() (via yield()/wait()); the run stops resumably and the
// scheduler round-robins the next Ready thread. Blocked threads wake when their
// Waitable becomes acquirable (notify()).
//
// The NativeVitaScheduler will implement the same ThreadScheduler API with one
// sceKernelThread per guest thread — call sites never change.
#pragma once
#include "runtime/guest_thread.h"
#include "runtime/cpu.h"
#include "runtime/bridge.h"

#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include <cstdio>

extern "C" { extern uint32_t d2rt_sw_seq; }   // C-visible scheduler event counter
                                              // (dynarec eipring timestamps; defined
                                              // in sched_cooperative.cpp)
namespace d2rt {

class CooperativeScheduler : public ThreadScheduler {
public:
    // stack_region: guest VA where per-thread stacks are carved (each `stack`
    // bytes, grown down). tib_region: where per-thread TIBs live. Both must be
    // unmapped ranges the scheduler may map into via cpu.
    CooperativeScheduler(Cpu* cpu, Bridge* br,
                         uint32_t stack_region, uint32_t stack_each,
                         uint32_t tib_region);

    // Wrap the initial/main thread (the guest executable's entry point). Uses
    // the given stack top (the bridge's main stack) and tib (linear 0).
    // Returns the main thread.
    GuestThread* set_main(uint32_t entry, uint32_t stack_top, uint32_t tib) override;

    GuestThread* create_thread(uint32_t entry, uint32_t param,
                               uint32_t stack_size, bool suspended) override;
    void resume(GuestThread* t) override;
    GuestThread* current() override { return cur_; }
    void yield() override { br_->request_yield(); }
    uint32_t wait(Waitable* w, uint32_t timeout_ms) override;
    // Timed wait whose wake leaves EAX untouched — for shims that already
    // returned their value (e.g. the real-clock PeekMessage throttle).
    void wait_noresult(Waitable* w, uint32_t timeout_ms) override {
        wait(w, timeout_ms);
        if (cur_ && cur_->state == GuestThread::State::Blocked) cur_->wake_writes_eax = false;
    }
    // Timed wait with a custom EAX on timeout — for shims whose Win32 return
    // value is not a wait code (GetQueuedCompletionStatus returns BOOL: a
    // timed-out call must come back FALSE, not WAIT_TIMEOUT=0x102=TRUE).
    uint32_t wait_timeout_result(Waitable* w, uint32_t timeout_ms, uint32_t eax_on_timeout) override {
        uint32_t r = wait(w, timeout_ms);
        if (cur_ && cur_->state == GuestThread::State::Blocked) cur_->wake_timeout_eax = eax_on_timeout;
        return r;   // placeholder in coop (EAX rewritten on wake) — see interface doc
    }
    void notify(Waitable* w) override;
    void exit_current(uint32_t code) override;
    void run() override;
    int live_count() override;

    // Diagnostics / safety.
    void set_max_switches(uint64_t n) { max_switches_ = n; }
    void set_quantum(uint32_t insns) { quantum_ = insns; }   // preemption slice length
    // Loader-lock semantics: pin the CURRENT thread so a time-slice preempt
    // re-runs it instead of switching away. Used around a DLL's DllMain (CRT
    // init): if another guest thread runs mid-DllMain it mutates the SHARED
    // dynarec state (block cache), and the CRT can resume into an unrelocated
    // address. A plain I/O yield still deschedules normally (the pin only
    // overrides a time-slice preempt). Cleared when DllMain returns. Mirrors
    // Windows serializing DLL_PROCESS_ATTACH under the loader lock.
    void set_no_preempt(bool on) override { no_preempt_thread_ = on ? cur_ : nullptr; }
    // Optional per-instruction probe (breakpoints in scheduler mode), chained
    // inside the preemption hook. Runs on EVERY instruction — keep it cheap.
    void set_probe(std::function<void(uint32_t)> p) { probe_ = std::move(p); }
    // Fault dispatcher: called in the fault branch BEFORE terminating a
    // faulting thread, with (thread, windows_exception_code, fault_addr).
    // What it does with that is the consumer's business — this engine does not
    // dispatch exceptions to guest code itself. Returns 0 = terminate, 1 =
    // resume. Only fires on a real fault, so it never touches the fault-free
    // boot/online-load paths.
    void set_fault_dispatcher(FaultFn f) override { fault_disp_ = std::move(f); }
    // Honest Toolhelp/OpenThread/GetThreadContext support: expose the REAL guest
    // thread set (never a fabricated view). live_thread_ids/count skip Finished.
    std::vector<uint32_t> live_thread_ids() override {
        std::vector<uint32_t> v;
        for (auto& up : threads_) if (up->state != GuestThread::State::Finished) v.push_back(up->id);
        return v;
    }
    int live_thread_count() override {
        int n = 0; for (auto& up : threads_) if (up->state != GuestThread::State::Finished) n++;
        return n;
    }
    GuestThread* thread_by_id(uint32_t id) override {
        for (auto& up : threads_) if (up->id == id) return up.get();
        return nullptr;
    }
    // Clean external stop (e.g. harness frame budget reached): finish the
    // current slice, then leave run() with stop_reason "shutdown requested".
    void request_shutdown() override { shutdown_ = true; if (cpu_) cpu_->request_stop(); }
    // Silent scheduler-event ring (diag): RUN/PREEMPT/BLOCK/FINISH/WAKE events
    // with thread id, guest EIP and virtual ms — recorded with zero I/O so a
    // timing race is not perturbed; dumped when the guest reports a fatal
    // error (crash-log hook).
    struct SwEv { uint32_t seq; uint8_t tid, ev; uint16_t aux; uint32_t eip; uint32_t ms; };
    SwEv sw_ring_[256] = {}; uint32_t sw_seq_ = 0;
    inline void sw_rec(uint32_t tid, uint8_t ev, uint16_t aux, uint32_t eip) {
        SwEv& e = sw_ring_[sw_seq_ & 255]; e.seq = ++sw_seq_; d2rt_sw_seq = sw_seq_;
        e.tid = (uint8_t)tid; e.ev = ev; e.aux = aux; e.eip = eip; e.ms = (uint32_t)virt_ms_;
    }
    void dump_switch_ring() {
        static const char* EV[9] = {"?","RUN","PREEMPT","BLOCK","FINISH","WAKEsig","WAKEto","WAKEidle","FAULT"};
        std::printf("=== [swring] last %u sched events (total %u) ===\n",
                    sw_seq_ < 256 ? sw_seq_ : 256, sw_seq_);
        for (uint32_t k = 0; k < 256; k++) {
            SwEv& e = sw_ring_[(sw_seq_ + k) & 255]; if (!e.seq) continue;
            std::printf("  [sw%u] t%u %-8s eip=%08x ms=%u aux=%u\n",
                        e.seq, e.tid, EV[e.ev < 9 ? e.ev : 0], e.eip, e.ms, e.aux);
        }
    }
    // Diagnostic: print every live thread (state, wait kind, last EIP).
    void dump_threads() {
        for (auto& up : threads_) { GuestThread* g = up.get();
            if (g->state != GuestThread::State::Finished)
                std::printf("  [sched] thread %u: state=%d wait=%s eip=0x%08x slices=%llu\n",
                            g->id, (int)g->state,
                            g->wait_obj ? static_cast<Waitable*>(g->wait_obj)->kind() : "-",
                            g->ctx.eip, (unsigned long long)g->slices); }
    }
    // Same dump routed through a sink (Vita: boot_progress heartbeats). This
    // runs on the WATCHDOG host thread, concurrently with the scheduler:
    // iterating threads_ directly would be a use-after-free waiting to
    // happen — a push_back reallocates the vector under the reader. Walk the
    // flat snapshot instead: entries are published (release) before the count
    // is bumped, and the GuestThread objects themselves never move (unique_ptr
    // heap cells), so a stale-but-consistent view is the worst case. wait_obj
    // is re-read once into a local: the scheduler may clear it mid-dump.
    void dump_threads_to(void(*sink)(const char*)) override {
        uint32_t n = flat_n_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i) {
            GuestThread* g = flat_[i];
            if (!g || g->state == GuestThread::State::Finished) continue;
            void* w = g->wait_obj;
            char m[128];
            std::snprintf(m, sizeof m, "thr %u st=%d wait=%s eip=%08x sl=%llu",
                          g->id, (int)g->state,
                          w ? static_cast<Waitable*>(w)->kind() : "-",
                          g->ctx.eip, (unsigned long long)g->slices);
            sink(m); }
    }
    uint32_t stacks_reused() const { return stacks_reused_; }   // recycled-stack accounting
    // PROF_COUNTERS: per-thread run() time, all threads (finished included).
    void dump_thread_times() {
        for (auto& up : threads_) { GuestThread* g = up.get();
            std::printf("    thread %u: slices=%llu run=%.1f ms%s\n", g->id,
                        (unsigned long long)g->slices, g->prof_ns / 1e6,
                        g->state == GuestThread::State::Finished ? " (finished)" : ""); }
    }
    uint64_t switches() const { return switches_; }
    const uint64_t* switches_ptr() const { return &switches_; }   // watchdog sampling
    // Wake-source attribution (real-clock diagnosis): how blocked threads wake.
    uint64_t wake_signal_ = 0, wake_timeout_ = 0;
    // Cumulative host sleep (ms) while every guest thread is blocked in
    // real-clock mode: CPU time actually given back to the machine. Read via
    // D2_PHASEPROF ("libre-hote="). Zero in virtual-clock mode.
    uint64_t idle_ms_ = 0;
    const char* stop_reason() const override { return stop_reason_; }
    // Real-time clock mode (hardware): virt_ms_ follows now_ms() instead of
    // warping to the next wait deadline, and an all-blocked idle actually
    // sleeps via sleep_ms(). Without this the guest clock advances at
    // emulation speed and the game runs fast-forward. Desktop/qemu never set
    // it — the deterministic virtual clock is what makes runs reproducible.
    void set_real_clock(std::function<uint64_t()> now_ms,
                        std::function<void(uint32_t)> sleep_ms) {
        real_now_ = std::move(now_ms); real_sleep_ = std::move(sleep_ms);
    }
    // Diagnostic knob: real mode normally expires timed waits even while other
    // threads are runnable. false = idle-only expiry (the virtual-mode rule),
    // to isolate whether a bug is triggered by WAIT_TIMEOUT deliveries.
    void set_real_expire(bool on) { real_expire_ = on; }
    void set_defer_towake(bool on) { defer_towake_ = on; }   // see D2_TOWAKE_DEFER above

private:
    GuestThread* pick_ready();     // round-robin; also wakes satisfiable Blocked
    void run_slice(GuestThread* t);
    void sync_real_clock();        // real-time mode: pull virt_ms_ up to now

    FaultFn fault_disp_;                       // SEH dispatcher (empty = terminate on fault)
    std::function<uint64_t()> real_now_;      // monotonic host ms (empty = virtual mode)
    std::function<void(uint32_t)> real_sleep_;
    uint64_t real_base_ = 0; bool real_base_set_ = false;
    bool real_expire_ = true;
    // D2_TOWAKE_DEFER=1: while a thread was BUDGET-preempted mid-computation
    // and has not yet reached a voluntary boundary (block/yield/finish), do
    // not deliver timed-wait EXPIRY wakes to other threads — the {preempt ->
    // WAKEto interleave -> resume} window is exactly what would let another
    // thread iterate a guest list while the preempted thread is mid-update,
    // freeing nodes out from under that iteration. Signal wakes are
    // unaffected; expiries deliver as soon as the interrupted thread blocks
    // (it does every frame), so Sleep durations stay honest.
    bool defer_towake_ = false;
    GuestThread* preempted_last_ = nullptr;

    Cpu* cpu_; Bridge* br_;
    uint32_t stack_region_, stack_each_, stack_next_;
    uint32_t tib_region_, tib_next_;
    std::vector<std::unique_ptr<GuestThread>> threads_;
    // Stable flat view for cross-thread readers (the Vita watchdog).
    static constexpr uint32_t kFlatMax = 128;
    GuestThread* flat_[kFlatMax] = {nullptr};
    std::atomic<uint32_t> flat_n_{0};
    uint32_t stacks_reused_ = 0;        // recycled worker stacks/TIBs
    GuestThread* cur_ = nullptr;
    GuestThread* no_preempt_thread_ = nullptr;  // loader-lock pin; see set_no_preempt
    uint32_t next_id_ = 1;
    size_t rr_ = 0;
    uint64_t switches_ = 0, max_switches_ = 200000000ull;
    uint64_t slice_insns_ = 0;
    uint32_t quantum_ = 300000;   // instructions per slice before preemption
    bool preempted_ = false;
    bool hook_installed_ = false;
    bool stack_overrun_warned_ = false;   // one-shot worker-stack overrun warning
    uint64_t virt_ms_ = 0;                                // virtual clock for timed waits
    std::function<void(uint64_t)> time_sink_;             // fires when virt_ms_ advances
    std::function<void(uint32_t)> probe_;                 // optional breakpoint probe
    bool shutdown_ = false;                               // request_shutdown() flag
public:
    uint64_t virt_ms() const { return virt_ms_; }
    // Observer for virtual-time advances (guest-inlined GetTickCount keeps a
    // tick dword in guest memory; rt_boot refreshes it from here).
    void set_time_sink(std::function<void(uint64_t)> f) override { time_sink_ = std::move(f); }
    const char* stop_reason_ = "";
};

} // namespace d2rt
