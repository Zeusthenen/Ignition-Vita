// src/runtime/sched_native.cpp — see sched_native.h.
#include "runtime/sched_native.h"

extern "C" { extern uint64_t dyn86_jp_run_us; uint64_t dyn86_jp_now_us(void); }
extern "C" {
    int      d2rt_wakeprof = 0;          // D2_WAKEPROF, armed by rt_boot
    uint64_t d2rt_wake_n = 0, d2rt_wake_us = 0, d2rt_wake_worst = 0;
    // Raw count of signals emitted. Without it, "n=0" cannot be told apart
    // from a dead instrument.
    uint64_t d2rt_wake_sig = 0;
    // Count of zero-timeout polls served WITHOUT sleeping — the measure of
    // this optimization's effect.
    uint64_t d2rt_poll0_n = 0;
}
#include "runtime/prof.h"

#include <cstdio>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <sched.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
extern "C" void dyn86_vita_open_vm_thread(void);   // mman_vita.c — VM domain PER THREAD (DACR)
#endif

// Logging, pinning and core accounting belong to the ENGINE
// (platform/vita_host.h), because the engine targets the Vita.
//
// This is the project's DEFAULT scheduler: it is the one that pins and
// accounts for worker threads. Pinning is not cosmetic — placement rules are
// measured on hardware — so this dependency is direct and strongly linked
// rather than a weak reference that could silently resolve to nothing.
//
// The three core-related services stay CONSOLE-ONLY: every call below
// already lives under `#ifdef __vita__` — there is no core to spread work
// across, and no affinity to read back, anywhere else. Logging, though, is
// called from the generic body: it has a no-op version off-console (see
// platform/vita_host.h).
#include "platform/vita_host.h"
// The monotonic clock belongs to the engine (same as bridge.cpp).
#include "runtime/host_clock.h"
extern "C" { extern uint32_t d2rt_sw_seq; }   // tick_real cache invalidation (rt_boot)
extern "C" void dyn86_dump_xfer(void);

namespace d2rt {

using S = GuestThread::State;

static void progress(const char* m) { wx86_vita_progress_c(m); }

NativeScheduler::NativeScheduler(Cpu* cpu, Bridge* br,
                                 uint32_t stack_region, uint32_t stack_each,
                                 uint32_t tib_region)
    : cpu_(cpu), br_(br),
      stack_region_(stack_region), stack_each_(stack_each), stack_next_(stack_region),
      tib_region_(tib_region), tib_next_(tib_region) {
    gil::enable();
}

uint64_t NativeScheduler::now_ms() const {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// ---- GIL observability: the TWO readers, lock-free -------------------------

int NativeScheduler::gil_field(char* out, unsigned n) {
    gil::Probe p; gil::probe(&p);
    if (!p.active || n < 8) return 0;            // coop: field ABSENT from the alive: line
    // Age is DERIVED (gil.h): as long as the acquisition counter doesn't
    // move and the GIL stays held, it's the SAME acquisition. `since` is set
    // at the first sample that sees it, so the reported duration is a LOWER
    // BOUND (at most one watchdog period short of reality) — never inflated.
    static uint32_t last_acq = 0; static bool last_held = false; static uint64_t since = 0;
    uint64_t now = now_ms();
    if (!(p.held && last_held && p.acq == last_acq)) since = now;
    last_acq = p.acq; last_held = p.held;
    if (!p.held) return std::snprintf(out, n, " gil=-");
    char who[24];
    if (p.owner_tag) std::snprintf(who, sizeof who, "thr%u", p.owner_tag);
    else             std::snprintf(who, sizeof who, "hote");
    int w = std::snprintf(out, n, " gil=%s/>=%llums/acq=%u", who,
                          (unsigned long long)(now - since), p.acq);
    if (w > 0 && (unsigned)w < n && p.in_shim)
        w += std::snprintf(out + w, n - (unsigned)w, " dans=%s",
                           p.shim ? p.shim : "?");
    return w < 0 ? 0 : (w > (int)n ? (int)n : w);
}

// ---- Per-runner liveness (sched_native.h) -----------------------------------
//
// WHY THIS MEASUREMENT POINT: the counter is derived from the emu's block
// budget, decremented by the prologue of EVERY translated block. That makes
// it the translation seam itself — the most frequent point a progressing
// guest thread crosses, and the ONLY one that stays true when a thread spins
// without calling any shim (an InterlockedCompareExchange/select spin loop
// goes through traps, but a purely-translated spin loop would not). A trap
// counter would have this blind spot; this one doesn't, and it costs
// NOTHING on the hot path (Cpu::thread_emu_blocks: one addition per SLICE,
// never per block).
//
// WHAT THE FIELD SAYS: "translated blocks executed since boot", MONOTONIC
// (modulo 2^32; only differences are read, and an unsigned difference stays
// exact across the wraparound). Two samples are enough:
//   * one counter moves while frames= is frozen   => STARVATION;
//   * NONE move                                   => everyone has stopped.
//
// WHAT THE FIELD DOES NOT SAY: "stopped" is NOT "deadlocked". A thread
// parked in WaitForSingleObject, in recv, or in a blocking select is
// healthily blocked and its counter doesn't move either. Hence the state
// published RIGHT NEXT TO the counter (R/B/P/N): that's what separates
// "everyone is waiting on the network" (B everywhere, healthy) from a
// deadlock (an R making no progress, or B on objects nobody will ever
// signal). A thread that executes very few blocks between two samples stays
// indistinguishable from a stopped one at this resolution.
int NativeScheduler::vivacity_field(char* out, unsigned n) {
    if (!cpu_ || !out || n < 8) return 0;
    // One question to the backend: if it has no block counter, the field is
    // ABSENT (nothing written to `out`), never a lying zero.
    uint32_t probe = 0;
    if (!cpu_->thread_emu_blocks(nullptr, &probe)) return 0;
    // Not enough room: the field stays PRESENT, with an admittedly-unknown
    // value. "Absent" means "coop backend" and nothing else — a field that
    // disappears because the line was full would make the silence
    // ambiguous at the worst possible moment.
    if (n < 24) return std::snprintf(out, n, " run=?");
    char buf[256]; int w = 0; unsigned shown = 0, skipped = 0;
    // 24 = worst case for one entry (",<id>:<E>:<10 digits>") — an entry is
    // never split in two, unlisted threads go into "+k" instead. The same
    // constant guards both the entry AND the "+k" suffix.
    static const int kEntry = 24;
    uint32_t fn = flat_n_.load(std::memory_order_acquire);   // flat snapshot, lock-free
    for (uint32_t i = 0; i < fn && i < kFlatMax; ++i) {
        GuestThread* g = flat_[i];
        if (!g || g->state == S::Finished || !g->native) continue;
        if (shown >= kVivacityMaxThreads || w > (int)sizeof buf - kEntry) { ++skipped; continue; }
        uint32_t blocks = 0;
        cpu_->thread_emu_blocks(static_cast<NT*>(g->native)->emu, &blocks);  // racy read, accepted
        // State NEXT TO the counter: R=Running, B=Blocked, P=Ready, N=New.
        // Two characters, and "no counter moves" stops being ambiguous
        // between a healthy wait and a pathological block.
        char st = g->state == S::Running ? 'R' : g->state == S::Blocked ? 'B'
                : g->state == S::Ready   ? 'P' : 'N';
        w += std::snprintf(buf + w, sizeof buf - (size_t)w, "%s%u:%c:%u",
                           shown ? "," : "", g->id, st, (unsigned)blocks);
        ++shown;
    }
    if (!shown) return 0;                       // no live thread: nothing to say
    if (skipped && w <= (int)sizeof buf - kEntry)
        w += std::snprintf(buf + w, sizeof buf - (size_t)w, "+%u", skipped);
    int r = std::snprintf(out, n, " run=%s", buf);
    if (r < 0) { out[0] = 0; return 0; }
    return (unsigned)r >= n ? (int)(n - 1) : r; // snprintf truncated AND terminated: no leftover
}

// ---- Per-thread block census (sched_native.h) ------------------------------
//
// Called at teardown, from the main thread, with no runner in flight:
// lock-free read, assumed race-free.
//
// THREE SAFEGUARDS, because this number decides on future work:
//
//  1. ALL threads, Finished included. vivacity_field skips them (it answers
//     "who is progressing right now"); omitting them HERE would drop from
//     the total a thread that did all its work and then exited — the
//     measurement would lie by omission exactly where it matters for a
//     decision.
//  2. Main counts like any other thread. It runs on the BASE emu
//     (`nt->emu == nullptr`), and thread_emu_blocks(nullptr) returns exactly
//     that counter. Forgetting it would attribute 0% to the thread that,
//     under coop, does everything.
//  3. The total is that of the LISTED threads, never a separate total: a
//     percentage whose denominator includes threads absent from the list
//     would be unverifiable by inspection.
int NativeScheduler::blocks_census(char* out, unsigned n) {
    if (!cpu_ || !out || n < 16) return 0;
    uint32_t probe = 0;
    if (!cpu_->thread_emu_blocks(nullptr, &probe)) return 0;   // backend has no counter

    struct Ent { uint32_t id; uint32_t blocks; };
    Ent e[kFlatMax + 1]; unsigned ne = 0;
    unsigned long long total = 0;
    for (auto& up : threads_) {
        GuestThread* g = up.get();
        if (!g || ne >= kFlatMax + 1) break;
        // A thread with no native extension never ran under this backend: it
        // has no counter, and inventing one at 0 would make it look idle
        // when it is simply out of scope.
        if (!g->native) continue;
        uint32_t b = 0;
        cpu_->thread_emu_blocks(static_cast<NT*>(g->native)->emu, &b);
        e[ne].id = g->id; e[ne].blocks = b; ++ne;
        total += b;
    }
    if (!ne || !total) return 0;

    // Descending sort (insertion sort: ne <= 129, done at teardown).
    for (unsigned i = 1; i < ne; ++i) {
        Ent k = e[i]; int j = (int)i - 1;
        while (j >= 0 && e[j].blocks < k.blocks) { e[j + 1] = e[j]; --j; }
        e[j + 1] = k;
    }

    // "max=" comes first: it's the decision number, and must never be
    // truncated by the list that follows it.
    unsigned pct = (unsigned)((e[0].blocks * 200ull + total) / (2ull * total));
    int w = std::snprintf(out, n, "blocs/fil: total=%llu fils=%u max=%u%% (fil %u)",
                          total, ne, pct, e[0].id);
    if (w < 0 || (unsigned)w >= n) { out[n - 1] = 0; return (int)(n - 1); }
    for (unsigned i = 0; i < ne && i < 12; ++i) {
        int r = std::snprintf(out + w, n - (unsigned)w, "%s%u:%u",
                              i ? "," : " | ", e[i].id, e[i].blocks);
        if (r < 0 || (unsigned)(w + r) >= n) { out[w] = 0; break; }   // never a truncated entry
        w += r;
    }
    return w;
}

// ---- thread creation --------------------------------------------------------

GuestThread* NativeScheduler::set_main(uint32_t entry, uint32_t stack_top, uint32_t main_tib) {
    gil::Guard g;
    auto t = std::make_unique<GuestThread>();
    t->id = next_id_++;
    t->stack_top = stack_top; t->stack_base = stack_top - 0x200000;
    t->tib = main_tib; t->ctx.fs_base = main_tib;   // fs-direct backend only (asserted at selection)
    t->entry = entry; t->ctx.eip = entry; t->started = false; t->state = S::Ready;
    NT* n = new NT(); pthread_cond_init(&n->cv, nullptr);
    n->is_main = true; n->emu = nullptr;            // main runs on the BASE emu
    t->native = n;
    auto* p = t.get(); threads_.push_back(std::move(t));
    uint32_t fn = flat_n_.load(std::memory_order_relaxed);
    if (fn < kFlatMax) { flat_[fn] = p; flat_n_.store(fn + 1, std::memory_order_release); }
    main_ = p;
    return p;
}

GuestThread* NativeScheduler::create_thread(uint32_t entry, uint32_t param,
                                            uint32_t stack_size, bool suspended) {
    // Caller = a shim => GIL already held. Guest stack/TIB carving mirrors
    // the cooperative allocator, including stack/TIB recycling (see
    // sched_cooperative.cpp) — the native map would otherwise be append-only
    // for the lock-free flat_ reader.
    gil::assert_held();
    if (shutdown_) return nullptr;          // teardown race: no new runners once shutting down
    if ((uint32_t)threads_.size() >= thread_cap_) {
        std::fprintf(stderr, "[sched-native] thread cap %u reached — CreateThread fails cleanly\n", thread_cap_);
        return nullptr;
    }
    auto t = std::make_unique<GuestThread>();
    t->id = next_id_++;
    uint32_t ssize = stack_size ? ((stack_size + 0xFFFF) & ~0xFFFFu) : stack_each_;
    // Recycles finished threads' stacks/TIBs; without this the allocation
    // only ever grows, and a long session with many CreateThread calls
    // eventually overruns the TIB region, then the trap window, then the
    // arena itself. A Finished thread whose runner has given up its host
    // thread will never run again, so its stack and TIB are free real
    // estate — exactly what Windows does when a TEB/stack is released.
    // Exact-size match only; the donor hands over ownership so it is reused
    // once.
    uint32_t sbase = 0, tib = 0;
    for (auto& u : threads_) {
        GuestThread* g = u.get();
        if (g->state == S::Finished && g->native && nt(g)->runner_done && !nt(g)->is_main &&
            g->stack_base && (g->stack_top - g->stack_base) == ssize && g->tib) {
            sbase = g->stack_base; tib = g->tib;
            g->stack_base = g->stack_top = 0; g->tib = 0;
            ++stacks_reused_;
            if (stacks_reused_ <= 8) { char m[128]; std::snprintf(m, sizeof m,
                "SCHED-NATIVE: pile+TIB du fil %u recyclees pour le fil %u (reutilisations=%u)",
                g->id, t->id, stacks_reused_); progress(m); }
            break;
        }
    }
    if (sbase) {
        // Windows hands out a ZEROED stack and a fresh TEB page.
        static const std::vector<uint8_t> z(0x10000, 0);
        for (uint32_t o = 0; o < ssize; o += (uint32_t)z.size())
            cpu_->write(sbase + o, z.data(), (ssize - o) > (uint32_t)z.size() ? (uint32_t)z.size() : (ssize - o));
        cpu_->write(tib, z.data(), 0x1000);
    } else {
        // Nothing left to recycle AND the stack region is full: REFUSE
        // cleanly (CreateThread fails) rather than place a stack on the
        // TIBs, the trap window, or outside the arena.
        if (tib_region_ && stack_next_ + ssize > tib_region_) {
            if (!stack_overrun_warned_) {
                stack_overrun_warned_ = true;
                char m[160]; std::snprintf(m, sizeof m,
                    "SCHED-NATIVE: region des piles pleine (%u fils vivants) — CreateThread REFUSE plutot que deborder sur les TIB",
                    (unsigned)threads_.size());
                progress(m);
            }
            return nullptr;
        }
        sbase = stack_next_; stack_next_ += ssize + 0x10000;
        cpu_->map(sbase, ssize, nullptr, P_RW);
        tib = tib_next_; tib_next_ += 0x1000;
        cpu_->map(tib, 0x1000, nullptr, P_RW);
    }
    t->stack_base = sbase; t->stack_top = sbase + ssize;
    cpu_->write_u32(tib + 0x00, 0xFFFFFFFF);        // ExceptionList (end of SEH chain)
    cpu_->write_u32(tib + 0x04, t->stack_top);      // StackBase
    cpu_->write_u32(tib + 0x08, t->stack_base);     // StackLimit
    cpu_->write_u32(tib + 0x18, tib);               // fs-direct: Self = own address
    // TIB+0x24 (ClientId.UniqueThread) DELIBERATELY zero — see
    // sched_cooperative.cpp for why.
    t->tib = tib; t->ctx.fs_base = tib;
    t->entry = entry; t->param = param; t->ctx.eip = entry;
    t->started = false; t->state = suspended ? S::New : S::Ready;
    NT* n = new NT(); pthread_cond_init(&n->cv, nullptr);
    n->emu = cpu_->thread_emu_create();
    t->native = n;
    auto* p = t.get(); threads_.push_back(std::move(t));
    uint32_t fn = flat_n_.load(std::memory_order_relaxed);
    if (fn < kFlatMax) { flat_[fn] = p; flat_n_.store(fn + 1, std::memory_order_release); }
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);     // shim bodies use std::string/printf
    int rc = pthread_create(&n->pth, &at, runner_tramp, p);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        std::fprintf(stderr, "[sched-native] pthread_create FAILED (rc=%d)\n", rc);
        p->state = S::Finished; return nullptr;
    }
    return p;
}

