#include "win32/winmm_ign.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/host_clock.h"
#include "runtime/ds_emul.h"
#include "win32/dinput.h"
#include "win32/glide2x_ign.h"
#include "diagnostics/log.h"
#include "music/music_service.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>

using namespace d2rt;

// iteration-G14: same cross-TU plain-C-global counter pattern as
// user32_window.cpp's g_ign_msg_queue_depth/g_ign_*message_count --
// read by win32/glide2x_ign.cpp's frame-loop diagnostic summary.
extern "C" {
    volatile uint32_t g_ign_timegettime_count = 0;
    volatile uint32_t g_ign_timesetevent_count = 0;
    volatile uint32_t g_ign_timekillevent_count = 0;
    volatile uint32_t g_ign_joygetposex_count = 0;
    // iteration-G15, Parte G: aggregate counter for EVERY WINMM shim
    // registered through this file's own W() closure -- same centralized
    // pattern as user32_window.cpp's g_ign_user32_call_count.
    volatile uint32_t g_ign_winmm_call_count = 0;
    volatile uint32_t g_g99TimeGetTimeSerial = 0;
    volatile uint32_t g_g99TimeGetTimeValue = 0;
    volatile uint32_t g_g99TimeGetTimeCaller = 0;
}

namespace ign {

namespace {

std::string read_guest_cstr(Cpu& c, uint32_t va, size_t maxlen = 260) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < maxlen; ++i) {
        uint8_t b = 0;
        c.read(va + (uint32_t)i, &b, 1);
        if (!b) break;
        s.push_back((char)b);
    }
    return s;
}

// iteration-G129: sparse DirectSound service, driven from the guest's own
// timeGetTime calls (the one shim the game polls continuously, always on the
// guest thread). Costs one increment per call and acts only every 256th call.
//   * every ~1 s  : dsound::frame_pump -- a no-op under the self-clocked Vita
//                   sink; it only runs the deferred audio-thread retry (and the
//                   PULLED null/wav sinks, which never apply on hardware).
//   * every ~30 s : ONE summary line ([G129-DS] audio(10s): ...). The engine
//                   computes it from its own counters; nothing is logged per
//                   Lock/Play/grain/sample.
void ign_dsound_service(Cpu& c, uint32_t nowMs) {
    const bool ds = dsound::enabled();
    if (!ds && !music::service_enabled()) return;
    static bool armed = false;
    static uint32_t nextPump = 0, nextSummary = 0;
    if (!armed) { armed = true; nextPump = nowMs + 1000u; nextSummary = nowMs + 30000u; return; }
    if (ds && (int32_t)(nowMs - nextPump) >= 0) {
        nextPump = nowMs + 1000u;
        dsound::frame_pump(c);
    }
    if ((int32_t)(nowMs - nextSummary) >= 0) {
        nextSummary = nowMs + 30000u;
        char line[1100];
        // iteration-G132: the music line (only once something has played; the player reports nothing before that)
        { const int mn = music::service_summary(line, sizeof line); if (mn > 0) IGN_LOG("[G132-MUSIC]", "%s", line); }
        if (!ds) return;
        // iteration-G130/G131: three bounded lines per 30 s window -- the engine's counters, the stream feed (what
        // the game writes vs what the Vita consumes, how far ahead of the cursor, late/silenced stretches) and the
        // sink's own timing (cadence, pauses, time blocked in the output call, cores, priority, real gaps).
        int n = dsound::stat_line(line, sizeof line);
        if (n > 0) IGN_LOG("[G131-DS]", "%s", line);
        n = dsound::detail_line(0, line, sizeof line);
        if (n > 0) IGN_LOG("[G131-FEED]", "%s", line);
        n = dsound::detail_line(1, line, sizeof line);
        if (n > 0) IGN_LOG("[G131-SINK]", "%s", line);
        // Play/Stop folded by buffer/caller/flags: printed once, ~60 s into the session, and again at shutdown.
        static uint32_t windows = 0;
        if (++windows == 2u)
            for (int i = 2; i < 2 + 26 && (n = dsound::detail_line(i, line, sizeof line)) > 0; ++i) IGN_LOG("[G131-PLAYSTOP]", "%s", line);
    }
}

