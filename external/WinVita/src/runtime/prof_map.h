// src/runtime/prof_map.h — the FAMILY MAP for the address profiler.
//
// The engine's address samplers (D2_EIPPROF, block sampling; D2_TIMEPROF,
// uniform time sampling) file each sample into a FAMILY. The engine itself
// knows no subsystem names: it counts RVA ranges that the CONSUMER
// describes, and the consumer names them at print time. WITHOUT an
// installed map — a port that hasn't described its binary — everything
// falls into the catch-all family (kProfFamOther), the only honest profile
// possible: "I don't know where this is."
//
// Shape: a TABLE CONFIGURED AT INSTALL TIME, like wx86_net_set_observer and
// wx86_sync_set_observer. The engine exposes and reports; it never asks.
#pragma once
#include <cstdint>

// A family is an RVA range [lo, hi). Ranges are tried IN ORDER: the first
// one containing the address wins, the same semantics as an `else if`
// chain. They may therefore overlap.
struct Wx86ProfRange { uint32_t lo = 0, hi = 0; };

// Max number of NAMED families, and the index of the catch-all family.
// These two numbers match the shape of counters the engine already exports
// (d2rt_eipprof[8], d2rt_tp_bucket[8]) and the seven labels a port prints:
// one side can't move without the other.
enum { kProfFamMax = 6, kProfFamOther = 6 };

struct Wx86ProfMap {
    Wx86ProfRange fam[kProfFamMax] = {};
    int           nfam = 0;        // families actually described (0 = none)
    // Three DETAIL windows, each feeding an already-exported counter.
    // 0 = window off (RVA 0 is the PE header, never code).
    uint32_t zoom_4k_a = 0;        // 32 slots of 4 KiB  -> d2rt_eipprof_sub
    uint32_t zoom_4k_b = 0;        // 32 slots of 4 KiB  -> d2rt_eipprof_sub2
    uint32_t zoom_256  = 0;        // 16 slots of 256 B  -> d2rt_eipprof_fn
};

// Call before arming a profile. Without a call, the map stays empty.
void wx86_prof_set_map(const Wx86ProfMap& m);
const Wx86ProfMap& wx86_prof_map();

// ---- THE CLASSIFICATION, in the header -------------------------------------
// Lives here rather than in cpu_box86.cpp for ONE reason: cpu_box86.cpp only
// compiles for ARM/Vita, so its classification could never be exercised by
// an oracle. Here, tools/prof_map_selftest.cpp compares it against a
// faithful transcription of the original else-if chain over a full scan of
// .text — and proves the test actually catches a fault by injecting a
// one-bound error.

// Family of an RVA: the FIRST range that contains it, else kProfFamOther.
inline int wx86_prof_family(const Wx86ProfMap& m, uint32_t rva) {
    for (int i = 0; i < m.nfam; ++i)
        if (rva >= m.fam[i].lo && rva < m.fam[i].hi) return i;
    return kProfFamOther;
}

// Slot in a detail window of `slots` slots of 2^shift bytes starting at
// `base`, or -1 if the address is outside the window (base = 0: window off).
inline int wx86_prof_zoom(uint32_t base, unsigned shift, int slots, uint32_t rva) {
    if (!base || rva < base) return -1;
    const uint32_t off = rva - base;
    if ((off >> shift) >= (uint32_t)slots) return -1;
    return (int)(off >> shift);
}
