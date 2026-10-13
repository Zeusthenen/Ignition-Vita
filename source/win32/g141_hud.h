// iteration-G141: first-race HUD sharpness -- bounded, passive comparison of the HUD state of the first race of a process against a later race (pure: no GL, no guest access, host-testable).
//
// What the HUD is (proven in the G141 report): the race HUD is NOT drawn with guest textures. The guest writes it into the LFB (grLfbLock / palette blit 0x004451A0 / text 0x00453560, ~42 lock/unlock pairs per
// race frame); the port's compositor (G104..G115) turns the LFB shadow into the RGBA5551 overlay texture (alpha 0 = overlay key) and draws one quad per active tile (g104_sparse_overlay_blit, NEAREST / CLAMP
// textures created once). "The HUD" of a frame is therefore identified structurally: an overlay draw (quads > 0) inside an armed race scene (race latch, a structural track load before it, no car carousel).
// This header holds the signature of such a draw, its comparison and the per-race state machine that decides WHEN a signature is recorded (frame 1, 30, 120 and every change, at most kRaceMaxLines lines per race).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign::g141 {

// guest values (read-only, 0x........ addresses of Ign_3dfx.exe; see the report for how each was established)
constexpr uint32_t kAddrRaceFlag = 0x00537180u;   // 1 while a race runs (set 0x004113E0 after the front end returns "start", cleared 0x00411D54)
constexpr uint32_t kAddrTrackIdx = 0x00536EA4u;   // selected track: 0 CANADA / MOOSEJAW, 1 USA / GOLDRUSH, 2 CARIB / SNAKEISL, 3 BRAZIL / LOSTRUIN, 4 AUSTRIA / YODELPEA, 5 ICELAND / CAPETHOR, 6 JAPAN / TOKYOBUL
constexpr uint32_t kAddrHudSet = 0x005338A8u;     // HUD font / layout set: 0 *_s, 1 normal, 2 *_h (fonts 0x0050C504 + set*4, layout tables 0x00488448 + set*0x20)
constexpr uint32_t kAddrHudMode = 0x0050BE08u;    // mode byte of the table that picks the set (0x0041A060 / 0x00415970)
constexpr uint32_t kAddrPlayers = 0x00536F68u;
constexpr uint32_t kAddrHires = 0x00536EA0u;      // set to 1 at 0x00413275 when a race is set up

constexpr uint32_t kRaceMaxLines = 20;            // lines per race (all four tags together)
constexpr uint32_t kCaptureFrames[3] = {1, 30, 120};
constexpr uint32_t kMaxChangesPerRace = 4;
constexpr uint32_t kUnset = 0xFFFFFFFFu;

inline const char* track_dir(uint32_t idx) {      // 0x004137C0 .. 0x0041394F: index -> (directory, short name used for the ghost / loading files)
    switch (idx) { case 0: return "CANADA(MOOSEJAW)"; case 1: return "USA(GOLDRUSH)"; case 2: return "CARIB(SNAKEISL)"; case 3: return "BRAZIL(LOSTRUIN)";
                   case 4: return "AUSTRIA(YODELPEA)"; case 5: return "ICELAND(CAPETHOR)"; case 6: return "JAPAN(TOKYOBUL)"; default: return "?"; }
}

