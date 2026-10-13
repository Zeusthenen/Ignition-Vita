// DirectSound emulation: guest COM shell + mixer.
// See ds_emul.h for the design. Nothing here runs unless D2_SON is set, except
// allocation of the 32 trap slots (touches no guest memory and keeps slot
// numbering identical across A/B runs).
#include "runtime/ds_emul.h"
#include "runtime/guest_scratch.h"
#include "runtime/audio_sink.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/gil.h"
#include "runtime/host_clock.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace d2rt { namespace dsound {

namespace {

// ---- stream constants -------------------------------------------------------
// Mixed-stream frequency, provided by the host (HostOps::mix_rate) since it's
// a property of the game's own samples. 0 means unset; install() then refuses
// to arm rather than guess (see HostOps::mix_rate in ds_emul.h).
//
// kRate is never used as an array bound (only kGrain is): being a runtime
// variable instead of a constexpr only changes divisions to use a loaded
// integer instead of an immediate, outside the per-frame loop.
int kRate = 0;
constexpr int kOutCh = 2;
constexpr int kGrain = 512;          // 23.2 ms — 43 wakeups/s, ~46 ms latency

constexpr uint32_t DS_OK              = 0x00000000u;
constexpr uint32_t DSERR_NODRIVER     = 0x88780078u;
constexpr uint32_t DSERR_INVALIDPARAM = 0x80070057u;
constexpr uint32_t E_NOINTERFACE      = 0x80004002u;
constexpr uint32_t E_NOTIMPL          = 0x80004001u;

constexpr uint32_t MAGIC_DEV = 0x56445344u;   // 'DSDV'
constexpr uint32_t MAGIC_BUF = 0x46425344u;   // 'DSBF'

// ---- state -------------------------------------------------------------------
struct Voice {
    uint32_t obj = 0;            // guest COM object (16 B scratch)
    uint32_t buf = 0;            // guest PCM buffer (VA arena)
    uint32_t cap = 0;            // bytes actually allocated (for reuse)
    uint32_t len = 0;            // buffer size as seen by the game
    uint32_t cursor = 0;         // read cursor, in bytes
    uint64_t posAcc = 0;         // resampling remainder
    uint32_t flags = 0;
    int      ch = 2, bits = 16;
    uint32_t rate = kRate;
    uint32_t blockAlign = 4;
    bool     primary = false;
    bool     playing = false, looping = false;
    bool     alive = false;
    int32_t  volmB = 0, panmB = 0;
    uint32_t gL = 65536, gR = 65536;
    uint32_t refs = 0;
    const uint8_t* host = nullptr;   // flat host view of the guest buffer (Box86)
    // A voice created by DuplicateSoundBuffer SHARES its original's sample
    // data (DirectSound contract: same bytes, independent cursor and gains).
    // It does NOT own `buf`, so releasing it must never return that buffer to
    // the reuse pool — the original is still reading it.
    bool     ownsBuf = true;
    uint32_t shares = 0;         // owner only: live duplicates of this buffer
    size_t   owner = (size_t)-1; // duplicate only: owner's slot index
    // KiB locked by this voice. A single global total couldn't answer "is the
    // stream pump feeding the 256 KiB rings?" — the only question that tells
    // apart "no audio out" from "audio never written". Published as flux=.
    uint64_t lockKio = 0;
    bool     volSaid = false;    // first SetVolume logged (verbose log)
    // ---- iteration-G130: freshness of a STREAMING ring --------------------------------------
    // A looping buffer the game keeps refilling behind the play cursor (Ignition: a 1 s ring, 10 ms
    // Locks at its own write pointer) is a stream. Its content is only valid where the game has
    // written since the cursor last passed. If the game falls behind (load, long frame), the old
    // behaviour -- like real hardware -- replays the previous turn's stale audio, which is what made
    // effects/music repeat or arrive seconds late. For a stream, bytes the game did not write for the
    // upcoming cursor position are now mixed as SILENCE. The cursor itself is untouched (it still runs
    // at the sample rate), so the game's own timing, A/V sync and window logic are exactly as before.
    //   fresh[i]  1 = block i (blockAlign bytes) was Locked by the game since the cursor last passed it.
    //             Set by Lock, cleared by the mixer as the cursor consumes it. Exact: no heuristics about
    //             where the game "should" write.
    bool     stream = false;     // a partial Lock was seen while playing
    // One flag per blockAlign bytes of the ring, stored between two guard bands of kFreshGuard canary bytes
    // (0xA5). `freshN` is the flag count; the flag of block q lives at fresh[kFreshGuard + q]. Every index is
    // reduced modulo freshN or bounded by it; the guards exist so a stray write would be CAUGHT (detail_line,
    // `canary=` / `flagbad=`), not to be relied on. Allocated when `stream` turns on, dropped on reuse.
    std::vector<uint8_t> fresh;
    uint32_t freshN = 0;
};
constexpr uint32_t kFreshGuard = 8;

// Engine setting, with fallback to the port's older env var name (see install()).
inline const char* env2(const char* neuf, const char* ancien) {
    const char* v = getenv(neuf); return v ? v : getenv(ancien);
}

// Logging: formatted here, the host decides where it goes (see set_logger).
LogFn g_logger = nullptr;
ExtraStatFn g_extra = nullptr;
CapsObserverFn g_capsObs = nullptr;
void jpline(const char* fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_logger) g_logger(line);
    else          std::printf("%s\n", line);
}
// iteration-G129: a diagnostic line that is emitted exactly once per call site
// (each expansion owns its own block-scope static). Used for the "first call of
// every COM method" log: never per-call, never per-sample.
#define SON_ONCE(...) do { static bool once_ = false; if (!once_) { once_ = true; jpline(__VA_ARGS__); } } while (0)

HostOps           g_ops;
bool              g_installed = false;
bool              g_on = false;              // is D2_SON set
bool              g_log = false;             // D2_SONLOG
int               g_voiceCap = 0;            // D2_SONVOICES, 0 = no cap
uint64_t          g_maxFrames = 0;           // D2_SONMAXS: sink duration cap, in frames (0 = unlimited)
std::string       g_sinkWant;                // "wav" / "null" / "vita"
std::string       g_dumpPath;

// g_sink is atomic and is never destroyed while a thread may still use it: the
// audio thread reloads it every grain, teardown swaps in nullptr before any
// close, and it only closes/destroys once the thread has actually joined.
std::atomic<audio::Sink*> g_sink{nullptr};
bool              g_selfPaced = false;      // informational only (logging) — the
                                            // decision path reads s->self_paced(), never this.
std::atomic<int>  g_threadRun{0};
bool              g_built = false;           // vtables written into guest memory

std::mutex        g_mx;                      // small host-side lock (never the GIL)
std::vector<Voice> g_voices;
std::vector<size_t> g_freeVoices;
// Object slots returned by non-owning voices: their buffer belongs to someone
// else, so they have nothing to offer ordinary reuse (which picks by
// capacity). Mixing them into g_freeVoices would either resurface a shared
// buffer or clutter the list with entries nothing can satisfy.
std::vector<size_t> g_freeDupVoices;

uint32_t g_vtDS[11]  = {0};
uint32_t g_vtBuf[21] = {0};
uint32_t g_vtDSva = 0, g_vtBufVA = 0;
uint32_t g_devObj = 0;

// Counters — all published by stat_line() (Vita watchdog, 10 s window) and
// the final report line. No counter is exposed unless it's both fed and read:
// a lying counter costs more than a missing one.
std::atomic<unsigned long long> c_created{0}, c_grains{0}, c_famine{0}, c_play{0},
    c_stop{0}, c_frames{0}, c_lockKio{0}, c_nohost{0}, c_mixus{0}, c_outus{0},
    c_mixed{0}, c_restmin{0xffffffffull},
    // --- three named failure modes -------------------------------------------
    c_playnl{0},     // Play without DSBPLAY_LOOPING: one-shot voice (see loop handling)
    c_endnl{0},      // one-shot voice reached its end and was stopped by the mixer
    c_resamp{0},     // grains mixed for a voice whose rate != kRate
    // --- actual amplitude sent to the sink ------------------------------------
    // The counters above stay perfect even with a fully-zero g_out: "the mixer
    // produces silence" and "the port isn't playing" are indistinguishable on
    // console. These four close that gap; published as crete= / rms= in the
    // counters line.
    c_peak{0}, c_peakall{0}, c_sqsum{0}, c_sqn{0},
    c_gain0{0},      // voices mixed with gL == gR == 0 (volume floor)
    // --- iteration-G129 (Ignition SFX first pass): counters the mixer could not
    // publish before. All plain relaxed atomics, no logging on the hot path.
    c_peakvox{0},    // most voices mixed in a single grain (whole run)
    c_mixmax{0},     // longest mix_grain, µs — window (reset by stat_line)
    c_mixmaxall{0},  // same, whole run
    c_locks{0},      // partial Lock calls on secondary buffers
    c_late{0},       // ... of which landed BEHIND the play cursor (game wrote late = underrun)
    c_latemax{0},    // longest lateness seen, ms — window (reset by stat_line)
    // --- iteration-G130: feed / stream health, all WINDOW counters (reset by detail_line part 0) ---
    w_lockBytes{0},  // bytes of partial stream Locks (audio the game produced)
    w_lateSumMs{0}, w_lateMaxMs{0},
    w_aheadSumMs{0}, w_aheadN{0}, w_aheadMinMs{~0ull},     // lead of on-time writes over the play cursor
    w_bk0{0}, w_bk1{0}, w_bk2{0}, w_bk3{0}, w_bk4{0},       // lead: <0 late | 0-46 ms (inside the write-cursor guard) | 46-150 | 150-300 | >300
    w_silRegions{0}, w_silFrames{0},                        // grains/regions where a stream's stale content was replaced by silence
    c_silRegionsAll{0}, c_silFramesAll{0},
    c_dscreate{0}, c_bufrel{0}, c_clearlocks{0},            // DirectSoundCreate calls, buffer Releases to 0, whole-buffer Locks
    // --- iteration-G131: integrity checks of the stream freshness tables (see detail_line part 0) ---
    c_canaryBad{0}, c_flagBad{0}, c_freshChecks{0};          // guard bands overwritten, flags not in {0,1}, tables checked
bool g_freshOff = false;                                     // WX86_FRESH=0: never switch a ring to stream mode (G129 ring behaviour)

// Output/accumulation buffers (audio thread OR frame-tick pump, never both: a
// self-clocked sink disables the frame-tick pump).
// alignas(64): g_out is handed directly to the driver's output call, which
// does DMA. Nothing in the SDK header guarantees 2-byte alignment is enough,
// and a failure here would be silent. ARM cache line = 64 B; not proven the
// driver actually requires it, but cheap insurance.
alignas(64) int32_t  g_acc[kGrain * kOutCh];
alignas(64) int16_t  g_out[kGrain * kOutCh];

// ---- iteration-G130: Play/Stop audit table + stream rate --------------------------------------
// Every Play/Stop is folded into a row keyed by (op, buffer kind, caller EIP, flags): 24 rows max,
// the rest counted as dropped. Read by detail_line (never printed per call). Written under g_mx.
struct PsRow { uint8_t op; uint8_t prim; uint32_t caller, flags, n, firstMs, lastMs; };
constexpr int kPsRows = 24;
PsRow g_ps[kPsRows];
int   g_psN = 0;
unsigned long long g_psDropped = 0;
std::atomic<uint32_t> g_streamBps{0};      // bytes/s of the streaming ring (rate*blockAlign); 0 = none seen
void ps_note(uint8_t op, bool prim, uint32_t caller, uint32_t flags, uint32_t nowMs) {   // under g_mx
    for (int i = 0; i < g_psN; i++) {
        PsRow& r = g_ps[i];
        if (r.op == op && r.prim == (prim ? 1 : 0) && r.caller == caller && r.flags == flags) { r.n++; r.lastMs = nowMs; return; }
    }
    if (g_psN >= kPsRows) { g_psDropped++; return; }
    g_ps[g_psN++] = PsRow{op, (uint8_t)(prim ? 1 : 0), caller, flags, 1, nowMs, nowMs};
}

