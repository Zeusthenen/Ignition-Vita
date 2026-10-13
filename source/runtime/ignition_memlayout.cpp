#include "runtime/ignition_memlayout.h"

#include "runtime/cpu.h"
#include "runtime/guest_region.h"
#include "runtime/guest_scratch.h"
#include "diagnostics/log.h"
#include "diagnostics/diag_level.h"   // G150
#include "win32/g150_ctl.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>   // G150

using namespace d2rt;

namespace ign {

namespace {

constexpr uint32_t kPageSize = 0x1000u;   // confirmed from cpu_box86.cpp's map(): va & ~0xFFFu

inline uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1); }
inline uint32_t align_up(uint32_t v, uint32_t a)   { return (v + a - 1) & ~(a - 1); }

// One entry in the layout table (for the dump + overlap matrix). `mapped_by`
// says who actually calls Cpu::map() for it, purely for the log -- it
// doesn't change any behavior here.
struct Entry {
    const char* name;
    uint32_t base, size;
    const char* mapped_by;
};

uint32_t region_end(const Entry& e) { return e.base + e.size; }

// The bounds Cpu::map() ACTUALLY uses: cpu_box86.cpp's map() rounds
// [va, va+size) out to page boundaries before touching anything
// (`a = va & ~0xFFFu; end = (va+size+0xFFFu) & ~0xFFFu`). Checking overlaps
// against the raw, un-rounded bounds could miss a collision that rounding
// introduces between two regions that only *look* separate on paper.
Entry page_rounded(const Entry& e) {
    uint32_t a = align_down(e.base, kPageSize);
    uint32_t end = align_up(e.base + e.size, kPageSize);
    return { e.name, a, end - a, e.mapped_by };
}

bool overlaps(const Entry& a, const Entry& b) {
    // Half-open ranges [base, base+size): touching endpoints do NOT overlap.
    return a.base < region_end(b) && b.base < region_end(a);
}

} // namespace

const MemLayout& ignition_mem_layout() {
    // Chosen entirely independently of D2Vita's own WX86_LAYOUT=compact/haut
    // presets (calibrated for Diablo II's own EXE/heap sizes, per bridge.cpp)
    // -- these are Ignition's own numbers, picked only to clear its real PE
    // image (0x00400000..0x0066E000, confirmed on hardware) and to fit
    // comfortably inside a single ~160 MiB arena.
    static const MemLayout L = {
        /* heap    */ 0x01000000u, 16u * 1024 * 1024,
        /* va      */ 0x02000000u, 64u * 1024 * 1024,
        /* scratch */ 0x06000000u, 4u * 1024 * 1024,
        /* mod     */ 0x06400000u, 128u * 1024,
        /* stack   */ 0x06420000u, 2u * 1024 * 1024,   // matches Bridge's own default stack_size_
        /* trap    */ 0x06620000u, 1u * 1024 * 1024,   // matches trap_hi_-trap_base_ once WX86_TRAPBASE is set
        /* arena   */ 0x0A000000u,                      // 160 MiB requested -> ~144 MiB effective span (see .h)
    };
    return L;
}

void apply_memory_layout_env() {
    const MemLayout& L = ignition_mem_layout();
    char buf[16];

    std::snprintf(buf, sizeof buf, "%x", L.arena_size);
    setenv("WX86_ARENA", buf, 1);
    IGN_LOG("[MEMENV]", "WX86_ARENA=%s (read once by CpuBox86's constructor)", buf);

    std::snprintf(buf, sizeof buf, "%x", L.mod_base);
    setenv("WX86_MODBASE", buf, 1);
    IGN_LOG("[MEMENV]", "WX86_MODBASE=%s", buf);

    std::snprintf(buf, sizeof buf, "%x", L.stack_base);
    setenv("WX86_STACKBASE", buf, 1);
    IGN_LOG("[MEMENV]", "WX86_STACKBASE=%s", buf);

    std::snprintf(buf, sizeof buf, "%x", L.trap_base);
    setenv("WX86_TRAPBASE", buf, 1);
    IGN_LOG("[MEMENV]", "WX86_TRAPBASE=%s (Bridge auto-sizes trap_hi_ = this + 0x100000)", buf);

    // iteration 14: hardens Cpu::read() (used by every shim's c.read()/
    // read_guest_cstr()) against a bad guest pointer -- without this, an
    // invalid guest address handed to a shim would fault deep inside the
    // host's own memcpy (uncatchable on Vita, see cpu_box86.cpp's own
    // comment on D2_MEMGUARD). Does NOT protect ordinary guest code's own
    // loads/stores (the dynarec's translated MOV/etc. never consult this) --
    // only calls that go through the C++ Cpu::read() path, i.e. our shims.
    // G150-BEGIN
    // iteration-G150: the G149 real run showed what this guard costs. With WX86_MEMGUARD set, EVERY shim-side guest read (Cpu::arg(), read_u32, the return address the bridge reads on each trap, the vertices of each
    // triangle ...) first calls getProtection(), i.e. a mutex lock + a red-black tree lookup + unlock (custommem.c): the triangle segments of the log (prologue 4.5 us for ~4 reads, level 0 9.4 us for ~9, decode 9.0 us for ~9,
    // validation / state / uv without reads ~1 us each) are all about 1 us PER READ. Cpu::read() keeps its own arena bounds check (arena_check) whatever this variable says, so a pointer outside the arena is still refused and
    // can never fault the host; what the guard added is zero-filling reads of pages that are inside the arena but not mapped by the guest, which no correct run does. The LIGHT build therefore does not arm the guard
    // (control file ux0:data/ignition/g146_geo.txt, token memguard=on, brings it back for an A/B run); the FULL diagnostic build keeps it armed exactly as before (token memguard=off removes it).
    // G150-END
    bool g150Guard = IGN_DIAG_FULL != 0;   // G150
    {   // G150
        char tb[400] = {0};   // G150
        if (FILE* f = fopen("ux0:data/ignition/g146_geo.txt", "r")) {   // G150
            const size_t n = fread(tb, 1, sizeof tb - 1, f); fclose(f);   // G150
            for (size_t i = 0; i < n; ++i) if (tb[i] == '\n' || tb[i] == '\r' || tb[i] == '\t') tb[i] = ' ';   // G150
            const ign::g150::Control g150Ctl = ign::g150::parse_control(tb);   // G150
            if (g150Ctl.memguard == 1) g150Guard = true;   // G150
            if (g150Ctl.memguard == 0) g150Guard = false;   // G150
        }   // G150
    }   // G150
    if (g150Guard) {   // G150
    setenv("WX86_MEMGUARD", "1", 1);
    IGN_LOG("[MEMENV]", "WX86_MEMGUARD=1 (guards shim-side Cpu::read() against unmapped guest pointers)");
    } else {   // G150
        IGN_LOG("[G150-MEMGUARD]", "WX86_MEMGUARD not armed (LIGHT default; token memguard=on in ux0:data/ignition/g146_geo.txt arms it): shim-side guest reads keep the arena bounds check and skip getProtection (a mutex + a tree lookup, ~1 us per read)");   // G150
    }   // G150
}

