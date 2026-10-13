// win32/g156_cull.h -- iteration-G156, PURE (no GL, no Vita API, no logging, no heap, no global state; the guest memory goes through a small interface so tools/g156_test.cpp can run the same code against the ORIGINAL machine code).
//
// WHAT THE G155 REAL RUN SAID (tmp/G155_REAL_RUN_LOGS, five races; analysis: tmp/G156_HEAVY_ZONE_ANALYSIS.txt):
//  * the heavy windows of Yodel (frames of 45-55 ms) receive 1,100-1,670 triangles per frame and the host rejects 50-66 % of them at LEVEL 0 (all of them beyond the GL viewport, none by the scissor). A rejected triangle is cheap for
//    the HOST (~3 us) but not for the whole machine: the game spends ~7 us of guest code to build the call (a helper per triangle), 1.7 Glide state calls (~7 us each: guest set-up + the guest->host crossing) per rejected triangle,
//    and the crossing itself. Measured (windows of the real run): 20 ms of the 52 ms frame of the heaviest Yodel window goes to triangles that are then rejected. The keyed work, the key, the batch and the GL calls were already skipped for them.
//  * the game does NOT draw those triangles one call at a time out of nowhere: it walks DISPLAY LISTS (a function at 0x0044FF20 with a header {array of record pointers, x offset, y offset, clip rectangle}); every record is a command
//    (opcode, operands) that a small helper turns into ONE Glide triangle. The coordinates of the triangle are plain 24.8 fixed-point integers inside the record (+ the list's offset).
// WHAT THIS HEADER DOES: before the dispatcher walks a list, scan its records natively (a few ns each) and turn every triangle command whose three vertices are beyond the SAME edge of the GL viewport (the predicate of G146, with 2 more
//    pixels of margin) into the dispatcher's own "do nothing" opcode; the dispatcher then skips it exactly as it skips any unknown opcode. When the list is done the opcodes are put back (a second hook at the end of the dispatcher).
// WHY IT IS EXACT (proved by tools/g156_test.cpp with the machine code of Ign_3dfx.exe executed natively and a model of the Glide state):
//  * the triangle a command draws is exactly the one its record says (all 12 opcodes checked: the drawn vertices are the record's), and a triangle wholly beyond one edge of the GL viewport produces no fragment (G146);
//  * the game keeps its Glide draw state behind per-field caches that only the helpers write; every helper sets every field its output depends on (or relies on a field no helper leaves different), so skipping a whole command leaves the
//    state EVERY LATER DRAWN TRIANGLE SEES unchanged: for random lists of every opcode, with and without the cull, the drawn triangles and the full Glide state at each of them are identical, and so is the state any later list starts from.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "win32/g146_geo.h"

