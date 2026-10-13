// src/runtime/win32_shims_misc.h — generic "honest failure/no-op" stubs for
// a handful of small, unrelated DLLs (DirectDraw, Bink/Smacker video,
// ijl11 JPEG codec, IMM32 IME): each says "unavailable" or "not implemented"
// the way a real absent driver/codec/input-method would, no state, no
// literal specific to any one guest.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_misc_install(d2rt::Bridge& br);
