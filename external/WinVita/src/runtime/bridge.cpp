// src/runtime/bridge.cpp — see bridge.h.
#include "runtime/bridge.h"
#include "runtime/layout.h"
#include "runtime/gil.h"   // observability: name the currently running shim (§19)
#include "runtime/prof.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- D2_NATPROF: per-slot time -------------------------------------------
extern "C" {
    int d2rt_natprof = 0;                       // read in commit()
    extern int d2rt_wakeprof;                   // D2_WAKEPROF (sched_native.cpp)
}
namespace {
    uint64_t g_natprof_us[d2rt::trapcnt::kMax];
    uint64_t g_natprof_n[d2rt::trapcnt::kMax];
    uint64_t g_natprof_prev_us[d2rt::trapcnt::kMax];
    d2rt::Bridge* g_natprof_bridge = nullptr;
}

// Engine log service (platform/vita_host.h): writes a durable line on
// console, no-ops elsewhere. Linked directly and strongly — never as a weak
// reference to a specific consumer's symbol: a generic engine has no
// business assuming its consumer's name, and a port whose symbols don't
// match one would otherwise get a silently muted log.
#include "platform/vita_host.h"
// Monotonic clock lives in the engine — used directly, no per-port indirection.
#include "runtime/host_clock.h"
extern "C" { extern uint32_t d2rt_timeprof_base; }   // guest base (cpu_box86.cpp)

