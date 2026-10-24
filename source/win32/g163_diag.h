// win32/g163_diag.h -- iteration-G163, PURE (no GL, no Vita API, no logging, no heap, no global state; tools/g163_test.cpp drives exactly this code, and runs the ORIGINAL machine code of Ign_3dfx.exe as the oracle of the two rules).
//
// What the real test of G162 (logs.zip + the two ign_3dfx.btz of 19/10/2026) showed, and what this header holds:
//  1. THE CRASH. The draw that destroyed the process is the glyph of the first character of "(inverted-!)FELICIDADES!" (the Spanish text of the finale; the glyph record has a NEGATIVE width: cols = -14, rows = 12). The blitter of the game
//     (0x0045B965 / 0x0045B99C: `dec ecx; jne`) is a do-while: with a width <= 0 it counts down through 2^32 and writes 2 bytes per step over the whole address space. Every legitimate exit of that blitter for "nothing left to draw"
//     is a `jle` on the remaining size (the clip tests: 0x0045B8xx..); the case "the size is <= 0 BEFORE any clipping" is the only one that has no such exit. The guard of G162/G163 gives it the same exit: sprite_probe() answers 1 only
//     for a draw whose rectangle reaches the pixel loop with rows <= 0 or cols <= 0. Nothing else is skipped (tools/g163_test.cpp [C]: 300,000 random worlds against the G150 kernel and against an independent int64 reference).
//  2. THE UNLOCKS. ONE field drives both the Amateur league and the Banana: n = the number of leagues unlocked. The game keeps it in the saved configuration block (file offset 0x587 = [0x0061D947] in the race block, [0x00491A37] in the
//     menu copy) and sets it in the results code 0x00438516..0x004385B9 (reward() below = that code), BEFORE the finale screens are drawn; the block is written to ign_3dfx.btz by the race cleanup (0x0041CA0C) AFTER them.
//     The menu reads n twice: the difficulty selector (0x0040E637: item maximum = n) and the list of selectable cars (0x0040692D..0x00406972: car_list() below = that code; the cars 1 (Banana), 6, 9 and 10 are the locked ones, in
//     the order 10, 9, 6, 1: n = 1 unlocks car 1). [0x0061D94B] / [0x00491A3B] (m) is the Mirror-league completion (the fourth bonus car); it is NOT the Banana.
//  3. THE SAVE of the game is `fopen("wb")` + one WriteFile of 0x597 bytes + CloseHandle: the file is truncated first. G163 writes the .btz through a temporary file and replaces the old one only after a complete write (win32/g163_save.h).
// Nothing here changes what the game computes except the one decision of 1.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g150_hud.h"

