// Console-specific services — not generic, not game-specific. The engine
// targets the Vita, so hardware access like this belongs here rather than in
// a port, which only consumes the hardware.
//
// What a port keeps: the path of its own log file, and whatever policy it
// wants to apply. Everything else lives here.
//
// No indirection is added on the per-frame path: these services are called
// at startup, at thread creation, or once per 10-second window. The log
// opens and closes the file on every line on purpose, so a line survives a
// hard crash.
#pragma once
#include <stdint.h>

// ---- Durable progress log ----------------------------------------------------
// On console, printf proves nothing — there's no stdout to read. This log is
// the only window into a screenless boot. It opens, writes, and closes on
// every line so the line survives a hard crash.
//
// The lock is not decorative: without it, two threads doing
// fopen/fprintf/fclose on the same file can corrupt the newlib heap (a
// timestamp's ASCII bytes landing on top of a heap pointer). A boot log that
// can crash the boot it's observing is the worst possible failure mode.

// The path is provided by the port — the only thing it provides. It is a
// constant definition rather than a setter: a log line can fire before any
// initialization point runs, and lazy init would pull in
// __cxa_guard_acquire (the class of trap the build script's `nm` guard exists
// to catch). A constant initializer has no ordering and no guard.
//
// The port writes it once, at file scope:
//     extern "C" const char* const wx86_vita_progress_path = "ux0:data/…/x.txt";
//
// Off console, the port defines nothing — see below.
extern "C" const char* const wx86_vita_progress_path;

// These two entry points exist on both targets. Unlike the rest of this file
// (a pure console service, guarded by `#ifdef __vita__` on both the
// declaration and its callers), the log is also called from the engine's
// generic body (bridge, cpu_box86, both schedulers, the mapper), which also
// compiles for the qemu/desktop harness. A console-only declaration would
// leave an unresolved reference there.
//
// Off `__vita__`, vita_host.cpp defines both functions as no-ops instead of
// using a weak symbol:
//   * the generic body links strongly, everywhere, with no null check;
//   * no port needs to provide a symbol off console;
//   * off-console behavior stays silent either way.
// A weak reference to the engine's name was considered and rejected: it would
// keep the same failure mode (a port providing nothing stays silently mute)
// under a different symbol name.
void wx86_vita_progress(const char* msg);
extern "C" void wx86_vita_progress_c(const char* msg);

// ---- Real sleep ----------------------------------------------------------
// The monotonic clock is not here: it lives in runtime/host_clock.h
// (wx86_now_us / wx86_now_ms) because it's also meaningful off console. Only
// sleep stays here — it goes through the Sony scheduler.
void wx86_vita_sleep_ms(uint32_t ms);

// ---- Host thread placement across user cores ---------------------------------
// WX86_COEURS (falls back to D2_COEURS) — three digits 0..3, one per role:
//   position 0 = presentation, 1 = watchdog, 2 = anti-starvation heartbeat.
// Default "222".
//
// The fourth core: the SDK only defines USER_0/1/2 masks (0x10000/0x20000/
// 0x40000), but the console reports activeCpuMask=0x000f0000 — four active
// user-core bits. Digit `3` requests this core; if the kernel refuses it, the
// rc reports that and nothing changes.
#define WX86_CPU_MASK_USER_3  0x00080000

int  wx86_vita_core_mask(int who);

// Self-pinning happens at thread entry, not at creation time: setting the
// affinity mask from the creator before sceKernelStartThread returns rc=0 but
// reads back as 0 once the thread is running, and threads migrate (two
// workers can end up on the same core). Each host thread must therefore
// re-pin itself as its first instruction.
extern "C" int wx86_vita_pin_self(int mask, unsigned* relu);

// Pins a thread other than the caller. Exists so a caller that needs to set
// the mask of a thread it creates doesn't call
// sceKernelChangeThreadCpuAffinityMask directly.
//
// Caveat: the limitation above still applies — a mask set by the creator
// before the thread starts reads back as 0 once the thread is running. This
// function only expresses intent and publishes its rc; the only reliable
// pinning is self-pinning via wx86_vita_pin_self at thread entry.
//
// `relu`: mask read back after setting, like pin_self. Passing nullptr skips
// the extra sceKernelGetThreadCpuAffinityMask call, so a call site that
// doesn't read back keeps exactly one syscall.
extern "C" int wx86_vita_pin_thread(int thread_uid, int mask, unsigned* relu);

// Registers a thread in the table published by wx86_vita_core_window_line().
void wx86_vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc);
// Number of host threads registered so far — useful context for thread
// creation diagnostics, since an exhausted UID quota and exhausted memory
// otherwise look the same from the rc alone.
int  wx86_vita_core_count();

// "cores:" line of the 10-second window, published to the log. Runs the
// 4th-core probe on first invocation.
void wx86_vita_core_window_line();