namespace d2rt {

// Per-host-thread slots (see bridge.h). File-scope __thread: zero-initialized
// per thread, no ctor needed.
#ifdef D2_TLSCOUNT
extern "C" { unsigned long long d2_tls_traps = 0; }
// Calls to E() (cpu_box86.cpp). Counted as the DIFFERENCE across a
// crossing, so the figure is a genuine per-slot measurement, never a unit
// price borrowed from another operation.
extern "C" { extern unsigned long long d2_e_calls; }
#endif
// Environment-derived diagnostic flags: read ONCE in commit(), never in
// trap_handler. A function-static `static X v = getenv(...)` there would be
// a non-trivial-init function static, which GCC guards with an
// acquire-load check on every single trap — real cost for two flags that
// stay off in production. Not a file-scope static either: on console that
// would run before the environment file is read, so the flag would always
// read false. Setting them explicitly in commit(), after the environment is
// read and before the first trap can fire, avoids both problems.
static uint32_t g_watch_va = 0;      // D2_WATCH=<hex address>
static bool     g_traptag  = false;  // TRAPTAG=1

// A SINGLE __thread variable instead of three. Same semantics (per-thread
// scope, zero-initialized); what changes is the number of emutls objects.
// The VitaSDK's GCC is built --disable-tls: every distinct __thread
// variable touched in a function costs its own __emutls_get_address call,
// an INTER-MODULE call (pthread_getspecific -> pte_osTlsGetValue ->
// sceKernelGetTLSAddr). With a single object, GCC shares that resolution
// across every field touched in the same function — including
// t_b.yield_pending and t_b.redirect_eip, both on the path of a single trap
// (trap_handler reads the first, and take_redirect(), defined in this same
// unit and therefore inlinable, consumes the second).
struct BridgeTls { bool yield_pending; bool yielded; uint32_t redirect_eip; };
static __thread BridgeTls t_b = { false, false, 0 };

void Bridge::request_yield() { t_b.yield_pending = true; }
bool Bridge::take_yield()    { bool y = t_b.yielded; t_b.yielded = false; return y; }
void Bridge::redirect_next(uint32_t va) { t_b.redirect_eip = va; }
uint32_t Bridge::take_redirect() { uint32_t v = t_b.redirect_eip; t_b.redirect_eip = 0; return v; }

std::string Bridge::lc(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

Bridge::Bridge(Cpu* cpu) : cpu_(cpu) {
    // Headroom so alloc_trap growth is rare. Correctness does not depend on
    // it: growth AND indexing run under the trap-dispatch GIL in native mode,
    // and trap_handler re-derives its slot pointer after shim.fn (which may
    // grow slots_ via GetProcAddress / LoadLibrary DISK) — the reserve is
    // only headroom.
    slots_.reserve(2048);
    // Compressed-layout overrides (D2LAYOUT=compact): repack module/stack/trap
    // bases LOW so the whole guest span fits in device RAM for the Vita
    // single-block arena. Defaults keep the validated sparse layout untouched.
    // D2LAYOUT=haut: the SAME pack, translated via layout_hi() (see
    // src/runtime/layout.h). Offset 0 for "compact" -> byte-identical lines.
    if (d2rt::layout_packed()) {
        const uint32_t HI = d2rt::layout_hi();
        next_base_  = HI + 0x01900000;   // modules (relocatable) right after the 20 MiB heap
        stack_base_ = HI + 0x10000000;   // main guest stack, just past the 220 MiB VA arena
        trap_base_  = HI + 0x10E00000;   // native-thunk window, above the worker stacks/TIBs
        trap_next_  = trap_base_;
        trap_hi_    = HI + 0x10F00000;
        // The sentinel must stay inside the trap window/VA arena, consistent
        // with trap_base_/trap_hi_ above: H(va)=va+membase never faults
        // (single block, no guard), so a sentinel outside the arena would
        // silently corrupt host memory instead of crashing — the exact
        // failure mode an arena-span check exists to catch. This matters
        // doubly here because redirect_next() can place the EIP at this
        // value and the dynarec would then fetch code from it.
        sentinel_   = HI + 0x10EFFFF0;   // last page INSIDE the trap window
    }
    // Named overrides: a host that already has its OWN memory map (typically
    // a single-block arena sized and placed for its own game — the D2LAYOUT
    // presets above are calibrated for Diablo II and don't fit every image)
    // states it directly, field by field, instead of picking from a preset
    // that fits nothing on its side. Applied AFTER the presets, so a caller
    // can start from "compact"/"haut" and override just one field.
    //
    // Without this, a host that sets its own single-block D2ARENA (often a
    // few hundred MiB) but leaves next_base_/stack_base_/trap_base_ at their
    // scattered defaults (up to 0x7F100000, designed for a per-region host
    // mmap, not one block) overflows H(va)=va+membase in 32-bit arithmetic:
    // the write lands on a host address unrelated to the arena, which the
    // host refuses.
    auto hx = [](const char* wx86Name, const char* legacyName, uint32_t& v) {
        const char* e = std::getenv(wx86Name);
        if (!e) e = std::getenv(legacyName);
        if (e && *e) v = (uint32_t)std::strtoul(e, nullptr, 16);
    };
    hx("WX86_MODBASE",   "D2_MODBASE",   next_base_);
    hx("WX86_STACKBASE", "D2_STACKBASE", stack_base_);
    {
        const uint32_t oldTrapBase = trap_base_;
        hx("WX86_TRAPBASE", "D2_TRAPBASE", trap_base_);
        if (trap_base_ != oldTrapBase) {
            // Same extent as the D2LAYOUT presets above (1 MiB): enough for
            // the trap slots, never sized separately by any known caller.
            trap_hi_  = trap_base_ + 0x00100000u;
            sentinel_ = trap_hi_ - 0x10;
        }
    }
    trap_next_ = trap_base_;
}

Bridge::Mod* Bridge::find(const std::string& key) {
    for (auto& m : mods_) if (m.key == key) return &m;
    return nullptr;
}
PeImage* Bridge::module(const std::string& key) { auto* m = find(key); return m ? m->img.get() : nullptr; }
uint32_t Bridge::module_base(const std::string& key) { auto* m = find(key); return m ? m->img->load_base() : 0; }

std::vector<std::pair<uint32_t,uint32_t>> Bridge::loaded_modules() const {
    std::vector<std::pair<uint32_t,uint32_t>> out;
    out.reserve(mods_.size());
    for (auto& m : mods_) out.emplace_back(m.img->load_base(), m.img->image_size());
    return out;
}

void Bridge::set_version_resource_source(std::function<std::vector<uint8_t>(const std::string&)> fn) {
    version_src_ = std::move(fn);
}

std::vector<uint8_t> Bridge::version_resource_bytes(const std::string& guestFileName) const {
    return version_src_ ? version_src_(guestFileName) : std::vector<uint8_t>{};
}

PeImage* Bridge::add_module(const std::string& key, const std::vector<uint8_t>& bytes, std::string& err) {
    auto img = std::make_unique<PeImage>();
    // Reloc-stripped modules (typically the main EXE) have no base relocations
    // and MUST load at their preferred image_base; others get the next
    // auto-placed slot.
    uint32_t base = next_base_;
    bool keepBase = false;   // load at preferred ImageBase (no auto-placement)?
    if (bytes.size() > 0x40) {
        uint32_t pe = 0; std::memcpy(&pe, bytes.data() + 0x3c, 4);
        uint32_t sig = 0;
        if (pe + 0x40 < bytes.size()) std::memcpy(&sig, bytes.data() + pe, 4);
        if (sig == 0x00004550) {   // "PE\0\0"
            uint16_t chars = 0; std::memcpy(&chars, bytes.data() + pe + 22, 2);
            bool stripped = (chars & 0x0001) != 0;   // IMAGE_FILE_RELOCS_STRIPPED
            bool isDll    = (chars & 0x2000) != 0;    // IMAGE_FILE_DLL
            // A real Windows loader maps the main .exe at its preferred ImageBase
            // (relocating only on a collision) and auto-places DLLs. Mirror that:
            // an EXE (or any reloc-stripped image) keeps its preferred base, so the
            // guest sees Game.exe at 0x00400000 with ZERO relocations — byte-identical
            // to disk, and absolute guest reads (e.g. an anti-cheat MEM_CHECK) land on
            // the real bytes. D2_RELOC_EXE=1 forces the old auto-placed (0x30000000,
            // relocated) behavior for an A/B comparison.
            bool forceReloc = std::getenv("WX86_RELOC_EXE") || std::getenv("D2_RELOC_EXE");
            if (stripped || (!isDll && !forceReloc)) {
                uint32_t pref = 0; std::memcpy(&pref, bytes.data() + pe + 24 + 28, 4);
                base = pref; keepBase = true;
            }
        }
    }
    if (!img->load(bytes, base, err)) return nullptr;
    if (!keepBase && module_limit_ && (uint64_t)base + img->image_size() > module_limit_) {
        char m[160];
        std::snprintf(m, sizeof m, "module window full: %s needs 0x%08x..0x%08x, limit 0x%08x",
                      key.c_str(), base, (unsigned)(base + img->image_size()), module_limit_);
        err = m;
        return nullptr;
    }
    if (!keepBase) {
        // Advance next_base_ past this image (page aligned, 64 KiB gap).
        next_base_ = (base + img->image_size() + 0xFFFFu) & ~0xFFFFu;
        next_base_ += 0x10000;
    }
    mods_.push_back({key, std::move(img)});
    return mods_.back().img.get();
}

PeImage* Bridge::load_library_runtime(const std::string& key,
                                      const std::vector<uint8_t>& bytes, std::string& err) {
    if (!committed_) { err = "load_library_runtime before commit"; return nullptr; }
    // add_module() places the image (relocated) at the next module base and
    // parses its imports/exports. It does NOT map it into the CPU yet.
    PeImage* pi = add_module(key, bytes, err);
    if (!pi) return nullptr;
    if (!cpu_->map(pi->load_base(), pi->image_size(), pi->image().data(), P_RWX)) {
        err = "cpu.map failed for " + key;
        return nullptr;
    }
    // Resolve THIS module's imports — the per-module half of link().
    for (const auto& imp : pi->imports()) {
        uint32_t target = 0;
        std::string want = lc(imp.dll);
        for (auto& other : mods_) {                       // another loaded module's export
            if (lc(other.key) != want) continue;
            target = imp.name.empty() ? other.img->export_va_ordinal(imp.ordinal)
                                      : other.img->export_va(imp.name);
            break;
        }
        if (!target) {                                    // a registered native shim
            std::string k = imp.name.empty()
                ? (want + "!#" + std::to_string(imp.ordinal))
                : (want + "!" + imp.name);
            auto it = shims_.find(k);
            if (it != shims_.end()) target = alloc_trap(it->second);
        }
        if (!target) {                                    // unresolved → default shim trap
            unresolved_++;
            Shim s;
            std::string sym = imp.name.empty()
                ? (imp.dll + "!#" + std::to_string(imp.ordinal))
                : (imp.dll + "!" + imp.name);
            s.tag = sym; s.argc = 0; s.stdcall_cleanup = false;
            std::string tag = sym;
            s.fn = [this, tag](Cpu& c) -> uint32_t {
                return default_shim_ ? default_shim_(c, tag) : 0;
            };
            target = alloc_trap(s);
        }
        pi->patch_iat(imp.iat_va, target);
        cpu_->write_u32(imp.iat_va, target);
    }
    return pi;
}

void Bridge::register_shim(const std::string& dll, const std::string& name, Shim s) {
    if (s.tag.empty()) s.tag = dll + "!" + name;
    shims_[lc(dll) + "!" + name] = std::move(s);
}
void Bridge::register_shim_ordinal(const std::string& dll, uint32_t ordinal, Shim s) {
    char buf[32]; std::snprintf(buf, sizeof buf, "!#%u", ordinal);
    if (s.tag.empty()) s.tag = dll + buf;
    shims_[lc(dll) + buf] = std::move(s);
}
void Bridge::set_default_shim(std::function<uint32_t(Cpu&, const std::string&)> fn) {
    default_shim_ = std::move(fn);
}

uint32_t Bridge::shim_trap(const std::string& dll, const std::string& name) {
    auto it = shims_.find(lc(dll) + "!" + name);
    if (it == shims_.end()) return 0;
    return alloc_trap(it->second);
}

uint32_t Bridge::shim_trap_existing(const std::string& dll, const std::string& name) const {
    auto it = shims_.find(lc(dll) + "!" + name);
    if (it == shims_.end()) return 0;
    auto st = slot_by_tag_.find(it->second.tag);
    return st == slot_by_tag_.end() ? 0u : st->second;
}

uint32_t Bridge::alloc_trap(const Shim& s) {
    auto it = slot_by_tag_.find(s.tag);
    if (it != slot_by_tag_.end()) return it->second;
    uint32_t va = trap_next_;
    trap_next_ += 16;
    slots_.push_back({s, va});
    // The stable name comes from the map's KEY (see TrapSlot::tagc): once
    // inserted here, it is never relocated or erased for the rest of the run.
    auto ins = slot_by_tag_.emplace(s.tag, va);
    slots_.back().tagc = ins.first->first.c_str();
    return va;
}

bool Bridge::commit(std::string& err) {
    if (committed_) return true;
    // Diagnostic flags: read HERE — once, after env.txt, before the first
    // trap (no trap can fire before set_trap opens the window, further below
    // in this same function).
    { const char* w = getenv("WX86_WATCH"); if (!w) w = getenv("D2_WATCH");
      g_watch_va = w ? (uint32_t)strtoul(w, nullptr, 16) : 0u;
      g_traptag  = getenv("TRAPTAG") != nullptr; }
    // Map every module image.
    for (auto& m : mods_) {
        auto& im = *m.img;
        if (!cpu_->map(im.load_base(), im.image_size(), im.image().data(), P_RWX)) {
            err = "cpu.map failed for " + m.key; return false;
        }
    }
    // Guest stack.
    if (!cpu_->map(stack_base_, stack_size_, nullptr, P_RW)) { err = "stack map failed"; return false; }
    // Trap window stays UNMAPPED: any fetch into it vectors to the handler below.
    cpu_->set_trap(trap_base_, trap_hi_,
                   [this](Cpu& c, uint32_t va) { return trap_handler(c, va); });
    // Per-slot trap counter (runtime/trapcnt.h): the base is set HERE, at
    // the same place and moment as the trap window, so the intrinsic path
    // (cpu_box86.cpp) and trap_handler necessarily index from the same
    // origin. Before this point, bump() is inert (base == 0) — and no trap
    // can fire before set_trap opens the window.
    trapcnt::base = trap_base_;
    { const char* w = std::getenv("WX86_WAKEPROF"); if (!w) w = std::getenv("D2_WAKEPROF"); d2rt_wakeprof = (w && *w && *w != '0') ? 1 : 0;
      if (d2rt_wakeprof)
          wx86_vita_progress_c("wakeprof: ARME — latence pthread_cond_signal -> reprise du fil"); }
    { const char* e = std::getenv("WX86_NATPROF"); if (!e) e = std::getenv("D2_NATPROF"); d2rt_natprof = (e && *e && *e != '0') ? 1 : 0;
      g_natprof_bridge = this;
      if (d2rt_natprof) wx86_vita_progress_c("natprof: ARME — temps par creneau publie par fenetre (cout ~5 %)"); }
    committed_ = true;
    return true;
}

bool Bridge::link(std::string& err) {
    if (!committed_) { err = "link before commit"; return false; }
    unresolved_ = 0;
    for (auto& m : mods_) {
        for (const auto& imp : m.img->imports()) {
            uint32_t target = 0;

            // 1) Another loaded module's export (real x86 → real x86).
            std::string want = lc(imp.dll);
            for (auto& other : mods_) {
                if (lc(other.key) != want) continue;
                target = imp.name.empty() ? other.img->export_va_ordinal(imp.ordinal)
                                          : other.img->export_va(imp.name);
                break;
            }

            // 2) A registered native shim.
            if (!target) {
                std::string k;
                if (imp.name.empty()) {
                    char buf[32]; std::snprintf(buf, sizeof buf, "!#%u", imp.ordinal);
                    k = want + buf;
                } else {
                    k = want + "!" + imp.name;
                }
                auto it = shims_.find(k);
                if (it != shims_.end()) target = alloc_trap(it->second);
            }

            // 3) Unresolved → default shim trap (logs, returns 0).
            if (!target) {
                unresolved_++;
                Shim s;
                std::string sym = imp.name.empty()
                    ? (imp.dll + "!#" + std::to_string(imp.ordinal))
                    : (imp.dll + "!" + imp.name);
                s.tag = sym;
                s.argc = 0; s.stdcall_cleanup = false;   // unknown: don't corrupt stack
                std::string tag = sym;
                s.fn = [this, tag](Cpu& c) -> uint32_t {
                    return default_shim_ ? default_shim_(c, tag) : 0;
                };
                target = alloc_trap(s);
            }

            // Patch the IAT slot in the guest image (already mapped) AND the
            // in-memory PeImage buffer (keep them consistent).
            m.img->patch_iat(imp.iat_va, target);
            cpu_->write_u32(imp.iat_va, target);
        }
    }
    // Published to the LOG, not stdout — on console stdout reaches nowhere.
    // Uses the same offset convention as the external time sampler (VA -
    // guest base) so the two can be read against each other without
    // conversion.
    if (std::getenv("WX86_DUMPTRAPS") || std::getenv("D2_DUMPTRAPS")) {
        for (auto& sl : slots_) {
            char m[160];
            std::snprintf(m, sizeof m, "[trap] %x  va=0x%08x  %s",
                          (unsigned)(sl.va - d2rt_timeprof_base), sl.va, sl.shim.tag.c_str());
            std::printf("%s\n", m);
            wx86_vita_progress_c(m);
        }
    }
    (void)err;
    return true;
}

// Window: per-slot time delta since the last call, top-k by us. Writes
// '\n'-separated lines into `out`. Returns the number of lines.
int d2rt::Bridge::natprof_window(char* out, unsigned n, int topk) {
    if (!out || !n) return 0;
    const size_t cnt = slots_.size() < trapcnt::kMax ? slots_.size() : trapcnt::kMax;
    uint64_t tot = 0; std::vector<std::pair<uint64_t,size_t>> v; v.reserve(64);
    for (size_t i = 0; i < cnt; ++i) {
        const uint64_t d = g_natprof_us[i] - g_natprof_prev_us[i];
        g_natprof_prev_us[i] = g_natprof_us[i];
        if (d) { tot += d; v.push_back({d, i}); }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.first > b.first; });
    unsigned o = 0; int lines = 0;
    o += (unsigned)std::snprintf(out + o, n - o, "natif: total=%lluus creneaux=%lu\n",
                                 (unsigned long long)tot, (unsigned long)v.size());   // no %zu: the Vita's printf prints it literally as "zu"
    ++lines;
    for (int k = 0; k < topk && k < (int)v.size() && o + 8 < n; ++k) {
        const size_t i = v[k].second;
        const char* t = slots_[i].tagc ? slots_[i].tagc : "?";
        o += (unsigned)std::snprintf(out + o, n - o, "natif %d: %lluus %llu.%01llu%% %s\n",
                 k + 1, (unsigned long long)v[k].first,
                 tot ? (unsigned long long)(v[k].first * 1000 / tot / 10) : 0ull,
                 tot ? (unsigned long long)(v[k].first * 1000 / tot % 10) : 0ull, t);
        ++lines;
    }
    return lines;
}
extern "C" int d2rt_natprof_lines(char* out, unsigned n, int topk) {
    return g_natprof_bridge ? g_natprof_bridge->natprof_window(out, n, topk) : 0;
}

bool Bridge::trap_handler(Cpu& cpu, uint32_t trap_va) {
#ifdef D2_TLSCOUNT
    ++d2_tls_traps;   // denominator for the emutls count (measurement build)
#endif
    // The final-return sentinel: the invoked guest function has returned.
    if (trap_va == sentinel_) return false;   // stop run

    // Find the slot. Trap VAs are allocated 16 bytes apart from trap_base_
    // (alloc_trap), so the slot index is a direct division — O(1), no scan.
    // The `.va != trap_va` guard keeps it robust to any future non-slot VA in
    // the window (the sentinel is already handled above).
    if (trap_va < trap_base_) return false;
    uint32_t idx = (trap_va - trap_base_) / 16;
    if (idx >= slots_.size() || slots_[idx].va != trap_va) return false;   // unknown trap → stop
    TrapSlot* slot = &slots_[idx];
    // Trap counter (runtime/trapcnt.h): the index was just computed, under
    // the trap-dispatch GIL. Just an increment. Slots served as an
    // INTRINSIC never reach here — they're counted in the same array by
    // CpuBox86::try_intrinsic, so there's no double count (when
    // try_intrinsic returns true, trap_fn_ is never called).
    if (idx < trapcnt::kMax) ++trapcnt::hits[idx];

#ifdef D2_TLSCOUNT
    // Crossing boundary: opened BEFORE the first resolution and closed
    // AFTER the last.
    const unsigned long long e0 = d2_e_calls;
#endif
    // At trap entry, ESP points at the return address (call just executed).
    uint32_t retaddr = cpu.trap_retaddr();
    // D2_WATCH=<hexaddr>: integrity-check 24 bytes at addr on every trap, to
    // bracket a guest-memory trample between two traps (diagnostic only).
    { const uint32_t wva = g_watch_va;   // plain load: no guard, no dmb
      if (wva) { static uint8_t ref[24]; static bool winit = false; uint8_t cur[24];
          cpu.read(wva, cur, 24);
          if (!winit) { std::memcpy(ref, cur, 24); winit = true; }
          else if (std::memcmp(ref, cur, 24)) {
              std::printf("[watch] 24B @%08x CHANGED (trap %s, ret=%08x):",
                          wva, slot->shim.tag.c_str(), retaddr);
              for (int i = 0; i < 24; i++) std::printf(" %02x", cur[i]);
              std::printf("\n"); std::fflush(stdout);
              std::memcpy(ref, cur, 24);
          } } }
#ifdef PROF_COUNTERS
    uint64_t t0 = prof::now_ns();
#endif
    // GIL observability (§19): name the shim body currently running. This
    // runs under the GIL (trap-dispatch gil::Guard), so the write is the
    // holder's own. Inert under coop (a single bool check). The name is the
    // slot's STABLE pointer, never shim.tag.c_str().
    gil::note_shim_enter(trap_va, slot->tagc);
    uint32_t eax;
    if (d2rt_natprof) {
        const uint64_t t0 = wx86_now_us();
        eax = slot->shim.fn(cpu);
        const uint64_t dt = wx86_now_us() - t0;
        if (idx < trapcnt::kMax) { g_natprof_us[idx] += dt; ++g_natprof_n[idx]; }
    } else
    eax = slot->shim.fn(cpu);
    gil::note_shim_exit();
    slot = &slots_[idx];   // re-derive: shim.fn may have alloc_trap'd (GetProcAddress /
                           // LoadLibrary DISK) and grown slots_ — the old pointer would dangle
    // TRAPTAG=1: log shim tags (debug; env checked once — hot path stays clean)
    { static uint64_t tn = 0;   // CONSTANT initialization: zero guard, zero dmb
      if (g_traptag && ++tn <= 4000)
        std::fprintf(stderr,"[tag %llu] %s -> eax=0x%08x\n",
            (unsigned long long)tn, slot->shim.tag.c_str(), eax); }
#ifdef PROF_COUNTERS
    uint64_t dt = prof::now_ns() - t0;
    slot->prof_calls++; slot->prof_ns += dt;
    prof::trap_calls++;  prof::trap_ns += dt;
#endif
    // Simulate `ret [imm]`: pop retaddr; for stdcall also pop argc dwords.
    uint32_t esp_add = 4;                              // pop return address
    if (slot->shim.stdcall_cleanup) esp_add += 4 * slot->shim.argc;
    // Guest-callback redirect: run a stub instead of returning to the call site.
    // take_redirect() has a SIDE EFFECT (it consumes the redirect) and was
    // already called unconditionally: hoisting it here doesn't change the
    // call count, only where the final EIP gets decided.
    const uint32_t rd = take_redirect();
    cpu.trap_epilogue(eax, esp_add, rd ? rd : retaddr);
#ifdef D2_TLSCOUNT
    // Close the boundary. RE-DERIVE the slot: shim.fn may have grown slots_
    // (same reason as the re-derivation above).
    { TrapSlot* sc = &slots_[idx];
      sc->tls_calls += (unsigned long long)(d2_e_calls - e0); ++sc->tls_traps; }
#endif
    // Cooperative yield: a shim asked to suspend the current guest thread. Leave
    // it fully resumable (EIP=retaddr) and stop the run; the scheduler will
    // save its context and switch. t_b.yielded tells this apart from a thread
    // return (which hits the sentinel above).
    if (t_b.yield_pending) { t_b.yield_pending = false; t_b.yielded = true; return false; }
    return true;   // resume at retaddr
}

int Bridge::repatch_import(const std::string& dll, const std::string& name, uint32_t va) {
    int n = 0;
    std::string want = lc(dll);
    for (auto& m : mods_) {
        for (const auto& imp : m.img->imports()) {
            if (imp.name != name || lc(imp.dll) != want) continue;
            m.img->patch_iat(imp.iat_va, va);
            cpu_->write_u32(imp.iat_va, va);
            ++n;
        }
    }
    return n;
}

int Bridge::prof_window(char* out, unsigned n, int topk) {
#ifdef PROF_COUNTERS
    if (!out || !n) return 0;
    out[0] = 0;
    if (prof_prev_.size() < slots_.size()) prof_prev_.resize(slots_.size(), 0);
    std::vector<std::pair<uint64_t, size_t>> v;
    for (size_t i = 0; i < slots_.size(); ++i) {
        const uint64_t d = slots_[i].prof_ns - prof_prev_[i];
        prof_prev_[i] = slots_[i].prof_ns;
        if (d) v.emplace_back(d, i);
    }
    std::sort(v.begin(), v.end(),
              [](const std::pair<uint64_t,size_t>& a, const std::pair<uint64_t,size_t>& b){
                  return a.first > b.first; });
    unsigned off = 0; int k = 0;
    for (const auto& p : v) {
        if (k++ >= topk) break;
        const char* t = slots_[p.second].tagc ? slots_[p.second].tagc : "?";
        const int w = std::snprintf(out + off, n - off, " %s=%llums",
                                    t, (unsigned long long)(p.first / 1000000ull));
        if (w <= 0 || (unsigned)w >= n - off) break;
        off += (unsigned)w;
    }
    return (int)off;
#else
    if (out && n) out[0] = 0;
    (void)topk;
    return 0;
#endif
}

void Bridge::dump_trap_counts(int topn) const {
    const size_t n = slots_.size() < trapcnt::kMax ? slots_.size() : trapcnt::kMax;
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) total += trapcnt::hits[i];
    // Sorted plainly rather than optimized for it: n is only a few hundred
    // at most, and this only runs at shutdown.
    std::vector<size_t> ord;
    ord.reserve(n);
    for (size_t i = 0; i < n; ++i) if (trapcnt::hits[i]) ord.push_back(i);
    std::sort(ord.begin(), ord.end(),
              [](size_t a, size_t b) { return trapcnt::hits[a] > trapcnt::hits[b]; });
    char m[160];
    // This embedded snprintf does NOT substitute %zu: it passes the literal
    // characters "zu" through without consuming an argument, silently
    // shifting every subsequent format argument by one — a %s downstream
    // would then read an unrelated integer as a string pointer. Use %u plus
    // a cast instead, as everywhere else in this file.
    std::snprintf(m, sizeof m, "trapcnt: %llu prises sur %u creneaux actifs%s",
                  (unsigned long long)total, (unsigned)ord.size(),
                  slots_.size() > trapcnt::kMax ? " (TRONQUE: slots_ > trapcnt::kMax)" : "");
    std::printf("  %s\n", m);
    wx86_vita_progress_c(m);
    for (int k = 0; k < topn && k < (int)ord.size(); ++k) {
        const size_t i = ord[k];
        const char* t = slots_[i].tagc ? slots_[i].tagc : "?";
        std::snprintf(m, sizeof m, "trapcnt %2d: %12llu  %5.1f%%  %s",
                      k + 1, (unsigned long long)trapcnt::hits[i],
                      total ? 100.0 * (double)trapcnt::hits[i] / (double)total : 0.0, t);
        std::printf("    %s\n", m);
        wx86_vita_progress_c(m);
    }
    std::fflush(stdout);
}

