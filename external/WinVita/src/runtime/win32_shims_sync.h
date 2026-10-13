// src/runtime/win32_shims_sync.h — KERNEL32 synchronization shims (event,
// mutex, semaphore) and reading a thread's exit code. No literal or
// behavior specific to any one guest: plain Win32 semantics. A consumer's
// instrumentation hooks in via wx86_sync_set_observer() (guest_sync.h); it
// has no business living in these bodies.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_sync_install(d2rt::Bridge& br);
