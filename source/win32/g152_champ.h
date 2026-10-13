// win32/g152_champ.h -- iteration-G152, PURE (no GL, no Vita API, no guest access, no logging, no heap, no global state; tools/g152_test.cpp drives exactly this code).
//
// WHY THE FIRST CHAMPIONSHIP RACE WAS NOT A RACE (G151 real run, F:\data\Ignition\logs, tmp/G151_CHAMP_REAL_RUN_LOGS):
//  * G145's lifecycle leaves LOADING for RACE_SETTLING only after two consecutive swaps with race geometry AND NO CAR CAROUSEL (g145_life.h: geom = trackTris > 0 && anyTris && !carousel).
//  * "Car carousel" is G125's flag `g_g125.active`: the game's current menu node [0x0049C320] is one of the two car-selection nodes of the language tree ([0x0049069C] / [0x004906A0]).
//  * In the four normal races the node has left the car-selection node when the race starts (carousel=0 at every LOADING -> RACE_SETTLING of races 1-4). In the Championship race the log shows the node STILL being the
//    car-selection node 0x0047A458: [G145-RACE-LIFECYCLE] RESULTS -> LOADING says carousel=1 at the load of race#5 (the others say 0), and [G140-LANG-NODE-ENTER] entry#6 (in the Championship race) has prev_node=0x0047A458
//    where entries #2..#5 (normal races) have prev_node=0x0047A428. With the flag set the lifecycle never saw race geometry => LOADING for the whole race ("phase=RACE(non-race)"), and every consumer of the flag
//    (G143 mode machine, G144 arbiter, G146 rejection, G148 deferral, G138 classification, the compositor's HUD gate) took the general path: 12.6 FPS, compositor 8.35 ms, 195.8 kB/frame uploaded.
//  * The game's own signal for "a race frame is being produced": the main loop 0x00411330 runs its race-frame body (0x0041149F ..) only while [0x00509D2C] == 1 (`cmp [0x509d2c],ebx ; jne 0x411d17` at 0x00411493, ebx = 1 for the whole function). The word
//    is set to 1 once, at the END of the track load (0x00411436), and cleared at the end of the race (0x00411D09 / 0x00411D38 / 0x00411D5A), at the start of a load (0x00411473, 0x00411454) and at init (0x0041130B). Nothing
//    in the menu code writes it. So "the car-selection node is current AND the race loop word is 1 AND a track load with its .TRI/.SRF has happened" cannot be a car selection: the game is running a race inside that node.
//  G152 changes ONE thing: the carousel flag is held OFF while that holds. Everything else (the lifecycle, the arbiter, the G143-G149 code) reads the flag exactly as before. If the evidence is not there (word != 1, no load) the
//  flag is what it was in G151. The detector counts what it saw and reports it (small lines, never per frame).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g152 {

constexpr uint32_t kAddrRaceLoop = 0x00509D2Cu;        // the race-frame guard of the game loop
constexpr uint32_t kAddrMenuNode = 0x0049C320u;        // the current menu node
constexpr uint32_t kStuckSwaps = 120u;                 // swaps in LOADING with race geometry and the carousel flag set before the rejection is reported (once per load)

// the guest words that are logged next to the decision (evidence only, never a condition except [0x00509D2C])
constexpr uint32_t kWordCount = 12;
constexpr uint32_t kWordAddr[kWordCount] = {0x00537180u, 0x00509D2Cu, 0x0049BBC0u, 0x005371A0u, 0x00547AFCu, 0x00536DF0u, 0x00536DF8u, 0x00536E08u, 0x0061D3A4u, 0x00547B30u, 0x00547C8Cu, 0x0053385Cu};
constexpr const char* kWordName[kWordCount] = {"load_step[537180]", "race_loop[509D2C]", "pause_guard[49BBC0]", "pause_flag[5371A0]", "finish[547AFC]", "mode_a[536DF0]", "mode_b[536DF8]", "load_var[536E08]", "game_a[61D3A4]", "game_b[547B30]", "game_c[547C8C]", "hud_var[53385C]"};

