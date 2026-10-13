#include "diagnostics/call_trace.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "diagnostics/log.h"

#include <map>
#include <set>
#include <string>
#include <cstring>

using namespace d2rt;

// iteration-G138: the G67/G68 watch (a closed investigation of stale stack residue) ran before EVERY resolved import's shim: a std::map lookup, a std::set insert, string compares and
// three guest reads per host call, plus the explicit duplicates in the Glide entry points. It is diagnostics only (it never intercepts: the intrinsic returns false). In the LIGHT build
// nothing is armed, so no import pays for it; the FULL build is the G137 code unchanged.
#if !IGN_DIAG_FULL
namespace ign {
void arm_call_trace(Bridge&, Cpu&, const std::vector<ImportRef>&) { IGN_LOG("[CALL]", "call trace not armed (IGN_DIAGNOSTICS_LIGHT)"); }
void dump_g67_watch_ring() {}
bool g68_get_last_watch_sample(uint32_t&, uint32_t&, uint32_t&) { return false; }
}   // namespace ign
#else

namespace ign {

namespace {

// Process-lifetime, like guest_scratch's allocations: built once by
// arm_call_trace(), read for the rest of the run. Cpu::IntrinsicFn is a
// plain function pointer (no captures), so the per-slot tag has to live
// somewhere static rather than in a closure.
std::map<uint32_t, std::string>& trace_names() {
    static std::map<uint32_t, std::string> m;
    return m;
}
std::set<uint32_t>& trace_seen() {
    static std::set<uint32_t> s;
    return s;
}

// iteration-G67, Parte 16-20/47-48: a low-overhead cross-shim watch for
// ONLY the three guest bytes that hold V3.r/g/b's stale stack residue in
// Ignition's own PATH_B quad function (0x00451f10-0x00452263). Piggybacks
// on this file's OWN existing "fires before every resolved import's real
// shim, on every call, for the whole run" mechanism (Cpu::set_intrinsic,
// already armed broadly across kernel32/user32/winmm/dinput/dsound/
// glide2x -- every DLL!name pair the guest actually imports) instead of
// touching vendored dynarec dispatch or adding a new global instruction
// trace, neither of which this project can do (Cpu::set_code_hook is a
// confirmed no-op, unchanged since G16). No log unless the watched
// content changes; the ring is RAM-only and dumped exactly once, from
// glide2x_ign.cpp's own existing one-shot [G47-NONFINITE] failure branch.
//
// The watch target is a FIXED guest stack address rather than something
// computed per-call: G64's own real-hardware run measured V3 at exactly
// 0x0661EC1C on this exact build, and this project's own established
// practice (kG56TargetAddrA/B, G56-G62) already relies on guest stack/
// heap addresses being deterministic run-to-run for the SAME binary with
// no allocator/heap changes -- true here, since G65/G66 made none.
// Watching unconditionally from process start (Part 19 would prefer a
// later activation point, but choosing a wrong one risks missing the
// real transition per Part 21) is acceptable overhead: three guest
// memory reads and three integer compares per host call, not per guest
// instruction.
constexpr uint32_t kG67WatchV3Base = 0x0661EC1Cu;
constexpr uint32_t kG67WatchAddrR = kG67WatchV3Base + 0xCu;
constexpr uint32_t kG67WatchAddrG = kG67WatchV3Base + 0x10u;
constexpr uint32_t kG67WatchAddrB = kG67WatchV3Base + 0x14u;

struct G67WatchEvent {
    uint32_t seq;
    const char* shim;
    uint32_t caller_ret;
    uint32_t esp;
    uint32_t r_old, g_old, b_old;
    uint32_t r_new, g_new, b_new;
};
constexpr int kG67WatchRingSize = 128;
G67WatchEvent g_g67WatchRing[kG67WatchRingSize];
int g_g67WatchRingHead = 0;
int g_g67WatchRingCount = 0;
uint32_t g_g67WatchSeq = 0;
bool g_g67WatchHaveLast = false;
uint32_t g_g67WatchLastR = 0, g_g67WatchLastG = 0, g_g67WatchLastB = 0;

// iteration-G68, Parte 5-7: empirical audit of the watch mechanism
// itself, rather than assuming its coverage from reading vendored
// dispatch code alone. Three cheap counters (a couple of string
// compares per host call, not a map) answer directly: does this
// function get INVOKED AT ALL for glide2x imports, and specifically for
// guDrawTriangleWithClip, regardless of whether the watched value ever
// changes during those calls (which is a SEPARATE question from
// coverage).
uint32_t g_g68AuditTotalSamples = 0;
uint32_t g_g68AuditGlideSamples = 0;
uint32_t g_g68AuditGuDrawSamples = 0;

void g67_watch_sample(Cpu& c, const char* shimName) {
    ++g_g68AuditTotalSamples;
    if (shimName) {
        if (std::strncmp(shimName, "glide2x.dll!", 12) == 0) {
            ++g_g68AuditGlideSamples;
            if (std::strcmp(shimName, "glide2x.dll!guDrawTriangleWithClip") == 0) {
                ++g_g68AuditGuDrawSamples;
            }
        }
    }
    uint32_t r = c.read_u32(kG67WatchAddrR);
    uint32_t g = c.read_u32(kG67WatchAddrG);
    uint32_t b = c.read_u32(kG67WatchAddrB);
    if (g_g67WatchHaveLast && r == g_g67WatchLastR && g == g_g67WatchLastG && b == g_g67WatchLastB) {
        return;   // unchanged -- no event, no log (Part 18)
    }
    uint32_t esp = c.reg(R_ESP);
    G67WatchEvent& ev = g_g67WatchRing[g_g67WatchRingHead];
    ev.seq = g_g67WatchSeq++;
    ev.shim = shimName;
    ev.caller_ret = c.read_u32(esp);   // top of stack at intrinsic-fire time == the call's own return address
    ev.esp = esp;
    ev.r_old = g_g67WatchHaveLast ? g_g67WatchLastR : 0; ev.g_old = g_g67WatchHaveLast ? g_g67WatchLastG : 0; ev.b_old = g_g67WatchHaveLast ? g_g67WatchLastB : 0;
    ev.r_new = r; ev.g_new = g; ev.b_new = b;
    g_g67WatchRingHead = (g_g67WatchRingHead + 1) % kG67WatchRingSize;
    if (g_g67WatchRingCount < kG67WatchRingSize) ++g_g67WatchRingCount;
    g_g67WatchHaveLast = true;
    g_g67WatchLastR = r; g_g67WatchLastG = g; g_g67WatchLastB = b;
}

} // namespace

// iteration-G68, Parte 21-22: G68_DIAGNOSTIC_ONLY explicit sampling
// point, callable from a small hand-picked set of already-implemented
// Glide _impl functions (guColorCombineFunction/guAlphaSource/
// grConstantColorValue/grAlphaBlendFunction/grChromakeyMode/
// grClipWindow/grBufferSwap, per Part 22) as insurance against Part 5-7's
// own open question -- if the generic call-trace intrinsic turns out to
// have a real coverage gap for glide2x.dll specifically, these explicit
// call sites still feed the SAME ring/audit via g67_watch_sample,
// independent of that mechanism. Reuses g67_watch_sample directly (same
// TU) rather than duplicating the ring-write logic. NOT wired into
// arbitrary/internal guest code -- only into functions this project
// already fully implements and controls the entry of.
void g68_explicit_sample(Cpu& c, const char* label) {
    g67_watch_sample(c, label);
}

namespace {

bool call_trace_intrinsic(Cpu& c, uint32_t slot_va) {
    auto it = trace_names().find(slot_va);
    const char* name = it != trace_names().end() ? it->second.c_str() : "(unknown slot)";
    if (trace_seen().insert(slot_va).second) {   // true only the first time this VA is hit
        IGN_LOG("[CALL]", "%s", name);
    }
    g67_watch_sample(c, name);
    return false;   // never intercept -- Bridge's real shim still runs
}

} // namespace

void arm_call_trace(Bridge& br, Cpu& cpu, const std::vector<ImportRef>& imports) {
    int armed = 0;
    for (const auto& imp : imports) {
        if (imp.name.empty()) continue;   // ordinal import: no pre-link() way to get its trap VA
        uint32_t va = br.shim_trap(imp.dll, imp.name);   // 0 if nothing is registered for it yet
        if (!va) continue;
        trace_names()[va] = imp.dll + "!" + imp.name;
        cpu.set_intrinsic(va, call_trace_intrinsic);
        ++armed;
    }
    IGN_LOG("[CALL]", "call trace armed on %d resolved import(s)", armed);
}

// iteration-G68, Parte 6: lets the caller (guDrawTriangleWithClip_impl's
// own one-shot failure branch) compare its OWN direct read of V3.r/g/b
// against the watcher's last recorded sample, independent of whether the
// generic watch mechanism ever recorded a CHANGE event for this exact
// call. Returns false if the watch never sampled anything yet.
bool g68_get_last_watch_sample(uint32_t& r, uint32_t& g, uint32_t& b) {
    if (!g_g67WatchHaveLast) return false;
    r = g_g67WatchLastR; g = g_g67WatchLastG; b = g_g67WatchLastB;
    return true;
}

// iteration-G67, Parte 20/48-49: dumps the G67 watch ring exactly once,
// in chronological order, from the caller's own one-shot failure branch
// (glide2x_ign.cpp). Oldest-to-newest so the LAST entry printed is the
// closest one to the actual halt -- if 0xFFFFEEE0 first appears mid-ring,
// this is where "shim/caller immediately before/after the transition"
// (deliverable #13/14) comes from directly.
void dump_g67_watch_ring() {
    // iteration-G68, Parte 5-7: empirical coverage audit -- see
    // g67_watch_sample's own G68 comment. total==0 would mean the
    // intrinsic never fires at all (a real arming bug); glide==0 with
    // total>0 would mean it fires for other DLLs but not glide2x
    // specifically; gudraw>0 with zero ring events attributable to it
    // means the intrinsic DOES fire on every guDrawTriangleWithClip call
    // but the watched value simply never changes during those specific
    // calls (WATCH_CASE_C) -- a completely different, much less
    // surprising explanation than a broken watcher.
    IGN_LOG("[G68-WATCH-AUDIT]", "total_watch_samples=%u glide2x_samples=%u guDrawTriangleWithClip_samples=%u "
                               "(interpretation: total==0 -> WATCH_CASE_A [intrinsic never fires]; "
                               "glide2x_samples==0 with total>0 -> WATCH_CASE_A restricted to glide2x; "
                               "guDrawTriangleWithClip_samples>0 -> intrinsic DOES fire on this shim, so any "
                               "missing ring event for it is WATCH_CASE_C [fires correctly, value simply "
                               "never changed during those specific calls], not a broken watcher)",
            g_g68AuditTotalSamples, g_g68AuditGlideSamples, g_g68AuditGuDrawSamples);
    // iteration-G80, Parte 34/36: G79's own real-hardware run on the NEW
    // caller (0x00451EDB, never audited before) printed the FULL ring --
    // up to kG67WatchRingSize (128) [G67-WATCH-CHANGE] lines in one shot,
    // exactly the "enormous spam" this iteration was asked to cut. Capped
    // to the LAST kG80MaxWatchDumpEvents (16) events -- chronologically
    // closest to the actual halt, which is the only part this project's
    // own provenance-closure logic (g70_log_provenance_closure) has ever
    // actually needed. RAM ring itself is unchanged (still keeps all 128
    // for in-memory lookups); only how many get PRINTED is reduced.
    constexpr int kG80MaxWatchDumpEvents = 16;
    int totalToConsider = g_g67WatchRingCount;
    int printCount = (totalToConsider < kG80MaxWatchDumpEvents) ? totalToConsider : kG80MaxWatchDumpEvents;
    IGN_LOG("[G67-WATCH-DUMP]", "ring_capacity=%d events_recorded=%u events_kept=%d events_printed=%d (last %d only, see G80 Parte 34)",
            kG67WatchRingSize, g_g67WatchSeq, g_g67WatchRingCount, printCount, kG80MaxWatchDumpEvents);
    int start = (g_g67WatchRingCount < kG67WatchRingSize) ? 0 : g_g67WatchRingHead;
    int skip = totalToConsider - printCount;
    for (int i = skip; i < totalToConsider; ++i) {
        const G67WatchEvent& ev = g_g67WatchRing[(start + i) % kG67WatchRingSize];
        IGN_LOG("[G67-WATCH-CHANGE]", "seq=%u shim=%s caller=0x%08x esp=0x%08x r_old=0x%08x g_old=0x%08x b_old=0x%08x r_new=0x%08x g_new=0x%08x b_new=0x%08x",
                ev.seq, ev.shim, ev.caller_ret, ev.esp, ev.r_old, ev.g_old, ev.b_old, ev.r_new, ev.g_new, ev.b_new);
    }
}

} // namespace ign
#endif   // IGN_DIAG_FULL
