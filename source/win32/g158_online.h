// win32/g158_online.h -- iteration-G158, PURE (no GL, no Vita API, no logging, no heap, no global state; the guest memory goes through a small interface so tools/g158_test.cpp can run the same decision against the ORIGINAL menu code).
//
// WHAT THE NETWORK GAME OPTION DOES IN Ign_3dfx.exe (static analysis, tmp/g158_dis_*.txt; tools/g158_test.cpp runs the menu engine natively):
//  * it is item 1 of the multiplayer node [0x00490694] (G157 blocks item 0 = split screen). In EVERY language it is the ONLY menu item of the game that carries an item callback: callback id 3 -> entry 2 of the callback table
//    [0x0049C0DC] (0x0046EB20) = the function 0x00406E60 (tmp/G158_ONLINE_ANALYSIS.txt lists all items of all 6 node sets).
//  * the engine's key loop (0x00445DF0) calls that callback for the item after the node's input proc (0x004065F0) has said "not handled" (0x00445F5D), with (item argument, command). 0x00406E60 starts with a network tear-down and, for the
//    confirm command (+6), prepares to leave the full-screen game window: two internal helpers, then `call 0x00449E40` = the thunk of glide2x!grGlideGetState (the return address 0x00406E97 of that call is the `caller_guest_eip` of the real
//    log), then DestroyWindow, ShowCursor and the DirectPlay connection dialog (0x00456E40 -> DialogBoxParamA). The port implements neither grGlideGetState nor DialogBoxParamA: the FIRST of them reached, grGlideGetState, is an
//    unimplemented Glide call, which the port's G20 mechanism turns into [STOP] / clean halt of the guest - the return to the LiveArea (G158 real log: `NEXT_UNIMPLEMENTED_GLIDE_CALL name=grGlideGetState caller_guest_eip=0x00406e97`).
//    So the exit is a side effect of a missing Glide function on the network path, not an intended quit and not an error return.
//  * after the callback the engine still runs its own confirm: navigate to the network node [0x004906A8].
// WHAT THIS HEADER DECIDES: the confirm (command 6) of item 1 of the node [0x00490694] is swallowed at the SAME seam as G157 (the node input proc returns 0x441 = "handled"), before the callback, the navigation and the node's own reactions run.
// The test names the option by structure, not by text: node variable + item index + the callback the item really carries (id 3 -> table entry -> 0x00406E60). If any of these is not as expected nothing is blocked (G157 behaviour).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g157_split.h"

namespace ign { namespace g158 {

namespace G7 = ign::g157;
constexpr uint32_t kOnlineProc = 0x00406E60u, kOnlineProcHi = 0x00406F3Cu;     // the callback of the network item (its first 6 bytes: 81 EC 38 01 00 00 = sub esp,0x138)
constexpr uint64_t kOnlineProcFnv = 0x89e2b95a9d682f5cull;                      // FNV-1a of [kOnlineProc, kOnlineProcHi)
constexpr uint32_t kAddrCallbackTable = 0x0049C0DCu;                            // pointer to the engine's item callback table (entry n-1 for callback id n)
constexpr uint32_t kOnlineItem = 1u;                                            // item index in the multiplayer node
constexpr uint32_t kOnlineCallbackId = 3u;                                      // the callback id of that item
constexpr uint32_t kNodeItemsOff = 0x10u, kItemStride = 0x1Cu, kItemCallbackOff = 0x10u;

inline bool off_from_text(const char* text) {
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == 8 && memcmp(p, "g158=off", 8) == 0) return true;
        p = e;
    }
    return false;
}

enum Why : uint8_t { kNotBlocked = 0, kBlockedOnline, kWhyNotConfirm, kWhyNoNode, kWhyOtherNode, kWhyOtherItem, kWhyShape, kWhyNoCallback };
inline const char* why_name(uint32_t w) {
    static const char* const n[] = {"not_blocked", "blocked_online_confirm", "not_a_confirm", "no_current_node", "other_menu_node", "other_item", "node_shape_unexpected", "item_has_not_the_network_callback"};
    return w < sizeof n / sizeof n[0] ? n[w] : "?";
}
struct Decision { Why why = kNotBlocked; uint32_t node = 0, sel = 0; };

// M::r32(va, out) -> bool (pointers read from the guest are in the guest's address space)
template <class M>
inline Decision decide(M& m, uint32_t cmdArg) {
    Decision d;
    if ((int)(int8_t)(uint8_t)(cmdArg & 0xFFu) != G7::kCmdConfirm) { d.why = kWhyNotConfirm; return d; }
    uint32_t node = 0, multi = 0, sel = 0;
    if (!m.r32(G7::kAddrCurNode, node) || node == 0u) { d.why = kWhyNoNode; return d; }
    if (!m.r32(G7::kAddrMultiNode, multi) || multi == 0u || node != multi) { d.why = kWhyOtherNode; d.node = node; return d; }
    uint32_t count = 0, proc = 0;
    if (!m.r32(node + G7::kNodeItemCountOff, count) || !m.r32(node + G7::kNodeInputProcOff, proc) || count != G7::kMultiItemCount || proc != G7::kHandler) { d.why = kWhyShape; d.node = node; return d; }
    if (!m.r32(G7::kAddrCurSel, sel)) { d.why = kWhyShape; d.node = node; return d; }
    d.node = node; d.sel = sel;
    if (sel != kOnlineItem) { d.why = kWhyOtherItem; return d; }
    uint32_t items = 0, cbWord = 0, table = 0, fn = 0;
    if (!m.r32(node + kNodeItemsOff, items) || items == 0u || !m.r32(items + kItemStride * sel + kItemCallbackOff, cbWord)) { d.why = kWhyShape; return d; }
    const uint32_t id = cbWord & 0xFFFFu;
    if (id != kOnlineCallbackId) { d.why = kWhyNoCallback; return d; }
    if (!m.r32(kAddrCallbackTable, table) || table == 0u || !m.r32(table + 4u * (id - 1u), fn) || fn != kOnlineProc) { d.why = kWhyNoCallback; return d; }
    d.why = kBlockedOnline; return d;
}

struct Stats { uint64_t calls = 0, blocked = 0, other[8] = {}; uint32_t active = 0; };
inline void note(Stats& s, const Decision& d) { ++s.calls; if (d.why == kBlockedOnline) ++s.blocked; else ++s.other[d.why < 8 ? d.why : 0]; }

} }   // namespace ign::g158
