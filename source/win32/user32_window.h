// win32/user32_window.h — the minimal Win32 window/message-loop subsystem
// WinVita deliberately leaves to the consumer (confirmed by reading
// win32_shims_window.cpp's own header comment: "RegisterClassA/
// CreateWindowExA... not a generic winx86 primitive" and "ShowWindow/
// PeekMessageA/GetMessageA/DispatchMessageA... stays put").
//
// Scope for this iteration: exactly one guest window, one registered class,
// a small FIFO message queue, and synchronous WM_CREATE/WM_SETFOCUS dispatch
// through the guest WNDPROC (real code, invoked via Bridge::call_va -- the
// same reusable mechanism the engine already uses for guest calls, per
// bridge.h's own note that CreateWindowExA-style synchronous WM_CREATE
// dispatch is an anticipated pattern). No DirectDraw, no input, no real
// rendering -- see ignition_boot.cpp for where DirectDrawCreate is caught
// and the run stops cleanly.
#pragma once

namespace d2rt { class Bridge; }

namespace ign {

// Registers RegisterClassA, CreateWindowExA, ShowWindow, SetFocus,
// SetWindowLongA/GetWindowLongA, PostMessageA, PeekMessageA, GetMessageA,
// DispatchMessageA, and an overriding PostQuitMessage (the engine's own
// win32_shims_window.cpp version is a no-op that never actually arms a
// quit condition a message loop could observe -- "last registration wins"
// applies here deliberately). Call AFTER win32_shims_window_install so the
// override takes effect.
void user32_window_install(d2rt::Bridge& br);

} // namespace ign
