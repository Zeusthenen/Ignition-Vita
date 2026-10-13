// src/runtime/win32_shims_user32.h — generic USER32 shims: pure geometry
// (RECT ops), string helpers, and honest no-op stubs. No literal, path, game
// window state, or behavior specific to any one guest.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_user32_install(d2rt::Bridge& br);