// Deferred retry for the audio thread. When audio::thread_start()'s fallback
// chain runs out, the realtime sink is closed and replaced by the null sink
// (game stays playable, muted). These three variables let it retry later,
// outside the init path, since the fallback chain runs during device
// creation, near a low point in the memory curve. Touched only from the guest
// thread (open_sink and frame_pump), never from the audio thread, which in
// this case doesn't exist.
bool     g_deferArmed = false;      // the wanted realtime sink failed to open
int      g_deferLeft  = 0;          // retries remaining
uint32_t g_deferNext  = 0;          // next attempt, in guest ms

// frame-tick pump clock
bool     g_pumpStarted = false;
uint32_t g_pumpT0 = 0;
uint64_t g_produced = 0;

// ---- utilities ---------------------------------------------------------------
uint32_t gain_q16(int32_t mb) {
    if (mb <= -10000) return 0;
    if (mb >= 0) return 65536;
    return (uint32_t)(65536.0 * std::pow(10.0, (double)mb / 2000.0) + 0.5);
}
void recompute_gains(Voice& v) {
    const uint32_t base = gain_q16(v.volmB);
    uint32_t l = base, r = base;
    // DSBPAN: > 0 attenuates LEFT, < 0 attenuates RIGHT (DirectSound contract).
    if (v.panmB > 0)      l = (uint32_t)((uint64_t)base * gain_q16(-v.panmB) >> 16);
    else if (v.panmB < 0) r = (uint32_t)((uint64_t)base * gain_q16( v.panmB) >> 16);
    v.gL = l; v.gR = r;
}

Voice* voice_of(Cpu& c, uint32_t self) {          // called under g_mx
    if (!self) return nullptr;
    if (c.read_u32(self + 8) != MAGIC_BUF) return nullptr;
    uint32_t i = c.read_u32(self + 4);
    if (i >= g_voices.size()) return nullptr;
    Voice* v = &g_voices[i];
    return v->alive ? v : nullptr;
}
bool is_device(Cpu& c, uint32_t self) {
    return self && c.read_u32(self + 8) == MAGIC_DEV;
}

void guest_zero(Cpu& c, uint32_t va, uint32_t n) {
    if (uint8_t* h = (uint8_t*)c.hostptr(va, n)) { std::memset(h, 0, n); return; }
    static const uint8_t z[4096] = {0};
    for (uint32_t o = 0; o < n; o += (uint32_t)sizeof z)
        c.write(va + o, z, (uint32_t)((n - o < sizeof z) ? (n - o) : sizeof z));
}

// ---- the mixer ---------------------------------------------------------------
// A snapshot is taken under the host lock (a few µs); mixing and the sink
// write happen outside it. The audio thread never takes the GIL; shim bodies
// take g_mx while already holding the GIL — a single lock order, GIL -> g_mx,
// so no deadlock is possible.
// `acc`/`rate`: resampling. The read pointer doesn't advance one source frame
// per output frame — it advances by rate/kRate, using the same integer
// arithmetic as the cursor (`posAcc`), so the mix's final position and the
// cursor's final position are exactly the same. At rate == kRate, `acc` stays
// zero and playback is sample-for-sample identical to the unresampled path.
// `loop`: a voice without DSBPLAY_LOOPING doesn't loop — it goes silent at the
// end and the mixer stops it (see below).
struct Snap { const uint8_t* host; uint32_t len, cur, blockAlign, rate; uint64_t acc;
              int ch; uint32_t gL, gR; bool loop;
              bool useFresh; uint32_t frOff, frN; size_t vi; };   // iteration-G130: freshness flags of this grain in g_frbuf[frOff..frOff+frN)
std::vector<Snap> g_snap;
std::vector<uint8_t> g_frbuf;          // per-grain copy of the consumed freshness flags (mixer thread only)
std::vector<uint8_t> g_silPrev;        // per voice: did the previous grain substitute silence (mixer thread only)
constexpr uint32_t kFrMax = 2304;      // flags per voice per grain (512 frames at up to 4.5x the mix rate)

void mix_grain(Cpu* c, int frames) {
    // Hard bound: `frames` comes either from the audio thread (fixed kGrain)
    // or frame-tick catch-up (at most kGrain). The rest of this function
    // writes into g_acc/g_out sized for kGrain — an over-size grain would
    // overflow them, and a grain <= 0 would feed the sink nothing.
    if (frames <= 0) return;
    if (frames > kGrain) frames = kGrain;
    const uint64_t t0 = wx86_now_us();
    int nmix = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_snap.clear(); g_frbuf.clear();
        for (Voice& v : g_voices) {
            if (!v.alive || !v.playing || v.primary || !v.len) continue;
            // Host view: cached at creation, retried on the guest thread (Play
            // and Lock, which have a Cpu&) — see voice_rehost(). Retry only
            // makes sense for the frame-tick pump; on the self-clocked path
            // c == nullptr and there is nothing to retry, which is why the
            // retry lives on the guest-thread caller instead.
            if (!v.host && c) v.host = (const uint8_t*)c->hostptr(v.buf, v.len);
            const uint64_t acc0 = v.posAcc;
            if (!v.host) c_nohost.fetch_add(1, std::memory_order_relaxed);
            else if (!g_voiceCap || (int)g_snap.size() < g_voiceCap) {
                // iteration-G130: freshness window of a streaming ring (see Voice::stream). The bytes the
                // cursor will read in this grain that the game did NOT write become silence.
                bool useFresh = v.stream && v.looping && v.blockAlign && v.freshN && v.fresh.size() == (size_t)v.freshN + 2u * kFreshGuard;
                uint32_t frOff = 0, frN = 0;
                if (useFresh) {
                    // Blocks the cursor passes in this grain (exact: same integer arithmetic as the advance below).
                    const uint64_t nb = ((v.posAcc + (uint64_t)frames * v.rate) / (uint64_t)kRate);
                    if (nb + 1 > kFrMax) useFresh = false;
                    else {
                        // The flags are only COPIED here (into the mixer-thread buffer) and cleared in the voice's
                        // own table; the guest ring is never written by the mixer.
                        const size_t ng = v.freshN;
                        const size_t c0 = (size_t)(v.cursor / v.blockAlign) % ng;
                        frOff = (uint32_t)g_frbuf.size(); frN = (uint32_t)nb + 1;      // +1: a resampled read may look one block ahead
                        for (uint32_t i = 0; i < frN; i++) g_frbuf.push_back(v.fresh[kFreshGuard + (c0 + i) % ng]);
                        for (uint64_t i = 0; i < nb; i++) v.fresh[kFreshGuard + (c0 + (size_t)i) % ng] = 0;   // consumed: must be rewritten to be heard again
                    }
                }
                g_snap.push_back(Snap{v.host, v.len, v.cursor, v.blockAlign, v.rate, acc0,
                                      v.ch, v.gL, v.gR, v.looping, useFresh, frOff, frN, (size_t)(&v - &g_voices[0])});
                if (v.rate != (uint32_t)kRate) c_resamp.fetch_add(1, std::memory_order_relaxed);
            }
            // Cursor advances for every voice that's playing, mixed or not: a
            // voice whose cursor never moves is never seen as finished by the
            // game's polling loop, so Stop is never called and the sfx voice
            // pool eventually runs dry — silently.
            v.posAcc += (uint64_t)frames * v.rate;
            uint32_t adv = (uint32_t)(v.posAcc / (uint64_t)kRate) * v.blockAlign;
            v.posAcc %= (uint64_t)kRate;
            if (adv) {
                const uint64_t p = (uint64_t)v.cursor + adv;
                if (v.looping) {
                    v.cursor = (uint32_t)(p % v.len);
                } else if (p >= v.len) {
                    // One-shot voice reached its end. Without this it would
                    // loop forever: an sfx that never finishes and a voice
                    // never returned to the pool — the exact silent failure
                    // the cursor advance above exists to prevent.
                    v.cursor = v.len ? v.len - v.blockAlign : 0;
                    v.playing = false; v.posAcc = 0;
                    c_endnl.fetch_add(1, std::memory_order_relaxed);
                } else {
                    v.cursor = (uint32_t)p;
                }
            }
        }
        nmix = (int)g_snap.size();
        int n0 = 0;
        for (const Snap& sn : g_snap) if (!sn.gL && !sn.gR) n0++;
        c_gain0.store((unsigned long long)n0, std::memory_order_relaxed);
        // The invisible failure mode: everything works, but every voice is at
        // gain 0. grains=, voix=, rest= and famine= all look perfect — gain
        // floors to exactly 0, i.e. total digital silence. This log line is
        // the only way to tell that apart from a dead output port.
        if (nmix > 0 && n0 == nmix) {
            static bool cried = false;
            if (!cried) { cried = true;
                jpline("[son] TOUTES les voix melangees (%d) sont a GAIN NUL."
                       " Le melangeur produit un silence NUMERIQUE : ce n'est pas le port.", nmix); }
        }
    }
    c_mixed.store((unsigned long long)nmix, std::memory_order_relaxed);
    { unsigned long long pv = c_peakvox.load(std::memory_order_relaxed);
      while ((unsigned long long)nmix > pv
             && !c_peakvox.compare_exchange_weak(pv, (unsigned long long)nmix)) {} }

    const int ns = frames * kOutCh;
    std::memset(g_acc, 0, sizeof(int32_t) * (size_t)ns);
    for (const Snap& s : g_snap) {
        if (!s.blockAlign) continue;
        // Actual read width, distinct from blockAlign: a buffer whose
        // nBlockAlign lies (or whose length isn't a multiple of it) would
        // read 2 or 4 bytes past the end of the host view. The advance step
        // stays blockAlign — that's the DirectSound contract — the guard is
        // only on the read.
        const uint32_t width = (s.ch == 2) ? 4u : 2u;
        if (s.len < width) continue;
        uint32_t pos = s.cur % s.len;
        if (pos + width > s.len) pos = 0;
        uint64_t acc = s.acc;
        uint32_t gi = 0;                 // iteration-G130: blocks consumed since this grain's first block
        uint32_t silentFrames = 0;
        bool done = false;
        for (int i = 0; i < frames && !done; i++) {
            const bool fresh = !s.useFresh || (gi < s.frN && g_frbuf[s.frOff + gi]);
            if (!fresh) silentFrames++;
            if (fresh) {                                         // stale part of a stream ring -> silence
              if (s.ch == 2) {
                int16_t l, r; std::memcpy(&l, s.host + pos, 2); std::memcpy(&r, s.host + pos + 2, 2);
                g_acc[2*i]   += (int32_t)(((int64_t)l * (int64_t)s.gL) >> 16);
                g_acc[2*i+1] += (int32_t)(((int64_t)r * (int64_t)s.gR) >> 16);
              } else {
                int16_t v; std::memcpy(&v, s.host + pos, 2);
                g_acc[2*i]   += (int32_t)(((int64_t)v * (int64_t)s.gL) >> 16);
                g_acc[2*i+1] += (int32_t)(((int64_t)v * (int64_t)s.gR) >> 16);
              }
            }
            // Advances by rate/kRate, exact integer arithmetic (no drift).
            acc += s.rate;
            while (acc >= (uint64_t)kRate) {
                acc -= (uint64_t)kRate;
                pos += s.blockAlign;
                gi++;
                if (pos + width > s.len) {                 // end of buffer
                    if (s.loop) { pos = 0; }
                    else { done = true; break; }           // one-shot voice: SILENCE
                }
            }
        }
        if (s.useFresh) {                // iteration-G130: silence substituted for stale ring content (mixer thread only)
            if (g_silPrev.size() <= s.vi) g_silPrev.resize(s.vi + 1, 0);
            if (silentFrames) {
                w_silFrames.fetch_add(silentFrames, std::memory_order_relaxed);
                c_silFramesAll.fetch_add(silentFrames, std::memory_order_relaxed);
                if (!g_silPrev[s.vi]) { w_silRegions.fetch_add(1, std::memory_order_relaxed); c_silRegionsAll.fetch_add(1, std::memory_order_relaxed); }
            }
            g_silPrev[s.vi] = silentFrames != 0;
        }
    }
    for (int k = 0; k < ns; k++) {
        int32_t v = g_acc[k];
        g_out[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    // Second hard bound: the console sink always consumes a fixed outLen_
    // (512 frames) regardless of `frames`, so the unused tail of g_out must
    // be silent — otherwise the driver replays the end of the previous grain.
    // Peak and energy of the grain, measured on what's actually sent to the
    // sink: without this, there's no way to tell a silent mixer apart from a
    // dead output port. Cost (1024 absolute values, 43 times/sec) is in the
    // noise.
    { int pk = 0; uint64_t sq = 0;
      for (int k = 0; k < ns; k++) { const int v = g_out[k]; const int a = v < 0 ? -v : v;
                                     if (a > pk) pk = a; sq += (uint64_t)((int64_t)v * (int64_t)v); }
      unsigned long long cur = c_peak.load(std::memory_order_relaxed);
      while ((unsigned long long)pk > cur
             && !c_peak.compare_exchange_weak(cur, (unsigned long long)pk)) {}
      // c_peak resets every window; c_peakall never does — the final report
      // line reads c_peakall, otherwise "crete" would only reflect the last
      // few seconds of the run.
      cur = c_peakall.load(std::memory_order_relaxed);
      while ((unsigned long long)pk > cur
             && !c_peakall.compare_exchange_weak(cur, (unsigned long long)pk)) {}
      c_sqsum.fetch_add(sq, std::memory_order_relaxed);
      c_sqn.fetch_add((unsigned long long)ns, std::memory_order_relaxed); }
    if (ns < kGrain * kOutCh)
        std::memset(g_out + ns, 0, sizeof(int16_t) * (size_t)(kGrain * kOutCh - ns));
    const uint64_t t1 = wx86_now_us();
    c_mixus.fetch_add(t1 - t0, std::memory_order_relaxed);
    { const unsigned long long d = (unsigned long long)(t1 - t0);
      unsigned long long cur = c_mixmax.load(std::memory_order_relaxed);
      while (d > cur && !c_mixmax.compare_exchange_weak(cur, d)) {}
      cur = c_mixmaxall.load(std::memory_order_relaxed);
      while (d > cur && !c_mixmaxall.compare_exchange_weak(cur, d)) {} }
    // g_sink is read into a local exactly once: teardown swaps it for nullptr
    // and only destroys it after the thread has joined, so this pointer stays
    // valid for the rest of the function.
    audio::Sink* sk = g_sink.load(std::memory_order_acquire);
    // Third and strongest guard, because it sits at the write site itself.
    // `c != nullptr` means this call is on the guest thread (the audio thread
    // has no Cpu& and always calls mix_grain(nullptr, …)). A realtime sink writes
    // with a blocking call; the guest thread holds the GIL. That combination
    // must never happen, and this guard makes it structurally impossible here
    // rather than relying on callers upstream (frame_pump checking
    // self_paced(), open_sink substituting the null sink) to prevent it.
    if (c && sk && sk->self_paced()) {
        static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] REFUS: puits temps-reel atteint depuis le fil INVITE — ecriture ignoree"
                   " (aucun appel bloquant ne s'execute sous le GIL)"); }
        sk = nullptr;
    }
    // Duration cap: exists only to bound the size of the proof WAV under a
    // virtual clock. Meaningless for a realtime sink, where it's inert. On
    // other sinks its activation is logged once — otherwise it would swallow
    // audio silently: mixing and cursor advance continue, but no more bytes
    // reach the sink.
    const bool capped = g_maxFrames && sk && !sk->self_paced() && c_frames.load() >= g_maxFrames;
    if (capped) { static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] borne de duree atteinte (%llu trames) : le puits n'est PLUS alimente"
                   " (le melange continue, les curseurs avancent)", (unsigned long long)g_maxFrames); } }
    if (sk && !capped) {
        const int rest = sk->rest_samples();
        if (rest == 0) c_famine.fetch_add(1, std::memory_order_relaxed);
        if (rest >= 0) { unsigned long long r = (unsigned long long)rest, cur = c_restmin.load();
                         while (r < cur && !c_restmin.compare_exchange_weak(cur, r)) {} }
        sk->write(g_out, frames);
    }
    c_outus.fetch_add(wx86_now_us() - t1, std::memory_order_relaxed);
    c_grains.fetch_add(1, std::memory_order_relaxed);
    c_frames.fetch_add((unsigned long long)frames, std::memory_order_relaxed);
}

