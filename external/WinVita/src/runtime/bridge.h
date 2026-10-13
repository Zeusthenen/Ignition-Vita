// src/runtime/bridge.h — x86↔native import bridge + module manager.
//
// Responsibilities:
//   * Own the loaded PE images and place them in one guest address space.
//   * Resolve every import: either to another loaded module's export
//     (real x86 → real x86 direct call, no bridge cost) or to a native
//     shim registered by name/ordinal (x86 → ARM host call via a trap).
//   * Provide the trap handler that marshals cdecl/stdcall args, dispatches
//     to the shim, and returns to the guest.
//   * Expose call_export() to invoke a guest function from host code.
//
// Calling-convention note: the shim declares how many dword args it consumes
// and whether the callee cleans the stack (stdcall) or the caller (cdecl).
// Most D2/Win32 exports are stdcall; C-runtime-ish ones are cdecl. Win32 DLLs
// default to stdcall, and each shim can override.

#pragma once
#include "runtime/cpu.h"
#include "runtime/pe_image.h"
#include "runtime/trapcnt.h"   // per-slot trap counter (including intrinsics)

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace d2rt {

// A native shim: reads args via cpu.arg(k), returns the EAX value.
// `argc` dwords are cleaned off the stack for stdcall (cleanup==true).
struct Shim {
    std::function<uint32_t(Cpu& cpu)> fn;
    uint32_t argc = 0;
    bool     stdcall_cleanup = true;   // Win32 default
    std::string tag;                   // "KERNEL32.dll!HeapAlloc" for logging
};

class Bridge {
public:
    explicit Bridge(Cpu* cpu);

    // Load a DLL file (already-read bytes) under `key` (e.g. "Fog.dll").
    // Picks a non-overlapping base automatically. Returns the image, or null.
    PeImage* add_module(const std::string& key, const std::vector<uint8_t>& bytes,
                        std::string& err);

    PeImage* module(const std::string& key);
    uint32_t module_base(const std::string& key);

    // Snapshot of every currently loaded module's (load_base, image_size),
    // in load order. Generic module-enumeration primitive for consumers that
    // need to reflect their own process's module list (EnumProcessModules
    // and friends — see win32_shims_psapi.cpp).
    std::vector<std::pair<uint32_t,uint32_t>> loaded_modules() const;

    // VERSION.dll support: register the callback that turns a guest-supplied
    // file name (as passed to GetFileVersionInfo*/VerQueryValue*) into that
    // file's raw bytes, however the consumer resolves guest paths to real
    // ones. win32_shims_version.cpp parses the real VS_FIXEDFILEINFO out of
    // whatever bytes come back — it never fabricates version data itself.
    void set_version_resource_source(std::function<std::vector<uint8_t>(const std::string&)> fn);
    std::vector<uint8_t> version_resource_bytes(const std::string& guestFileName) const;

    // Register a native shim for dll!name or dll!#ordinal. Keys are matched
    // case-insensitively on the dll and exactly on symbol/ordinal.
    void register_shim(const std::string& dll, const std::string& name, Shim s);
    void register_shim_ordinal(const std::string& dll, uint32_t ordinal, Shim s);

    // Trap VA for a registered shim (created on demand, cached by tag) — lets
    // GetProcAddress resolve dynamically-loaded SYSTEM dlls to their shims.
    // Returns 0 if no shim is registered under dll!name.
    uint32_t shim_trap(const std::string& dll, const std::string& name);
    // Like shim_trap, but allocates nothing: returns the slot only if it
    // already exists (import actually bound), 0 otherwise. Needed to probe
    // or arm a single intrinsic without shifting the numbering of
    // subsequent slots.
    uint32_t shim_trap_existing(const std::string& dll, const std::string& name) const;

    // A single catch-all shim used for any unresolved import (logs + returns 0).
    void set_default_shim(std::function<uint32_t(Cpu&, const std::string& tag)> fn);

    // After all modules are added + shims registered, wire every IAT slot.
    // Unresolved imports get a trap that routes to the default shim.
    bool link(std::string& err);

    // Re-point every module's IAT slot for dll!name at `va` (a guest-code
    // stub). Call after link(). This is how ultra-hot trivial imports are
    // inlined as guest x86 (real-Windows style: GetLastError IS a 2-insn
    // fs:[0x34] read, never a syscall) — eliminating the dynarec-exit trap
    // round-trip entirely. Returns the number of slots repatched.
    int repatch_import(const std::string& dll, const std::string& name, uint32_t va);

    // Map all module images into the CPU, set up the trap window + stack.
    // Call once after add_module()s, before link()/call_export().
    bool commit(std::string& err);

    // Load an extra DLL at RUNTIME (after commit()/link()) — for a guest
    // LoadLibraryA of a real file readable from disk (the lockdown
    // CheckRevision.dll that D2 extracts from CheckRevision.mpq during the
    // Battle.net login). Places it in the module region (relocated), maps it
    // into the CPU, and resolves its imports against the already-loaded modules
    // and registered shims (unshimmed imports route to the default shim, like
    // link()). Does NOT run DllMain. Returns the image (the caller registers it
    // in its own name/base maps) or null on error.
    PeImage* load_library_runtime(const std::string& key,
                                  const std::vector<uint8_t>& bytes, std::string& err);

    // Invoke guest export `name`/`ordinal` of `key` with cdecl-style args.
    // Sets up a fresh frame: pushes args, sets return addr to the sentinel,
    // runs until the function returns. Returns EAX in `ret`.
    bool call_export(const std::string& key, const std::string& name,
                     const std::vector<uint32_t>& args, uint32_t& ret,
                     const char** fault);
    bool call_export_ordinal(const std::string& key, uint32_t ordinal,
                             const std::vector<uint32_t>& args, uint32_t& ret,
                             const char** fault);
    bool call_va(uint32_t va, const std::vector<uint32_t>& args,
                 uint32_t& ret, const char** fault);

    Cpu* cpu() { return cpu_; }

    // Stats.
    uint32_t unresolved_count() const { return unresolved_; }
    // PROF_COUNTERS: print the top-N shims by cumulative shim-body time
    // (tag, calls, total ms, µs/call). No-op in default builds.
    void dump_prof(int topn) const;
    // PROF_COUNTERS: top-K shims by time elapsed since the last call,
    // formatted into `out` (" tag=Nms tag=Nms ..."). Windowed variant of
    // dump_prof: a since-startup cumulative total says nothing about what a
    // specific stretch of play costs. Returns the number of bytes written;
    // writes "" and returns 0 without PROF_COUNTERS.
    int prof_window(char* out, unsigned n, int topk);

    // Top-N slots by NUMBER OF TRAPS (see runtime/trapcnt.h). Unrelated to
    // dump_prof: this counter is present in shipping builds and also counts
    // slots served as intrinsics, which the Bridge never sees go by.
    // Published via wx86_vita_progress_c (console, where printf reaches no
    // log) and via printf (qemu/host).
    void dump_trap_counts(int topn) const;
    // D2_NATPROF=1: per-slot TIME (µs accumulated in the shim body),
    // published in 10s windows. Native ports and shims are host code,
    // invisible to the dynarec's own profiler by construction. Known
    // overhead: two clock reads per crossing (~1.3 µs each on Vita, ~900
    // crossings/frame => ~2.3 ms, ~5%) — a measurement instrument, never
    // enabled during normal play.
    int natprof_window(char* out, unsigned n, int topk);

    // Top-N slots by calls to E() (per-thread state resolution, see
    // cpu_box86.cpp d2_e_calls) and, more usefully, by calls PER CROSSING:
    // this ratio is what a native hook costs in resolutions, and the only
    // number a batching decision can be based on. Reports nothing outside a
    // -DD2_TLSCOUNT build (where the counters are always zero) rather than
    // printing all-zero data.
    void dump_tls_counts(int topn) const;

    // Cooperative-threading hooks. A shim calls request_yield() to make the
    // current cpu->run() stop *resumably* after the shim returns; the scheduler
    // polls take_yield() to tell a yield apart from a thread return (sentinel).
    //
    // Guest-callback support: a shim may redirect execution (redirect_next) to
    // a guest stub instead of returning straight to the call site (e.g.
    // CreateWindowExA dispatching WM_CREATE to the game's wndproc
    // synchronously, as Windows does). The stub is responsible for finally
    // jumping to the original return address (the shim reads it from [esp]
    // before cleanup).
    //
    // These are per-HOST-thread slots. __thread deliberately: under the
    // cooperative backend there is one host thread (behaviour unchanged);
    // under the native backend each guest thread runs on its own host thread,
    // so an ExitThread redirect or a yield can never be consumed by another
    // thread's trap return (spec D7: the Bridge mono-slot trap). A redirect is
    // posted by shim.fn and consumed at the tail of the SAME trap_handler
    // invocation — no scheduler point exists between the two, so coop's
    // N-guest-on-1-host multiplexing can never leak it across guest threads
    // (do not ever insert a scheduling point in the trap tail).
    void request_yield();
    bool take_yield();
    void redirect_next(uint32_t va);
    uint32_t take_redirect();          // 0 = none (consumes)
    uint32_t sentinel() const { return sentinel_; }

private:
    struct Mod {
        std::string key;
        std::unique_ptr<PeImage> img;
    };
    // One trap slot per distinct native target. prof_* are only written under
    // PROF_COUNTERS builds (per-shim call count + cumulative shim-body time).
    // tagc: the same text as shim.tag, but as a STABLE pointer — it points
    // into the key of slot_by_tag_ (a std::map: nodes never move, nothing is
    // ever erased from it). shim.tag itself does not work here: slots_ is a
    // vector, a hot-path growth (GetProcAddress / LoadLibrary) relocates the
    // TrapSlots, and a short tag ("ws2_32.dll!#18") lives in SSO — its
    // c_str() would then dangle.
    struct TrapSlot { Shim shim; uint32_t va; const char* tagc = nullptr;
                      uint64_t prof_calls = 0, prof_ns = 0;
                      // (build -DD2_TLSCOUNT) calls to E() attributable to
                      // THIS slot — the full crossing, from the entry
                      // trap_retaddr to the exit trap_epilogue, shim body
                      // included. The fields always exist (two words per
                      // slot, a few hundred slots) so TrapSlot's layout does
                      // not depend on a -D flag: a struct whose size changes
                      // with a build flag is an ODR violation waiting to
                      // happen. Only the INCREMENT is conditional.
                      uint64_t tls_calls = 0, tls_traps = 0; };
    // Snapshot of each slot's prof_ns at the last prof_window() call (for windowing).
    std::vector<uint64_t> prof_prev_;

    Cpu* cpu_;
    std::vector<Mod> mods_;
    // dll(lower)!name → shim ; dll(lower)!#ord → shim
    std::map<std::string, Shim> shims_;
    std::function<uint32_t(Cpu&, const std::string&)> default_shim_;
    std::function<std::vector<uint8_t>(const std::string&)> version_src_;
    std::vector<TrapSlot> slots_;
    std::map<std::string, uint32_t> slot_by_tag_;  // tag → trap va

    uint32_t next_base_ = 0x30000000;   // module load bases grow from here
    uint32_t module_limit_ = 0;         // 0 = no bound; otherwise the (exclusive) end of the module window
public:
    // Upper bound of the auto-placed module window. A module that would
    // exceed it is REFUSED (add_module returns null + err) instead of being
    // placed over the neighboring region of the host memory map, silently
    // overwriting guest callback stubs there.
    void set_module_limit(uint32_t end) { module_limit_ = end; }
private:
    uint32_t trap_base_ = 0x7F000000;   // native-thunk window
    uint32_t trap_next_ = 0x7F000000;
    uint32_t trap_hi_   = 0x7F100000;
    uint32_t stack_base_ = 0x60000000;
    uint32_t stack_size_ = 0x00200000;  // 2 MiB guest stack
    uint32_t sentinel_   = 0x7F0FFFF0;  // "call finished" retaddr (inside trap window)
    bool committed_ = false;
    uint32_t unresolved_ = 0;

    Mod* find(const std::string& key);
    uint32_t alloc_trap(const Shim& s);
    bool trap_handler(Cpu& cpu, uint32_t trap_va);
    static std::string lc(std::string s);
};

} // namespace d2rt
