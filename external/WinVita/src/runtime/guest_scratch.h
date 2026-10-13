// src/runtime/guest_scratch.h — guest scratch allocator.
//
// Engine-level primitive, not consumer code: a good chunk of the Win32 API
// returns a POINTER to memory the caller never frees (GetCommandLineA,
// GetEnvironmentStrings, inet_ntoa, gethostbyname, the
// RTL_CRITICAL_SECTION_DEBUG block...). On real hardware that memory
// belongs to a system DLL; here there is no system DLL, so the engine must
// own where these return values live. The pointer must be a GUEST address
// (32-bit x86, readable by the game), not a host address — host new/malloc
// won't do. Any port onto this engine has the same need, so it lives here
// rather than being reimplemented per port.
//
// ROLE SPLIT. The ENGINE owns the allocator (allocation, bounds, OOM
// signaling). The CONSUMER declares the range it grants
// (wx86_scratch_init) — the memory layout itself is project-specific and
// not universal.
//
// CONTRACT: allocation is PERMANENT. Nothing is ever freed, there is no
// free, and there will not be one. This allocator is reserved for
// process-lifetime return values — typically a constant string or a
// structure allocated ONCE and cached by the caller. Allocating here on
// every call on a hot path is a bug, not a use case: that's how the range
// gets exhausted in a long session. For bounded-lifetime memory, the
// embedder has its own guest heaps.
#pragma once
#include <cstdint>

namespace d2rt { class Cpu; }

// Declares the guest range granted to the allocator: base is the guest
// address of the first usable byte, size is the size in bytes. The pointer
// resets to base on every call, which an embedder that switches memory
// layout needs.
//
// size == 0 EXPLICITLY means "unbounded range": allocate without checking
// the limit. Kept because an embedder often knows its start address well
// before it knows its size (the guest region may only be sized once it is
// mapped out). Arm the bound later, once known, via
// wx86_scratch_set_limit — WITHOUT touching the pointer, so nothing
// already allocated is lost.
void wx86_scratch_init(uint32_t base, uint32_t size);

// Arms (or moves) the upper bound; current pointer UNCHANGED. limit is the
// guest address of the first byte OUTSIDE the range; 0 clears it.
void wx86_scratch_set_limit(uint32_t limit);

// Allocates n bytes (rounded up to 8) and returns the guest address, or 0
// if the bounded range is exhausted. Returning 0 is far safer for callers
// than silently overflowing into the neighboring region.
uint32_t wx86_scratch_alloc(uint32_t n);

// Writes string s (including its terminator) into the scratch range and
// returns its guest address, or 0 if the allocation fails. The shortcut
// every shim that returns a char* needs.
uint32_t wx86_scratch_put_cstr(d2rt::Cpu& c, const char* s);

// State, for the embedder's diagnostics (occupancy, end-of-session
// summary). used = bytes consumed since base; size = range granted.
uint32_t wx86_scratch_used();
uint32_t wx86_scratch_size();
uint32_t wx86_scratch_base();

// OOM signal. The engine is silent by construction (it knows nothing of
// the embedder's log, console, or format): it invokes this callback AT
// MOST ONCE, on the first refused allocation, with the current address,
// the size requested, and the limit. The embedder turns that into a log
// line. No handler registered = silent exhaustion, allocation still
// returns 0.
typedef void (*Wx86ScratchOomFn)(uint32_t at, uint32_t want, uint32_t limit);
void wx86_scratch_set_oom_handler(Wx86ScratchOomFn cb);