void Bridge::dump_tls_counts(int topn) const {
    uint64_t total = 0, traps = 0;
    std::vector<size_t> ord;
    for (size_t i = 0; i < slots_.size(); ++i) {
        total += slots_[i].tls_calls; traps += slots_[i].tls_traps;
        if (slots_[i].tls_traps) ord.push_back(i);
    }
    char m[192];
    if (!traps) {
        // Active silence: without -DD2_TLSCOUNT the fields stay zero, and a
        // list of zeros would misleadingly read as "no resolutions".
        std::snprintf(m, sizeof m,
            "tlscnt: RIEN A DIRE (build sans -DD2_TLSCOUNT, ou aucune traversee)");
        std::printf("  %s\n", m);
        wx86_vita_progress_c(m);
        std::fflush(stdout);
        return;
    }
    std::sort(ord.begin(), ord.end(),
              [this](size_t a, size_t b) { return slots_[a].tls_calls > slots_[b].tls_calls; });
    // Average in hundredths, as an INTEGER: %f in a console log is an
    // unnecessary risk.
    const unsigned long long avg = (unsigned long long)((total * 100ull) / traps);
    std::snprintf(m, sizeof m,
                  "tlscnt: %llu appels E() sur %llu traversees => %llu.%02llu par traversee",
                  (unsigned long long)total, (unsigned long long)traps, avg / 100ull, avg % 100ull);
    std::printf("  %s\n", m);
    wx86_vita_progress_c(m);
    for (int k = 0; k < topn && k < (int)ord.size(); ++k) {
        const size_t i = ord[k];
        const char* t = slots_[i].tagc ? slots_[i].tagc : "?";
        const unsigned long long per =
            (unsigned long long)((slots_[i].tls_calls * 100ull) / slots_[i].tls_traps);
        std::snprintf(m, sizeof m, "tlscnt %2d: E=%llu trav=%llu => %llu.%02llu/trav  %s",
                      k + 1, (unsigned long long)slots_[i].tls_calls,
                      (unsigned long long)slots_[i].tls_traps,
                      per / 100ull, per % 100ull, t);
        std::printf("    %s\n", m);
        wx86_vita_progress_c(m);
    }
    std::fflush(stdout);
}

