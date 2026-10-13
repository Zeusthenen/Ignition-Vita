#include "win32/dinput.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/ignition_guest_alloc.h"
#include "runtime/ignition_boot.h"
#include "runtime/host_clock.h"
#include "diagnostics/log.h"
#include "diagnostics/phase_state.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <psp2/ctrl.h>

using namespace d2rt;

// iteration-G15, Parte G/L: same cross-TU plain-C-global counter pattern
// as user32_window.cpp/winmm_ign.cpp/kernel32_ign.cpp -- covers every
// DirectInput vtable method call, implemented or not (make_vtable wraps
// both paths below), read by win32/glide2x_ign.cpp's per-swap-interval
// activity summary. Per G15's Part L: not implementing input on
// suspicion -- this is a counter only, an unimplemented method still
// halts exactly as before.
extern "C" {
volatile uint32_t g_ign_dinput_call_count = 0;
// G98: monotonically records physical ACCEPT down-edges. The renderer
// only observes the serial at swap boundaries; input behavior is unchanged.
volatile uint32_t g_g98AcceptSerial = 0;
// iteration-G159: defined in win32/glide2x_ign.cpp; makes the pad idle while the player name keyboard is open (and until the buttons are released afterwards)
void ign_g159_pad_filter(uint32_t* buttons, uint8_t* lx, uint8_t* ly, uint8_t* rx, uint8_t* ry);
}

// iteration-G57, Parte 11: defined in the GLOBAL namespace by
// win32/kernel32_ign.cpp (same scope as g_g56TargetAllocBase etc. there) --
// declared here, also at global scope, NOT inside the ign::(anonymous)
// namespace below, so the linker resolves to the same symbol rather than
// silently creating an unrelated internal-linkage one.
extern bool g_g57Gen31Armed;
extern uint32_t g_g57Gen31FieldSnapshotAtAlloc;

// iteration-G78, Parte 30: defined in runtime/ignition_boot.cpp, set once
// at boot_run() entry -- read here only for this file's own
// [G78-STARTUP-TIMING] checkpoint's elapsed_ms.
extern uint64_t g_g78ProcessStartMs;

// iteration-G78, Parte 22: defined in win32/glide2x_ign.cpp -- armed here
// the instant CIRCLE/DIK_ESCAPE is pressed, so that file's own LFB/
// triangle/combine counters can accumulate a 2-second post-pause window
// without this file needing to know anything about the renderer.
extern bool g_g78PauseWindowActive;
extern uint64_t g_g78PauseWindowStartMs;

