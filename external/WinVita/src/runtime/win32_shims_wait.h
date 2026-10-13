// src/runtime/win32_shims_wait.h — WAITING: WaitForSingleObject,
// WaitForMultipleObjects, SleepEx, and the core of Sleep.
//
// Why this family lives here: both consumers of this engine share the exact
// same body — WaitForMultipleObjects is byte-identical, comments included,
// atomicity invariant and all. What differs between them is not semantics,
// it's the INSTRUMENTATION wrapped around it (tracing, wait logging, timing,
// async-load profiling, loop counters) — none of which decides anything. The
// engine carries the body and reports through wx86_wait_set_observer(); each
// consumer wires its own instrumentation into a single observer.
//
// Sleep stays with the consumer, and not out of caution: its body starts
// with shortcuts keyed to GAME-SPECIFIC ADDRESSES (a given Sleep(0) call site
// in the network loop behaving differently). Those are guest-specific
// literals, exactly the criterion that keeps a shim on the consumer side.
// What IS generic in Sleep — yielding, or sleeping for a delay — lives here,
// under wx86_wait_sleep(), which each consumer calls after its own prelude.
#pragma once
#include <cstdint>

namespace d2rt { class Bridge; class Cpu; class Waitable; }

// Generic core of Sleep/SleepEx: ms == 0 yields, otherwise waits out the
// delay. Without a scheduler, does nothing.
//
// Sleep and SleepEx share a single "never signaled" event to wait on. A
// manual event that's never signaled has no observable state — the wait
// state lives in the thread, not the object — so sharing one instance
// across every Sleep caller is safe.
void wx86_wait_sleep(uint32_t ms);

// ---- Observer -------------------------------------------------------------
enum {
    WX86_WAIT_SINGLE_ENTER = 0,  // before blocking: obj/handle/timeout are set
    WX86_WAIT_SINGLE_EXIT,       // after: ret = the code returned to the guest
    WX86_WAIT_SINGLE_UNKNOWN,    // unknown handle: reported "signaled" WITHOUT waiting
    WX86_WAIT_MULTI_ENTER,       // count/timeout set; handles = guest array
    WX86_WAIT_MULTI_EXIT,
};
struct WxWaitEvent {
    int              kind;
    d2rt::Cpu*       cpu;
    d2rt::Waitable*  obj;      // object being waited on (single wait), else nullptr
    uint32_t         handle;   // handle being waited on (single wait)
    uint32_t         timeout;  // requested delay, 0xFFFFFFFF = infinite
    uint32_t         ret;      // return code (EXIT events)
    uint32_t         count;    // handle count (multi-wait)
    uint32_t         handles;  // GUEST address of the handle array
    uint32_t         all;      // bWaitAll (multi-wait)
};
typedef void (*WxWaitObserverFn)(const WxWaitEvent&);
// A SINGLE observer: a second call REPLACES the first.
void wx86_wait_set_observer(WxWaitObserverFn cb);

void win32_shims_wait_install(d2rt::Bridge& br);
