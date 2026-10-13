// win32/g159_name.h -- iteration-G159, PURE (no GL, no Vita API, no logging, no heap, no global state; guest memory, the Vita IME and the log go through small interfaces so tools/g159_test.cpp runs the SAME code against the ORIGINAL menu code).
//
// WHERE THE PLAYER NAME LIVES AND HOW THE GAME EDITS IT (static analysis of Ign_3dfx.exe, tmp/G159_NAME_ANALYSIS.txt; tools/g159_test.cpp executes the real menu engine natively):
//  * the name is a 4-byte C string in the option block of ign_3dfx.btz (block base 0x004914B0, size 0x597): player 1 at 0x0049151C, player 2 at 0x00491594 ("PL1\0", "PL2\0" by default, 0x0040E7D6..). 8-bit characters. Capacity 4 = three
//    characters + the terminator. The draw proc replaces an empty name by one space every frame (0x00406FF4).
//  * the menu item is item 0 of the node variables [0x004906B8] (player 1 options) and [0x004906BC] (player 2 options), 7 items each. The item carries a TEXT-EDIT record at item + 0x16: +4 = the name buffer, +0x10 (byte) = 4 = buffer
//    capacity, +0x14 (word) = 1 = text typing (3 = key capture, used by the controls screens).
//  * the menu engine edits text in its own state: word [0x0049BE58] (0 idle, 1 typing), an edit buffer at 0x0049BFC4 and the caret/length dword [0x0049C13C]. A confirm (command +6) on the item with state 0 sets state 1 and copies the name into the edit
//    buffer (0x0044629D..). While state is 1 each key whose scancode maps to a character in the table 0x0048A1B0 (84 bytes; 0xFF = none) appends it (a fourth character replaces the third), Backspace (0x0E) removes one. A confirm with state 1
//    (0x0044631D) writes the terminator at the caret and copies the edit buffer, terminator included, to the name buffer: that is the original VALIDATION/COMMIT. The option block is written to ign_3dfx.btz by the game when it is left (0x004051A0).
// WHAT THIS CODE DOES: the confirm of the exact item is swallowed at the menu input proc seam (the same 5 bytes G157 / G158 already use); the Vita's system keyboard (SceImeDialog) is opened over the running menu; its answer (UTF-16) is converted with
//    the game's own character table; the result is handed to the ORIGINAL commit: state word = 1, edit buffer = the new name, caret = its length, and the Enter key flag raised, so that the next key loop runs the original commit code once.
//    A cancel changes nothing at all.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g157_split.h"

namespace ign { namespace g159 {

namespace G7 = ign::g157;
constexpr uint32_t kAddrNodeP1 = 0x004906B8u, kAddrNodeP2 = 0x004906BCu;        // node variables: player 1 / player 2 options (7 items)
constexpr uint32_t kNameBufP1 = 0x0049151Cu, kNameBufP2 = 0x00491594u;          // the 4-byte name strings
constexpr uint32_t kNodeItems = 7u, kNameItem = 0u, kNameCapacity = 4u, kNameMaxChars = 3u;
constexpr uint32_t kNodeItemsOff = 0x10u, kItemStride = 0x1Cu, kItemCbOff = 0x10u, kItemEnabledOff = 0x1Au, kItemTextEditOff = 0x16u;
constexpr uint32_t kTeBufOff = 0x04u, kTeCapOff = 0x10u, kTeModeOff = 0x14u;    // text-edit record
constexpr uint32_t kModeText = 1u;
constexpr uint32_t kAddrEditState = 0x0049BE58u, kAddrEditBuf = 0x0049BFC4u, kAddrEditLen = 0x0049C13Cu, kAddrKeyFlagsPtr = 0x0049C0E0u;
constexpr uint32_t kKeyEnter = 0x1Cu;                                             // scancode whose flag the original confirm uses
constexpr uint32_t kCharTable = 0x0048A1B0u, kCharTableLen = 0x54u;
constexpr uint64_t kCharTableFnv = 0x7626f602bb64f99eull;                         // FNV-1a of the 84 bytes at kCharTable (Enter = 0x1C and Backspace = 0x0E map to 0xFF = "no character": they are commands)
constexpr uint32_t kWaitCommitFrames = 30u, kWaitStatusFrames = 240u, kCalmTicks = 6u;

inline uint64_t fnv1a(const uint8_t* p, size_t n) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; } return h; }