namespace ign { namespace g163 {

// ---- guest addresses of Ign_3dfx.exe (sha256 9C40E7D2...B79B) ----------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kAddrMode = 0x0061D4B3u;     // index of the race inside the league (0..4 Novice, 0..5 Amateur, 0..6 Pro): the progress update tests the LAST race index 4 / 5 / 6
constexpr uint32_t kAddrLevel = 0x0061D3CCu;    // the difficulty in use (0 Novice, 1 Amateur, 2 Pro, 3 Mirror)
constexpr uint32_t kAddrU947 = 0x0061D947u;     // n: leagues unlocked (race block copy; file offset 0x587)
constexpr uint32_t kAddrU94B = 0x0061D94Bu;     // m: Mirror league completed (file offset 0x58B)
constexpr uint32_t kAddrFirst = 0x00509D40u;    // result table: index of the player ranked first (0 = the human player)
constexpr uint32_t kAddrSolo = 0x00536F68u;     // 0 = one human player
constexpr uint32_t kAddrCfg = 0x0061D3C0u, kCfgSize = 0x597u;   // the block that 0x0041CA0C / 0x0041E545 write to ign_3dfx.btz
constexpr uint32_t kMenuBlock = 0x004914B0u;    // the menu copy of the same 0x597 bytes (read from the file at the front-end start, 0x0040E5A3)
constexpr uint32_t kMenuN = 0x00491A37u, kMenuM = 0x00491A3Bu;   // n, m in the menu copy (= kMenuBlock + 0x587 / + 0x58B)
constexpr uint32_t kMenuCount = 0x00490780u, kMenuList = 0x0049060Cu, kMenuCheat = 0x00533808u;   // number of selectable cars, the list (11 dwords), the "all cars" key code flag
constexpr uint32_t kMenuSel1 = 0x004914F0u, kMenuCar1 = 0x004914CCu;   // selected position of player 1 in the list / the car id derived from it (0x00409DB5)
constexpr uint32_t kCarNodeVar = 0x0049069Cu;   // node variable of the car selection of player 1
constexpr uint32_t kBtzN = 0x587u, kBtzM = 0x58Bu, kBtzSel1 = 0x40u, kBtzCar1 = 0x1Cu, kBtzLevel = 0x0Cu, kBtzVersion = 0x1Fu;   // file offsets inside ign_3dfx.btz

// ---- the progress update (0x00438516..0x004385B9) --------------------------------------------------------------------------------------------------------------------------------------------------------
struct Progress { uint32_t mode = 0, level = 0, u947 = 0, u94b = 0; };
// `first`: [0x00509D40] == 0 and `solo`: [0x00536F68] == 0 are the two conditions at 0x00438516 / 0x00438522 (the human is ranked first in a one-player game). Signed compares exactly as the machine code. Returns true when a word changed.
inline bool reward(Progress& p, bool first, bool solo) {
    if (!first || !solo) return false;
    const Progress o = p;
    if (p.mode == 6u) {                                              // 0x0043852E: the last race of the 7-race league
        if (p.level == 3u) p.u94b = 1u;                              // 0x00438546
        else if ((int32_t)p.level <= 2) { if ((int32_t)p.u947 < 3) p.u947 = 3u; p.level = 3u; }   // 0x00438553 / 0x0043855B / 0x00438561
    } else if (p.mode == 5u) {                                       // 0x00438569: the last race of the 6-race league
        if ((int32_t)p.level <= 1) { if ((int32_t)p.u947 < 2) p.u947 = 2u; p.level = 2u; }      // 0x0043856E / 0x00438585 / 0x0043858A
    } else if (p.mode == 4u) {                                       // 0x00438591: the last race of the 5-race league (Novice)
        if (p.level == 0u) { if ((int32_t)p.u947 < 1) p.u947 = 1u; p.level = 1u; }               // 0x00438596 / 0x004385A6 / 0x004385AC
    }
    return p.mode != o.mode || p.level != o.level || p.u947 != o.u947 || p.u94b != o.u94b;
}

// ---- the list of selectable cars (0x0040692D..0x00406972; the cheat of 0x00406DB8 fills 0..10) -------------------------------------------------------------------------------------------------------------------
constexpr uint8_t kLockedOrder[4] = {10, 9, 6, 1};   // the bytes the input proc stores at [esp+0x10..0x13] (0x004065F7): the cars of the four bonus unlocks, last unlock first
constexpr uint32_t kCarBanana = 1u;                  // abbreviation table 0x00487A84: COO BAN VAC ENF RED BUS MON BUG SMO VEG IGN = car ids 0..10
inline uint32_t car_list(uint32_t n, uint32_t m, bool cheat, uint32_t out[11]) {
    uint32_t cnt = 0;
    if (cheat) { for (uint32_t i = 0; i < 11u; ++i) out[cnt++] = i; return cnt; }
    int32_t excl = 4 - (int32_t)n; if (m != 0u) --excl;                   // 0x0040693B / 0x00406949: how many of the locked ids are still excluded
    for (uint32_t id = 0; id < 11u; ++id) {
        int32_t k = 0;
        if (excl > 0) { for (; k < excl; ++k) if ((uint32_t)kLockedOrder[k] == id) break; }
        if (k == excl) out[cnt++] = id;                                   // 0x00406964: added only when no excluded id matched (and never when excl < 0)
    }
    return cnt;
}
inline bool list_has(const uint32_t* l, uint32_t cnt, uint32_t id) { for (uint32_t i = 0; i < cnt; ++i) if (l[i] == id) return true; return false; }

// ---- a saved configuration block as a file (ign_3dfx.btz: 4-byte version 0x1F + the block) ------------------------------------------------------------------------------------------------------------------------
inline bool btz_valid(const uint8_t* p, size_t n) { uint32_t v = 0; if (!p || n != kCfgSize) return false; std::memcpy(&v, p, 4); return v == kBtzVersion; }
inline uint32_t btz_word(const uint8_t* p, uint32_t off) { uint32_t v = 0; std::memcpy(&v, p + off, 4); return v; }

// ---- the probe of a refused sprite draw (the guard) -------------------------------------------------------------------------------------------------------------------------------------------------------------
// M: bool r32(uint32_t va, uint32_t& v);  const uint8_t* rspan(uint32_t va, uint64_t n)   (nullptr = not readable)
// The same reads and arithmetic as ign::g150::sprite_blit up to the point where the sequence draws. Returns 1 = the original instructions would reach the pixel loop with rows <= 0 or cols <= 0 (it would never end); 0 = anything else
// (a legitimate shape the native kernel does not handle, an early exit, another path); -1 = a word was not readable.
struct SpriteFacts {
    uint32_t S = 0, w0 = 0, flag = 0, R = 0, P = 0, r[7] = {}, px = 0, py = 0, top = 0, bottom = 0, left = 0, right = 0, pitch = 0, lfb = 0, srcPtr = 0, dst = 0;
    int32_t destX = 0, destY = 0, cols0 = 0, rows0 = 0, cols = 0, rows = 0; int stage = 0;   // stage: 1 = the draw point was reached
    uint32_t cmdObj = 0, handle = 0, rec = 0; int32_t recW = 0, recH = 0;
};
template <class M>
inline int sprite_probe(M& m, uint32_t S, SpriteFacts& f) {
    using namespace ign::g150;
    f = SpriteFacts(); f.S = S;
    if (!m.r32(S + 0x0Cu, f.w0)) return -1;
    if (!m.r32(kLfbMode, f.flag)) return -1;
    if (!m.r32(S + 0x08u, f.R) || !m.r32(S + 0x04u, f.P)) return -1;
    for (uint32_t i = 0; i < 7u; ++i) if (!m.r32(f.R + 4u * i, f.r[i])) return -1;
    const uint8_t* b9p = m.rspan(f.R + 9u, 1u); if (!b9p) return -1;
    if (!m.r32(f.P, f.px) || !m.r32(f.P + 4u, f.py)) return -1;
    if (!m.r32(kClipTop, f.top) || !m.r32(kClipBottom, f.bottom) || !m.r32(kClipLeft, f.left) || !m.r32(kClipRight, f.right) || !m.r32(kLfbPitch, f.pitch) || !m.r32(kLfbPtr, f.lfb)) return -1;
    if (f.w0 != 0u || f.flag != 1u) return 0;   // the paths the original takes elsewhere (not the LFB loop)
    const uint32_t r0 = f.r[0], r1 = f.r[1], r2 = f.r[2], r3 = f.r[3], r4 = f.r[4], r5 = f.r[5], r6 = f.r[6];
    uint32_t srcPtr = ((r3 & 0xFFFFFF00u) | *b9p) + r6;
    int32_t cols = wrap_sub((int32_t)r4 >> 8, (int32_t)r2 >> 8), rows = wrap_sub((int32_t)r5 >> 8, (int32_t)r3 >> 8);
    f.cols0 = cols; f.rows0 = rows;
    int32_t ebx = wrap_sub((int32_t)f.px, (int32_t)r0) >> 8, ecx = wrap_sub((int32_t)f.py, (int32_t)r1) >> 8;
    f.destX = ebx; f.destY = ecx;
    if (ecx < (int32_t)f.top) {
        ecx = wrap_add(wrap_sub(0, ecx), (int32_t)f.top);
        const int32_t nr = wrap_sub(rows, ecx); const bool le = sub_le(rows, ecx); rows = nr; if (le) return 0;
        srcPtr += (uint32_t)ecx << 8; ecx = (int32_t)f.top;
    }
    { int32_t edx = wrap_add(ecx, rows); const bool le = sub_le(edx, (int32_t)f.bottom); edx = wrap_sub(edx, (int32_t)f.bottom);
      if (!le) { const int32_t nr = wrap_sub(rows, edx); const bool le2 = sub_le(rows, edx); rows = nr; if (le2) return 0; } }
    if (ebx < (int32_t)f.left) {
        ebx = wrap_add(wrap_sub(0, ebx), (int32_t)f.left);
        const int32_t nc = wrap_sub(cols, ebx); const bool le = sub_le(cols, ebx); cols = nc; if (le) return 0;
        srcPtr += (uint32_t)ebx; ebx = (int32_t)f.left;
    }
    { int32_t edx = wrap_add(ebx, cols); const bool le = sub_le(edx, (int32_t)f.right); edx = wrap_sub(edx, (int32_t)f.right);
      if (!le) { const int32_t nc = wrap_sub(cols, edx); const bool le2 = sub_le(cols, edx); cols = nc; if (le2) return 0; } }
    f.cols = cols; f.rows = rows; f.srcPtr = srcPtr; f.stage = 1;
    f.dst = (uint32_t)wrap_add(wrap_add((int32_t)((uint32_t)ebx << 1), (int32_t)((uint32_t)ecx * f.pitch)), (int32_t)f.lfb);
    // the sprite object of the command record (0x004F3510+4), its handle, record and size: what the log names
    { uint32_t o = 0, table = 0, count = 0; m.r32(0x004F3514u, o); m.r32(0x00504024u, table); m.r32(0x00504020u, count); f.cmdObj = o;
      uint32_t h = 0; if (o && m.r32(o, h)) { f.handle = h; uint32_t rec = 0; if ((int32_t)h > 0 && h < count && m.r32(table + 4u * h, rec) && rec) { f.rec = rec; uint32_t w = 0, hh = 0; if (m.r32(rec + 4u, w) && m.r32(rec + 8u, hh)) { f.recW = (int32_t)w; f.recH = (int32_t)hh; } } } }
    return (rows <= 0 || cols <= 0) ? 1 : 0;
}
inline bool skip_original(int probe) { return probe == 1; }

// Words of [esp, esp+window) that point into the game's code right after a call instruction (E8 rel32, FF 15 abs32, FF /2); a guess by shape, enough to name the screen.
template <class M>
inline uint32_t callers(M& m, uint32_t esp, uint32_t* out, uint32_t cap, uint32_t window = 0x300u) {
    uint32_t n = 0;
    for (uint32_t o = 0; o < window && n < cap; o += 4u) {
        uint32_t v = 0; if (!m.r32(esp + o, v)) break;
        if (v < 0x00401006u || v >= 0x0046A000u) continue;
        const uint8_t* t = m.rspan(v - 6u, 6u); if (!t) continue;
        const bool e8 = t[1] == 0xE8, ff15 = t[0] == 0xFF && t[1] == 0x15, ff2 = (t[4] == 0xFF && (t[5] & 0x38u) == 0x10u) || (t[3] == 0xFF && (t[4] & 0x38u) == 0x10u);
        if (e8 || ff15 || ff2) out[n++] = v;
    }
    return n;
}
// the first printable string pointed to by a word of [esp, esp+window): the text a draw routine was asked to show (at most 39 characters)
template <class M>
inline bool stack_string(M& m, uint32_t esp, char (&out)[40], uint32_t window = 0x200u) {
    for (uint32_t o = 0; o < window; o += 4u) {
        uint32_t v = 0; if (!m.r32(esp + o, v) || v < 0x00400000u || v >= 0x08000000u) continue;
        const uint8_t* q = m.rspan(v, 40u); if (!q) continue;
        uint32_t len = 0; while (len < 39u && q[len] >= 0x20u && q[len] < 0x7Fu) ++len;
        if (len < 4u || (len < 39u && q[len] != 0u)) continue;
        std::memcpy(out, q, len); out[len] = 0; return true;
    }
    out[0] = 0; return false;
}

} }   // namespace ign::g163
