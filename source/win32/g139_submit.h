// win32/g139_submit.h -- G139 geometry submission: a small authoritative GL state SHADOW and a draw SESSION that batches CONSECUTIVE triangles with identical state. Pure code (no GL, no guest access, no
// logging): the GL calls go through a `Gl` ops type, so tools/g139_test.cpp runs exactly the code the console runs, against a model of GL state.
//
// WHY: G138 still issued, for EVERY triangle, ~35 GL calls (7 state queries, matrix push/ortho/pop, client-array enable/pointer/disable, texture env, a draw call and a glGetError) and one glDrawArrays
// per triangle. Everything in that sequence except the vertex data is a function of state that cannot change between two consecutive triangles unless some OTHER GL call runs in between.
//
// THE CONTRACT (what makes batching exact):
//  * Every GL call of the module that is not the draw path itself goes through a wrapper (glide2x_ign.cpp, "iteration-G139") that first calls Session::end(): the batch is flushed (in order), the state the
//    draw path changed is restored to the baseline captured when the session began, and only then the other call executes. So no other code can ever observe or depend on a half-open session, and the order
//    of GL operations is exactly the order of the original calls.
//  * The shadow records what the wrappers set; a field it cannot know is INVALID and is synchronised with ONE query at the next session start (never per triangle).
//  * A triangle joins the open session only if its Key (every state that influences the result) is identical; a different key flushes first, then changes only what differs; no triangle is ever reordered.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g153_key.h"   // G153: the mark of the chroma-keyed draws in Key::ckValue (pure header)

