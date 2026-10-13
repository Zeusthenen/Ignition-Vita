// src/runtime/guest_region.h — guest REGION allocator.
//
// Engine-level primitive, not consumer code: a Win32 program both allocates
// and FREES (HeapAlloc/HeapFree, VirtualAlloc/VirtualFree, LocalAlloc,
// GlobalAlloc). On real hardware the OS heap manager handles this; here
// there is no OS, so the engine must carve a GUEST address range into
// blocks, hand them out, and stitch them back together on free.
//
// Distinct from guest_scratch.h, and the two coexist:
//   * guest_scratch — ONE range, PERMANENT allocation, no free. For
//     process-lifetime return values (a string returned by GetCommandLineA,
//     a cached hostent struct).
//   * guest_region (this file) — MULTIPLE independent ranges, with free,
//     neighbor-block merging, and occupancy tracking. For the guest heap
//     and the virtual-address arena, which live for the whole session.
//
// Any port onto this engine has the same need, so it belongs here rather
// than being reimplemented per port.
//
// ROLE SPLIT. The ENGINE owns the carving (allocate, free, merge, measure).
// The CONSUMER owns the INSTANCES and the ranges they cover: where the guest
// heap starts, how big it is, how many arenas to open — that depends on the
// port's memory layout, which isn't universal. The engine therefore declares
// no global instance; the embedder instantiates whatever it needs.
//
// ALIGNMENT. Each region has its own, fixed at init: 16 bytes for a
// HeapAlloc-style heap, a page (0x1000), or a reservation granule (0x10000)
// for a VirtualAlloc-style arena. The consumer picks it, since only it knows
// what the game expects from MEM_RESERVE.
#pragma once
#include <cstdint>
#include <map>

namespace wx86 {

// OOM signal. The engine is silent by construction — it knows nothing of
// the embedder's log, console, or format — so it invokes this callback on
// every refused allocation, with enough data to tell "region full" from
// "region fragmented": if `largest` is comfortable while `want` is small,
// it's fragmentation, not a lack of space. No handler registered = silent
// refusal, alloc() still returns 0.
struct RegionFail {
    const char* name;      // region name, as passed to init()
    uint32_t    base;      // guest address of the region's first byte
    uint32_t    want;      // bytes requested (raw size, before alignment)
    uint32_t    cur;       // bytes currently live
    uint32_t    peak;      // max bytes live since init
    uint32_t    largest;   // largest single free block
    uint32_t    used;      // sum of allocated blocks (recomputed, cf. used_bytes)
};
typedef void (*RegionFailFn)(const RegionFail&);

// Shared by all regions: the embedder only needs one log, not one callback
// per instance — the `name` field says which region refused.
void region_set_fail_handler(RegionFailFn cb);

class GuestRegion {
public:
    // b = guest address of the first byte, sz = size in bytes, al =
    // alignment of returned blocks, nm = name for diagnostics (must outlive
    // the region: a literal, not a buffer).
    void init(uint32_t b, uint32_t sz, uint32_t al, const char* nm = "region") {
        name_ = nm; base_ = b; limit_ = b + sz; align_ = al; freeb_[b] = sz;
    }

    // Returns the guest address of an n-byte block, or 0. First-fit over
    // the free-block map, which is ordered by address.
    uint32_t alloc(uint32_t n) {
        // (n + align - 1) overflows for n >= 0xFFFFFFF1, which would let a
        // "negative" request succeed with a tiny block. Rejecting anything
        // the region can't hold closes both cases at once.
        if (n > (limit_ - base_)) { record_fail(n); return 0; }
        uint32_t sz = (n + align_ - 1) & ~(align_ - 1);
        if (!sz) sz = align_;
        for (auto it = freeb_.begin(); it != freeb_.end(); ++it)
            if (it->second >= sz) {
                uint32_t a = it->first, rem = it->second - sz;
                freeb_.erase(it);
                if (rem) freeb_[a + sz] = rem;
                used_[a] = sz;
                cur_ += sz; if (cur_ > peak_) peak_ = cur_;
                return a;
            }
        record_fail(n);
        return 0;
    }

    // Returns true if a was the start of an allocated block. The freed
    // block is merged with its right neighbor then its left: without this,
    // a region that allocates and frees in a loop ends up fragmented into
    // unusable blocks while nearly empty.
    bool free(uint32_t a) {
        auto it = used_.find(a); if (it == used_.end()) return false;
        uint32_t sz = it->second; used_.erase(it); cur_ -= sz;
        auto nx = freeb_.lower_bound(a);
        if (nx != freeb_.end() && a + sz == nx->first) { sz += nx->second; nx = freeb_.erase(nx); }
        if (nx != freeb_.begin()) {
            auto pv = std::prev(nx);
            if (pv->first + pv->second == a) { pv->second += sz; return true; }
        }
        freeb_[a] = sz; return true;
    }

    // Base of the block that CONTAINS a, or 0. VirtualFree(MEM_RELEASE)
    // needs this: the game may pass an interior address, not necessarily
    // the start.
    uint32_t block_of(uint32_t a) {
        auto it = used_.upper_bound(a); if (it == used_.begin()) return 0; --it;
        return (a >= it->first && a < it->first + it->second) ? it->first : 0;
    }
    uint32_t size_of(uint32_t a) { auto it = used_.find(a); return it == used_.end() ? 0 : it->second; }
    uint32_t largest_free() { uint32_t m = 0; for (auto& p : freeb_) if (p.second > m) m = p.second; return m; }
    // Recomputes the sum of allocated blocks. cur() gives the same value in
    // O(1); used_bytes() is the reference reading (it can't drift), used
    // for end-of-session diagnostics.
    uint32_t used_bytes() { uint32_t t = 0; for (auto& p : used_) t += p.second; return t; }

    uint32_t cur()  const { return cur_; }
    uint32_t peak() const { return peak_; }
    uint32_t base() const { return base_; }
    // Map of allocated blocks, for surveying the largest occupants ("who's
    // holding the arena?"). Read-only by convention.
    const std::map<uint32_t, uint32_t>& used_map() const { return used_; }

private:
    void record_fail(uint32_t n);

    const char* name_ = "region";
    uint32_t base_ = 0, limit_ = 0, align_ = 16;
    uint32_t cur_ = 0, peak_ = 0;
    std::map<uint32_t, uint32_t> used_, freeb_;   // address -> size
};

} // namespace wx86
