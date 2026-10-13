// src/runtime/guest_sync.h — Win32 waitable kernel objects, the handle table,
// and an observer hook so a consumer can instrument sync activity without
// owning any of this logic.
//
// The KERNEL32 synchronization shims (CreateEvent, SetEvent,
// WaitForSingleObject, critical sections...) all share one handle table and
// one set of object types, so they have to move into the engine together
// rather than one at a time. Instrumentation built on top (sync tracing,
// wait timing, starvation counters) never decides anything — it only
// observes, through the hook below (same model as the network observer in
// win32_shims_wsock32.h).
#pragma once
#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <vector>
#include "runtime/guest_thread.h"
#include "runtime/cpu.h"

// ---- Kernel objects -----------------------------------------------------------
struct WxEvent : d2rt::Waitable {
    bool signaled=false, manual=false;
    bool try_acquire(d2rt::GuestThread*) override {
        if(signaled){ if(!manual) signaled=false; return true; } return false; }
    bool ready(d2rt::GuestThread*) override { return signaled; }
    const char* kind() const override { return "event"; } };

// Counting semaphore (a real count, not a disguised binary event): a wait
// decrements it, a release increments it. Correct for counts > 1.
struct WxSemaphore : d2rt::Waitable {
    int count=0, maxc=0x7fffffff;
    bool try_acquire(d2rt::GuestThread*) override { if(count>0){ count--; return true; } return false; }
    bool ready(d2rt::GuestThread*) override { return count>0; }
    const char* kind() const override { return "sema"; } };

struct WxThread : d2rt::Waitable {
    d2rt::GuestThread* gt=nullptr;
    bool try_acquire(d2rt::GuestThread*) override { return gt && gt->state==d2rt::GuestThread::State::Finished; }
    bool ready(d2rt::GuestThread*) override { return gt && gt->state==d2rt::GuestThread::State::Finished; }
    const char* kind() const override { return "thread"; } };

// Critical section. Owner/count live in C++ and stay INVISIBLE to guest code:
// some guest binaries inline-read OwningThread/RecursionCount directly, and a
// nonzero OwningThread there routes them onto Enter/Leave paths that
// shim-based synchronization never balances, deadlocking. Keep the real
// struct fields at zero.
struct WxCrit : d2rt::Waitable {
    uint32_t va=0, owner=0; int count=0;
    bool try_acquire(d2rt::GuestThread* t) override { uint32_t me=t?t->id:1u;
        if(owner==0||owner==me){ owner=me; count++; return true; } return false; }
    bool ready(d2rt::GuestThread* t) override { uint32_t me=t?t->id:1u; return owner==0||owner==me; }
    const char* kind() const override { return "critsec"; } };

// Multi-object wait — WaitForMultipleObjects(nCount, ..., bWaitAll).
struct WxMultiWait : d2rt::Waitable {
    std::vector<d2rt::Waitable*> objs; bool all=false; uint32_t last=0;
    // TWO-PASS ATOMICITY INVARIANT: both passes must run inside a single
    // critical section (the scheduler lock / GIL — never per-object locks).
    // A backend that serialized per object could let another thread consume
    // object A between pass 1 and pass 2: pass 2's try_acquire(A) would then
    // fail silently (return ignored) and the wait-all would report success
    // without actually owning A — a phantom acquisition, worse than the
    // partial-consume bug it replaces. Pass 2's return values are
    // deliberately ignored: under the invariant they can only fail for a
    // duplicate handle — do not turn that into an assertion (the duplicate
    // case would legitimately trip it).
    // Deliberate deviation from Windows: the same handle listed twice in a
    // bWaitAll set. Real Windows refuses the wait (WAIT_FAILED,
    // ERROR_INVALID_PARAMETER; MSDN: lpHandles "may not contain multiple
    // copies of the same handle"). Here it's accepted and consumed once —
    // more permissive than Windows, but harmless since no real program can
    // depend on behavior that fails there.
    bool try_acquire(d2rt::GuestThread* t) override {
        if(all){ for(auto* o:objs) if(!o->ready(t)) return false;   // pass 1: probe only
                 for(auto* o:objs) o->try_acquire(t);               // pass 2: consume all
                 last=0; return true; }
        for(size_t i=0;i<objs.size();++i) if(objs[i]->try_acquire(t)){ last=(uint32_t)i; return true; }
        return false; }
    bool ready(d2rt::GuestThread* t) override {
        if(all){ for(auto* o:objs) if(!o->ready(t)) return false; return true; }
        for(auto* o:objs) if(o->ready(t)) return true; return false; }
    uint32_t result() const override { return last; }   // WAIT_OBJECT_0 + index
    const char* kind() const override { return "multi"; } };