// Host audio thread body (self-clocked sink). Registers itself in the GIL's
// stack table — not to acquire the GIL (it never does) but so thread
// observability doesn't mistake it for an unregistered thread.
void audio_body() {
    char here;
    const int slot = gil::register_stack((uintptr_t)&here - 0x4000, (uintptr_t)&here, 0xA0D10u);
    while (g_threadRun.load(std::memory_order_relaxed)) mix_grain(nullptr, kGrain);
    gil::unregister_stack(slot);
}

// ---- COM object factory --------------------------------------------------
uint32_t alloc_obj(Cpu& c, uint32_t vt, uint32_t magic, uint32_t index) {
    uint32_t o = wx86_scratch_alloc(16);
    if (!o) return 0;
    c.write_u32(o + 0, vt);
    c.write_u32(o + 4, index);
    c.write_u32(o + 8, magic);
    c.write_u32(o + 12, 1);
    return o;
}

void build_vtables(Cpu& c) {
    if (g_built) return;
    g_vtDSva  = wx86_scratch_alloc(sizeof g_vtDS);
    g_vtBufVA = wx86_scratch_alloc(sizeof g_vtBuf);
    if (!g_vtDSva || !g_vtBufVA) { jpline("[son] MISC epuise : vtables non posees"); return; }
    c.write(g_vtDSva,  g_vtDS,  (uint32_t)sizeof g_vtDS);
    c.write(g_vtBufVA, g_vtBuf, (uint32_t)sizeof g_vtBuf);
    g_built = true;
}

// ---- method bodies ----------------------------------------------------------
uint32_t m_qi(Cpu& c) {                       // QueryInterface (both vtables)
    // Hardware 3D and EAX are refused: GetCaps reports dwMaxHw3DAllBuffers=0,
    // so a game's capability probe here typically falls back to its own 2D
    // stereo path, computing panning itself.
    if (uint32_t pp = c.arg(2)) c.write_u32(pp, 0);
    return E_NOINTERFACE;
}
uint32_t m_addref(Cpu& c) {
    uint32_t s = c.arg(0); if (!s) return 0;
    uint32_t r = c.read_u32(s + 12) + 1; c.write_u32(s + 12, r); return r;
}
uint32_t m_ds_release(Cpu& c) {
    uint32_t s = c.arg(0); if (!s) return 0;
    uint32_t r = c.read_u32(s + 12); if (r) --r; c.write_u32(s + 12, r); return r;
}
uint32_t m_buf_release(Cpu& c) {
    uint32_t s = c.arg(0); if (!s) return 0;
    uint32_t r = c.read_u32(s + 12); if (r) --r; c.write_u32(s + 12, r);
    if (!r) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (Voice* v = voice_of(c, s)) {
            v->playing = false; v->alive = false;
            c_bufrel.fetch_add(1, std::memory_order_relaxed);
            // Reuse list: the scratch allocator never frees and is capped at
            // 2 MiB, and the VA arena at 200/230 MiB. A voice pool recreated
            // across game state changes would eat into both, so the object
            // and buffer are returned to reuse instead of freed.
            // A shared buffer returns to the reuse pool only once nobody is
            // reading it anymore. Without this refcount, freeing the original
            // while a duplicate is still playing would hand its bytes to the
            // next CreateSoundBuffer while the duplicate kept reading over
            // the new owner's shoulder.
            const size_t slot = (size_t)c.read_u32(s + 4);
            if (v->ownsBuf) {
                if (!v->shares) g_freeVoices.push_back(slot);
                // otherwise: slot stays held, the last surviving duplicate will return it
            } else {
                const size_t os = v->owner;
                if (os < g_voices.size() && g_voices[os].shares) {
                    if (--g_voices[os].shares == 0 && !g_voices[os].alive)
                        g_freeVoices.push_back(os);
                }
                g_freeDupVoices.push_back(slot);
            }
        }
    }
    return r;
}
// IDirectSound::SetCooperativeLevel(hwnd, level).
//
// iteration-G129: DSSCL_WRITEPRIMARY (4) is REFUSED with DSERR_UNSUPPORTED
// (== E_NOTIMPL). In that mode a game mixes by itself straight into the
// PRIMARY buffer, and this engine never mixes the primary one (see the voice
// loop in mix_grain: `v.primary` is skipped) — so accepting the level would
// answer success to a request that can only end in total silence, with every
// counter looking healthy. Ignition (Ign_3dfx.exe 0x0045d819) asks for level 4
// first and, when it fails, falls back to DSSCL_EXCLUSIVE (3) with a looping
// SECONDARY stream buffer (0x0045d824..0x0045dafa) — that path is what the
// mixer really serves. Every other level is accepted as before.
uint32_t m_ds_setcooplevel(Cpu& c) {
    const uint32_t level = c.arg(2);
    if (level == 4u /*DSSCL_WRITEPRIMARY*/) {
        SON_ONCE("[son] SetCooperativeLevel(WRITEPRIMARY) -> DSERR_UNSUPPORTED (0x%08x): le primaire n'est jamais"
                 " melange ici, le jeu doit retomber sur son chemin tampon secondaire", E_NOTIMPL);
        return E_NOTIMPL;
    }
    SON_ONCE("[son] SetCooperativeLevel(niveau=%u) -> DS_OK", level);
    return DS_OK;
}
uint32_t m_ds_compact(Cpu&)      { return DS_OK; }
uint32_t m_ds_initialize(Cpu&)   { return DS_OK; }
uint32_t m_ds_getspeaker(Cpu& c) { if (uint32_t p = c.arg(1)) c.write_u32(p, 1u /*DSSPEAKER_STEREO*/); return DS_OK; }
uint32_t m_ds_setspeaker(Cpu&)   { return DS_OK; }
// IDirectSound::DuplicateSoundBuffer(original, ppDuplicate).
//
// A generic engine shouldn't refuse a method just because one consumer
// doesn't call it — another one will.
//
// DirectSound contract: the duplicate SHARES the original's sample data
// (writing into one is visible in the other) but has its own read cursor,
// volume, pan, and frequency. That's exactly what's needed to play the same
// sound twice at once without copying bytes, which is why games use it.
uint32_t m_ds_duplicate(Cpu& c) {
    uint32_t src = c.arg(1), ppdup = c.arg(2);
    if (!src || !ppdup) return DSERR_INVALIDPARAM;
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* o = voice_of(c, src);
    if (!o) return DSERR_INVALIDPARAM;
    // The primary buffer can't be duplicated (real DirectSound returns
    // DSERR_INVALIDCALL; INVALIDPARAM is the closest refusal this shell
    // exposes — the primary is never mixed here anyway, so there's nothing
    // to gain from duplicating it).
    if (o->primary) return DSERR_INVALIDPARAM;

    size_t idx = (size_t)-1;
    if (!g_freeDupVoices.empty()) { idx = g_freeDupVoices.back(); g_freeDupVoices.pop_back(); }
    else {
        g_voices.push_back(Voice());
        idx = g_voices.size() - 1;
        // `o` may have been invalidated by the vector's reallocation.
        o = voice_of(c, src);
        if (!o) return DSERR_INVALIDPARAM;
        uint32_t obj = alloc_obj(c, g_vtBufVA, MAGIC_BUF, (uint32_t)idx);
        if (!obj) { g_voices.pop_back(); return DSERR_NODRIVER; }
        g_voices[idx].obj = obj;
    }
    const size_t oslot = (size_t)c.read_u32(src + 4);
    g_voices[oslot].shares += 1;
    Voice& d = g_voices[idx];
    const uint32_t obj = d.obj;
    d.owner = oslot;
    // Shared samples: same guest address, same host view, and crucially
    // cap = 0 so ordinary reuse (which picks by `cap >= bytes`) can never
    // resurface this buffer.
    d.obj = obj; d.buf = o->buf; d.host = o->host; d.cap = 0; d.ownsBuf = false;
    d.len = o->len; d.flags = o->flags;
    d.ch = o->ch; d.bits = o->bits; d.rate = o->rate; d.blockAlign = o->blockAlign;
    d.primary = false; d.alive = true;
    // Independent playback state — that's the whole point of a duplicate.
    d.cursor = 0; d.posAcc = 0; d.playing = false; d.looping = false;
    d.stream = false; d.fresh.clear(); d.freshN = 0;        // iteration-G130
    d.volmB = o->volmB; d.panmB = o->panmB; d.refs = 1;
    // KiB locked: its own counter, per duplicate. It doesn't lock the same
    // region as its original — DirectSound has each writer lock for itself —
    // so inheriting the original's count would make its ring look fed when
    // nobody has touched it.
    d.lockKio = 0; d.volSaid = false;
    recompute_gains(d);
    c.write_u32(obj + 12, 1);
    c.write_u32(ppdup, obj);
    c_created.fetch_add(1, std::memory_order_relaxed);
    if (g_log) jpline("[son] DuplicateSoundBuffer obj=0x%08x -> obj=0x%08x (tampon 0x%08x partage)",
                      src, obj, d.buf);
    return DS_OK;
}

