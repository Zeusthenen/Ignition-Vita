// src/runtime/gil.h — the runtime Global Interpreter-style Lock (native
// scheduler only).
//
// One process-wide pthread_mutex serializing everything that runs OUTSIDE
// translated guest code: shim bodies, intrinsics, SEH dispatch, scheduler
// state. Translated guest code runs WITHOUT it (the game's own x86 `lock`
// prefixes are translated to real ldrex/strex). It is taken at the CpuBox86
// trap dispatch — NOT in Bridge::trap_handler, because Tier-1 intrinsics
// bypass the Bridge (cpu_box86.cpp try_intrinsic).
//
// It doubles as the native scheduler's lock: NativeScheduler::wait() blocks
// with pthread_cond_wait(cv, gil::mutex()) — atomically releasing the GIL
// while the thread sleeps. This gives the timeout-vs-completion exclusion
// (the GQCS phantom-packet bug) for free. NON-recursive: never nest lock().
//
// Inert until enable(): the cooperative backend never calls enable(), so its
// deterministic single-runner path never touches the mutex.
#pragma once
#include <pthread.h>
#include <cstdint>

namespace d2rt { namespace gil {

bool active();
void enable();                 // called ONCE, by the native scheduler ctor
void lock();
void unlock();
// Non-blocking version of lock() (used for D2_FILSTAT): true = acquired
// (same bookkeeping as lock()), false = contended, nothing touched. Used to
// count per-thread GIL contention without changing the default lock path
// (cpu_box86.cpp TrapGuard).
bool try_lock();
pthread_mutex_t* mutex();      // for pthread_cond_wait integration

// Debug owner tracking (cheap: plain assignments protected by the mutex
// itself — set after lock, cleared before unlock; never atomics).
// assert_held() asserts the CALLING host thread owns the GIL; no-op while
// inactive. Called at the top of every NativeScheduler method whose contract
// requires the lock (wait_common/wake_check_all/create_thread/notify).
// Two checks: (1) the GIL is held at all — true for every caller; (2) it is
// held by ME — silent (no verdict) for a thread that never registered a
// stack (see the identity table below), since there is nothing to compare
// against. For every real caller (main in run(), the runners) check (2) is
// exact. Check (2) is compiled only under #ifndef NDEBUG.
void assert_held();
// Toggles assert_held()'s check (2) ("I am really the holder"), which walks
// the identity table. Off by default in production (it cost a table scan
// per synchronization shim); re-enabled via D2_GILCHECK=1, read once at
// startup. This function exists for tests, which need to arm it without
// depending on the environment — call it before spawning threads. Check
// (1) — assert(g_owned) — is always unconditional.
void set_identity_check(bool on);
// pthread_cond_wait(cv, mutex()) releases and reacquires the mutex BEHIND
// lock()/unlock()'s back: the last unlocker cleared the owner flag, so a
// waiter must re-stamp itself when the wait returns or a later assert_held()
// in the same shim body (wait-then-notify) fires falsely. Call with the
// mutex held, right after the cond-wait loop exits.
void mark_owned();
// Symmetric to mark_owned(), for observability: call JUST BEFORE a cond-wait
// loop. Without it, the published holder would stay the thread that went to
// sleep in the cond (which released the mutex without going through
// unlock()), and a reader would believe the GIL held forever — exactly the
// lie this instrumentation exists to avoid. Does not touch assert_held()'s
// own bookkeeping.
void mark_released();

// ---- Observability: who holds the GIL, and since when ----------------------
//
// Written UNIQUELY by the holder, at acquire and release, as relaxed atomics
// (no lock, no barrier). Read by the watchdog and the starvation-detector
// heartbeat WITHOUT taking the GIL — a reader that had to block on the lock
// it's observing would have nothing to report.
//
// No clock is read at acquire time, deliberately: on Vita clock_gettime is a
// kernel call, and the GIL is taken on every import trap, so timestamping
// every acquire would be too costly. Hold time is therefore DERIVED BY THE
// READER: two consecutive samples with the same `acq` counter prove the
// acquisition has lasted at least the interval between the samples. What
// this module reports is always a LOWER BOUND (printed with a `>=` prefix),
// never a duration inflated by a stale timestamp.
//
// Holder identity is a STACK MARK (the address of a local variable in
// lock(), i.e. a point in the holder's host stack), resolved to a label by
// the reader — not a pthread_t: on pte, pthread_self() goes through
// pthread_getspecific -> pte_osTlsGetValue -> sceKernelGetTLSAddr, the same
// kernel chain as an emutls access — an inter-module call on every GIL
// acquire (i.e. every trap). The stack mark costs one instruction (add rX,
// sp, #n) and no call; the mark-to-label resolution happens on the READER
// side (probe(), off the hot path), from the table declared below.
struct Probe {
    bool        active;    // the GIL exists (native backend) — false under coop
    bool        held;      // held at the moment of the sample
    uint32_t    owner_tag; // holder's label (guest thread id), 0 = UNKNOWN
    uint32_t    acq;       // total acquisitions since boot
    bool        in_shim;   // the holder is INSIDE a Bridge shim body
    uint32_t    shim_va;   // trap VA (mappable via D2_DUMPTRAPS=1)
    const char* shim;      // e.g. "ws2_32.dll!#18" — stable string (slot_by_tag_ key)
};
void probe(Probe* out);        // never blocks; all-zero if inactive

// ---- Identity table, no TLS lookup ------------------------------------------
//
// The only identity a thread carries for free (in a register, no call) is
// its stack pointer. Each thread that may take the GIL therefore registers,
// ONCE, FROM ITSELF, an interval [lo,hi] of its own host stack and the label
// to report (guest thread id, never 0):
//
//     char ici;                                     // its address is the mark
//     int s = gil::register_stack((uintptr_t)&ici - SPAN, (uintptr_t)&ici, t->id);
//     ...                                           // thread lifetime
//     gil::unregister_stack(s);                     // AFTER its last acquire
//
// INVARIANT THE WHOLE SAFETY PROPERTY RESTS ON:
//   *** every thread that calls gil::lock() must have registered before, and
//       unregistered after its LAST acquire. ***
// Without it, an unregistered thread running on a dead, still-registered
// thread's RECYCLED stack would inherit that thread's label — the only false
// name this mechanism can produce. Both halves matter: registering names the
// live thread, unregistering stops a dead one from lending its name. (No
// target today actually recycles a stack within a session — this half fixes
// nothing reachable yet, but closes the hole so the invariant holds by
// construction rather than by accident.)
// The one deliberately UNREGISTERED caller is the main thread before run()
// (an early gil::Guard taken during boot self-tests): its stack is disjoint
// from any worker stack, it prints as "host", and assert_held() then claims
// nothing more than "the GIL is held".
//
// The declared interval must be a STRICT SUBSET of the real stack: the safe
// failure mode is "don't know" (mark outside every interval, label 0,
// printed as "host"), never "name the wrong thread". Resolution also
// refuses to answer as soon as TWO intervals contain the mark (recycled
// stacks after an exit): ambiguous => 0. An unregistered thread is simply
// never named.
//
// Cost of resolution, plainly: a linear scan of the table, with NO barrier
// and NO call (safety comes from write-once slots, not an acquire — see
// gil.cpp). Scan time grows with the number of guest threads ever
// registered since boot (slots are not reused), capped at 132. Not measured.
// Off the trap path (readers + assert_held only, never inside lock()).
//
// Returns the slot index, or -1 if the thread could not be registered (table
// full, bad arguments) — it then stays "host", which is the safe outcome.
// unregister_stack(-1) is a no-op.
int  register_stack(uintptr_t lo, uintptr_t hi, uint32_t tag);
void unregister_stack(int slot);

// Marks the shim body currently executing (Bridge::trap_handler, under the
// GIL). A single global record is enough: the GIL guarantees only one
// thread is inside a shim body at a time. It remembers its holder so that a
// shim which RELEASES the GIL (gil::Release around a ::poll) and reacquires
// it after another thread ran does not mislead a reader: the two holder
// marks differ, so no shim gets attributed to the wrong thread. Inert while
// the GIL is inactive (coop) — just a global-bool check.
// NOTE (stack marks): the holder recorded here is the MARK published by
// lock(), which differs between acquisitions of the SAME thread (different
// call depth). Comparison is therefore done by probe() on RESOLVED LABELS,
// not raw marks — otherwise a shim that releases and reacquires the GIL
// would lose its attribution even though nothing changed.
void note_shim_enter(uint32_t trap_va, const char* tag);
void note_shim_exit();

// RAII: take the GIL iff active (trap dispatch, host-side entry points).
struct Guard {
    bool a;
    Guard()  { a = active(); if (a) lock(); }
    ~Guard() { if (a) unlock(); }
    Guard(const Guard&) = delete;              // a copied Guard double-unlocks
    Guard& operator=(const Guard&) = delete;
};
// Inverse RAII: RELEASE the GIL around a blocking host call (::poll, long
// translation, sleep) so other guest threads keep running.
struct Release {
    bool a;
    Release()  { a = active(); if (a) unlock(); }
    ~Release() { if (a) lock(); }
    Release(const Release&) = delete;          // a copied Release double-locks
    Release& operator=(const Release&) = delete;
};

}} // namespace d2rt::gil
