// win32/g160_clocks.h -- iteration-G160, PURE (no Vita API, no log, no heap, no global state; the four getters and four setters of the Vita's power service go through a small interface so tools/g160_test.cpp can drive the SAME code with
// a simulated console).
//
// POLICY ("minimum profile"): the port asks, ONCE at start-up, for at least ARM 444 MHz, BUS 222, GPU 222, GPU XBAR 166 (the profile measured in the G158 / G159 real runs). Per component:
//   * effective frequency BELOW the target       -> the setter is called with the target (one call, never repeated);
//   * effective frequency EXACTLY the target     -> no call;
//   * effective frequency ABOVE the target (a plugin such as PSVshell / LOLIcon chose 500 MHz, say) -> kept, no call: the port never lowers a profile somebody else chose and never asks for more than 444 / 222 / 222 / 166;
//   * a frequency that cannot be read (getter <= 0) -> that component is not touched (there would be nothing to restore).
// A setter that fails (negative rc) or whose effect is not the requested value is reported (result=PARTIAL); the game goes on with the effective frequencies.
// RESTORE (once, idempotent, at the normal end of the process): only the components the port really changed (setter ok AND the effective frequency differed afterwards) and only if their effective frequency is still the one the port applied.
// A component somebody else changed in the meantime is left alone.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g160 {

constexpr int kN = 4;
enum Comp { kArm = 0, kBus = 1, kGpu = 2, kXbar = 3 };
constexpr const char* kCompName[kN] = {"arm", "bus", "gpu", "xbar"};
struct Clocks { int mhz[kN]; };
constexpr Clocks kTarget = {{444, 222, 222, 166}};
constexpr int kNotCalled = 0x7FFFFFFF;                 // rc marker of a setter that was not called

enum Result : uint8_t { kUnchanged = 0, kOk, kPartial };
inline const char* result_name(Result r) { return r == kOk ? "OK" : (r == kPartial ? "PARTIAL" : "UNCHANGED"); }

struct Profile {
    Clocks before = {{0, 0, 0, 0}}, after = {{0, 0, 0, 0}};
    int rc[kN] = {kNotCalled, kNotCalled, kNotCalled, kNotCalled};
    uint32_t attemptedMask = 0, changedMask = 0, higherMask = 0, unreadableMask = 0;   // changedMask = the components the port changed (setter ok and the effective value differs afterwards)
    Result result = kUnchanged;
    bool applyDone = false, restoreDone = false;
};
struct RestoreReport {
    bool ran = false;                                  // false: nothing was applied, or the restore already ran
    Clocks saved = {{0, 0, 0, 0}}, current = {{0, 0, 0, 0}};
    int rc[kN] = {kNotCalled, kNotCalled, kNotCalled, kNotCalled};
    uint32_t restoredMask = 0, preservedExternalMask = 0;
};

// P::get(int comp) -> MHz (<= 0: unreadable); P::set(int comp, int mhz) -> rc (< 0: error)
template <class P>
inline void apply(P& p, Profile& pr, const Clocks& target = kTarget) {
    if (pr.applyDone) return;                          // once
    pr.applyDone = true;
    for (int i = 0; i < kN; ++i) pr.before.mhz[i] = p.get(i);
    for (int i = 0; i < kN; ++i) {
        const uint32_t bit = 1u << i; const int cur = pr.before.mhz[i];
        if (cur <= 0) { pr.unreadableMask |= bit; continue; }
        if (cur > target.mhz[i]) { pr.higherMask |= bit; continue; }
        if (cur == target.mhz[i]) continue;
        pr.attemptedMask |= bit; pr.rc[i] = p.set(i, target.mhz[i]);
    }
    for (int i = 0; i < kN; ++i) pr.after.mhz[i] = p.get(i);
    bool partial = pr.unreadableMask != 0;
    for (int i = 0; i < kN; ++i) {
        const uint32_t bit = 1u << i; if (!(pr.attemptedMask & bit)) continue;
        if (pr.rc[i] >= 0 && pr.after.mhz[i] != pr.before.mhz[i]) pr.changedMask |= bit;
        if (pr.rc[i] < 0 || pr.after.mhz[i] != target.mhz[i]) partial = true;
    }
    pr.result = partial ? kPartial : (pr.attemptedMask ? kOk : kUnchanged);
}

template <class P>
inline RestoreReport restore(P& p, Profile& pr) {
    RestoreReport rep;
    if (!pr.applyDone || pr.restoreDone) return rep;   // idempotent: the second call (and a call before apply) does nothing
    pr.restoreDone = true; rep.ran = true; rep.saved = pr.before;
    for (int i = 0; i < kN; ++i) rep.current.mhz[i] = p.get(i);
    for (int i = kN - 1; i >= 0; --i) {                // xbar, gpu, bus, arm
        const uint32_t bit = 1u << i; if (!(pr.changedMask & bit)) continue;
        if (rep.current.mhz[i] == pr.after.mhz[i]) { rep.rc[i] = p.set(i, pr.before.mhz[i]); rep.restoredMask |= bit; }
        else rep.preservedExternalMask |= bit;
    }
    return rep;
}

inline void rc_text(int rc, char* out) { if (rc == kNotCalled) std::snprintf(out, 12, "none"); else std::snprintf(out, 12, "0x%08x", (unsigned)rc); }
inline int format_start(char* buf, size_t n, const Profile& pr, const Clocks& t = kTarget) {
    char r[kN][12]; for (int i = 0; i < kN; ++i) rc_text(pr.rc[i], r[i]);
    char un[40] = ""; if (pr.unreadableMask) std::snprintf(un, sizeof un, " unreadable_mask=0x%x", (unsigned)pr.unreadableMask);
    return std::snprintf(buf, n, "before{arm=%d bus=%d gpu=%d xbar=%d} target_min{arm=%d bus=%d gpu=%d xbar=%d} rc{arm=%s bus=%s gpu=%s xbar=%s} after{arm=%d bus=%d gpu=%d xbar=%d} changed_mask=0x%x preserved_higher_mask=0x%x%s result=%s",
                         pr.before.mhz[0], pr.before.mhz[1], pr.before.mhz[2], pr.before.mhz[3], t.mhz[0], t.mhz[1], t.mhz[2], t.mhz[3], r[0], r[1], r[2], r[3],
                         pr.after.mhz[0], pr.after.mhz[1], pr.after.mhz[2], pr.after.mhz[3], (unsigned)pr.changedMask, (unsigned)pr.higherMask, un, result_name(pr.result));
}
inline int format_restore(char* buf, size_t n, const RestoreReport& rp) {
    char r[kN][12]; for (int i = 0; i < kN; ++i) rc_text(rp.rc[i], r[i]);
    return std::snprintf(buf, n, "saved{arm=%d bus=%d gpu=%d xbar=%d} current{arm=%d bus=%d gpu=%d xbar=%d} restored_mask=0x%x preserved_external_mask=0x%x rc{arm=%s bus=%s gpu=%s xbar=%s}",
                         rp.saved.mhz[0], rp.saved.mhz[1], rp.saved.mhz[2], rp.saved.mhz[3], rp.current.mhz[0], rp.current.mhz[1], rp.current.mhz[2], rp.current.mhz[3],
                         (unsigned)rp.restoredMask, (unsigned)rp.preservedExternalMask, r[0], r[1], r[2], r[3]);
}

} }   // namespace ign::g160
