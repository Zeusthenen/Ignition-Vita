// win32/kernel32_ign.h — the KERNEL32 entry points WinVita's engine
// deliberately leaves to the consumer: process identity/exit and basic
// file I/O (see docs/migration.md: "~115 KERNEL32 functions (still
// port-specific)" and the absence of CreateFileA/ExitProcess/GetProcAddress
// anywhere in win32_shims_kernel32.cpp, confirmed by reading it before
// writing this).
#pragma once

#include <string>

namespace d2rt { class Bridge; class PeImage; }

namespace ign {

// Registers GetModuleHandleA/GetModuleFileNameA/GetCommandLineA,
// ExitProcess/TerminateProcess (both call ignition_boot's request_halt),
// and CreateFileA/ReadFile/WriteFile/CloseHandle/GetFileSize/
// SetFilePointer/DeleteFileA translated through ignition_paths.
// `exe` supplies the image base/name GetModuleHandleA(NULL) and
// GetModuleFileNameA answer with.
void kernel32_ign_install(d2rt::Bridge& br, d2rt::PeImage& exe);

// iteration-G15: returns a compact "name(read=N) ..." summary of every
// file read since the last call, then clears its internal per-handle
// counters -- used by win32/glide2x_ign.cpp's per-swap-interval activity
// summary. Never affects file I/O behavior itself.
std::string kernel32_ign_g15_snapshot_file_activity();

} // namespace ign