void NativeScheduler::resume(GuestThread* t) {
    // Caller = shim => GIL held.
    gil::assert_held();
    if (t && t->state == S::New) { t->state = S::Ready; pthread_cond_signal(&nt(t)->cv); }
}

// TLS current-thread pointer: set by run() (main) and runner() (workers).
static __thread GuestThread* t_cur = nullptr;
GuestThread* NativeScheduler::current() { return t_cur; }

// D2_YIELD: what Sleep(0)/yield() does under the native scheduler.
//   0 (default) = the Vita pte's sched_yield(), whose FLOOR is ~1ms. Guest
//       code that calls Sleep(0)/yield very frequently pays that floor every
//       time, which adds up.
//   1 = sceKernelDelayThread(D2_YIELD_US, default 10): yields the core to an
//       equal-priority ready thread without the pte's floor.
//   3 = bounded active wait of D2_YIELD_US us, GIL released, no kernel call.
//   2 = yields only the GIL (no kernel call at all): guest threads are
//       preemptible on Vita, so a Sleep(0) that returns immediately is
//       still valid Windows semantics.
static uint64_t now_us_mono() {
#ifdef __vita__
    return (uint64_t)sceKernelGetSystemTimeWide();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
#endif
}
static int yield_mode() {
    static int m = -1;
    if (m < 0) { const char* e = getenv("WX86_YIELD"); if (!e) e = getenv("D2_YIELD"); m = (e && *e) ? atoi(e) : 0; if (m < 0 || m > 3) m = 0; }
    return m;
}
static unsigned yield_us() {
    static int us = -1;
    if (us < 0) { const char* e = getenv("WX86_YIELD_US"); if (!e) e = getenv("D2_YIELD_US"); us = (e && *e) ? atoi(e) : 10; if (us < 0) us = 10; }
    return (unsigned)us;
}
void NativeScheduler::yield() {
    // OS preemption makes cooperative yields advisory. Still honor the intent
    // (spin-break, SwitchToThread): give siblings the core for a beat.
    gil::Release r;
    switch (yield_mode()) {
#ifdef __vita__
      case 1: sceKernelDelayThread(yield_us()); break;
#endif
      case 2: break;
      case 3: {
        // Bounded ACTIVE wait, GIL released, NO kernel call. A kernel sleep
        // (mode 1) has a high fixed cost per call — the syscall and
        // rescheduling, not the requested duration — and guest code that
        // yields very frequently pays that cost every time. Returning
        // immediately (mode 2) removes the cost but also the pacing a
        // spin/poll loop relies on, which can make it iterate more often and
        // cost more overall. Spinning for a small but real amount of time
        // keeps the pacing while cutting the per-call cost. Other guest
        // threads are true preemptible threads: they keep running during
        // the wait.
        const uint64_t t0 = now_us_mono();
        const uint64_t us = (uint64_t)yield_us();
        while (now_us_mono() - t0 < us) { __asm__ __volatile__("" ::: "memory"); }
        break; }
      default: sched_yield(); break;
    }
}