inline bool off_from_text(const char* text) {
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == 8 && memcmp(p, "g159=off", 8) == 0) return true;
        p = e;
    }
    return false;
}

// ---- the character set of the original typing code: the 84-byte table of scancode -> character (0xFF = none) ---------------------------------------------------------------------------------------------------------------
struct CharSet {
    bool allowed[256]; uint32_t count;
    void clear() { for (int i = 0; i < 256; ++i) allowed[i] = false; count = 0; }
    void from_table(const uint8_t* t, uint32_t n) { clear(); for (uint32_t i = 0; i < n; ++i) if (t[i] != 0xFFu && !allowed[t[i]]) { allowed[t[i]] = true; ++count; } }
};
// UTF-16 -> the game's characters: ASCII letters in upper case (the game's keyboard types capitals), everything the original table cannot type is dropped; at most max characters, never half of a sequence.
// Returns how many characters were produced; out is NOT terminated here.
inline uint32_t convert(const CharSet& cs, const uint16_t* text, uint32_t n, uint8_t* out, uint32_t max) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < max; ++i) {
        uint32_t c = text[i];
        if (c == 0) break;
        if (c >= 0x80u) continue;                       // not representable in the 8-bit set of the game (surrogates, accents, ...)
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (!cs.allowed[c]) continue;
        out[k++] = (uint8_t)c;
    }
    return k;
}
inline uint32_t name_to_utf16(const uint8_t* name, uint16_t* out) {   // the current name (up to 3 chars) as the dialog's initial text, NUL-terminated; unprintable bytes become '?'
    uint32_t n = 0; while (n < kNameMaxChars && name[n] != 0) { out[n] = (name[n] >= 0x20u && name[n] <= 0x7Eu) ? name[n] : '?'; ++n; } out[n] = 0; return n;
}

// ---- what the confirm is: the exact item, in the exact node, with the exact record -------------------------------------------------------------------------------------------------------------------------------------------
enum Why : uint8_t { kNo = 0, kOpen, kWhyNotConfirm, kWhyNoNode, kWhyOtherNode, kWhyOtherItem, kWhyShape, kWhyEditing, kWhyDisabled };
inline const char* why_name(uint32_t w) { static const char* const n[] = {"no", "open_name_ime", "not_a_confirm", "no_current_node", "other_menu_node", "other_item", "node_shape_unexpected", "engine_already_editing", "item_disabled"}; return w < sizeof n / sizeof n[0] ? n[w] : "?"; }
struct Target { uint32_t node = 0, buf = 0, which = 0; uint8_t old[4] = {0, 0, 0, 0}; };
struct Decision { Why why = kNo; Target t; };
// M: r8/r16/r32 (va, out) -> bool
template <class M>
inline Decision decide(M& m, uint32_t cmdArg) {
    Decision d;
    if ((int)(int8_t)(uint8_t)(cmdArg & 0xFFu) != G7::kCmdConfirm) { d.why = kWhyNotConfirm; return d; }
    uint32_t node = 0, p1 = 0, p2 = 0, sel = 0;
    if (!m.r32(G7::kAddrCurNode, node) || node == 0u) { d.why = kWhyNoNode; return d; }
    m.r32(kAddrNodeP1, p1); m.r32(kAddrNodeP2, p2);
    uint32_t which = 0, wantBuf = 0;
    if (p1 != 0u && node == p1) { which = 1; wantBuf = kNameBufP1; } else if (p2 != 0u && node == p2) { which = 2; wantBuf = kNameBufP2; }
    if (!which) { d.why = kWhyOtherNode; d.t.node = node; return d; }
    d.t.node = node; d.t.which = which;
    uint32_t count = 0, proc = 0, items = 0;
    if (!m.r32(node + G7::kNodeItemCountOff, count) || !m.r32(node + G7::kNodeInputProcOff, proc) || count != kNodeItems || proc != G7::kHandler || !m.r32(node + kNodeItemsOff, items) || items == 0u) { d.why = kWhyShape; return d; }
    if (!m.r32(G7::kAddrCurSel, sel)) { d.why = kWhyShape; return d; }
    if (sel != kNameItem) { d.why = kWhyOtherItem; return d; }
    uint16_t enabled = 0, cb = 0, mode = 0; uint8_t cap = 0; uint32_t te = 0, buf = 0;
    if (!m.r16(items + kItemEnabledOff, enabled) || !m.r16(items + kItemCbOff, cb) || !m.r32(items + kItemTextEditOff, te) || te == 0u) { d.why = kWhyShape; return d; }
    if (!m.r16(te + kTeModeOff, mode) || !m.r8(te + kTeCapOff, cap) || !m.r32(te + kTeBufOff, buf) || mode != kModeText || cap != kNameCapacity || buf != wantBuf || cb != 0) { d.why = kWhyShape; return d; }
    if (enabled == 0) { d.why = kWhyDisabled; return d; }               // the engine ignores a confirm on a disabled item
    uint16_t edit = 0;
    if (!m.r16(kAddrEditState, edit)) { d.why = kWhyShape; return d; }
    if (edit != 0) { d.why = kWhyEditing; return d; }                    // the engine is already typing (its own path): not ours
    bool term = false; for (uint32_t i = 0; i < kNameCapacity; ++i) { uint8_t b = 0; if (!m.r8(buf + i, b)) { d.why = kWhyShape; return d; } d.t.old[i] = b; if (b == 0) term = true; }
    if (!term) { d.why = kWhyShape; return d; }                           // an unterminated name: leave the engine alone
    d.t.buf = buf; d.why = kOpen; return d;
}