enum Verdict : uint8_t { kNoCarouselNode = 0, kRealCarousel, kRaceUnderCarouselNode, kLoopWithoutLoadEvidence, kVerdictCount };
inline const char* verdict_name(uint32_t v) {
    switch (v) { case kNoCarouselNode: return "node_is_not_car_selection"; case kRealCarousel: return "car_selection(race_loop_word!=1)"; case kRaceUnderCarouselNode: return "RACE_UNDER_CAR_SELECTION_NODE";
                 case kLoopWithoutLoadEvidence: return "race_loop_word==1_without_a_track_load(rejected)"; default: return "?"; }
}
// rawCarousel: the node is a car-selection node (G125). loadSeq / triSrfOpened: structural track loads seen / the .TRI or .SRF of the load in progress was opened. loopWord: [0x00509D2C].
inline Verdict classify(bool rawCarousel, uint32_t loadSeq, bool triSrfOpened, uint32_t loopWord) {
    if (!rawCarousel) return kNoCarouselNode;
    if (loopWord != 1u) return kRealCarousel;
    if (loadSeq == 0u || !triSrfOpened) return kLoopWithoutLoadEvidence;
    return kRaceUnderCarouselNode;
}
inline bool carousel_effective(Verdict v) { return v == kRealCarousel || v == kLoopWithoutLoadEvidence; }

struct Evidence {
    uint32_t swap = 0, loadSeq = 0, node = 0; bool triSrf = false; uint32_t words[kWordCount] = {};
};

class Detector {
public:
    struct Out { Verdict v = kNoCarouselNode; bool carousel = false, episodeStart = false, episodeEnd = false; };
    // once per swap (G125's swap-begin). `word` is only meaningful (and only read from the guest) when `rawCarousel`.
    Out feed(bool rawCarousel, uint32_t loadSeq, bool triSrfOpened, uint32_t word, uint32_t swap) {
        if (loadSeq != lastLoad_) { lastLoad_ = loadSeq; ovLoad_ = 0; stuck_ = 0; stuckLogged_ = false; }
        Out o; o.v = classify(rawCarousel, loadSeq, triSrfOpened, word); o.carousel = carousel_effective(o.v); last_ = o.v;
        if (rawCarousel) ++rawSwaps_;
        if (o.v == kRaceUnderCarouselNode) {
            ++ovSwaps_; ++ovLoad_;
            if (!ovNow_) { o.episodeStart = true; ++episodes_; firstSwap_ = swap; }
            ovNow_ = true;
        } else {
            if (ovNow_) { o.episodeEnd = true; lastEndSwap_ = swap; }
            ovNow_ = false;
            if (o.v == kLoopWithoutLoadEvidence) ++noEvidence_;
        }
        return o;
    }
    // once per swap (the lifecycle feed). A LOAD THAT DOES NOT BECOME A RACE: LOADING for kStuckSwaps swaps with race geometry in front of us while the carousel flag is still set (kind 1: the detector did not recognise a
    // race), or with the flag held off but no geometry from the known track callers (kind 2: the race loop runs but the lifecycle's evidence is missing). True once per load.
    bool note_loading(bool loading, bool carousel, bool geometry, bool overrideNow = false) {
        const uint32_t kind = (carousel && geometry) ? 1u : (overrideNow && !geometry) ? 2u : 0u;
        if (loading && kind) { if (++stuck_ >= kStuckSwaps && !stuckLogged_) { stuckLogged_ = true; ++stuckReports_; stuckKind_ = kind; return true; } }
        else stuck_ = 0;
        return false;
    }
    uint32_t stuck_kind() const { return stuckKind_; }    bool override_now() const { return ovNow_; }
    uint32_t override_this_load() const { return ovLoad_; }
    uint64_t raw_swaps() const { return rawSwaps_; }
    uint64_t override_swaps() const { return ovSwaps_; }
    uint64_t no_evidence_swaps() const { return noEvidence_; }
    uint32_t episodes() const { return episodes_; }
    uint32_t stuck_reports() const { return stuckReports_; }
    uint32_t stuck_swaps() const { return stuck_; }
    uint32_t first_swap() const { return firstSwap_; }
    Verdict last_verdict() const { return last_; }
private:
    uint32_t lastLoad_ = 0, ovLoad_ = 0, stuck_ = 0, episodes_ = 0, stuckReports_ = 0, firstSwap_ = 0, lastEndSwap_ = 0; bool ovNow_ = false, stuckLogged_ = false; Verdict last_ = kNoCarouselNode; uint32_t stuckKind_ = 0;
    uint64_t rawSwaps_ = 0, ovSwaps_ = 0, noEvidence_ = 0;
};

