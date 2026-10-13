// win32/ddraw.h — minimal, real IDirectDraw COM object for Ignition.
//
// Scope for this iteration: DirectDrawCreate returns a genuine guest
// x86-visible COM object (object+vtable via ignition_guest_alloc, methods
// wired through Bridge::register_shim/shim_trap -- WinVita's own
// guest-callable-native mechanism, no hand-written trampoline). Implemented:
// QueryInterface/AddRef/Release, SetCooperativeLevel, SetDisplayMode,
// CreateSurface (which itself returns a real IDirectDrawSurface object,
// same construction). Every OTHER method on both interfaces is a
// diagnostic trap: logs this/EIP/ESP and halts cleanly the first time
// Ignition calls it, so the exact next blocker shows up in the log instead
// of being guessed at.
//
// Vtable layout confidence: IDirectDraw's order/offsets were cross-checked
// against Ign_d3d.exe's own disassembly (SetCooperativeLevel at vtable+0x50,
// SetDisplayMode at +0x54, CreateSurface at +0x18 -- all confirmed against
// real CALL [reg+offset] sites, not assumed). IDirectDrawSurface's order
// was NOT verified the same way this iteration (no surface method call has
// been disassembled yet) -- it's the standard DirectX 2/3-era order, and a
// wrong slot there is still safe (every slot is a diagnostic trap unless
// overridden), just possibly mislabeled in the log until the next disassembly
// pass confirms it.
#pragma once

namespace d2rt { class Bridge; class Cpu; }

namespace ign {

// Registers DDRAW.dll!DirectDrawCreate (overriding both the engine's own
// honest-failure stub in win32_shims_misc.cpp and this project's previous
// "stop here" placeholder) and builds the shared IDirectDraw/
// IDirectDrawSurface vtables. Call once, after the engine's own DDRAW stub
// is installed (win32_shims_misc_install) so "last registration wins"
// makes this the one that sticks.
void ddraw_install(d2rt::Bridge& br, d2rt::Cpu& cpu);

} // namespace ign
