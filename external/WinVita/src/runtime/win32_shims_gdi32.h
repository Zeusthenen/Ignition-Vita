// src/runtime/win32_shims_gdi32.h — generic GDI32 shims: fake handles, honest
// no-ops, and pure geometry/palette-table plumbing. No literal, path, or
// behavior specific to any one guest.
//
// Also owns the generic mutable "guest display size" used by these shims
// (GetDeviceCaps HORZRES/VERTRES, ...) and by win32_shims_user32's window
// geometry. Defaults to a neutral placeholder; the consumer sets the real
// value once at boot via wx86_set_screen_size — a plain config value, not a
// second shim registration, so it carries none of the duplicate-registration
// risk a generic/specific shim override would.
#pragma once
#include <cstdint>
namespace d2rt { class Bridge; }

void win32_shims_gdi32_install(d2rt::Bridge& br);

void wx86_set_screen_size(uint32_t w, uint32_t h);
uint32_t wx86_screen_w();
uint32_t wx86_screen_h();