namespace ign { namespace g139 {

// standard OpenGL values; glide2x_ign.cpp static_asserts every one of them against vitaGL.h
constexpr uint32_t kGlDepthTest = 0x0B71, kGlCullFace = 0x0B44, kGlTexture2D = 0x0DE1, kGlBlend = 0x0BE2;
constexpr uint32_t kGlModelview = 0x1700, kGlProjection = 0x1701, kGlReplace = 0x1E01;
constexpr uint32_t kGlVertexArray = 0x8074, kGlColorArray = 0x8076, kGlTexCoordArray = 0x8078;
constexpr uint32_t kGlTextureEnv = 0x2300, kGlTextureEnvMode = 0x2200;
constexpr uint32_t kGlModulate = 0x2100;   // G153: GL_MODULATE (static_assert in glide2x_ign.cpp)

// ---- the shadow --------------------------------------------------------------------------------------------------------------------------------------
enum Field : uint32_t { kFDepth = 1, kFCull = 2, kFTex2d = 4, kFBlend = 8, kFBound = 16, kFMatMode = 32, kFVp = 64, kFTexEnv = 128, kFAll = 255 };
constexpr uint32_t kSessionFields = kFDepth | kFCull | kFTex2d | kFBound | kFMatMode | kFVp;   // what a session needs at its start (texenv: lazily, on the first textured triangle)

struct GlBase {                       // the GL state the draw path reads and restores
    bool depth = false, cull = false, tex2d = false, blend = false;
    uint32_t bound = 0, matMode = 0, texEnv = 0;
    int32_t vp[4] = {0, 0, 0, 0};
};

class Shadow {
public:
    uint32_t valid() const { return valid_; }
    bool has(uint32_t mask) const { return (valid_ & mask) == mask; }
    void invalidate(uint32_t mask) { valid_ &= ~mask; }
    void invalidate_all() { valid_ = 0; }
    const GlBase& v() const { return v_; }
    void set_depth(bool b) { v_.depth = b; valid_ |= kFDepth; }
    void set_cull(bool b) { v_.cull = b; valid_ |= kFCull; }
    void set_tex2d(bool b) { v_.tex2d = b; valid_ |= kFTex2d; }
    void set_blend(bool b) { v_.blend = b; valid_ |= kFBlend; }
    void set_bound(uint32_t t) { v_.bound = t; valid_ |= kFBound; }
    void set_matmode(uint32_t m) { v_.matMode = m; valid_ |= kFMatMode; }
    void set_texenv(uint32_t m) { v_.texEnv = m; valid_ |= kFTexEnv; }
    void set_vp(int32_t x, int32_t y, int32_t w, int32_t h) { v_.vp[0] = x; v_.vp[1] = y; v_.vp[2] = w; v_.vp[3] = h; valid_ |= kFVp; }
    // ---- what the wrappers record AFTER the real call (shared by the console wrappers and the host test) ----
    void track_toggle(uint32_t cap, bool on) {
        if (cap == kGlDepthTest) set_depth(on); else if (cap == kGlCullFace) set_cull(on); else if (cap == kGlTexture2D) set_tex2d(on); else if (cap == kGlBlend) set_blend(on);
    }
    void track_bind(uint32_t target, uint32_t id) { if (target == kGlTexture2D) set_bound(id); }
    void track_delete_textures() { invalidate(kFBound); }
    void track_active_texture() { invalidate(kFBound | kFTex2d | kFTexEnv); }              // these are per texture unit
    void track_matrix_mode(uint32_t m) { set_matmode(m); }
    void track_viewport(int32_t x, int32_t y, int32_t w, int32_t h) { set_vp(x, y, w, h); }
    void track_texenv(uint32_t target, uint32_t pname, int32_t param) { if (target == kGlTextureEnv && pname == kGlTextureEnvMode) set_texenv((uint32_t)param); }
    void track_swap_or_init() { invalidate_all(); }
    // one query per INVALID field of `mask`, nothing else; returns the number of GL queries issued
    template <class Gl> int sync(uint32_t mask, Gl& gl) {
        int q = 0; const uint32_t need = mask & ~valid_;
        if (need & kFDepth) { set_depth(gl.is_enabled(kGlDepthTest)); ++q; }
        if (need & kFCull) { set_cull(gl.is_enabled(kGlCullFace)); ++q; }
        if (need & kFTex2d) { set_tex2d(gl.is_enabled(kGlTexture2D)); ++q; }
        if (need & kFBlend) { set_blend(gl.is_enabled(kGlBlend)); ++q; }
        if (need & kFBound) { set_bound(gl.get_bound_texture()); ++q; }
        if (need & kFMatMode) { set_matmode(gl.get_matrix_mode()); ++q; }
        if (need & kFVp) { int32_t p[4]; gl.get_viewport(p); set_vp(p[0], p[1], p[2], p[3]); ++q; }
        if (need & kFTexEnv) { set_texenv(gl.get_texenv_mode()); ++q; }
        return q;
    }
    // compares every VALID field with the real GL state (diagnostic; the caller has closed any session); returns a bit mask of the fields that differed, and resynchronises them
    template <class Gl> uint32_t verify(Gl& gl, int* queries = nullptr) {
        uint32_t bad = 0; int q = 0;
        if (valid_ & kFDepth)  { ++q; if (gl.is_enabled(kGlDepthTest) != v_.depth) { bad |= kFDepth; v_.depth = !v_.depth; } }
        if (valid_ & kFCull)   { ++q; if (gl.is_enabled(kGlCullFace) != v_.cull) { bad |= kFCull; v_.cull = !v_.cull; } }
        if (valid_ & kFTex2d)  { ++q; if (gl.is_enabled(kGlTexture2D) != v_.tex2d) { bad |= kFTex2d; v_.tex2d = !v_.tex2d; } }
        if (valid_ & kFBlend)  { ++q; if (gl.is_enabled(kGlBlend) != v_.blend) { bad |= kFBlend; v_.blend = !v_.blend; } }
        if (valid_ & kFBound)  { ++q; const uint32_t t = gl.get_bound_texture(); if (t != v_.bound) { bad |= kFBound; v_.bound = t; } }
        if (valid_ & kFMatMode){ ++q; const uint32_t m = gl.get_matrix_mode(); if (m != v_.matMode) { bad |= kFMatMode; v_.matMode = m; } }
        if (valid_ & kFVp)     { ++q; int32_t p[4]; gl.get_viewport(p); if (memcmp(p, v_.vp, sizeof p) != 0) { bad |= kFVp; memcpy(v_.vp, p, sizeof p); } }
        if (valid_ & kFTexEnv) { ++q; const uint32_t m = gl.get_texenv_mode(); if (m != v_.texEnv) { bad |= kFTexEnv; v_.texEnv = m; } }
        if (queries) *queries = q;
        return bad;
    }
private:
    GlBase v_{};
    uint32_t valid_ = 0;
};

// ---- the key: every state that decides what a triangle looks like ------------------------------------------------------------------------------------
// Fields that change only through a GL call (blend, scissor, program, depth/cull toggles of other code ...) cannot differ inside a session: that GL call is a boundary. The key therefore holds what the
// draw path itself applies (texture, depth, cull, viewport) plus the LOGICAL Glide states that never reach GL on their own (colour combine, alpha combine/source, chromakey, blend record).
struct Key {
    uint32_t tex = 0, texReady = 0, depth = 0, depthMask = 0, depthFunc = 0, cull = 0;
    int32_t vp[4] = {0, 0, 0, 0};
    uint32_t colorFnc = 0, alphaMode = 0, ckEnabled = 0, ckValue = 0, blendValid = 0, blend[4] = {0, 0, 0, 0}, useConstRgb = 0, useConstAlpha = 0;
};
inline bool same_key(const Key& a, const Key& b) { return memcmp(&a, &b, sizeof(Key)) == 0; }   // no padding: only 32-bit fields
inline bool same_gl_state(const Key& a, const Key& b) {                                         // what the GL state machine sees
    return a.tex == b.tex && a.texReady == b.texReady && a.depth == b.depth && a.depthMask == b.depthMask && a.depthFunc == b.depthFunc && a.cull == b.cull && memcmp(a.vp, b.vp, sizeof a.vp) == 0;
}
inline bool only_texture_differs(const Key& a, const Key& b) {                                  // same everything the GL state machine sees except the texture / textured flag
    return (a.tex != b.tex || a.texReady != b.texReady) && a.depth == b.depth && a.depthMask == b.depthMask && a.depthFunc == b.depthFunc && a.cull == b.cull && memcmp(a.vp, b.vp, sizeof a.vp) == 0;
}
static_assert(sizeof(Key) == 4 * 21, "Key must be 21 packed 32-bit fields (memcmp equality)");

// ---- the session -----------------------------------------------------------------------------------------------------------------------------------
constexpr int kBatchCap = 256;                       // triangles per draw call (fixed, static storage)
struct SubmitCounters {
    uint64_t triangles = 0, draws = 0, sessions = 0, maxBatch = 0, syncQueries = 0, glCalls = 0, texturedTriangles = 0;
    uint64_t flushTexture = 0, flushState = 0, flushCapacity = 0, flushBoundary = 0;
    uint64_t g154KeyedDraws = 0, g154KeyedTris = 0, g154AlphaOn = 0, g154AlphaOff = 0, g154EnvMod = 0, g154EnvRepl = 0, g154KeyedFlushes = 0;   // G154: what the chroma-keyed draws cost the session (counted per group, never per triangle)
    uint64_t g155OffForced = 0, g155OffHorizon = 0, g155OffEnd = 0, g155NeutralKept = 0, g155EnvEnd = 0, g155LogicalFlushes = 0, g155TransTris = 0, g155TransDepthWrite = 0, g155Binds = 0, g155EnvOther = 0;   // G155: why the alpha test went off, texture-environment restores, flushes no GL-visible or keyed field explains (per group, never per triangle)
};

// G153-BEGIN
// iteration-G153: the alpha test of the chroma-keyed draws (the keyed variant of a texture carries alpha 0 on the texels the Glide chroma key discards). The `Gl` ops types of the older tests have no alpha_test(): there the call is a no-op
// (their keys never carry the G153 mark).
template <class G> inline auto g153_alpha(G& g, bool on, int) -> decltype(g.alpha_test(on), void()) { g.alpha_test(on); }
template <class G> inline void g153_alpha(G&, bool, long) {}
constexpr uint32_t kG155NeutralDraws = 16;   // G155: how many non-keyed textured key changes the alpha test is left on after the last keyed draw (the fragment alpha of those is 1: no output difference; bounds the fill cost of the test)
// G153-END
template <class Gl>
class Session {
public:
    bool open() const { return open_; }
    int pending() const { return n_; }
    const SubmitCounters& counters() const { return c_; }
    SubmitCounters& counters_mut() { return c_; }
    // adds one triangle (positions x,y; colours r,g,b,a; texcoords u,v: three vertices each)
    void submit(const Key& k, const float* pos6, const float* col12, const float* uv6, Shadow& sh, Gl& gl) {
        if (!open_) { begin(k, sh, gl); }
        else if (!same_key(k, key_)) {
            { const bool g154a = ign::g153::is_keyed(k.ckEnabled, k.ckValue), g154b = ign::g153::is_keyed(key_.ckEnabled, key_.ckValue); if (g154a != g154b || (g154a && k.ckValue != key_.ckValue)) ++c_.g154KeyedFlushes; }   // G154: a flush only the keyed state caused (keyed <-> not keyed, opaque <-> translucent); a texture change between two keyed draws would also flush without the key
            if (same_gl_state(k, key_) && k.ckEnabled == key_.ckEnabled && k.ckValue == key_.ckValue) ++c_.g155LogicalFlushes;   // G155: a flush that no GL-visible field and no keyed field explains (a key field left by an older phase)
            flush(only_texture_differs(k, key_) ? kTexture : kState, gl);
            transition(k, sh, gl);
        }
        memcpy(&P_[(size_t)n_ * 6], pos6, 6 * sizeof(float));
        memcpy(&C_[(size_t)n_ * 12], col12, 12 * sizeof(float));
        if (g155Sub_) { float* g155c = &C_[(size_t)n_ * 12]; for (int g155i = 0; g155i < 12; ++g155i) g155c[g155i] = 1.0f; }   // G155: the texture environment is MODULATE (sticky): a draw that was GL_REPLACE gets white vertex colours (colour x 1 = REPLACE, alpha x 1 = the texel alpha)
        memcpy(&U_[(size_t)n_ * 6], uv6, 6 * sizeof(float));
        ++n_; ++c_.triangles; if (k.texReady) ++c_.texturedTriangles;
        if (n_ == kBatchCap) flush(kCapacity, gl);
    }
    // flush in order, restore the baseline, close. Safe to call when closed.
    void end(Gl& gl) {
        if (!open_) return;
        flush(kBoundary, gl);
        if (arrTex_) { gl.disable_client(kGlTexCoordArray); arrTex_ = false; }
        gl.disable_client(kGlColorArray);
        gl.disable_client(kGlVertexArray);
        // G153-BEGIN
        if (g153Mod_ && g155Classic_) { gl.texenv_mode(kGlReplace); g153Mod_ = false; ++c_.g154EnvRepl; }   // G155: classic (G154) behaviour, kept for an A/B on the console
        if (g153Mod_) { if (base_.texEnv != kGlModulate) { gl.texenv_mode(base_.texEnv); ++c_.g155EnvEnd; } appliedEnv_ = base_.texEnv; g153Mod_ = false; }   // G155: the sticky MODULATE goes straight back to the baseline (no call when the baseline IS MODULATE); G153: // appliedEnv_ still says REPLACE while the keyed translucent draws ran under MODULATE
        if (g153Alpha_) { g153_alpha(gl, false, 0); g153Alpha_ = false; ++c_.g154AlphaOff; ++c_.g155OffEnd; }   // G155: the counters         // the alpha test never outlives the session
        // G153-END
        if (appliedEnv_ != base_.texEnv && envKnown_) ++c_.g155EnvOther;   // G155
        if (appliedEnv_ != base_.texEnv && envKnown_) { gl.texenv_mode(base_.texEnv); }
        gl.matrix_mode(kGlProjection); gl.pop_matrix();
        gl.matrix_mode(kGlModelview); gl.pop_matrix();
        if (base_.matMode) gl.matrix_mode(base_.matMode);
        if (a_.depth != base_.depth) { if (base_.depth) gl.enable(kGlDepthTest); else gl.disable(kGlDepthTest); }
        if (a_.cull != base_.cull) { if (base_.cull) gl.enable(kGlCullFace); else gl.disable(kGlCullFace); }
        if (a_.tex2d != base_.tex2d) { if (base_.tex2d) gl.enable(kGlTexture2D); else gl.disable(kGlTexture2D); }
        if (a_.bound != base_.bound) ++c_.g155Binds;   // G155
        if (a_.bound != base_.bound) { gl.bind_texture(base_.bound); }
        open_ = false;
    }
    // iteration-G148: draws the triangles that are pending, in order, at the GL state they were submitted under, and keeps the session OPEN (its arrays, matrices, texture and key stay valid). Used before a GL state that is not part
    // of the session (blend, scissor) changes, instead of the full end() + begin() (restore the baseline and set it all up again). A capacity flush does exactly this; it is counted as a state flush.
    void flush_pending(Gl& gl) { if (open_) flush(kState, gl); }
    void set_classic(bool on) { g155Classic_ = on; }   // G155: true = the alpha test and the texture environment are switched draw by draw as in G154 (the A/B token g155=classic); must be set while no session is open
    bool classic() const { return g155Classic_; }   // G155
    const GlBase& baseline() const { return base_; }
    const int32_t* base_viewport() const { return base_.vp; }
private:
    enum Reason { kTexture, kState, kCapacity, kBoundary };
    void flush(Reason r, Gl& gl) {
        if (n_ == 0) return;
        gl.draw_triangles(n_);
        if (g155Key_) { ++c_.g154KeyedDraws; c_.g154KeyedTris += (uint64_t)n_; }   // G154 (G155: the draw is keyed; the alpha test may also be on for a neutral draw)
        if (g155Trans_) c_.g155TransTris += (uint64_t)n_;   // G155
        ++c_.draws; if ((uint64_t)n_ > c_.maxBatch) c_.maxBatch = (uint64_t)n_;
        switch (r) { case kTexture: ++c_.flushTexture; break; case kState: ++c_.flushState; break; case kCapacity: ++c_.flushCapacity; break; default: ++c_.flushBoundary; break; }
        n_ = 0;
    }
    void begin(const Key& k, Shadow& sh, Gl& gl) {
        c_.syncQueries += (uint64_t)sh.sync(kSessionFields, gl);
        base_ = sh.v();
        ++c_.sessions;
        a_ = base_; appliedEnv_ = base_.texEnv; envKnown_ = sh.has(kFTexEnv);
        depthCfgValid_ = false; cullCfgValid_ = false; arrTex_ = false;
        g153Mod_ = false; g153Alpha_ = false; g155Neutral_ = 0; g155Sub_ = false; g155Trans_ = false; g155Key_ = false; g155LastBound_ = a_.bound;   // G153 / G155
        memcpy(vp_, base_.vp, sizeof vp_);
        gl.matrix_mode(kGlProjection); gl.push_matrix(); gl.load_identity(); gl.ortho(vp_[0], vp_[0] + vp_[2], vp_[1], vp_[1] + vp_[3]);
        gl.matrix_mode(kGlModelview); gl.push_matrix(); gl.load_identity();
        gl.enable_client(kGlVertexArray); gl.enable_client(kGlColorArray);
        gl.vertex_pointer(P_); gl.color_pointer(C_);
        open_ = true; n_ = 0;
        key_ = Key{};   // forces every field to be applied by transition()
        transition(k, sh, gl);
    }
    void transition(const Key& k, Shadow& sh, Gl& gl) {
        // G153-BEGIN
        const bool g153K = k.texReady != 0 && ign::g153::is_keyed(k.ckEnabled, k.ckValue), g153M = g153K && ign::g153::is_modulate(k.ckEnabled, k.ckValue);
        if (g155Classic_ && g153Mod_ && !g153M) { gl.texenv_mode(kGlReplace); g153Mod_ = false; ++c_.g154EnvRepl; }   // G155: classic (G154) behaviour: leaving a translucent keyed draw puts REPLACE back at once the GL texture environment stays MODULATE for the rest of the session once a translucent keyed draw set it (a textured draw is white-coloured by submit(), an untextured one ignores it); the code below sees an applied environment that needs no call
        if (g153Mod_) appliedEnv_ = k.texReady != 0 ? kGlReplace : base_.texEnv;   // G155: the GL texture environment stays MODULATE for the rest of the session once a translucent keyed draw set it (a textured draw is white-coloured by submit(), an untextured one ignores it); the code below sees an applied environment that needs no call
        const uint32_t g155EnvBefore = appliedEnv_; const bool g155EnvKnown = envKnown_;   // G155: the real texenv calls of the unmarked code below are counted from the change of the applied environment
        // G153-END
        // texture 2D enable + binding
        const bool wantTex = k.texReady != 0;
        if (a_.tex2d != wantTex) { if (wantTex) gl.enable(kGlTexture2D); else gl.disable(kGlTexture2D); a_.tex2d = wantTex; }
        if (wantTex && a_.bound != k.tex) { gl.bind_texture(k.tex); a_.bound = k.tex; }
        // depth
        const bool wantDepth = k.depth != 0;
        if (wantDepth) {
            if (!a_.depth) { gl.enable(kGlDepthTest); a_.depth = true; }
            if (!depthCfgValid_ || k.depthMask != depthMaskApplied_ || k.depthFunc != depthFuncApplied_) {
                gl.depth_mask(k.depthMask != 0); gl.depth_func(k.depthFunc); depthMaskApplied_ = k.depthMask; depthFuncApplied_ = k.depthFunc; depthCfgValid_ = true; }
        } else if (a_.depth) { gl.disable(kGlDepthTest); a_.depth = false; }
        // cull
        const bool wantCull = k.cull != 0;
        if (wantCull) {
            if (!a_.cull) { gl.enable(kGlCullFace); a_.cull = true; }
            if (!cullCfgValid_) { gl.cull_face_back(); gl.front_face_ccw(); cullCfgValid_ = true; }
        } else if (a_.cull) { gl.disable(kGlCullFace); a_.cull = false; }
        // viewport -> projection
        if (memcmp(k.vp, vp_, sizeof vp_) != 0) {
            memcpy(vp_, k.vp, sizeof vp_);
            gl.matrix_mode(kGlProjection); gl.load_identity(); gl.ortho(vp_[0], vp_[0] + vp_[2], vp_[1], vp_[1] + vp_[3]); gl.matrix_mode(kGlModelview);
        }
        // texture environment + texcoord array
        if (wantTex) {
            if (!envKnown_) { c_.syncQueries += (uint64_t)sh.sync(kFTexEnv, gl); base_.texEnv = sh.v().texEnv; appliedEnv_ = base_.texEnv; envKnown_ = true; }
            if (appliedEnv_ != kGlReplace) { gl.texenv_mode(kGlReplace); appliedEnv_ = kGlReplace; }
            if (!arrTex_) { gl.enable_client(kGlTexCoordArray); gl.texcoord_pointer(U_); arrTex_ = true; }
        } else {
            if (envKnown_ && appliedEnv_ != base_.texEnv) { gl.texenv_mode(base_.texEnv); appliedEnv_ = base_.texEnv; }
            if (arrTex_) { gl.disable_client(kGlTexCoordArray); arrTex_ = false; }
        }
        // G153-BEGIN
        if (g155EnvKnown ? appliedEnv_ != g155EnvBefore : (envKnown_ && appliedEnv_ != base_.texEnv)) ++c_.g155EnvOther;   // G155: a texenv call of the code above (the first textured draw of a session synchronises the baseline first: no call for that)
        if (a_.bound != g155LastBound_) { ++c_.g155Binds; g155LastBound_ = a_.bound; }   // G155: a texture bind the code above issued
        if (g153M && !g153Mod_) { gl.texenv_mode(kGlModulate); g153Mod_ = true; ++c_.g154EnvMod; }   // G154: the counter
        // G155: the alpha test. A keyed draw needs it ON. A textured draw that is not keyed samples a GL_RGB texture (fragment alpha 1, REPLACE or MODULATE with white): GREATER 0 cannot discard anything, so it does not care and the test is
        // left as it is for up to kG155NeutralDraws such draws (the next keyed run then costs no toggle). An untextured draw takes its alpha from the vertices: it needs the test OFF.
        if (g155Classic_) { if (g153K != g153Alpha_) { g153_alpha(gl, g153K, 0); g153Alpha_ = g153K; if (g153K) ++c_.g154AlphaOn; else ++c_.g154AlphaOff; } }   // G155: classic (G154) behaviour: the alpha test follows the keyed flag draw by draw
        else if (g153K) { if (!g153Alpha_) { g153_alpha(gl, true, 0); g153Alpha_ = true; ++c_.g154AlphaOn; } g155Neutral_ = 0; }
        else if (k.texReady != 0) { if (g153Alpha_) { if (++g155Neutral_ > kG155NeutralDraws) { g153_alpha(gl, false, 0); g153Alpha_ = false; ++c_.g154AlphaOff; ++c_.g155OffHorizon; } else ++c_.g155NeutralKept; } }
        else if (g153Alpha_) { g153_alpha(gl, false, 0); g153Alpha_ = false; ++c_.g154AlphaOff; ++c_.g155OffForced; }
        g155Sub_ = g153Mod_ && k.texReady != 0 && !g153M; g155Trans_ = g153M; g155Key_ = g153K; if (g153M && k.depth != 0 && k.depthMask != 0) ++c_.g155TransDepthWrite;   // G155
        // G153-END
        key_ = k;
    }