uint32_t m_ds_getcaps(Cpu& c) {
    uint32_t p = c.arg(1); if (!p) return DSERR_INVALIDPARAM;
    uint32_t caps[24] = {0};                 // DSCAPS, dwSize = 0x60
    caps[0]  = 0x60;
    // DSCAPS_PRIMARYSTEREO 0x02 | DSCAPS_PRIMARY16BIT 0x08
    // | DSCAPS_SECONDARYSTEREO 0x200 | DSCAPS_SECONDARY16BIT 0x800.
    // Must not include DSCAPS_SECONDARY8BIT (0x400): the mixer forces 16-bit
    // and never accepts that format.
    caps[1]  = 0x00000A0Au;
    caps[2]  = 100; caps[3] = 100000;        // min/max secondary sample rate
    caps[4]  = 1;                            // dwPrimaryBuffers
    // caps[5..10] = hardware mixing: 0. caps[11] = dwMaxHw3DAllBuffers = 0:
    // this zero is what makes a game's 3D capability probe fail and fall back
    // to 2D stereo — the cheapest mode, and the only one implemented here.
    c.write(p, caps, (uint32_t)sizeof caps);
    // Said once, when the cause is set. The engine states the generic fact;
    // the host, if it has one, names what that changes in its own game's menu.
    static bool said = false;
    if (!said) { said = true;
        jpline("[son] GetCaps: dwMaxHw3DAllBuffers=0 (VOULU) -> le jeu retombera sur son chemin"
               " 2D stereo, et ses options 3D materielles seront inactives.");
        if (g_capsObs) g_capsObs(); }
    return DS_OK;
}

uint32_t m_ds_createbuffer(Cpu& c) {
    uint32_t pdesc = c.arg(1), ppbuf = c.arg(2);
    if (!pdesc || !ppbuf) return DSERR_INVALIDPARAM;
    const uint32_t flags = c.read_u32(pdesc + 4);
    uint32_t bytes = c.read_u32(pdesc + 8);
    const uint32_t pwfx = c.read_u32(pdesc + 16);
    const bool primary = (flags & 0x1u) != 0;

    int ch = 2, bits = 16; uint32_t rate = kRate, blockAlign = 4;
    uint32_t srcTag = 0; int srcBits = 16;         // as requested by the game (diagnostics only)
    if (pwfx) {
        uint32_t w0 = c.read_u32(pwfx);            // wFormatTag | nChannels<<16
        srcTag = w0 & 0xffffu;
        ch   = (int)(w0 >> 16); if (ch < 1 || ch > 2) ch = 2;
        rate = c.read_u32(pwfx + 4); if (!rate) rate = kRate;
        uint32_t w3 = c.read_u32(pwfx + 12);       // nBlockAlign | wBitsPerSample<<16
        blockAlign = w3 & 0xffffu; bits = (int)(w3 >> 16); srcBits = bits;
        if (!blockAlign) blockAlign = (uint32_t)ch * 2u;
        if (bits != 16) {                          // mixer only supports 16-bit PCM
            SON_ONCE("[son] ATTENTION: tampon demande en %d bits — le melangeur lit TOUJOURS du PCM 16 bits"
                     " (ce format sonnerait faux)", srcBits);
            bits = 16;
        }
    }
    // The primary buffer is never mixed (PRIORITY cooperative level — the
    // game only ever writes silence to it) but it must still be a real,
    // writable guest buffer: Lock/Unlock of silence, and consistent GetCaps.
    if (primary && !bytes) bytes = 0x8000;
    if (!bytes || bytes > 0x400000u) return DSERR_INVALIDPARAM;

    std::lock_guard<std::mutex> lk(g_mx);
    // reuse
    size_t idx = (size_t)-1;
    for (size_t k = 0; k < g_freeVoices.size(); k++) {
        Voice& f = g_voices[g_freeVoices[k]];
        if (f.cap >= bytes && f.obj) { idx = g_freeVoices[k]; g_freeVoices.erase(g_freeVoices.begin() + (long)k); break; }
    }
    if (idx == (size_t)-1) {
        if (!g_ops.va_alloc) return DSERR_NODRIVER;
        uint32_t buf = g_ops.va_alloc(bytes);
        if (!buf) { jpline("[son] arene VA epuisee : CreateSoundBuffer(%u o) refuse", bytes); return DSERR_NODRIVER; }
        g_voices.push_back(Voice());
        idx = g_voices.size() - 1;
        Voice& nv = g_voices[idx];
        nv.buf = buf; nv.cap = bytes;
        nv.obj = alloc_obj(c, g_vtBufVA, MAGIC_BUF, (uint32_t)idx);
        if (!nv.obj) return DSERR_NODRIVER;
    }
    Voice& v = g_voices[idx];
    v.len = bytes; v.cursor = 0; v.posAcc = 0; v.flags = flags;
    v.ch = ch; v.bits = bits; v.rate = rate; v.blockAlign = blockAlign;
    v.primary = primary; v.playing = false; v.looping = false; v.alive = true;
    v.volmB = 0; v.panmB = 0; v.refs = 1; v.lockKio = 0; v.volSaid = false; recompute_gains(v);
    // A reused slot always comes from g_freeVoices, i.e. an owner with no
    // live shares — but reset explicitly anyway, so sharing state can never
    // survive into a reused slot.
    v.ownsBuf = true; v.shares = 0; v.owner = (size_t)-1;
    v.stream = false; v.fresh.clear(); v.freshN = 0;        // iteration-G130: a recycled slot starts as a plain buffer
    v.host = (const uint8_t*)c.hostptr(v.buf, v.len);
    if (rate != (uint32_t)kRate) {
        // The mixer resamples via nearest-neighbor (no interpolation): pitch
        // is correct, but audio quality is reduced accordingly.
        static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] tampon a %u Hz (attendu %d) : REECHANTILLONNAGE actif"
                   " — voir resamp= du chien de garde", rate, kRate); }
    }
    c.write_u32(v.obj + 12, 1);
    guest_zero(c, v.buf, v.len);
    c.write_u32(ppbuf, v.obj);
    if (!primary) c_created.fetch_add(1, std::memory_order_relaxed);
    // iteration-G129: the first primary buffer and the first few secondary ones
    // are always logged (flags, size, requested format); the rest only under
    // the verbose switch below.
    if (primary) {
        SON_ONCE("[son] 1er tampon PRIMAIRE: flags=0x%x octets=%u format=%s -> obj=0x%08x",
                 flags, bytes, pwfx ? "fourni" : "aucun (primaire sans format)", v.obj);
    } else {
        const unsigned long long nsec = c_created.load(std::memory_order_relaxed);
        if (nsec <= 4)
            jpline("[son] tampon secondaire #%llu: flags=0x%x octets=%u tag=%u %dch %d bits %u Hz blockAlign=%u"
                   " (%.0f ms) -> obj=0x%08x", nsec, flags, bytes, srcTag, ch, srcBits, rate, blockAlign,
                   rate && blockAlign ? 1000.0 * (double)bytes / ((double)rate * (double)blockAlign) : 0.0, v.obj);
    }
    if (g_log) jpline("[son] CreateSoundBuffer %s %u o %dch/%u Hz -> obj=0x%08x buf=0x%08x",
                      primary ? "PRIMAIRE" : "secondaire", bytes, ch, rate, v.obj, v.buf);
    return DS_OK;
}

uint32_t m_buf_getcaps(Cpu& c) {
    uint32_t p = c.arg(1); if (!p) return DSERR_INVALIDPARAM;
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t caps[5] = {0x14, v ? v->flags : 0u, v ? v->len : 0u, 0, 0};   // DSBCAPS
    c.write(p, caps, (uint32_t)sizeof caps);
    return DS_OK;
}
// The write cursor must never equal the read cursor: a streaming pump would
// otherwise believe the whole ring is free and overwrite audio still being
// played (choppy sound). Fixed advance of two grains, capped at a quarter of
// the buffer. One single definition, shared by GetCurrentPosition and
// DSBLOCK_FROMWRITECURSOR: two diverging formulas would reopen exactly the
// gap this closes. Called under g_mx.
uint32_t write_cursor(const Voice& v) {
    if (!v.len) return 0;
    uint32_t lead = (uint32_t)(2 * kGrain) * v.blockAlign;
    if (lead > v.len / 4) lead = v.len / 4;
    if (lead < v.blockAlign) lead = v.blockAlign;
    return (uint32_t)(((uint64_t)v.cursor + lead) % v.len);
}

