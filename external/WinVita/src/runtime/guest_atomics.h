// src/runtime/guest_atomics.h — atomics on GUEST memory.
//
// KERNEL32's Interlocked* ops must be genuinely atomic with respect to
// other guest threads. That needs a HOST pointer to the guest cell
// (Cpu::hostptr) and compliance with the dynarec's SMC contract (a page
// holding translated code must be marked dirty before writing) — both of
// which only the engine can do. This module lives here instead of being
// copied into each port.
//
// wx86_ilk_ptr() returns nullptr when the cell isn't atomizable (unaligned,
// or no host pointer): the caller then falls back to a non-atomic guest
// read/write, which is the original behavior.
#pragma once
#include <cstdint>
namespace d2rt { class Cpu; }

uint32_t* wx86_ilk_ptr(d2rt::Cpu& c, uint32_t va);

// Memory order: RELAXED by default. On a target where every guest thread is
// pinned to the same core, the HARDWARE barrier is already guaranteed and
// only the COMPILER needs to be stopped from reordering. WX86_ILK_SEQCST=1
// (or D2_ILK_SEQCST=1, an older name) restores the full barrier.
bool wx86_ilk_seqcst();

uint32_t wx86_ilk_add_fetch  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_sub_fetch  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_exchange   (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_add  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_or   (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_and  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_xor  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_cas        (uint32_t* h, uint32_t expected, uint32_t desired);
void     wx86_ilk_fence();

// Diagnostic counters: the port displays them in its own end-of-run report,
// so they need to be readable.
void wx86_ilk_stats(unsigned long long* atomic,
                    unsigned long long* fallback,
                    unsigned long long* unaligned);
