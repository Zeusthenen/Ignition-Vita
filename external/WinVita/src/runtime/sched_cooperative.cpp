// src/runtime/sched_cooperative.cpp — see sched_cooperative.h.
#include "runtime/sched_cooperative.h"
#include "runtime/prof.h"

// Engine-owned progress log (platform/vita_host.h): writes a durable line on
// console, no-op elsewhere. Direct, strongly-linked call — a generic engine
// must not depend on a specific consumer's symbol names for something this
// basic to work.
#include "platform/vita_host.h"
extern "C" uint32_t d2rt_sw_seq = 0;   // C-visible scheduler event counter (dynarec eipring timestamps)

#include <cstdio>
#include <vector>
#include <utility>
#include <cstring>

extern "C" void dyn86_dump_xfer(void);   // dynarec transfer ring (D2_XFERTRACE)
// D2_JITPROFILE (src/dynarec86/dyn86.h): guest-run wall time, one clock pair
// per slice. dyn86_jitprof is 0 unless the knob is set at boot.
extern "C" { extern int dyn86_jitprof; extern uint64_t dyn86_jp_run_us; uint64_t dyn86_jp_now_us(void); }

namespace d2rt {

using S = GuestThread::State;

CooperativeScheduler::CooperativeScheduler(Cpu* cpu, Bridge* br,
                                           uint32_t stack_region, uint32_t stack_each,
                                           uint32_t tib_region)
    : cpu_(cpu), br_(br),
      stack_region_(stack_region), stack_each_(stack_each), stack_next_(stack_region),
      tib_region_(tib_region), tib_next_(tib_region) {}

GuestThread* CooperativeScheduler::set_main(uint32_t entry, uint32_t stack_top, uint32_t main_tib) {
    auto t = std::make_unique<GuestThread>();
    t->id = next_id_++;
    t->stack_top = stack_top;
    t->stack_base = stack_top - 0x200000;
    uint32_t tib;
    if (cpu_->fs_base_is_direct()) {
        // fs-direct backend (CpuBox86): the caller's TIB address (typically 0 —
        // the pre-scheduler phase ran with FS base 0 and the live TIB mapped at
        // linear 0) IS the main thread's TIB; FS points at it on every slice.
        // No copy: whatever the CRT already stored there stays authoritative.
        tib = main_tib;
    } else {
        // Page-swap backend (Unicorn): give main its own TIB backing page,
        // seeded from the current active TIB at linear 0.
        tib = tib_next_; tib_next_ += 0x1000;
        cpu_->map(tib, 0x1000, nullptr, P_RW);
        std::vector<uint8_t> buf(0x1000); cpu_->read(0, buf.data(), 0x1000); cpu_->write(tib, buf.data(), 0x1000);
    }
    t->tib = tib; t->ctx.fs_base = tib;
    t->entry = entry; t->ctx.eip = entry; t->started = false; t->state = S::Ready;
    auto* p = t.get(); threads_.push_back(std::move(t));
    { uint32_t n = flat_n_.load(std::memory_order_relaxed);      // flat snapshot: main thread too
      if (n < kFlatMax) { flat_[n] = p; flat_n_.store(n + 1, std::memory_order_release); } }
    return p;
}

GuestThread* CooperativeScheduler::create_thread(uint32_t entry, uint32_t param,
                                                 uint32_t stack_size, bool suspended) {
    auto t = std::make_unique<GuestThread>();
    t->id = next_id_++;
    uint32_t ssize = stack_size ? ((stack_size + 0xFFFF) & ~0xFFFFu) : stack_each_;
    // The bump allocator below is never reclaimed. Over a long session with
    // many CreateThread calls (short-lived workers recreated repeatedly),
    // stacks would climb into the TIB region, then the trap window, then off
    // the arena entirely on Vita (a silent host overrun, not a clean fault).
    // RECYCLE first: a Finished thread never runs again, so its stack + TIB
    // are free real estate — exactly what Windows does when a TEB/stack is
    // released. Exact-size match only (no splitting), and the donor hands
    // over ownership so it is reused once.
    uint32_t sbase = 0, tib = 0;
    for (auto& u : threads_) {
        GuestThread* g = u.get();
        if (g->state == S::Finished && g->stack_base && (g->stack_top - g->stack_base) == ssize) {
            sbase = g->stack_base; tib = g->tib;
            g->stack_base = g->stack_top = 0; g->tib = 0;   // ownership transferred
            ++stacks_reused_;
            break;
        }
    }
    if (sbase) {
        // Windows hands a thread a ZEROED stack and a fresh TEB page: clear both
        // so a recycled worker can never read the previous thread's leftovers.
        static std::vector<uint8_t> z(0x10000, 0);
        for (uint32_t o = 0; o < ssize; o += (uint32_t)z.size())
            cpu_->write(sbase + o, z.data(), (ssize - o) > (uint32_t)z.size() ? (uint32_t)z.size() : (ssize - o));
        if (tib) cpu_->write(tib, z.data(), 0x1000);
    } else {
        sbase = stack_next_; stack_next_ += ssize + 0x10000;
        if (tib_region_ && stack_next_ > tib_region_ && !stack_overrun_warned_) {
            stack_overrun_warned_ = true;
            std::fprintf(stderr, "[sched] WARNING worker stacks (%u threads) reached the TIB region 0x%08x — long-session overrun risk\n",
                         next_id_, tib_region_);
            char m[128]; std::snprintf(m, sizeof m, "SCHED: %u threads, worker stacks reached TIB 0x%08x (overrun risk)", next_id_, tib_region_);
            wx86_vita_progress_c(m);
        }
        cpu_->map(sbase, ssize, nullptr, P_RW);
    }
    t->stack_base = sbase; t->stack_top = sbase + ssize;
    if (!tib) { tib = tib_next_; tib_next_ += 0x1000; cpu_->map(tib, 0x1000, nullptr, P_RW); }
    cpu_->write_u32(tib + 0x00, 0xFFFFFFFF);       // ExceptionList (end of SEH chain)
    cpu_->write_u32(tib + 0x04, t->stack_top);      // StackBase
    cpu_->write_u32(tib + 0x08, t->stack_base);     // StackLimit
    // Self: linear addr of the active TIB — linear 0 on the page-swap backend
    // (Unicorn), the TIB's own address on the fs-direct backend (CpuBox86).
    cpu_->write_u32(tib + 0x18, cpu_->fs_base_is_direct() ? tib : 0);
    // NOTE: do NOT write ClientId.UniqueThread at TIB+0x24. Some guest code
    // reads the TEB thread id directly to pick a per-thread allocation pool;
    // distinct ids per thread then multiply pool count and heap usage,
    // which can exhaust memory under a heavy load. All zeros (every thread
    // shares one pool) is the behavior that keeps such code working, and
    // matches the C++ GetCurrentThreadId shim never being consulted for
    // pooling.
    t->tib = tib; t->ctx.fs_base = tib;
    t->entry = entry; t->param = param; t->ctx.eip = entry; t->started = false;
    t->state = suspended ? S::New : S::Ready;
    auto* p = t.get(); threads_.push_back(std::move(t));
    // Publish into the flat snapshot BEFORE bumping the count — the Vita
    // watchdog runs on another core and only ever reads [0, flat_n_).
    if (flat_n_.load(std::memory_order_relaxed) < kFlatMax) {
        uint32_t n = flat_n_.load(std::memory_order_relaxed);
        flat_[n] = p; flat_n_.store(n + 1, std::memory_order_release);
    }
    return p;
}

void CooperativeScheduler::resume(GuestThread* t) {
    if (t && t->state == S::New) t->state = S::Ready;
}

uint32_t CooperativeScheduler::wait(Waitable* w, uint32_t timeout_ms) {
    if (!w) return 0;
#ifdef PROF_COUNTERS
    prof::wait_calls++;
    if (w->try_acquire(cur_)) { prof::wait_immediate++; return w->result(); }
#else
    if (w->try_acquire(cur_)) return w->result();   // already signaled
#endif
    cur_->wait_obj = w;
    // Absolute virtual-time deadline (0 = infinite). This is what lets a timed
    // WaitForSingleObject actually time out under the cooperative scheduler —
    // e.g. a render worker waiting on a frame event must still wake up to
    // keep producing, or a thread blocked on it forever would deadlock.
    cur_->wake_deadline = (timeout_ms == 0xFFFFFFFFu) ? 0
                          : virt_ms_ + (timeout_ms ? timeout_ms : 1);
    cur_->state = S::Blocked;
    br_->request_yield();                           // stop resumably; scheduler switches
    return 0;                                        // placeholder; EAX set to result on wake
}

void CooperativeScheduler::notify(Waitable*) { /* pick_ready() re-checks blocked threads */ }

void CooperativeScheduler::exit_current(uint32_t code) {
    if (cur_) { cur_->exit_code = code; cur_->state = S::Finished; }
    br_->request_yield();
}

int CooperativeScheduler::live_count() {
    int n = 0; for (auto& u : threads_) if (u->state != S::Finished) ++n; return n;
}

void CooperativeScheduler::sync_real_clock() {
    if (!real_now_) return;
    uint64_t now = real_now_();
    // Anchor so virtual time is continuous at the moment real mode kicks in.
    if (!real_base_set_) { real_base_ = now - virt_ms_; real_base_set_ = true; }
    uint64_t v = now - real_base_;
    if (v > virt_ms_) { virt_ms_ = v; if (time_sink_) time_sink_(virt_ms_); }
}

GuestThread* CooperativeScheduler::pick_ready() {
    // Wake any Blocked thread whose Waitable is now acquirable.
    for (auto& up : threads_) {
        GuestThread* t = up.get();
        if (t->state == S::Blocked && t->wait_obj) {
            Waitable* w = static_cast<Waitable*>(t->wait_obj);
            if (w->try_acquire(t)) {
                t->wait_obj = nullptr;
                if (t->wake_writes_eax) t->ctx.gpr[0] = w->result();   // EAX = WAIT_OBJECT_0(+index)
                t->wake_writes_eax = true; t->wake_timeout_eax = 0x102;
                t->wake_deadline = 0;
                t->state = S::Ready;
                wake_signal_++;
                sw_rec(t->id, 5, 0, t->ctx.eip);          // WAKEsig
            } else if (real_now_ && real_expire_ && t->wake_deadline && virt_ms_ >= t->wake_deadline
                       && !(defer_towake_ && preempted_last_ && preempted_last_ != t
                            && preempted_last_->state == S::Ready)) {
                // Real-time mode only: expire timed waits even while other
                // threads are runnable (a Sleep(100) must last ~100 real ms,
                // not "until nothing else runs"). Virtual mode keeps the old
                // idle-only expiry — that's the tested deterministic baseline.
                t->wait_obj = nullptr; t->wake_deadline = 0;
                if (t->wake_writes_eax) t->ctx.gpr[0] = t->wake_timeout_eax;  // WAIT_TIMEOUT (or shim override)
                t->wake_writes_eax = true; t->wake_timeout_eax = 0x102;
                t->state = S::Ready;
                wake_timeout_++;
                sw_rec(t->id, 6, 0, t->ctx.eip);          // WAKEto (real-expiry)
            }
        }
    }
    size_t n = threads_.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (rr_ + 1 + i) % n;
        if (threads_[idx]->state == S::Ready) { rr_ = idx; return threads_[idx].get(); }
    }
    // No thread runnable now — but a timed wait may be due. Advance virtual time
    // to the nearest finite deadline and wake that thread with WAIT_TIMEOUT(0x102).
    GuestThread* soonest = nullptr;
    for (auto& up : threads_) { GuestThread* t = up.get();
        if (t->state == S::Blocked && t->wake_deadline)
            if (!soonest || t->wake_deadline < soonest->wake_deadline) soonest = t; }
    if (soonest) {
        if (real_now_) {
            // Real-time mode: nothing can signal a Waitable while every guest
            // thread is blocked (signals only come from guest code), so it is
            // safe to sleep the host thread until the nearest timed deadline.
            for (;;) {
                sync_real_clock();
                if (virt_ms_ >= soonest->wake_deadline || !real_sleep_) break;
                uint64_t d = soonest->wake_deadline - virt_ms_;
                idle_ms_ += (d > 100 ? 100 : d);   // D2_PHASEPROF: host nap time given back
                real_sleep_((uint32_t)(d > 100 ? 100 : d));
            }
        }
        virt_ms_ = soonest->wake_deadline > virt_ms_ ? soonest->wake_deadline : virt_ms_;
        if (time_sink_) time_sink_(virt_ms_);
        wake_timeout_++;
        soonest->wait_obj = nullptr; soonest->wake_deadline = 0;
        if (soonest->wake_writes_eax) soonest->ctx.gpr[0] = soonest->wake_timeout_eax;  // WAIT_TIMEOUT (or shim override)
        soonest->wake_writes_eax = true; soonest->wake_timeout_eax = 0x102;
        soonest->state = S::Ready;
        sw_rec(soonest->id, 7, 0, soonest->ctx.eip);      // WAKEidle (all-blocked timeout)
        return soonest;
    }
    return nullptr;
}

