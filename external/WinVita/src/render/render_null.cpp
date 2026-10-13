// The counting backend.
//
// Draws nothing, counts everything. Two uses:
//   * on desktop and under qemu, where there is no console GPU: the port's
//     batch builder still runs, and every one of its counters stays
//     measurable — only submission is missing;
//   * as the fallback for a backend whose initialization fails, so an
//     unavailable GPU produces an unaccelerated frame instead of a black
//     screen.
#include "render.h"

#include <cstdio>

namespace wx86 {
namespace render {

namespace {

NullStats g_stats{};

bool null_init(int, int) { return true; }
void null_shutdown() {}

bool null_texture_create(uint32_t, int, int, TexFormat) { ++g_stats.texCreates; return true; }

void null_texture_upload(uint32_t, int, int, int w, int h, const void*, int srcPitch) {
    // Counts bytes actually passed, not the rectangle's area: a row pitch
    // wider than the rectangle is common, and conflating the two would skew
    // any bandwidth comparison.
    if (w > 0 && h > 0 && srcPitch > 0)
        g_stats.texUploadBytes += (uint64_t)h * (uint64_t)srcPitch;
}

void null_palette_set(int, const uint32_t*) { ++g_stats.paletteSets; }

void null_draw(const DrawKey&, const Vertex*, uint32_t vertCount,
               const uint16_t*, uint32_t idxCount) {
    ++g_stats.draws;
    g_stats.verts += vertCount;
    g_stats.indices += idxCount;
}

void null_clear_color(uint32_t) { ++g_stats.clearColors; }
void null_clear_depth() { ++g_stats.clearDepths; }
void null_present(uint64_t) { ++g_stats.frames; }

// Nothing is ever in flight: counting is synchronous by construction.
uint64_t null_in_flight_from() { return ~(uint64_t)0; }
void null_drain() {}

int null_counters(char* out, unsigned n) {
    if (!out || n == 0) return 0;
    const int r = std::snprintf(out, n,
        "render(null): images=%llu lots=%llu sommets=%llu indices=%llu "
        "tex=%llu octets=%llu pal=%llu effC=%llu effZ=%llu",
        (unsigned long long)g_stats.frames,  (unsigned long long)g_stats.draws,
        (unsigned long long)g_stats.verts,   (unsigned long long)g_stats.indices,
        (unsigned long long)g_stats.texCreates, (unsigned long long)g_stats.texUploadBytes,
        (unsigned long long)g_stats.paletteSets,
        (unsigned long long)g_stats.clearColors, (unsigned long long)g_stats.clearDepths);
    if (r < 0) return 0;
    return (unsigned)r >= n ? (int)n - 1 : r;
}

const Backend kNull = {
    null_init, null_shutdown,
    null_texture_create, null_texture_upload, null_palette_set,
    null_draw,
    null_clear_color, null_clear_depth, null_present,
    null_in_flight_from, null_drain,
    null_counters,
};

}  // namespace

const Backend& null_backend() { return kNull; }
const NullStats& null_stats() { return g_stats; }
void null_stats_reset() { g_stats = NullStats{}; }

}  // namespace render
}  // namespace wx86
