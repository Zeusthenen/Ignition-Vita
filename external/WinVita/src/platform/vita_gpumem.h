// GPU memory for the console (kernel blocks + GXM).
//
// Console-specific rather than generic or game-specific: allocating a kernel
// block, mapping it for the GPU, and mapping it into USSE space doesn't depend
// on which game is running. Consumers share the same pattern — prefer CDRAM,
// fall back to user RAM, log every failure — because free user RAM is
// measured in MiB and a silently null pointer causes an unexplained black
// screen.
//
// Sizes, formats, and what goes in the blocks are the caller's decision; this
// file only allocates and frees memory.
//
// Bodies reference sceGxm* and live in a separate archive, so a port that
// never calls these functions doesn't need to link the GXM stub — this lets
// the file coexist with a port that goes through an OpenGL layer instead.
#pragma once
#include <cstdint>

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>

namespace wx86 {
namespace vita {

// A GPU-visible memory block. `uid` < 0 means empty.
struct GpuBlock {
    SceUID   uid   = -1;
    void*    p     = nullptr;
    uint32_t size  = 0;
    bool     cdram = false;
};

inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

// Allocates a block of the requested TYPE and maps it for the GPU.
//
// Every return code is checked: an unexplained freeze must not be confused
// with an allocation that silently failed and left a null pointer in use. On
// failure this logs the exact call and its rc and returns false; the caller
// must stop.
bool gpu_alloc(GpuBlock& b, SceKernelMemBlockType type, uint32_t size,
               uint32_t attribs, const char* name);

// Allocates wherever is best: CDRAM if preferred (default), falling back to
// uncached user RAM. CDRAM is GPU memory and stays plentiful when user RAM
// doesn't.
bool gpu_alloc_best(GpuBlock& b, uint32_t size, uint32_t attribs, const char* name);

// CDRAM preference toggle. The getter exists so a caller that wants to
// display the current setting doesn't keep its own copy of the flag — two
// sources of truth for one piece of state always end up diverging; this is
// the only place the preference lives.
void gpu_prefer_cdram(bool on);
bool gpu_prefers_cdram();

// USSE memory. The SDK's USSE allocation helpers are sample code, not part of
// the library, so the block must be allocated manually and then mapped into
// USSE space.
// USSE does not accept CDRAM — these blocks stay in user RAM.
bool usse_alloc(GpuBlock& b, uint32_t size, bool fragment, unsigned int* offset,
                const char* name);

void gpu_free(GpuBlock& b);

// Free memory, in KB. On this console the number itself is the diagnostic:
// it shows which init stage ran out of room.
void mem_free_kb(int* user_kb, int* cdram_kb);

}  // namespace vita
}  // namespace wx86

#endif  // __vita__
