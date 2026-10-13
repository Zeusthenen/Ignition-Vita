// src/runtime/audio_sink.h — THE AUDIO SINK, swappable.
//
// Guiding idea: separate "how much DirectSound" (the guest COM layer,
// ds_emul.cpp) from "how it comes out" (this file). The mixer only knows a
// `write(pcm, frames)` interface; behind it, three implementations:
//   * wav  — writes a RIFF/WAVE file to the write folder. THE off-console
//            evidence: a file you can listen to on a dev machine.
//   * null — discards samples. The mixer still runs, so playback cursors
//            still advance: this is the leg that measures the guest cost of
//            audio before any DSP work exists.
//   * vita — sceAudioOut (src/platform/vita_audio.cpp), console only.
//
// No sink is OPENED unless D2_SON is armed — on by default; D2_SON=0 keeps
// this file inert.
#pragma once
#include <cstdint>

namespace d2rt { namespace audio {

struct Sink {
    virtual ~Sink() {}
    // freq/channels of the mixed stream, as the EMBEDDER gave them
    // (ds_emul.h HostOps::mix_rate) — the engine imposes none of its own.
    // `grain` = frames per write() call; a sink may ignore it.
    virtual bool open(int freq, int channels, int grain) = 0;
    // CAN BLOCK (sceAudioOutOutput blocks ~23 ms). Never called holding the GIL.
    virtual void write(const int16_t* interleaved, int frames) = 0;
    virtual void close() = 0;
    virtual const char* name() const = 0;
    // Samples still in flight on the driver side; -1 = the sink doesn't know.
    // 0 on a real-time sink = STARVATION (the only detector the Vita SDK
    // offers, sceAudioOutGetRestSample).
    virtual int rest_samples() { return -1; }
    // WRITES REFUSED BY THE DRIVER. The output call's return code used to be
    // DISCARDED: a refusing port returns immediately, the audio thread's loop
    // loses its ONLY clock (that blocking call), it busy-waits burning a
    // core, playback cursors run away, every voice "finishes" right away —
    // and the sound disappears without a single log line. Published via the
    // errsortie= field of the counters line.
    virtual unsigned long long write_errors() const { return 0; }
    // Does the sink impose its own pace (real-time)? False for wav/null:
    // those are PULLED by the game's frame tick, so the duration produced
    // tracks guest time even when qemu runs at a tenth of real speed — no
    // gaps, no pitch distortion.
    virtual bool self_paced() const { return false; }
    // ---- iteration-G130 (appended LAST so existing vtable slots keep their index) ----
    // REAL output discontinuities, as measured by the sink itself: a block submitted
    // later than 1.5 block-times after the previous one (so the port had to run dry).
    // rest_samples()==0 right before a write is NOT that: a blocking, single-buffered
    // port is empty by construction when the next block is mixed (G129 counted it as
    // "famine" on every block). 0 for sinks that don't measure.
    virtual unsigned long long gaps() const { return 0; }
    // One compact line of the sink's own timing and thread placement for the window
    // since the previous call (cadence, pauses, time blocked in the output call,
    // cores, priority). Returns the bytes written, 0 = nothing to report.
    virtual int timing_line(char* out, unsigned n) { (void)out; (void)n; return 0; }
};

// ---- OUTPUT HOST THREAD -----------------------------------------------------
// On Vita: sceKernelCreateThread + auto-affinity pinning as the FIRST
// instruction of the thread (a mask set by the creator reads back as 0 and
// the thread migrates otherwise). Everywhere else: returns false, and the
// mixer falls back to the sink PULLED by the frame tick — so NO thread under
// qemu, exactly what a deterministic proof needs.
//
// The cascade: thread_start tries several (priority, stack) pairs — proven
// first, then smaller stacks, then the highest LEGAL relative priority — and
// LOGS the rc of each rung, plus free memory and host thread count before
// and after. The log therefore says either "AUDIO THREAD STARTED (try N,
// priority X, stack Y)" or "AUDIO THREAD NOT STARTED after N tries — rc:
// [1]=... [2]=...", never a bare "FAILED": without the rcs, several
// hypotheses stay alive with no way to tell them apart.
bool thread_start(void (*body)(void));
// FALLBACK OUTSIDE THE INIT PATH: replays the full cascade, once per call.
// thread_start runs during audio device creation, at the low point of the
// memory curve; a few seconds later the heap has recovered. Returns true if
// the thread is running (including if it already was).
bool thread_retry(void);
// Returns true only if the thread ACTUALLY joined. False => it may still be
// running, and destroying the sink out from under it would be a
// use-after-free during teardown — the same "at shutdown" crash family seen
// elsewhere.
bool thread_stop(void);

Sink* make_wav_sink(const char* path);
Sink* make_null_sink();
// Provided by src/platform/vita_audio.cpp on __vita__, null everywhere else.
Sink* make_vita_sink();

// ---- WAV VERIFIER -----------------------------------------------------------
// Reads back a file produced by make_wav_sink and writes a readable verdict
// into `report`. Returns true if the file is a coherent PCM WAV AND carries
// signal: duration > 0, expected frequency, nonzero peak amplitude, not 100%
// silence, no massive clipping. That's exactly what off-console evidence
// needs: "neither silent nor clipped" doesn't show up in a log.
//
// PLUS a spectral measurement, because everything above would also be true
// of WHITE NOISE. `tone_frac` is the share of spectral energy carried by the
// 20 dominant bins of a 512-bin spectrum (1024-point FFT, Hann window,
// averaged over the whole file). White noise spreads its energy: 20/512 ≈
// 4%. Music or speech concentrates it: 40% and up. This check now lives IN
// the binary, so it's replayable and regression-testable.
struct WavStats {
    bool     ok = false;
    uint32_t rate = 0; int channels = 0, bits = 0;
    uint64_t frames = 0;
    double   seconds = 0;
    int      peak = 0;          // max |sample|
    double   rms = 0;           // overall RMS (0..32768)
    double   silence_frac = 0;  // share of frames that are all zero
    uint64_t clipped = 0;       // samples at +-32767/-32768
    double   tone_frac = 0;     // share of energy in the 20 dominant bins (0..1)
    int      top_hz = 0;        // frequency of the dominant bin
    uint32_t windows = 0;       // 1024-point windows analyzed
};
// want_rate = 0: frequency is NOT checked — a nonzero default here would
// bake one consumer's expected rate into an otherwise generic header.
bool wav_check(const char* path, WavStats* st, char* report, unsigned n,
               int want_rate = 0);

}} // namespace d2rt::audio