// ---- wait / notify (the heart; GIL held throughout except inside cond_wait) -

uint32_t NativeScheduler::wait_common(Waitable* w, uint32_t timeout_ms, uint32_t eax_on_timeout) {
    gil::assert_held();
    GuestThread* t = t_cur;
    // TerminateThread landed while this thread was computing (its shim
    // marked this thread Finished from OUTSIDE): never block again — post a redirect
    // so this shim's trap tail sends the thread to the sentinel;
    // finish_thread's already-Finished guard then preserves
    // TerminateThread's exit code.
    // Native kills at the NEXT WAIT (coop kills at the next slice instead —
    // one notch coarser: a terminated compute-bound thread keeps running
    // until its next wait). A thread terminated while BLOCKED keeps its host
    // pthread parked on its cv until shutdown (nothing re-signals it —
    // wake_check_all only walks Blocked threads): a bounded leak of one
    // 256 KiB host stack per kill, accepted for now.
    // Terminated-while-Running is the case handled below.
    if (t && t->state == S::Finished) { br_->redirect_next(br_->sentinel()); return 0; }
    if (!w) return 0;
    NT* n = nt(t);
    if (w->try_acquire(t)) return w->result();
    // A ZERO timeout is a TEST, not a WAIT.
    // Windows: WaitForSingleObject(h, 0) on an unsignaled object returns
    // WAIT_TIMEOUT INSTANTLY. This path used to sleep for >= 1ms regardless
    // (see "ms = timeout_ms ? timeout_ms : 1" below), a rule copied from the
    // cooperative scheduler where it is necessary: under the virtual clock,
    // time only advances via deadlines, and a poll that returned immediately
    // would stall the clock. Native, though, refuses D2_VIRTCLOCK at startup
    // (native is real-clock by construction), so that rule serves no purpose
    // here and costs full price: guest code polling with a zero timeout ends
    // up sleeping at least a millisecond on every check that should be
    // instantaneous — and the effect is self-amplifying, since the slower
    // things get, the more objects end up waited on, the more polling
    // happens, and the more sleeping happens.
    //
    // D2_POLL0=0 restores the old behavior, for A/B comparison.
    if (timeout_ms == 0) {
        static int poll0 = -1;
        if (poll0 < 0) { const char* e = getenv("WX86_POLL0"); if (!e) e = getenv("D2_POLL0"); poll0 = (e && *e == '0') ? 0 : 1; }
        if (poll0) { ++d2rt_poll0_n; return eax_on_timeout; }
    }
    // Honest blocked-context snapshot for GetThreadContext/dumps.
    cpu_->save_context(t->ctx);
    t->wait_obj = w;
    t->state = S::Blocked;
    n->woken = false;
    bool inf = (timeout_ms == 0xFFFFFFFFu);
    timespec abs;
    if (!inf) {
        // timeout 0 => 1 ms, replicating the cooperative deadline rule
        // (sched_cooperative.cpp wait(): virt_ms_ + (timeout ? timeout : 1)).
        uint32_t ms = timeout_ms ? timeout_ms : 1;
        // CLOCK_REALTIME because pthread_cond_timedwait measures the absolute
        // deadline against it:
        //   glibc (qemu): REALTIME base by default, re-evaluated on every
        //     wakeup — a clock step shifts pending timeouts;
        //   pte (Vita): cond_timedwait -> sem_timedwait -> pte_relmillisecs
        //     = abstime - ftime(), and ftime = clock_gettime(CLOCK_REALTIME);
        //     the RELATIVE delta then goes into sceKernelWaitSema (usec). The
        //     abs->rel conversion happens on EVERY (re)entry into the wait,
        //     so a clock step only skews conversions made after it, never
        //     waits already in flight.
        // TRAP — pthread_condattr_setclock(CLOCK_MONOTONIC): pte ACCEPTS it
        // (returns 0, stores the id in the attr) but the wait path NEVER
        // reads that attribute back — the call would be a silent no-op that
        // creates exactly the base/measurement mismatch it claims to avoid.
        // Do not use it; the shared REALTIME base on both sides is the only
        // coherent configuration here.
        clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_sec  += ms / 1000;
        abs.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
        // Starvation net: deadline published for the detector, on the
        // now_ms()/MONOTONIC base (the REALTIME abs value above is only for
        // the cond). Set here under the GIL, cleared under the GIL after
        // wake-up.
        n->deadline_ms = now_ms() + ms;
    }
    // pte lazy-init note (Vita): PTHREAD_MUTEX_INITIALIZER is a sentinel that
    // pthread_mutex_lock materializes on FIRST lock(); pthread_cond_wait on a
    // never-locked mutex would touch the un-materialized sentinel. Safe here
    // by construction: the ctor enable()d the GIL and run()/runner() lock()
    // it before any wait can execute — a lock() always precedes the first
    // cond_wait on this mutex.
    int rc = 0;
    gil::mark_released();               // observability: the cond releases the mutex here
    while (!n->woken && !shutdown_) {
        rc = inf ? pthread_cond_wait(&n->cv, gil::mutex())
                 : pthread_cond_timedwait(&n->cv, gil::mutex(), &abs);
        if (rc == ETIMEDOUT && !n->woken) break;
    }
    if (d2rt_wakeprof && n->t_signal_us) {
        const uint64_t dt = wx86_now_us() - n->t_signal_us;
        n->t_signal_us = 0;
        ++d2rt_wake_n; d2rt_wake_us += dt;
        if (dt > d2rt_wake_worst) d2rt_wake_worst = dt;
    }
    // The cv released/reacquired the GIL mutex behind lock()/unlock()'s back:
    // re-stamp ownership so a later assert_held() in this shim body holds.
    gil::mark_owned();
    n->deadline_ms = 0;                 // starvation net: no timed wait in flight anymore
    t->wait_obj = nullptr;
    // Guarded transition (same idiom as the cooperative scheduler): never
    // clobber a Finished posted from OUTSIDE (TerminateThread) while blocked.
    if (t->state == S::Blocked) t->state = S::Running;
    if (n->woken) { wake_signal_++; wakes_total_++; d2rt_sw_seq++; return n->delivered_eax; }
    wake_timeout_++; wakes_total_++; d2rt_sw_seq++;
    return eax_on_timeout;      // shutdown_ also lands here: shim returns, next
                                // trap dispatch sees shutdown and stops the run
}

uint32_t NativeScheduler::wait(Waitable* w, uint32_t timeout_ms) {
    return wait_common(w, timeout_ms, 0x102);      // WAIT_TIMEOUT
}
void NativeScheduler::wait_noresult(Waitable* w, uint32_t timeout_ms) {
    (void)wait_common(w, timeout_ms, 0x102);       // shim supplies its own EAX
}
uint32_t NativeScheduler::wait_timeout_result(Waitable* w, uint32_t timeout_ms,
                                              uint32_t eax_on_timeout) {
    return wait_common(w, timeout_ms, eax_on_timeout);
}

void NativeScheduler::wake_check_all() {
    // GIL held. EXACT mirror of the cooperative pick_ready() wake loop:
    // id-creation order, try_acquire ON BEHALF of the waiter (KIocp writes the
    // waiter's guest out-params in there), deliver result(), signal.
    // Native wakes on EACH notify; coop coalesces multiple SetEvents per
    // slice into one wake — a wake-count difference between the two
    // backends is by design, not a bug.
    gil::assert_held();
    for (auto& up : threads_) {
        GuestThread* t = up.get();
        if (t->state != S::Blocked || !t->wait_obj) continue;
        Waitable* w = static_cast<Waitable*>(t->wait_obj);
        if (w->try_acquire(t)) {
            NT* n = nt(t);
            n->delivered_eax = w->result();
            n->woken = true;
            t->wait_obj = nullptr;      // consumed; wait_common finishes state
            // Pure scheduling latency: from pthread_cond_signal() to the
            // woken thread's EFFECTIVE resumption (once it has reacquired
            // the GIL). Isolates the scheduler's half of "signal -> seen"
            // latency, as opposed to the guest's own polling cadence.
            if (d2rt_wakeprof) { n->t_signal_us = wx86_now_us(); ++d2rt_wake_sig; }
            pthread_cond_signal(&n->cv);
        }
    }
}

void NativeScheduler::notify(Waitable*) {
    gil::assert_held();
    wake_check_all();
}

// ---- runner / lifecycle -----------------------------------------------------

