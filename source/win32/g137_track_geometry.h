// win32/g137_track_geometry.h -- the pure decision logic of the G137 track-geometry integrity work (no GL, no guest access, no logging), so that the host
// test (tools/g137_test.cpp) runs exactly the code the console runs.
//
// WHAT THE GUEST DOES (proved statically from Ign_3dfx.exe, see tmp/G137_TRACK_GEOMETRY_REPORT.txt, scratchpad g137_vtxflow.py):
// the display-list interpreter 0x0044FF20 draws its polygons through three helpers that build the three GrVertex of guDrawTriangleWithClip in LOCALS of
// their own stack frame and never write the `a` field (offset 0x1C) of any vertex, nor guAlphaSource:
//     flat triangle           0x00450B50  -> guDrawTriangleWithClip return address 0x00450C88
//     textured, opaque        0x004510C0  -> 0x00451336
//     textured, chroma-keyed  0x00451380  -> 0x00451618
// Their blend is always (ONE,ZERO) (0x00450C90) and the colour combine is CCRGB / DECAL_TEXTURE, so the source alpha is not read by anything -- real Glide
// ignores it. `a` therefore holds whatever the stack had there (0xFFFFFFCB, 0xFFFFFFD9, 0xFFFFFFA8 ... are NaNs as floats; small positive leftovers are
// denormals and pass the finite test, which is why the drops were intermittent).
//
// THE PORT'S PROBLEM: G108 treats `a` as "required" whenever the alpha source is not the constant colour (guAlphaSource state left over from some earlier
// draw), and drops the whole triangle when it is not finite -> holes in the track.
// THE FIX: for exactly these three callers, a non-finite `a` is repaired (never validated, set to opaque) ONLY while nothing can read the source alpha
// (blend factors without SRC_ALPHA / ONE_MINUS_SRC_ALPHA / ALPHA_SATURATE, colour combine without "local alpha"). Everything else keeps the G108 policy.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ign { namespace g137 {

// guDrawTriangleWithClip return addresses of the three helpers
constexpr uint32_t kCallerFlat = 0x00450C88u, kCallerTex = 0x00451336u, kCallerTexKey = 0x00451618u;
constexpr int kFamilies = 3;
// offsets, from the C vertex pointer, of the return address of the helper itself (the display-list interpreter's call site): the helper's frame is
// 0xB8 bytes, vertex C lives at frame+4 in all three helpers (proved by the static stack-flow analysis)
constexpr uint32_t kHelperReturnFromPtrC = 0xB4u;

inline int family_index(uint32_t caller) {
    return caller == kCallerFlat ? 0 : caller == kCallerTex ? 1 : caller == kCallerTexKey ? 2 : -1;
}
inline const char* family_name(int f) {
    return f == 0 ? "flat_0x00450B50" : f == 1 ? "textured_0x004510C0" : f == 2 ? "textured_chromakey_0x00451380" : "other";
}

// ----- is the source alpha consumed by anything? ------------------------------------------------------------------------------------------------
struct AlphaState {
    bool blendValid = false; uint32_t rgbSrc = 0, rgbDst = 0, aSrc = 0, aDst = 0;   // Glide-side record of the guest's last grAlphaBlendFunction
    bool colorValid = false; uint32_t colorFunction = 0;                               // Glide-side record of the colour combine
    bool alphaTest = false;                                                            // the guest never sets an alpha test (no import, no state)
};
constexpr uint32_t kBlendSrcAlpha = 0x1, kBlendOneMinusSrcAlpha = 0x5, kBlendAlphaSaturate = 0xF;   // GR_BLEND_*
constexpr uint32_t kFuncScaleOtherAddLocalAlpha = 0x5;                                              // the only colour-combine function of the port that reads local alpha
inline bool blend_factor_reads_source_alpha(uint32_t f) { return f == kBlendSrcAlpha || f == kBlendOneMinusSrcAlpha || f == kBlendAlphaSaturate; }
// strict: any state this code cannot vouch for counts as "consumed", which keeps the G108 policy
inline bool alpha_consumed(const AlphaState& s) {
    if (!s.blendValid || !s.colorValid) return true;
    if (s.alphaTest) return true;
    if (blend_factor_reads_source_alpha(s.rgbSrc) || blend_factor_reads_source_alpha(s.rgbDst) ||
        blend_factor_reads_source_alpha(s.aSrc) || blend_factor_reads_source_alpha(s.aDst)) return true;
    if (s.colorFunction == kFuncScaleOtherAddLocalAlpha) return true;
    return false;
}