inline int format_words(char* out, size_t n, const Evidence& e) {
    int len = snprintf(out, n, "node[0x0049C320]=0x%08x load_seq=%u tri_srf_opened=%d", e.node, e.loadSeq, e.triSrf ? 1 : 0);
    for (uint32_t i = 0; i < kWordCount && len > 0 && (size_t)len + 48 < n; ++i) len += snprintf(out + len, n - (size_t)len, " %s=%u", kWordName[i], e.words[i]);
    return len;
}
// [G152-CHAMP-RACE]: written once, at the LOADING -> RACE_SETTLING of a race that the detector recognised
inline int format_champ_race(char* out, size_t n, uint32_t raceNo, uint32_t entrySwap, uint32_t structuralLoad, const char* track, const char* dir, const Evidence& e, uint32_t overrideSwaps, const char* from, const char* to) {
    char w[420]; format_words(w, sizeof w, e);
    return snprintf(out, n, "race#%u structural_load=%u track=\"%s\" dir=%s signal=race_loop_word[0x00509D2C]==1 while the menu node is the car-selection node (the game runs its race-frame body inside that node: the carousel flag is held off) | evidence={%s} | first_override_swap=%u override_swaps_before_entry=%u entry_swap=%u | %s -> %s",
                    raceNo, structuralLoad, track ? track : "?", dir ? dir : "?", w, e.swap, overrideSwaps, entrySwap, from, to);
}
// [G152-CHAMP-RACE-END]: at the end of such a race
inline int format_champ_end(char* out, size_t n, uint32_t raceNo, uint32_t entrySwap, uint32_t leaveSwap, bool armed, uint32_t fast, uint32_t full, uint32_t scanned, uint32_t pauses, uint64_t overrideSwaps, const char* exitState) {
    return snprintf(out, n, "race#%u entry_swap=%u leave_swap=%u fastpath_armed=%d scanned=%u fast=%u(%.1f%%) full_path=%u pauses=%u override_swaps_total=%llu | exit: RACE -> %s (the carousel flag returns when the game clears the race loop word)",
                    raceNo, entrySwap, leaveSwap, armed ? 1 : 0, scanned, fast, scanned ? 100.0 * (double)fast / (double)scanned : 0.0, full, pauses, (unsigned long long)overrideSwaps, exitState);
}
// [G152-CHAMP-REJECT]: LOADING with race geometry and the carousel flag set for kStuckSwaps swaps: the detector did not recognise a race; everything it saw
inline const char* stuck_reason(uint32_t kind) { return kind == 1u ? "race geometry is being drawn but the car-carousel flag is still set" : kind == 2u ? "the race loop word is 1 (flag held off) but no triangle came from the known track display-list callers" : "?"; }
inline int format_champ_reject(char* out, size_t n, uint32_t structuralLoad, uint32_t stuckSwaps, uint32_t kind, Verdict last, const Evidence& e, uint64_t rawSwaps, uint64_t overrideSwaps, uint64_t noEvidence) {
    char w[420]; format_words(w, sizeof w, e);
    return snprintf(out, n, "structural_load=%u: %u swaps in LOADING and the lifecycle has not entered the race: %s | the detector says %s (race_loop_word must be 1 and the .TRI/.SRF of the load must have been opened) | detector{raw_carousel_swaps=%llu override_swaps=%llu loop_word_without_load=%llu} | evidence={%s} | NOT treated as a race",
                    structuralLoad, stuckSwaps, stuck_reason(kind), verdict_name(last), (unsigned long long)rawSwaps, (unsigned long long)overrideSwaps, (unsigned long long)noEvidence, w);
}
// [G152-CHAMP-SESSION] / [G152-CHAMP-SESSION-FINAL]
inline int format_champ_session(char* out, size_t n, uint32_t champRaces, uint32_t episodes, uint64_t rawSwaps, uint64_t overrideSwaps, uint64_t noEvidence, uint32_t stuckReports) {
    return snprintf(out, n, "championship_races_recognised=%u override_episodes=%u carousel_node_swaps=%llu of_which_race_loop_running=%llu loop_word_without_load_evidence=%llu detector_rejections=%u",
                    champRaces, episodes, (unsigned long long)rawSwaps, (unsigned long long)overrideSwaps, (unsigned long long)noEvidence, stuckReports);
}

} }   // namespace ign::g152
