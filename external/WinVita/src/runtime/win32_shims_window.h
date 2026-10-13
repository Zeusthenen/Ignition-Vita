// src/runtime/win32_shims_window.h — generic USER32 window/cursor/paint
// shims: fake handles, honest no-ops, and pure geometry — no wndproc
// dispatch, no message queue. See win32_shims_window.cpp for the boundary
// rationale (what stayed d2vita-side and why).
#pragma once
#include <cstdint>
namespace d2rt { class Bridge; }

void win32_shims_window_install(d2rt::Bridge& br);

// Generic mutable virtual-cursor position, clamped to the display size
// (win32_shims_gdi32.h's wx86_set_screen_size). A plain config value the
// consumer's own input-injection code can drive directly — same pattern as
// the screen-size accessor, not a second shim registration.
void wx86_set_cursor(uint32_t x, uint32_t y);
void wx86_get_cursor(uint32_t* x, uint32_t* y);
