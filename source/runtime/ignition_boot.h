// runtime/ignition_boot.h — the Phase-1 diagnostic boot sequence.
#pragma once
#include <cstdint>   // iteration-G150: guest_run_resumes()

namespace wx86 { class GuestRegion; }

namespace ign {

// Opens the log, loads ux0:data/ignition/Ign_d3d.exe, wires WinVita's
// engine shims plus Ignition's own KERNEL32/WINMM stubs, and attempts to
// run the PE entry point. Returns once the guest halts for any reason
// (normal exit, an unresolved import, a CPU fault) — never throws, never
// aborts the process, so the app always returns cleanly to LiveArea with a
// complete log on the memory card.
void boot_run();

// Called by any shim that wants the run to stop and the session to end
// (ExitProcess, an import nothing implements yet, ...). Safe to call from
// inside a shim, including nested inside cpu->run(); boot_run() notices
// once that run() call returns. Only the first call's reason is kept.
void request_halt(const char* reason);
// iteration-G150: how many times boot_run() re-entered the guest run after the engine's block budget ended it (0 in a normal session shorter than ~65 minutes of racing).
uint32_t guest_run_resumes();   // G150

// The guest HEAP region (HeapAlloc/Local*/Global* backing, 16 MiB at
// 0x01000000 -- see ignition_memlayout.cpp). Exposed so other shims can
// query which allocation contains a given guest address, via the region's
// own GuestRegion::block_of()/size_of() -- iteration 14's ReadFile
// destination-buffer bounds check uses this instead of duplicating the
// region's own bookkeeping in a second tracker.
wx86::GuestRegion& ignition_heap_region();

} // namespace ign