void NativeScheduler::seed_first_run(GuestThread* t) {
    // GIL held; current host thread's emu already bound. Mirrors the
    // cooperative run_slice() first-run seeding byte for byte.
    cpu_->set_fs_base(t->tib);
    uint32_t esp = t->stack_top;
    esp -= 4; cpu_->write_u32(esp, t->param);        // arg to thread proc
    esp -= 4; cpu_->write_u32(esp, br_->sentinel()); // return -> thread exit
    cpu_->set_reg(R_ESP, esp);
    cpu_->set_reg(R_EIP, t->entry);
    // Power-on EFLAGS. A fresh thread emu is memset-zeroed (EFLAGS=0); coop
    // loads 0x202 via load_context (the X86Context default).
    // set_reg(R_EFLAGS) also resets df=d_none — verified in cpu_box86.
    cpu_->set_reg(R_EFLAGS, 0x202);
    t->started = true;
}

void NativeScheduler::finish_thread(GuestThread* t, bool ok, const char* fault) {
    // GIL held. (The SEH dispatcher call below therefore runs GIL-held — that
    // is correct: it reads guest memory via shim-grade helpers.)
    // TerminateThread already marked this thread Finished from OUTSIDE and
    // set its exit code — touch NEITHER state nor exit_code, just wake
    // joiners. A fault surfacing here was raced by the terminate: name it in
    // ONE line (no full dump, no state touch) so the log never hides it.
    if (t->state == S::Finished) {
        if (!ok)
            std::printf("  [sched-native] fault SUPPRESSED after external terminate: thr %u eip=%08x addr=%08x\n",
                        t->id, cpu_->reg(R_EIP), cpu_->fault_addr());
        wake_check_all(); return;
    }
    if (!ok) {
        uint32_t code = cpu_->fault_code();
        int act = (code && fault_disp_) ? fault_disp_(t, code, cpu_->fault_addr()) : 0;
        if (act == 1) {                  // consumer asked to resume
            std::printf("  [sched-native] fault dispatcher asked to resume (act=1): not supported under this backend — thread leaks\n");
            return;
        }
        // Exit code: the precise one when the emulator could name the fault
        // (divide-by-zero, illegal instruction), the access-violation fallback
        // when it could not. Reporting every fault as 0xC0000005 discarded a
        // value the emulator had already computed.
        t->exit_code = code ? code : 0xC0000005u;
        faulted_ = true; stop_reason_ = fault ? fault : "fault";
        std::printf("  [sched-native] thread %u FAULT: %s EIP=0x%08x faultAddr=0x%08x ESP=0x%08x EAX=0x%08x\n",
                    t->id, stop_reason_, cpu_->reg(R_EIP), cpu_->fault_addr(),
                    cpu_->reg(R_ESP), cpu_->reg(R_EAX));
        dyn86_dump_xfer();
        // STACK SCAN: sample stack words that point INTO a loaded module — a
        // cheap call-stack hint when the frame is lost. Ranges come from the
        // bridge (each loaded module's base + size) rather than hardcoded
        // addresses, so this keeps working regardless of where a given
        // port's modules sit in memory.
        // This whole block ALSO goes through progress(): stdout does not
        // exist on Vita, so without it a console crash would leave only the
        // one-line summary below — no registers, no stack, no faulting
        // module.
        { uint32_t esp = cpu_->reg(R_ESP);
          const std::vector<std::pair<uint32_t,uint32_t>> mods =
              br_ ? br_->loaded_modules() : std::vector<std::pair<uint32_t,uint32_t>>();
          char l[160];
          std::snprintf(l, sizeof l, "  fault thr %u: EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x",
                        t->id, cpu_->reg(R_EAX), cpu_->reg(R_EBX), cpu_->reg(R_ECX), cpu_->reg(R_EDX),
                        cpu_->reg(R_ESI), cpu_->reg(R_EDI), cpu_->reg(R_EBP), esp);
          progress(l);
          // Faulting instruction bytes + where the EIP sits. On a dynarec fault
          // (unimplemented/illegal/div0) the EIP is the block the translator
          // choked on, so these bytes ARE the offending opcode; "hors module"
          // means generated / VirtualAlloc'd memory (wild execution) rather than
          // a missing opcode in real image code. This is the only handle on the
          // dump-less dynarec-fault crashes. read() fails soft (zeros) outside
          // the arena, so it can never fault here.
          { uint32_t fa = cpu_->fault_addr(); uint8_t op[16] = {0};
            cpu_->read(fa, op, sizeof op);
            uint32_t mb = 0; bool inMod = false;
            for (size_t k = 0; k < mods.size(); ++k)
                if (fa >= mods[k].first && fa - mods[k].first < mods[k].second) { inMod = true; mb = mods[k].first; break; }
            if (inMod)
                std::snprintf(l, sizeof l, "  fault opcode @%08x (module @%08x +0x%x): "
                    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                    fa, mb, (unsigned)(fa - mb), op[0],op[1],op[2],op[3],op[4],op[5],op[6],op[7],
                    op[8],op[9],op[10],op[11],op[12],op[13],op[14],op[15]);
            else
                std::snprintf(l, sizeof l, "  fault opcode @%08x (hors module -- genere/VirtualAlloc): "
                    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                    fa, op[0],op[1],op[2],op[3],op[4],op[5],op[6],op[7],
                    op[8],op[9],op[10],op[11],op[12],op[13],op[14],op[15]);
            progress(l); }
          for (size_t k = 0; k < mods.size(); ++k) {
              std::snprintf(l, sizeof l, "  fault module @%08x taille %08x%s", mods[k].first, mods[k].second,
                            (cpu_->reg(R_EIP) - mods[k].first < mods[k].second) ? "  <- EIP ICI" : "");
              progress(l); }
          int shown = 0;
          // Bounded by the thread's own stack: scanning a fixed 0x100 past ESP
          // read off the end of the region whenever the fault happened near the
          // stack top, and under an identity memory model that is a host fault
          // — the diagnostic meant to explain a crash was killing the process
          // before it could print the summary. Zero bounds (main, whose region
          // the scheduler does not own) keep the old fixed window.
          const uint32_t span = (t->stack_top && esp >= t->stack_base && esp < t->stack_top)
                              ? (t->stack_top - esp) : 0x100u;
          for (uint32_t off = 0; off < (span < 0x100u ? span : 0x100u); off += 4) {
              uint32_t v = cpu_->read_u32(esp + off);
              for (size_t k = 0; k < mods.size(); ++k)
                  if (v >= mods[k].first && v - mods[k].first < mods[k].second) {
                      std::printf("      [esp+0x%02x] 0x%08x  (module @%08x +0x%x)\n",
                                  off, v, mods[k].first, (unsigned)(v - mods[k].first));
                      if (shown++ < 16) {
                          std::snprintf(l, sizeof l, "  fault pile [esp+0x%02x] 0x%08x (module @%08x +0x%x)",
                                        (unsigned)off, v, mods[k].first, (unsigned)(v - mods[k].first));
                          progress(l); }
                      break; } } }
        char m[128]; std::snprintf(m, sizeof m, "NATIVE FAULT thr %u eip=%08x addr=%08x",
                                   t->id, cpu_->reg(R_EIP), cpu_->fault_addr());
        progress(m);
    } else {
        // A GENUINE thread exit stopped AT the Bridge sentinel (trap_handler
        // returned false there): EAX is the thread proc's return value. A
        // clean stop anywhere else only reaches here during shutdown teardown
        // (async quit=1 broadcast, no break marker) — EAX is then
        // mid-computation garbage: report exit_code 0, don't pretend the
        // thread returned.
        bool genuine = cpu_->reg(R_EIP) == br_->sentinel();
        t->exit_code = genuine ? cpu_->reg(R_EAX) : 0;
    }
    cpu_->save_context(t->ctx);                      // final honest snapshot
    t->state = S::Finished;
    wake_check_all();                                // KThread joiners
}

// ---- Second core: knobs (read ONCE) and pinning by the thread itself ------
// WX86_COEUR_SERVEUR=<c>: 0/absent = OFF (default topology, byte for byte);
// 1..3 = the server thread runs on USER_c (3 = 4th core, CapUnlocker). Other
// guest threads stay on USER_0. Identification: WX86_COEUR_SERVEUR_ID=<id>
// (takes priority), otherwise the guest ENTRY ADDRESS the consumer supplied
// via set_server_thread_entry() — the engine resolves no module names, so it
// has no RELATIVE-address knob either: an RVA is only meaningful relative to
// a module, and it's the port that knows which one. Under qemu:
// WX86_QEMU_MONOCOEUR=<cpu> emulates the console topology via
// sched_setaffinity (main + runners on <cpu>, server on <cpu>+c) — without
// it, qemu leaves its threads free to run on any host core, the default for
// native qemu runs.
static int coeur2_core() {
    static int c = -1;
    if (c < 0) { const char* e = getenv("WX86_COEUR_SERVEUR"); if (!e) e = getenv("D2_COEUR_SERVEUR"); c = (e && *e) ? atoi(e) : 0; if (c < 0 || c > 3) c = 0; }
    return c;
}
static uint32_t coeur2_id() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("WX86_COEUR_SERVEUR_ID"); if (!e) e = getenv("D2_COEUR_SERVEUR_ID"); v = (e && *e) ? atoi(e) : 0; if (v < 0) v = 0; }
    return (uint32_t)v;
}
static int qemu_monocoeur() {           // -1 = knob absent (aucune affinite hote)
    static int v = -2;
    if (v < -1) { const char* e = getenv("WX86_QEMU_MONOCOEUR"); if (!e) e = getenv("D2_QEMU_MONOCOEUR"); v = (e && *e) ? atoi(e) : -1; if (v < -1) v = -1; }
    return v;
}

