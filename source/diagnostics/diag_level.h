// diagnostics/diag_level.h -- the compile-time diagnostics level (G138).
//
//   IGN_DIAGNOSTICS_LIGHT  (default; the normal VPK): no per-call / per-triangle diagnostics on the hot paths. Boot and exit checkpoints, critical errors, the allowlisted
//                          audio / stage lines, the G108 protection, the G137 alpha repair (counters only) and the G138 frame-window telemetry remain.
//   IGN_DIAGNOSTICS_FULL   (future diagnostic VPKs only): everything the G137 build did (G137 ring / bursts / capture, per-triangle timers, G67-G103 historical hooks, the G136 pause probe, the
//                          call-trace intrinsic) plus the same G138 telemetry, so a FULL build is "G137 behaviour with block metrics".
//
// The level is selected ONLY at build time (CMake option IGN_DIAGNOSTICS=LIGHT|FULL -> a compile definition). No file, flag or setting is consulted while the game runs.
#pragma once

#if defined(IGN_DIAGNOSTICS_FULL) && defined(IGN_DIAGNOSTICS_LIGHT)
#error "define exactly one of IGN_DIAGNOSTICS_LIGHT / IGN_DIAGNOSTICS_FULL"
#endif
#if !defined(IGN_DIAGNOSTICS_FULL) && !defined(IGN_DIAGNOSTICS_LIGHT)
#define IGN_DIAGNOSTICS_LIGHT 1
#endif

#ifdef IGN_DIAGNOSTICS_FULL
#define IGN_DIAG_FULL 1
#else
#define IGN_DIAG_FULL 0
#endif