// ---- the state machine ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum State : uint8_t { kIdle = 0, kRequested, kActive, kCommit, kCooldown };
inline const char* state_name(uint32_t s) { static const char* const n[] = {"IDLE", "OPENING", "ACTIVE", "COMMIT", "COOLDOWN"}; return s < 5 ? n[s] : "?"; }
// Platform P: int open(const uint16_t* title, const uint16_t* initial, uint32_t maxChars)  (0 = ok); int status() (0 none, 1 running, 2 finished); int result(bool& accepted, uint16_t* text, uint32_t cap, uint32_t& len) (0 = ok); void term()
// Log L: open(old, max), commit(old, neu, valid), cancel(kept), error(stage, rc)
class Controller {
public:
    CharSet cs; State st = kIdle; Target t; uint32_t waitFrames = 0, calm = 0; uint8_t pending[4] = {0, 0, 0, 0}; uint32_t pendingN = 0;
    bool active() const { return st == kActive; }                          // the swap passes has_commondialog while true
    bool busy() const { return st != kIdle; }
    // the seam: true = swallow the confirm (the engine does nothing)
    template <class M>
    bool on_confirm(M& m, uint32_t cmdArg) {
        const Decision d = decide(m, cmdArg);
        if (d.why != kOpen) return false;
        if (st == kIdle) { st = kRequested; t = d.t; waitFrames = 0; calm = 0; }
        return true;                                                         // already busy: still swallowed (the engine must not start its own typing)
    }
    // once per presented frame, BEFORE the swap
    template <class M, class P, class L>
    void frame(M& m, P& p, L& lg) {
        switch (st) {
        case kRequested: {
            static const uint16_t kTitle[] = {'P', 'l', 'a', 'y', 'e', 'r', ' ', 'n', 'a', 'm', 'e', 0};
            uint16_t init[8]; name_to_utf16(t.old, init);
            const int rc = p.open(kTitle, init, kNameMaxChars);
            if (rc != 0) { lg.error("open", rc); st = kCooldown; calm = 0; break; }
            lg.open(t.old, kNameMaxChars); st = kActive; waitFrames = 0; break; }
        case kActive: {
            const int s = p.status();
            if (s == 2) {
                bool accepted = false; uint16_t text[16]; uint32_t len = 0; const int rr = p.result(accepted, text, 15, len); p.term();
                if (rr != 0) { lg.error("result", rr); st = kCooldown; calm = 0; break; }
                if (!accepted) { lg.cancel(t.old); st = kCooldown; calm = 0; break; }
                uint8_t neu[4] = {0, 0, 0, 0}; const uint32_t n = convert(cs, text, len, neu, kNameMaxChars);
                if (n == 0) { lg.error("no_valid_characters", 0); st = kCooldown; calm = 0; break; }
                if (!begin_commit(m, neu, n)) { lg.error("stale_menu", 0); st = kCooldown; calm = 0; break; }
                st = kCommit; waitFrames = 0; break;
            }
            if (s == 0 && ++waitFrames > kWaitStatusFrames) { p.term(); lg.error("status_none", 0); st = kCooldown; calm = 0; }
            break; }
        case kCommit: {
            uint16_t edit = 1; m.r16(kAddrEditState, edit);
            if (edit == 0) {                                                 // the original commit ran
                uint8_t got[4] = {0, 0, 0, 0}; for (uint32_t i = 0; i < kNameCapacity; ++i) m.r8(t.buf + i, got[i]);
                bool ok = true; for (uint32_t i = 0; i < pendingN; ++i) if (got[i] != pending[i]) ok = false; if (got[pendingN] != 0) ok = false;
                if (ok) lg.commit(t.old, pending, pendingN); else lg.error("commit_mismatch", 0);
                st = kCooldown; calm = 0; break;
            }
            { uint32_t flags = 0; if (m.r32(kAddrKeyFlagsPtr, flags) && flags != 0u) m.w8(flags + kKeyEnter, 1); }   // the input pump may clear the flag before the key loop sees it: raise it again until the original commit has run
            if (++waitFrames > kWaitCommitFrames) { m.w16(kAddrEditState, 0); lg.error("commit_timeout", 0); st = kCooldown; calm = 0; }
            break; }
        default: break;
        }
    }
    // the Vita pad as the game sees it: nothing while the keyboard is up, and afterwards nothing until every button and stick has been at rest for a few polls
    bool pad_filter(uint32_t& buttons, uint8_t& lx, uint8_t& ly, uint8_t& rx, uint8_t& ry) {
        if (st == kIdle) return false;
        if (st == kCooldown) {
            const bool rest = buttons == 0u && near128(lx) && near128(ly) && near128(rx) && near128(ry);
            calm = rest ? calm + 1u : 0u;
            if (calm >= kCalmTicks) { st = kIdle; calm = 0; return false; }
        }
        buttons = 0u; lx = ly = rx = ry = 128u; return true;
    }
private:
    static bool near128(uint8_t v) { return v >= 88u && v <= 168u; }
    // hands the new name to the ORIGINAL commit: engine state "typing", edit buffer = the name, caret = its length, and the Enter key flag; the next key loop runs the original confirm/commit once
    template <class M>
    bool begin_commit(M& m, const uint8_t* neu, uint32_t n) {
        uint32_t node = 0, sel = 0; uint16_t edit = 0; uint32_t flags = 0;
        if (!m.r32(G7::kAddrCurNode, node) || node != t.node || !m.r32(G7::kAddrCurSel, sel) || sel != kNameItem || !m.r16(kAddrEditState, edit) || edit != 0 || !m.r32(kAddrKeyFlagsPtr, flags) || flags == 0u) return false;
        for (uint32_t i = 0; i < 4; ++i) pending[i] = i < n ? neu[i] : 0;
        pendingN = n;
        for (uint32_t i = 0; i < kNameCapacity; ++i) if (!m.w8(kAddrEditBuf + i, i < n ? neu[i] : 0)) return false;   // the edit buffer holds exactly what typing the name would have left: the characters, then zeros
        if (!m.w32(kAddrEditLen, n) || !m.w16(kAddrEditState, 1)) return false;
        if (!m.w8(flags + kKeyEnter, 1)) { m.w16(kAddrEditState, 0); return false; }   // typing state without the Enter flag would wait for a key: put it back
        return true;
    }
};

} }   // namespace ign::g159
