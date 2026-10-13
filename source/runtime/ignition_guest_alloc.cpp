#include "runtime/ignition_guest_alloc.h"
#include "runtime/guest_scratch.h"
#include "diagnostics/log.h"

namespace ign {

uint32_t ignition_guest_alloc(uint32_t size, uint32_t alignment) {
    if (alignment < 4) alignment = 4;
    uint32_t padded = ((size + alignment - 1) / alignment) * alignment;
    uint32_t addr = wx86_scratch_alloc(padded);
    if (!addr) {
        IGN_LOG("[COM]", "guest alloc size=%u align=%u -> FAILED (scratch range exhausted)", size, alignment);
        return 0;
    }
    if (addr % alignment != 0) {
        // wx86_scratch_alloc rounds to 8 bytes internally (see guest_scratch.h),
        // which already satisfies every alignment this project asks for (4).
        // Logged rather than silently trusted, in case that assumption ever
        // stops holding for a larger alignment request.
        IGN_LOG("[ERROR]", "guest alloc size=%u align=%u -> 0x%08x is NOT aligned", size, alignment, addr);
    }
    IGN_LOG("[COM]", "guest alloc size=%u align=%u -> 0x%08x", size, alignment, addr);
    return addr;
}

uint32_t ignition_guest_alloc_pixels(uint32_t size) {
    static uint32_t s_total = 0;
    uint32_t addr = wx86_scratch_alloc(size);
    if (!addr) {
        IGN_LOG("[SURFACE]", "pixel alloc size=%u -> FAILED (scratch range exhausted, %u bytes used so far)", size, s_total);
        return 0;
    }
    s_total += size;
    IGN_LOG("[SURFACE]", "pixel alloc size=%u -> 0x%08x (pixel pool total so far: %u bytes)", size, addr, s_total);
    return addr;
}

} // namespace ign
