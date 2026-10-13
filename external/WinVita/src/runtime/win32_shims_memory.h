// src/runtime/win32_shims_memory.h — the memory map as seen by the guest
// program: heap (Heap/Local/Global), virtual-address arena (Virtual*),
// region inventory (VirtualQuery), and machine description (GetSystemInfo,
// GlobalMemoryStatus).
//
// Why this lives in the engine: a Win32 program allocates and frees; on a
// real machine the OS answers, here the engine stands in for the OS. The
// bodies gathered here are byte-identical across both consumers of this
// engine (checked one by one; the only difference is the NAME of an
// allocation alias).
//
// What stays with the consumer, and why: the PLAN itself — where the heap
// starts, how large it is, where the arena lives, how regions are described.
// That depends on the target platform and port (guest_region.h already says
// so for the allocator). The consumer supplies it via Wx86MemoryPlan.
//
// Instrumentation does not follow the bodies: occupancy counters, log lines,
// naming the faulting site in a crash log — none of that DECIDES anything.
// These are observers, reached through wx86_mem_set_observer(). The engine
// reports; it never asks for permission — same rule as the synchronization
// observer (guest_sync.h) and the network observer.
#pragma once
#include <cstdint>

namespace d2rt { class Bridge; class Cpu; }
namespace wx86 { class GuestRegion; }

// ---- Zero-filling a GUEST range ---------------------------------------------
// Lives here because it's a Win32 guarantee these shims make (HEAP_ZERO_MEMORY,
// LMEM_ZEROINIT, MEM_COMMIT). Goes through hostptr plus a code-invalidation
// barrier when possible (equivalent to a write() loop, minus the virtual
// call and per-chunk copy each iteration), falls back to write() otherwise.
void wx86_gzero(d2rt::Cpu& c, uint32_t va, uint32_t n);

// ---- The plan the consumer provides -----------------------------------------
struct Wx86MemoryPlan {
    // Region backing the guest heap (HeapAlloc/HeapFree/Local*/Global*).
    wx86::GuestRegion* heap = nullptr;
    // Region backing the virtual-address arena (VirtualAlloc/VirtualFree).
    wx86::GuestRegion* va   = nullptr;
    // Describes a region for VirtualQuery. The engine knows the FORMAT
    // (MEMORY_BASIC_INFORMATION, 28 bytes) but not the plan itself. Returns
    // false for anything Windows itself refuses to introspect (kernel space).
    bool (*vquery)(uint32_t addr, uint32_t& base, uint32_t& size, uint32_t& state,
                   uint32_t& type, uint32_t& protect, uint32_t& allocbase) = nullptr;
};

// ---- Observer -----------------------------------------------------------------
enum {
    WX86_MEM_VA_COMMIT_IN = 0,  // MEM_COMMIT inside an existing reservation
    WX86_MEM_VA_RESERVE,        // MEM_RESERVE (outside any existing reservation)
    WX86_MEM_VA_COMMIT_FRESH,   // MEM_COMMIT outside any reservation
    WX86_MEM_VA_BIG,            // request >= 1 MiB: worth a log line
    WX86_MEM_VA_GARBAGE,        // request >= 2 GiB: clearly a corrupted size
    WX86_MEM_VA_FAIL,           // the arena refused
    WX86_MEM_VA_RELEASE,        // MEM_RELEASE honored
    WX86_MEM_VA_RELEASE_MISS,   // MEM_RELEASE missed its target: arena leak
    WX86_MEM_VA_DECOMMIT,       // MEM_DECOMMIT (backing kept, a later recommit zeroes it)
};
struct WxMemEvent {
    int         kind;
    d2rt::Cpu*  cpu;      // CPU at the call site
    uint32_t    addr;     // address involved (hint, freed block, first page)
    uint32_t    size;     // bytes involved
    uint32_t    type;     // original dwAllocationType / dwFreeType
    uint32_t    protect;  // original flProtect (0 when not applicable)
    uint32_t    ra;       // guest caller's return address (0 if not read)
};
typedef void (*WxMemObserverFn)(const WxMemEvent&);
// A SINGLE observer, as elsewhere: a second call REPLACES the first rather
// than silently adding to it.
void wx86_mem_set_observer(WxMemObserverFn cb);

// ---- Reported physical memory ------------------------------------------------
// GlobalMemoryStatus tells the guest how much RAM the machine has. Many
// games size their caches off this, so the value is a DECISION for the
// consumer to make, not a universal constant — it sets it here. Default
// 256 MiB.
void wx86_mem_set_total_phys_mb(uint32_t mb);

void win32_shims_memory_install(d2rt::Bridge& br, const Wx86MemoryPlan& plan);