// Host-view retry, on the guest thread. The audio thread has no Cpu&, so if
// `hostptr` failed at CreateSoundBuffer time (arena not yet committed), the
// voice would stay permanently silent — the mixer has no way to retry since
// it's always called with c == nullptr. The retry happens here instead, at the
// two points the game always passes through before it can hear anything: Lock
// (where it writes its bytes) and Play. If the retry still fails, the voice
// is counted in `sansvue=` on the watchdog line, its cursor still advances
// (so the game sees it finish and reclaims it), and it stays inaudible — a
// visible failure, not a silent one. Called under g_mx.
void voice_rehost(Cpu& c, Voice& v) {
    if (v.host || !v.buf || !v.len) return;
    v.host = (const uint8_t*)c.hostptr(v.buf, v.len);
    if (!v.host) {
        static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] VUE HOTE ABSENTE pour buf=0x%08x (%u o) : voix INAUDIBLE"
                   " (curseur avance quand meme) — voir sansvue= du chien de garde", v.buf, v.len); }
    }
}

uint32_t m_buf_getpos(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    SON_ONCE("[son] 1er GetCurrentPosition (tampon %s, %u o)", v->primary ? "primaire" : "secondaire", v->len);
    if (uint32_t p = c.arg(1)) c.write_u32(p, v->cursor);
    if (uint32_t p = c.arg(2)) c.write_u32(p, write_cursor(*v));
    return DS_OK;
}
uint32_t m_buf_getformat(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t p = c.arg(1), sz = c.arg(2), pw = c.arg(3);
    if (v && p && sz >= 18) {
        uint32_t w[5];
        w[0] = 1u | ((uint32_t)v->ch << 16);
        w[1] = v->rate;
        w[2] = v->rate * v->blockAlign;
        w[3] = v->blockAlign | (16u << 16);
        w[4] = 0;
        c.write(p, w, 18);
    }
    if (pw) c.write_u32(pw, 18);
    return DS_OK;
}
uint32_t m_buf_getvolume(Cpu& c) { std::lock_guard<std::mutex> lk(g_mx); Voice* v = voice_of(c, c.arg(0));
    if (uint32_t p = c.arg(1)) c.write_u32(p, (uint32_t)(v ? v->volmB : 0)); return DS_OK; }
uint32_t m_buf_getpan(Cpu& c) { std::lock_guard<std::mutex> lk(g_mx); Voice* v = voice_of(c, c.arg(0));
    if (uint32_t p = c.arg(1)) c.write_u32(p, (uint32_t)(v ? v->panmB : 0)); return DS_OK; }
uint32_t m_buf_getfreq(Cpu& c) { std::lock_guard<std::mutex> lk(g_mx); Voice* v = voice_of(c, c.arg(0));
    if (uint32_t p = c.arg(1)) c.write_u32(p, v ? v->rate : (uint32_t)kRate); return DS_OK; }
uint32_t m_buf_getstatus(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t st = 0;
    if (v && v->playing) { st |= 1u; if (v->looping) st |= 4u; }   // PLAYING | LOOPING
    // Never DSBSTATUS_BUFFERLOST: these buffers never get lost. Reporting it
    // would send a game into a busy-poll-with-Sleep loop waiting to restore.
    if (uint32_t p = c.arg(1)) c.write_u32(p, st);
    return DS_OK;
}
uint32_t m_buf_initialize(Cpu&) { return DS_OK; }
uint32_t m_buf_lock(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    uint32_t off = c.arg(1), bytes = c.arg(2);
    const uint32_t pp1 = c.arg(3), pb1 = c.arg(4), pp2 = c.arg(5), pb2 = c.arg(6);
    const uint32_t lf = c.arg(7);
    voice_rehost(c, *v);
    if (lf & 0x2u) { off = 0; bytes = v->len; }                 // DSBLOCK_ENTIREBUFFER
    // DSBLOCK_FROMWRITECURSOR: the write cursor, not the read cursor. Using
    // v->cursor would hand the game exactly the bytes currently playing for
    // it to overwrite — the choppy audio write_cursor() exists to prevent.
    else if (lf & 0x1u) { off = write_cursor(*v); }             // DSBLOCK_FROMWRITECURSOR
    if (v->len) off %= v->len;
    if (bytes > v->len) bytes = v->len;
    uint32_t b1 = bytes, b2 = 0;
    if (off + b1 > v->len) { b1 = v->len - off; b2 = bytes - b1; }
    if (!pp2) { b2 = 0; }                                       // no second segment requested
    if (pp1) c.write_u32(pp1, v->buf + off);
    if (pb1) c.write_u32(pb1, b1);
    if (pp2) c.write_u32(pp2, b2 ? v->buf : 0);
    if (pb2) c.write_u32(pb2, b2);
    c_lockKio.fetch_add((b1 + b2) >> 10, std::memory_order_relaxed);
    v->lockKio += (uint64_t)((b1 + b2) >> 10);
    SON_ONCE("[son] 1er Lock (tampon %s): decalage=%u octets=%u drapeaux=0x%x -> seg1=%u seg2=%u",
             v->primary ? "primaire" : "secondaire", off, bytes, lf, b1, b2);
    // iteration-G129: underrun detector, measured where the game's own write
    // pointer is visible — at its Lock. A streaming game locks a short region
    // (~10 ms) a little AHEAD of the play cursor. A region that starts behind
    // the cursor was written late: the audio for that stretch has already gone
    // by (or will be replayed stale one ring-turn later). Whole-buffer locks
    // (the game clears the ring at every scene change) and cursor-relative
    // locks are not streaming writes and are not counted.
    if (!v->primary && v->len && bytes && bytes >= v->len) {
        // Whole-buffer Lock: the game clearing the ring at a scene change. Nothing of the old content
        // may be replayed afterwards, so every freshness flag goes down (the ring is zeroed anyway).
        c_clearlocks.fetch_add(1, std::memory_order_relaxed);
        if (v->stream) std::fill(v->fresh.begin() + kFreshGuard, v->fresh.begin() + kFreshGuard + v->freshN, (uint8_t)0);
    } else if (!v->primary && !(lf & 0x3u) && v->len && bytes && bytes < v->len) {
        c_locks.fetch_add(1, std::memory_order_relaxed);
        w_lockBytes.fetch_add(bytes, std::memory_order_relaxed);
        const uint64_t bps = (uint64_t)v->rate * v->blockAlign;
        // A partial Lock while the buffer plays = the game is streaming into this ring.
        if (!g_freshOff && !v->stream && v->playing && v->looping && v->blockAlign && bytes <= v->len / 4 && v->len % v->blockAlign == 0) {
            v->stream = true; v->freshN = v->len / v->blockAlign;
            v->fresh.assign((size_t)v->freshN + 2u * kFreshGuard, 0);
            std::fill(v->fresh.begin(), v->fresh.begin() + kFreshGuard, (uint8_t)0xA5);
            std::fill(v->fresh.end() - kFreshGuard, v->fresh.end(), (uint8_t)0xA5);
            g_streamBps.store((uint32_t)bps, std::memory_order_relaxed);
            jpline("[son] anneau en MODE FLUX: tampon %u o, bloc %u o, %u Hz — le contenu non reecrit depuis le passage du curseur"
                   " sera lu comme SILENCE (plus de rejeu de contenu ancien)", v->len, v->blockAlign, v->rate);
        }
        if (v->stream) {                // mark what the game is about to write as fresh (up to 2 segments)
            const uint32_t ba = v->blockAlign;
            auto mark = [&](uint32_t o, uint32_t n) { for (uint32_t q = o / ba, e = (o + n + ba - 1) / ba; q < e && q < v->freshN; q++) v->fresh[kFreshGuard + q] = 1; };
            mark(off, b1); if (b2) mark(0, b2);
        }
        // Where the requested offset lies relative to the play cursor. Ring distance d = (off - cursor) mod len,
        // read as "ahead" when d <= len/2 and as "behind" (late) otherwise. The game never looks at the write
        // cursor (it overwrites it right after GetCurrentPosition), so the write-cursor guard (2 grains) is only
        // used here to classify: 0-46 ms ahead = inside the guard.
        const uint32_t d = (uint32_t)(((uint64_t)off + v->len - v->cursor) % v->len);
        if (d > v->len / 2) {
            const uint32_t behind = v->len - d;
            const unsigned long long ms = bps ? (unsigned long long)behind * 1000ull / bps : 0ull;
            c_late.fetch_add(1, std::memory_order_relaxed);
            w_bk0.fetch_add(1, std::memory_order_relaxed);
            w_lateSumMs.fetch_add(ms, std::memory_order_relaxed);
            unsigned long long cur = c_latemax.load(std::memory_order_relaxed);
            while (ms > cur && !c_latemax.compare_exchange_weak(cur, ms)) {}
            cur = w_lateMaxMs.load(std::memory_order_relaxed);
            while (ms > cur && !w_lateMaxMs.compare_exchange_weak(cur, ms)) {}
            SON_ONCE("[son] 1er Lock EN RETARD: %llu ms derriere le curseur de lecture (sous-alimentation du jeu,"
                     " le son de cet intervalle est perdu)", ms);
        } else {
            const unsigned long long ms = bps ? (unsigned long long)d * 1000ull / bps : 0ull;
            w_aheadN.fetch_add(1, std::memory_order_relaxed);
            w_aheadSumMs.fetch_add(ms, std::memory_order_relaxed);
            unsigned long long cur = w_aheadMinMs.load(std::memory_order_relaxed);
            while (ms < cur && !w_aheadMinMs.compare_exchange_weak(cur, ms)) {}
            (ms < 46 ? w_bk1 : ms < 150 ? w_bk2 : ms < 300 ? w_bk3 : w_bk4).fetch_add(1, std::memory_order_relaxed);
        }
    }
    return DS_OK;
}
uint32_t m_buf_unlock(Cpu&) {   // direct write: nothing to copy back
    SON_ONCE("[son] 1er Unlock");
    return DS_OK;
}
uint32_t m_buf_play(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    voice_rehost(c, *v);
    v->playing = true; v->looping = (c.arg(3) & 0x1u) != 0;      // DSBPLAY_LOOPING
    if (v->primary) { SON_ONCE("[son] 1er Play du PRIMAIRE (jamais melange): flags=0x%x", c.arg(3)); }
    else            { SON_ONCE("[son] 1er Play d'un secondaire: flags=0x%x boucle=%s tampon=%u o %u Hz", c.arg(3),
                               v->looping ? "OUI" : "non", v->len, v->rate); }
    if (!v->looping) {
        // A non-looping Play is unusual — most callers always loop and stop
        // buffers themselves. Handled correctly either way (mixer stops the
        // voice at buffer end, c_endnl), but logged loudly since it's rare.
        if (c_playnl.fetch_add(1, std::memory_order_relaxed) == 0)
            jpline("[son] Play SANS DSBPLAY_LOOPING (flags=0x%x) : voix a UN COUP,"
                   " arret automatique en fin de tampon", c.arg(3));
    }
    c_play.fetch_add(1, std::memory_order_relaxed);
    ps_note(0, v->primary, c.read_u32(c.reg(R_ESP)), c.arg(3), g_ops.tick_ms ? g_ops.tick_ms(c) : 0u);
    return DS_OK;
}
uint32_t m_buf_stop(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    ps_note(1, v->primary, c.read_u32(c.reg(R_ESP)), 0u, g_ops.tick_ms ? g_ops.tick_ms(c) : 0u);
    SON_ONCE("[son] 1er Stop (tampon %s)", v->primary ? "primaire" : "secondaire");
    v->playing = false;
    c_stop.fetch_add(1, std::memory_order_relaxed);
    return DS_OK;
}
uint32_t m_buf_setpos(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v || !v->len) return DSERR_INVALIDPARAM;
    SON_ONCE("[son] 1er SetCurrentPosition(%u)", c.arg(1));
    v->cursor = c.arg(1) % v->len; v->posAcc = 0;
    return DS_OK;
}
uint32_t m_buf_setformat(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t pwfx = c.arg(1);
    if (!v || !pwfx) return DSERR_INVALIDPARAM;
    uint32_t w0 = c.read_u32(pwfx), w3 = c.read_u32(pwfx + 12);
    { static int said = 0;                       // iteration-G129: first 3 SetFormat requests, then silence
      if (said < 3) { said++;
          jpline("[son] SetFormat #%d (tampon %s): tag=%u %uch %u Hz %u bits blockAlign=%u -> DS_OK",
                 said, v->primary ? "primaire" : "secondaire", w0 & 0xffffu, w0 >> 16,
                 c.read_u32(pwfx + 4), w3 >> 16, w3 & 0xffffu); } }
    int ch = (int)(w0 >> 16); if (ch >= 1 && ch <= 2) v->ch = ch;
    uint32_t r = c.read_u32(pwfx + 4); if (r) v->rate = r;
    const uint32_t oldBa = v->blockAlign;
    uint32_t ba = w3 & 0xffffu; v->blockAlign = ba ? ba : (uint32_t)v->ch * 2u;
    if (v->stream && v->blockAlign != oldBa) { v->stream = false; v->fresh.clear(); v->freshN = 0; }   // iteration-G131: the flag table was sized for the old block
    return DS_OK;
}
uint32_t m_buf_setvolume(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    int32_t mb = (int32_t)c.arg(1);
    if (mb > 0) mb = 0; if (mb < -10000) mb = -10000;           // DSBVOLUME_MIN
    SON_ONCE("[son] 1er SetVolume(%d mB)", (int)mb);
    v->volmB = mb; recompute_gains(*v);
    // Only path from application volume setting to audible output. Logged
    // once per voice at its first SetVolume, plus every call that hits the
    // floor — otherwise "everything works but every voice is at gain 0" is
    // invisible even in verbose mode.
    if (g_log && (!v->volSaid || mb <= -10000)) {
        v->volSaid = true;
        jpline("[son] SetVolume obj=0x%08x %d mB -> gL=%u gR=%u%s", c.arg(0), (int)mb, v->gL, v->gR,
               (!v->gL && !v->gR) ? "  (SILENCE NUMERIQUE)" : "");
    }
    return DS_OK;
}
uint32_t m_buf_setpan(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    int32_t mb = (int32_t)c.arg(1);
    if (mb > 10000) mb = 10000; if (mb < -10000) mb = -10000;
    SON_ONCE("[son] 1er SetPan(%d mB)", (int)mb);
    v->panmB = mb; recompute_gains(*v);
    if (g_log && (!v->gL || !v->gR))
        jpline("[son] SetPan obj=0x%08x %d mB -> gL=%u gR=%u (une voie eteinte)", c.arg(0), (int)mb, v->gL, v->gR);
    return DS_OK;
}
uint32_t m_buf_setfreq(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (Voice* v = voice_of(c, c.arg(0))) {
        uint32_t f = c.arg(1);
        SON_ONCE("[son] 1er SetFrequency(%u Hz)", f);
        // DSBFREQUENCY_ORIGINAL == 0: revert to the format's frequency.
        if (f && f != v->rate) {
            v->rate = f;
            static bool cried = false;
            if (!cried) { cried = true;
                jpline("[son] SetFrequency(%u) : REECHANTILLONNAGE actif (attendu %d Hz)", f, kRate); }
        }
    }
    return DS_OK;
}
uint32_t m_buf_restore(Cpu&) { return DS_OK; }