struct Sig {
    // ---- compositor path ----
    uint8_t useAlpha = 0, alphaMode = 0, alphaOk = 0, armed = 0, fillKnown = 0, shadowClean = 0, keyValid = 0, keyProv = 0, keySrc = 0;
    uint8_t ovTexInit = 0, lfbTexInit = 0, wasBlend = 0, wasDepth = 0, wasCull = 0, wasAlphaTest = 0, scissorOn = 0xFF /* 0xFF = not queried (expensive part) */, cls = 0, sessionOpen = 0;
    uint16_t keyRgb565 = 0; uint32_t keyRaw = 0;
    // ---- GL state found by the overlay draw (inherited from the 3D scene) ----
    uint32_t ovTex = 0, lfbTex = 0, prevTex = 0, prevMat = 0, texEnv = 0, logicalW = 0, logicalH = 0, stride = 0;
    int32_t vp[4] = {0, 0, 0, 0}, scissor[4] = {0, 0, 0, 0};
    // ---- who drew it (first pair of the race phase table, number of distinct pairs) ----
    uint32_t lockCaller = 0, unlockCaller = 0, families = 0;
    // ---- Glide state left by the last 3D material of the frame ----
    uint32_t ckOn = 0, ckValue = 0, colorFnc = 0, colorFactor = 0, colorLocal = 0, colorOther = 0, alphaFnc = 0, alphaFactor = 0, alphaLocal = 0, alphaOther = 0;
    uint32_t blendValid = 0, blendSrc = 0, blendDst = 0, depthMode = 0, depthMask = 0, depthFunc = 0, cullMode = 0;
    uint32_t texMmid = 0, texGl = 0, texW = 0, texH = 0, texFmt = 0, texMin = 0, texMag = 0, texS = 0, texT = 0;
    // ---- guest values ----
    uint32_t gTrack = kUnset, gHudSet = kUnset, gMode = kUnset, gPlayers = kUnset, gHires = kUnset, gRaceFlag = kUnset;
    // ---- sample of the geometry of the first active tile (logical tile -> physical quad) ----
    float qx0 = 0, qy0 = 0, qx1 = 0, qy1 = 0, qu0 = 0, qv0 = 0, qu1 = 0, qv1 = 0;
    // ---- volatile (never part of the comparison) ----
    uint32_t tiles = 0, col0 = 0, row0 = 0, col1 = 0, row1 = 0, uploadPending = 0, pendingTris = 0, hudFrame = 0, raceSwaps = 0;
    uint64_t flushTex = 0, flushState = 0, flushCap = 0, flushBoundary = 0, draws = 0, tris = 0;
};

// Names of the fields that differ between two signatures, comma separated into `out`; returns how many differ.
//  coreOnly == true : only what describes the PATH of the overlay (alpha mode, key, textures, viewport, callers, guest HUD values) -- used to detect a change INSIDE a race. The GL state the 3D scene happens to leave
//                     (blend, depth, cull, bound texture, last material) legitimately changes from frame to frame and is only compared between recorded captures.
//  coreOnly == false: every stable field (the cross-race comparison); fields of the expensive part (scissor, texenv, alpha test) are skipped when either side did not query them (both scissorOn values equal 0xFF).
inline uint32_t diff_fields(const Sig& a, const Sig& b, char* out, size_t n, bool coreOnly) {
    uint32_t cnt = 0; size_t len = 0; if (n) out[0] = 0;
    auto add = [&](bool d, const char* name) {
        if (!d) { return; }
        ++cnt;
        const size_t l = strlen(name);
        if (len + l + 2 < n) { if (len) { out[len++] = ','; } memcpy(out + len, name, l); len += l; out[len] = 0; }
    };
#define G141_F(f) add(a.f != b.f, #f)
    G141_F(useAlpha); G141_F(alphaMode); G141_F(alphaOk); G141_F(armed); G141_F(keyValid); G141_F(keyProv); G141_F(keySrc); G141_F(keyRgb565);
    G141_F(ovTexInit); G141_F(lfbTexInit); G141_F(ovTex); G141_F(lfbTex); G141_F(logicalW); G141_F(logicalH); G141_F(stride);
    add(memcmp(a.vp, b.vp, sizeof a.vp) != 0, "viewport");
    G141_F(lockCaller); G141_F(unlockCaller); G141_F(families);
    G141_F(gTrack); G141_F(gHudSet); G141_F(gMode); G141_F(gPlayers); G141_F(gHires);
    if (!coreOnly) {
        G141_F(keyRaw); G141_F(fillKnown); G141_F(shadowClean); G141_F(wasBlend); G141_F(wasDepth); G141_F(wasCull); G141_F(cls); G141_F(sessionOpen); G141_F(prevTex); G141_F(prevMat);
        const bool q = a.scissorOn != 0xFF && b.scissorOn != 0xFF;
        if (q) { G141_F(wasAlphaTest); G141_F(scissorOn); G141_F(texEnv); add(memcmp(a.scissor, b.scissor, sizeof a.scissor) != 0, "scissor_box"); }
        G141_F(ckOn); G141_F(ckValue); G141_F(colorFnc); G141_F(colorFactor); G141_F(colorLocal); G141_F(colorOther); G141_F(alphaFnc); G141_F(alphaFactor); G141_F(alphaLocal); G141_F(alphaOther);
        G141_F(blendValid); G141_F(blendSrc); G141_F(blendDst); G141_F(depthMode); G141_F(depthMask); G141_F(depthFunc); G141_F(cullMode);
        G141_F(texMmid); G141_F(texGl); G141_F(texW); G141_F(texH); G141_F(texFmt); G141_F(texMin); G141_F(texMag); G141_F(texS); G141_F(texT);
        add(a.qx0 != b.qx0 || a.qy0 != b.qy0 || a.qx1 != b.qx1 || a.qy1 != b.qy1, "quad_xy"); add(a.qu0 != b.qu0 || a.qv0 != b.qv0 || a.qu1 != b.qu1 || a.qv1 != b.qv1, "quad_uv");
    }
#undef G141_F
    return cnt;
}