bool NativeScheduler::is_server_thread(GuestThread* t) {
    if (!t) return false;
    if (const uint32_t id = coeur2_id()) return t->id == id;
    // server_entry_ is an ABSOLUTE guest address, set by the consumer. 0 =
    // none was set: nobody is the server, and the topology stays the
    // default. The engine does not try to guess.
    return server_entry_ && t->entry == server_entry_;
}

void NativeScheduler::pin_runner(GuestThread* t, bool is_main) {
    const int c = coeur2_core();
    const bool srv = !is_main && c > 0 && is_server_thread(t);
#ifdef __vita__
    SceUID self = sceKernelGetThreadId();
    if (!is_main) nt(t)->sce_uid = (int32_t)self;   // starvation net: diagnostics + a possible priority-boost fallback
    // Stage-1 topology: every guest runner pinned on USER_0, priority =
    // present/watchdog (0x10000100), set from inside the thread itself.
    // KERNEL BEHAVIOR for this topology: RUN-TO-BLOCK — the Vita kernel does
    // NOT timeslice equal-priority threads on one core; the first runner
    // keeps the core until it blocks. Anti-starvation nets are therefore
    // REQUIRED before any online native run (solo stays testable: guest
    // code yields through its own imports).
    // Second core: the server thread (and only it) runs on USER_c; SCE_KERNEL_CPU_MASK_USER_n = 0x10000 << n.
    const int mask = srv ? (int)(0x10000u << c) : (int)SCE_KERNEL_CPU_MASK_USER_0;
    const int rc = sceKernelChangeThreadCpuAffinityMask(self, mask);
    if (!is_main) sceKernelChangeThreadPriority(self, 0x10000100);
    else
        // The guest main thread is registered in the core map as a WITNESS
        // for USER_0: the "coeurs:" line needs at least one thread whose
        // expected location is known, so the other lines can be read against
        // it. Runners are not registered — they are born and die, the
        // registry is fixed.
        wx86_vita_core_register("invite-main", (int)self, (unsigned)SCE_KERNEL_CPU_MASK_USER_0, rc);
    if (srv) {
        // The server IS registered: the coeurs: line will publish its d=
        // (last core it ran on), the only proof that it RUNS elsewhere (the
        // affinity mask reads back as 0 on this firmware).
        wx86_vita_core_register("serveur", (int)self, (unsigned)mask, rc);
        char m[160];
        std::snprintf(m, sizeof m, "coeur2: fil %u (entree=%08x) epingle sur USER_%d masque=0x%x rc=0x%08x — fil serveur sur son coeur",
                      t->id, t->entry, c, (unsigned)mask, (unsigned)rc);
        progress(m); std::printf("  %s\n", m);
    }
    // VM domain is PER THREAD (full explanation in mman_vita.c): this thread
    // is about to TRANSLATE, i.e. WRITE the JIT arena; without its own
    // OpenVMDomain (DACR = thread context), its first allocBlock is a
    // deterministic Data abort. The permission's scope is the THREAD, not
    // the core, so it follows the thread onto USER_c.
    dyn86_vita_open_vm_thread();
#else
    // qemu / host: console topology emulation, opt-in only.
    const int base = qemu_monocoeur();
    if (base >= 0) {
        const int cpu = srv ? base + c : base;
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
        const int rc = sched_setaffinity(0, sizeof set, &set);
        char m[160];
        std::snprintf(m, sizeof m, "coeur2: fil %u (entree=%08x) affinite hote cpu=%d rc=%d%s%s",
                      t->id, t->entry, cpu, rc, rc ? " (ECHEC : fil libre)" : "",
                      srv ? " — fil serveur sur son coeur" : "");
        std::printf("  %s\n", m); std::fflush(stdout);
    } else if (srv) {
        std::printf("  coeur2: fil %u (entree=%08x) reconnu SERVEUR — sans WX86_QEMU_MONOCOEUR aucune affinite hote (qemu : fils deja libres)\n",
                    t->id, t->entry);
    }
#endif
}

void* NativeScheduler::runner_tramp(void* p) {
    // The one pthread start argument carries the GuestThread*; the scheduler
    // singleton is reached through g_native_sched (sched_native.h — assigned
    // by make_scheduler before any create_thread).
    GuestThread* t = static_cast<GuestThread*>(p);
    g_native_sched->runner(t);
    return nullptr;
}

void NativeScheduler::runner(GuestThread* t) {
    // Pinning + priority + VM domain, BY THE THREAD ITSELF (the server
    // thread may run on another core — see pin_runner, knob D2_COEUR_SERVEUR).
    pin_runner(t, false);
    NT* n = nt(t);
    cpu_->thread_emu_bind(n->emu);
    t_cur = t;
    // GIL observability: this thread registers the interval of ITS OWN host
    // stack, BEFORE its first GIL acquire. This is what replaces the
    // pthread_self() that lock() used to pay for on every trap (gil.h).
    // `ici` is never read — only its address matters, and it sits a few
    // hundred bytes below the top (two frames: runner_tramp then runner).
    int gslot;
    { char ici; uintptr_t hi = (uintptr_t)&ici;
      gslot = gil::register_stack(hi - kRunnerStackDeclared, hi, t->id); }
    // Unregisters at EXIT, by the thread itself and after its last GIL
    // acquire: the second half of gil.h's invariant. Without it, an
    // unregistered thread later running on this recycled stack would
    // inherit the dead thread's label — the only false name this mechanism
    // can produce. RAII to cover BOTH exits of runner().
    struct Unreg { int s; ~Unreg() { gil::unregister_stack(s); } } unreg{gslot};
    gil::lock();
    if (t->state == S::New && !shutdown_) gil::mark_released();   // observability
    while (t->state == S::New && !shutdown_)         // CREATE_SUSPENDED
        pthread_cond_wait(&n->cv, gil::mutex());
    gil::mark_owned();                               // see wait_common
    if (shutdown_) { t->state = S::Finished; n->runner_done = true; gil::unlock(); return; }
    seed_first_run(t);
    t->state = S::Running;
    run_guest(t);
    n->runner_done = true;                           // no more guest code will run on this stack
    gil::unlock();
}

void NativeScheduler::run_guest(GuestThread* t) {
    // GIL held on entry. Translated code must run WITHOUT the GIL: release
    // around cpu_->run() — the trap dispatch re-takes it per shim (gil::Guard
    // in cpu_box86.cpp). No cooperative yields on this backend: waits block
    // inside the shim.
    uint32_t eip = cpu_->reg(R_EIP);
    const char* fault = nullptr;
    bool ok;
    {
        gil::Release r;
        for (;;) {
#ifdef PROF_COUNTERS
            // Guest-time accounting, MIRRORS the cooperative scheduler's
            // (sched_cooperative.cpp). Without it, prof::run_ns stays zero
            // under the native scheduler and the frame profile's "budget:"
            // line reports dynarec=0.0, dumping everything into "outside
            // run". run_ns is INCLUSIVE (translated code + shim bodies
            // executed within the slice), so pure dynarec = run - trap.
            const uint64_t prof_t0 = d2rt::prof::now_ns();
#endif
            // Feeds dyn86_jp_run_us under the NATIVE scheduler too, not just
            // under coop, still gated behind the dyn86_jitprof knob. Without
            // this it stays at 0 under the native scheduler, and the frame
            // profiler reports "run=0ms" even while the thread is actually
            // executing translated code — a zero here would look like valid
            // data instead of an unfed counter.
            const uint64_t run_t0 = dyn86_jp_now_us();
            ok = cpu_->run(eip, &fault);
            dyn86_jp_run_us += dyn86_jp_now_us() - run_t0;
#ifdef PROF_COUNTERS
            { const uint64_t dt = d2rt::prof::now_ns() - prof_t0;
              d2rt::prof::run_ns += dt; t->prof_ns += dt; }
#endif
            if (!ok) break;                              // fault
            if (cpu_->take_limit_hit()) {                // stop broadcast landed
                if (shutdown_) break;                    // shutdown takes priority
                // Starvation net: a poke from the heartbeat zeroed THIS
                // thread's budget → one bounded nap per epoch. This runs
                // INSIDE the gil::Release block above: the nap blocks
                // outside the GIL by construction (the GIL stays acquirable
                // by the starved thread). A SPONTANEOUS expiry of the
                // 0x7FFFFFFF budget falls back to epoch==fam_ack → immediate
                // resume, identical to the existing "spurious resume" behavior.
                uint32_t e = fam_epoch_.load(std::memory_order_relaxed);
                NT* n = nt(t);
                if (fam_on_.load(std::memory_order_relaxed) && e != n->fam_ack) {
                    n->fam_ack = e;
                    fam_note_nap(t, cpu_->reg(R_EIP));       // log + counter
                    fam_nap();                               // DelayThread/nanosleep — OUTSIDE the GIL
                }
                eip = cpu_->reg(R_EIP); continue;        // spurious: resume
            }
            if (br_->take_yield()) {                     // advisory yield (SwitchToThread)
                eip = cpu_->reg(R_EIP); sched_yield(); continue;
            }
            // Clean stop: ONLY a Bridge-sentinel return is a real thread exit
            // (trap_handler returned false at the sentinel slot, EIP == sentinel).
            // An async quit=1 with no marker (another thread's shutdown-grade
            // request_stop) must not "finish" a live thread: it resumes
            // unless shutdown is actually in progress.
            if (cpu_->reg(R_EIP) == br_->sentinel()) break;   // genuine finish
            if (shutdown_) break;
            eip = cpu_->reg(R_EIP); continue;
        }
    }
    finish_thread(t, ok, fault);
    t->slices++;
}

