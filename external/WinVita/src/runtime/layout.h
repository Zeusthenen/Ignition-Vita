// src/runtime/layout.h — global offset applied to the guest memory plan.
//
// Why: on Vita, guest memory is NOT identity-mapped — the kernel only hands
// out blocks >= 0x80000000, while the game lives low (compact plan
// 0x00500000..0x11900000). Hence a single arena and `membase = host - guest`:
// every x86 memory access becomes "ADD address+membase" then "LDR/STR", and
// that ADD sits on the critical path of every load.
//
// A full identity plan (membase = 0, guest address == host address, i.e.
// D2LAYOUT/WX86_LAYOUT="haut") would remove that ADD entirely, but requires
// placing ALL guest memory above 0x80000000. That breaks a container library
// that stores COMPLEMENTED pointers and distinguishes them from plain
// offsets by the SIGN BIT: above 2 GiB, `~p` becomes positive and the
// encoding silently misclassifies. Staying under 2 GiB (default 0x01000000)
// avoids that, at the cost of the extra ADD.
//
// Contract: D2LAYOUT/WX86_LAYOUT absent, or any value other than "haut",
// yields an offset of 0 — byte-for-byte the old behavior. "compact" keeps
// its exact meaning (null translation). The offset is read LAZILY (a local
// static): on Vita, env.txt is only applied after static initialization, so
// a pre-main getenv would never see the knob.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace d2rt {

// Offset applied to ALL guest regions. 0 = none (default). WX86_HI=<hex>
// (D2HI as a fallback name) changes the base (default 0x81000000, matching
// Vita's block granularity). WX86_* names are PRIMARY; the older,
// first-consumer-prefixed names are still accepted so existing setups keep
// working. Aligned to 1 MiB: the compact plan already is, and an encodable
// membase ADD needs zero low bits on the arena side; alignment is also kept
// here so addresses stay readable in logs.
inline uint32_t layout_hi() {
    static const uint32_t hi = [] () -> uint32_t {
        const char* l = std::getenv("WX86_LAYOUT");
        if (!l) l = std::getenv("D2LAYOUT");
        if (!l || std::strcmp(l, "haut")) return 0u;
        const char* h = std::getenv("WX86_HI");
        if (!h) h = std::getenv("D2HI");
        uint32_t v = h ? (uint32_t)std::strtoul(h, nullptr, 16) : 0x81000000u;
        return v & ~0xFFFFFu;
    }();
    return hi;
}

// True for both PACKED plans (compact and high): they share the same
// layout, only the offset changes.
inline bool layout_packed() {
    const char* l = std::getenv("WX86_LAYOUT");
    if (!l) l = std::getenv("D2LAYOUT");
    return l && (!std::strcmp(l, "compact") || !std::strcmp(l, "haut"));
}

// Bounds of the "user space" reported to the game (GetSystemInfo) and used
// by VirtualQuery introspection. In the high plan, user space IS the block:
// [HI, HI + 0x12000000). Without an offset, the classic Win32 values.
inline uint32_t layout_user_lo() { uint32_t h = layout_hi(); return h ? h : 0x00010000u; }
inline uint32_t layout_user_hi() { uint32_t h = layout_hi(); return h ? (h + 0x12000000u) : 0x7FFF0000u; }

} // namespace d2rt