void CooperativeScheduler::run_slice(GuestThread* t) {
    // TIB switch. fs-direct backend (CpuBox86): just point FS at the incoming
    // thread's TIB — guest fs:[...] accesses resolve through segs_offs[_FS].
    // Page-swap backend (Unicorn): copy TIBs through the fixed page at linear 0.
    if (cpu_->fs_base_is_direct()) {
        cpu_->set_fs_base(t->tib);
    } else if (cur_ && cur_ != t) {
        std::vector<uint8_t> buf(0x1000);
        cpu_->read(0, buf.data(), 0x1000); cpu_->write(cur_->tib, buf.data(), 0x1000);
        cpu_->read(t->tib, buf.data(), 0x1000); cpu_->write(0, buf.data(), 0x1000);
    } else if (!cur_) {
        std::vector<uint8_t> buf(0x1000);
        cpu_->read(t->tib, buf.data(), 0x1000); cpu_->write(0, buf.data(), 0x1000);
    }
    cur_ = t;
    sw_rec(t->id, 1, 0, t->ctx.eip);                      // RUN
    // Preemption: with a probe installed (diagnostics), use the per-insn code
    // hook (slow but instrumented). Without one, use the backend's NATIVE
    // instruction budget (Unicorn uc_emu_start count) — 10-50x faster.
    if (probe_ && !hook_installed_) {
        cpu_->set_code_hook([this](uint32_t eip){
            if (probe_) probe_(eip);
            if (++slice_insns_ >= quantum_) { preempted_ = true; cpu_->request_stop(); } });
        hook_installed_ = true;
    }
    if (!probe_) cpu_->set_run_limit(quantum_);
    cpu_->load_context(t->ctx);
    if (!t->started) {
        uint32_t esp = t->stack_top;
        esp -= 4; cpu_->write_u32(esp, t->param);          // arg to thread proc
        esp -= 4; cpu_->write_u32(esp, br_->sentinel());   // return -> thread exit
        cpu_->set_reg(R_ESP, esp);
        cpu_->set_reg(R_EIP, t->entry);
        t->started = true;
    }
    t->state = S::Running;
    slice_insns_ = 0; preempted_ = false;
    const char* fault = nullptr;
#ifdef PROF_COUNTERS
    uint64_t prof_t0 = prof::now_ns();
    uint32_t prof_b0 = 0; cpu_->thread_emu_blocks(nullptr, &prof_b0);   // blocks per thread (exact)
#endif
    // D2_JITPROFILE guest-run timer. Deliberately here — ONE pair of clock
    // reads per scheduler slice — and NOT around each dynablock: per-block
    // timing would cost more than the blocks. INCLUSIVE: this covers the
    // translated ARM code, the JIT, the lookups and the OS-shim bodies that
    // run inside the slice (see dyn86.h). Off => two dead branches.
    uint64_t jp_t0 = dyn86_jitprof ? dyn86_jp_now_us() : 0;
    bool ok = cpu_->run(cpu_->reg(R_EIP), &fault);
    if (dyn86_jitprof) dyn86_jp_run_us += dyn86_jp_now_us() - jp_t0;
#ifdef PROF_COUNTERS
    { uint64_t dt = prof::now_ns() - prof_t0; prof::run_ns += dt; t->prof_ns += dt; }
#endif
    cpu_->save_context(t->ctx);
    if (cpu_->take_limit_hit()) preempted_ = true;        // native insn-budget expiry
#ifdef PROF_COUNTERS
    // Hot-block sampling: one EIP sample per PREEMPTED slice (quantum expiry is
    // effectively a random cut in guest execution — unbiased, zero per-insn cost).
    if (preempted_) { prof::sample_eip(cpu_->reg(R_EIP)); prof::sample_eip_tid(cpu_->reg(R_EIP), t->id); }
    { uint32_t prof_b1 = 0; cpu_->thread_emu_blocks(nullptr, &prof_b1);
      prof::add_blocks_tid(t->id, prof_b1 - prof_b0); prof::note_thread(t->id, t->entry); }
#endif
    if (preempted_) {
        if (t->state == S::Running) t->state = S::Ready;  // time-slice expired; resume later
        preempted_last_ = t;                              // mid-computation (D2_TOWAKE_DEFER)
        sw_rec(t->id, 2, 0, t->ctx.eip);                  // PREEMPT
    } else if (br_->take_yield()) {
        if (t->state == S::Running) t->state = S::Ready;  // plain yield; wait() already set Blocked
        if (preempted_last_ == t) preempted_last_ = nullptr;   // voluntary boundary reached
        sw_rec(t->id, 3, t->state == S::Blocked ? 1 : 0, t->ctx.eip);   // BLOCK(aux=1)/yield
    } else if (!ok) {
        // Fault dispatcher: the consumer is called BEFORE the thread is
        // terminated, with the precise Windows exception code, and answers
        // 0 = terminate or 1 = resume. code==0 (an emulator gap, not a guest
        // fault) is deliberately NOT dispatch-eligible: letting a consumer
        // "handle" a gap in this emulator would mask it and diverge from real
        // hardware.
        uint32_t code = cpu_->fault_code();
        int act = (code && fault_disp_) ? fault_disp_(t, code, cpu_->fault_addr()) : 0;
        if (act == 1) { if (t->state == S::Running) t->state = S::Ready; return; }  // resume
        // Exit code: the precise one when the emulator could name the fault
        // (divide-by-zero, illegal instruction), the access-violation fallback
        // when it could not. Reporting every fault as 0xC0000005 discarded a
        // value the emulator had already computed.
        t->exit_code = code ? code : 0xC0000005u;
        t->state = S::Finished; stop_reason_ = fault ? fault : "fault";
        std::printf("  [sched] thread %u FAULT: %s  EIP=0x%08x faultAddr=0x%08x ESP=0x%08x EAX=0x%08x\n",
                    t->id, stop_reason_, cpu_->reg(R_EIP), cpu_->fault_addr(),
                    cpu_->reg(R_ESP), cpu_->reg(R_EAX));
        dyn86_dump_xfer();   // D2_XFERTRACE + D2_NOLINK: last control transfers before the fault
        // STACK SCAN: sample stack words that point INTO a loaded module — a
        // cheap call-stack hint when the frame is lost. Ranges come from the
        // bridge (each loaded module's base + size) rather than hardcoded
        // addresses, so this keeps working regardless of where a given
        // port's modules sit in memory.
        { uint32_t esp = cpu_->reg(R_ESP);
          const std::vector<std::pair<uint32_t,uint32_t>> mods =
              br_ ? br_->loaded_modules() : std::vector<std::pair<uint32_t,uint32_t>>();
          // Bounded by the thread's own stack — see the matching comment in
          // sched_native.cpp: a fixed 0x100 window read off the end of the
          // region when the fault happened near the stack top, and the
          // diagnostic killed the process it was meant to explain.
          const uint32_t span = (t->stack_top && esp >= t->stack_base && esp < t->stack_top)
                              ? (t->stack_top - esp) : 0x100u;
          for (uint32_t off = 0; off < (span < 0x100u ? span : 0x100u); off += 4) {
              uint32_t v = cpu_->read_u32(esp + off);
              for (size_t k = 0; k < mods.size(); ++k)
                  if (v >= mods[k].first && v - mods[k].first < mods[k].second) {
                      std::printf("      [esp+0x%02x] 0x%08x  (module @%08x +0x%x)\n",
                                  off, v, mods[k].first, (unsigned)(v - mods[k].first));
                      break; } } }
    } else {
        t->exit_code = cpu_->reg(R_EAX); t->state = S::Finished;  // returned to sentinel
        if (getenv("THREADLOG"))
            std::printf("  [sched] thread %u FINISHED (exit 0x%08x)\n", t->id, t->exit_code);
    }
}

