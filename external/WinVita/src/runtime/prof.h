// src/runtime/prof.h — PROF_COUNTERS profiling.
//
// Deterministic-friendly counters + wall-clock accumulators at the C++ hot
// spots (bridge trap, scheduler slice, sync shims). NOTHING on the dynarec
// per-instruction path — bias stays under 5%. EIP hot-block sampling
// piggybacks on quantum preemption (one sample per preempted slice), not a
// per-instruction hook, so the sample distribution matches real execution.
//
// Compiles away entirely without -DPROF_COUNTERS.
#pragma once
#include <cstdint>

namespace d2rt { namespace prof {

#ifdef PROF_COUNTERS
uint64_t now_ns();                       // monotonic (qemu/linux + Vita)

// Time split. run_ns covers cpu->run() (dynarec + OS traps executed inside);
// trap_ns covers only the OS-shim bodies — so pure dynarec = run_ns - trap_ns.
extern uint64_t run_ns;
extern uint64_t trap_ns;
extern uint64_t trap_calls;

// Sync behaviour under the (mono-runner) scheduler: how often a Wait* was
// satisfiable immediately vs actually blocked the thread. This is THE number
// that decides whether ESync/CS-fast-path style optimizations can pay (§2.1).
extern uint64_t wait_calls, wait_immediate;
extern uint64_t cs_enter, cs_contended;

// EIP histogram sampled at preemption (bucketed to 64 bytes).
void   sample_eip(uint32_t eip);
// Iterate top-N buckets, hottest first: cb(eip_bucket, hits).
void   top_eips(int n, void (*cb)(uint32_t eip, uint64_t hits, void* u), void* u);
uint64_t eip_samples();

// PER GUEST THREAD: the same preemption sample, attributed to the thread
// that was running, at the EXACT EIP (no bucket); plus the EXACT count of
// translated blocks executed per thread (delta of the dyn86_blocks counter
// around each coop slice). One sample per slice = one sample per QUANTUM of
// blocks: this is a uniform draw over blocks, not over time.
void   sample_eip_tid(uint32_t eip, uint32_t tid);
void   add_blocks_tid(uint32_t tid, uint32_t blocks);
void   note_thread(uint32_t tid, uint32_t entry);
// cb(tid, entry, samples, blocks) for each thread seen, in increasing tid order.
void   threads(void (*cb)(uint32_t tid, uint32_t entry, uint64_t samples, uint64_t blocks, void* u), void* u);
// cb(eip, hits): the n most frequent exact EIPs for the thread (n<0: all).
void   top_eips_tid(uint32_t tid, int n, void (*cb)(uint32_t eip, uint64_t hits, void* u), void* u);

#else
inline uint64_t now_ns() { return 0; }
inline void sample_eip(uint32_t) {}
inline void top_eips(int, void (*)(uint32_t, uint64_t, void*), void*) {}
inline uint64_t eip_samples() { return 0; }
inline void sample_eip_tid(uint32_t, uint32_t) {}
inline void add_blocks_tid(uint32_t, uint32_t) {}
inline void note_thread(uint32_t, uint32_t) {}
inline void threads(void (*)(uint32_t, uint32_t, uint64_t, uint64_t, void*), void*) {}
inline void top_eips_tid(uint32_t, int, void (*)(uint32_t, uint64_t, void*), void*) {}
#endif

}} // namespace d2rt::prof
