// win32/g157_split.h -- iteration-G157, PURE (no GL, no Vita API, no logging, no heap, no global state; the guest memory goes through a small interface so tools/g157_test.cpp can run the same decision against the ORIGINAL menu code).
//
// WHAT THE MENU OF Ign_3dfx.exe IS (static analysis, tmp/g157_dis_*.txt, tools/g157_test.cpp runs the real code):
//  * a generic menu engine (0x00445800-0x00446B80). The current menu NODE is [0x0049C298] (a 24-byte record {frame proc, input proc, key->command table, item count, item array, selected item}), the selected item
//    index is [0x0049C0C8]. A node is reached through a data-driven transition table (0x004801A8, 25 paths of (selection+1, node id) pairs; node ids index the node array 0x00472088 of the language in use).
//  * the key loop (0x00445DF0) consumes one pressed-key flag at a time (0x00446570) and calls the INPUT PROC of the current node with (key, command): command 6 is "confirm", -6 "cancel", +-1..5 move / change. The input proc of
//    EVERY node is the same function, 0x004065F0. When it returns 0x441 the engine takes the key as handled and does nothing else (loop on to the next key). Otherwise the engine runs its own confirm (0x0044627E -> navigate 0x00446640).
//  * the node variables 0x0049068C.. hold the nodes of the language in use; [0x00490694] is the MULTIPLAYER menu: two items, item 0 = SPLIT SCREEN, item 1 = NETWORK GAME. (Transition table: path main-menu item 2 -> node id 0; item 1 of it ->
//    node 0x10 -> car selection of player 1 (0x30) -> player 2 (0x31) -> track (0x60) = split screen; item 2 of it -> node 0x05 = network.) The variables are filled by the game for the language chosen, so the test below does not depend on any text.
// WHAT THIS HEADER DECIDES: the confirm (command 6) of item 0 of the node [0x00490694] is swallowed (the input proc is told "handled": return 0x441) before the game's own code for that node runs. Nothing else is touched.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g157 {

constexpr uint32_t kHandler = 0x004065F0u;            // input proc of every menu node; first 5 bytes: sub esp,0xC; push ebx; push ebp   (83 EC 0C 53 55)
constexpr uint32_t kHandlerResume = 0x004065F5u;      // the instruction after them
constexpr uint32_t kHandlerHi = 0x00406D28u;          // end of the function body (its jump table follows)
constexpr uint64_t kHandlerFnv = 0xa8d87f3bfcac48caull;   // FNV-1a of [kHandler, kHandlerHi)
constexpr uint32_t kEngineLo = 0x00445DF0u, kEngineHi = 0x00446500u;   // the key loop: calls the input proc, treats 0x441 as "handled"
constexpr uint64_t kEngineFnv = 0x78f293c18c4fb524ull;    // FNV-1a of [kEngineLo, kEngineHi)
constexpr uint32_t kHandledResult = 0x441u;           // what the input proc returns to say "this key is handled"
constexpr uint32_t kAddrCurNode = 0x0049C298u;        // current menu node (engine)
constexpr uint32_t kAddrCurSel = 0x0049C0C8u;         // selected item index of the current node (engine)
constexpr uint32_t kAddrMultiNode = 0x00490694u;      // the MULTIPLAYER node of the language in use
constexpr uint32_t kNodeItemCountOff = 0x0Cu, kNodeInputProcOff = 0x04u;
constexpr uint32_t kMultiItemCount = 2u;
constexpr uint32_t kSplitItem = 0u;                   // item 0 of the multiplayer node
constexpr int kCmdConfirm = 6;

// the control file token that leaves the menu exactly as G156 (A/B / emergency)
inline bool off_from_text(const char* text) {
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == 8 && memcmp(p, "g157=off", 8) == 0) return true;
        p = e;
    }
    return false;
}

enum Why : uint8_t { kNotBlocked = 0, kBlockedSplit, kWhyNotConfirm, kWhyNoNode, kWhyOtherNode, kWhyOtherItem, kWhyShape };
inline const char* why_name(uint32_t w) {
    static const char* const n[] = {"not_blocked", "blocked_split_screen_confirm", "not_a_confirm", "no_current_node", "other_menu_node", "other_item", "node_shape_unexpected"};
    return w < sizeof n / sizeof n[0] ? n[w] : "?";
}
struct Decision { Why why = kNotBlocked; uint32_t node = 0, sel = 0; };

// M::r32(va, out) -> bool. cmd = the second argument of the input proc (the low byte is the command).
template <class M>
inline Decision decide(M& m, uint32_t cmdArg) {
    Decision d;
    if ((int)(int8_t)(uint8_t)(cmdArg & 0xFFu) != kCmdConfirm) { d.why = kWhyNotConfirm; return d; }
    uint32_t node = 0, multi = 0, sel = 0;
    if (!m.r32(kAddrCurNode, node) || node == 0u) { d.why = kWhyNoNode; return d; }
    if (!m.r32(kAddrMultiNode, multi) || multi == 0u || node != multi) { d.why = kWhyOtherNode; d.node = node; return d; }
    uint32_t count = 0, proc = 0;
    if (!m.r32(node + kNodeItemCountOff, count) || !m.r32(node + kNodeInputProcOff, proc) || count != kMultiItemCount || proc != kHandler) { d.why = kWhyShape; d.node = node; return d; }
    if (!m.r32(kAddrCurSel, sel)) { d.why = kWhyShape; d.node = node; return d; }
    d.node = node; d.sel = sel;
    if (sel != kSplitItem) { d.why = kWhyOtherItem; return d; }
    d.why = kBlockedSplit; return d;
}

struct Stats { uint64_t calls = 0, blocked = 0, other[8] = {}; uint32_t installed = 0; };
inline void note(Stats& s, const Decision& d) { ++s.calls; if (d.why == kBlockedSplit) ++s.blocked; else ++s.other[d.why < 8 ? d.why : 0]; }

} }   // namespace ign::g157