namespace ign {

namespace {

// ---- GUID identity ------------------------------------------------------
constexpr uint8_t kIID_IUnknown[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };

// Real DirectInput device GUIDs, verified against Wine's dinput.h (not
// recalled from memory): GUID_SysKeyboard/SysMouse/Joystick differ only in
// Data1's last byte (0x61/0x60/0x70).
constexpr uint8_t kGUID_SysKeyboard[16] = {
    0x61,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7, 0x44,0x45,0x53,0x54,0x00,0x00 };
constexpr uint8_t kGUID_SysMouse[16] = {
    0x60,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7, 0x44,0x45,0x53,0x54,0x00,0x00 };
constexpr uint8_t kGUID_Joystick[16] = {
    0x70,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7, 0x44,0x45,0x53,0x54,0x00,0x00 };

// DISCL_* -- verified against Wine's dinput.h. Ignition's own
// SetCooperativeLevel call passes 0x6 = DISCL_NONEXCLUSIVE|DISCL_FOREGROUND
// (confirmed by disassembly: `6A 06` immediately before the vtable call).
constexpr uint32_t kDISCL_EXCLUSIVE = 0x00000001u;
constexpr uint32_t kDISCL_NONEXCLUSIVE = 0x00000002u;
constexpr uint32_t kDISCL_FOREGROUND = 0x00000004u;
constexpr uint32_t kDISCL_BACKGROUND = 0x00000008u;

// DIPROP_BUFFERSIZE = MAKEDIPROP(1) -- a small integer used directly as the
// REFGUID argument (never dereferenced as a real 16-byte GUID pointer).
// Verified against Wine's dinput.h; also exactly what Ignition's own
// disassembly pushes (`6A 01`) as SetProperty's rguidProp argument.
constexpr uint32_t kDIPROP_BUFFERSIZE = 1u;

// iteration-G27: DirectInput HRESULT/flag constants for GetDeviceData.
// DI_OK/DI_BUFFEROVERFLOW and DIGDD_PEEK are long-standing, unchanged
// DirectX SDK constants (same status as kDISCL_*/kDIPROP_BUFFERSIZE above).
// DIERR_INPUTLOST=0x8007001E is not just "known" -- it's the literal
// operand of Ignition's own `cmp eax,0x8007001e` at the real call site
// (0x00454053), confirmed by disassembly, not recalled from memory.
constexpr uint32_t kDI_OK = 0x00000000u;
constexpr uint32_t kDI_BUFFEROVERFLOW = 0x00000001u;
constexpr uint32_t kDIERR_NOTACQUIRED = 0x8007001Cu;
constexpr uint32_t kDIERR_INPUTLOST = 0x8007001Eu;          // confirmed via disassembly, see above
constexpr uint32_t kDIERR_INVALIDPARAM = 0x80070057u;
constexpr uint32_t kDIGDD_PEEK = 0x00000001u;

// One buffered-input event, DIRECTINPUT_VERSION<0x0800 layout (no
// uAppData -- see GetDeviceData's own comment for how cbObjectData=16 was
// confirmed for this exact build).
struct DIObjectDataEvent {
    uint32_t dwOfs = 0;
    uint32_t dwData = 0;
    uint32_t dwTimeStamp = 0;
    uint32_t dwSequence = 0;
};

enum class DevKind { Keyboard, Mouse, Joystick, Unknown };

const char* kind_name(DevKind k) {
    switch (k) {
        case DevKind::Keyboard: return "KEYBOARD";
        case DevKind::Mouse:    return "MOUSE";
        case DevKind::Joystick: return "JOYSTICK";
        default:                return "UNKNOWN";
    }
}

std::string format_guid(const uint8_t g[16]) {
    char buf[40];
    snprintf(buf, sizeof buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return std::string(buf);
}

struct DIObj {
    uint32_t vtable = 0;
    uint32_t refcount = 1;
    uint32_t hInstance = 0;
    uint32_t version = 0;
};
struct DIDevice {
    uint32_t vtable = 0;
    uint32_t refcount = 1;
    DevKind kind = DevKind::Unknown;
    bool acquired = false;
    uint32_t coopHwnd = 0, coopFlags = 0;
    uint32_t dataFormatPtr = 0;
    uint32_t bufferSize = 0;
    // iteration-G27, Parte I: buffered-event state, per device (never a
    // queue shared across devices). G27 left this empty (no producer
    // wired up); G28 feeds it from real Vita input for the keyboard
    // device -- see poll_vita_input_into_keyboard_queue() below.
    std::vector<DIObjectDataEvent> eventQueue;
    uint32_t sequenceCounter = 0;     // monotonic dwSequence source -- never reset, only incremented
    bool bufferOverflowed = false;    // sticky DI_BUFFEROVERFLOW flag, set on a real queue-capacity drop
    uint32_t getDeviceDataCalls = 0;          // for first-N-verbose-then-summary logging
    uint32_t getDeviceDataEventsReturned = 0;
    // iteration-G117: device lifecycle bookkeeping (diagnostics + the
    // re-acquire resync). Never read by the game.
    bool everAcquired = false;
    bool firstDrainLogged = false;
    bool notAcquiredLogged = false;
};

std::map<uint32_t, DIObj>& g_objects() { static std::map<uint32_t, DIObj> m; return m; }
std::map<uint32_t, DIDevice>& g_devices() { static std::map<uint32_t, DIDevice> m; return m; }

// iteration-G28 (producer/consumer split in G29), Parte C/D/E/F/G/K:
// minimal Vita-physical-input -> DirectInput-buffered-keyboard-event
// bridge. G27's own grep audit found no sceCtrl*/SDL_*/existing
// key-mapping code anywhere in this project, so this is new, not a
// rewrite of anything.
//
// iteration-G29: G28 polled sceCtrl from INSIDE GetDeviceData, which
// G29's real-hardware run proved wrong -- the guest called GetDeviceData
// only 5 times (46.596s/51.373s/78.466s/78.599s/78.730s) and then never
// again for 440+ more real seconds while still rendering live frames
// (grLfbLock/grBufferSwap/vglSwapBuffers all kept running). Static
// disassembly (0x00453fd0-0x00454148, the function GetDeviceData's real
// caller EIP 0x00454053 lives inside) found the reason it's not a hang:
// this function is called from a `[0x491a48]`-flag-guarded ONE-SHOT
// "wait for DIK_RETURN" gate (0x00405c80-0x00405d9c) that Ignition only
// enters once, and that gate's own polling loop is itself only reached
// through a state==6-gated per-tick dispatcher (0x00406e60 ->
// 0x00453f90 -> ... -> 0x00453fd0). Separately, and likely the REAL
// intended per-tick driver: 0x00453ef4 calls `timeSetEvent` (WINMM.dll,
// the ONLY static call site for it in the whole exe, confirmed via IAT
// address 0x46a1a0) passing 0x00454210 -- a thin wrapper around THIS
// SAME GetDeviceData-calling function -- as the timer callback. This
// project's own timeSetEvent shim (win32/winmm_ign.cpp) unconditionally
// returns 0 (TIMERR_NOCANDO, documented there as "not implemented yet"),
// so that periodic timer is never actually created -- a strong, disassembly-
// backed candidate for why no further ticks ever arrive through THIS path.
// NOT fixed this iteration (out of G29's explicit scope: "responder dos
// preguntas... y solo despues corregir", and the only architecture change
// authorized this iteration is the producer/consumer split below) --
// flagged here for a future iteration.
//
// Because of this, sceCtrl sampling is moved OUT of GetDeviceData
// entirely (Part I/J: producer and consumer now fully separate).
// dinput_poll_vita_input() is called once per grBufferSwap
// (win32/glide2x_ign.cpp) -- a point independently confirmed to keep
// running for 500+ real seconds regardless of the guest's own
// GetDeviceData call pattern -- and only pushes onto dev.eventQueue.
// GetDeviceData (further down) now ONLY drains that queue; it no longer
// samples sceCtrl itself. This guarantees the queue reflects real Vita
// input whenever the guest DOES next call GetDeviceData, even after a
// long gap -- though per Part K, filling the queue alone does not by
// itself prove the guest will ever drain it again through this path;
// see the disassembly findings above for what's actually known.
//
// DIK_* values are the standard, unchanging PC XT keyboard scan-code set 1
// that DirectInput's DIK_* constants map directly onto (same "fixed
// platform ABI fact" status this file already gives DISCL_*/
// DIPROP_BUFFERSIZE/GUID_Sys* above) -- not guessed.
//
// iteration-G34, Parte H/I/J/L/M/N/O: START now maps to DIK_RETURN, no
// longer withheld. G33/G34's disassembly of the language-select screen's
// own input function (0x004122C0-0x004124C9) found and catalogued its
// ONLY two IsKeyDown() checks: `push 0x1C(DIK_RETURN) / call 0x00454150`
// at 0x004123CA -- TRUE calls 0x004124D0, which calls 0x00454E20 (commits
// the selection) then copies a small settings block -- and
// `push 0x01(DIK_ESCAPE) / call 0x00454150` at 0x004123DE -- TRUE sets
// idle-stage=2, the confirmed WM_CLOSE trigger (G33). No OTHER DIK is
// checked anywhere in this function -- DIK_RETURN is therefore the real,
// sole confirm key for this screen, not assumed. CROSS already mapped to
// it since G28 (independently correct); Part O explicitly allows both
// CROSS and START mapping to the same confirm key once verified useful,
// so START joins it here. CIRCLE stays on DIK_ESCAPE (Part K: confirmed
// cancel/exit, not touched).
constexpr uint32_t kDIK_UP = 0xC8u;
constexpr uint32_t kDIK_DOWN = 0xD0u;
constexpr uint32_t kDIK_LEFT = 0xCBu;
constexpr uint32_t kDIK_RIGHT = 0xCDu;
constexpr uint32_t kDIK_RETURN = 0x1Cu;
constexpr uint32_t kDIK_ESCAPE = 0x01u;

// iteration-G116 (controls first pass): the single static table above is
// replaced by the small deterministic mapper below. Keys the original game
// reads (Manual.pdf "Player One" + readme.txt): arrows, '/' gear up, '.'
// gear down, Right Shift boost, ''' rear view, ESC quit, Tab = change view
// after a race, '`' = toggle race results. The Y/N confirmation keys are
// from the static disassembly of Ign_3dfx.exe (handler 0x0041CF70, block
// 0x0041D876): gated by the game's own flag [0x0050C4A8]==1, YES key chosen
// by the language index [0x0061D953] (0:Y 1:J 2:S 3:S 4:J 5:O), NO = N (0x31).
constexpr uint32_t kDIK_Y = 0x15u;
constexpr uint32_t kDIK_O = 0x18u;
constexpr uint32_t kDIK_J = 0x24u;
constexpr uint32_t kDIK_S = 0x1Fu;
constexpr uint32_t kDIK_N = 0x31u;
constexpr uint32_t kDIK_APOSTROPHE = 0x28u;
constexpr uint32_t kDIK_GRAVE = 0x29u;
constexpr uint32_t kDIK_PERIOD = 0x34u;
constexpr uint32_t kDIK_SLASH = 0x35u;
constexpr uint32_t kDIK_RSHIFT = 0x36u;
constexpr uint32_t kDIK_TAB = 0x0Fu;

constexpr uint32_t kG116ConfirmFlagVA = 0x0050C4A8u;   // game's own "Y/N prompt active" gate (==1)
constexpr uint32_t kG116LanguageVA = 0x0061D953u;      // game's own YES-key language selector
// iteration-G117: the game never clears [0x0050C4A8] when a race is quit via
// the prompt's YES (static audit of Ign_3dfx.exe: the YES branch of 0x0041D91E
// leaves it at 1; it is only cleared by the next race start 0x0041142A, by the
// NO branch 0x0041DA4A and a few draw routines), so in the main menu after a
// race it can still read 1 and would turn CROSS into the YES key. The S/N
// context therefore also requires the game to be in its race state:
// [0x00547B30]==1 <=> main-menu function 0x00404BF0 is the active screen
// (set at 0x0041137F, cleared at 0x004113A6/0x004113D4 when a choice starts the
// race), [0x0061D3A4] is 0 while a race runs and 1 for the one tick after it
// ends (0x00411D66, which re-enters the menu setup 0x004049D0).
constexpr uint32_t kG117MenuActiveVA = 0x00547B30u;
constexpr uint32_t kG117GameStateVA = 0x0061D3A4u;
constexpr uint32_t kG117DialogStateVA = 0x005E5584u;

// Left stick: raw 0..255, centre 128. Press at >=60 from centre (~47%), only
// released once back inside 36 (~28%): 24 counts of hysteresis, and anything
// below 36 is dead zone. Each direction yields exactly one DOWN and one UP.
constexpr int kG116StickPress = 60;
constexpr int kG116StickRelease = 36;
// SELECT is a tap (TAB sent on release, so the SELECT+SQUARE chord never
// produces a camera change); the pulse is held long enough for the ~10 FPS
// game loop to observe it.
constexpr uint64_t kG116TabHoldMs = 150;

// ---- G116-LOGIC-BEGIN (pure logic: no guest memory, no I/O -- host-testable)
// iteration-G117: the S/N prompt is only live while the game is in its race
// state (see kG117MenuActiveVA/kG117GameStateVA above) AND its own gate is set.
inline bool g117_sn_context(uint32_t flag, uint32_t menuActive, uint32_t gameState) {
    return flag == 1u && menuActive == 0u && gameState == 0u;
}

inline uint32_t g116_yes_key_for_language(uint32_t lang) {
    static const uint32_t kYes[6] = { kDIK_Y, kDIK_J, kDIK_S, kDIK_S, kDIK_J, kDIK_O };
    return lang < 6 ? kYes[lang] : kDIK_S;   // out-of-range (never seen): the project's documented S
}

struct G116Mapper {
    uint32_t prevButtons = 0;
    uint32_t latchCross = 0, latchTriangle = 0, latchSquare = 0;   // dik chosen at press, kept until release
    bool selectChord = false;      // SQUARE was pressed while SELECT was held
    bool tabTapActive = false;
    uint64_t tabTapEndMs = 0;
    bool stickActive[4] = { false, false, false, false };           // 0 up, 1 down, 2 left, 3 right
    uint8_t sent[256] = { 0 };     // what the guest queue currently believes is down
};

// Sink requirements:  uint32_t confirmYesDik()  -- 0 outside a Y/N prompt, else the YES dik
//                     void button(const char* name, bool down)
//                     void stick(const char* name, bool down)
//                     void key(uint32_t dik, bool down)       -- one queue event
template <class Sink>
void g116_step(G116Mapper& m, uint32_t cur, uint8_t lx, uint8_t ly, uint64_t nowMs, Sink& sink) {
    const uint32_t changed = cur ^ m.prevButtons;
    if (changed) {
        static const struct { uint32_t bit; const char* name; } kNames[] = {
            { SCE_CTRL_UP, "UP" }, { SCE_CTRL_DOWN, "DOWN" }, { SCE_CTRL_LEFT, "LEFT" }, { SCE_CTRL_RIGHT, "RIGHT" },
            { SCE_CTRL_CROSS, "CROSS" }, { SCE_CTRL_CIRCLE, "CIRCLE" }, { SCE_CTRL_TRIANGLE, "TRIANGLE" },
            { SCE_CTRL_SQUARE, "SQUARE" }, { SCE_CTRL_START, "START" }, { SCE_CTRL_SELECT, "SELECT" },
            { SCE_CTRL_LTRIGGER, "L" }, { SCE_CTRL_RTRIGGER, "R" },
        };
        for (const auto& n : kNames) if (changed & n.bit) sink.button(n.name, (cur & n.bit) != 0);

        // Context-dependent buttons latch their dik at the press edge (so the
        // matching release always clears the same key, even if the prompt
        // opens or closes while the button is held).
        if (changed & SCE_CTRL_CROSS) {
            if (cur & SCE_CTRL_CROSS) { uint32_t y = sink.confirmYesDik(); m.latchCross = y ? y : kDIK_RETURN; }
            else m.latchCross = 0;
        }
        if (changed & SCE_CTRL_TRIANGLE) {
            if (cur & SCE_CTRL_TRIANGLE) m.latchTriangle = sink.confirmYesDik() ? kDIK_N : kDIK_SLASH;
            else m.latchTriangle = 0;
        }
        if (changed & SCE_CTRL_SQUARE) {
            if (cur & SCE_CTRL_SQUARE) {
                if (cur & SCE_CTRL_SELECT) { m.latchSquare = kDIK_GRAVE; m.selectChord = true; }
                else m.latchSquare = kDIK_PERIOD;
            } else m.latchSquare = 0;
        }
        if (changed & SCE_CTRL_SELECT) {
            if (cur & SCE_CTRL_SELECT) m.selectChord = false;
            else if (!m.selectChord) { m.tabTapActive = true; m.tabTapEndMs = nowMs + kG116TabHoldMs; }
        }
        m.prevButtons = cur;
    }
    if (m.tabTapActive && nowMs >= m.tabTapEndMs) m.tabTapActive = false;

    // Left stick with hysteresis; axis directions are independent, so
    // diagonals are simply two active directions.
    const int dx = (int)lx - 128, dy = (int)ly - 128;
    const int dist[4] = { -dy, dy, -dx, dx };
    static const char* const kStickNames[4] = { "UP", "DOWN", "LEFT", "RIGHT" };
    for (int i = 0; i < 4; ++i) {
        bool a = m.stickActive[i] ? (dist[i] > kG116StickRelease) : (dist[i] >= kG116StickPress);
        if (a != m.stickActive[i]) { m.stickActive[i] = a; sink.stick(kStickNames[i], a); }
    }

    uint8_t want[256] = { 0 };
    if ((cur & SCE_CTRL_UP) || m.stickActive[0]) want[kDIK_UP] = 1;
    if ((cur & SCE_CTRL_DOWN) || m.stickActive[1]) want[kDIK_DOWN] = 1;
    if ((cur & SCE_CTRL_LEFT) || m.stickActive[2]) want[kDIK_LEFT] = 1;
    if ((cur & SCE_CTRL_RIGHT) || m.stickActive[3]) want[kDIK_RIGHT] = 1;
    if (cur & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) want[kDIK_ESCAPE] = 1;
    if (cur & SCE_CTRL_RTRIGGER) want[kDIK_RSHIFT] = 1;
    if (cur & SCE_CTRL_LTRIGGER) want[kDIK_APOSTROPHE] = 1;
    if ((cur & SCE_CTRL_CROSS) && m.latchCross) want[m.latchCross] = 1;
    if ((cur & SCE_CTRL_TRIANGLE) && m.latchTriangle) want[m.latchTriangle] = 1;
    if ((cur & SCE_CTRL_SQUARE) && m.latchSquare) want[m.latchSquare] = 1;
    if (m.tabTapActive) want[kDIK_TAB] = 1;

    static const uint32_t kUsed[] = {
        kDIK_UP, kDIK_DOWN, kDIK_LEFT, kDIK_RIGHT, kDIK_RETURN, kDIK_ESCAPE, kDIK_RSHIFT, kDIK_APOSTROPHE,
        kDIK_SLASH, kDIK_PERIOD, kDIK_TAB, kDIK_GRAVE, kDIK_Y, kDIK_J, kDIK_S, kDIK_O, kDIK_N,
    };
    for (uint32_t d : kUsed) {
        if (want[d] != m.sent[d]) { m.sent[d] = want[d]; sink.key(d, want[d] != 0); }
    }
}
// ---- G116-LOGIC-END

bool g_vitaCtrlInitialized = false;
G116Mapper g_g116;
uint64_t g_g117RouteUntilMs = 0;   // temporary diagnostic window: per-event routing lines (critical log) around a race<->menu change
uint32_t g_vitaPrevButtons = 0;

// iteration-G49, Parte R/S: stall-probe state -- see
// dinput_poll_vita_input_impl's own top-of-function comment.
uint64_t g_g49LastBridgeMs = 0;
uint32_t g_g49LastBridgeEip = 0;
uint8_t g_vitaPrevLx = 128, g_vitaPrevLy = 128, g_vitaPrevRx = 128, g_vitaPrevRy = 128;   // sceCtrl center value
uint64_t g_vitaPollCount = 0;
uint32_t g_prevFlag491A48 = 0xFFFFFFFFu;   // sentinel: "never read yet", so the FIRST real read always logs once
// iteration-G31, Parte F: [0x004F2E48] -- G31's own disassembly identified
// this as the flag written to 1 immediately before the ONE static call
// site that reaches timeSetEvent (0x00453EF4), inside a function
// (0x00453D60) only reached from the WM_ACTIVATE dispatcher when this
// SAME flag is already non-zero. A sibling function (0x00453BD0, taken
// when the flag is still 0) runs the identical DirectInputCreateA/
// CreateDevice/SetDataFormat/SetCooperativeLevel/SetProperty/Acquire
// sequence but explicitly WRITES 0 here and returns WITHOUT ever calling
// timeSetEvent -- watching this exact address is the single most direct
// way to observe, from real hardware, whether the timeSetEvent-reaching
// branch ever becomes reachable.
uint32_t g_prevFlag4F2E48 = 0xFFFFFFFFu;
// iteration-G33, Parte E/F: the WM_CLOSE self-post gate globals -- see
// the passive-read block in the poll body for the full disassembly
// citation.
uint32_t g_prevStage49BC94 = 0xFFFFFFFFu;
uint32_t g_prevFlag49BCA8 = 0xFFFFFFFFu;
// iteration-G35, Parte F/I: the language-select confirm-path globals.
uint8_t g_prevDikReturnState = 0xFFu;
uint32_t g_prevLangIndex = 0xFFFFFFFFu;
uint64_t g_g31LastHeartbeatMs = 0;
uint32_t g_g31GetDeviceDataBaseline = 0;
uint64_t g_g34InputEdges = 0;   // Parte A: RAM-only, folded into [G34-PERF]
// iteration-G53, Parte 5/6/21 (SUPERSEDED this iteration -- see G54
// note below): a targeted, single-address value watch for the exact
// guest address G52's real hardware run reported as the failing draw's
// branch condition field: [esi+0x18] = 0x01039DD8 (esi=0x01039DC0).
// G53's own real hardware run showed this address is highly dynamic
// (0x00000000 -> 0xD3E5DFDD -> ... -> 0x70000018), and the old 8-change
// cap stopped logging well before the actual failure value appeared --
// insufficient, per this iteration's own Part 5/6 instruction.
//
// iteration-G54, Parte 5/6: replaced the change-counter cap with a
// condition armed until the EXACT failure value is seen (or the run
// ends), emitting exactly ONE capture line instead of a running log --
// still NOT a global write trace (Part 6 forbids that), still just this
// one fixed address, still the SAME established "poll a fixed address
// at the existing shared bridge point" pattern already used for
// g_prevFlag4F2E48/g_prevStage49BC94/g_prevFlag49BCA8/g_prevLangIndex.
// The one capture line includes the value seen immediately before the
// match (Part 10) and the last host-visible bridge EIP (Part 11,
// reusing g_g49LastBridgeEip -- already updated earlier in this same
// function, immediately above this block) as the narrowest
// "guest-only region containing the write" this project's architecture
// can produce without a true write-watchpoint (still a confirmed no-op
// without editing the forbidden vendored dynarec, unchanged since G16).
constexpr uint32_t kG53WatchAddr = 0x01039DD8u;
constexpr uint32_t kG54TargetValue = 0x70000018u;
uint32_t g_g53PrevWatchValue = 0xFFFFFFFFu;
bool g_g53WatchEverRead = false;
bool g_g54TargetCaptured = false;

// iteration-G57, Parte 11: G56's own real hardware run identified the
// EXACT allocation (generation 31, base=0x01039CA0, size=0x850) that
// owns this field -- so the watch is now armed only from that moment,
// via win32/kernel32_ign.cpp's g_g57Gen31Armed/g_g57Gen31FieldSnapshotAtAlloc,
// instead of comparing from process start (G53's own lesson: this
// address is highly dynamic before that point, which only added noise).
// Captures the FIRST change of any kind after arming (not only an exact
// match to 0x70000018), per Part 11's own wording ("first write/first
// observed change") -- still exactly one log line, still the same
// bridge-bracketing technique as G54. (g_g57Gen31Armed/
// g_g57Gen31FieldSnapshotAtAlloc are declared at file scope above, not
// here, so they resolve to kernel32_ign.cpp's global-namespace symbols.)
bool g_g57Gen31FirstChangeCaptured = false;


// iteration-G29, Parte I/J/L/N/O/P/Q (G30: multi-hooked + poll_count +
// [0x491A48] audit, see dinput.h's declaration for the full G30 rationale
// on WHY this moved off being grBufferSwap-only): independent producer.
// Parte O: a real one-shot init probe proving the backend itself works on
// hardware, logged unconditionally the first time this ever runs,
// separate from any later edge-only logging. Parte P: real
// sceCtrlSetSamplingMode/sceCtrlPeekBufferPositive return values logged,
// not assumed. Parte D: edge-only after that (no per-poll spam). Parte E:
// a plain poll_count heartbeat every 300 calls, proving this is actually
// being reached continuously while the screen stays up, independent of
// whether any button changed. Parte F/Q: analog logged only on a
// significant change (>=16 of 255, roughly 6%), never mapped to anything.
// Parte T: a passive read of [0x00491A48] (G29's disassembly: the
// language-confirm gate's own one-shot flag) -- logged only when its
// value changes, never written.
//
// Internal-linkage implementation -- dinput.h declares the public
// ign::dinput_poll_vita_input() wrapper (defined at the bottom of this
// file, outside this anonymous namespace) that just forwards here, since
// a function defined inside an anonymous namespace has different linkage
// than the externally-callable symbol other translation units need.
void dinput_poll_vita_input_impl(Cpu& c) {
    // iteration-G49, Parte R/S: low-overhead long-gap stall probe.
    // Placed at the TOP of THIS exact function because it is already the
    // single shared bridge point every kernel32/user32/winmm/glide
    // wrapper closure AND every dinput COM method calls before anything
    // else (Part R's own explicit instruction: "sample ONLY at existing
    // host-visible bridge entries", no new hook points added anywhere).
    // Cost per call: one wx86_now_us()-derived ms read, one subtraction,
    // one comparison -- a log line is emitted only when a gap exceeds
    // the threshold, never per-call (Part R: "No per-call logging").
    // Answers Part S ("between which two host-visible calls does the
    // stall occur") directly via last/next_guest_eip -- the return
    // address sitting at [esp] on entry to whichever trap called this,
    // same read_caller_eip() formula every other _impl function in this
    // project already uses (glide2x_ign.cpp), just inlined locally here
    // rather than pulling a cross-TU dependency for one read.
    uint32_t g49StallEip = c.read_u32(c.reg(R_ESP));
    {
        uint64_t g49NowMs = wx86_now_ms();
        if (g_g49LastBridgeMs != 0) {
            uint64_t g49GapMs = g49NowMs - g_g49LastBridgeMs;
            if (g49GapMs >= 5000) {
                IGN_LOG("[G49-STALL]", "duration_ms=%llu last_guest_eip=0x%08x next_guest_eip=0x%08x",
                        (unsigned long long)g49GapMs, g_g49LastBridgeEip, g49StallEip);
            }
        }
        g_g49LastBridgeMs = g49NowMs;
    }

    // iteration-G54, Parte 5/6: targeted single-address watch, ARMED
    // UNTIL THE EXACT FAILURE VALUE IS SEEN (replacing G53's 8-change
    // cap, which G53's own hardware run showed was insufficient -- the
    // address is highly dynamic and the failure value never appeared
    // within the first 8 transitions). One extra guest read per poll
    // call (already-safe via this project's existing guest-read guard).
    // Emits exactly ONE line, ever, at the exact moment the value first
    // equals kG54TargetValue -- no running change log, no per-poll spam.
    // g_g49LastBridgeEip still holds the PREVIOUS call's eip here (this
    // call's own g49StallEip is not stored into it until below), so
    // "last_bridge_eip"/"this_bridge_eip" bracket the exact guest-only
    // region the write happened in, same as Part 15 asks for.
    {
        uint32_t g53Value = c.read_u32(kG53WatchAddr);
        if (!g_g53WatchEverRead) {
            g_g53WatchEverRead = true;
            g_g53PrevWatchValue = g53Value;
        } else if (!g_g54TargetCaptured && g53Value == kG54TargetValue && g53Value != g_g53PrevWatchValue) {
            g_g54TargetCaptured = true;
            IGN_LOG("[G54-70000018]", "addr=0x%08x old_value=0x%08x new_value=0x%08x "
                                      "last_bridge_eip=0x%08x this_bridge_eip=0x%08x",
                    kG53WatchAddr, g_g53PrevWatchValue, g53Value, g_g49LastBridgeEip, g49StallEip);
            g_g53PrevWatchValue = g53Value;
        } else {
            g_g53PrevWatchValue = g53Value;
        }
    }

    // iteration-G57, Parte 11: gen-31-gated first-change capture (see
    // g_g57Gen31Armed's own comment in win32/kernel32_ign.cpp). Fires at
    // most once, and only after win32/kernel32_ign.cpp's HeapAlloc shim
    // has observed the exact generation-31 allocation signature -- so
    // this reports the field's FIRST mutation after that specific
    // allocation, with none of G53's pre-arming noise. g_g49LastBridgeEip
    // still holds the PREVIOUS call's eip here, same bracketing as G54.
    if (g_g57Gen31Armed && !g_g57Gen31FirstChangeCaptured) {
        uint32_t g57Value = c.read_u32(kG53WatchAddr);
        if (g57Value != g_g57Gen31FieldSnapshotAtAlloc) {
            g_g57Gen31FirstChangeCaptured = true;
            IGN_LOG("[G57-FIELD-WRITE]", "destination=0x%08x old_raw=0x%08x new_raw=0x%08x "
                                        "last_bridge_eip=0x%08x this_bridge_eip=0x%08x (exact source_reg/source_raw NOT "
                                        "recoverable without a true write-watchpoint -- see comment)",
                    kG53WatchAddr, g_g57Gen31FieldSnapshotAtAlloc, g57Value, g_g49LastBridgeEip, g49StallEip);
        }
    }

    g_g49LastBridgeEip = g49StallEip;

    if (!g_vitaCtrlInitialized) {
        // G116: ANALOG instead of DIGITAL -- same button bits plus the stick
        // axes the left-stick directions need. Nothing else in this project
        // calls sceCtrl* (grep-verified), so the mode is not shared.
        int modeResult = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        SceCtrlData initPad;
        memset(&initPad, 0, sizeof initPad);
        int peekResult = sceCtrlPeekBufferPositive(0, &initPad, 1);
        IGN_LOG("[G30-INPUT]", "sceCtrl_init_ok=%s sampling_mode_result=%d peek_result=%d buttons=0x%08x",
                peekResult >= 0 ? "YES" : "NO", modeResult, peekResult, initPad.buttons);

        // iteration-G77, Parte 25/26: pause-key audit, static disassembly
        // only (no live tracing exists on this dynarec target, per this
        // project's own confirmed Cpu::set_code_hook limitation). The
        // generic "IsKeyDown(dik)" helper at guest VA 0x00454150 is called
        // with a literal DIK code pushed immediately before it at exactly
        // two confirmed sites: `push 0x1c; call 0x454150` (DIK_RETURN,
        // 0x004123ca) and `push 0x1; call 0x454150` (DIK_ESCAPE,
        // 0x004123de) -- both inside the language-select confirm/cancel
        // function (0x004122c0-0x004124c9, already cited by this file's
        // own G35 comment). A second, much larger cluster of
        // `call 0x454150` sites exists at 0x0041cf00-0x0041e110+, but
        // every one of those pushes a REGISTER (esi/ebx/edi/ecx/edx), not
        // a literal DIK code, and a handful push literal DIK codes 0x15/
        // 0x24/0x1f/0x18/0x31 (Y/J/S/O/N) -- consistent with a generic
        // key-rebinding/options-menu loop, not a single dedicated pause
        // check, and NOT conclusively tied to in-race gameplay code by
        // this iteration's static analysis. No `call 0x454150` site was
        // found anywhere inside the confirmed race-material-setup address
        // range (0x00449000-0x00453000, where guTexSource/guDrawTriangle-
        // WithClip's own real callers live). Per this project's own "no
        // inventar" policy (same standard as grDrawPoint's undetermined
        // effect type, G76), the original in-race PAUSE/menu key is
        // reported as UNDETERMINED rather than guessed -- Vita START stays
        // mapped to DIK_RETURN unchanged (kVitaKeyMap below), matching the
        // explicit instruction not to remap it without proof.
        IGN_LOG("[G77-PAUSE-AUDIT]", "ORIGINAL_PAUSE_DIK=UNDETERMINED ORIGINAL_ESCAPE_DIK=0x01(DIK_ESCAPE, "
                                     "language-select cancel only, NOT proven in-race) ORIGINAL_CONFIRM_DIK=0x1c(DIK_RETURN, "
                                     "language-select confirm, evidence: push 0x1c/call 0x454150 at 0x004123ca) "
                                     "vita_start_mapping=UNCHANGED(DIK_RETURN) reason=no_call_454150_site_found_in_race_address_range");
        IGN_LOG("[G117-BUILD]", "artifact=G117_CONTROLS_MENU_EXIT_FIX base=G116_CONTROLS_FIRST_PASS control_map=FROZEN_G116 "
                                "fixes=sn_context_requires_race_state+keyboard_delivery_to_acquired_devices+Unacquire+Release_erase+DefWindowProc_WM_CLOSE_destroys_window");
        IGN_CRITICAL("[G117-BUILD]", "artifact=G117_CONTROLS_MENU_EXIT_FIX base=G116_CONTROLS_FIRST_PASS control_map=FROZEN_G116");
        IGN_LOG("[G116-CTRL-MAP]", "build=G116_CONTROLS_FIRST_PASS sampling=ANALOG dpad+leftstick(press>=%u release<=%u of 128)->arrows "
                                   "CROSS->RETURN(S/Y/J/O per language in SN prompt) CIRCLE,START->ESC TRIANGLE->'/'(N in SN prompt) "
                                   "SQUARE->'.' R->RSHIFT L->''' SELECT(tap)->TAB SELECT+SQUARE->'`'(suppresses '.') "
                                   "sn_gate=[0x0050C4A8]==1 yes_key_index=[0x0061D953]",
                (unsigned)kG116StickPress, (unsigned)kG116StickRelease);
        if (peekResult >= 0) {
            g_g116.prevButtons = initPad.buttons;
            g_vitaPrevButtons = initPad.buttons;
            g_vitaPrevLx = initPad.lx; g_vitaPrevLy = initPad.ly;
            g_vitaPrevRx = initPad.rx; g_vitaPrevRy = initPad.ry;
        }
        g_vitaCtrlInitialized = true;
        return;   // this tick was the init probe only -- next tick starts real edge detection cleanly
    }

    // iteration-G34, Parte A: G33's own hardware log had ~6800
    // [G30-INPUT] lines, most of them this per-300-polls counter --
    // removed as a log line entirely (RAM-only counter now), folded into
    // the single consolidated [G34-PERF] summary below instead.
    ++g_vitaPollCount;

    // Parte T (G30) / Parte F (G31): passive reads only, never written --
    // same safe pattern this project already uses for other guest globals
    // (e.g. kernel32_ign.cpp's calloc-count peek).
    uint32_t flag491A48 = c.read_u32(0x00491A48u);
    if (flag491A48 != g_prevFlag491A48) {
        bool firstRead = (g_prevFlag491A48 == 0xFFFFFFFFu);
        IGN_LOG("[G31-STATE]", "addr=0x00491A48 old=%u%s new=%u",
                firstRead ? 0 : g_prevFlag491A48, firstRead ? "(first read)" : "", flag491A48);
        g_prevFlag491A48 = flag491A48;
    }
    uint32_t flag4F2E48 = c.read_u32(0x004F2E48u);
    if (flag4F2E48 != g_prevFlag4F2E48) {
        bool firstRead = (g_prevFlag4F2E48 == 0xFFFFFFFFu);
        IGN_LOG("[G31-STATE]", "addr=0x004F2E48 (timeSetEvent-imminent flag) old=%u%s new=%u%s",
                firstRead ? 0 : g_prevFlag4F2E48, firstRead ? "(first read)" : "", flag4F2E48,
                flag4F2E48 != 0 ? " -- timeSetEvent callsite should now be reachable" : "");
        g_prevFlag4F2E48 = flag4F2E48;
    }

    // iteration-G33, Parte E/F: passive reads of the exact globals
    // disassembly (0x00445680-0x0044571D and 0x004123C5-0x004123F9) proved
    // gate the self-posted WM_CLOSE: [0x0049BC94] is the idle-work stage
    // counter (0 -> 1 on the very first idle tick, unconditionally; -> 2
    // ONLY when 0x004123F9 runs, which itself only happens right after
    // IsKeyDown(DIK_ESCAPE=0x01) reads true at 0x004123E8 -- i.e. a real
    // Circle/Escape press). [0x0049BCA8] is the one-shot flag the
    // stage==2 idle-work branch sets to 1 in the SAME call that returns
    // FALSE and triggers PostMessageA(WM_CLOSE) at 0x004459A0. Watching
    // both on-change gives the next hardware run an exact, unambiguous
    // timeline of this transition without needing a trap at either guest
    // address (none exists safely, per this project's own confirmed
    // Cpu::set_code_hook limitation).
    uint32_t stage94 = c.read_u32(0x0049BC94u);
    if (stage94 != g_prevStage49BC94) {
        bool firstRead = (g_prevStage49BC94 == 0xFFFFFFFFu);
        IGN_LOG("[G33-CLOSE-GATE]", "addr=0x0049BC94 (idle-work stage) old=%u%s new=%u%s",
                firstRead ? 0 : g_prevStage49BC94, firstRead ? "(first read)" : "", stage94,
                stage94 == 2 ? " -- stage==2 reached, set by 0x004123F9 right after IsKeyDown(DIK_ESCAPE)==TRUE; WM_CLOSE gate now imminent" : "");
        if (!firstRead) IGN_CRITICAL("[G35-TRANS]", "idle_stage[0x0049BC94] %u -> %u", g_prevStage49BC94, stage94);
        // iteration-G117: the game's own "quit" request (menu choice 3 at
        // 0x004113F8 or ESC on the language screen) -- breadcrumb that survives the log allowlist.
        if (stage94 == 2) {
            IGN_LOG("[G117-EXIT]", "game idle stage -> 2 (quit requested by the game itself) t_ms=%llu", (unsigned long long)wx86_now_ms());
            IGN_CRITICAL("[G117-EXIT]", "game idle stage -> 2 (quit requested by the game itself)");
        }
        g_prevStage49BC94 = stage94;
    }
    uint32_t flag49BCA8 = c.read_u32(0x0049BCA8u);
    if (flag49BCA8 != g_prevFlag49BCA8) {
        bool firstRead = (g_prevFlag49BCA8 == 0xFFFFFFFFu);
        IGN_LOG("[G33-CLOSE-GATE]", "addr=0x0049BCA8 (stage==2 one-shot-done flag) old=%u%s new=%u%s",
                firstRead ? 0 : g_prevFlag49BCA8, firstRead ? "(first read)" : "", flag49BCA8,
                flag49BCA8 != 0 ? " -- the idle-work call that just ran returned FALSE, PostMessageA(WM_CLOSE) should have just fired from 0x00445680" : "");
        if (!firstRead && flag49BCA8 != 0) IGN_CRITICAL("[G35-TRANS]", "idle_stage2_done[0x0049BCA8] set -- PostMessageA(WM_CLOSE) imminent from 0x00445680");
        g_prevFlag49BCA8 = flag49BCA8;
    }

    // iteration-G35, Parte F/G/H/I: the SAME safe passive-read technique,
    // aimed at the language-select screen's confirm path this time (not
    // just its cancel path). [0x004F2624+0x1C] is the exact byte IsKeyDown
    // reads for DIK_RETURN (0x1C) -- the same state array GetDeviceData's
    // event loop maintains. [0x00536ED8] is the language-index global the
    // confirm/cancel function (0x004122C0-0x004124C9) repeatedly indexes
    // string/sprite tables with (`lea eax,[eax+eax*4]` *5 chains at
    // 0x004123AA/0x004123B1 etc.) -- logged as a raw index only; no
    // language NAME is guessed from it without a decoded string table
    // (Part I: "no inventar nombre... sin verificar").
    uint8_t dikReturnState = 0;
    c.read(0x004F2624u + 0x1Cu, &dikReturnState, 1);
    if (dikReturnState != g_prevDikReturnState) {
        bool firstRead = (g_prevDikReturnState == 0xFFu);
        if (firstRead) {
            // iteration-G78, Parte 30: first time this guest code path
            // (the language-select confirm check) ever executes is a
            // reasonable, evidence-based proxy for "the language screen is
            // now up and polling input" -- not an exact "screen became
            // visible" event (no such signal exists on this dynarec
            // target), documented honestly as a proxy.
            IGN_LOG("[G78-STARTUP-TIMING]", "checkpoint=language_screen_active elapsed_ms=%llu (proxy: first confirm_check poll)",
                    (unsigned long long)(wx86_now_ms() - g_g78ProcessStartMs));
#ifdef G103_ZERO_DIRTY_LFB_GUARD
            IGN_LOG("[G103-BOOT-VISIBILITY]", "milestone=language_selector_phase_reached elapsed_ms=%llu proxy=first_confirm_check_poll",
                    (unsigned long long)(wx86_now_ms() - g_g78ProcessStartMs));
#endif
        }
        IGN_LOG("[G35-LANG]", "confirm_check IsKeyDown(DIK_RETURN)=%u%s", dikReturnState, firstRead ? " (first read)" : "");
        if (!firstRead) IGN_CRITICAL("[G35-LANG]", "confirm_check IsKeyDown(DIK_RETURN) %u -> %u", g_prevDikReturnState, dikReturnState);
        if (dikReturnState != 0) {
            IGN_CRITICAL("[G35-LANG]", "IsKeyDown(DIK_RETURN)=TRUE");
#ifdef G103_ZERO_DIRTY_LFB_GUARD
            static bool g103IntroPhaseLogged = false;
            if (!g103IntroPhaseLogged) {
                g103IntroPhaseLogged = true;
                IGN_LOG("[G103-BOOT-VISIBILITY]", "milestone=intro_phase_reached elapsed_ms=%llu proxy=language_confirm_accepted",
                        (unsigned long long)(wx86_now_ms() - g_g78ProcessStartMs));
            }
#endif
        }
        g_prevDikReturnState = dikReturnState;
    }
    uint32_t langIndex = c.read_u32(0x00536ED8u);
    if (langIndex != g_prevLangIndex) {
        bool firstRead = (g_prevLangIndex == 0xFFFFFFFFu);
        IGN_LOG("[G35-LANG]", "selected_index=%u%s (raw index only -- string table not decoded, not naming a language)",
                langIndex, firstRead ? " (first read)" : "");
        if (!firstRead) IGN_CRITICAL("[G35-LANG]", "selected_index %u -> %u", g_prevLangIndex, langIndex);
        g_prevLangIndex = langIndex;
    }

    // iteration-G31/G32/G34, Parte A/F: ONE consolidated host-side
    // heartbeat, wall-clock gated at ~10s (Part F's own widened interval),
    // independent of whether grBufferSwap/GetDeviceData fire at all --
    // lets the next hardware log distinguish "program alive but no
    // present" from an actual deadlock, folds in the input queue's real
    // state, and now the poll/edge totals Part A moved out of the hot
    // path. Replaces G32's separate [G32-HEARTBEAT]/[G32-INPUT] lines
    // with a single line, halving that line count too.
    uint64_t nowMsHeartbeat = wx86_now_ms();
    if (nowMsHeartbeat - g_g31LastHeartbeatMs >= 10000) {
        DIDevice* kbHb = nullptr;
        for (auto& kv : g_devices()) {
            if (kv.second.kind == DevKind::Keyboard) { kbHb = &kv.second; break; }
        }
        uint32_t depth = kbHb ? (uint32_t)kbHb->eventQueue.size() : 0;
        uint32_t totalCalls = kbHb ? kbHb->getDeviceDataCalls : 0;
        IGN_LOG("[G34-PERF]", "t=%llu guest_alive=YES input_polls_total=%llu input_edges_total=%llu queue_depth=%u GetDeviceData_calls_since_last=%u",
                (unsigned long long)nowMsHeartbeat, (unsigned long long)g_vitaPollCount, (unsigned long long)g_g34InputEdges,
                depth, totalCalls - g_g31GetDeviceDataBaseline);
        g_g31GetDeviceDataBaseline = totalCalls;
        g_g31LastHeartbeatMs = nowMsHeartbeat;
    }

    // iteration-G117 (temporary diagnostic): passive watch of the four game
    // globals that define the S/N context, at most every 50 ms and logged only
    // when one of them CHANGES (a handful of lines per race). A change of
    // the menu/state words opens a 4 s window in which every physical
    // transition is routed to the critical log with its destination device.
    {
        static uint64_t s_watchMs = 0;
        static uint32_t s_pf = 0xFFFFFFFFu, s_pm = 0xFFFFFFFFu, s_ps = 0xFFFFFFFFu, s_pd = 0xFFFFFFFFu;
        if (nowMsHeartbeat - s_watchMs >= 50) {
            s_watchMs = nowMsHeartbeat;
            uint32_t wf = c.read_u32(kG116ConfirmFlagVA), wm = c.read_u32(kG117MenuActiveVA);
            uint32_t ws = c.read_u32(kG117GameStateVA), wd = c.read_u32(kG117DialogStateVA);
            if (wf != s_pf || wm != s_pm || ws != s_ps || wd != s_pd) {
                bool first = (s_pf == 0xFFFFFFFFu);
                bool screenChange = !first && (wm != s_pm || ws != s_ps);
                s_pf = wf; s_pm = wm; s_ps = ws; s_pd = wd;
                if (screenChange) g_g117RouteUntilMs = nowMsHeartbeat + 4000;
                static uint32_t s_lines = 0;
                if (++s_lines <= 300) {   // hard cap: the critical log is never truncated
                    uint32_t nKb = 0, nAcq = 0;
                    for (auto& kv : g_devices()) { if (kv.second.kind == DevKind::Keyboard) { ++nKb; if (kv.second.acquired) ++nAcq; } }
                    IGN_LOG("[G117-CTX-WATCH]", "flag[0x0050C4A8]=%u menu[0x00547B30]=%u state[0x0061D3A4]=%u dialog[0x005E5584]=%u sn_context=%s keyboards=%u acquired=%u",
                            wf, wm, ws, wd, g117_sn_context(wf, wm, ws) ? "YES" : "NO", nKb, nAcq);
                    IGN_CRITICAL("[G117-CTX-WATCH]", "flag=%u menu=%u state=%u dialog=%u sn_context=%s keyboards=%u acquired=%u",
                                 wf, wm, ws, wd, g117_sn_context(wf, wm, ws) ? "YES" : "NO", nKb, nAcq);
                }
            }
        }
    }

    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    int ret = sceCtrlPeekBufferPositive(0, &pad, 1);
    if (ret < 0) return;   // no controller data this tick -- leave everything exactly as-is, nothing fabricated
    ign_g159_pad_filter(&pad.buttons, &pad.lx, &pad.ly, &pad.rx, &pad.ry);   // iteration-G159: an idle pad while the player name keyboard is open (and until the buttons are released afterwards)

    // iteration-G116: all button/stick -> DIK translation lives in g116_step()
    // (pure logic above). This sink only does the guest/Vita side effects:
    // the guest-memory context probe (on demand, only at a CROSS/TRIANGLE
    // press edge -- never per tick), the DIDevice queue push (same
    // DIObjectDataEvent + overflow handling as G28-G115) and sparse logs.
    struct G116Sink {
        Cpu& c;
        DIDevice* tgt[4];        // every ACQUIRED keyboard device (iteration-G117), see below
        uint32_t tgtHandle[4];
        uint32_t ntgt;
        uint64_t nowMs;
        uint32_t confirmYesDik() {
            uint32_t flag = c.read_u32(kG116ConfirmFlagVA);
            uint32_t menu = c.read_u32(kG117MenuActiveVA);
            uint32_t state = c.read_u32(kG117GameStateVA);
            bool confirm = g117_sn_context(flag, menu, state);
            uint32_t yes = confirm ? g116_yes_key_for_language(c.read_u32(kG116LanguageVA)) : 0u;
            // 0 NORMAL, 1 SN_CONFIRM, 2 NORMAL-because-stale-flag (flag==1 outside the race state)
            int ctx = confirm ? 1 : (flag == 1u ? 2 : 0);
            static int s_lastCtx = -1;
            if (ctx != s_lastCtx) {
                s_lastCtx = ctx;
                IGN_LOG("[G116-CTX]", "context=%s yes_dik=0x%02x flag[0x0050C4A8]=%u menu[0x00547B30]=%u state[0x0061D3A4]=%u lang[0x0061D953]=%u",
                        ctx == 1 ? "SN_CONFIRM" : (ctx == 2 ? "NORMAL(stale_flag_ignored)" : "NORMAL"), yes, flag, menu, state,
                        c.read_u32(kG116LanguageVA));
            }
            return yes;
        }
        void button(const char* name, bool down) {
            ++g_g34InputEdges;
            IGN_LOG("[G116-INPUT]", "Vita %s %s", name, down ? "DOWN" : "UP");
        }
        void stick(const char* name, bool down) {
            IGN_LOG("[G116-INPUT]", "stick %s %s", name, down ? "ENTER" : "EXIT");
        }
        void key(uint32_t dik, bool down) {
            if (dik == kDIK_RETURN && down) ++g_g98AcceptSerial;
            if (dik == kDIK_ESCAPE && down && !g_g78PauseWindowActive) {
                g_g78PauseWindowActive = true;
                g_g78PauseWindowStartMs = nowMs;
            }
            // iteration-G117: the event goes to every keyboard device the
            // game currently has ACQUIRED (the one(s) its GetDeviceData
            // actually drains) -- not to "the first keyboard in the map",
            // which could be a stale, released or unacquired instance.
            if (ntgt == 0) {
                static uint32_t s_dropped = 0;
                if (++s_dropped <= 8) IGN_CRITICAL("[G117-ROUTE]", "dik=0x%02x %s DROPPED: no acquired keyboard device (count=%u)",
                                                  dik, down ? "DOWN" : "UP", s_dropped);
                return;
            }
            for (uint32_t t = 0; t < ntgt; ++t) {
                DIDevice* kb = tgt[t];
                if (kb->bufferSize > 0 && kb->eventQueue.size() >= kb->bufferSize) {
                    kb->eventQueue.erase(kb->eventQueue.begin());
                    kb->bufferOverflowed = true;
                    IGN_LOG("[G116-INPUT]", "keyboard event queue at capacity (%u) -- oldest event dropped, DI_BUFFEROVERFLOW pending",
                            kb->bufferSize);
                }
                DIObjectDataEvent ev;
                ev.dwOfs = dik;
                ev.dwData = down ? 0x80u : 0x00u;
                ev.dwTimeStamp = (uint32_t)nowMs;
                ev.dwSequence = ++kb->sequenceCounter;   // never reset, only incremented
                kb->eventQueue.push_back(ev);
                IGN_LOG("[G116-INPUT]", "sent DIK 0x%02x %s seq=%u", dik, down ? "DOWN" : "UP", ev.dwSequence);
                static uint32_t s_routeLines = 0;
                if (nowMs < g_g117RouteUntilMs && ++s_routeLines <= 400) {
                    IGN_CRITICAL("[G117-ROUTE]", "dik=0x%02x %s -> dev=0x%08x acquired=%d queue_depth=%u seq=%u",
                                 dik, down ? "DOWN" : "UP", tgtHandle[t], (int)kb->acquired, (unsigned)kb->eventQueue.size(), ev.dwSequence);
                }
            }
        }
    };

    G116Sink sink{ c, { nullptr, nullptr, nullptr, nullptr }, { 0, 0, 0, 0 }, 0, wx86_now_ms() };
    uint32_t nKeyboards = 0;
    for (auto& kv : g_devices()) {
        if (kv.second.kind != DevKind::Keyboard) continue;
        ++nKeyboards;
        if (kv.second.acquired && sink.ntgt < 4) { sink.tgt[sink.ntgt] = &kv.second; sink.tgtHandle[sink.ntgt] = kv.first; ++sink.ntgt; }
    }
    {   // temporary G117 diagnostic: which device(s) the producer feeds, logged only when that set changes
        static uint32_t s_sig = 0xFFFFFFFFu;
        uint32_t sig = nKeyboards * 0x10001u + sink.ntgt;
        for (uint32_t t = 0; t < sink.ntgt; ++t) sig = sig * 31u + sink.tgtHandle[t];
        if (sig != s_sig) {
            s_sig = sig;
            IGN_LOG("[G117-DINPUT]", "producer targets: keyboards_existing=%u acquired_targets=%u dev0=0x%08x dev1=0x%08x",
                    nKeyboards, sink.ntgt, sink.tgtHandle[0], sink.tgtHandle[1]);
            IGN_CRITICAL("[G117-DINPUT]", "producer targets: keyboards_existing=%u acquired_targets=%u dev0=0x%08x dev1=0x%08x",
                         nKeyboards, sink.ntgt, sink.tgtHandle[0], sink.tgtHandle[1]);
        }
    }
    g116_step(g_g116, pad.buttons, pad.lx, pad.ly, sink.nowMs, sink);
    g_vitaPrevButtons = pad.buttons;
}

// IDirectInputDeviceA vtable order -- verified against Wine's dinput.h.
// SetDataFormat (0x2C), SetCooperativeLevel (0x34), SetProperty (0x18) and
// Acquire (0x1C) are all CONFIRMED against Ign_d3d.exe's own disassembly
// (real CALL [vtable+offset] sites for the exact bootstrap chain the game
// runs on its first device). Everything else is the standard order,
// not independently re-verified this iteration.
const std::vector<const char*> kDeviceMethods = {
    "QueryInterface", "AddRef", "Release",              // 0x00, 0x04, 0x08
    "GetCapabilities", "EnumObjects", "GetProperty",    // 0x0C, 0x10, 0x14
    "SetProperty",                                       // 0x18 -- confirmed
    "Acquire",                                           // 0x1C -- confirmed
    "Unacquire", "GetDeviceState", "GetDeviceData",      // 0x20, 0x24, 0x28
    "SetDataFormat",                                     // 0x2C -- confirmed
    "SetEventNotification",                              // 0x30
    "SetCooperativeLevel",                               // 0x34 -- confirmed
    "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize",   // 0x38, 0x3C, 0x40, 0x44
};

// IDirectInputA vtable order -- the well-established, unchanged-since-DX3
// order (CreateDevice/EnumDevices/GetDeviceStatus/RunControlPanel/
// Initialize after IUnknown). CreateDevice (0x0C) is CONFIRMED against
// Ign_d3d.exe's own disassembly; the rest follows the standard historical
// order (repeated attempts to fetch the exact declaration from a live
// header this session hit the fetch tool's own size/quoting limits, so
// this is knowledge, not a fresh verbatim source -- flagged honestly
// rather than presented as independently re-checked).
const std::vector<const char*> kDIMethods = {
    "QueryInterface", "AddRef", "Release",   // 0x00, 0x04, 0x08
    "CreateDevice",                           // 0x0C -- confirmed: real CALL [eax+0x0C]
    "EnumDevices", "GetDeviceStatus", "RunControlPanel", "Initialize",   // 0x10, 0x14, 0x18, 0x1C
};

std::function<uint32_t(Cpu&)> unimpl_trap(std::string fullName) {
    return [fullName](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        uint32_t esp = c.reg(R_ESP);
        uint32_t retAddr = c.read_u32(esp);
        char detail[256];
        snprintf(detail, sizeof detail, "%s (this=0x%08x, called from guest EIP=0x%08x ESP=0x%08x)",
                 fullName.c_str(), thisPtr, retAddr, esp);
        IGN_LOG("[DINPUT]", "UNIMPLEMENTED %s", detail);
        request_halt(detail);
        return 0u;
    };
}

using Override = std::pair<uint32_t, std::function<uint32_t(Cpu&)>>;

uint32_t make_vtable(Bridge& br, Cpu& cpu, const char* ifaceTag,
                     const std::vector<const char*>& methodNames,
                     const std::map<std::string, Override>& overrides) {
    uint32_t vtable = ignition_guest_alloc((uint32_t)methodNames.size() * 4, 4);
    if (!vtable) return 0;
    for (size_t i = 0; i < methodNames.size(); ++i) {
        std::string fullName = std::string(ifaceTag) + "::" + methodNames[i];
        Shim s;
        auto it = overrides.find(methodNames[i]);
        // iteration-G30, Parte A/B/C: one of several central per-call
        // wrapper points the Vita input poll now hooks into (see
        // dinput.h). Includes GetDeviceData itself and every unimplemented-
        // trap path -- harmless (dinput_poll_vita_input only ever fills
        // the queue, never calls back into DirectInput), and ensures the
        // freshest possible queue state right before GetDeviceData reads it.
        if (it != overrides.end()) {
            s.argc = it->second.first;
            s.stdcall_cleanup = true;
            std::function<uint32_t(Cpu&)> realFn = it->second.second;
            s.fn = [realFn](Cpu& c) -> uint32_t { ++g_ign_dinput_call_count; dinput_poll_vita_input_impl(c); return realFn(c); };
        } else {
            s.argc = 0;
            s.stdcall_cleanup = false;
            std::function<uint32_t(Cpu&)> trapFn = unimpl_trap(fullName);
            s.fn = [trapFn](Cpu& c) -> uint32_t { ++g_ign_dinput_call_count; dinput_poll_vita_input_impl(c); return trapFn(c); };
        }
        s.tag = fullName;
        br.register_shim("native.dinput", fullName, s);
        uint32_t va = br.shim_trap("native.dinput", fullName);
        cpu.write_u32(vtable + (uint32_t)i * 4, va);
    }
    IGN_LOG("[COM]", "%s vtable built: %u slot(s) at 0x%08x", ifaceTag, (unsigned)methodNames.size(), vtable);
    return vtable;
}

} // namespace

void dinput_install(Bridge& br, Cpu& cpu) {
    // ---- IDirectInputDeviceA ----
    std::map<std::string, Override> deviceOverrides;
    deviceOverrides["QueryInterface"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), riid = c.arg(1), ppv = c.arg(2);
        uint8_t guid[16] = {0};
        if (riid) c.read(riid, guid, 16);
        bool known = std::memcmp(guid, kIID_IUnknown, 16) == 0;
        IGN_LOG("[DINPUTDEVICE]", "QueryInterface(this=0x%08x) -> %s", thisPtr, known ? "S_OK" : "E_NOINTERFACE");
        if (known) {
            auto it = g_devices().find(thisPtr);
            if (it != g_devices().end()) ++it->second.refcount;
            if (ppv) c.write_u32(ppv, thisPtr);
            return 0u;
        }
        if (ppv) c.write_u32(ppv, 0);
        return 0x80004002u;
    }};
    deviceOverrides["AddRef"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_devices().find(thisPtr);
        uint32_t rc = it != g_devices().end() ? ++it->second.refcount : 1u;
        IGN_LOG("[DINPUTDEVICE]", "AddRef(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    deviceOverrides["Release"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_devices().find(thisPtr);
        uint32_t rc = 0;
        if (it != g_devices().end() && it->second.refcount > 0) rc = --it->second.refcount;
        IGN_LOG("[DINPUTDEVICE]", "Release(this=0x%08x) -> %u", thisPtr, rc);
        // iteration-G117: a device whose last reference is gone no longer
        // exists. Dropping its entry (instead of keeping a dead, still-"Keyboard"
        // record that the producer could pick) is real DirectInput semantics.
        if (it != g_devices().end() && rc == 0) {
            IGN_LOG("[G117-DINPUT]", "Release dev=0x%08x kind=%s -> refcount 0, device destroyed (entry removed)", thisPtr, kind_name(it->second.kind));
            IGN_CRITICAL("[G117-DINPUT]", "Release dev=0x%08x kind=%s -> refcount 0, device destroyed (entry removed)", thisPtr, kind_name(it->second.kind));
            g_devices().erase(it);
        }
        return rc;
    }};
    // HRESULT Unacquire(void) (vtable 0x20). The game's WM_DESTROY cleanup
    // (0x00453F10) calls it right before Release; until G117 it was an
    // UNIMPLEMENTED trap that would have halted the process there. Real
    // semantics: the device stops delivering, buffered data is discarded.
    deviceOverrides["Unacquire"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_devices().find(thisPtr);
        if (it != g_devices().end()) {
            it->second.acquired = false;
            it->second.eventQueue.clear();
            it->second.bufferOverflowed = false;
        }
        IGN_LOG("[G117-DINPUT]", "Unacquire dev=0x%08x kind=%s -> DI_OK (queue flushed)", thisPtr, it != g_devices().end() ? kind_name(it->second.kind) : "UNKNOWN");
        IGN_CRITICAL("[G117-DINPUT]", "Unacquire dev=0x%08x kind=%s -> DI_OK (queue flushed)", thisPtr, it != g_devices().end() ? kind_name(it->second.kind) : "UNKNOWN");
        return 0u;
    }};
    // HRESULT SetDataFormat(LPCDIDATAFORMAT lpdf). Ignition's real format
    // (read from the actual guest struct, not hardcoded): dwSize=0x18,
    // dwObjSize=0x10, dwFlags=0x2 (DIDF_RELAXIS), dwDataSize=0x100,
    // dwNumObjs=256 -- exactly the shape of the standard c_dfDIKeyboard
    // (a flat 256-byte keyboard state array), confirming this is the
    // keyboard device independently of the GUID check in CreateDevice.
    deviceOverrides["SetDataFormat"] = { 2, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), lpdf = c.arg(1);
        uint32_t dwSize = c.read_u32(lpdf + 0x00);
        uint32_t dwObjSize = c.read_u32(lpdf + 0x04);
        uint32_t dwFlags = c.read_u32(lpdf + 0x08);
        uint32_t dwDataSize = c.read_u32(lpdf + 0x0C);
        uint32_t dwNumObjs = c.read_u32(lpdf + 0x10);
        auto it = g_devices().find(thisPtr);
        const char* which = it != g_devices().end() ? kind_name(it->second.kind) : "UNKNOWN";
        IGN_LOG("[DINPUTDEVICE]", "%s SetDataFormat", which);
        IGN_LOG("[DINPUTDEVICE]", "  dwSize=%u dwObjSize=%u dwFlags=0x%x%s dwDataSize=%u dwNumObjs=%u",
                dwSize, dwObjSize, dwFlags,
                (dwFlags & 1) ? " DIDF_ABSAXIS" : (dwFlags & 2) ? " DIDF_RELAXIS" : "",
                dwDataSize, dwNumObjs);
        if (it != g_devices().end()) it->second.dataFormatPtr = lpdf;
        IGN_LOG("[DINPUTDEVICE]", "%s SetDataFormat -> DI_OK", which);
        return 0u;
    }};
    // HRESULT SetCooperativeLevel(HWND hwnd, DWORD dwFlags). Ignition's
    // real flags (confirmed by disassembly, `6A 06`): 0x6 =
    // DISCL_NONEXCLUSIVE|DISCL_FOREGROUND. Both are compatible with a
    // pure-software input model (no real device arbitration to perform),
    // so DI_OK is honest here, not a shortcut.
    deviceOverrides["SetCooperativeLevel"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), hwnd = c.arg(1), flags = c.arg(2);
        auto it = g_devices().find(thisPtr);
        const char* which = it != g_devices().end() ? kind_name(it->second.kind) : "UNKNOWN";
        std::string decoded;
        if (flags & kDISCL_EXCLUSIVE) decoded += " DISCL_EXCLUSIVE";
        if (flags & kDISCL_NONEXCLUSIVE) decoded += " DISCL_NONEXCLUSIVE";
        if (flags & kDISCL_FOREGROUND) decoded += " DISCL_FOREGROUND";
        if (flags & kDISCL_BACKGROUND) decoded += " DISCL_BACKGROUND";
        IGN_LOG("[DINPUTDEVICE]", "%s SetCooperativeLevel(hwnd=0x%08x, flags=0x%08x%s)",
                which, hwnd, flags, decoded.c_str());
        if (it != g_devices().end()) { it->second.coopHwnd = hwnd; it->second.coopFlags = flags; }
        IGN_LOG("[DINPUTDEVICE]", "%s SetCooperativeLevel -> DI_OK", which);
        return 0u;
    }};
    // HRESULT SetProperty(REFGUID rguidProp, LPCDIPROPHEADER pdiph).
    // rguidProp is a MAKEDIPROP pseudo-GUID here (a small integer used
    // directly as the pointer value, never dereferenced as real GUID
    // bytes) -- confirmed by Ignition's own disassembly pushing the
    // literal 1 (DIPROP_BUFFERSIZE). Only that one property is recognized;
    // anything else is logged honestly as unrecognized but still answered
    // DI_OK (SetProperty has no observable effect on this software model
    // yet -- GetDeviceData, which would actually need a real buffer, isn't
    // implemented this iteration).
    deviceOverrides["SetProperty"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), rguidProp = c.arg(1), pdiph = c.arg(2);
        uint32_t dwHeaderSize = c.read_u32(pdiph + 0x04);
        uint32_t dwObj = c.read_u32(pdiph + 0x08);
        uint32_t dwHow = c.read_u32(pdiph + 0x0C);
        auto it = g_devices().find(thisPtr);
        const char* which = it != g_devices().end() ? kind_name(it->second.kind) : "UNKNOWN";
        if (rguidProp == kDIPROP_BUFFERSIZE) {
            uint32_t dwData = c.read_u32(pdiph + 0x10);   // DIPROPDWORD.dwData
            IGN_LOG("[DINPUTDEVICE]", "%s SetProperty(DIPROP_BUFFERSIZE, dwData=%u) dwObj=%u dwHow=%u",
                    which, dwData, dwObj, dwHow);
            if (it != g_devices().end()) it->second.bufferSize = dwData;
        } else {
            IGN_LOG("[DINPUTDEVICE]", "%s SetProperty(rguidProp=0x%08x, dwHeaderSize=%u) -- unrecognized property, no effect",
                    which, rguidProp, dwHeaderSize);
        }
        IGN_LOG("[DINPUTDEVICE]", "%s SetProperty -> DI_OK", which);
        return 0u;
    }};
    // HRESULT Acquire(void). No real device to arbitrate for yet (no Vita
    // input has been wired up), so this only flips the logical flag --
    // confirmed as the real next call by disassembly (slot 0x1C right
    // after SetProperty succeeds).
    deviceOverrides["Acquire"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_devices().find(thisPtr);
        const char* which = it != g_devices().end() ? kind_name(it->second.kind) : "UNKNOWN";
        if (it != g_devices().end()) {
            DIDevice& d = it->second;
            // iteration-G117: re-acquire after an Unacquire -- the game's own key
            // array may still show keys we reported DOWN; release them first, and
            // forget what we sent so any key still physically held is re-sent.
            uint32_t resynced = 0;
            if (d.everAcquired && !d.acquired && d.kind == DevKind::Keyboard) {
                for (uint32_t k = 0; k < 256; ++k) {
                    if (!g_g116.sent[k]) continue;
                    DIObjectDataEvent ev;
                    ev.dwOfs = k; ev.dwData = 0; ev.dwTimeStamp = (uint32_t)wx86_now_ms(); ev.dwSequence = ++d.sequenceCounter;
                    d.eventQueue.push_back(ev);
                    g_g116.sent[k] = 0;
                    ++resynced;
                }
            }
            bool wasAcquired = d.acquired;
            d.acquired = true;
            d.everAcquired = true;
            if (!wasAcquired) {   // log state TRANSITIONS only (an Acquire on an already-acquired device is a no-op)
                IGN_LOG("[G117-DINPUT]", "Acquire dev=0x%08x kind=%s -> DI_OK resynced_keys=%u", thisPtr, which, resynced);
                IGN_CRITICAL("[G117-DINPUT]", "Acquire dev=0x%08x kind=%s -> DI_OK resynced_keys=%u", thisPtr, which, resynced);
            }
        }
        IGN_LOG("[DINPUTDEVICE]", "%s Acquire -> DI_OK", which);
        return 0u;
    }};
    // iteration-G27, Parte A/B/C: HRESULT GetDeviceData(DWORD cbObjectData,
    // LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) --
    // stdcall, argc=5 (this + 4 params, matching this file's own
    // this-as-arg(0) convention for every other COM method here).
    //
    // Confirmed by disassembling Ignition's own real call site
    // (caller_guest_eip=0x00454053, call itself at 0x00454050:
    // `call [ecx+0x28]` -- offset 0x28 independently matches this project's
    // own kDeviceMethods table, index 10 = GetDeviceData): the push order
    // (0x00454039-0x0045404f) is `push 0(dwFlags) / push &[esp+0x10]
    // (pdwInOut) / push &[esp+0x1c](rgdod) / push 0x10(cbObjectData) / push
    // eax(this)`, i.e. cbObjectData=16 (the DIRECTINPUT_VERSION<0x0800
    // DIDEVICEOBJECTDATA layout: dwOfs+dwData+dwTimeStamp+dwSequence, no
    // uAppData -- not assumed, this IS what got pushed), rgdod is a real
    // non-NULL 32-slot stack buffer (not a NULL "just tell me the count"
    // query), dwFlags=0 (DIGDD_PEEK NOT set -- a real call consumes queued
    // events). Earlier still, at 0x00453fec: `mov [esp+0x10],0x20` sets
    // *pdwInOut to 32 BEFORE the call -- exactly Ignition's own
    // DIPROP_BUFFERSIZE value from SetProperty -- confirming *pdwInOut on
    // entry is the requested capacity. After the call, 0x00454090 does
    // `mov eax,[esp+0x10]` and compares it against 0 -- confirming
    // *pdwInOut on exit is read back as the number of events actually
    // written, standard buffered-data semantics for this exact build, not
    // assumed from a modern header. The post-call branch also checks
    // eax==DIERR_INPUTLOST(0x8007001E, the literal disassembled operand)
    // first (re-Acquire path), then treats eax==0(DI_OK) or eax==1
    // (DI_BUFFEROVERFLOW) as the two outcomes that let the guest continue.
    deviceOverrides["GetDeviceData"] = { 5, [](Cpu& c) -> uint32_t {
        uint32_t esp = c.reg(R_ESP);
        uint32_t callerEip = c.read_u32(esp);
        uint32_t thisPtr = c.arg(0);
        uint32_t cbObjectData = c.arg(1);
        uint32_t rgdod = c.arg(2);
        uint32_t pdwInOut = c.arg(3);
        uint32_t dwFlags = c.arg(4);
        bool peek = (dwFlags & kDIGDD_PEEK) != 0;

        auto it = g_devices().find(thisPtr);
        if (it == g_devices().end()) {
            IGN_LOG("[G27]", "GetDeviceData(this=0x%08x) -- unknown device handle, DIERR_INVALIDPARAM", thisPtr);
            return kDIERR_INVALIDPARAM;
        }
        DIDevice& dev = it->second;
        const char* which = kind_name(dev.kind);
        ++dev.getDeviceDataCalls;
        // Parte S: full detail for the first 5 calls per device, then a
        // periodic summary line instead of one line per call -- Ignition's
        // real flow calls this every input-poll tick, which would otherwise
        // flood the log with identical "0 events" lines.
        bool verbose = dev.getDeviceDataCalls <= 5;

        if (!dev.acquired) {
            IGN_LOG("[G27]", "%s GetDeviceData -> DIERR_NOTACQUIRED (device not acquired)", which);
            if (!dev.notAcquiredLogged) {
                dev.notAcquiredLogged = true;
                IGN_CRITICAL("[G117-DINPUT]", "GetDeviceData dev=0x%08x kind=%s -> DIERR_NOTACQUIRED (first time; the game is polling an unacquired device)", thisPtr, which);
            }
            return kDIERR_NOTACQUIRED;
        }
        if (!dev.firstDrainLogged) {
            dev.firstDrainLogged = true;
            IGN_LOG("[G117-DINPUT]", "GetDeviceData now consuming dev=0x%08x kind=%s queued=%u (first drain of this device)", thisPtr, which, (unsigned)dev.eventQueue.size());
            IGN_CRITICAL("[G117-DINPUT]", "GetDeviceData now consuming dev=0x%08x kind=%s queued=%u (first drain of this device)", thisPtr, which, (unsigned)dev.eventQueue.size());
        }

        // iteration-G29, Parte J/S: GetDeviceData no longer samples sceCtrl
        // itself -- dinput_poll_vita_input() (called once per grBufferSwap,
        // see win32/glide2x_ign.cpp) is the ONLY producer now. This
        // function's own return/queue-drain semantics are otherwise
        // unchanged from G27 (Parte S: frozen).
        uint32_t requested = pdwInOut ? c.read_u32(pdwInOut) : 0;
        uint32_t queuedBefore = (uint32_t)dev.eventQueue.size();
        if (verbose) {
            IGN_LOG("[G27]", "GetDeviceData entry: device=%s this=0x%08x caller_guest_eip=0x%08x guest_esp=0x%08x",
                    which, thisPtr, callerEip, esp);
            IGN_LOG("[G27]", "  stack_check: entry_esp=0x%08x argc_dwords=5 expected_esp_after_return=0x%08x (entry_esp+24) return_target=0x%08x",
                    esp, esp + 24, callerEip);
            IGN_LOG("[G27]", "  cbObjectData=%u rgdod=0x%08x pdwInOut=0x%08x *pdwInOut_before=%u dwFlags=0x%08x%s",
                    cbObjectData, rgdod, pdwInOut, requested, dwFlags, peek ? " DIGDD_PEEK" : "");
            IGN_LOG("[G27]", "  requested=%u queued_before=%u", requested, queuedBefore);
            if (cbObjectData != 16) {
                IGN_LOG("[G27]", "  NOTE: cbObjectData=%u != 16 (the only value this project's own disassembly has "
                                   "confirmed) -- writing min(cbObjectData,16) real bytes per event, zero-filling any "
                                   "remainder, nothing beyond cbObjectData touched per event", cbObjectData);
            }
        }
        // iteration-G29, Parte R: if the (now independently-fed) queue is
        // non-empty, log each event about to be consumed in full detail,
        // regardless of the verbose/summary split above -- this is exactly
        // the evidence needed to confirm real Vita input reaching the guest
        // through this path, so it's never folded into the post-5-calls
        // summary.
        if (queuedBefore > 0) {
            uint32_t n = requested < queuedBefore ? requested : queuedBefore;
            for (uint32_t i = 0; i < n; ++i) {
                const DIObjectDataEvent& ev = dev.eventQueue[i];
                IGN_LOG("[G29]", "  event[%u]: dwOfs=0x%02x dwData=0x%02x dwTimeStamp=%u dwSequence=%u",
                        i, ev.dwOfs, ev.dwData, ev.dwTimeStamp, ev.dwSequence);
            }
        }

        // Parte L/M/T: no key is ever synthesized here -- events only exist
        // if dinput_poll_vita_input() actually saw a real physical
        // transition. Returning 0 events + DI_OK for an empty queue is
        // real, honest DirectInput behavior, not a fake success.
        uint32_t toReturn = requested;
        if ((uint32_t)dev.eventQueue.size() < toReturn) toReturn = (uint32_t)dev.eventQueue.size();

        for (uint32_t i = 0; i < toReturn && rgdod; ++i) {
            const DIObjectDataEvent& ev = dev.eventQueue[i];
            // iteration-G35, Parte E: "consumed" = actually handed back to
            // the guest via GetDeviceData -- distinct from "queued" (the
            // producer side, dinput_poll_vita_input_impl above).
            if (ev.dwOfs == kDIK_RETURN || ev.dwOfs == kDIK_ESCAPE) {
                IGN_CRITICAL("[G35-TRANS]", "%s consumed data=0x%02x seq=%u",
                        ev.dwOfs == kDIK_RETURN ? "DIK_RETURN" : "DIK_ESCAPE", ev.dwData, ev.dwSequence);
            }
            uint32_t dst = rgdod + i * cbObjectData;
            uint32_t bytesToWrite = cbObjectData < 16u ? cbObjectData : 16u;
            uint8_t buf[16];
            memcpy(buf + 0, &ev.dwOfs, 4);
            memcpy(buf + 4, &ev.dwData, 4);
            memcpy(buf + 8, &ev.dwTimeStamp, 4);
            memcpy(buf + 12, &ev.dwSequence, 4);
            c.write(dst, buf, bytesToWrite);
            if (cbObjectData > 16u) {
                std::vector<uint8_t> zeros(cbObjectData - 16u, 0);
                c.write(dst + 16u, zeros.data(), (uint32_t)zeros.size());
            }
        }
        if (!peek && toReturn > 0) {
            dev.eventQueue.erase(dev.eventQueue.begin(), dev.eventQueue.begin() + toReturn);
        }
        if (pdwInOut) c.write_u32(pdwInOut, toReturn);
        dev.getDeviceDataEventsReturned += toReturn;

        uint32_t hr = dev.bufferOverflowed ? kDI_BUFFEROVERFLOW : kDI_OK;
        dev.bufferOverflowed = false;   // real DirectInput clears the sticky flag once reported once

        if (verbose) {
            IGN_LOG("[G27]", "  returned_events=%u queued_after=%u HRESULT=%s",
                    toReturn, (unsigned)dev.eventQueue.size(), hr == kDI_OK ? "DI_OK" : "DI_BUFFEROVERFLOW");
        } else if ((dev.getDeviceDataCalls % 256u) == 0u) {
            IGN_LOG("[G27]", "%s GetDeviceData calls=%u events_returned_total=%u (queue empty -- no Vita input "
                              "backend wired up yet)",
                    which, dev.getDeviceDataCalls, dev.getDeviceDataEventsReturned);
        }
        return hr;
    }};
    uint32_t deviceVtable = make_vtable(br, cpu, "IDirectInputDeviceA", kDeviceMethods, deviceOverrides);

    // ---- IDirectInputA ----
    std::map<std::string, Override> diOverrides;
    diOverrides["QueryInterface"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), riid = c.arg(1), ppv = c.arg(2);
        uint8_t guid[16] = {0};
        if (riid) c.read(riid, guid, 16);
        bool known = std::memcmp(guid, kIID_IUnknown, 16) == 0;
        IGN_LOG("[DINPUT]", "IDirectInputA::QueryInterface(this=0x%08x) -> %s", thisPtr, known ? "S_OK" : "E_NOINTERFACE");
        if (known) {
            auto it = g_objects().find(thisPtr);
            if (it != g_objects().end()) ++it->second.refcount;
            if (ppv) c.write_u32(ppv, thisPtr);
            return 0u;
        }
        if (ppv) c.write_u32(ppv, 0);
        return 0x80004002u;
    }};
    diOverrides["AddRef"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_objects().find(thisPtr);
        uint32_t rc = it != g_objects().end() ? ++it->second.refcount : 1u;
        IGN_LOG("[DINPUT]", "IDirectInputA::AddRef(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    diOverrides["Release"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_objects().find(thisPtr);
        uint32_t rc = 0;
        if (it != g_objects().end() && it->second.refcount > 0) rc = --it->second.refcount;
        IGN_LOG("[DINPUT]", "IDirectInputA::Release(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    // HRESULT CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICEA* out, LPUNKNOWN pUnkOuter).
    // rguid is read from wherever the guest ACTUALLY points at call time --
    // Ignition builds it in a stack-local rather than referencing a static
    // constant directly, so there's nothing meaningful to pre-read from the
    // file; the real value only exists at runtime, which is exactly what
    // this shim inspects.
    diOverrides["CreateDevice"] = { 4, [deviceVtable](Cpu& c) -> uint32_t {
        uint32_t rguid = c.arg(1), lplpDevice = c.arg(2), pUnkOuter = c.arg(3);
        uint8_t guid[16] = {0};
        if (rguid) c.read(rguid, guid, 16);
        DevKind kind = DevKind::Unknown;
        const char* kindName = "UNKNOWN";
        if (std::memcmp(guid, kGUID_SysKeyboard, 16) == 0) { kind = DevKind::Keyboard; kindName = "GUID_SysKeyboard"; }
        else if (std::memcmp(guid, kGUID_SysMouse, 16) == 0) { kind = DevKind::Mouse; kindName = "GUID_SysMouse"; }
        else if (std::memcmp(guid, kGUID_Joystick, 16) == 0) { kind = DevKind::Joystick; kindName = "GUID_Joystick"; }

        IGN_LOG("[DINPUT]", "IDirectInputA::CreateDevice");
        IGN_LOG("[DINPUT]", "  GUID  = %s (%s)", format_guid(guid).c_str(), kindName);
        IGN_LOG("[DINPUT]", "  out   = 0x%08x", lplpDevice);
        IGN_LOG("[DINPUT]", "  outer = 0x%08x", pUnkOuter);

        uint32_t obj = ignition_guest_alloc(4, 4);
        if (!obj) {
            IGN_LOG("[ERROR]", "CreateDevice: guest alloc failed");
            return 0x8007000Eu;
        }
        c.write_u32(obj, deviceVtable);
        DIDevice dev;
        dev.vtable = deviceVtable; dev.kind = kind;
        g_devices()[obj] = dev;

        static int deviceCount = 0;
        IGN_LOG("[DINPUT]", "DEVICE #%d = 0x%08x (%s)", ++deviceCount, obj, kind_name(kind));
        IGN_LOG("[G117-DINPUT]", "CreateDevice dev=0x%08x kind=%s device_number=%d", obj, kind_name(kind), deviceCount);
        IGN_CRITICAL("[G117-DINPUT]", "CreateDevice dev=0x%08x kind=%s device_number=%d", obj, kind_name(kind), deviceCount);

        if (lplpDevice) c.write_u32(lplpDevice, obj);
        IGN_LOG("[DINPUT]", "IDirectInputA::CreateDevice -> DI_OK");
        return 0u;
    }};
    uint32_t diVtable = make_vtable(br, cpu, "IDirectInputA", kDIMethods, diOverrides);

    // ---- DirectInputCreateA itself ----
    Shim create;
    create.argc = 4;
    create.stdcall_cleanup = true;
    create.tag = "DINPUT.dll!DirectInputCreateA";
    create.fn = [diVtable](Cpu& c) -> uint32_t {
        uint32_t hInstance = c.arg(0), dwVersion = c.arg(1), lplpDirectInput = c.arg(2), pUnkOuter = c.arg(3);
        IGN_LOG("[DINPUT]", "DirectInputCreateA");
        IGN_LOG("[DINPUT]", "  hInstance = 0x%08x", hInstance);
        IGN_LOG("[DINPUT]", "  version   = 0x%08x%s", dwVersion, dwVersion == 0x0300 ? " (DirectInput 3.0, classic IDirectInputA)" : "");
        IGN_LOG("[DINPUT]", "  out       = 0x%08x", lplpDirectInput);
        IGN_LOG("[DINPUT]", "  outer     = 0x%08x", pUnkOuter);

        uint32_t obj = ignition_guest_alloc(4, 4);
        if (!obj) {
            IGN_LOG("[ERROR]", "DirectInputCreateA: guest alloc failed");
            return 0x8007000Eu;
        }
        c.write_u32(obj, diVtable);
        DIObj st; st.vtable = diVtable; st.hInstance = hInstance; st.version = dwVersion;
        g_objects()[obj] = st;

        if (lplpDirectInput) c.write_u32(lplpDirectInput, obj);
        IGN_LOG("[COM]", "IDirectInputA object = 0x%08x vtable = 0x%08x", obj, diVtable);
        IGN_LOG("[DINPUT]", "DirectInputCreateA -> DI_OK");
        return 0u;
    };
    br.register_shim("DINPUT.dll", "DirectInputCreateA", create);
}

void dinput_poll_vita_input(Cpu& c) {
    dinput_poll_vita_input_impl(c);
}

} // namespace ign