bool log_and_check_layout(uint32_t exeBase, uint32_t exeSize) {
    const MemLayout& L = ignition_mem_layout();

    const Entry regions[] = {
        { "PE",      exeBase,      exeSize,      "Bridge::add_module (preferred ImageBase, no relocation)" },
        { "HEAP",    L.heap_base,  L.heap_size,  "ignition_memlayout (Cpu::map)" },
        { "VA",      L.va_base,    L.va_size,    "ignition_memlayout (Cpu::map)" },
        { "SCRATCH", L.scratch_base, L.scratch_size, "ignition_memlayout (Cpu::map)" },
        { "MODULE",  L.mod_base,   L.mod_size,   "reserved for a future add_module(); unused this phase" },
        { "STACK",   L.stack_base, L.stack_size, "Bridge::commit() (WX86_STACKBASE)" },
        { "TRAP",    L.trap_base,  L.trap_size,  "no backing needed -- jump-target window, never dereferenced as data" },
    };
    constexpr int n = sizeof(regions) / sizeof(regions[0]);

    IGN_LOG("[LAYOUT]", "========== GUEST MEMORY LAYOUT (arena=0x%08x) ==========", L.arena_size);
    for (int i = 0; i < n; ++i) {
        const Entry& e = regions[i];
        IGN_LOG("[LAYOUT]", "%-8s 0x%08x - 0x%08x  size=0x%08x (%u KiB)  base%%page=%u size%%page=%u  [%s]",
                e.name, e.base, region_end(e) - 1, e.size, e.size / 1024,
                e.base % kPageSize, e.size % kPageSize, e.mapped_by);
    }
    IGN_LOG("[LAYOUT]", "highest address used = 0x%08x (arena covers up to ~0x%08x)",
            regions[n - 1].base + regions[n - 1].size - 1,
            L.arena_size > 0x1000000u ? L.arena_size - 0x1000000u : L.arena_size);
    IGN_LOG("[LAYOUT]", "=========================================================");

    Entry rounded[n];
    for (int i = 0; i < n; ++i) rounded[i] = page_rounded(regions[i]);

    bool ok = true;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            // Skip a pair where either side has size 0 (nothing to overlap).
            if (rounded[i].size == 0 || rounded[j].size == 0) continue;
            bool bad = overlaps(rounded[i], rounded[j]);
            IGN_LOG("[MAPCHECK]", "%s vs %s = %s", regions[i].name, regions[j].name,
                    bad ? "OVERLAP" : "no overlap");
            if (bad) ok = false;
        }
        // Overflow guard: base+size must not wrap past 0xFFFFFFFF.
        if (regions[i].size && (uint64_t)regions[i].base + regions[i].size > 0x100000000ull) {
            IGN_LOG("[MAPCHECK]", "%s OVERFLOW: base=0x%08x size=0x%08x wraps past 4 GiB",
                    regions[i].name, regions[i].base, regions[i].size);
            ok = false;
        }
    }
    if (!ok) IGN_LOG("[ERROR]", "Memory layout has overlapping or overflowing regions -- refusing to map anything");
    return ok;
}

bool map_layout_regions(Cpu& cpu, wx86::GuestRegion& heap, wx86::GuestRegion& va) {
    const MemLayout& L = ignition_mem_layout();

    auto map_one = [&](const char* name, uint32_t base, uint32_t size) -> bool {
        IGN_LOG("[MAP]", "%s begin: guest_start=0x%08x guest_end=0x%08x size=0x%08x perms=RW", name, base, base + size, size);
        bool result = cpu.map(base, size, nullptr, P_RW);
        if (result) IGN_LOG("[MAP]", "%s -> OK", name);
        else        IGN_LOG("[MAP]", "%s -> FAIL", name);
        return result;
    };

    if (!map_one("HEAP", L.heap_base, L.heap_size)) return false;
    if (!map_one("VA", L.va_base, L.va_size)) return false;
    if (!map_one("SCRATCH", L.scratch_base, L.scratch_size)) return false;

    heap.init(L.heap_base, L.heap_size, 16, "ign_heap");
    va.init(L.va_base, L.va_size, 0x1000, "ign_va");
    wx86_scratch_init(L.scratch_base, L.scratch_size);
    return true;
}

} // namespace ign