// ---- Starvation-avoidance net: mechanism helpers ---------------------------

// Real nap: the ONLY reliable scheduling point under run-to-block (blocking
// = yielding the core to the next ready thread). Call OUTSIDE the GIL only.
void NativeScheduler::fam_nap() {
#ifdef __vita__
    sceKernelDelayThread(fam_nap_us_);
#else
    timespec ts{ (time_t)(fam_nap_us_ / 1000000u), (long)(fam_nap_us_ % 1000000u) * 1000L };
    nanosleep(&ts, nullptr);
#endif
}

// Counter + RATE-LIMITED log: 1st nap logged, then 1 in kFamineLogEvery — a
// long starvation period must not flood boot_progress.
// PLUS: each thread's 1st nap is logged too (NT::fam_napped) — otherwise a
// thread starved between two multiples of the global modulo would stay
// invisible. This is the line that NAMES the spinner (the EIP at the
// translation-seam exit is roughly the hot loop). Called outside the GIL by
// the runner itself: progress() (an appending fopen on Vita) tolerates
// concurrency (the same regime as the watchdog); also to stdout for the
// qemu bench (progress is a no-op there).
void NativeScheduler::fam_note_nap(GuestThread* t, uint32_t eip) {
    uint32_t nth = fam_naps_.fetch_add(1, std::memory_order_relaxed) + 1;
    NT* n = nt(t);
    bool first_of_thread = !n->fam_napped;
    n->fam_napped = true;
    if (!first_of_thread && (nth % kFamineLogEvery) != 0) return;
    char m[64];
    std::snprintf(m, sizeof m, "famine: sieste thr %u eip=%08x", t->id, eip);
    std::printf("  %s\n", m);
    progress(m);
}

// ---- Starvation-detector heartbeat: the detector that ARMS the mechanism --

// Heartbeat sleep — same pair of calls as fam_nap(), but a free-form
// duration: the heartbeat sleeps in <=50ms slices to stay responsive to
// teardown (its join happens before the runners' bounded join).
static void fam_sleep_us(uint32_t us) {
#ifdef __vita__
    sceKernelDelayThread(us);
#else
    timespec ts{ (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, nullptr);
#endif
}

void NativeScheduler::arm_famine(const volatile int* frames, uint32_t window_ms,
                                 uint32_t nap_us) {
    fam_frame_     = frames;
    fam_window_ms_ = window_ms ? window_ms : kFamineWindowMsDefault;
    fam_nap_us_    = nap_us ? nap_us : kFamineNapUsDefault;
    // Arms the translation-seam mechanism: epoch still 0 = already
    // acknowledged by every thread by construction — no nap happens until
    // something is actually detected.
    fam_on_.store(true, std::memory_order_relaxed);
}

void* NativeScheduler::fam_tramp(void* self) {
    static_cast<NativeScheduler*>(self)->fam_beat();
    return nullptr;
}

// Detector loop: every fam_window_ms_, read wakes_total_ and the frame
// counter; suspicion = BOTH frozen over the window. On suspicion: a RAW GIL
// trylock (same pattern as dump_threads_to) — NEVER gil::lock() here, and
// therefore no call to any method under gil::assert_held() (thread_by_id,
// live_thread_ids...): threads_ is walked directly instead.
void NativeScheduler::fam_beat() {
#ifdef __vita__
    // Vitality self-report. A thread pinned to a core from the moment it is
    // created never executes its own pinning code, so self-pinning from
    // inside the heartbeat cannot work — pinning is done by the CREATOR
    // before start (see run()) instead. A "mask read back" check is not
    // usable either: sceKernelGetThreadCpuAffinityMask returns 0 even for a
    // thread pinned successfully on this firmware, so it would print a
    // FALSE "INVARIANT VIOLATED" alarm on every boot. This line is the
    // replacement: printed BY the heartbeat, it is proof that it is running
    // (a dead thread does not write), and its absence is detectable
    // post-mortem (an INERT stamp from rt_boot). The priority read back is
    // informative only — the actual resolved scheduling class (0x10000100)
    // — not an invariant.
    // "USER_2" hardcoded here would lie as soon as D2_COEURS changes: the
    // core announced is whichever one the knob actually requested.
    { const int fm = wx86_vita_core_mask(2);
      char m[120];
      std::snprintf(m, sizeof m, "famine: battement arme (Sce brut, USER_%d createur [D2_COEURS], prio relue=%d)",
                    fm == SCE_KERNEL_CPU_MASK_USER_0 ? 0
                      : fm == SCE_KERNEL_CPU_MASK_USER_1 ? 1 : 2,
                    sceKernelGetThreadCurrentPriority());
      progress(m); }
#endif
    // Cross-core volatile reads, 64-bit tearing tolerated — same regime as
    // the watchdog heartbeat (sched_native.h, wakes_total_ field). A torn
    // read misses ONE window (mismatched values => no detection): the safe
    // failure mode.
    const volatile uint64_t* wp = (const volatile uint64_t*)&wakes_total_;
    uint64_t last_w = *wp;
    int      last_f = fam_frame_ ? *fam_frame_ : -1;
    uint64_t next   = now_ms() + fam_window_ms_;
    // GIL observability, state PRIVATE to this thread: the heartbeat samples
    // the GIL on EVERY window (not just on suspicion), otherwise acquisition
    // age would start counting from the first observed freeze instead of
    // from the acquisition itself. Two samples with the same acquisition
    // counter mean the same acquisition.
    uint32_t gil_acq = 0; bool gil_held = false; uint64_t gil_since = 0;
    while (!fam_stop_.load(std::memory_order_relaxed)) {
        uint64_t now = now_ms();
        if (now < next) {
            uint64_t left = next - now;
            fam_sleep_us((uint32_t)((left > 50 ? 50 : left) * 1000));
            continue;
        }
        next = now + fam_window_ms_;
        if (shutdown_) break;               // teardown: no more detection, the join is coming
        // ---- "fils:" per-guest-thread translated block counts, sampled
        // every 10s window. When frame rate drops, it can look like the
        // main thread is simply waiting on something (blocked, or with free
        // time in its step) while in fact ANOTHER guest thread is holding
        // the core. No existing signal showed that: this is the per-thread
        // CPU proxy — the delta of executed blocks (thread_emu_blocks), the
        // same counter as the detections' run= field, but sampled
        // periodically.
        {
            static uint64_t fils_next = 0; static uint32_t prevBlk[16] = {0}; static uint32_t prevId[16] = {0};
            static uint32_t prevTr[16] = {0}, prevCo[16] = {0}, prevWu[16] = {0};
            static bool g_filstat_seen = false;
            if (cpu_ && now >= fils_next) {
                fils_next = now + 10000;
                char line[420]; int w = std::snprintf(line, sizeof line, "fils:");
                { uint32_t a = 0; g_filstat_seen = cpu_->thread_emu_filstat(nullptr, &a, nullptr, nullptr); }
                uint32_t fn = flat_n_.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < fn && i < kFlatMax && i < 16; ++i) {
                    GuestThread* g = flat_[i];
                    if (!g || g->state == S::Finished || !g->native) continue;
                    uint32_t blocks = 0;
                    if (!cpu_->thread_emu_blocks(static_cast<NT*>(g->native)->emu, &blocks)) continue;
                    const uint32_t d = (prevId[i] == g->id) ? blocks - prevBlk[i] : 0;
                    prevBlk[i] = blocks;
                    const char st = g->state == S::Running ? 'R' : g->state == S::Blocked ? 'B' : g->state == S::Ready ? 'P' : 'N';
                    if (w < (int)sizeof line - 32)
                        w += std::snprintf(line + w, sizeof line - (size_t)w, " %u=%u%c", g->id, d / 10, st);
                    // D2_FILSTAT=1: /t<traps/s>/c<contended acquires/s>/w<ms of GIL wait per s>
                    // — the thread's trap density and what shim serialization costs it.
                    uint32_t tr = 0, co = 0, wu = 0;
                    if (cpu_->thread_emu_filstat(static_cast<NT*>(g->native)->emu, &tr, &co, &wu)) {
                        const uint32_t dt = (prevId[i] == g->id) ? tr - prevTr[i] : 0;
                        const uint32_t dc = (prevId[i] == g->id) ? co - prevCo[i] : 0;
                        const uint32_t dw = (prevId[i] == g->id) ? wu - prevWu[i] : 0;
                        prevTr[i] = tr; prevCo[i] = co; prevWu[i] = wu;
                        if (w < (int)sizeof line - 40)
                            w += std::snprintf(line + w, sizeof line - (size_t)w, "/t%u/c%u/w%u", dt / 10, dc / 10, dw / 10000);
                    }
                    prevId[i] = g->id;
                }
                std::snprintf(line + w, sizeof line - (size_t)w, " (blocs/s par fil%s)", g_filstat_seen ? " ; /t traps/s /c contendues/s /w ms-attente-GIL/s" : "");
                progress(line);
                std::printf("  %s\n", line); std::fflush(stdout);   // qemu: progress is a no-op there
            }
        }
        gil::Probe gp; gil::probe(&gp);     // lock-free read — never blocks
        if (!(gp.held && gil_held && gp.acq == gil_acq)) gil_since = now;   // new acquisition
        gil_acq = gp.acq; gil_held = gp.held;
        uint64_t w = *wp;
        int      f = fam_frame_ ? *fam_frame_ : -1;
        bool frozen = (w == last_w) && (f == last_f);   // suspicion = BOTH frozen
        last_w = w; last_f = f;
        if (!frozen) continue;
        if (pthread_mutex_trylock(gil::mutex()) != 0) {
            // GIL busy = a shim is making progress — not TOTAL starvation:
            // skipping the window is CORRECT.
            //
            // But the skip itself must be VISIBLE: if this branch stayed
            // silent, the absence of a "famine" line would prove nothing
            // (the net could give up for minutes in a row without a trace)
            // — exactly the kind of silent failure this diagnostic exists
            // to avoid. It reports at a rate bounded in TIME
            // (kFamineSkipReportMs, justified in sched_native.h): the 1st
            // skip immediately, then at most one summary every 5s.
            uint32_t skips = fam_gil_skips_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skips == 1 || now - fam_skip_last_ms_ >= kFamineSkipReportMs) {
                uint32_t since = skips - fam_skip_last_n_;
                fam_skip_last_ms_ = now; fam_skip_last_n_ = skips;
                char who[24];
                if (!gp.held)          std::snprintf(who, sizeof who, "libre-au-releve");
                else if (gp.owner_tag) std::snprintf(who, sizeof who, "thr %u", gp.owner_tag);
                else                   std::snprintf(who, sizeof who, "un fil hote");
                char line[420];   // + run= field (up to 10 runners): the line must not be truncated
                int lw = std::snprintf(line, sizeof line,     // lw: do NOT shadow `w` (the wakes count)
                    // The title says what ACTUALLY happened (the trylock
                    // failed), and the holder is explicitly timestamped "at
                    // sample time": the gil::probe sample is taken at the
                    // TOP of the window, before the trylock, so it can
                    // legitimately say "free" even though the trylock failed
                    // a few microseconds later. A title that instead claimed
                    // "GIL busy" would then contradict its own field.
                    "famine: fenetre SAUTEE (trylock GIL echoue) (sautees=%u, +%u depuis le dernier resume) "
                    "detenteur-au-releve=%s acq=%u%s%s — le filet ne desaffame pas pendant ce temps",
                    skips, since, who, gp.acq,
                    gp.in_shim ? " dans=" : "",
                    gp.in_shim ? (gp.shim ? gp.shim : "?") : "");
                // Per-runner liveness on the SAME line: this is what tells
                // starvation (one counter moves) apart from deadlock (none
                // do) on targets without an alive: line (qemu).
                if (lw > 0 && (unsigned)lw < sizeof line)
                    vivacity_field(line + lw, (unsigned)(sizeof line - lw));
                std::printf("  %s\n", line);
                progress(line);
            }
            // ... EXCEPT if the holder has stopped moving. "A shim is making
            // progress" is an INTERPRETATION: it stops holding once the same
            // acquisition (same acq counter) lasts longer than kGilStuckMs.
            // The heartbeat SAYS so, and still does NOTHING more — no wake,
            // no poke, no nap: the fix is out of reach by construction, and
            // that is exactly what needs to be measured before touching the
            // net.
            uint64_t hold = gp.held ? (now - gil_since) : 0;
            if (gp.held && hold >= kGilStuckMs) {
                uint32_t nst = fam_gil_stuck_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (nst == 1 || (nst % kFamineLogEvery) == 0) {   // same cadence as the other log streams
                    char who[24];
                    if (gp.owner_tag) std::snprintf(who, sizeof who, "thr %u", gp.owner_tag);
                    else              std::snprintf(who, sizeof who, "un fil hote");
                    char line[240];
                    if (gp.in_shim)
                        std::snprintf(line, sizeof line,
                            "famine: GIL tenu par %s depuis >=%llu ms dans %s "
                            "(trap=%08x acq=%u sautees=%u) — desaffamage impossible",
                            who, (unsigned long long)hold, gp.shim ? gp.shim : "?",
                            gp.shim_va, gp.acq, skips);
                    else
                        std::snprintf(line, sizeof line,
                            "famine: GIL tenu par %s depuis >=%llu ms hors shim "
                            "(acq=%u sautees=%u) — desaffamage impossible",
                            who, (unsigned long long)hold, gp.acq, skips);
                    std::printf("  %s\n", line);
                    progress(line);
                }
            }
            continue;
        }
        // Under lock: corroboration — count by state + expired timed waits
        // (NT::deadline_ms, on the now_ms()/MONOTONIC base, read here only
        // under the (try)lock — contract in sched_native.h)...
        uint32_t running = 0, expired = 0;
        uint64_t nowl = now_ms();
        for (auto& up : threads_) {
            GuestThread* g = up.get();
            if (g->state == S::Running) ++running;
            else if (g->state == S::Blocked) {
                NT* n = static_cast<NT*>(g->native);
                if (n->deadline_ms && nowl > n->deadline_ms + kFamineExpiredMarginMs)
                    ++expired;
            }
        }
        // Detection number drawn NOW: it decides whether the line will be
        // printed, and therefore whether the liveness sample needs to be
        // taken at all — at a 20ms test-bench window this loop can run up
        // to 50 times/s, so skipping unnecessary work here matters.
        uint32_t nth = fam_detections_.fetch_add(1, std::memory_order_relaxed) + 1;
        bool logit = (nth == 1) || (nth % kFamineLogEvery) == 0;
        // Per-runner liveness SAMPLED NOW, before the poke. The counter is
        // monotonic, so the poke no longer skews it — but it DOES PUT
        // THREADS BACK IN MOTION: a sample taken after would describe the
        // net, not the freeze. Measured here, printed further down.
        char viv[224]; viv[0] = 0;
        if (logit) vivacity_field(viv, sizeof viv);
        // ... then the CONTRACTUAL ORDER (see sched_native.h): publish the
        // epoch BEFORE poking the budgets — the translation seam reads the
        // EPOCH ...
        fam_epoch_.fetch_add(1, std::memory_order_release);
        // ... and pokes ONLY Running threads (Blocked ones are woken by the
        // kernel already; sparing their emu avoids a spurious nap right at
        // wake-up). nt->emu nullptr = the base emu (main) — same contract
        // as the mechanism above.
        for (auto& up : threads_) {
            GuestThread* g = up.get();
            if (g->state != S::Running) continue;
            cpu_->nudge_thread_budget(static_cast<NT*>(g->native)->emu);
        }
        // Detection log stream, rate-limited by kFamineLogEvery, independent
        // of the naps stream. Line formatted under the lock (cheap), emitted
        // after.
        char line[480];
        if (logit)
            // The run= field carries the PRE-POKE sample above: at the
            // exact moment the freeze is observed, it says who is still
            // progressing (starvation) or nobody (deadlock).
            std::snprintf(line, sizeof line,
                "famine: detection #%u (fenetre=%ums frames=%d wakes=%llu running=%u expirees=%u sautees-gil=%u)%s",
                nth, fam_window_ms_, f, (unsigned long long)w, running, expired,
                fam_gil_skips_.load(std::memory_order_relaxed), viv);
        pthread_mutex_unlock(gil::mutex());
        if (logit) { std::printf("  %s\n", line); progress(line); }  // stdout (qemu) + boot_progress (Vita)
    }
}

