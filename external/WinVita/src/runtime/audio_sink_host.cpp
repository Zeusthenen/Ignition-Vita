// src/runtime/audio_sink_host.cpp — portable WAV sink and null sink.
//
// Compiled into ALL binaries (host, ARM oracle, Vita eboot): the WAV sink is
// the off-console evidence for the audio path, so it must exist in the qemu
// oracle binary too, not just the eboot.
#include "runtime/audio_sink.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdarg>

namespace d2rt { namespace audio {

namespace {

inline void put32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
inline void put16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
inline uint32_t get32(const uint8_t* p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
inline uint16_t get16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0]|((uint32_t)p[1]<<8)); }

class WavSink : public Sink {
public:
    explicit WavSink(const char* path) { std::snprintf(path_, sizeof path_, "%s", path ? path : ""); }
    ~WavSink() override { close(); }

    bool open(int freq, int ch, int /*grain*/) override {
        freq_ = freq; ch_ = ch;
        f_ = std::fopen(path_, "wb");
        if (!f_) { std::fprintf(stderr, "[son] puits wav: ouverture impossible '%s'\n", path_); return false; }
        write_header(0);
        return true;
    }
    void write(const int16_t* pcm, int frames) override {
        if (!f_ || frames <= 0) return;
        const size_t n = (size_t)frames * (size_t)ch_;
        if (std::fwrite(pcm, sizeof(int16_t), n, f_) != n) return;
        data_ += (uint32_t)(n * sizeof(int16_t));
        // Rewrite the header every ~64 KiB: a killed run (timeout, Halt)
        // then leaves a READABLE file instead of a zero-size RIFF.
        if (data_ - lastHdr_ >= 64u * 1024u) { lastHdr_ = data_; refresh_header(); }
    }
    void close() override {
        if (!f_) return;
        refresh_header();
        std::fclose(f_); f_ = nullptr;
    }
    const char* name() const override { return "wav"; }

private:
    void write_header(uint32_t data) {
        uint8_t h[44];
        std::memcpy(h, "RIFF", 4);        put32(h+4, 36 + data);
        std::memcpy(h+8, "WAVEfmt ", 8);  put32(h+16, 16);
        put16(h+20, 1); put16(h+22, (uint16_t)ch_);
        put32(h+24, (uint32_t)freq_);
        put32(h+28, (uint32_t)(freq_ * ch_ * 2));
        put16(h+32, (uint16_t)(ch_ * 2)); put16(h+34, 16);
        std::memcpy(h+36, "data", 4);     put32(h+40, data);
        std::fwrite(h, 1, sizeof h, f_);
    }
    void refresh_header() {
        long cur = std::ftell(f_);
        if (cur < 0) return;
        std::fseek(f_, 0, SEEK_SET);
        write_header(data_);
        std::fseek(f_, cur, SEEK_SET);
        std::fflush(f_);
    }
    char  path_[256] = {0};
    FILE* f_ = nullptr;
    int   freq_ = 0, ch_ = 2;      // set by open(), never guessed here
    uint32_t data_ = 0, lastHdr_ = 0;
};

class NullSink : public Sink {
public:
    bool open(int, int, int) override { return true; }
    void write(const int16_t*, int) override {}
    void close() override {}
    const char* name() const override { return "null"; }
};

} // namespace

Sink* make_wav_sink(const char* path) { return new WavSink(path); }
Sink* make_null_sink() { return new NullSink(); }

#ifndef __vita__
// No host thread outside the console: the mixer falls back to the sink
// PULLED by the game's frame tick. Deliberate — under qemu, a real-time
// thread would starve the sink and the WAV evidence would be full of gaps.
bool thread_start(void (*)(void)) { return false; }
bool thread_retry(void) { return false; }
bool thread_stop(void) { return true; }

// Outside the console there's no sceAudioOut: this factory exists so
// ds_emul.cpp compiles identically everywhere, and returns null (the mixer
// then falls back to the null sink, and SAYS so in the log).
Sink* make_vita_sink() { return nullptr; }
#endif