inline const char* cls_name(uint32_t c) {   // G104Class order in glide2x_ign.cpp
    switch (c) { case 0: return "LFB_ONLY"; case 1: return "GL_ONLY"; case 2: return "GL_PLUS_UNCHANGED_LFB"; case 3: return "GL_PLUS_SPARSE_LFB"; case 4: return "GL_PLUS_FULLSCREEN_LFB"; case 5: return "UNKNOWN"; default: return "?"; }
}
inline const char* key_src_name(uint32_t s) { switch (s) { case 1: return "LOCK_CONFIRMED"; case 2: return "CACHED_PROVISIONAL"; case 3: return "SEED_PROVISIONAL"; default: return "NONE"; } }

// One signature as ONE line body (< 1.5 KB). Filter / wrap of the overlay textures are "declared": vitaGL has no glGetTexParameter, they are written once at creation (NEAREST / CLAMP_TO_EDGE).
inline int format_sig(char* out, size_t n, const Sig& s) {
    const double sx = s.logicalW ? (double)s.vp[2] / (double)s.logicalW : 0.0, sy = s.logicalH ? (double)s.vp[3] / (double)s.logicalH : 0.0;
    return snprintf(out, n,
        "compositor{alpha_overlay_used=%u alpha_mode=%u alpha_ok=%u armed=%u class=%s fill_known=%u shadow_clean=%u key=%s raw=0x%08x rgb565=0x%04x provisional=%u tiles=%u bbox=(%u,%u)-(%u,%u) upload_pending=%u} "
        "lfb{lock_caller=0x%08x unlock_caller=0x%08x distinct_pairs=%u logical=%ux%u stride=%u fmt=RGB565->RGBA5551(alpha0=key)} "
        "textures{overlay=%u(init=%u) lfb=%u(init=%u) bound_before_overlay=%u filters=NEAREST/NEAREST(declared) wrap=CLAMP_TO_EDGE(declared)} "
        "gl_before_overlay{blend=%u depth=%u cull=%u alpha_test=%d scissor=%d box=(%d,%d,%d,%d) texenv=0x%04x matrix_mode=0x%04x viewport=(%d,%d,%d,%d) scale=%.4fx%.4f (logical %ux%u -> physical %dx%d)} "
        "quad0{xy=(%.2f,%.2f)-(%.2f,%.2f) uv=(%.5f,%.5f)-(%.5f,%.5f)} "
        "batch{open_at_overlay=%u pending_tris=%u flush_tex=%llu flush_state=%llu flush_cap=%llu flush_boundary=%llu draws=%llu tris=%llu} "
        "glide_last_material{chroma=%u/0x%08x color_combine=%u/%u/%u/%u alpha_combine=%u/%u/%u/%u blend=%u:%u>%u depth=%u/%u/0x%x cull=%u} "
        "tex3d_last{mmid=%u gl=%u %ux%u fmt=%u min=%u mag=%u clamp=%u/%u} "
        "guest{race_flag=%d track=%d(%s) hud_set=%d mode=%d players=%d hires=%d}",
        s.useAlpha, s.alphaMode, s.alphaOk, s.armed, cls_name(s.cls), s.fillKnown, s.shadowClean, s.keyValid ? key_src_name(s.keySrc) : "NONE", s.keyRaw, (unsigned)s.keyRgb565, s.keyProv, s.tiles, s.col0, s.row0, s.col1, s.row1, s.uploadPending,
        s.lockCaller, s.unlockCaller, s.families, s.logicalW, s.logicalH, s.stride,
        s.ovTex, s.ovTexInit, s.lfbTex, s.lfbTexInit, s.prevTex,
        s.wasBlend, s.wasDepth, s.wasCull, s.scissorOn == 0xFF ? -1 : (int)s.wasAlphaTest, s.scissorOn == 0xFF ? -1 : (int)s.scissorOn, s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3], s.texEnv, s.prevMat, s.vp[0], s.vp[1], s.vp[2], s.vp[3], sx, sy, s.logicalW, s.logicalH, s.vp[2], s.vp[3],
        s.qx0, s.qy0, s.qx1, s.qy1, s.qu0, s.qv0, s.qu1, s.qv1,
        s.sessionOpen, s.pendingTris, (unsigned long long)s.flushTex, (unsigned long long)s.flushState, (unsigned long long)s.flushCap, (unsigned long long)s.flushBoundary, (unsigned long long)s.draws, (unsigned long long)s.tris,
        s.ckOn, s.ckValue, s.colorFnc, s.colorFactor, s.colorLocal, s.colorOther, s.alphaFnc, s.alphaFactor, s.alphaLocal, s.alphaOther, s.blendValid, s.blendSrc, s.blendDst, s.depthMode, s.depthMask, s.depthFunc, s.cullMode,
        s.texMmid, s.texGl, s.texW, s.texH, s.texFmt, s.texMin, s.texMag, s.texS, s.texT,
        (int)s.gRaceFlag, (int)s.gTrack, s.gTrack == kUnset ? "?" : track_dir(s.gTrack), (int)s.gHudSet, (int)s.gMode, (int)s.gPlayers, (int)s.gHires);
}

