// src/runtime/win32_shims_shell32.h — generic SHELL32 shims: no literal,
// path, or behavior specific to any one guest. Registered via
// Bridge::register_shim, same mechanism a consumer uses to override any of
// these for its own game (see Bridge::register_shim overwrite-on-duplicate-
// key semantics in bridge.h).
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_shell32_install(d2rt::Bridge& br);
