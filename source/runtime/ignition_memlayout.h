// runtime/ignition_memlayout.h — the whole guest memory plan, in one place.
//
// Background (real-hardware finding, see README "Phase 1b"): on the ARM
// dynarec backend, Cpu::map() only works correctly in what cpu_box86.cpp
// calls "arena mode" -- a SINGLE physical host block backs every guest
// address as H(va) = va + membase. Arena mode is armed exclusively by the
// WX86_ARENA environment variable, read ONCE in the CPU backend's
// constructor; without it, map() falls back to a per-region host mmap()
// that Vita's own mmap() shim (src/dynarec86/shim/vita/mman_vita.c)
// structurally cannot satisfy (it never honors a requested address), so it
// always fails -- exactly the "cpu->map() failed" seen on the first real
// test. Confirmed by reading cpu_box86.cpp and mman_vita.c, not guessed.
//
// Consequence: EVERY guest address actually touched -- our heap/VA/scratch,
// but also Bridge's own guest stack and native-thunk trap window -- must
// fit inside that one physical block. Bridge's built-in defaults for those
// two (0x60000000 / 0x7F000000) don't; bridge.cpp documents exactly this
// case and exposes WX86_STACKBASE/WX86_TRAPBASE/WX86_MODBASE overrides for
// "a host that already has its own memory map ... calibrated for its own
// game". This file is that: our own values, not D2Vita's Diablo-II-shaped
// presets (WX86_LAYOUT=compact/haut), chosen to clear Ignition's own PE
// image, which loads at its own fixed preferred base (confirmed on real
// hardware: 0x00400000..0x0066E000) and is never touched here.
#pragma once
#include <cstdint>

namespace d2rt { class Cpu; }
namespace wx86 { class GuestRegion; }

namespace ign {

struct MemLayout {
    uint32_t heap_base,    heap_size;      // HeapAlloc/Local*/Global* backing
    uint32_t va_base,      va_size;        // VirtualAlloc arena
    uint32_t scratch_base, scratch_size;   // permanent guest-scratch (GetCommandLineA, ...)
    uint32_t mod_base,     mod_size;       // WX86_MODBASE: headroom for a future add_module(); unused this phase
    uint32_t stack_base,   stack_size;     // WX86_STACKBASE: mapped by Bridge::commit(), not by us
    uint32_t trap_base,    trap_size;      // WX86_TRAPBASE: native-thunk window, needs no backing memory
    uint32_t arena_size;                   // WX86_ARENA request (must exceed every region's top)
};

const MemLayout& ignition_mem_layout();

// Sets WX86_ARENA/WX86_MODBASE/WX86_STACKBASE/WX86_TRAPBASE via setenv().
// MUST run before make_cpu_box86() (WX86_ARENA is read in its constructor)
// and before constructing Bridge (the other three are read in Bridge's
// constructor) -- both read their variable exactly once.
void apply_memory_layout_env();

// Logs the full "GUEST MEMORY LAYOUT" table -- including the PE image,
// passed in since only the caller knows it -- and every pairwise overlap
// check between all seven regions. Returns false (having logged [ERROR])
// if any two overlap; callers must not proceed past that.
bool log_and_check_layout(uint32_t exeBase, uint32_t exeSize);

// Maps HEAP, VA and SCRATCH independently, each with its own [MAP] log
// line before and after (address range, page alignment, result) -- never
// grouped behind one shared condition, so a real-hardware log always says
// exactly which region failed. PE mapping happens inside Bridge::commit();
// the stack is likewise Bridge's job once WX86_STACKBASE is set; the trap
// window needs no backing at all (it's a jump-target range, never
// dereferenced as data). Returns false on the first failure.
bool map_layout_regions(d2rt::Cpu& cpu, wx86::GuestRegion& heap, wx86::GuestRegion& va);

} // namespace ign