void CooperativeScheduler::run() {
    while (switches_ < max_switches_ && !shutdown_) {
        sync_real_clock();
        // Loader-lock pin: while a DllMain runs non-preemptibly, a time-slice
        // preempt must re-run the SAME thread rather than switch away (another
        // thread mutating shared dynarec state mid-DllMain is what corrupts the
        // CRT init). Only honored while the pinned thread is Ready — an I/O
        // yield (Blocked) or a fault (Finished) falls through to normal
        // scheduling, so a never-cleared pin can never deadlock.
        GuestThread* t = (no_preempt_thread_ && no_preempt_thread_->state == S::Ready)
                             ? no_preempt_thread_ : pick_ready();
        if (!t) { stop_reason_ = (live_count() ? "deadlock (all blocked)" : "all finished");
            if (live_count())
                for (auto& up : threads_) { GuestThread* g = up.get();
                    if (g->state != S::Finished)
                        std::printf("  [sched] blocked thread %u: state=%d wait=%s eip=0x%08x\n",
                                    g->id, (int)g->state,
                                    g->wait_obj ? static_cast<Waitable*>(g->wait_obj)->kind() : "-",
                                    g->ctx.eip); }
            return; }
        ++switches_; t->slices++;
        run_slice(t);
    }
    stop_reason_ = shutdown_ ? "shutdown requested" : "max switches";
}

} // namespace d2rt
