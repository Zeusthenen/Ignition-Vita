// win32/g150_hud.h -- iteration-G150, PURE (no GL, no Vita API, no logging, no heap, no global state). Native versions of the two pixel loops of the game that the G149 real run priced.
//
// WHAT THE REAL RUN SHOWED. The frame time of a race is 20.8 ms + 0.44 us/block + 5.1 us/crossing + 22.8 us/triangle (R2 0.975 over 95 windows) and the "guest code and the rest" is 14.0 ms + 0.2 us/block (R2 0.972): 14 ms of every frame
// is guest code that does not depend on the amount of game logic. The gap profiler found it: one lock/unlock pair per frame whose gap is 8.9 ms (0.9 ms of jitter over five tracks) in SIX translated blocks, and 33 sprite lock/unlock
// pairs of 110 us each in seven blocks. Six blocks in 8.9 ms is a pixel loop inside one dynarec block (WinVita: "a loop entirely inside one block never re-checks the budget"): the game's own software blitters, run by the dynarec at ~35 ns
// per pixel.
//   * DrawSurface (0x00458780, the "driver interface" slot 0x004F3348 of the game's 2D layer): when the width is a multiple of 160 and the pointers are aligned it expands an 8-bit canvas to RGB565 with the palette at 0x0061E520,
//     index 0 transparent (the loop 0x00458862..0x004588A1, per pixel `mov al,[esi]; or al,al; je; mov bx,[eax*2+0x61e520]; mov [edi],bx`);
//   * the sprite blitter (0x0045B821, one caller 0x004549CE): clips a 256-wide 8-bit sprite sheet rectangle against [0x623054..0x623060] and writes it to the LFB (0x623074, pitch 0x623068), index < 6 transparent, 6..15 through the table
//     at 0x0048EFA8, 16.. through 0x0061E520 (the loop 0x0045B965..0x0045B99C).
// The functions below do EXACTLY what those instruction sequences do to memory (same pixels, same order, same clipping arithmetic with the same signed comparisons, the same final values in the scratch globals 0x48F008/0C/10), reading and
// writing guest memory through host pointers. Anything outside the proven shape (unaligned, a pitch that is not positive, extents outside the arena, source and destination overlapping, a path the sequences do not take) is NOT handled:
// the caller then lets the ORIGINAL instructions run. tools/g150_test.cpp proves the equivalence by executing the original machine code of Ign_3dfx.exe natively (x86 host) on random inputs and comparing memory and registers.
#pragma once
#include <cstdint>
#include <cstring>

