// COM/vtable-based DirectSound guest emulation.
//
// The guest binary only imports 2 DSOUND.dll symbols by ordinal — everything
// else goes through COM vtables, synthesized here: Bridge::shim_trap
// returns a guest VA for a shim, and a table of such VAs IS a vtable. x86 COM
// methods are stdcall with `this` pushed first, so Shim{argc = 1 + nparams,
// stdcall_cleanup = true} with cpu.arg(0) == this covers it — no Bridge
// changes, no hand-written assembly.
//
// D2_SON is armed by default; with D2_SON=0, DirectSoundCreate returns
// DSERR_NODRIVER (0x88780078) instead — no thread, no library, no vtable
// touches guest memory. The 32 trap slots are
// still allocated unconditionally: alloc_trap advances the allocator by a
// fixed 16 bytes per slot, so conditional registration would shift guest
// memory layout between the enabled/disabled paths for reasons unrelated to
// the toggle itself (see shim_trap_existing in bridge.h). Slot allocation
// alone never touches guest memory.
#pragma once
#include <cstdint>

namespace d2rt { class Bridge; class Cpu; }

namespace d2rt { namespace dsound {

// What the embedding host provides to the engine: only the fields that
// genuinely depend on the host — where its guest buffers live, what guest
// clock it exposes, and where it writes files.
struct HostOps {
    uint32_t (*va_alloc)(uint32_t n) = nullptr; // guest address arena — PCM buffers
    uint32_t (*tick_ms)(Cpu& c)    = nullptr;   // guest clock in ms (timeGetTime)
    const char* write_root         = nullptr;   // host write directory
    // Mixed-stream frequency in Hz — the rate at which the mixer needs zero
    // resampling. This belongs to the game, not the engine: a hardcoded
    // default here would silently mis-pitch any game sampled at another rate.
    //
    // 0 means the host did not provide it. The engine then refuses to arm
    // itself and reports why, rather than guessing a game's sample rate.
    int mix_rate = 0;
};

// Log sink. The engine has no console of its own — on Vita, printf output is
// invisible, so an unwired logger would go silent exactly when it's needed
// most. Unset: lines go to stdout, which is fine for desktop and qemu.
typedef void (*LogFn)(const char* ligne);
void set_logger(LogFn cb);

// Tail of the counters line. The engine publishes what it measures itself
// (voices, grains, starvation, sink…); whatever the host measures above that
// is its own to report, appended to the same line so it isn't misattributed
// to the mixer (e.g. a codec decode cost running on the game thread). The
// callback writes at most n bytes into out and returns the byte count written.
typedef int (*ExtraStatFn)(char* out, unsigned n);
void set_extra_stat(ExtraStatFn cb);

// Fires once, when the engine reports dwMaxHw3DAllBuffers = 0 (no hardware 3D
// buffers) — the same capability profile as a PC with no 3D card, though from
// the player's side it can look like a bug. The engine doesn't know a game's
// menu option names or which ones depend on this capability, so it only
// signals the cause; the host maps that to consequences for its own UI.
typedef void (*CapsObserverFn)();
void set_caps_observer(CapsObserverFn cb);

// Registers the 2 DSOUND exports plus the 32 COM methods and forces their
// trap slots to be allocated. Call once, before Bridge::link().
void install(Bridge& br, Cpu& cpu, const HostOps& ops);

bool enabled();                 // is D2_SON set?
const char* sink_name();        // "wav" / "null" / "vita" / "-"

// Drives the host sink (wav/null) from the game's frame tick: each presented
// frame produces exactly the frames worth of elapsed guest time. No thread,
// no pinning, no GIL release — WAV duration tracks guest time even when qemu
// runs slower than real time. No-op under a self-clocked sink (vita) or when
// audio is off.
void frame_pump(Cpu& cpu);

// Stops the audio thread (vita sink) and closes the sink. Call before
// end-of-run reporting, alongside the ring-buffer flush thread shutdown.
void shutdown();

// Only stats accessor. Writes the 10 s window line into `out` (called by the
// Vita watchdog, vita_present.cpp) and returns bytes written, 0 if audio is
// off. Fields:
//   voix=<mixed>/<total>  creees=  grains=  famine=  us/grain=  sortie=
//   rest=  verrou=<KiB>  play=  stop=  s=<window seconds>  sansvue=
//   uncoup=<one-shot voices finished>/<Play without DSBPLAY_LOOPING>
//   resamp=<grains mixed at a rate != the configured mix rate>  puits=  codec=h/a/fallback
int stat_line(char* out, unsigned n);

// iteration-G130: bounded diagnostic lines, a few per session (never per grain / per Lock).
//   part 0   stream feed since the previous part-0 call: locks/s, bytes/s vs expected, audio the game produced
//            vs audio the sink consumed, how far ahead of the play cursor the writes land (on-time / inside the
//            write-cursor guard / late), stale-ring silence substituted, ring clears
//   part 1   the sink's own timing window (cadence, pauses >30/50/100/250 ms, time blocked in the output call,
//            thread cores/priority, REAL gaps)
//   part 2+k Play/Stop folded by (op, buffer, caller EIP, flags); the row after the last one is the summary
// Returns the bytes written, 0 when there is nothing (more) to report.
int detail_line(int part, char* out, unsigned n);

// Sink-only self-test (D2_SONTEST=1): pushes a 440 Hz sine into a WAV sink,
// then reads the file back and verifies it. Isolates "the sink works" from
// "DirectSound emulation works". Returns 0 on PASS.
int selftest(const char* path, int ms, int rate);

}} // namespace d2rt::dsound
