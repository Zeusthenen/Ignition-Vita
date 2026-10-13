// win32/dinput.h — minimal, real IDirectInputA/IDirectInputDeviceA COM
// objects for Ignition, built the same way as win32/ddraw.h: guest
// x86-visible object+vtable via ignition_guest_alloc, methods wired
// through Bridge::register_shim/shim_trap.
//
// Scope for this iteration, all confirmed by real disassembly of
// Ign_d3d.exe (see dinput.cpp for the exact evidence): DirectInputCreateA
// (dwVersion=0x0300, classic DirectInput 3), IDirectInputA::CreateDevice,
// and the full first-device bootstrap chain SetDataFormat ->
// SetCooperativeLevel -> SetProperty(DIPROP_BUFFERSIZE) -> Acquire. Every
// other method on both interfaces is a diagnostic trap.
#pragma once

namespace d2rt { class Bridge; class Cpu; }

namespace ign {

// Registers DINPUT.dll!DirectInputCreateA and builds the shared
// IDirectInputA/IDirectInputDeviceA vtables.
void dinput_install(d2rt::Bridge& br, d2rt::Cpu& cpu);

// iteration-G29, Parte I/J (G30: multi-hooked, see below): samples real
// Vita physical input and feeds any acquired keyboard device's buffered-
// event queue -- decoupled from GetDeviceData (G27/G28 called this from
// there, but G29's real-hardware log showed the guest can stop calling
// GetDeviceData for minutes while still rendering live frames).
//
// iteration-G30: G29's own real-hardware run then showed grBufferSwap
// ALONE is far too sparse at this screen (~27s between swaps) -- a
// per-swap poll could miss every physical press between swaps entirely.
// Per G30's explicit instruction NOT to spawn a background thread yet
// (would risk racing guest/DirectInput state), this is now called from
// every one of this project's own central per-call wrapper points
// (kernel32/user32/winmm/dinput's own shim-counting closures, PLUS
// grBufferSwap) -- still fully synchronous on the single host thread that
// runs the guest, just hooked at every point that thread already passes
// through host C++ code. Takes Cpu& so it can passively read guest global
// memory (e.g. the [0x00491A48] one-shot flag G29's disassembly found) --
// a plain read, the same safe pattern this project already uses elsewhere
// (e.g. kernel32_ign.cpp's calloc-count peek), never a write.
void dinput_poll_vita_input(d2rt::Cpu& c);

} // namespace ign