namespace ign { namespace g156 {

namespace G5 = ign::g145;

// ---- the guest code this was written from (Ign_3dfx.exe; the runtime refuses to patch anything else: FNV-1a of [kCodeLo, kCodeHi) must be kCodeFnv) -----------------------------------------------------------------------
constexpr uint32_t kEntry = 0x0044FF20u;        // function entry: push ebx; mov ebx,[esp+8]   (5 bytes: 53 8B 5C 24 08)
constexpr uint32_t kEntryResume = 0x0044FF25u;  // the instruction after them
constexpr uint32_t kExit = 0x00450534u;         // the common exit: pop edi; pop esi; pop ebp; pop ebx; ret   (5 bytes: 5F 5E 5D 5B C3)
constexpr uint32_t kCodeLo = 0x0044FF20u, kCodeHi = 0x00453090u;
constexpr uint64_t kCodeFnv = 0x65aa4cdb54b33a55ull;   // FNV-1a of the original bytes of [kCodeLo, kCodeHi) (tools/g156_test.cpp prints it for the executable it is given)
constexpr uint32_t kNopOpcode = 3u;             // a record whose opcode maps to the dispatcher's "next record" case (checked by the test against the real jump tables)
constexpr uint32_t kMaxRecords = 16384u;        // a list longer than this is not scanned (and everything done so far is undone)
constexpr uint32_t kJournalCap = 12288u;        // records patched at the same time across nested lists
constexpr float kExtraMarginPx = 2.0f;          // on top of G146's 4 px: the guest rounds nothing (positions are integers) but the host snaps to 1/16 px and maps through floats

enum Layout : uint8_t { kLayoutNone = 0, kLayoutTri16, kLayoutFlat6 };
// the dispatcher's jump table maps these opcodes to helpers that draw exactly ONE Glide triangle from three (x, y) pairs of the record:
//  Tri16 (textured / keyed / materials): x0,y0 at +0x04,+0x08; x1,y1 at +0x18,+0x1C; x2,y2 at +0x2C,+0x30 (vertex stride 0x14);   Flat6 (flat colour): x0,y0,x1,y1,x2,y2 at +0x04..+0x18
inline Layout layout_of(uint32_t op) {
    switch (op) {
        case 0x11: case 0x12: case 0x13: case 0x18: case 0x75: case 0x76: case 0x77: case 0x7C: return kLayoutTri16;
        case 0x0F: case 0x14: case 0x1C: case 0x1D: return kLayoutFlat6;
        default: return kLayoutNone;
    }
}
// the track callers of G145 (is_track_caller): the helpers of opcodes 0x0F, 0x11 and 0x12 (their return addresses are 0x450C88, 0x451336, 0x451618)
inline bool is_track_opcode(uint32_t op) { return op == 0x0F || op == 0x11 || op == 0x12; }

// ---- the fixed-point arithmetic of the helpers: cdq; and edx,0xff; add eax,edx; sar eax,8  (an integer division by 256 rounding toward zero) --------------------------------------------------------------------------------
inline int32_t trunc256(int32_t v) { return (int32_t)(((uint32_t)v + (uint32_t)((v >> 31) & 0xFF))) >> 8; }

struct Params {                      // everything the predicate needs (set once per list)
    int32_t vp[4] = {0, 0, 0, 0};    // the GL viewport the draw path uses
    int32_t logicalW = 0, logicalH = 0;   // Glide screen size (g_glideContext.width / height)
    bool valid() const { return vp[2] > 0 && vp[3] > 0 && logicalW > 0 && logicalH > 0; }
};
// physical window coordinates of a logical vertex: the SAME expressions as glide_logical_xy_to_physical (glide2x_ign.cpp)
inline void to_physical(const Params& p, float lx, float ly, float& ox, float& oy) {
    const float sx = (p.logicalW > 0) ? (float)p.vp[2] / (float)p.logicalW : 1.0f;
    const float sy = (p.logicalH > 0) ? (float)p.vp[3] / (float)p.logicalH : 1.0f;
    ox = (float)p.vp[0] + lx * sx;
    oy = (float)p.vp[1] + ((float)p.vp[3] - ly * sy);
}
struct Verdict { bool cull = false; G5::Plane plane = G5::kNotDecidable; bool exact = false; };
// xi / yi = the integer parts the helper passes on (trunc256 of coordinate + list offset). cull = beyond one edge with margin (4 + 2 px); plane / exact = what the host's level 0 will say / count for it
inline Verdict judge(const Params& p, const int32_t xi[3], const int32_t yi[3]) {
    Verdict v;
    if (!p.valid()) return v;
    for (int i = 0; i < 3; ++i) { if (xi[i] > 262143 || xi[i] < -262143 || yi[i] > 262143 || yi[i] < -262143) return v; }   // far away or wrapped: the host decides
    float ax, ay, bx, by, cx, cy;
    to_physical(p, (float)xi[0], (float)yi[0], ax, ay); to_physical(p, (float)xi[1], (float)yi[1], bx, by); to_physical(p, (float)xi[2], (float)yi[2], cx, cy);
    const ign::g146::Rect r{(float)p.vp[0], (float)p.vp[1], (float)p.vp[0] + (float)p.vp[2], (float)p.vp[1] + (float)p.vp[3]};
    const G5::Plane host = ign::g146::outside_rect(r, ign::g146::kViewportMarginPx, ax, ay, bx, by, cx, cy);
    if (!G5::is_reject(host)) return v;
    const G5::Plane wide = ign::g146::outside_rect(r, ign::g146::kViewportMarginPx + kExtraMarginPx, ax, ay, bx, by, cx, cy);
    if (!G5::is_reject(wide)) return v;
    v.cull = true; v.plane = host; v.exact = ign::g146::outside_rect(r, 0.0f, ax, ay, bx, by, cx, cy) != G5::kInside;
    return v;
}

// ---- why a list was not (completely) scanned ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum Why : uint8_t { kWhyNone = 0, kWhyHeaderUnreadable, kWhyArrayUnreadable, kWhyRecordUnreadable, kWhyTooLong, kWhyJournalFull, kWhyViewport, kWhyNotEligible, kWhyDisabled, kWhyNested, kWhyExitMismatch, kWhyInvalidTexture, kWhyCount };
inline const char* why_name(uint32_t w) {
    static const char* const n[kWhyCount] = {"none", "header_unreadable", "record_array_unreadable", "record_unreadable", "list_too_long", "journal_full", "viewport_invalid", "rejection_not_eligible", "switched_off", "nested_list", "exit_without_entry", "textured_command_with_invalid_texture_index"};
    return w < kWhyCount ? n[w] : "?";
}
struct JEnt { uint32_t rec, op; };
struct ListStats {                  // what ONE list did
    uint32_t records = 0, tri16 = 0, flat6 = 0, culled = 0, culledTri16 = 0, culledFlat6 = 0, culledTrack = 0;
    uint32_t plane[5] = {0, 0, 0, 0, 0}, exact = 0;   // by G5::Plane value (host's classification of the culled triangles), and how many are outside the viewport with no margin at all
    uint32_t why = kWhyNone;
};
// Reads the list at `header` and patches the opcode of every culled record to kNopOpcode, journalling (record, original opcode). Returns the number of journal entries written (also in st.culled).
// Two passes: the first one decides (and refuses the whole list when a textured command has a texture index outside 0..255: that helper takes a debug path that does not set the blend itself and so depends on what the commands before it
// left behind - exactly what a cull would change), the second one patches. M::rspan(va, n) -> const uint8_t* (null when [va, va + n) is not readable), M::wspan(va, n) -> uint8_t* (null when not writable)
template <class M>
inline uint32_t cull_list(M& m, uint32_t header, const Params& p, JEnt* journal, uint32_t cap, ListStats& st) {
    st = ListStats();
    const uint8_t* h = m.rspan(header, 0x20);
    if (!h) { st.why = kWhyHeaderUnreadable; return 0; }
    uint32_t arr, ox, oy; memcpy(&arr, h, 4); memcpy(&ox, h + 4, 4); memcpy(&oy, h + 8, 4);
    const int32_t offX = (int32_t)(ox << 8), offY = (int32_t)(oy << 8);   // shl esi,8 / shl edi,8
    uint32_t n = 0, i = 0; bool taint = false;
    for (; i < kMaxRecords; ++i) {
        const uint8_t* pp = m.rspan(arr + 4u * i, 4);
        if (!pp) { st.why = kWhyArrayUnreadable; break; }
        uint32_t rec; memcpy(&rec, pp, 4);
        if (rec == 0u) { st.records = i; goto scanned; }
        {
            const uint8_t* r0 = m.rspan(rec, 4);
            if (!r0) { st.why = kWhyRecordUnreadable; break; }
            uint32_t op; memcpy(&op, r0, 4);
            const Layout lay = layout_of(op);
            if (lay == kLayoutNone) continue;
            const uint8_t* r = m.rspan(rec + 4u, lay == kLayoutTri16 ? 0x40u : 0x18u);
            if (!r) { st.why = kWhyRecordUnreadable; break; }
            int32_t xi[3], yi[3]; uint32_t w[6];
            if (lay == kLayoutTri16) {
                uint32_t tex; memcpy(&tex, r + 0x3C, 4); if (tex >= 256u) taint = true;
                memcpy(&w[0], r + 0x00, 4); memcpy(&w[1], r + 0x04, 4); memcpy(&w[2], r + 0x14, 4); memcpy(&w[3], r + 0x18, 4); memcpy(&w[4], r + 0x28, 4); memcpy(&w[5], r + 0x2C, 4); ++st.tri16;
            } else { for (int k = 0; k < 6; ++k) memcpy(&w[k], r + 4 * k, 4); ++st.flat6; }
            for (int k = 0; k < 3; ++k) { xi[k] = trunc256((int32_t)(w[2 * k] + (uint32_t)offX)); yi[k] = trunc256((int32_t)(w[2 * k + 1] + (uint32_t)offY)); }
            const Verdict v = judge(p, xi, yi);
            if (!v.cull) continue;
            if (n >= cap) { st.why = kWhyJournalFull; break; }
            journal[n].rec = rec; journal[n].op = op; ++n;
            ++st.culled; if (lay == kLayoutTri16) ++st.culledTri16; else ++st.culledFlat6;
            if (is_track_opcode(op)) ++st.culledTrack;
            ++st.plane[(int)v.plane < 5 ? (int)v.plane : 0]; if (v.exact) ++st.exact;
        }
    }
    if (st.why == kWhyNone) st.why = kWhyTooLong;
    st.records = i;
scanned:
    if (taint || st.why != kWhyNone) {   // an anomaly, or a list that could not be read to its end: it is left exactly as the game wrote it
        const ListStats keep = st; st = ListStats(); st.records = keep.records; st.tri16 = keep.tri16; st.flat6 = keep.flat6; st.why = taint ? (uint32_t)kWhyInvalidTexture : keep.why; return 0;
    }
    {
        uint32_t done = 0;
        for (uint32_t k = 0; k < n; ++k) {
            uint8_t* wp = m.wspan(journal[k].rec, 4);
            if (!wp) { st.why = kWhyRecordUnreadable; journal[k].rec = 0; continue; }
            const uint32_t nop = kNopOpcode; memcpy(wp, &nop, 4); ++done;
        }
        (void)done;
    }
    return n;
}// Puts the original opcodes back (newest first). A record the game has rewritten in the meantime (its opcode is no longer ours) is left alone. Returns how many were restored.
template <class M>
inline uint32_t restore(M& m, const JEnt* journal, uint32_t n) {
    uint32_t done = 0;
    for (uint32_t i = n; i-- > 0;) {
        if (journal[i].rec == 0u) continue;
        uint8_t* wp = m.wspan(journal[i].rec, 4);
        if (!wp) continue;
        uint32_t cur; memcpy(&cur, wp, 4);
        if (cur != kNopOpcode) continue;
        const uint32_t op = journal[i].op; memcpy(wp, &op, 4); ++done;
    }
    return done;
}

// the control file token that switches the cull off (A/B measurement against G155): g156=off
inline bool off_from_text(const char* text) {
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == 8 && memcmp(p, "g156=off", 8) == 0) return true;
        p = e;
    }
    return false;
}