// ---- sink opening -------------------------------------------------------
// Invariant: a realtime (self_paced) sink must never end up driven by the
// frame-tick pump. The frame-tick pump runs inside frameTick, i.e. inside a
// shim body, i.e. with the GIL held — chaining blocking sceAudioOutOutput
// calls there (~23 ms each, up to 43 per frame) would stall the game for up
// to a second per presented frame. If the thread meant to drive the sink
// fails to start, there is no degraded mode: the sink is closed and replaced
// by the null sink. The game stays playable and goes silent; that's the only
// defensible fallback.
void open_sink() {
    if (g_sink.load()) return;
    audio::Sink* s = nullptr;
    if (g_sinkWant == "vita") {
        s = audio::make_vita_sink();
        if (!s) jpline("[son] puits 'vita' indisponible dans ce binaire -> repli nul");
    } else if (g_sinkWant == "wav") {
        s = audio::make_wav_sink(g_dumpPath.c_str());
    }
    if (!s) s = audio::make_null_sink();
    if (!s->open(kRate, kOutCh, kGrain)) { delete s; s = audio::make_null_sink(); s->open(kRate, kOutCh, kGrain); }

    if (s->self_paced()) {
        // Publish before starting: the thread body reads g_sink from its
        // first grain onward. On failure we unpublish it, and nobody else has
        // seen it yet (nothing else is running: open_sink() is called from
        // DirectSoundCreate).
        g_sink.store(s, std::memory_order_release);
        g_threadRun.store(1);
        if (!audio::thread_start(audio_body)) {
            g_threadRun.store(0);
            g_sink.store(nullptr, std::memory_order_release);
            jpline("[son] fil audio NON demarre -> puits %s FERME et remplace par le puits NUL"
                   " (aucun appel bloquant ne peut atteindre le fil du jeu ; le jeu reste MUET)", s->name());
            s->close();
            delete s;                       // no thread ever read it
            s = audio::make_null_sink();
            s->open(kRate, kOutCh, kGrain);
            g_sink.store(s, std::memory_order_release);
            // Last rung of the fallback chain, and the only one outside the
            // init path: 3 more retries, 5 s of guest time apart, driven from
            // frame_pump.
            g_deferArmed = true; g_deferLeft = 3; g_deferNext = 0;
            jpline("[son] repli differe ARME : %d nouvelles tentatives de fil audio,"
                   " une toutes les 5 s, hors du chemin d'init", g_deferLeft);
        }
    } else {
        g_sink.store(s, std::memory_order_release);
    }
    g_selfPaced = s->self_paced();
    jpline("[son] puits=%s cadence=%s grain=%d (%d Hz %dch)", s->name(),
           g_selfPaced ? "temps-reel" : "tick-image", kGrain, kRate, kOutCh);
}

uint32_t ds_create(Cpu& c) {
    if (!g_on) return DSERR_NODRIVER;             // disabled by default
    build_vtables(c);
    if (!g_built) return DSERR_NODRIVER;
    c_dscreate.fetch_add(1, std::memory_order_relaxed);
    open_sink();
    uint32_t pp = c.arg(1);
    if (!pp) return DSERR_INVALIDPARAM;
    if (!g_devObj) g_devObj = alloc_obj(c, g_vtDSva, MAGIC_DEV, 0);
    if (!g_devObj) return DSERR_NODRIVER;
    c.write_u32(g_devObj + 12, c.read_u32(g_devObj + 12) + 1);
    c.write_u32(pp, g_devObj);
    jpline("[son] DirectSoundCreate -> DS_OK (objet invite 0x%08x, vtable 0x%08x)", g_devObj, g_vtDSva);
    return DS_OK;
}
uint32_t ds_enum(Cpu&) { return DS_OK; }          // callback is never invoked
// No capture support. DirectSoundCaptureCreate returns DSERR_NODRIVER — "no
// capture device", the same answer a machine with no microphone gives.
uint32_t ds_capture(Cpu&) { return DSERR_NODRIVER; }

} // namespace

// ---------------------------------------------------------------------------
void set_logger(LogFn cb) { g_logger = cb; }
void set_extra_stat(ExtraStatFn cb) { g_extra = cb; }
void set_caps_observer(CapsObserverFn cb) { g_capsObs = cb; }

