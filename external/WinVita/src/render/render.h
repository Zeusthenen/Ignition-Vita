// The seam between a guest graphics API translation (Glide, Direct3D, ...)
// and the host GPU.
//
// Deliberately partial: this layer doesn't try to cover 3D in general, only
// what real consumers submit — one translates a Glide command ring to the
// console GPU (paletted 8-bit texture atlas, palette applied in the shader),
// another translates a Direct3D execute buffer to an OpenGL layer (ARGB1555
// textures, a depth buffer). The vocabulary below is the real intersection
// of both, widened where one had strictly more (depth, palette) rather than
// forced down to the poorer one. What's missing is listed at the bottom.
//
// Guest API decoding itself (parsing a Glide ring, a Direct3D execute
// buffer, figuring out which states a game actually emits) is port-specific
// and stays there. This file starts after that decoding: it deals in
// vertices, batches, and textures, never opcodes.
//
// The console GPU, its OpenGL layer, and their memory constraints belong to
// the engine, which targets this console — so a concrete backend lives here,
// not in the port.
#pragma once
#include <cstdint>

namespace wx86 {
namespace render {

// ---- vertex -----------------------------------------------------------------
// Position in screen space. Ports do their own transform in software, so
// there is no matrix to set here.
//
// `w` carries perspective correction: a 2D port sets w=1 and z=0; a 3D port
// supplies a position premultiplied by w. Passing only three components
// would give affine texturing and warp surfaces — hence the field exists
// even for a port that doesn't use it.
//
// `layer` selects a palette or a texture array layer. 0 when the texture
// already carries its own colors.
struct Vertex {
    float    x, y, z, w;
    uint32_t rgba;        // R,G,B,A bytes in memory order
    float    u, v;
    float    layer;
};

// ---- etats de dessin ------------------------------------------------------
enum class Blend : uint8_t {
    Opaque = 0,
    Alpha,        // src.a / 1-src.a
    Additive,
    Multiply,
};

enum class Filter : uint8_t { Nearest = 0, Bilinear };

// Batch flags. Bit values are kept stable on purpose: they travel through
// traces and A/B comparison logs.
enum KeyFlags : uint8_t {
    KF_ColorKey  = 1u << 0,   // the key texel (black, or index 0) is transparent
    KF_Modulate  = 1u << 1,   // color = texture x vertex color
    KF_ConstAlpha= 1u << 2,   // alpha taken from `constColor`
    KF_AlphaTest = 1u << 3,   // threshold rejection, see alphaFunc/alphaRef
    KF_ZWrite    = 1u << 4,   // writes to the depth buffer
};

// Two draws with the same key can be batched together; two draws with
// different keys cannot. This is the only property both backends require.
struct DrawKey {
    uint32_t texture;      // 0 = flat fill, no texture
    uint32_t constColor;   // 0xAARRGGBB
    Blend    blend;
    Filter   filter;
    uint8_t  flags;        // KeyFlags
    uint8_t  alphaFunc;    // 0 = backend default; otherwise the port's comparison op
    uint8_t  alphaRef;
    uint8_t  pad[3];
};

// ---- textures ----------------------------------------------------------------
enum class TexFormat : uint8_t {
    Idx8,       // 8-bit indexed; palette is set via palette_set()
    Argb1555,
    Rgba8888,
};

// ---- the interface every render target implements ---------------------------
// A concrete backend (console GPU, OpenGL layer, counting-only) fills in
// this struct. This is all the port ever sees.
//
// No method reports failure except `init`: a backend that fails to
// initialize must return false, and the caller then falls back to the
// counting backend — never to a black screen.
struct Backend {
    // Opens the context for a `w` x `h` target. false = unavailable.
    bool (*init)(int w, int h);
    void (*shutdown)();

    // Creates or resizes a texture. `id` is chosen by the caller and serves
    // as its handle afterward. Returns false if creation fails.
    bool (*texture_create)(uint32_t id, int w, int h, TexFormat fmt);
    // Uploads a rectangle. `src` is a host pointer; `srcPitch` is in bytes.
    // The upload may be lazy on the backend side.
    void (*texture_upload)(uint32_t id, int x, int y, int w, int h,
                           const void* src, int srcPitch);
    // Palette for Idx8 textures. `slot` matches the vertex's `layer`.
    // `argb256` = 256 words of 0xAARRGGBB.
    void (*palette_set)(int slot, const uint32_t* argb256);

    // A batch. `idx` indexes into `verts`. The backend may not retain either
    // pointer beyond the call.
    void (*draw)(const DrawKey& key,
                 const Vertex* verts, uint32_t vertCount,
                 const uint16_t* idx, uint32_t idxCount);

    void (*clear_color)(uint32_t argb);
    void (*clear_depth)();
    // Presents the frame. `frame` is the caller's frame number: it's what
    // dates resources, and so what the barriers below use as a reference.
    void (*present)(uint64_t frame);

    // ---- pipelined submission (optional) ----------------------------------
    // First frame still in flight (submitted, not yet completed by the GPU).
    // Any resource used by a frame >= this number must not be overwritten —
    // doing so causes intermittent rendering corruption. Returns UINT64_MAX
    // when nothing is in flight, which is what a synchronous backend does.
    uint64_t (*in_flight_from)();
    // Waits until everything is presented. A bounded wait beats a resource
    // getting overwritten under the GPU.
    void (*drain)();

    // Counter line for a periodic log. Returns the number of bytes written
    // to `out`.
    int (*counters)(char* out, unsigned n);
};

// The counting backend: compiles everywhere, draws nothing, counts
// everything. Used on desktop and under qemu, and as the fallback for any
// backend whose init fails.
const Backend& null_backend();

// Counters from the counting backend, readable by tests.
struct NullStats {
    uint64_t frames, draws, verts, indices;
    uint64_t texCreates, texUploadBytes, paletteSets;
    uint64_t clearColors, clearDepths;
};
const NullStats& null_stats();
void null_stats_reset();

// ---- the cost of this shared vertex format -----------------------------------
// The vocabulary above is the union of what a 2D and a 3D port need, and a
// union has a cost: each port carries the other's fields down the per-frame
// path. Vertex size, field by field:
//
//   2D port (Glide-style ring)   x,y,u,v,argb,pal                24 bytes
//   3D port (Direct3D-style)     x,y,z,w,rgba,u,v                28 bytes
//   this file (the union)        x,y,z,w,rgba,u,v,layer          32 bytes
//
// A 2D port pays +33% vertex bandwidth for z and w it never uses; a 3D port
// pays +14% for `layer`. At high per-frame vertex counts, on a console where
// memory bandwidth is the scarce resource, that shows up in both CPU copy
// and GPU read traffic.
//
// Practical consequence: an existing concrete backend cannot be wired in
// here without reworking its GPU vertex format. A backend that declares its
// attributes tightly packed on its own smaller vertex needs either a
// per-vertex conversion (replacing a plain memcpy) or a wider GPU vertex
// declaration and shader — both cost something every frame.
//
// The zero-cost path is for the port's own batch builder to emit this
// file's vertex format directly instead of converting into it — that
// removes the conversion entirely, leaving only a wider vertex.
//
// ---- explicitly not covered ---------------------------------------------------
// Add these when a real port needs them, not before:
//   * matrices and hardware lighting — ports do their own software transform;
//   * trilinear filtering and mip levels — none currently emit them;
//   * caller-supplied shaders — backends intentionally have a closed set of
//     states;
//   * render-to-texture, stencil, multiple render targets;
//   * texture compression.
// These are absences, not prohibitions: the vocabulary above doesn't rule
// any of them out.

}  // namespace render
}  // namespace wx86