// ---- the nested entry / exit bookkeeping (static storage in the caller): the frame of a list is found again at its exit by the stack pointer ---------------------------------------------------------------------------------
constexpr uint32_t kMaxFrames = 8;
struct Frames {
    uint32_t key[kMaxFrames]; uint32_t first[kMaxFrames], count[kMaxFrames]; uint32_t depth = 0;
    bool push(uint32_t k, uint32_t f, uint32_t c) { if (depth >= kMaxFrames) return false; key[depth] = k; first[depth] = f; count[depth] = c; ++depth; return true; }
    // finds the frame for `k` from the top; frames above it (a missing exit) are returned through `above` so the caller can restore them too
    int find(uint32_t k) const { for (uint32_t i = depth; i-- > 0;) if (key[i] == k) return (int)i; return -1; }
};

// ---- counters and the summary line ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Stats {
    uint64_t lists = 0, listsWithCull = 0, listsSkipped = 0, records = 0, tri16 = 0, flat6 = 0, culled = 0, culledTri16 = 0, culledFlat6 = 0, culledTrack = 0, plane[5] = {0, 0, 0, 0, 0}, exact = 0;
    uint64_t why[kWhyCount] = {};
    uint64_t scanUs = 0, scanSamples = 0, scanSampledLists = 0, restored = 0, hooksEnter = 0, hooksExit = 0;
    uint32_t installed = 0;
};
inline void add(Stats& s, const ListStats& l) {
    ++s.lists; s.records += l.records; s.tri16 += l.tri16; s.flat6 += l.flat6; s.culled += l.culled; s.culledTri16 += l.culledTri16; s.culledFlat6 += l.culledFlat6; s.culledTrack += l.culledTrack;
    for (int i = 0; i < 5; ++i) s.plane[i] += l.plane[i];
    s.exact += l.exact; if (l.culled) ++s.listsWithCull;
    if (l.why != kWhyNone) { ++s.why[l.why < kWhyCount ? l.why : 0u]; }
}
// the estimate of what one culled command used to cost the whole machine, from the G155 real run (see tmp/G156_HEAVY_ZONE_ANALYSIS.txt): ~10 us for the triangle call + 1.7 state calls x ~7 us. NOT a measurement of G156.
constexpr double kEstUsPerCulledCommand = 21.0, kEstStateCallsPerCulledCommand = 1.7;
inline size_t why_list(char* out, size_t cap, const uint64_t* w) {
    size_t p = 0; out[0] = 0; bool any = false;
    for (uint32_t i = 1; i < kWhyCount; ++i) if (w[i] && p + 64 < cap) { p += (size_t)snprintf(out + p, cap - p, "%s%s=%llu", any ? " " : "", why_name(i), (unsigned long long)w[i]); any = true; }
    if (!any) snprintf(out, cap, "none");
    return strlen(out);
}
inline int format_summary(char* out, size_t n, const char* tag, const Stats& s, const Stats& base, const char* track, bool final) {
    char why[360]; uint64_t wd[kWhyCount]; for (uint32_t i = 0; i < kWhyCount; ++i) wd[i] = s.why[i] - base.why[i]; why_list(why, sizeof why, wd);
    const uint64_t lists = s.lists - base.lists, records = s.records - base.records, tri16 = s.tri16 - base.tri16, flat6 = s.flat6 - base.flat6, culled = s.culled - base.culled, c16 = s.culledTri16 - base.culledTri16, c6 = s.culledFlat6 - base.culledFlat6,
                   ctrack = s.culledTrack - base.culledTrack, withCull = s.listsWithCull - base.listsWithCull, ex = s.exact - base.exact, scanUs = s.scanUs - base.scanUs, samples = s.scanSamples - base.scanSamples, restored = s.restored - base.restored;
    const uint64_t targets = tri16 + flat6;
    return snprintf(out, n,
        "%s %s | groups{lists=%llu with_cull=%llu records_scanned=%llu triangle_commands=%llu (textured=%llu flat=%llu)} | culled_before_any_work{commands=%llu (textured=%llu flat=%llu track_family=%llu) = %.2f%% of the triangle commands; by_edge{left=%llu right=%llu bottom=%llu top=%llu} outside_viewport_no_margin=%llu} | "
        "avoided{triangle_calls=%llu guest_helper_runs=%llu vertex_decodes=%llu keyed_queries<=%llu batch_keys=0(already rejected in G155) state_calls_est=%.0f time_est_ms=%.1f (a model from the G155 run, NOT a measurement)} | classify{sampled_ms=%.3f us_per_list(sampled %llu lists)=%.1f est_total_ms=%.1f restored=%llu} | not_scanned{%s}",
        final ? "FINAL" : "RACE", track ? track : "", (unsigned long long)lists, (unsigned long long)withCull, (unsigned long long)records, (unsigned long long)targets, (unsigned long long)tri16, (unsigned long long)flat6, (unsigned long long)culled, (unsigned long long)c16, (unsigned long long)c6, (unsigned long long)ctrack,
        targets ? 100.0 * (double)culled / (double)targets : 0.0, (unsigned long long)(s.plane[G5::kLeft] - base.plane[G5::kLeft]), (unsigned long long)(s.plane[G5::kRight] - base.plane[G5::kRight]), (unsigned long long)(s.plane[G5::kBottom] - base.plane[G5::kBottom]), (unsigned long long)(s.plane[G5::kTop] - base.plane[G5::kTop]), (unsigned long long)ex,
        (unsigned long long)culled, (unsigned long long)culled, (unsigned long long)culled, (unsigned long long)culled, (double)culled * kEstStateCallsPerCulledCommand, (double)culled * kEstUsPerCulledCommand / 1000.0,
        (double)scanUs / 1000.0, (unsigned long long)samples, samples ? (double)scanUs / (double)samples : 0.0, samples ? (double)scanUs / (double)samples * (double)lists / 1000.0 : 0.0, (unsigned long long)restored, why);
}

