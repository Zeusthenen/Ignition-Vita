// src/runtime/guest_region.cpp — see guest_region.h.
//
// Carving a guest address range into blocks is an engine primitive, not
// game code. The OOM diagnostic goes through the embedder's callback
// instead of a direct log call, since the engine knows neither the
// embedder's log nor its console.
//
// Instances (the guest heap, the VA arena) stay with the embedder: this
// file deliberately declares no global region. An unfed global region here
// would be invisible at compile time and fatal the day some caller reaches
// for it.
#include "runtime/guest_region.h"

namespace wx86 {

namespace { RegionFailFn g_fail = nullptr; }

void region_set_fail_handler(RegionFailFn cb) { g_fail = cb; }

void GuestRegion::record_fail(uint32_t n) {
    if (!g_fail) return;
    g_fail(RegionFail{ name_, base_, n, cur_, peak_, largest_free(), used_bytes() });
}

} // namespace wx86