// ---------------------------------------------------------------------------
// SPECTRAL ANALYSIS — the only test that tells MUSIC apart from WHITE NOISE.
// In-place radix-2 FFT, 1024 real points treated as complex (zero imaginary
// part): ~30 lines, no dependency, and it runs in the oracle binary under
// qemu just like in the eboot.
namespace {
constexpr int kFftN = 1024;

void fft1024(double* re, double* im) {
    // bit-reversal permutation
    for (int i = 1, j = 0; i < kFftN; i++) {
        int bit = kFftN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t;
                     t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= kFftN; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / (double)len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < kFftN; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                const double ur = re[i+k],        ui = im[i+k];
                const double vr = re[i+k+len/2] * cr - im[i+k+len/2] * ci;
                const double vi = re[i+k+len/2] * ci + im[i+k+len/2] * cr;
                re[i+k] = ur + vr;        im[i+k] = ui + vi;
                re[i+k+len/2] = ur - vr;  im[i+k+len/2] = ui - vi;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;   cr = nr;
            }
        }
    }
}

// Accumulator: fed MONO samples (channel average), it averages the power
// spectrum of each full window.
struct Spectrum {
    double win[kFftN] = {0};
    double pow_[kFftN/2] = {0};
    double hann[kFftN];
    int    fill = 0;
    uint32_t nwin = 0;
    uint32_t skip = 0, seen = 0;      // only analyze one window out of every `skip+1`
    Spectrum() { for (int i = 0; i < kFftN; i++)
                     hann[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / (kFftN - 1)); }
    void push(double v) {
        win[fill++] = v;
        if (fill < kFftN) return;
        fill = 0;
        if (skip && (seen++ % (skip + 1))) return;
        static double re[kFftN], im[kFftN];
        for (int i = 0; i < kFftN; i++) { re[i] = win[i] * hann[i]; im[i] = 0.0; }
        fft1024(re, im);
        for (int k = 0; k < kFftN/2; k++) pow_[k] += re[k]*re[k] + im[k]*im[k];
        nwin++;
    }
    // Share of energy in the 20 dominant bins, DC excluded (a constant
    // component isn't sound and would inflate the measurement).
    double concentration(int rate, int* top_hz) const {
        double tot = 0, best = 0; int bestk = 0;
        for (int k = 1; k < kFftN/2; k++) { tot += pow_[k];
            if (pow_[k] > best) { best = pow_[k]; bestk = k; } }
        if (top_hz) *top_hz = (int)((double)bestk * rate / kFftN + 0.5);
        if (tot <= 0) return 0.0;
        double top[20] = {0};
        for (int k = 1; k < kFftN/2; k++) {
            double v = pow_[k];
            for (int j = 0; j < 20; j++) if (v > top[j]) { double t = top[j]; top[j] = v; v = t; }
        }
        double s20 = 0; for (int j = 0; j < 20; j++) s20 += top[j];
        return s20 / tot;
    }
};
} // namespace

