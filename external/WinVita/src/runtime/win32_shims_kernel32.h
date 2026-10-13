// src/runtime/win32_shims_kernel32.h — generic KERNEL32 shims: no literal,
// path, or behavior specific to any one guest. Registered via
// Bridge::register_shim, the same mechanism a consumer uses to override one
// for its own game (overwrite-on-duplicate-key semantics, see bridge.h).
#pragma once
#include <cstdint>
namespace d2rt { class Bridge; }

// --- Process/thread identity, matching real Windows ------------------------
// Windows allocates PIDs and TIDs in steps of 4 (kernel table indices) and
// never returns 1 — PID 1 does not exist. These three functions live in the
// engine because it owns the real ID table and registers
// GetCurrentProcessId; a consumer filling a PROCESSENTRY32/THREADENTRY32
// must read the guest-facing IDs from here rather than inventing its own
// numbering, or the two views drift apart.
//
// Internal scheduler IDs stay small integers (1, 2, 3...) since logs,
// benchmarks, and profiles reference them everywhere; only the guest-facing
// view is translated, in both directions.
uint32_t wx86_win_pid();
uint32_t wx86_win_tid(uint32_t schedId);    // scheduler id -> guest-facing view
uint32_t wx86_sched_tid(uint32_t winTid);   // guest-facing view -> scheduler id (0 if invalid)

// Compatibility toggle: WX86_FID_AVANT=1 (alias D2_FID_AVANT) reverts to the
// legacy, non-faithful responses (PID 1, tid 1/2/3...,
// IsProcessorFeaturePresent always 0). Default is the faithful behavior,
// never the reverse. Exists so a fidelity test can be shown to actually fail
// when the toggle flips it off — a test that passes either way proves nothing.
bool wx86_fid_avant();

// --- Performance-counter frequency ------------------------------------------
// QueryPerformanceFrequency's return value; a consumer registering its own
// QueryPerformanceCounter must count in the same unit (read
// wx86_perf_frequency()). Default 1,000,000 (microsecond counter). A modern
// Windows with an invariant TSC reports 10,000,000: matching that requires
// setting this value AND scaling the counter together — never one without
// the other.
void     wx86_set_perf_frequency(uint64_t hz);
uint64_t wx86_perf_frequency();

void win32_shims_kernel32_install(d2rt::Bridge& br);