void Bridge::dump_prof(int topn) const {
#ifdef PROF_COUNTERS
    std::vector<const TrapSlot*> v;
    for (const auto& s : slots_) if (s.prof_calls) v.push_back(&s);
    std::sort(v.begin(), v.end(),
              [](const TrapSlot* a, const TrapSlot* b){ return a->prof_ns > b->prof_ns; });
    std::printf("  [prof shims] top %d of %zu by cumulative time (calls, total ms, us/call):\n",
                topn, v.size());
    for (int i = 0; i < topn && i < (int)v.size(); ++i)
        std::printf("    %10llu  %9.2f ms  %7.2f us  %s\n",
                    (unsigned long long)v[i]->prof_calls,
                    v[i]->prof_ns / 1e6,
                    v[i]->prof_calls ? v[i]->prof_ns / 1e3 / v[i]->prof_calls : 0.0,
                    v[i]->shim.tag.c_str());
#else
    (void)topn;
#endif
}

bool Bridge::call_va(uint32_t va, const std::vector<uint32_t>& args,
                     uint32_t& ret, const char** fault) {
    // Fresh frame at top of stack.
    uint32_t esp = stack_base_ + stack_size_ - 0x1000;
    // Push args right-to-left (cdecl).
    for (auto it = args.rbegin(); it != args.rend(); ++it) {
        esp -= 4; cpu_->write_u32(esp, *it);
    }
    // Return address = sentinel (a trap-window address → "finished").
    esp -= 4; cpu_->write_u32(esp, sentinel_);
    cpu_->set_reg(R_ESP, esp);
    cpu_->set_reg(R_EBP, 0);
    const char* f = nullptr;
    bool ok = cpu_->run(va, &f);
    if (!ok) { if (fault) *fault = f ? f : "fault"; return false; }
    ret = cpu_->reg(R_EAX);
    return true;
}

bool Bridge::call_export(const std::string& key, const std::string& name,
                         const std::vector<uint32_t>& args, uint32_t& ret,
                         const char** fault) {
    auto* m = find(key);
    if (!m) { if (fault) *fault = "no such module"; return false; }
    uint32_t va = m->img->export_va(name);
    if (!va) { if (fault) *fault = "no such export"; return false; }
    return call_va(va, args, ret, fault);
}
bool Bridge::call_export_ordinal(const std::string& key, uint32_t ordinal,
                                 const std::vector<uint32_t>& args, uint32_t& ret,
                                 const char** fault) {
    auto* m = find(key);
    if (!m) { if (fault) *fault = "no such module"; return false; }
    uint32_t va = m->img->export_va_ordinal(ordinal);
    if (!va) { if (fault) *fault = "no such ordinal"; return false; }
    return call_va(va, args, ret, fault);
}

} // namespace d2rt
