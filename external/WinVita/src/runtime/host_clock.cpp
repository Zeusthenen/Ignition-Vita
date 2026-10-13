// src/runtime/host_clock.cpp — see host_clock.h.
//
// The same body every consumer of this engine would otherwise duplicate on
// its own side, and the one prof.cpp also needs but can't provide since it
// only compiles under -DPROF_COUNTERS.
#include "runtime/host_clock.h"

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#else
#include <time.h>
#endif

uint64_t wx86_now_us() {
#ifdef __vita__
    // The Sony kernel already counts in microseconds.
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

uint64_t wx86_now_ms() { return wx86_now_us() / 1000ull; }