enum Plan : uint32_t { kPlanNone = 0, kPlanCapture = 1, kPlanChange = 2, kPlanNewRace = 4 };

class Tracker {
public:
    uint32_t race() const { return race_; }
    uint32_t frame() const { return frame_; }                 // overlay (HUD) frames seen in the current race
    uint32_t lines() const { return lines_; }
    uint32_t captures_in_race() const { return captures_; }
    uint32_t changes_in_race() const { return changes_; }
    // true while the guest values are worth reading (the first two races, around the capture frames)
    bool wants_guest(uint32_t raceNo) const { return raceNo >= 1 && raceNo <= 2 && (raceNo != race_ || frame_ < kCaptureFrames[2] + 4); }
    // announces the race the compositor is in (race scene counter of the port); returns kPlanNewRace on a change
    uint32_t on_race(uint32_t raceNo) {
        if (raceNo == race_) return kPlanNone;
        race_ = raceNo; frame_ = 0; lines_ = 0; captures_ = 0; changes_ = 0; haveLast_ = false; return kPlanNewRace;
    }
    // decides, from the CHEAP part of the signature, whether this overlay frame must be recorded (then the caller fills the expensive part and calls commit)
    uint32_t plan(const Sig& cheap) const {
        if (race_ == 0) return kPlanNone;
        const uint32_t f = frame_ + 1;
        uint32_t p = kPlanNone;
        for (uint32_t i = 0; i < 3; ++i) if (f == kCaptureFrames[i] && captures_ < 3) p |= kPlanCapture;
        if (haveLast_ && changes_ < kMaxChangesPerRace && race_ <= 2) { char tmp[8]; if (diff_fields(cheap, last_, tmp, sizeof tmp, true) != 0) p |= kPlanChange; }
        return p;
    }
    // records the signature of this frame (once per overlay frame; `full` is the complete signature when plan() asked for a record, the cheap one otherwise)
    void commit(const Sig& full, uint32_t plan) {
        ++frame_;
        if (plan & kPlanCapture) {
            ++captures_;
            if (captures_ <= 3) { if (race_ == 1) { first_[captures_ - 1] = full; firstMask_ |= 1u << (captures_ - 1); } else if (race_ == 2) { ref_[captures_ - 1] = full; refMask_ |= 1u << (captures_ - 1); } }
        }
        if (plan & kPlanChange) ++changes_;
        if (plan != kPlanNone || !haveLast_) { last_ = full; haveLast_ = true; }
    }
    // the per-race line budget (the four tags together)
    bool take_line() { if (lines_ >= kRaceMaxLines) return false; ++lines_; return true; }
    bool has_first(uint32_t i) const { return i < 3 && ((firstMask_ >> i) & 1u); }
    bool has_ref(uint32_t i) const { return i < 3 && ((refMask_ >> i) & 1u); }
    const Sig& first(uint32_t i) const { return first_[i]; }
    const Sig& ref(uint32_t i) const { return ref_[i]; }
    const Sig& last() const { return last_; }
private:
    uint32_t race_ = 0, frame_ = 0, lines_ = 0, captures_ = 0, changes_ = 0, firstMask_ = 0, refMask_ = 0;
    bool haveLast_ = false;
    Sig last_{}, first_[3]{}, ref_[3]{};
};

}  // namespace ign::g141
