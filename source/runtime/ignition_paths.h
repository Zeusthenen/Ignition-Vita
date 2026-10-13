// runtime/ignition_paths.h — guest (Windows) path -> host (ux0:) path.
//
// WinVita's guest_files.h explicitly leaves path resolution to the
// consumer ("where data lives, platform naming conventions, root"). This is
// that policy for Ignition specifically.
#pragma once
#include <string>

namespace ign {

// Every guest path resolves somewhere under here.
inline constexpr const char* kDataRoot = "ux0:data/ignition/";

// iteration-G74, Parte 8-13: shared gate for the VERBOSE, per-call file
// diagnostic dumps this project's own logger implementation (diagnostics/
// log.cpp's log_line -- see its own comment) turns into a full
// fopen(append)+fprintf+fclose cycle EVERY call, with zero buffering, by
// deliberate design (crash survivability). During file-heavy operations
// (e.g. level/track loading) those per-call dumps multiply into hundreds
// of extra open/write/close cycles on real Vita storage -- suspected,
// this iteration, of being a large and previously unmeasured source of
// apparent "stall" time. This flag gates ONLY the verbose diagnostic
// TEXT (translate_path's own [PATH] line here, plus win32/
// kernel32_ign.cpp's CreateFileA/ReadFile/SetFilePointer per-call field
// dumps and routine HEAPCHECK success lines) -- it never changes any
// actual file-open/read/seek/close call, return value, or guest-visible
// behavior; only log volume differs. Errors, failures, and safety-check
// halts remain logged unconditionally either way. Default false for this
// iteration's own A/B test (see the G74 BUILD markers); trivially
// flippable back to true to restore G73's full verbosity.
inline constexpr bool kVerboseFileIoDiagnostics = false;

// Translates a guest path from the game (as passed to CreateFileA and
// friends) into a host path under kDataRoot:
//   - accepts both '\\' and '/' as separators;
//   - strips a leading drive letter ("C:\\..." / "C:/...") if present;
//   - drops any leading separators left after that;
//   - otherwise preserves the relative structure, so
//     "CARS\\car1.dat" -> "ux0:data/ignition/CARS/car1.dat".
// Logs every translation under "[PATH]" so a wrong guess about the game's
// on-disk layout shows up in the log instead of failing silently deep
// inside a shim.
std::string translate_path(const char* guest_path);

} // namespace ign
