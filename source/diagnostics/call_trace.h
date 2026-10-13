// diagnostics/call_trace.h — logs the FIRST call to every resolved import,
// to reconstruct the CRT/WinMain bootstrap sequence from the log alone.
//
// Mechanism: Cpu::set_intrinsic(slot_va, fn) (runtime/cpu.h) lets an
// observer run BEFORE Bridge's normal shim dispatch and defer to it by
// returning false -- "the guest observes NOTHING new... this only makes
// servicing the trap cheaper [or, here, observed]". Using it means this
// file never touches Bridge's own shim table and can't change behavior,
// only watch it -- exactly the "no refactor, no invented API" contract for
// this iteration.
#pragma once
#include <cstdint>
#include <vector>
#include "runtime/pe_image.h"
#include "diagnostics/diag_level.h"   // iteration-G138: LIGHT (default) / FULL

namespace d2rt { class Bridge; class Cpu; }

namespace ign {

// Arms the trace: for every NAMED import in `imports` that already has a
// shim registered (checked via Bridge::shim_trap, which returns 0 for
// anything unresolved -- so this never allocates a trap for an import that
// would otherwise become one of the "missing" ones), installs the shared
// logging intrinsic on its trap slot. Ordinal imports are skipped (no
// public API to pre-resolve their trap VA before Bridge::link()); an
// ordinal import that's actually unresolved is still caught by the
// [STOP] path regardless. Call once, after every shim is registered
// (engine's and Ignition's own) and before Bridge::commit()/link().
void arm_call_trace(d2rt::Bridge& br, d2rt::Cpu& cpu, const std::vector<d2rt::ImportRef>& imports);

// iteration-G67: dumps (once, chronological order) the RAM-only ring of
// every host-call-boundary change observed at the fixed guest addresses
// holding PATH_B's V3.r/g/b stack residue (see call_trace.cpp's own G67
// comment). Called from glide2x_ign.cpp's existing one-shot [G47-
// NONFINITE] failure branch, not on a hot path.
void dump_g67_watch_ring();

// iteration-G68: last watch sample (independent of whether it produced
// a ring CHANGE event), for direct comparison against an explicit
// caller-side read at the exact moment of interest. false if the watch
// has not sampled anything yet.
bool g68_get_last_watch_sample(uint32_t& r, uint32_t& g, uint32_t& b);

// iteration-G68, Parte 21-22/24: G68_DIAGNOSTIC_ONLY. Feeds the same
// ring/audit as the generic call-trace watch, from a small hand-picked
// set of already-implemented Glide _impl entry points -- insurance in
// case that generic mechanism has a real coverage gap for glide2x.dll
// (see call_trace.cpp's own G68 comment). `label` should be a static
// string literal (stored by pointer, not copied).
// iteration-G138: an empty inline in the LIGHT build (the call sites in glide2x_ign.cpp then cost nothing); the real function only exists in FULL.
#if IGN_DIAG_FULL
void g68_explicit_sample(d2rt::Cpu& c, const char* label);
#else
inline void g68_explicit_sample(d2rt::Cpu&, const char*) {}
#endif

} // namespace ign
