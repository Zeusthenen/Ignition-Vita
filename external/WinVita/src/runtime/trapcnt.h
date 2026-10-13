// src/runtime/trapcnt.h — HIT counter per trap slot.
//
// Why this file exists: Bridge::dump_prof() gives a TIME per slot but never
// a COUNT, and it's compiled OUT of the shipped binary (PROF_COUNTERS). It
// also completely misses intrinsics: the fast path (cpu_box86.cpp
// try_intrinsic) returns BEFORE entering the Bridge, so a slot served as an
// intrinsic never shows up there. Any count built on dump_prof therefore
// undercounts the hottest slots — exactly the ones worth pricing.
//
// This counter is ALWAYS armed (it's the denominator for any future
// per-unit cost) and stays cheap: one load, two comparisons, one 64-bit add.
// It's incremented on paths ALREADY under the GIL (the trap-dispatch
// gil::Guard in CpuBox86::run, and Bridge::trap_handler's body, which runs
// under that same Guard), so no extra atomic or lock.
//
// The index is EXACTLY trap_handler's: (va - trap_base)/16, the same
// division that finds the slot. No second addressing scheme to keep in
// sync.
#pragma once
#include <cstdint>

namespace d2rt {
namespace trapcnt {

// Bridge::alloc_trap spaces slots 16 bytes apart and reserves 2048 of them;
// 4096 entries (32 KiB of BSS) cover twice that reservation. Past that,
// bump() silently ignores: a truncated counter beats an out-of-bounds
// write, and dump_trap_counts() says so.
inline constexpr uint32_t kMax = 4096;

inline uint32_t base = 0;          // = Bridge::trap_base_, set by commit()
inline uint64_t hits[kMax] = {};   // cumulative hits per slot

inline void bump(uint32_t va) {
    const uint32_t b = base;
    if (!b || va < b) return;
    const uint32_t i = (va - b) >> 4;   // /16: alloc_trap's stride
    if (i < kMax) ++hits[i];
}

}  // namespace trapcnt
}  // namespace d2rt
