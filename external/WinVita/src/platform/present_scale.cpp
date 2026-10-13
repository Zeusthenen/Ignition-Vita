// See present_scale.h for the rationale.
//
// These loops are a direct transcription, not a rewrite: the 4:1 unroll, the
// memcpy-based row duplication, and the exact operation order all come from
// the existing presentation code. Any change here must be verified
// against a pixel-identity oracle rather than reasoned about — a single byte
// difference in this path is visible on screen.
#include "present_scale.h"

#include <cstring>

namespace wx86 {

namespace {
// Expands a Windows DIB palette (B,G,R,0) to opaque 0xAARRGGBB.
// Recomputed every frame: 256 iterations cost nothing measurable, and a
// persistent table would be one more piece of hidden state.
inline void expand_palette(const uint8_t* palette, uint32_t out[256]) {
    for (int i = 0; i < 256; ++i) {
        const uint8_t* p = palette + i * 4;        // B,G,R,0
        out[i] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
}
}  // namespace

void scale_blit(uint32_t* dst, int dstPitch, const DstRect& r,
                const uint8_t* src, int srcW, int srcH, int srcPitch,
                SrcFormat fmt, const uint8_t* palette) {
    if (!dst || !src || r.w <= 0 || r.h <= 0 || srcW <= 0 || srcH <= 0) return;
    if (fmt == SrcFormat::Pal8 && !palette) return;

    uint32_t pal32[256];
    if (fmt == SrcFormat::Pal8) expand_palette(palette, pal32);

    const uint32_t sx = (uint32_t)(((uint32_t)srcW << 16) / (uint32_t)r.w);
    const uint32_t sy = (uint32_t)(((uint32_t)srcH << 16) / (uint32_t)r.h);

    int prev_srcy = -1;
    for (int y = 0; y < r.h; ++y) {
        const int srcy = (int)(((uint32_t)y * sy) >> 16);
        uint32_t* out = dst + (size_t)(r.y + y) * dstPitch + r.x;
        // Repeated row: copy the row already written just above.
        if (srcy == prev_srcy) {
            std::memcpy(out, out - dstPitch, (size_t)r.w * 4);
            continue;
        }
        prev_srcy = srcy;
        const uint8_t* row = src + (size_t)srcy * srcPitch;
        uint32_t xacc = 0;
        if (fmt == SrcFormat::Pal8) {
            int x = 0;
            for (; x + 4 <= r.w; x += 4) {             // 4:1 unroll
                out[x + 0] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x + 1] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x + 2] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x + 3] = pal32[row[xacc >> 16]]; xacc += sx;
            }
            for (; x < r.w; ++x, xacc += sx) out[x] = pal32[row[xacc >> 16]];
        } else if (fmt == SrcFormat::Rgb555) {
            // The DIB is top-down (negative biHeight), so no vertical flip here.
            const uint16_t* r16 = (const uint16_t*)row;
            for (int x = 0; x < r.w; ++x, xacc += sx) {
                const uint32_t p = r16[xacc >> 16];
                const uint32_t rr = (p >> 10) & 31, gg = (p >> 5) & 31, bb = p & 31;
                out[x] = 0xFF000000u
                       | (((bb << 3) | (bb >> 2)) << 16)
                       | (((gg << 3) | (gg >> 2)) << 8)
                       |  ((rr << 3) | (rr >> 2));
            }
        } else {
            const uint32_t* r32 = (const uint32_t*)row;
            for (int x = 0; x < r.w; ++x, xacc += sx) {
                const uint32_t bgr = r32[xacc >> 16];   // 0x00RRGGBB, i.e. B,G,R,X in memory
                out[x] = 0xFF000000u | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
            }
        }
    }
}

DstRect fit_rect(int srcW, int srcH, int dstW, int dstH, bool stretch) {
    if (stretch || srcW <= 0 || srcH <= 0) return DstRect{0, 0, dstW, dstH};
    int w = (int)(((long long)srcW * dstH) / srcH);     // try full height first
    int h = dstH;
    if (w > dstW) {                                      // too wide: use full width instead
        w = dstW;
        h = (int)(((long long)srcH * dstW) / srcW);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return DstRect{(dstW - w) / 2, (dstH - h) / 2, w, h};
}

}  // namespace wx86