#ifndef G150_TRACE
#define G150_TRACE(n) ((void)0)   // the host test defines it to learn why a shape was handed back
#endif
namespace ign { namespace g150 {

// ---- the guest addresses of Ign_3dfx.exe (Ign_3dfx.exe sha256 in tools/g150_guest_bytes.inc) ------------------------------------------------------------------------------------------------------
struct RealAddrs {   // the addresses of Ign_3dfx.exe; the tests instantiate the kernels with the addresses of their own arena (the original machine code is patched to the same ones)
    static constexpr uint32_t kPalA = 0x0061E520u;          // u16[256]: 8-bit index -> RGB565
    static constexpr uint32_t kPalB = 0x0048EFA8u;          // u16[16]: indices 6..15 of the sprite blitter
    static constexpr uint32_t kSprSrc = 0x0048F008u, kSprCols = 0x0048F00Cu, kSprRows = 0x0048F010u;   // scratch globals of the sprite blitter (written before they are read, read nowhere else)
    static constexpr uint32_t kClipBottom = 0x00623054u, kClipTop = 0x00623058u, kClipRight = 0x0062305Cu, kClipLeft = 0x00623060u, kLfbPitch = 0x00623068u, kLfbPtr = 0x00623074u, kLfbMode = 0x004852A8u;
};
constexpr uint32_t kPalA = RealAddrs::kPalA, kPalB = RealAddrs::kPalB, kSprSrc = RealAddrs::kSprSrc, kSprCols = RealAddrs::kSprCols, kSprRows = RealAddrs::kSprRows;
constexpr uint32_t kClipBottom = RealAddrs::kClipBottom, kClipTop = RealAddrs::kClipTop, kClipRight = RealAddrs::kClipRight, kClipLeft = RealAddrs::kClipLeft, kLfbPitch = RealAddrs::kLfbPitch, kLfbPtr = RealAddrs::kLfbPtr, kLfbMode = RealAddrs::kLfbMode;
// hook sites (instruction starts; the 5 bytes there are replaced by a jmp to the trap) and the addresses the original code continues at
constexpr uint32_t kSitePal = 0x00458856u, kPalResume = 0x00458913u, kPalAfterPushes = 0x0045885Bu;      // DrawSurface: push eax,ebx,ecx,edx,esi | the pops end at jmp 0x458913
constexpr uint32_t kSiteSprite = 0x0045B821u, kSpriteGl = 0x0045B864u, kSpriteOther = 0x0045B836u, kSpriteBpp8 = 0x0045B9A4u;   // sprite blitter: pusha; cmp [esi+0xc],0 | LFB path | [esi+0xc]!=0 path | 8-bit surface path

// ---- the displaced instructions, emulated when a hook refuses a shape ------------------------------------------------------------------------------------------------------------------------------
// r[] is indexed like WinVita's enum Reg (the x86 numbering: eax ecx edx ebx esp ebp esi edi). The images are the dwords from the NEW esp upwards (image[i] lives at new_esp + 4*i); the shim writes them and sets esp.
inline void pusha_image(const uint32_t r[8], uint32_t espOld, uint32_t img[8]) { img[0] = r[7]; img[1] = r[6]; img[2] = r[5]; img[3] = espOld; img[4] = r[3]; img[5] = r[2]; img[6] = r[1]; img[7] = r[0]; }   // pusha: eax ecx edx ebx esp ebp esi edi
inline void push5_image(const uint32_t r[8], uint32_t img[5]) { img[0] = r[6]; img[1] = r[2]; img[2] = r[1]; img[3] = r[3]; img[4] = r[0]; }                                                         // push eax; push ebx; push ecx; push edx; push esi

// ---- why a shape was handed back to the original instructions (G151: counted in the session summary; the kernels decide exactly as in G150) -------------------------------------------------------------
enum Why : uint32_t { kWhyNone = 0, kWhyRead = 1, kWhyShape = 2, kWhyExtent = 3, kWhyOverlap = 4, kWhyArena = 5, kWhyTable = 6, kWhyWrite = 7, kWhyCount = 8 };
inline const char* why_name(uint32_t w) { static const char* const n[kWhyCount] = {"none", "unreadable_guest_word", "shape(size/pitch/count)", "address_wrap", "source_destination_overlap", "outside_arena", "overlaps_table", "write_failed"}; return w < kWhyCount ? n[w] : "?"; }

// ---- guest memory as the kernels see it -------------------------------------------------------------------------------------------------------------------------------------------------------
// M needs:  bool r32(uint32_t va, uint32_t& v)  (false = not readable)   const uint8_t* rspan(uint32_t va, uint64_t n)  /  uint8_t* wspan(uint32_t va, uint64_t n)  (nullptr = outside the arena)
//           bool w32(uint32_t va, uint32_t v)
inline bool disjoint(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a1 <= b0 || b1 <= a0; }   // half-open ranges

// ---- 1. DrawSurface, the palette-expand loop -----------------------------------------------------------------------------------------------------------------------------------------------------
// Frame of the function at the hook site (ebp = the function's frame): [ebp+8] source pointer, [ebp+0xc] source row padding, [ebp+0x10] 160-pixel blocks per row, [ebp+0x14] destination pointer, [ebp+0x18] destination row padding
// (bytes), [ebp+0x1c] rows. The original loops do-while (a zero count would run 2^32 times), so everything that is not strictly positive is refused. Effects: pixels, and [ebp+0x1c] = 0.
// Returns true = done (the caller resumes at kPalResume), false = nothing touched (the caller runs the original instructions).
template <class M, class A = RealAddrs>
inline bool palette_blit(M& m, uint32_t ebp, uint32_t* why = nullptr) {   // (G151: why = the cause of a refusal, for the session summary; nothing else changed)
    uint32_t src, srcPad, blocks, dst, dstPad, rows;
    if (!m.r32(ebp + 0x08u, src) || !m.r32(ebp + 0x0Cu, srcPad) || !m.r32(ebp + 0x10u, blocks) || !m.r32(ebp + 0x14u, dst) || !m.r32(ebp + 0x18u, dstPad) || !m.r32(ebp + 0x1Cu, rows)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return false; }
    if (blocks == 0u || blocks > 64u || rows == 0u || rows > 4096u || srcPad > 0x00FFFFFFu || dstPad > 0x00FFFFFFu) { G150_TRACE(__LINE__); if (why) *why = kWhyShape; return false; }   // (negative paddings and absurd sizes stay with the original)
    const uint64_t w = (uint64_t)blocks * 160u;                                       // pixels per row
    const uint64_t srcStride = w + srcPad, dstStride = w * 2u + dstPad;
    const uint64_t srcExt = (uint64_t)(rows - 1u) * srcStride + w, dstExt = (uint64_t)(rows - 1u) * dstStride + w * 2u;
    if ((uint64_t)src + srcExt > 0xFFFFFFFFull || (uint64_t)dst + dstExt > 0xFFFFFFFFull) { G150_TRACE(__LINE__); if (why) *why = kWhyExtent; return false; }
    if (!disjoint(src, (uint64_t)src + srcExt, dst, (uint64_t)dst + dstExt)) { G150_TRACE(__LINE__); if (why) *why = kWhyOverlap; return false; }
    const uint8_t* s = m.rspan(src, srcExt); uint8_t* d = m.wspan(dst, dstExt); const uint8_t* palb = m.rspan(A::kPalA, 512u);
    if (!s || !d || !palb) { G150_TRACE(__LINE__); if (why) *why = kWhyArena; return false; }
    if (!disjoint(src, (uint64_t)src + srcExt, A::kPalA, A::kPalA + 512u) || !disjoint(dst, (uint64_t)dst + dstExt, A::kPalA, A::kPalA + 512u)) { G150_TRACE(__LINE__); if (why) *why = kWhyTable; return false; }
    uint16_t pal[256]; std::memcpy(pal, palb, sizeof pal);
    for (uint32_t r = 0; r < rows; ++r) {
        const uint8_t* sr = s + (uint64_t)r * srcStride; uint8_t* dr = d + (uint64_t)r * dstStride;
        uint32_t x = 0;
        while (x + 8u <= w) {   // eight transparent pixels cost one 64-bit compare (the canvas of the race is mostly index 0)
            uint64_t q; std::memcpy(&q, sr + x, 8);
            if (q) { for (uint32_t k = 0; k < 8u; ++k) { const uint8_t idx = sr[x + k]; if (idx) std::memcpy(dr + 2u * (x + k), &pal[idx], 2); } }
            x += 8u;
        }
        for (; x < w; ++x) { const uint8_t idx = sr[x]; if (idx) std::memcpy(dr + 2u * x, &pal[idx], 2); }   // (w is a multiple of 160: never runs; kept for any future change of the shape)
    }
    if (!m.w32(ebp + 0x1Cu, 0u)) { if (why) *why = kWhyWrite; return false; }   // (G151: the cause; the memory is already written, as in G150)
    return true;
}

// ---- 2. the sprite blitter, LFB path ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// S = ESI at the function's entry. Returns: 1 = done (the caller returns to the call site, every register as the function leaves it: pusha/popa), 0 = not handled here, nothing touched, the original code runs from kSpriteGl,
// 2 = not handled here, nothing touched, the original code runs from kSpriteOther ([S+0xc] != 0), 3 = nothing touched, the original code runs from kSpriteBpp8 (surface mode flag != 1).
inline bool sub_le(int32_t a, int32_t b) { return (int64_t)a - (int64_t)b <= 0; }   // `sub; jle` after a 32-bit subtraction: SF != OF or ZF  <=>  the true difference is <= 0
inline int32_t wrap_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
inline int32_t wrap_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
template <class M, class A = RealAddrs>
inline int sprite_blit(M& m, uint32_t S, uint32_t* why = nullptr) {   // (G151: why = the cause of a refusal)
    uint32_t w0 = 0, flag = 0;
    if (!m.r32(S + 0x0Cu, w0)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    if (w0 != 0u) return 2;
    if (!m.r32(A::kLfbMode, flag)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    if (flag != 1u) return 3;
    uint32_t R, P;
    if (!m.r32(S + 0x08u, R) || !m.r32(S + 0x04u, P)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    uint32_t r0, r1, r2, r3, r4, r5, r6, px, py, top, bottom, left, right, pitch, lfb;
    if (!m.r32(R, r0) || !m.r32(R + 4u, r1) || !m.r32(R + 8u, r2) || !m.r32(R + 0x0Cu, r3) || !m.r32(R + 0x10u, r4) || !m.r32(R + 0x14u, r5) || !m.r32(R + 0x18u, r6)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    const uint8_t* b9p = m.rspan(R + 9u, 1u); if (!b9p) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    if (!m.r32(P, px) || !m.r32(P + 4u, py)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    if (!m.r32(A::kClipTop, top) || !m.r32(A::kClipBottom, bottom) || !m.r32(A::kClipLeft, left) || !m.r32(A::kClipRight, right) || !m.r32(A::kLfbPitch, pitch) || !m.r32(A::kLfbPtr, lfb)) { G150_TRACE(__LINE__); if (why) *why = kWhyRead; return 0; }
    // 0x45B864..: the three scratch values and the destination corner
    uint32_t srcPtr = ((r3 & 0xFFFFFF00u) | *b9p) + r6;                                     // mov ebx,[eax+0xc]; mov bl,[eax+9]; add ebx,[eax+0x18]
    int32_t cols = wrap_sub((int32_t)r4 >> 8, (int32_t)r2 >> 8);                            // (R[4] >> 8) - (R[2] >> 8)
    int32_t rows = wrap_sub((int32_t)r5 >> 8, (int32_t)r3 >> 8);                            // (R[5] >> 8) - (R[3] >> 8)
    int32_t ebx = wrap_sub((int32_t)px, (int32_t)r0) >> 8, ecx = wrap_sub((int32_t)py, (int32_t)r1) >> 8;   // destX, destY (arithmetic shifts)
    auto leave = [&](int code) -> int {   // an exit through `jle 0x45b9a2`: the scratch globals hold what the sequence had stored so far
        m.w32(A::kSprSrc, srcPtr); m.w32(A::kSprCols, (uint32_t)cols); m.w32(A::kSprRows, (uint32_t)rows); return code; };
    if (ecx < (int32_t)top) {                                                               // cmp ecx,[0x623058]; jge
        ecx = wrap_add(wrap_sub(0, ecx), (int32_t)top);                                     // neg ecx; add ecx,[0x623058]
        const int32_t nr = wrap_sub(rows, ecx); const bool le = sub_le(rows, ecx); rows = nr;   // sub [0x48f010],ecx; jle exit
        if (le) return leave(1);
        srcPtr += (uint32_t)ecx << 8;                                                        // shl ecx,8; add [0x48f008],ecx
        ecx = (int32_t)top;
    }
    { int32_t edx = wrap_add(ecx, rows); const bool le = sub_le(edx, (int32_t)bottom);      // mov edx,ecx; add edx,[rows]; sub edx,[0x623054]; jle skip
      edx = wrap_sub(edx, (int32_t)bottom);
      if (!le) { const int32_t nr = wrap_sub(rows, edx); const bool le2 = sub_le(rows, edx); rows = nr; if (le2) return leave(1); } }   // sub [rows],edx; jle exit
    if (ebx < (int32_t)left) {                                                              // cmp ebx,[0x623060]; jge
        ebx = wrap_add(wrap_sub(0, ebx), (int32_t)left);                                    // neg ebx; add ebx,[0x623060]
        const int32_t nc = wrap_sub(cols, ebx); const bool le = sub_le(cols, ebx); cols = nc;   // sub [0x48f00c],ebx; jle exit
        if (le) return leave(1);
        srcPtr += (uint32_t)ebx;                                                             // add [0x48f008],ebx
        ebx = (int32_t)left;
    }
    { int32_t edx = wrap_add(ebx, cols); const bool le = sub_le(edx, (int32_t)right);       // mov edx,ebx; add edx,[cols]; sub edx,[0x62305c]; jle skip
      edx = wrap_sub(edx, (int32_t)right);
      if (!le) { const int32_t nc = wrap_sub(cols, edx); const bool le2 = sub_le(cols, edx); cols = nc; if (le2) return leave(1); } }   // sub [cols],edx; jle exit
    // from here the sequence draws: rows > 0 and cols > 0 hold (every path that made them <= 0 left above)
    const uint32_t dst = (uint32_t)wrap_add(wrap_add((int32_t)((uint32_t)ebx << 1), (int32_t)((uint32_t)ecx * pitch)), (int32_t)lfb);   // imul ecx,[pitch]; shl ebx,1; add ebx,ecx; add ebx,[0x623074]
    const int32_t sp = (int32_t)pitch;
    if (rows <= 0 || cols <= 0 || sp <= 0 || sp > (1 << 20) || (int64_t)cols * 2 > sp) { G150_TRACE(__LINE__); if (why) *why = kWhyShape; return 0; }   // (a pitch smaller than a row of pixels would overlap rows: the original keeps that case)
    const uint64_t srcExt = (uint64_t)(rows - 1) * 256u + (uint32_t)cols, dstExt = (uint64_t)(rows - 1) * (uint32_t)sp + (uint64_t)(uint32_t)cols * 2u;
    if ((uint64_t)srcPtr + srcExt > 0xFFFFFFFFull || (uint64_t)dst + dstExt > 0xFFFFFFFFull) { G150_TRACE(__LINE__); if (why) *why = kWhyExtent; return 0; }
    if (!disjoint(srcPtr, (uint64_t)srcPtr + srcExt, dst, (uint64_t)dst + dstExt)) { G150_TRACE(__LINE__); if (why) *why = kWhyOverlap; return 0; }
    const uint8_t* s = m.rspan(srcPtr, srcExt); uint8_t* d = m.wspan(dst, dstExt); const uint8_t* pa = m.rspan(A::kPalA, 512u); const uint8_t* pb = m.rspan(A::kPalB, 32u);
    if (!s || !d || !pa || !pb) { G150_TRACE(__LINE__); if (why) *why = kWhyArena; return 0; }
    if (!disjoint(dst, (uint64_t)dst + dstExt, A::kPalA, A::kPalA + 512u) || !disjoint(dst, (uint64_t)dst + dstExt, A::kPalB, A::kPalB + 32u) || !disjoint(dst, (uint64_t)dst + dstExt, A::kSprSrc, A::kSprRows + 4u)) { G150_TRACE(__LINE__); if (why) *why = kWhyTable; return 0; }
    uint16_t lut[256]; std::memcpy(lut, pa, sizeof lut); std::memcpy(lut + 6, pb + 12, 20);   // one table: 16.. -> palette A, 6..15 -> table B (indices below 6 are transparent and never looked up)
    for (int32_t r = 0; r < rows; ++r) {                                                    // the pixels of a row are independent (disjoint source and destination ranges), so the order of the original (last column first) does not matter
        const uint8_t* sr = s + (uint64_t)r * 256u; uint8_t* dr = d + (uint64_t)r * (uint32_t)sp;
        int32_t c = 0;
        for (; c + 8 <= cols; c += 8) {                                                      // eight indices at once: no byte >= 6 means eight transparent pixels (bit 7 of (b & 0x7f) + 0x7a is set exactly when (b & 0x7f) >= 6)
            uint64_t q; std::memcpy(&q, sr + c, 8);
            if (!((((q & 0x7F7F7F7F7F7F7F7Full) + 0x7A7A7A7A7A7A7A7Aull) | q) & 0x8080808080808080ull)) continue;
            for (int32_t k = 0; k < 8; ++k) { const uint8_t idx = sr[c + k]; if (idx >= 6u) std::memcpy(dr + 2 * (c + k), &lut[idx], 2); }
        }
        for (; c < cols; ++c) { const uint8_t idx = sr[c]; if (idx >= 6u) std::memcpy(dr + 2 * c, &lut[idx], 2); }
    }
    rows = 0;                                                                               // dec [0x48f010] until zero
    m.w32(A::kSprSrc, srcPtr); m.w32(A::kSprCols, (uint32_t)cols); m.w32(A::kSprRows, 0u);
    return 1;
}

} }   // namespace ign::g150