    bool open_ = false, depthCfgValid_ = false, cullCfgValid_ = false, arrTex_ = false, envKnown_ = false;
    bool g153Mod_ = false, g153Alpha_ = false;   // G153: g153Mod_ = the GL texture environment is MODULATE (sticky since G155), g153Alpha_ = the alpha test is on
    bool g155Sub_ = false, g155Trans_ = false, g155Key_ = false, g155Classic_ = false; uint32_t g155Neutral_ = 0, g155LastBound_ = 0;   // G155: white-colour substitution active for the current key, the current key is a translucent keyed draw, neutral draws since the last keyed one
    int n_ = 0;
    Key key_{};
    GlBase base_{}, a_{};
    uint32_t appliedEnv_ = 0, depthMaskApplied_ = 0, depthFuncApplied_ = 0;
    int32_t vp_[4] = {0, 0, 0, 0};
    SubmitCounters c_{};
    float P_[kBatchCap * 6] = {};
    float C_[kBatchCap * 12] = {};
    float U_[kBatchCap * 6] = {};
};

// ---- the prelude of every wrapped GL call ------------------------------------------------------------------------------------------------------------
struct PreState { bool errPending = false; uint64_t errDrains = 0, errNonZero = 0, boundaryEnds = 0; };
// closes the open session (flush in order, restore the baseline) and drains the GL error the batched/individual draws may have raised (ONE glGetError per boundary instead of one per triangle)
template <class Gl>
inline void pre(Session<Gl>& s, Gl& gl, PreState& p) {
    if (s.open()) { s.end(gl); p.errPending = true; ++p.boundaryEnds; }
    if (p.errPending) {
        p.errPending = false; ++p.errDrains;
        for (int n = 0; n < 8; ++n) { if (gl.get_error() == 0) break; ++p.errNonZero; }
    }
}

// ---- per-window accounting + the one-line summaries (pure; one line per 120 race frames, never one per batch or triangle) ----------------------------------
struct SubmitWindow {
    uint32_t frames = 0;
    uint64_t tris = 0, texturedTris = 0, draws = 0, sessions = 0, flushTexture = 0, flushState = 0, flushCapacity = 0, flushBoundary = 0, individualTris = 0, syncQueries = 0, glCalls = 0,
             errDrains = 0, errNonZero = 0, maxBatch = 0, boundaryEnds = 0;
    void clear() { *this = SubmitWindow{}; }
    // what G138 did per triangle and this build no longer does: 7 state queries (+1 texture-environment query for a textured triangle) -- minus the queries still issued by the shadow
    uint64_t queries_avoided() const { const uint64_t g138 = 7 * (tris + individualTris) + texturedTris; return g138 > syncQueries ? g138 - syncQueries : 0; }
    double tris_per_draw() const { return draws ? (double)tris / (double)draws : 0.0; }
    // G138 issued about 35 GL calls per textured triangle (ign::g138 report, section 4); an ESTIMATE of the calls this window did not issue
    uint64_t calls_avoided_est() const { const uint64_t g138 = 35 * (tris + individualTris); return g138 > glCalls ? g138 - glCalls : 0; }
};
inline int format_submit_race(char* out, size_t n, int win, double fps, double avgMs, double p50, double p90, double p95, double p99, double sceneMs, double compMs, double uploadMs, double presentMs,
                              const SubmitWindow& w) {
    const double f = w.frames ? (double)w.frames : 1.0;
    return snprintf(out, n,
        "win=%d frames=%u | fps=%.2f avg_ms=%.2f p50=%.0f p90=%.0f p95=%.0f p99=%.0f | ms/frame scene=%.2f compositor=%.2f tex_upload=%.2f present=%.2f | per_frame: tris=%.0f draw_calls=%.1f "
        "tris_per_draw=%.1f batches=%.1f avg_batch=%.1f max_batch=%llu individual_tris=%.1f | flush: texture=%.1f state=%.1f capacity=%.1f boundary=%.1f (per frame) | "
        "gl_queries_avoided=%.1f/frame gl_calls_issued=%.1f/frame (%.1f per triangle) gl_calls_avoided_est=%.1f/frame",
        win, w.frames, fps, avgMs, p50, p90, p95, p99, sceneMs, compMs, uploadMs, presentMs, (double)(w.tris + w.individualTris) / f, (double)(w.draws + w.individualTris) / f, w.tris_per_draw(),
        (double)w.draws / f, w.draws ? (double)w.tris / (double)w.draws : 0.0, (unsigned long long)w.maxBatch, (double)w.individualTris / f,
        (double)w.flushTexture / f, (double)w.flushState / f, (double)w.flushCapacity / f, (double)w.flushBoundary / f,
        (double)w.queries_avoided() / f, (double)w.glCalls / f, (w.tris + w.individualTris) ? (double)w.glCalls / (double)(w.tris + w.individualTris) : 0.0, (double)w.calls_avoided_est() / f);
}
inline int format_batch_summary(char* out, size_t n, const SubmitWindow& w) {
    const uint64_t flushes = w.flushTexture + w.flushState + w.flushCapacity + w.flushBoundary;
    auto pct = [&](uint64_t v) { return flushes ? 100.0 * (double)v / (double)flushes : 0.0; };
    return snprintf(out, n,
        "cumulative race: frames=%u triangles_batched=%llu individual_triangles=%llu draw_calls=%llu sessions=%llu avg_batch=%.2f max_batch=%llu capacity=%d | flush reasons: texture=%.1f%% state=%.1f%% capacity=%.1f%% "
        "boundary(other GL call / swap)=%.1f%% | sessions closed by another GL call=%llu | one draw call replaces %.2f triangles on average",
        w.frames, (unsigned long long)w.tris, (unsigned long long)w.individualTris, (unsigned long long)w.draws, (unsigned long long)w.sessions, w.draws ? (double)w.tris / (double)w.draws : 0.0,
        (unsigned long long)w.maxBatch, kBatchCap, pct(w.flushTexture), pct(w.flushState), pct(w.flushCapacity), pct(w.flushBoundary), (unsigned long long)w.boundaryEnds, w.tris_per_draw());
}
inline int format_shadow(char* out, size_t n, const SubmitWindow& w, uint64_t checks, uint64_t mismatches, uint32_t fieldsSeen) {
    return snprintf(out, n,
        "cumulative race: shadow_checks=%llu mismatches=%llu (fields differing, bit mask 0x%02x: depth=1 cull=2 tex2d=4 blend=8 bound_texture=16 matrix_mode=32 viewport=64 texenv=128; a mismatch resynchronises the shadow) | "
        "sync_queries_total=%llu (G138 would have issued %llu) queries_avoided=%llu | pending_draw_errors_drained=%llu nonzero=%llu | per-triangle glIsEnabled/glGetIntegerv/glGetError in the draw session: 0",
        (unsigned long long)checks, (unsigned long long)mismatches, fieldsSeen, (unsigned long long)w.syncQueries, (unsigned long long)(7 * (w.tris + w.individualTris) + w.texturedTris),
        (unsigned long long)w.queries_avoided(), (unsigned long long)w.errDrains, (unsigned long long)w.errNonZero);
}

}}   // namespace ign::g139