bool wav_check(const char* path, WavStats* out, char* report, unsigned n, int want_rate) {
    WavStats st;
    auto say = [&](const char* fmt, ...) {
        if (!report || !n) return;
        va_list ap; va_start(ap, fmt); std::vsnprintf(report, n, fmt, ap); va_end(ap);
    };
    FILE* f = std::fopen(path, "rb");
    if (!f) { say("ABSENT: %s", path); if (out) *out = st; return false; }
    uint8_t h[44];
    if (std::fread(h, 1, sizeof h, f) != sizeof h) { std::fclose(f); say("TRONQUE (< 44 o d'en-tete)"); if (out) *out = st; return false; }
    if (std::memcmp(h, "RIFF", 4) || std::memcmp(h+8, "WAVE", 4)) {
        std::fclose(f); say("PAS UN RIFF/WAVE"); if (out) *out = st; return false; }
    st.channels = get16(h+22); st.rate = get32(h+24); st.bits = get16(h+34);
    uint32_t dataLen = get32(h+40);
    if (get16(h+20) != 1 || st.bits != 16 || st.channels < 1 || st.channels > 2) {
        std::fclose(f); say("format inattendu (tag=%u ch=%d bits=%d)", get16(h+20), st.channels, st.bits);
        if (out) *out = st; return false; }

    uint64_t nz = 0, tot = 0, clipped = 0; double acc = 0; int peak = 0;
    int16_t buf[2048];
    uint64_t left = dataLen / 2;   // declared samples
    static Spectrum sp; sp = Spectrum();
    // Bound the work: at most ~1500 windows analyzed, spread across the
    // whole file. A 600 s WAV holds 12,900; under qemu, analyzing all of
    // them would cost more than the run that produced it.
    { const uint64_t frames_tot = st.channels ? (dataLen / 2) / (uint64_t)st.channels : 0;
      const uint64_t wtot = frames_tot / kFftN;
      sp.skip = (uint32_t)(wtot > 1500 ? (wtot / 1500) : 0); }
    while (left) {
        size_t want = (size_t)(left < 2048 ? left : 2048);
        size_t got = std::fread(buf, sizeof(int16_t), want, f);
        if (!got) break;
        left -= got;
        for (size_t i = 0; i < got; i++) {
            int v = buf[i]; int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            if (v >= 32767 || v <= -32767) clipped++;
            acc += (double)v * (double)v;
        }
        // per-FRAME silence check (all channels zero) + feeding the spectrum
        for (size_t i = 0; i + (size_t)st.channels <= got; i += (size_t)st.channels) {
            bool z = true; double mono = 0;
            for (int c = 0; c < st.channels; c++) { if (buf[i+c]) z = false; mono += buf[i+c]; }
            tot++; if (!z) nz++;
            sp.push(mono / (double)st.channels);
        }
    }
    std::fclose(f);
    st.frames  = tot;
    st.seconds = st.rate ? (double)tot / (double)st.rate : 0.0;
    st.peak    = peak;
    st.rms     = tot ? std::sqrt(acc / (double)(tot * (uint64_t)st.channels)) : 0.0;
    st.silence_frac = tot ? 1.0 - (double)nz / (double)tot : 1.0;
    st.clipped = clipped;
    st.windows = sp.nwin;
    if (sp.nwin) st.tone_frac = sp.concentration((int)st.rate, &st.top_hz);

    const char* why = nullptr;
    if (!tot)                                     why = "aucune trame";
    else if (want_rate && (int)st.rate != want_rate) why = "frequence inattendue";
    else if (peak == 0)                           why = "SILENCE TOTAL (amplitude crete nulle)";
    else if (st.silence_frac > 0.999)             why = "99,9 % de trames muettes";
    else if (clipped * 100 > (tot * (uint64_t)st.channels))  why = "SATURATION (>1 % d'echantillons ecretes)";
    // The missing test: white noise passes ALL the previous checks. 20 bins
    // out of 512 amount to 3.9% for white noise; reject below 8%.
    else if (sp.nwin >= 8 && st.tone_frac < 0.08)  why = "BRUIT LARGE BANDE (energie etalee, pas un signal structure)";
    st.ok = (why == nullptr);
    say("%s: %.2f s  %u Hz  %dch/%d bits  trames=%llu  crete=%d (%.1f%% pleine echelle)  rms=%.0f  silence=%.1f%%"
        "  ecretes=%llu  tonalite=%.1f%% (raie dominante %d Hz, %u fenetres)%s%s",
        st.ok ? "OK" : "ECHEC", st.seconds, st.rate, st.channels, st.bits,
        (unsigned long long)st.frames, st.peak, 100.0 * st.peak / 32767.0, st.rms,
        100.0 * st.silence_frac, (unsigned long long)st.clipped,
        100.0 * st.tone_frac, st.top_hz, st.windows,
        why ? "  -> " : "", why ? why : "");
    if (out) *out = st;
    return st.ok;
}

}} // namespace d2rt::audio