// ----- the verdict on one triangle -----------------------------------------------------------------------------------------------------------------
enum class Decision : uint8_t { Send = 0, Repair = 1, Drop = 2 };
struct Verdict {
    Decision d = Decision::Send;
    const char* badField = nullptr;   // first non-finite required field (Drop only)
    int badVertex = -1;               // 0=A 1=B 2=C
    bool alphaRepaired[3] = {false, false, false};
};
// Same field order and requiredness as G108's gu_vertex_required_finite (x,y always; r,g,b if iterated colour; a if iterated alpha), per vertex A, B, C.
// `family` >= 0 (one of the three confirmed callers) and `alphaConsumed == false` and iterated alpha: `a` is NOT required; a non-finite `a` is flagged
// for repair instead of failing the triangle. Nothing else changes: x/y/r/g/b still drop the triangle, other callers keep the exact G108 behaviour.
template <class V>
inline Verdict judge(int family, bool useConstantRgb, bool useConstantAlpha, bool alphaConsumed, const V& a, const V& b, const V& c) {
    Verdict r;
    const V* v[3] = {&a, &b, &c};
    const bool alphaOptional = family >= 0 && !useConstantAlpha && !alphaConsumed;
    for (int i = 0; i < 3; ++i) {
        const V& q = *v[i];
        const char* bad = nullptr;
        if (!std::isfinite(q.x)) bad = "x";
        else if (!std::isfinite(q.y)) bad = "y";
        else if (!useConstantRgb && !std::isfinite(q.r)) bad = "r";
        else if (!useConstantRgb && !std::isfinite(q.g)) bad = "g";
        else if (!useConstantRgb && !std::isfinite(q.b)) bad = "b";
        else if (!useConstantAlpha && !alphaOptional && !std::isfinite(q.a)) bad = "a";
        if (bad) { r.d = Decision::Drop; r.badField = bad; r.badVertex = i; return r; }
    }
    if (alphaOptional) {
        for (int i = 0; i < 3; ++i) if (!std::isfinite(v[i]->a)) { r.alphaRepaired[i] = true; r.d = Decision::Repair; }
    }
    return r;
}
// iteration-G138, LIGHT fast path: judge() differs from G108's own chain ONLY for a family caller with iterated alpha and at least one non-finite alpha. In every other case (non-family caller,
// constant alpha, or three finite alphas) the verdict IS the G108 chain, so the LIGHT build skips judge()/alpha_consumed() unless this predicate holds. The host test proves the equivalence.
template <class V>
inline bool light_needs_judge(int family, bool useConstantAlpha, const V& a, const V& b, const V& c) {
    return family >= 0 && !useConstantAlpha && !(std::isfinite(a.a) && std::isfinite(b.a) && std::isfinite(c.a));
}
// the repair itself: only the flagged component of the flagged vertices, to the neutral opaque value (Glide's 0..255 range); returns how many were set
template <class V>
inline int apply_alpha_repair(const Verdict& r, V& a, V& b, V& c) {
    int n = 0;
    if (r.alphaRepaired[0]) { a.a = 255.0f; ++n; }
    if (r.alphaRepaired[1]) { b.a = 255.0f; ++n; }
    if (r.alphaRepaired[2]) { c.a = 255.0f; ++n; }
    return n;
}
inline uint32_t float_bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// ----- bounded capture of the last family calls (static storage, no heap) ------------------------------------------------------------------------
constexpr int kRingSize = 16;
struct Entry {
    uint32_t swap = 0, caller = 0, helperRet = 0, mmid = 0;
    uint32_t ptr[3] = {0, 0, 0};
    uint32_t raw[3][15] = {};         // the 15 dwords of each GrVertex, as read from guest memory
    uint8_t decision = 0;             // Decision
    char field = '-';                 // first non-finite required field, '-' when none
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
};
class Ring {
public:
    void push(const Entry& e) { buf_[head_] = e; head_ = (head_ + 1) % kRingSize; if (count_ < kRingSize) ++count_; }
    int size() const { return count_; }
    // i = 0 is the oldest kept entry
    const Entry& at(int i) const { return buf_[(head_ - count_ + i + 2 * kRingSize) % kRingSize]; }
private:
    Entry buf_[kRingSize];
    int head_ = 0, count_ = 0;
};

