// win32/g135_pause_metrics.h -- the pure measurement code of the G135 pause-menu probe (no GL, no guest access), so that the host test
// (tools/g135_test.cpp) can run exactly the code the console runs on synthetic crisp / blurred menus.
//
// R is a rectangle of the LFB shadow (RGB565, top-left origin, `stride` bytes per row). A pixel is "visible" (overlay content) unless it equals the
// overlay key under the mask -- the compositor's own rule. The GL half looks at a tightly packed RGB8 read-back of R's physical rectangle (bottom-left
// origin, like glReadPixels) and, for every physical pixel whose centre maps back to a visible LFB pixel, compares the colour with that LFB pixel.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace ign { namespace g135 {

inline void expand565(uint16_t v, int& r, int& g, int& b) {
    r = (v >> 11) & 31; r = (r << 3) | (r >> 2);
    g = (v >> 5) & 63;  g = (g << 2) | (g >> 4);
    b = v & 31;         b = (b << 3) | (b >> 2);
}
inline int luma(int r, int g, int b) { return (r * 30 + g * 59 + b * 11) / 100; }

struct Colors {
    uint32_t seen[64]; uint32_t n = 0;
    void add(uint32_t c) { for (uint32_t i = 0; i < n; ++i) { if (seen[i] == c) { return; } } if (n < 64) { seen[n++] = c; } }
};

inline bool is_key(uint16_t v, uint16_t key, uint16_t mask) { return (uint16_t)(v & mask) == (uint16_t)(key & mask); }

// number of visible pixels in R
inline uint32_t count_visible(const uint8_t* sh, uint32_t stride, int x0, int y0, int x1, int y1, uint16_t key, uint16_t mask) {
    uint32_t n = 0;
    for (int y = y0; y < y1; ++y) {
        const uint16_t* row = (const uint16_t*)(sh + (size_t)y * stride);
        for (int x = x0; x < x1; ++x) { if (!is_key(row[x], key, mask)) { ++n; } }
    }
    return n;
}

struct LfbStats { uint32_t visible = 0, collisions = 0, distinct = 0, soft = 0, hard = 0; };
// collisions = pixels that differ from the key but are equal to it under the mask (they would be treated as transparent)
// soft/hard  = horizontal neighbour pairs of different colour, luma step <= 24 / > 24
inline LfbStats lfb_stats(const uint8_t* sh, uint32_t stride, int x0, int y0, int x1, int y1, uint16_t key, uint16_t mask) {
    LfbStats s; Colors cs;
    for (int y = y0; y < y1; ++y) {
        const uint16_t* row = (const uint16_t*)(sh + (size_t)y * stride);
        bool havePrev = false; uint16_t pv = 0;
        for (int x = x0; x < x1; ++x) {
            const uint16_t v = row[x];
            if (is_key(v, key, mask)) { if (v != key) { ++s.collisions; } havePrev = false; continue; }
            ++s.visible; cs.add(v);
            if (havePrev && v != pv) {
                int r1, g1, b1, r2, g2, b2; expand565(v, r1, g1, b1); expand565(pv, r2, g2, b2);
                const int d = luma(r1, g1, b1) - luma(r2, g2, b2);
                if ((d < 0 ? -d : d) <= 24) { ++s.soft; } else { ++s.hard; }
            }
            pv = v; havePrev = true;
        }
    }
    s.distinct = cs.n;
    return s;
}

