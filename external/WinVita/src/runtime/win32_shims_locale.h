// src/runtime/win32_shims_locale.h — KERNEL32 shims for LOCALE, STRING, and
// CONSOLE handling.
//
// Why these three sub-groups share a file: they all need to read or write a
// string at a GUEST address (guest_str.h), and sometimes stash one in guest
// scratch space (guest_scratch.h). Both primitives live in the engine,
// which is what lets these bodies live here too.
//
// Genericity here is observed, not assumed: all bodies are byte-identical
// (ignoring comments/whitespace) to the second consumer of this engine,
// which ports an unrelated game — checked one by one, not sampled. The few
// that differ only in their LCID accessor (wx86_locale_lcid() here, a plain
// global there) carry the same semantics; that consumer just hasn't
// migrated to the accessor yet.
//
// What stays out: GetPrivateProfileStringA/IntA, which live in the same
// area on the consumer side, because their body reads a real .ini file via
// a consumer-owned lookup service — a named dependency, not caution.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_locale_install(d2rt::Bridge& br);