void NativeScheduler::exit_current(uint32_t code) {
    // ExitThread arrives via redirect_next(sentinel) and lands in run_guest's
    // sentinel finish; TerminateThread marks the TARGET in its shim. This entry
    // point only records the code — NEVER cpu_->request_stop(): that broadcast
    // is shutdown-grade and monotonic in native (see the cpu.h contract).
    if (t_cur) t_cur->exit_code = code;
}

void NativeScheduler::run() {
    // MAIN runs on the calling host thread (spec: run() blocks until exit).
    // Starvation-detector heartbeat: started HERE, only if arm_famine() has
    // been called (rt_boot, native branch, D2_FAMINE). Any failure leaves
    // the net inert — reported, never silent.
    if (fam_on_.load(std::memory_order_relaxed)) {
#ifdef __vita__
        // A RAW Sce thread rather than a pte one: pte creates threads at Sce
        // priority 191, the LEAST urgent, and discards sceKernelStartThread's
        // return code — a heartbeat created that way could be starved from
        // birth with no way to tell. Instead: explicit priority 0x10000100
        // (the same class as the present/watchdog thread, known to stay
        // responsive), pinned to USER_2 by the CREATOR before start (USER_2
        // is the core known to behave correctly here). Contention is
        // negligible: the present thread sleeps in WaitSema between frames
        // and the heartbeat only runs for a few microseconds per window — a
        // nap yields the core to an equal-priority ready thread. create/pin/
        // start return codes are ALL checked: a failed pin is NOT fatal (the
        // thread still starts and will report its actual position); a
        // failed create/start makes the net INERT, and that is reported.
        // Running fam_beat's pthread-based body from a non-pte thread is
        // safe: it only uses trylock/unlock on the GIL (no pte TLS), sleep
        // already goes through sceKernelDelayThread, and progress/printf are
        // already exercised from the watchdog under the same constraints.
        // 64 KiB stack: fam_beat formats strings and calls progress() (which
        // does an fopen) — a 4x margin over the comparable watchdog stack.
        auto tramp = [](SceSize, void* argp) -> int {
            // SELF-PINNING (wx86_vita_pin_self): the mask set by the
            // creator does not survive past the thread's start, so it must
            // be reapplied here, by the thread itself.
            {
                const unsigned fm = (unsigned)wx86_vita_core_mask(2);
                unsigned relu = 0; const int rc = wx86_vita_pin_self((int)fm, &relu);
                char m[112]; std::snprintf(m, sizeof m,
                    "famine: battement auto-epinglage masque=0x%x rc=0x%08x relu=0x%x", fm, (unsigned)rc, relu);
                wx86_vita_progress_c(m);
            }
            (*(NativeScheduler* const*)argp)->fam_beat();
            return 0; };
        SceUID th = sceKernelCreateThread("d2_famine", tramp, 0x10000100, 64 * 1024, 0, 0, nullptr);
        if (th < 0) {
            fam_on_.store(false, std::memory_order_relaxed);
            char m[112];
            std::snprintf(m, sizeof m, "famine: battement CreateThread KO (rc=0x%08x) — filet INERTE", (unsigned)th);
            std::printf("  %s\n", m); progress(m);
        } else {
            // D2_COEURS position 2. Default "2" = USER_2, consistent with
            // the engine's default topology. This is the ONE auxiliary
            // thread that must NEVER land on USER_0: the kernel's
            // RUN-TO-BLOCK behavior on a single core means that if a guest
            // runner on USER_0 stops yielding, the heartbeat exists
            // precisely to catch that — so it cannot share that same core.
            // wx86_vita_core_mask accepts the value but calls it out in the
            // log if it's wrong.
            const unsigned fmask = (unsigned)wx86_vita_core_mask(2);
            int prc = sceKernelChangeThreadCpuAffinityMask(th, (int)fmask);
            wx86_vita_core_register("battement", (int)th, fmask, prc);
            if (prc < 0) {
                char m[176];
                std::snprintf(m, sizeof m,
                    "famine: epinglage masque 0x%x KO (rc=0x%08x) — battement demarre non epingle (temoin : absence de la ligne 'battement arme')", fmask, (unsigned)prc);
                progress(m);
            }
            NativeScheduler* self_p = this;
            int src = sceKernelStartThread(th, sizeof self_p, &self_p);
            if (src < 0) {
                fam_on_.store(false, std::memory_order_relaxed);
                sceKernelDeleteThread(th);   // a created-but-never-started thread keeps its TCB + stack
                char m[112];
                std::snprintf(m, sizeof m, "famine: battement StartThread KO (rc=0x%08x) — filet INERTE", (unsigned)src);
                std::printf("  %s\n", m); progress(m);
            } else { fam_sce_th_ = th; fam_started_ = true; }
        }
#else
        int frc = pthread_create(&fam_th_, nullptr, fam_tramp, this);
        if (frc == 0) fam_started_ = true;
        else {
            fam_on_.store(false, std::memory_order_relaxed);
            char m[96];
            std::snprintf(m, sizeof m, "famine: battement pthread_create ECHEC (rc=%d) — filet INERTE", frc);
            std::printf("  %s\n", m); progress(m);
        }
#endif
    }
    GuestThread* t = main_;
    pin_runner(t, true);   // USER_0 + "invite-main" witness + VM domain (main translates too)
    NT* n = nt(t);
    cpu_->thread_emu_bind(nullptr);                  // base emu
    t_cur = t;
    // GIL observability: main runs on the calling host thread, never through
    // pthread_create. It registers its stack like the runners do, otherwise
    // it would be reported as "host". Registered HERE and not in set_main():
    // run() is the frame under which the rest of main executes, and only
    // ONE registration per thread is allowed (two overlapping intervals make
    // the label ambiguous, hence silent — gil.h). Accepted consequence: any
    // gil::Guard taken by rt_boot's boot-time self-tests BEFORE run() runs on
    // a not-yet-registered main; assert_held() there only verifies that the
    // GIL is held, and claims nothing more.
    // No symmetric unregistration here (unlike runner()): the main thread
    // never dies, so its stack is never recycled under another thread — the
    // hole unregister_stack() closes does not exist for it.
    { char ici; uintptr_t hi = (uintptr_t)&ici;
      gil::register_stack(hi - kMainStackDeclared, hi, t->id); }
    gil::lock();
    n->is_main = true;
    seed_first_run(t);
    t->state = S::Running;
    run_guest(t);
    // Main exited: process teardown (main-thread exit ends the process,
    // like on Windows). Stop the workers, join with a bounded wait, report
    // stragglers.
    // A recorded fault outranks both: overwriting it here reported a faulting
    // run as a normal exit, and the one-line summary a reader sees first is
    // built from this string.
    if (!faulted_) stop_reason_ = shutdown_ ? "shutdown requested" : "main exited";
    request_shutdown();
    // Snapshot the GuestThread* set UNDER the GIL before unlocking — the
    // vector cannot grow past shutdown_ (create_thread refuses), but iterating
    // the live vector unlocked would still race the last in-flight push_back's
    // reallocation. The cells themselves (unique_ptr heap objects) never move,
    // so raw pointers taken here are safe to walk unlocked below.
    std::vector<GuestThread*> snap;
    snap.reserve(threads_.size());
    for (auto& up : threads_) snap.push_back(up.get());
    gil::unlock();
    // Starvation-detector heartbeat stopped (flag + join) BEFORE the
    // runners' bounded join; it sleeps in <=50ms slices, so the join is
    // short. One LAST window can slip in between gil::unlock() above and the
    // fam_stop_ store (the heartbeat checks shutdown_ at the top of each
    // window, but the race remains possible) — consequences are null: the
    // budgets are already zeroed (request_stop) and the seam checks
    // shutdown_ BEFORE napping; at worst, one thread that hasn't exited yet
    // takes a 1ms nap.
    if (fam_started_) {
        fam_stop_.store(true, std::memory_order_relaxed);
#ifdef __vita__
        // A BOUNDED WaitThreadEnd (2s, matching the runners' bounded join
        // below) + DeleteThread (WaitThreadEnd frees neither the TCB nor the
        // stack on its own). A short join is expected: sleeping in <=50ms
        // slices, heartbeat pinned to USER_2 where everything else blocks.
        // The bound covers the compounded corner case of a failed pin + a
        // captive core + a wedged runner (a triple failure) — a bounded,
        // reported leak beats a silent infinite teardown. On expiry: NO
        // DeleteThread (a non-dormant thread means an error return code —
        // an accepted leak, not a disguised cleanup).
        SceUInt fam_to = 2 * 1000 * 1000;
        if (sceKernelWaitThreadEnd(fam_sce_th_, nullptr, &fam_to) < 0)
            progress("famine: battement ne sort pas (2 s) — fil abandonne (fuite bornee)");
        else
            sceKernelDeleteThread(fam_sce_th_);
        fam_sce_th_ = -1;
#else
        pthread_join(fam_th_, nullptr);
#endif
        fam_started_ = false;
    }
    uint64_t deadline = now_ms() + 2000;
    for (GuestThread* g : snap) {
        NT* gn = nt(g);
        if (gn->is_main || gn->joined || !gn->pth) continue;
        while (g->state != S::Finished && now_ms() < deadline) { sched_yield(); }
        if (g->state == S::Finished) { pthread_join(gn->pth, nullptr); gn->joined = true; }
        else std::printf("  [sched-native] thread %u did not stop (state=%d) — leaked\n",
                         g->id, (int)g->state);
    }
}

