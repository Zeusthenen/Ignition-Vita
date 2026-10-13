// Scales a guest image into the host's display buffer.
//
// Shared because multiple engine consumers need the same operation —
// 8-bit palette expansion, 16.16 fixed-point scaling with row duplication,
// and buffer swap — and the stretch-to-fill case is a strict degenerate case
// of the aspect-fit case (destination = full screen).
//
// Scope: converts and scales only. It draws no overlay (frame counter,
// on-screen keyboard) and does not present — those steps live in the caller
// because they depend on the target. It also does not fill the letterbox
// bars, since that only needs to happen when geometry changes, and only the
// caller knows when that is.
//
// Called once per frame (it processes the whole image, not per pixel), so
// its own call overhead is negligible. The internal loops use a 4:1 unroll
// and memcpy-based row duplication to keep that cost down.
#pragma once
#include <cstdint>

namespace wx86 {

// Source pixel format. New formats are added here, not in the port — this
// list must stay generic across every consumer of the engine.
enum class SrcFormat {
    Pal8,    // 8-bit indexed + B,G,R,0 palette (Windows DIB order)
    Bgra32,  // 32-bit 0x00RRGGBB in memory, i.e. B,G,R,X
    // 16-bit 5-5-5, bit 15 ignored: biBitCount=16 with biCompression=BI_RGB is
    // 555 by Win32 definition, not 565. Widening 5 bits to 8 uses
    // `x<<3 | x>>2` (replicates the top bits) rather than a plain `<<3`,
    // which would cap at 248 and wash out the image.
    Rgb555,
};

// Destination rectangle in the host buffer, in pixels.
// Full screen = {0, 0, screen_width, screen_height}: the degenerate case,
// byte-for-byte equivalent to a port that just stretches to fill.
struct DstRect { int x, y, w, h; };

// Converts `src` (srcW x srcH, row pitch `srcPitch` in bytes) to `dst` (row
// pitch `dstPitch` in 32-bit pixels), scaled to exactly fill `r`. Output is
// 0xAARRGGBB with alpha forced to 0xFF.
//
// `palette`: 256 B,G,R,0 quadruplets. Required for Pal8, ignored otherwise.
//
// Scaling is nearest-neighbor in 16.16 fixed point; duplicated rows are
// copied from the previously written row instead of resampling — cheaper,
// and identical by construction.
void scale_blit(uint32_t* dst, int dstPitch, const DstRect& r,
                const uint8_t* src, int srcW, int srcH, int srcPitch,
                SrcFormat fmt, const uint8_t* palette);

// Computes the destination rectangle that preserves the source aspect ratio
// and centers the image in a dstW x dstH screen (letterboxing on the sides or
// top/bottom). `stretch` = true just returns the full screen.
//
// Centralized here because a rounding mistake shifts the image by a pixel.
DstRect fit_rect(int srcW, int srcH, int dstW, int dstH, bool stretch);

}  // namespace wx86