// IO completion port (GetQueuedCompletionStatus). The three words written on
// wakeup (bytes, key, overlapped) land in GUEST memory, hence the Cpu*
// carried by this object.
struct WxIocp : d2rt::Waitable {
    std::deque<std::array<uint32_t,3>> q;                              // {bytes,key,overlapped}
    std::map<d2rt::GuestThread*,std::array<uint32_t,3>> outp;          // waiter -> {pBytes,pKey,pOv}
    d2rt::Cpu* cpu=nullptr;
    bool try_acquire(d2rt::GuestThread* t) override {
        if(q.empty()) return false;
        auto c=q.front(); q.pop_front();
        auto it=outp.find(t);
        if(it!=outp.end()){ auto&o=it->second;
            if(o[0]) cpu->write_u32(o[0],c[0]);
            if(o[1]) cpu->write_u32(o[1],c[1]);
            if(o[2]) cpu->write_u32(o[2],c[2]); }
        return true; }
    bool ready(d2rt::GuestThread*) override { return !q.empty(); }
    uint32_t result() const override { return 1; }                     // GQCS returns TRUE
    const char* kind() const override { return "iocp"; } };

bool wx86_is_kind(d2rt::Waitable* w, const char* kind);

// ---- Handle table ------------------------------------------------------------
// Owned by the engine, not the consumer — this is what allows sync shims to
// move into the engine one at a time instead of all at once.
uint32_t          wx86_handle_add(d2rt::Waitable* w);
d2rt::Waitable*   wx86_handle_find(uint32_t h);
void              wx86_handle_erase(uint32_t h);
unsigned          wx86_handle_count();

// Reserves an id WITHOUT storing anything in the table. A consumer can have
// its own non-waitable handles (e.g. a process snapshot); those must still
// come from the SAME counter, or two unrelated handles could end up sharing
// a number.
uint32_t          wx86_handle_next_id();

// ---- Critical sections --------------------------------------------------------
// Single-entry MRU cache: critical-section access is highly repetitive (the
// same section taken and released back-to-back), so this skips the map
// lookup in the common case.
WxCrit* wx86_crit_for(uint32_t cs_va);      // creates if absent
WxCrit* wx86_crit_lookup(uint32_t cs_va);   // does not create; nullptr if unknown
void    wx86_crit_forget(uint32_t cs_va);
// Iterates sections currently HELD (for end-of-run diagnostics). Callback is
// invoked once per held section.
void    wx86_crit_each_held(void (*fn)(uint32_t va, uint32_t owner, int count, void* ud), void* ud);

// Single body for critical-section exit, shared by the normal shim and the
// fast path invoked directly from the dynarec's trap dispatch (which falls
// back to the shim outside the uncontended case: active tracing, contention,
// exited thread). One implementation avoids hand-syncing duplicated logic on
// this hot path.
//
// Returns true if the section was actually released (count reached zero).
bool wx86_crit_leave(uint32_t cs_va, d2rt::ThreadScheduler* sched);

// ---- Observer ------------------------------------------------------------
enum {
    WX86_SYNC_EVENT_SET = 0,   // SetEvent: obj = the event
    WX86_SYNC_EVENT_RESET,     // ResetEvent
    WX86_SYNC_WAIT_DONE,       // a wait just succeeded: obj = the acquired object
    WX86_SYNC_OBJ_CREATE,      // creation: obj = the object, handle = its handle
    WX86_SYNC_OBJ_CLOSE,
    WX86_SYNC_EVENT_PULSE,
};
struct WxSyncEvent {
    int               kind;
    d2rt::Cpu*        cpu;      // CPU at the call site, for consumer diagnostics
    d2rt::Waitable*   obj;
    uint32_t          handle;
};
typedef void (*WxSyncObserverFn)(const WxSyncEvent&);
// Single observer, deliberately: silently allowing a second hook to replace
// the first without comment can mask real bugs. A second call REPLACES the
// first.
void wx86_sync_set_observer(WxSyncObserverFn cb);
void wx86_sync_notify(const WxSyncEvent& e);   // called by both the engine and the consumer