void NativeScheduler::request_shutdown() {
    gil::assert_held();
    shutdown_ = true;
    // The ONE legitimate request_stop broadcast: process shutdown. Monotonic
    // in native mode (see the cpu.h contract) — never call it per thread exit.
    if (cpu_) cpu_->request_stop();                  // broadcast: all emus' budgets zeroed
    for (auto& up : threads_) pthread_cond_broadcast(&nt(up.get())->cv);
}

// ---- introspection / diagnostics -------------------------------------------

void NativeScheduler::set_no_preempt(bool on) {
    if (on && !no_preempt_warned_) {
        no_preempt_warned_ = true;
        progress("sched-native: set_no_preempt no-op (dynarec cache is lock-protected; spec D7)");
    }
}

int NativeScheduler::live_count() {
    int c = 0; for (auto& up : threads_) if (up->state != S::Finished) ++c; return c;
}
int NativeScheduler::live_thread_count() { return live_count(); }
GuestThread* NativeScheduler::thread_by_id(uint32_t id) {
    gil::assert_held();
    for (auto& up : threads_) if (up->id == id) return up.get();
    return nullptr;
}
std::vector<uint32_t> NativeScheduler::live_thread_ids() {
    gil::assert_held();
    std::vector<uint32_t> v;
    for (auto& up : threads_) if (up->state != S::Finished) v.push_back(up->id);
    return v;
}

void NativeScheduler::dump_threads_to(void(*sink)(const char*)) {
    // WATCHDOG host thread. Try the GIL briefly; on failure dump the lock-free
    // flat snapshot anyway (stale-but-consistent), flagged.
    bool locked = (pthread_mutex_trylock(gil::mutex()) == 0);
    if (!locked) sink("sched-native: GIL busy — racy snapshot follows");
    uint32_t fn = flat_n_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < fn; ++i) {
        GuestThread* g = flat_[i];
        if (!g || g->state == S::Finished) continue;
        void* w = g->wait_obj;
        NT* gn = static_cast<NT*>(g->native);
        const uint32_t* ip = (g->state == S::Running && gn)
                             ? cpu_->thread_emu_ip(gn->emu) : nullptr;
        char m[128];
        std::snprintf(m, sizeof m, "thr %u st=%d wait=%s eip=%08x sl=%llu",
                      g->id, (int)g->state,
                      w ? static_cast<Waitable*>(w)->kind() : "-",
                      ip ? *ip : g->ctx.eip, (unsigned long long)g->slices);
        sink(m);
    }
    if (locked) pthread_mutex_unlock(gil::mutex());
}

NativeScheduler* g_native_sched = nullptr;           // runner_tramp backref

} // namespace d2rt
