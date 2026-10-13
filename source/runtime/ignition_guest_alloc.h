// runtime/ignition_guest_alloc.h — small, permanent guest allocations for
// runtime-created objects a shim needs to hand the guest a real x86
// address for (COM objects, their vtables, ...).
//
// Deliberately a thin wrapper over WinVita's own guest_scratch allocator
// (runtime/guest_scratch.h) rather than a second allocator: guest_scratch's
// contract -- ONE range, PERMANENT allocation, never freed, for
// process-lifetime objects -- is exactly what a COM object and its vtable
// need (they live for the whole run), and reusing it means every "how much
// guest memory is left" question has one answer instead of two.
#pragma once
#include <cstdint>

namespace ign {

// Allocates `size` bytes aligned to `alignment` (>=4; x86 pointers/vtable
// slots never need more) and logs the result, so guest object addresses
// stay auditable in the log instead of scattered magic numbers. Returns 0
// on failure (scratch range exhausted) -- callers must check, same as
// wx86_scratch_alloc itself.
uint32_t ignition_guest_alloc(uint32_t size, uint32_t alignment = 4);

// Same underlying pool as ignition_guest_alloc() (WinVita's guest_scratch --
// there's only one permanent-allocation arena, splitting it would just mean
// two "is it exhausted" questions instead of one), but tagged and tracked
// separately under "[SURFACE]" so pixel-buffer usage is visible on its own
// in the log as it grows (two 640x480x8 buffers is ~600 KiB out of the 4 MiB
// scratch range -- comfortable now, but worth watching if more/larger
// surfaces show up later). If this pool ever needs to be genuinely
// separate (its own Cpu::map()ed region), this is the one call site that
// would need to change.
uint32_t ignition_guest_alloc_pixels(uint32_t size);

} // namespace ign
