// src/runtime/guest_thread_ctx.h — current thread's TIB, and the last Win32
// error living there.
//
// KERNEL32 shims need to write the last error (SetLastError, and any call
// that fails cleanly) at TIB+0x34 of the current thread. The logic — current
// thread's TIB, or the main thread's if no scheduler is live yet — and the
// offset (TEB.LastErrorValue in the Win32 ABI) are generic; only which
// scheduler is live and where the main TIB sits are consumer-specific, so
// the engine owns the logic and the consumer supplies those two values.
#pragma once
#include <cstdint>
namespace d2rt { class Cpu; class ThreadScheduler; }

// Call once the consumer's memory layout is fixed.
void wx86_set_main_tib(uint32_t tib);
// Call once the consumer's scheduler exists (and again if it changes).
// nullptr is valid: before the scheduler exists, falls back to the main thread.
void wx86_set_scheduler(d2rt::ThreadScheduler* s);

// Current thread's TIB, or the main TIB if no thread is scheduled yet.
uint32_t wx86_cur_tib();
// The live scheduler, as declared by the consumer. Sync shims need it to
// block/wake threads; declaring it once avoids each consumer keeping its own
// pointer alongside the engine's, which would let the two drift out of sync.
d2rt::ThreadScheduler* wx86_sched();

// Last Win32 error (TEB+0x34) of the current thread.
void     wx86_set_lasterr(d2rt::Cpu& c, uint32_t v);
uint32_t wx86_get_lasterr(d2rt::Cpu& c);