// iteration-G129/G132: every distinct command string is logged exactly once (never per poll), with its caller,
// approximate moment, the answer given and whether the virtual CD recognised it. Bounded: at most 64 distinct
// strings, then one overflow note. The same line goes to ignition_critical.log ([G35-MCI]) so it survives a
// relaunch. `status cdaudio mode` is polled constantly and therefore appears exactly once.
void ign_mci_capture(const std::string& cmd, uint32_t caller, uint32_t pRet, uint32_t retLen,
                     uint32_t hwnd, uint32_t mciReturn, const std::string& reply, bool backend, bool known) {
    static std::vector<std::string> seen;
    static bool overflowSaid = false;
    for (const std::string& s : seen) if (s == cmd) return;
    if (seen.size() >= 64) {
        if (!overflowSaid) { overflowSaid = true; IGN_LOG("[G132-MCI]", "more than 64 distinct MCI strings -- further ones not captured"); }
        return;
    }
    seen.push_back(cmd);
    const uint32_t swaps = ign_get_completed_swap_count();
    char moment[64];
    if (swaps == 0) snprintf(moment, sizeof moment, "boot (no frame presented yet)");
    else            snprintf(moment, sizeof moment, "running (after %u presented frames)", swaps);
    IGN_LOG("[G132-MCI]", "distinct command #%u: \"%s\" caller_eip=0x%08x moment=%s uptime_ms=%u retbuf=0x%08x retlen=%u hwnd=0x%08x -> mci_return=%u reply=\"%s\" (%s)",
            (unsigned)seen.size(), cmd.c_str(), caller, moment, (unsigned)wx86_now_ms(), pRet, retLen, hwnd, mciReturn, reply.c_str(),
            !backend ? "virtual CD disabled: honest generic error" : known ? "handled by the virtual CD" : "UNKNOWN to the virtual CD: honest error");
    IGN_CRITICAL("[G35-MCI]", "mciSendStringA(\"%s\") -> %u \"%s\"", cmd.c_str(), mciReturn, reply.c_str());
}

} // namespace

