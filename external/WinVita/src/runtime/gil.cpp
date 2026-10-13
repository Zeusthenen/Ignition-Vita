// src/runtime/gil.cpp — see gil.h.
//
// PTHREAD_MUTEX_INITIALIZER static init is valid for both pthread
// implementations linked here: glibc (host/qemu checks) and VitaSDK's pte,
// where pthread_mutex_t is a pointer and the initializer is the sentinel
// ((pthread_mutex_t)-1) that pthread_mutex_lock lazily materializes via
// pte_mutex_check_need_init (verified in libpthread.a). custommem.c runs
// pthread_mutex_init at startup only because it wants an ERRORCHECK attr —
// not because static init breaks. pte's DEFAULT type is non-recursive,
// matching the never-nest contract.
#include "runtime/gil.h"
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace d2rt { namespace gil {

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// write precedes every pthread_create (NativeScheduler ctor) — plain bool is
// race-free by construction; do not "fix" with a toggle.
static bool g_active = false;
// "The GIL is held", for assert_held(): written ONLY under g_mu (set after
// lock / cleared before unlock), so a plain bool suffices — the mutex IS the
// synchronization. Holder IDENTITY lives elsewhere: published as a stack
// mark (g_pub_owner_mark, below). A thread parked in pthread_cond_wait
// leaves a stale mark behind (the cond released the mutex on its own) —
// harmless: the next acquire overwrites it, and the sleeper re-publishes via
// mark_owned() when its wait returns.
static bool g_owned = false;

// ---- Observability (see gil.h) ----------------------------------------------
// Published by the HOLDER only, relaxed: three stores at acquire, one at
// release. No barrier — a reader seeing a state that is a few nanoseconds
// stale is fine with it (it compares samples 200ms or 10s apart).
// `g_pub_held` is kept separate from `g_pub_owner_mark` ON PURPOSE: a holder
// can be unidentifiable to everyone (unregistered host thread, full table),
// and "unknown holder" must never read as "GIL free".
static std::atomic<bool>      g_pub_held{false};
// Holder's STACK MARK (see gil.h), not a pthread_t: lock() sits on the path
// of EVERY trap, and pthread_self() there would cost a full TLS chain
// (pthread_getspecific -> pte_osTlsGetValue -> sceKernelGetTLSAddr, an
// INTER-MODULE call). 0 = nobody has taken the GIL yet.
static std::atomic<uintptr_t> g_pub_owner_mark{0};
static std::atomic<uint32_t>  g_pub_acq{0};
// Shim body currently executing (only one at a time — the GIL guarantees it).
static std::atomic<uintptr_t> g_shim_owner{0};   // 0 = nobody in a shim
static std::atomic<uint32_t>  g_shim_va{0};
static std::atomic<const char*> g_shim_tag{nullptr};

// acq: single-writer (the holder), so load+store relaxed — no atomic RMW
// (ldrex/strex) on the hot path.
static inline void pub_take(uintptr_t mark) {
    g_pub_acq.store(g_pub_acq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_pub_owner_mark.store(mark, std::memory_order_relaxed);
    g_pub_held.store(true, std::memory_order_relaxed);
}
static inline void pub_drop() { g_pub_held.store(false, std::memory_order_relaxed); }

// ---- Identity table (see gil.h) ---------------------------------------------
// Slots are never REUSED (unregistering turns a slot off, it does not free
// it): they are read WITHOUT A LOCK by the watchdog, the starvation-detector
// heartbeat, and assert_held(). 132 = kFlatMax (128 published guest threads)
// + main + margin.
//
// NO BARRIER, on write or on read — not an oversight. Each field of a slot
// changes only ONCE (BSS zero -> its value), then at most once more back to
// 0 on shutdown. A reader can therefore never observe a "half-written"
// value: it reads either the old value (0) or the new one. It is then
// enough that 0 be REJECTING on the two fields that decide a match:
//
//   * `tag == 0`  -> slot ignored;
//   * `lo == 0`   -> slot ignored (explicit guard below);
//   * `hi == 0`   -> `mark <= 0` is false, a stack mark is never 0.
//
// Any PARTIAL visibility of a slot therefore yields a MISS ("don't know",
// label 0, printed as "host"), never a false match — exactly the failure
// mode gil.h requires. `lo` is written AFTER `hi` and cleared BEFORE `tag`
// for this reason — it is the guard.
// Fields stay atomic (relaxed) so that a concurrent read of a write is not a
// data race in the language sense; on ARMv7 a relaxed load/store of an
// aligned word is a bare `ldr`/`str`, so the cost is zero.
static constexpr unsigned kStackSlots = 132;
struct StackSlot {
    std::atomic<uintptr_t> lo, hi;   // lo == 0 => dead or not-yet-ready slot
    std::atomic<uint32_t>  tag;      // 0 = slot not ready yet / turned off
};
static StackSlot g_slots[kStackSlots];
static std::atomic<unsigned> g_slot_res{0};   // RESERVED slots

int register_stack(uintptr_t lo, uintptr_t hi, uint32_t tag) {
    if (!lo || lo >= hi || tag == 0) return -1;       // saying nothing beats a false name
    unsigned i = g_slot_res.fetch_add(1, std::memory_order_relaxed);
    if (i >= kStackSlots) return -1;                  // table full: this thread stays "host"
    g_slots[i].hi.store(hi, std::memory_order_relaxed);
    g_slots[i].lo.store(lo, std::memory_order_relaxed);   // the guard, written AFTER hi
    g_slots[i].tag.store(tag, std::memory_order_relaxed);
    return (int)i;
}

// Turns the slot off (it is not freed: see above, safety comes from
// write-once fields). Call from the thread itself, after its last GIL
// acquire — the half of the invariant that stops a dead thread from lending
// its name to an unregistered one running on its recycled stack (gil.h).
void unregister_stack(int slot) {
    if (slot < 0 || (unsigned)slot >= kStackSlots) return;
    g_slots[slot].lo.store(0, std::memory_order_relaxed);   // break the match FIRST
    g_slots[slot].tag.store(0, std::memory_order_relaxed);
}

// Mark -> label, for TWO marks in a SINGLE scan. Exactly one match gives an
// answer; zero (unregistered thread) or MULTIPLE matches (recycled stacks,
// overlapping intervals) return 0 = "don't know" — never a false name
// (gil.h). Off the hot path: called by READERS, and by assert_held() in
// debug builds, which needs both marks (its own and the holder's) and so
// pays for one scan instead of two. A null mark can never match (lo > 0).
static void resolve2(uintptr_t ma, uintptr_t mb, uint32_t* ta, uint32_t* tb) {
    uint32_t fa = 0, fb = 0; unsigned ha = 0, hb = 0;
    unsigned n = g_slot_res.load(std::memory_order_relaxed);
    if (n > kStackSlots) n = kStackSlots;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t t = g_slots[i].tag.load(std::memory_order_relaxed);
        if (!t) continue;                             // reserved, not yet filled, or off
        uintptr_t lo = g_slots[i].lo.load(std::memory_order_relaxed);
        if (!lo) continue;                            // the guard (see above)
        uintptr_t hi = g_slots[i].hi.load(std::memory_order_relaxed);
        if (ma >= lo && ma <= hi) { fa = t; ++ha; }
        if (mb >= lo && mb <= hi) { fb = t; ++hb; }
    }
    *ta = (ha == 1) ? fa : 0;
    *tb = (hb == 1) ? fb : 0;
}

// ---- WX86_GILCHECK: holder IDENTITY check, OFF BY DEFAULT ------------------
// assert_held()'s check (2) ("I am really the GIL holder") scans the
// identity table (resolve2) on EVERY synchronization shim, and the shipped
// binary is not built with -DNDEBUG: it would pay that scan in production
// for an invariant that only breaks during scheduler work. WX86_GILCHECK=1
// re-enables it as-is. Check (1) — assert(g_owned), a plain bool read —
// stays always on: it catches the real bug ("nobody holds the GIL"), and
// costs nothing.
// No global -DNDEBUG: other invariants in the project depend on assertions
// staying compiled in.
// Dynamic initialization at program startup (namespace scope): no
// thread-safe guard variable needed on the hot path, unlike a function-local
// static. assert_held() is only called once the runtime is running, well
// after static initialization.
static bool g_gilcheck = [] {
    const char* v = std::getenv("WX86_GILCHECK"); if (!v) v = std::getenv("D2_GILCHECK");
    return v && std::strcmp(v, "0") != 0;
}();
void set_identity_check(bool on) { g_gilcheck = on; }   // for tests (see gil.h)

bool active() { return g_active; }
void enable() { assert(!g_active); g_active = true; }   // called-ONCE contract
// `ici` is NEVER read: only its ADDRESS matters — a point in the calling
// thread's host stack, i.e. its identity, obtained without any call (see
// gil.h). That is all this path, walked on EVERY trap, publishes.
void lock()   { char ici; pthread_mutex_lock(&g_mu); g_owned = true; pub_take((uintptr_t)&ici); }
void unlock() { g_owned = false; pub_drop(); pthread_mutex_unlock(&g_mu); }
bool try_lock() {
    char ici;
    if (pthread_mutex_trylock(&g_mu) != 0) return false;
    g_owned = true; pub_take((uintptr_t)&ici); return true;
}
pthread_mutex_t* mutex() { return &g_mu; }
void assert_held() {
    if (!g_active) return;
    assert(g_owned);                         // (1) the GIL is held — a real bug if not
#ifndef NDEBUG
    // (2) and it is really ME who holds it. Exact as soon as the caller is a
    // REGISTERED thread, which every real caller is (main in run(), the
    // runners); an unregistered thread cannot be resolved — no verdict is
    // manufactured rather than risk a false one. All under #ifndef NDEBUG:
    // zero cost when assertions are compiled out.
    // A SINGLE table scan for both marks, and NO barrier inside (see
    // resolve2). This path is not cold — at least one assert_held() per
    // synchronization shim — and the shipped binary is not built with
    // -DNDEBUG, so it lives behind WX86_GILCHECK, OFF BY DEFAULT (see
    // g_gilcheck above): its cost grew with the number of registered
    // threads.
    if (!g_gilcheck) return;                 // OFF BY DEFAULT (see g_gilcheck)
    char ici;
    uint32_t me = 0, owner = 0;
    resolve2((uintptr_t)&ici, g_pub_owner_mark.load(std::memory_order_relaxed), &me, &owner);
    assert(!me || owner == me);
#endif
}
void mark_owned() { char ici; g_owned = true; pub_take((uintptr_t)&ici); }
void mark_released() { pub_drop(); }   // observability only: g_owned is left untouched (see gil.h)

void note_shim_enter(uint32_t trap_va, const char* tag) {
    if (!g_active) return;                       // coop: inert (one check, no write)
    g_shim_va.store(trap_va, std::memory_order_relaxed);
    g_shim_tag.store(tag, std::memory_order_relaxed);
    // Reuses the mark already published by lock(): recomputing it here would
    // cost one more write per trap, and RESOLVING it (resolve2) a table scan
    // on the hot path. Resolution happens in probe() instead.
    g_shim_owner.store(g_pub_owner_mark.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
void note_shim_exit() {
    if (!g_active) return;
    g_shim_owner.store(0, std::memory_order_relaxed);
}

void probe(Probe* p) {
    p->active = g_active; p->held = false; p->owner_tag = 0; p->acq = 0;
    p->in_shim = false; p->shim_va = 0; p->shim = nullptr;
    if (!g_active) return;                       // coop: nothing to report, nothing printed
    p->held      = g_pub_held.load(std::memory_order_relaxed);
    uintptr_t om = g_pub_owner_mark.load(std::memory_order_relaxed);
    uintptr_t so = g_shim_owner.load(std::memory_order_relaxed);
    p->acq       = g_pub_acq.load(std::memory_order_relaxed);
    uint32_t t_om = 0, t_so = 0;
    resolve2(om, so, &t_om, &t_so);              // resolved READER-SIDE (gil.h), one scan
    p->owner_tag = t_om;
    if (!p->held) return;
    // The shim is attributed ONLY if its holder is the one currently holding
    // the GIL (see gil.h: a shim that releases then reacquires it). Two
    // marks from the SAME thread differ (different call depths), hence
    // comparing RESOLVED LABELS; comparing raw marks also covers an
    // unregistered thread (label 0 on both sides, which must never count as
    // a match).
    if (so && (so == om || (p->owner_tag && t_so == p->owner_tag))) {
        p->in_shim = true;
        p->shim_va = g_shim_va.load(std::memory_order_relaxed);
        p->shim    = g_shim_tag.load(std::memory_order_relaxed);
    }
}

}} // namespace d2rt::gil
