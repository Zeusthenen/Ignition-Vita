// music/music_vita.cpp -- MusicPlatform on the PS Vita: the music goes to a SECOND audio port (MAIN, 48 kHz, 1024-frame
// blocks), written by its own thread. The DirectSound effects keep their own BGM port and thread untouched.
#include "music/music_vita.h"
#include "diagnostics/log.h"

#ifdef __vita__
#include "platform/vita_host.h"          // wx86_vita_pin_self / wx86_vita_progress
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ign { namespace music {

#ifdef __vita__
namespace {

class VitaMusicPlatform : public MusicPlatform {
public:
    VitaMusicPlatform() { creator_ = sceKernelGetThreadId(); }      // built on the game thread (boot_run), which is also the mciSendStringA caller

    uint64_t now_us() override { return (uint64_t)sceKernelGetProcessTimeWide(); }

    int out_open(int blockFrames, int rate) override {
        port_ = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, blockFrames, rate, SCE_AUDIO_OUT_MODE_STEREO);
        if (port_ < 0) return port_;
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        const int vrc = sceAudioOutSetVolume(port_, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
        char m[128]; snprintf(m, sizeof m, "music: port MAIN abierto (port=%d len=%d %d Hz stereo) volumen 0dB rc=0x%08x", port_, blockFrames, rate, (unsigned)vrc);
        wx86_vita_progress(m);
        return 0;
    }
    int out_write(const int16_t* stereo) override {
        const int rc = sceAudioOutOutput(port_, stereo);
        if (rc < 0) { if (++consec_ >= 4) sceKernelDelayThread(21000); return rc; }       // a refusing port must not become a busy loop
        consec_ = 0; wrote_ = true;
        return rc;
    }
    void out_close(bool drain) override {
        if (port_ < 0) return;
        if (drain && wrote_) sceAudioOutOutput(port_, nullptr);                          // waits for the last block
        sceAudioOutReleasePort(port_);
        char m[96]; snprintf(m, sizeof m, "music: port MAIN %s", wrote_ ? "drenado y liberado" : "liberado sin escribir");
        wx86_vita_progress(m);
        port_ = -1; wrote_ = false;
    }
    void thread_enter() override {
        self_ = sceKernelGetThreadId();
        // Placement: core 1 (the quiet core: the DirectSound effects thread lives there, the game was observed elsewhere in G130)
        // and an effective priority BELOW the effects thread (0x80) but not above the game (0xA0): relative 0x100000F0 = 0x90.
        int core = 1; int prio = 0x100000F0;
        if (const char* e = getenv("WX86_MUSICCPU")) { const int c = atoi(e); if (c >= 0 && c <= 2) core = c; }
        if (const char* e = getenv("WX86_MUSICPRIO")) { const long v = strtol(e, nullptr, 0); if (v) prio = (int)v; }
        unsigned relu = 0;
        const int rcAff = wx86_vita_pin_self((int)(0x10000u << core), &relu);
        const int rcPri = sceKernelChangeThreadPriority(self_, prio);
        SceKernelThreadInfo ti; memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        const int rcInfo = sceKernelGetThreadInfo(self_, &ti);
        char m[220];
        snprintf(m, sizeof m, "music: hilo core pedido=%d (rc=0x%08x) prioridad pedida=0x%08x (rc=0x%08x) | efectivo prio=0x%x core=%d affmask=0x%x (rc=%d)",
                 core, (unsigned)rcAff, (unsigned)prio, (unsigned)rcPri, (unsigned)ti.currentPriority, (int)ti.currentCpuId, (unsigned)ti.currentCpuAffinityMask, rcInfo);
        wx86_vita_progress(m);
        IGN_LOG("[G132-MUSIC]", "thread placement: %s", m);
    }
    int thread_probe(int* musicCore, int* musicPrio, int* gameCore, uint64_t* cpuUs) override {
        SceKernelThreadInfo a, g; memset(&a, 0, sizeof a); memset(&g, 0, sizeof g); a.size = g.size = sizeof a;
        if (self_ > 0 && sceKernelGetThreadInfo(self_, &a) == 0) { *musicCore = a.currentCpuId; *musicPrio = a.currentPriority; *cpuUs = (uint64_t)a.runClocks; }
        if (creator_ > 0 && sceKernelGetThreadInfo(creator_, &g) == 0) *gameCore = g.currentCpuId;
        return 0;
    }
    void log(const char* line) override { IGN_LOG("[G132-MUSIC]", "%s", line); }
private:
    int port_ = -1, consec_ = 0; bool wrote_ = false;
    SceUID creator_ = -1, self_ = -1;
};

} // namespace

std::unique_ptr<MusicPlatform> make_platform() { return std::unique_ptr<MusicPlatform>(new VitaMusicPlatform()); }

#else   // host build of the sources (never linked into the Vita executable): a platform that refuses to open a port

namespace {
class NullMusicPlatform : public MusicPlatform {
public:
    uint64_t now_us() override { return 0; }
    int out_open(int, int) override { return -1; }
    int out_write(const int16_t*) override { return -1; }
    void out_close(bool) override {}
    void thread_enter() override {}
    int thread_probe(int*, int*, int*, uint64_t*) override { return -1; }
    void log(const char* line) override { IGN_LOG("[G132-MUSIC]", "%s", line); }
};
}
std::unique_ptr<MusicPlatform> make_platform() { return std::unique_ptr<MusicPlatform>(new NullMusicPlatform()); }
#endif

}} // namespace ign::music