struct GlStats { uint32_t mapped = 0, exact = 0, near_ = 0, far_ = 0, distinct = 0, soft = 0, hard = 0; };
// rgb: gw x gh tightly packed RGB8 whose row 0 is the BOTTOM row, first pixel = physical (gx0, gy0). vp = GL viewport {x,y,w,h}; Wl x Hl = logical size.
// exact: every channel within 4 of the expanded LFB colour; near: within 24; far: the rest.
inline GlStats gl_stats(const uint8_t* rgb, int gx0, int gy0, int gw, int gh, const int vp[4], int Wl, int Hl,
                        const uint8_t* sh, uint32_t stride, int x0, int y0, int x1, int y1, uint16_t key, uint16_t mask) {
    GlStats s; Colors cs;
    const float sx = (Wl > 0) ? (float)vp[2] / (float)Wl : 1.0f, sy = (Hl > 0) ? (float)vp[3] / (float)Hl : 1.0f;
    for (int gy = 0; gy < gh; ++gy) {
        bool havePrev = false; int pl = 0;
        for (int gx = 0; gx < gw; ++gx) {
            const float lx = ((float)(gx0 + gx) + 0.5f - (float)vp[0]) / sx;
            const float ly = ((float)vp[3] - ((float)(gy0 + gy) + 0.5f - (float)vp[1])) / sy;
            const int ix = (int)std::floor(lx), iy = (int)std::floor(ly);
            if (ix < x0 || ix >= x1 || iy < y0 || iy >= y1) { havePrev = false; continue; }
            const uint16_t v = ((const uint16_t*)(sh + (size_t)iy * stride))[ix];
            if (is_key(v, key, mask)) { havePrev = false; continue; }
            const uint8_t* px = rgb + ((size_t)gy * gw + gx) * 3;
            int er, eg, eb; expand565(v, er, eg, eb);
            const int dr = std::abs((int)px[0] - er), dg = std::abs((int)px[1] - eg), db = std::abs((int)px[2] - eb);
            const int dmax = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            ++s.mapped; if (dmax <= 4) { ++s.exact; } else if (dmax <= 24) { ++s.near_; } else { ++s.far_; }
            cs.add(((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2]);
            const int l = luma(px[0], px[1], px[2]);
            if (havePrev && l != pl) { const int d = l - pl; if ((d < 0 ? -d : d) <= 24) { ++s.soft; } else { ++s.hard; } }
            pl = l; havePrev = true;
        }
    }
    s.distinct = cs.n;
    return s;
}

// G136: the same comparison against a COPY of the rectangle taken earlier (saved: (x1-x0) x (y1-y0) RGB565 values, row-major), used after a pause to see
// whether the menu was left behind in the frame buffer (exact matches against the old menu colours should then be a small minority, unlike the ~100 % of
// the frame that showed the menu).
inline GlStats gl_stats_saved(const uint8_t* rgb, int gx0, int gy0, int gw, int gh, const int vp[4], int Wl, int Hl,
                              const uint16_t* saved, int x0, int y0, int x1, int y1, uint16_t key, uint16_t mask) {
    GlStats s; Colors cs;
    const int rw = x1 - x0;
    const float sx = (Wl > 0) ? (float)vp[2] / (float)Wl : 1.0f, sy = (Hl > 0) ? (float)vp[3] / (float)Hl : 1.0f;
    for (int gy = 0; gy < gh; ++gy) {
        for (int gx = 0; gx < gw; ++gx) {
            const float lx = ((float)(gx0 + gx) + 0.5f - (float)vp[0]) / sx;
            const float ly = ((float)vp[3] - ((float)(gy0 + gy) + 0.5f - (float)vp[1])) / sy;
            const int ix = (int)std::floor(lx), iy = (int)std::floor(ly);
            if (ix < x0 || ix >= x1 || iy < y0 || iy >= y1) { continue; }
            const uint16_t v = saved[(size_t)(iy - y0) * rw + (ix - x0)];
            if (is_key(v, key, mask)) { continue; }
            const uint8_t* px = rgb + ((size_t)gy * gw + gx) * 3;
            int er, eg, eb; expand565(v, er, eg, eb);
            const int dr = std::abs((int)px[0] - er), dg = std::abs((int)px[1] - eg), db = std::abs((int)px[2] - eb);
            const int dmax = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            ++s.mapped; if (dmax <= 4) { ++s.exact; } else if (dmax <= 24) { ++s.near_; } else { ++s.far_; }
            cs.add(((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2]);
        }
    }
    s.distinct = cs.n;
    return s;
}

}} // namespace ign::g135