void install(Bridge& br, Cpu& cpu, const HostOps& ops) {
    if (g_installed) return;
    g_installed = true;
    g_ops = ops;
    // Frequency comes from the caller. Set here, before anything that reads
    // it (g_maxFrames below, sink opening, the resampling step).
    kRate = ops.mix_rate;

    // Settings: engine name first, falling back to the port's older name.
    // Same convention as the rest of the engine (gil.cpp, cpu_box86.cpp) —
    // scripts still using the old D2_SON* names keep working, no deprecation
    // warning needed.
    const char* k     = env2("WX86_SON",      "D2_SON");
    const char* dump0 = env2("WX86_SONDUMP",  "D2_SONDUMP");
    // On by default (unset == on, routed to the real sink below). SON=0 is
    // always authoritative: it's the explicit opt-out benches rely on
    // (D2_SONDUMP alone still selects the wav sink either way, see below).
    g_on  = (k && *k) ? (std::strcmp(k, "0") != 0)
                      : true;
    // A sink armed without a frequency can only play at the wrong pitch. The
    // engine doesn't guess: it refuses to arm, and says so.
    if (g_on && kRate <= 0) {
        jpline("[son] REFUS : HostOps::mix_rate absent — socle DirectSound DESARME."
               " La frequence d'echantillonnage appartient au jeu, pas au moteur.");
        g_on = false;
    }
    g_log = env2("WX86_SONLOG", "D2_SONLOG") != nullptr;
    { const char* f = env2("WX86_FRESH", "D2_FRESH");     // iteration-G131 diagnostic switch (default: stream silence ON)
      g_freshOff = f && *f == '0'; }
    if (const char* n = env2("WX86_SONVOICES", "D2_SONVOICES")) g_voiceCap = atoi(n);
    // Under a virtual clock, guest time can run far faster than real time, so
    // an unbounded proof WAV can grow huge. SONMAXS bounds what's written
    // (mixing itself keeps going — stopping the mixer would freeze cursors
    // and drain the voice pool, see the rule above).
    if (const char* n = env2("WX86_SONMAXS", "D2_SONMAXS")) g_maxFrames = (uint64_t)atoi(n) * (uint64_t)kRate;
    const char* dump = dump0;
    if (k && (!std::strcmp(k, "null") || !std::strcmp(k, "nul"))) g_sinkWant = "null";
    else if (k && !std::strcmp(k, "wav"))                          g_sinkWant = "wav";
    else if (k && !std::strcmp(k, "vita"))                         g_sinkWant = "vita";
    else if (dump)                                                 g_sinkWant = "wav";
#ifdef __vita__
    else                                                           g_sinkWant = "vita";
#else
    else                                                           g_sinkWant = "null";
#endif
    if (g_sinkWant == "wav") {
        if (dump && *dump) g_dumpPath = dump;
        else g_dumpPath = std::string(ops.write_root ? ops.write_root : "/tmp") + "/d2_son.wav";
    }

    // --- 32 vtable slots, allocated unconditionally ---------------------------
    // alloc_trap advances by 16 bytes per slot (bridge.cpp:226): registration
    // gated by the toggle would shift the numbering of every later slot, so
    // the enabled/disabled paths would diverge for reasons unrelated to the
    // toggle itself. Allocation alone touches no guest memory, so it's free
    // on the disabled path (D2_DUMPTRAPS=1 produces the same table either way).
    int slot = 0;
    auto R = [&](uint32_t* vt, const char* name, uint32_t nparams, uint32_t (*fn)(Cpu&)) {
        Shim s; s.argc = 1 + nparams; s.stdcall_cleanup = true;
        s.tag = std::string("DSOUNDVT.dll!") + name;
        s.fn = fn;
        br.register_shim("DSOUNDVT.dll", name, s);
        vt[slot++] = br.shim_trap("DSOUNDVT.dll", name);
    };
    slot = 0;
    R(g_vtDS, "IDS_QueryInterface",      2, m_qi);
    R(g_vtDS, "IDS_AddRef",              0, m_addref);
    R(g_vtDS, "IDS_Release",             0, m_ds_release);
    R(g_vtDS, "IDS_CreateSoundBuffer",   3, m_ds_createbuffer);
    R(g_vtDS, "IDS_GetCaps",             1, m_ds_getcaps);
    R(g_vtDS, "IDS_DuplicateSoundBuffer",2, m_ds_duplicate);
    R(g_vtDS, "IDS_SetCooperativeLevel", 2, m_ds_setcooplevel);
    R(g_vtDS, "IDS_Compact",             0, m_ds_compact);
    R(g_vtDS, "IDS_GetSpeakerConfig",    1, m_ds_getspeaker);
    R(g_vtDS, "IDS_SetSpeakerConfig",    1, m_ds_setspeaker);
    R(g_vtDS, "IDS_Initialize",          1, m_ds_initialize);
    slot = 0;
    R(g_vtBuf, "IDSB_QueryInterface",    2, m_qi);
    R(g_vtBuf, "IDSB_AddRef",            0, m_addref);
    R(g_vtBuf, "IDSB_Release",           0, m_buf_release);
    R(g_vtBuf, "IDSB_GetCaps",           1, m_buf_getcaps);
    R(g_vtBuf, "IDSB_GetCurrentPosition",2, m_buf_getpos);
    R(g_vtBuf, "IDSB_GetFormat",         3, m_buf_getformat);
    R(g_vtBuf, "IDSB_GetVolume",         1, m_buf_getvolume);
    R(g_vtBuf, "IDSB_GetPan",            1, m_buf_getpan);
    R(g_vtBuf, "IDSB_GetFrequency",      1, m_buf_getfreq);
    R(g_vtBuf, "IDSB_GetStatus",         1, m_buf_getstatus);
    R(g_vtBuf, "IDSB_Initialize",        2, m_buf_initialize);
    R(g_vtBuf, "IDSB_Lock",              7, m_buf_lock);
    R(g_vtBuf, "IDSB_Play",              3, m_buf_play);
    R(g_vtBuf, "IDSB_SetCurrentPosition",1, m_buf_setpos);
    R(g_vtBuf, "IDSB_SetFormat",         1, m_buf_setformat);
    R(g_vtBuf, "IDSB_SetVolume",         1, m_buf_setvolume);
    R(g_vtBuf, "IDSB_SetPan",            1, m_buf_setpan);
    R(g_vtBuf, "IDSB_SetFrequency",      1, m_buf_setfreq);
    R(g_vtBuf, "IDSB_Stop",              0, m_buf_stop);
    R(g_vtBuf, "IDSB_Unlock",            4, m_buf_unlock);
    R(g_vtBuf, "IDSB_Restore",           0, m_buf_restore);

    // --- the only 2 exports the guest binary imports (by ordinal) ------------
    { Shim s; s.argc = 3; s.stdcall_cleanup = true; s.tag = "DSOUND.dll!DirectSoundCreate";
      s.fn = ds_create;
      br.register_shim("DSOUND.dll", "DirectSoundCreate", s);
      br.register_shim_ordinal("DSOUND.dll", 1, s);
      Shim cap; cap.argc = 3; cap.stdcall_cleanup = true; cap.tag = "DSOUND.dll!DirectSoundCaptureCreate";
      cap.fn = ds_capture;
      br.register_shim("DSOUND.dll", "DirectSoundCaptureCreate", cap);
      br.register_shim_ordinal("DSOUND.dll", 6, cap);
      Shim e; e.argc = 2; e.stdcall_cleanup = true; e.tag = "DSOUND.dll!DirectSoundEnumerateA";
      e.fn = ds_enum;
      br.register_shim("DSOUND.dll", "DirectSoundEnumerateA", e);
      br.register_shim_ordinal("DSOUND.dll", 2, e);
      br.register_shim_ordinal("DSOUND.dll", 3, e);   // EnumerateW: also 2 args
      br.register_shim_ordinal("DSOUND.dll", 7, e); }
    (void)cpu;

    if (g_on) std::printf("DSOUND: 32 creneaux de vtable alloues ; son ARME (D2_SON=%s, puits=%s%s%s)\n",
                          k ? k : "(D2_SONDUMP seul)", g_sinkWant.c_str(), g_dumpPath.empty() ? "" : " -> ", g_dumpPath.c_str());
    else      std::printf("DSOUND: 32 creneaux de vtable alloues (numerotation figee) ;"
                          " son MUET par defaut (DirectSoundCreate -> DSERR_NODRIVER)\n");
}

bool enabled() { return g_on; }
const char* sink_name() { audio::Sink* s = g_sink.load(); return s ? s->name() : "-"; }

// Deferred fallback, runs on the guest thread from frame_pump. The sink in
// place is the null sink (nobody else reads it: the audio thread doesn't
// exist), so it can be swapped without special precautions. On failure,
// state is restored exactly. No blocking call: opening a port and creating a
// thread both return immediately; the sink's only blocking function is
// write(), which is never reached here (mix_grain's third guard forbids
// writing to a realtime sink from the guest thread, and frame_pump disarms
// itself once the sink is one).
static void try_deferred_sink(Cpu& cpu) {
    const uint32_t now = g_ops.tick_ms ? g_ops.tick_ms(cpu) : 0u;
    if (!g_deferNext) { g_deferNext = now + 5000u; return; }
    if ((int32_t)(now - g_deferNext) < 0) return;
    g_deferNext = now + 5000u;
    if (g_deferLeft <= 0) {
        g_deferArmed = false;
        jpline("[son] repli differe EPUISE : le fil audio n'a jamais demarre, le jeu reste MUET"
               " (les rc de chaque essai sont dans le journal)");
        return;
    }
    const int no = 4 - g_deferLeft;   // 1, 2, 3
    --g_deferLeft;
    audio::Sink* ns = audio::make_vita_sink();
    if (!ns) { g_deferArmed = false; return; }          // no console sink in this binary
    if (!ns->open(kRate, kOutCh, kGrain)) {
        jpline("[son] essai differe %d : ouverture du puits %s REFUSEE", no, ns->name());
        ns->close(); delete ns; return;
    }
    audio::Sink* old = g_sink.load(std::memory_order_acquire);
    g_sink.store(ns, std::memory_order_release);
    g_threadRun.store(1);
    if (audio::thread_retry()) {
        g_selfPaced = true;
        if (old) { old->close(); delete old; }          // the null sink, which nobody reads anymore
        jpline("[son] essai differe %d : FIL AUDIO PARTI — puits=%s cadence=temps-reel", no, ns->name());
        g_deferArmed = false;
        return;
    }
    // Failure: restore the null sink, exactly as it was.
    g_threadRun.store(0);
    g_sink.store(old, std::memory_order_release);
    ns->close(); delete ns;
    jpline("[son] essai differe %d : fil audio toujours refuse (%d restant(s))", no, g_deferLeft);
}

void frame_pump(Cpu& cpu) {
    // The test that matters is `s->self_paced()`, not the g_selfPaced flag: an
    // error path could reset the flag while the sink itself stays realtime,
    // and the frame-tick pump would then chain blocking writes on the game
    // thread with the GIL held. Always query the object, never the cached copy.
    if (g_on && g_deferArmed) try_deferred_sink(cpu);
    audio::Sink* s = g_sink.load(std::memory_order_acquire);
    if (!g_on || !s || s->self_paced()) return;
    const uint32_t now = g_ops.tick_ms ? g_ops.tick_ms(cpu) : 0u;
    if (!g_pumpStarted) { g_pumpStarted = true; g_pumpT0 = now; g_produced = 0; return; }
    const uint64_t elapsed = (uint64_t)(uint32_t)(now - g_pumpT0);
    const uint64_t target  = elapsed * (uint64_t)kRate / 1000ull;
    if (target <= g_produced) return;
    uint64_t need = target - g_produced;
    // A jump in the guest clock must never manufacture a minute of audio: cap
    // it at 1 s and resync the counter (WAV timing tracks the game's clock,
    // not the host's).
    if (need > (uint64_t)kRate) { g_produced = target - (uint64_t)kRate; need = (uint64_t)kRate; }
    while (need) {
        const int n = (int)(need > (uint64_t)kGrain ? (uint64_t)kGrain : need);
        mix_grain(&cpu, n);
        g_produced += (uint64_t)n; need -= (uint64_t)n;
    }
}

// Teardown. Idempotent (called twice: the normal path and the ExitProcess
// safety net). The ORDER is the whole point:
//   1. lower the flag and WAIT for the thread to join (capped at 2 s);
//   2. unpublish g_sink with one atomic exchange — the only write, and
//      mix_grain only ever does one read of it, into a local;
//   3. only close and destroy if the thread actually joined.
// Closing while the thread hasn't joined would release the sceAudioOut port
// while that thread might still be blocked in sceAudioOutOutput on the same
// port — a teardown-time crash.
void shutdown() {
    bool joined = true;
    if (g_threadRun.exchange(0)) joined = audio::thread_stop();
    audio::Sink* s = g_sink.exchange(nullptr, std::memory_order_acq_rel);
    if (!s) return;
    jpline("[son] arret: grains=%llu trames=%llu (%.1f s) voix creees=%llu rest0=%llu gaps=%llu"
           " sansvue=%llu uncoup=%llu/%llu resamp=%llu crete=%llu errsortie=%llu",
           (unsigned long long)c_grains.load(), (unsigned long long)c_frames.load(),
           (double)c_frames.load() / (double)kRate,
           (unsigned long long)c_created.load(), (unsigned long long)c_famine.load(), s->gaps(),
           (unsigned long long)c_nohost.load(), (unsigned long long)c_endnl.load(),
           (unsigned long long)c_playnl.load(), (unsigned long long)c_resamp.load(),
           (unsigned long long)c_peakall.load(), s->write_errors());
    jpline("[son] arret (G129): voix max simultanees=%llu melange max=%llu us (moyenne %llu us/grain) verrous=%llu"
           " en retard=%llu play=%llu stop=%llu",
           (unsigned long long)c_peakvox.load(), (unsigned long long)c_mixmaxall.load(),
           c_grains.load() ? (unsigned long long)(c_mixus.load() / c_grains.load()) : 0ull,
           (unsigned long long)c_locks.load(), (unsigned long long)c_late.load(),
           (unsigned long long)c_play.load(), (unsigned long long)c_stop.load());
    jpline("[son] arret (G130): silence de flux: %llu regions, %.0f ms | verrous complets (effacements)=%llu DirectSoundCreate=%llu tampons liberes=%llu",
           (unsigned long long)c_silRegionsAll.load(), 1000.0 * (double)c_silFramesAll.load() / (double)kRate,
           (unsigned long long)c_clearlocks.load(), (unsigned long long)c_dscreate.load(), (unsigned long long)c_bufrel.load());
    { char b[900];
      if (detail_line(0, b, sizeof b) > 0) jpline("[son] arret feed(derniere fenetre): %s", b);
      if (s->timing_line(b, sizeof b) > 0) jpline("[son] arret sink(derniere fenetre): %s", b);
      int i = 2; while (detail_line(i, b, sizeof b) > 0 && i < 2 + kPsRows + 2) { jpline("[son] %s", b); ++i; } }
    if (joined) { s->close(); delete s; }
    else jpline("[son] le fil audio n'a PAS joint : puits NI ferme NI detruit"
                " (le port reste au noyau, le processus sort juste apres) — VOLONTAIRE");
}