void winmm_ign_install(Bridge& br) {
    auto W = [&](const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
        Shim s; s.argc = argc; s.stdcall_cleanup = true;
        s.tag = std::string("WINMM.dll!") + name;
        // iteration-G30, Parte A/B/C: one of several central per-call
        // wrapper points the Vita input poll now hooks into (see
        // win32/dinput.h). timeGetTime in particular is a very common
        // per-frame timing call, a good candidate for firing at whatever
        // rate the guest's own main loop actually ticks.
        s.fn = [fn](Cpu& c) -> uint32_t { ++g_ign_winmm_call_count; dinput_poll_vita_input(c); return fn(c); };
        br.register_shim("WINMM.dll", name, s);
    };

    W("timeGetTime", 0, [](Cpu& c) -> uint32_t {
        ++g_ign_timegettime_count;
        uint32_t value=(uint32_t)wx86_now_ms();
        uint32_t esp=c.reg(R_ESP);
        g_g99TimeGetTimeCaller=c.read_u32(esp);
        g_g99TimeGetTimeValue=value;
        ++g_g99TimeGetTimeSerial;
        if ((g_ign_timegettime_count & 0xFFu) == 0u) ign_dsound_service(c, value);   // G129: sparse audio service
        return value;
    });
    W("timeBeginPeriod", 1, [](Cpu&) -> uint32_t { return 0u; });   // TIMERR_NOERROR
    W("timeEndPeriod", 1, [](Cpu&) -> uint32_t { return 0u; });     // TIMERR_NOERROR

    W("timeSetEvent", 5, [](Cpu& c) -> uint32_t {
        ++g_ign_timesetevent_count;
        // IgnitionVita iteration-32: full parameter visibility (was only
        // logging `delay`) so that IF the guest ever calls this, the next
        // real-hardware log tells us everything needed to actually
        // implement it later -- without implementing it now. Still
        // returns 0 (TIMERR_NOCANDO / "could not create timer"): this is
        // diagnostic-only, not a real timer. See MMSYSTEM.H for the flag
        // bit meanings decoded below.
        //
        // iteration-G30, Parte M/N/R: this is the ONLY static call site
        // for timeSetEvent in the whole exe (0x00453ef4, confirmed by
        // G29's disassembly), and G29's own runtime summary showed
        // timeSetEvent_count=0 up to the point measured -- i.e. it was
        // NOT yet proven this callsite is actually reached. The
        // [G30-TIMER] tag below is unmistakable specifically so the next
        // hardware log answers that with certainty, and explicitly checks
        // whether callback==0x00454210 (the wrapper G29's disassembly
        // found sitting on top of the same GetDeviceData-calling chain)
        // rather than assuming it.
        uint32_t delay = c.arg(0);
        uint32_t resolution = c.arg(1);
        uint32_t callback = c.arg(2);
        uint32_t dwUser = c.arg(3);
        uint32_t fuEvent = c.arg(4);
        const char* type = (fuEvent & 0x01u) ? "TIME_ONESHOT" : "TIME_PERIODIC";
        const char* cbKind =
            (fuEvent & 0x20u) ? "TIME_CALLBACK_EVENT_PULSE" :
            (fuEvent & 0x10u) ? "TIME_CALLBACK_EVENT_SET" : "TIME_CALLBACK_FUNCTION";
        IGN_LOG("[G30-TIMER]", "reached timeSetEvent callsite 0x00453EF4");
        IGN_LOG("[G30-TIMER]", "timeSetEvent(delay=%u, resolution=%u, callback=0x%08x, dwUser=0x%08x, fuEvent=0x%x[%s|%s])",
                delay, resolution, callback, dwUser, fuEvent, type, cbKind);
        IGN_LOG("[G30-TIMER]", "callback_matches_0x00454210_wrapper=%s", callback == 0x00454210u ? "YES" : "NO");
        IGN_LOG("[WIN32]", "timeSetEvent(delay=%u, resolution=%u, callback=0x%08x, dwUser=0x%08x, "
                           "fuEvent=0x%x[%s|%s]) -> 0 (multimedia timers not implemented yet -- "
                           "params logged for a future iteration, not acted on)",
                delay, resolution, callback, dwUser, fuEvent, type, cbKind);
        return 0u;   // NULL: "could not create the timer"
    });
    W("timeKillEvent", 1, [](Cpu&) -> uint32_t { ++g_ign_timekillevent_count; return 0u; });     // TIMERR_NOERROR

    W("joyGetDevCapsA", 3, [](Cpu&) -> uint32_t {
        IGN_LOG("[WIN32]", "joyGetDevCapsA -> MMSYSERR_NODRIVER (no joystick yet)");
        return 6u;   // MMSYSERR_NODRIVER
    });
    W("joyGetPosEx", 2, [](Cpu&) -> uint32_t {
        ++g_ign_joygetposex_count;
        return 6u;   // MMSYSERR_NODRIVER
    });

    // iteration-G16, Parte C/K: real signature is
    // `MCIERROR mciSendStringA(LPCSTR lpstrCommand, LPSTR lpstrReturnString,
    // UINT uReturnLength, HWND hwndCallback)` -- 4 stdcall dwords (16
    // bytes), NOT 5. Confirmed against Ignition's own real call site this
    // iteration (disassembly at 0x4551f0-0x455208, the "open cdaudio"
    // call): `push 0x0` (hwndCallback) / `push 0x100` (uReturnLength=256)
    // / `push 0x4f376c` (lpstrReturnString) / `push 0x48ed54`
    // (lpstrCommand="open cdaudio") / `call esi` -- exactly four pushes,
    // matching the real ABI. The PRE-G16 registration used argc=5, which
    // (per bridge.cpp's trap_epilogue: esp_add = 4 + 4*argc) popped 20
    // bytes of stdcall cleanup instead of the correct 16 -- a 4-byte
    // over-pop on EVERY mciSendStringA call, silently eating one dword
    // that belongs to the GUEST CALLER's own stack frame, not to this
    // call's arguments. That is a real, mechanically-confirmed stack
    // corruption bug (not a guess): fixed here to argc=4. This is an ABI
    // correctness fix only -- mci_return stays the same generic error
    // value as before (Part L: no fake success, MCI semantics untouched).
    // iteration-G132: the virtual CD-audio device (music/mci_cd.cpp) answers the cdaudio commands; ABI unchanged (4 stdcall
    // dwords, argc=4, see the G16 note above). The call runs on the guest thread and only parses, answers from the TOC and
    // posts at most one command to the music thread: no decoding, no file/port work, no waiting. With the service disabled
    // (control run `nomusic=1`) the G131 behaviour is kept exactly: generic error 1, nothing played.
    W("mciSendStringA", 4, [](Cpu& c) -> uint32_t {
        const uint32_t esp = c.reg(R_ESP);
        const uint32_t callerEip = c.read_u32(esp);
        const uint32_t pCommand = c.arg(0);
        const uint32_t pReturnString = c.arg(1);
        const uint32_t uReturnLength = c.arg(2);
        const uint32_t hwndCallback = c.arg(3);
        const std::string cmd = read_guest_cstr(c, pCommand);

        uint32_t mciReturn = 1u;       // generic MCI error (G131 behaviour when the virtual CD is disabled)
        std::string reply;
        bool known = false;
        const bool backend = music::service_enabled();
        if (backend) {
            const music::MciResult res = music::service_exec(cmd.c_str());
            mciReturn = (uint32_t)res.rc; reply = res.reply; known = res.known;
            // Reply -> guest buffer: bounded by uReturnLength, NUL-terminated, nothing written for a null pointer or zero length.
            if (pReturnString != 0 && uReturnLength != 0) {
                char buf[64];
                bool truncated = false;
                const size_t n = music::format_reply(reply, buf, uReturnLength < sizeof buf ? uReturnLength : (uint32_t)sizeof buf, &truncated);
                if (!truncated && reply.size() >= sizeof buf) truncated = true;      // longer than our staging buffer: never happens for MCI replies here
                if (n && !c.write(pReturnString, buf, (uint32_t)n)) { if (mciReturn == 0) mciReturn = (uint32_t)music::kMciErrHardware; }
                else if (truncated && mciReturn == 0) mciReturn = (uint32_t)music::kMciErrParamOverflow;
            } else if (!reply.empty() && mciReturn == 0) {
                mciReturn = (uint32_t)music::kMciErrParamOverflow;                   // a reply was due but there is nowhere to put it
            }
        }
        ign_mci_capture(cmd, callerEip, pReturnString, uReturnLength, hwndCallback, mciReturn, reply, backend, known);   // each distinct command logged once
        (void)hwndCallback;
        return mciReturn;
    });
}

} // namespace ign