// ----- bursts of invalid triangles --------------------------------------------------------------------------------------------------------------------
constexpr int kQuietSwapsToEnd = 20;     // swaps without an invalid family triangle that end a burst
constexpr int kMaxBurstsLogged = 4;
constexpr uint32_t kFarLimit = 4096;     // |x| or |y| beyond this is "extreme" (the Glide screen is 640x400): counted, never acted upon

struct BurstStats {
    int beginSwap = -1, lastSwap = -1;
    uint32_t total = 0, dropped = 0, repaired = 0, maxPerFrame = 0;
    uint32_t perFamily[kFamilies] = {0, 0, 0};
    uint32_t fieldA = 0, fieldOther = 0;
    float firstBox[4] = {0, 0, 0, 0}, lastBox[4] = {0, 0, 0, 0};
    uint32_t firstMmid = 0, lastMmid = 0;
};
enum : uint32_t { kEvNone = 0, kEvBegin = 1, kEvEnd = 2 };

class BurstTracker {
public:
    // one invalid (dropped or repaired) family triangle; returns kEvBegin when it opens a burst
    uint32_t on_invalid(int swap, int family, bool dropped, const char* field, const float box[4], uint32_t mmid) {
        uint32_t ev = kEvNone;
        if (!active_) { active_ = true; cur_ = BurstStats{}; cur_.beginSwap = swap; ++bursts_; ev = kEvBegin; frameCount_ = 0; frameSwap_ = swap; std::memcpy(cur_.firstBox, box, sizeof cur_.firstBox); cur_.firstMmid = mmid; }
        if (swap != frameSwap_) { frameSwap_ = swap; frameCount_ = 0; }
        ++frameCount_; if (frameCount_ > cur_.maxPerFrame) cur_.maxPerFrame = frameCount_;
        ++cur_.total; if (dropped) ++cur_.dropped; else ++cur_.repaired;
        if (family >= 0 && family < kFamilies) ++cur_.perFamily[family];
        if (field && field[0] == 'a' && field[1] == 0) ++cur_.fieldA; else ++cur_.fieldOther;
        cur_.lastSwap = swap; std::memcpy(cur_.lastBox, box, sizeof cur_.lastBox); cur_.lastMmid = mmid;
        ++totalInvalid_; if (dropped) ++totalDropped_; else ++totalRepaired_;
        return ev;
    }
    // once per swap; returns kEvEnd when the quiet period ended the burst (the finished burst stays readable through finished())
    uint32_t on_swap(int swap) {
        if (active_ && swap - cur_.lastSwap >= kQuietSwapsToEnd) { active_ = false; done_ = cur_; return kEvEnd; }
        return kEvNone;
    }
    bool active() const { return active_; }
    int bursts() const { return bursts_; }
    bool log_this_burst() const { return bursts_ <= kMaxBurstsLogged; }
    const BurstStats& current() const { return cur_; }
    const BurstStats& finished() const { return done_; }
    uint32_t total_invalid() const { return totalInvalid_; }
    uint32_t total_dropped() const { return totalDropped_; }
    uint32_t total_repaired() const { return totalRepaired_; }
private:
    bool active_ = false; int bursts_ = 0, frameSwap_ = -1; uint32_t frameCount_ = 0;
    BurstStats cur_{}, done_{};
    uint32_t totalInvalid_ = 0, totalDropped_ = 0, totalRepaired_ = 0;
};

// bounding box of the three screen positions (finite inputs; non-finite coordinates are skipped)
template <class V>
inline void bbox(const V& a, const V& b, const V& c, float out[4]) {
    const V* v[3] = {&a, &b, &c};
    bool have = false; out[0] = out[1] = out[2] = out[3] = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i]->x) || !std::isfinite(v[i]->y)) continue;
        if (!have) { out[0] = out[2] = v[i]->x; out[1] = out[3] = v[i]->y; have = true; continue; }
        if (v[i]->x < out[0]) { out[0] = v[i]->x; }
        if (v[i]->x > out[2]) { out[2] = v[i]->x; }
        if (v[i]->y < out[1]) { out[1] = v[i]->y; }
        if (v[i]->y > out[3]) { out[3] = v[i]->y; }
    }
}
inline bool extreme_position(const float box[4]) {
    return std::fabs(box[0]) > kFarLimit || std::fabs(box[1]) > kFarLimit || std::fabs(box[2]) > kFarLimit || std::fabs(box[3]) > kFarLimit;
}

}}   // namespace ign::g137
