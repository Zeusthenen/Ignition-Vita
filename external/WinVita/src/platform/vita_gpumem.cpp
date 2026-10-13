// See vita_gpumem.h.
//
// These bodies are a direct transcription of the existing allocation code: same
// alignment granularities, same fallbacks, same failure messages. Moving the
// ownership of code must never be mixed with changing its behavior, or
// nobody can tell what broke.
#ifdef __vita__
#include "platform/vita_gpumem.h"
#include "platform/vita_host.h"

#include <cstdio>

namespace wx86 {
namespace vita {

namespace {
bool g_prefer_cdram = true;
}

void gpu_prefer_cdram(bool on) { g_prefer_cdram = on; }
bool gpu_prefers_cdram() { return g_prefer_cdram; }

bool gpu_alloc(GpuBlock& b, SceKernelMemBlockType type, uint32_t size,
               uint32_t attribs, const char* name) {
    const bool isCd = (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
    // CDRAM aligns to 256 KiB, user RAM to 4 KiB.
    const uint32_t gran = isCd ? (256u << 10) : (4u << 10);
    b.size  = align_up(size, gran);
    b.cdram = isCd;
    b.uid   = sceKernelAllocMemBlock(name, type, b.size, nullptr);
    if (b.uid < 0) {
        char m[144];
        std::snprintf(m, sizeof m, "gpumem: AllocMemBlock(%s, %s, %u Ko) rc=0x%08x",
                      name, isCd ? "CDRAM" : "user", b.size >> 10, (unsigned)b.uid);
        wx86_vita_progress(m);
        b.uid = -1;
        return false;
    }
    const int grc = sceKernelGetMemBlockBase(b.uid, &b.p);
    if (grc < 0) {
        char m[128];
        std::snprintf(m, sizeof m, "gpumem: GetMemBlockBase(%s) rc=0x%08x", name, (unsigned)grc);
        wx86_vita_progress(m);
        sceKernelFreeMemBlock(b.uid);
        b.uid = -1; b.p = nullptr;
        return false;
    }
    const int mrc = sceGxmMapMemory(b.p, b.size, (SceGxmMemoryAttribFlags)attribs);
    if (mrc < 0) {
        char m[128];
        std::snprintf(m, sizeof m, "gpumem: sceGxmMapMemory(%s, %u Ko) rc=0x%08x",
                      name, b.size >> 10, (unsigned)mrc);
        wx86_vita_progress(m);
        sceKernelFreeMemBlock(b.uid);
        b.uid = -1; b.p = nullptr;
        return false;
    }
    return true;
}

bool gpu_alloc_best(GpuBlock& b, uint32_t size, uint32_t attribs, const char* name) {
    if (g_prefer_cdram &&
        gpu_alloc(b, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, attribs, name))
        return true;
    return gpu_alloc(b, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, attribs, name);
}

bool usse_alloc(GpuBlock& b, uint32_t size, bool fragment, unsigned int* offset,
                const char* name) {
    b.size = align_up(size, 4096);
    b.uid  = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                    b.size, nullptr);
    if (b.uid < 0) {
        char m[128];
        std::snprintf(m, sizeof m, "gpumem: AllocMemBlock USSE(%s, %u Ko) rc=0x%08x",
                      name, b.size >> 10, (unsigned)b.uid);
        wx86_vita_progress(m);
        b.uid = -1;
        return false;
    }
    const int grc = sceKernelGetMemBlockBase(b.uid, &b.p);
    if (grc < 0) {
        char m[128];
        std::snprintf(m, sizeof m, "gpumem: GetMemBlockBase USSE(%s) rc=0x%08x",
                      name, (unsigned)grc);
        wx86_vita_progress(m);
        sceKernelFreeMemBlock(b.uid);
        b.uid = -1; b.p = nullptr;
        return false;
    }
    const int rc = fragment ? sceGxmMapFragmentUsseMemory(b.p, b.size, offset)
                            : sceGxmMapVertexUsseMemory(b.p, b.size, offset);
    if (rc < 0) {
        char m[128];
        std::snprintf(m, sizeof m, "gpumem: Map%sUsseMemory(%s, %u Ko) rc=0x%08x",
                      fragment ? "Fragment" : "Vertex", name, b.size >> 10, (unsigned)rc);
        wx86_vita_progress(m);
        sceKernelFreeMemBlock(b.uid);
        b.uid = -1; b.p = nullptr;
        return false;
    }
    return true;
}

void gpu_free(GpuBlock& b) {
    if (b.uid < 0) return;
    if (b.p) sceGxmUnmapMemory(b.p);
    sceKernelFreeMemBlock(b.uid);
    b.uid = -1; b.p = nullptr; b.size = 0;
}

void mem_free_kb(int* user_kb, int* cdram_kb) {
    SceKernelFreeMemorySizeInfo fi;
    fi.size = sizeof fi;
    sceKernelGetFreeMemorySize(&fi);
    if (user_kb)  *user_kb  = fi.size_user  >> 10;
    if (cdram_kb) *cdram_kb = fi.size_cdram >> 10;
}

}  // namespace vita
}  // namespace wx86

#endif  // __vita__