// ---- the two hooks' state machine (pure: the runtime shims and the host test drive the same code) -----------------------------------------------------------------------------------------------------------------------------
// enter(): the dispatcher was just called for `header` (key = the stack pointer at the call: the return address is at [key]); the matching exit() finds the frame again with the same key (esp at the common exit + 16).
// `notEligible` != 0 is the reason the rejection of triangles is not available right now (nothing is culled, the frame is still pushed so the exit matches).
class Hooker {
public:
    Stats st; ListStats last;
    template <class M>
    void enter(M& m, uint32_t key, uint32_t header, const Params& p, uint32_t notEligible) {
        ++st.hooksEnter;
        ListStats ls; uint32_t n = 0;
        if (notEligible != kWhyNone) { ls.why = notEligible; }
        else if (tainted_) { ls.why = kWhyInvalidTexture; }   // a command with an invalid texture index was seen once: the game takes a path that depends on what the commands before it left, so nothing is culled any more (the exposure is that one list)
        else if (fr_.depth >= kMaxFrames) { ls.why = kWhyNested; }
        else if (!p.valid()) { ls.why = kWhyViewport; }
        else if (jn_ + 1u >= kJournalCap) { ls.why = kWhyJournalFull; }
        else n = cull_list(m, header, p, &journal_[jn_], kJournalCap - jn_, ls);
        if (fr_.depth < kMaxFrames) fr_.push(key, jn_, n);
        jn_ += n; last = ls; add(st, ls); if (ls.why == kWhyInvalidTexture) tainted_ = true;
    }
    template <class M>
    void exit(M& m, uint32_t key) {
        ++st.hooksExit;
        const int at = fr_.find(key);
        if (at < 0) { ++st.why[kWhyExitMismatch]; return; }
        while ((int)fr_.depth > at) {              // frames above `at` lost their exit: they are undone too
            const uint32_t d = fr_.depth - 1u; st.restored += restore(m, &journal_[fr_.first[d]], fr_.count[d]); jn_ = fr_.first[d]; fr_.depth = d;
        }
    }
    // a safety net for the places where no list can be active (frame boundary, race change): everything still patched is put back
    template <class M>
    uint32_t restore_all(M& m) {
        if (fr_.depth == 0 && jn_ == 0) return 0;
        const uint32_t pending = fr_.depth; st.restored += restore(m, journal_, jn_); jn_ = 0; fr_.depth = 0; if (pending) ++st.why[kWhyExitMismatch];
        return pending;
    }
    void new_race() { tainted_ = false; }   // a race starts: what the lists before it left was produced by lists that were never culled (nothing is eligible outside a race)
    bool tainted() const { return tainted_; }
    uint32_t depth() const { return fr_.depth; }
    uint32_t journaled() const { return jn_; }
private:
    Frames fr_; JEnt journal_[kJournalCap]; uint32_t jn_ = 0; bool tainted_ = false;
};
} }   // namespace ign::g156