int stat_line(char* out, unsigned n) {
    if (!g_on || !out || !n) return 0;
    static unsigned long long p_gr = 0, p_fa = 0, p_mix = 0, p_out = 0, p_fr = 0;
    const unsigned long long gr = c_grains.load(), fa = c_famine.load(),
        mu = c_mixus.load(), ou = c_outus.load(), fr = c_frames.load();
    const unsigned long long dgr = gr - p_gr, dfa = fa - p_fa,
        dmu = mu - p_mix, dou = ou - p_out, dfr = fr - p_fr;
    p_gr = gr; p_fa = fa; p_mix = mu; p_out = ou; p_fr = fr;
    unsigned long long rmin = c_restmin.load(); c_restmin.store(0xffffffffull);
    char rbuf[24];
    if (rmin == 0xffffffffull) std::snprintf(rbuf, sizeof rbuf, "-");
    else                       std::snprintf(rbuf, sizeof rbuf, "%llu", rmin);
    // Window amplitude. crete = max |sample| sent to the sink (0 means the
    // mixer produces digital silence — not necessarily a dead port); rms =
    // root of the average energy. Both reset here: window measurements, not
    // running totals.
    const unsigned long long pk = c_peak.exchange(0, std::memory_order_relaxed);
    const unsigned long long sq = c_sqsum.exchange(0, std::memory_order_relaxed);
    const unsigned long long sn = c_sqn.exchange(0, std::memory_order_relaxed);
    const double rms = sn ? std::sqrt((double)sq / (double)sn) : 0.0;
    // Streaming rings fed. A buffer >= 64 KiB has the shape of a music/stream
    // ring; if all of them show zero KiB locked, the game's stream pump isn't
    // running, and the missing audio isn't an output problem.
    unsigned nflux = 0, nfluxOk = 0;
    { std::lock_guard<std::mutex> lk(g_mx);
      for (const Voice& v : g_voices) {
          if (!v.alive || v.primary || v.len < 64u * 1024u) continue;
          nflux++; if (v.lockKio) nfluxOk++; } }
    audio::Sink* sk = g_sink.load(std::memory_order_acquire);
    int r = std::snprintf(out, n,
        // iteration-G130: "famine" (G129) counted rest_samples()==0 right before each write, which a blocking
        // single-buffered port satisfies on EVERY block by construction. It is kept, renamed rest0 (informational),
        // and the real discontinuity count comes from the sink's own submit timing: gaps (total since start).
        "audio(win): voix=%llu/%llu creees=%llu grains=%llu rest0=%llu gaps=%llu us/grain=%llu sortie=%llu"
        " rest=%s crete=%llu rms=%.0f gain0=%llu errsortie=%llu flux=%u/%u"
        " verrou=%lluKio play=%llu stop=%llu s=%.1f sansvue=%llu"
        " uncoup=%llu/%llu resamp=%llu puits=%s"
        " vmax=%llu mixmax=%lluus verrous=%llu retard=%llu retardmax=%llums",
        (unsigned long long)c_mixed.load(), (unsigned long long)g_voices.size(),
        (unsigned long long)c_created.load(), dgr, dfa, sk ? sk->gaps() : 0ull,
        dgr ? dmu / dgr : 0ull, dgr ? dou / dgr : 0ull,
        rbuf, pk, rms,
        (unsigned long long)c_gain0.load(),
        sk ? sk->write_errors() : 0ull, nfluxOk, nflux,
        (unsigned long long)c_lockKio.load(), (unsigned long long)c_play.load(),
        (unsigned long long)c_stop.load(), (double)dfr / (double)kRate,
        (unsigned long long)c_nohost.load(),
        (unsigned long long)c_endnl.load(), (unsigned long long)c_playnl.load(),
        (unsigned long long)c_resamp.load(), sink_name(),
        (unsigned long long)c_peakvox.load(),
        (unsigned long long)c_mixmax.exchange(0, std::memory_order_relaxed),
        (unsigned long long)c_locks.load(), (unsigned long long)c_late.load(),
        (unsigned long long)c_latemax.exchange(0, std::memory_order_relaxed));
    // Host-provided tail (e.g. a codec decode cost running on the game
    // thread) — without it, that cost would be misattributed to the mixer.
    // The engine doesn't know what it is, it just makes room for it. snprintf
    // may have truncated: r could then be >= n, hence the bound below.
    if (g_extra && r > 0 && (unsigned)r + 8u < n)
        r += g_extra(out + r, n - (unsigned)r);
    return r < 0 ? 0 : r;
}

// iteration-G130: compact, bounded diagnostics (see ds_emul.h). part 0 = stream feed for the window since the
// previous part-0 call; part 1 = the sink's own timing window; part 2+k = Play/Stop row k. Called a few times per
// session from the host, never per grain.
int detail_line(int part, char* out, unsigned n) {
    if (!g_on || !out || !n) return 0;
    if (part == 0) {
        static uint64_t t0 = 0, pFr = 0, pCl = 0;
        const uint64_t tn = wx86_now_us();
        const double sec = t0 ? (double)(tn - t0) / 1e6 : 0.0; t0 = tn;
        auto X = [](std::atomic<unsigned long long>& a) { return (unsigned long long)a.exchange(0, std::memory_order_relaxed); };
        const unsigned long long late = X(w_bk0), b1 = X(w_bk1), b2 = X(w_bk2), b3 = X(w_bk3), b4 = X(w_bk4);
        const unsigned long long aN = X(w_aheadN), aSum = X(w_aheadSumMs), lSum = X(w_lateSumMs), lMax = X(w_lateMaxMs);
        const unsigned long long aMin = w_aheadMinMs.exchange(~0ull, std::memory_order_relaxed);
        const unsigned long long lb = X(w_lockBytes), sr = X(w_silRegions), sf = X(w_silFrames);
        const unsigned long long locks = late + aN;
        const unsigned long long fr = c_frames.load(), cl = c_clearlocks.load();
        const double dfr = (double)(fr - pFr); pFr = fr;
        const unsigned long long dcl = cl - pCl; pCl = cl;
        const double bps = g_streamBps.load(std::memory_order_relaxed) ? (double)g_streamBps.load(std::memory_order_relaxed) : 44100.0;
        // iteration-G131: integrity of the freshness tables (guard bands intact, size consistent, flags only 0/1).
        // Read-only scan of at most a few tens of KB, once per summary, under the same lock every writer takes.
        unsigned nStream = 0; unsigned long long cBad = 0, fBad = 0;
        { std::lock_guard<std::mutex> lk(g_mx);
          for (const Voice& v : g_voices) {
              if (!v.alive || !v.stream) continue;
              ++nStream;
              if (v.fresh.size() != (size_t)v.freshN + 2u * kFreshGuard) { ++cBad; continue; }
              for (uint32_t i = 0; i < kFreshGuard; i++)
                  if (v.fresh[i] != 0xA5 || v.fresh[v.fresh.size() - 1 - i] != 0xA5) { ++cBad; break; }
              for (uint32_t q = 0; q < v.freshN; q++) if (v.fresh[kFreshGuard + q] > 1) ++fBad;
          } }
        c_freshChecks.fetch_add(nStream, std::memory_order_relaxed);
        c_canaryBad.fetch_add(cBad, std::memory_order_relaxed); c_flagBad.fetch_add(fBad, std::memory_order_relaxed);
        return std::snprintf(out, n,
            "feed win=%.1fs locks=%llu (%.1f/s) bytes=%llu (%.0f B/s, expected %.0f) game_audio=%.2fs vita_audio=%.2fs"
            " | lead on-time n=%llu avg=%.1f min=%lld ms | where: late=%llu 0-46ms=%llu 46-150ms=%llu 150-300ms=%llu >300ms=%llu"
            " | late avg=%.1f max=%llu ms | silence regions=%llu dur=%.0f ms | ring clears=%llu"
            " | dscreate=%llu bufrel=%llu | fresh tables: streams=%u checks=%llu canary_bad=%llu flag_bad=%llu (total) stream_switch=%s",
            sec, locks, sec > 0 ? (double)locks / sec : 0.0, lb, sec > 0 ? (double)lb / sec : 0.0, bps, (double)lb / bps,
            dfr / (double)kRate, aN, aN ? (double)aSum / (double)aN : 0.0, aN ? (long long)aMin : -1ll,
            late, b1, b2, b3, b4, late ? (double)lSum / (double)late : 0.0, lMax,
            sr, 1000.0 * (double)sf / (double)kRate, dcl,
            (unsigned long long)c_dscreate.load(), (unsigned long long)c_bufrel.load(),
            nStream, (unsigned long long)c_freshChecks.load(), (unsigned long long)c_canaryBad.load(), (unsigned long long)c_flagBad.load(),
            g_freshOff ? "OFF(WX86_FRESH=0)" : "ON");
    }
    if (part == 1) {
        audio::Sink* sk = g_sink.load(std::memory_order_acquire);
        return sk ? sk->timing_line(out, n) : 0;
    }
    PsRow r; unsigned long long dropped; int cnt;
    { std::lock_guard<std::mutex> lk(g_mx); cnt = g_psN; dropped = g_psDropped;
      const int k = part - 2; if (k < 0 || k > cnt) return 0;
      if (k < cnt) r = g_ps[k]; }
    const int k = part - 2;
    if (k == cnt) return std::snprintf(out, n, "playstop rows=%d dropped=%llu (play=%llu stop=%llu)", cnt, dropped,
                                       (unsigned long long)c_play.load(), (unsigned long long)c_stop.load());
    return std::snprintf(out, n, "playstop[%d] %s %s caller=0x%08x flags=0x%x n=%u first=%ums last=%ums", k,
                         r.op ? "Stop" : "Play", r.prim ? "primary" : "secondary", r.caller, r.flags, r.n, r.firstMs, r.lastMs);
}

// ---- sink-only self-test -----------------------------------------------------
// No DirectSound, no game, no thread: a 440 Hz sine written to a WAV, then
// read back and verified. Isolates "the sink works" from "DirectSound
// emulation works".
int selftest(const char* path, int ms, int rate) {
    // This test doesn't call install(), so its frequency can't come from the
    // engine — it comes from the caller, like everywhere else in this file.
    if (rate <= 0) { std::printf("=== [SONTEST] ECHEC: frequence non fournie\n"); return 2; }
    kRate = rate;
    audio::Sink* s = audio::make_wav_sink(path);
    if (!s->open(kRate, kOutCh, kGrain)) { std::printf("=== [SONTEST] ECHEC: ouverture %s\n", path); delete s; return 2; }
    const int total = (int)((int64_t)ms * kRate / 1000);
    int16_t buf[kGrain * kOutCh];
    double ph = 0.0; const double dp = 2.0 * 3.14159265358979323846 * 440.0 / (double)kRate;
    for (int done = 0; done < total; ) {
        const int n = (total - done) < kGrain ? (total - done) : kGrain;
        for (int i = 0; i < n; i++) {
            const int16_t v = (int16_t)(9000.0 * std::sin(ph)); ph += dp;
            buf[2*i] = v; buf[2*i+1] = v;
        }
        s->write(buf, n); done += n;
    }
    s->close(); delete s;
    audio::WavStats st; char rep[512];
    const bool ok = audio::wav_check(path, &st, rep, sizeof rep, kRate);
    std::printf("=== [SONTEST] %s\n=== [SONTEST] %s : sinusoide 440 Hz, %d ms demandes\n",
                rep, ok ? "PASS" : "ECHEC", ms);
    // Sink scale test: the duration written must match the duration requested.
    const double want = ms / 1000.0;
    if (ok && (st.seconds < want * 0.99 || st.seconds > want * 1.01)) {
        std::printf("=== [SONTEST] ECHEC: duree %.3f s pour %.3f s demandees\n", st.seconds, want);
        return 1;
    }
    return ok ? 0 : 1;
}

}} // namespace d2rt::dsound
