// src/runtime/guest_atomics.cpp — see guest_atomics.h.
//
// Shared engine primitive: hosts logic that used to be duplicated verbatim
// across every port.
//
// MEMORY ORDER — RELAXED by default. On a target where every guest-code
// thread is pinned to the same core, threads never truly run in parallel,
// so ORDERING is already guaranteed by the hardware. What RELAXED still
// provides is ATOMICITY: a bare RMW can be preempted between its read and
// its write, which is what corrupts reference counts.
//
// SEQ_CST costs real throughput here: extra hardware barriers on what is
// the hottest instruction in the online profile.
//
// IF SINGLE-CORE PINNING EVER STOPS HOLDING, THIS ORDER BECOMES WRONG.
// WX86_ILK_SEQCST=1 restores SEQ_CST without rebuilding.
#include "guest_atomics.h"
#include "runtime/cpu.h"
extern "C" { int dyn86_protectdb(void); }
#include <cstdlib>
using namespace d2rt;

static unsigned long long g_ilkAtomic = 0, g_ilkFallback = 0, g_ilkUnaligned = 0;

bool wx86_ilk_seqcst(){
    // D2_ILK_SEQCST is an older name for this variable, kept as a fallback.
    static const bool v = [](){
        const char* e = getenv("WX86_ILK_SEQCST");
        if(!e) e = getenv("D2_ILK_SEQCST");
        return e && *e=='1'; }();
    return v;
}

uint32_t* wx86_ilk_ptr(Cpu& c, uint32_t va){
    if(va & 3u){ ++g_ilkUnaligned; ++g_ilkFallback; return nullptr; }
    uint32_t* hp = (uint32_t*)c.hostptr(va, 4);
    if(!hp){ ++g_ilkFallback; return nullptr; }
    // Matches Cpu::write() semantics: a page holding translated code is
    // write-protected on the host side and must be unprotected and marked
    // dirty before writing. dyn86_protectdb() defaults to 0 (no SIGSEGV
    // handler ported), so this is just a global load and a branch — not a
    // tree lookup — on the hottest path in the online profile.
    if(dyn86_protectdb()) c.invalidate_code(va, 4);
    ++g_ilkAtomic;
    return hp;
}

#define WX86_ILK(name, expr_seq, expr_rel) \
    uint32_t name(uint32_t* h, uint32_t v){ \
        return wx86_ilk_seqcst() ? (expr_seq) : (expr_rel); }
WX86_ILK(wx86_ilk_add_fetch, __atomic_add_fetch(h,v,__ATOMIC_SEQ_CST), __atomic_add_fetch(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_sub_fetch, __atomic_sub_fetch(h,v,__ATOMIC_SEQ_CST), __atomic_sub_fetch(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_exchange,  __atomic_exchange_n(h,v,__ATOMIC_SEQ_CST), __atomic_exchange_n(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_add, __atomic_fetch_add(h,v,__ATOMIC_SEQ_CST), __atomic_fetch_add(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_or,  __atomic_fetch_or (h,v,__ATOMIC_SEQ_CST), __atomic_fetch_or (h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_and, __atomic_fetch_and(h,v,__ATOMIC_SEQ_CST), __atomic_fetch_and(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_xor, __atomic_fetch_xor(h,v,__ATOMIC_SEQ_CST), __atomic_fetch_xor(h,v,__ATOMIC_RELAXED))
#undef WX86_ILK

// Returns the OLD value in both cases: on success expected is unchanged (so
// equal to the comparand, i.e. the old value), and reloaded with the actual
// current value on failure.
uint32_t wx86_ilk_cas(uint32_t* h, uint32_t expected, uint32_t desired){
    if(wx86_ilk_seqcst()) __atomic_compare_exchange_n(h,&expected,desired,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    else                  __atomic_compare_exchange_n(h,&expected,desired,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
    return expected;
}

void wx86_ilk_fence(){
    // Same reasoning as the Interlocked* ops: with single-core pinning the
    // HARDWARE barrier is already guaranteed, so only the COMPILER needs to
    // be stopped from reordering our own accesses.
    if(wx86_ilk_seqcst()) __atomic_thread_fence(__ATOMIC_SEQ_CST);
    else                  __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

void wx86_ilk_stats(unsigned long long* atomic,
                    unsigned long long* fallback,
                    unsigned long long* unaligned){
    if(atomic)    *atomic    = g_ilkAtomic;
    if(fallback)  *fallback  = g_ilkFallback;
    if(unaligned) *unaligned = g_ilkUnaligned;
}
