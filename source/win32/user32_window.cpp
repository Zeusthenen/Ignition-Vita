#include "win32/user32_window.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_scratch.h"
#include "runtime/host_clock.h"
#include "runtime/ignition_boot.h"
#include "runtime/ignition_memlayout.h"
#include "win32/dinput.h"
#include "diagnostics/log.h"
#include "diagnostics/phase_state.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <map>
#include <deque>
#include <functional>

using namespace d2rt;

// iteration-33: message-queue depth, readable from the vendored C dynarec
// (dynarec_arm_00.c's ign_loop33_marker(), a different language/TU with no
// access to std::deque) as a plain C global. Written only by
// sync_queue_depth() below, right after every queue mutation.
extern "C" { volatile uint32_t g_ign_msg_queue_depth = 0; }

// iteration-G14: plain C globals (same cross-TU pattern as
// g_ign_msg_queue_depth above), read by win32/glide2x_ign.cpp's frame-loop
// diagnostic summary to answer "is the guest actually pumping messages
// during the clear/swap loop, or not touching the message queue at all."
// Incremented once per real shim invocation, nothing else changed.
extern "C" {
    volatile uint32_t g_ign_peekmessage_count = 0;
    volatile uint32_t g_ign_getmessage_count = 0;
    volatile uint32_t g_ign_dispatchmessage_count = 0;
    volatile uint32_t g_ign_postmessage_count = 0;
    // iteration-G15, Parte G: one aggregate counter for EVERY USER32 shim
    // registered through this file's own K() closure (RegisterClassA,
    // CreateWindowExA, ShowWindow, SetFocus, SetWindowLongA/GetWindowLongA,
    // PostQuitMessage, plus the four message-pump calls already counted
    // individually above) -- incremented centrally where K() wraps each
    // shim's std::function, so no individual shim body needed editing.
    volatile uint32_t g_ign_user32_call_count = 0;
}

namespace ign {

namespace {

// ---- WNDCLASSA/CREATESTRUCTA/MSG field offsets -- standard Win32 ABI,
// verified against the real WNDCLASSA WinMain builds on the stack before
// calling RegisterClassA (ignition_boot.cpp's kWinMainCandidateVA comment
// has the disassembly this was cross-checked against): style, wndproc,
// cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground,
// lpszMenuName, lpszClassName, in that order -- exactly this layout.
constexpr int32_t GWL_WNDPROC = -4;
constexpr uint32_t WM_CREATE = 0x0001, WM_QUIT = 0x0012, WM_SETFOCUS = 0x0007, WM_ACTIVATEAPP = 0x001C;
constexpr uint32_t WM_DESTROY = 0x0002, WM_CLOSE = 0x0010;   // iteration-G117

struct IgnitionWindowClass {
    uint16_t atom = 0;
    uint32_t guest_wndproc = 0;
    uint32_t hinstance = 0;
    uint32_t style = 0;
    std::string class_name;
};

struct IgnitionWindow {
    uint32_t hwnd = 0;
    uint32_t guest_wndproc = 0;
    uint32_t hinstance = 0;
    std::string class_name;
    int32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t style = 0, ex_style = 0;
    bool visible = false;
    bool active = false;
    std::map<int32_t, uint32_t> extra;   // SetWindowLongA on an index other than GWL_WNDPROC
};

struct IgnitionMsg {
    uint32_t hwnd = 0, message = 0, wParam = 0, lParam = 0, time = 0;
    int32_t pt_x = 0, pt_y = 0;
};

std::map<std::string, IgnitionWindowClass>& g_classes() {
    static std::map<std::string, IgnitionWindowClass> m;
    return m;
}
std::map<uint32_t, IgnitionWindow>& g_windows() {
    static std::map<uint32_t, IgnitionWindow> m;
    return m;
}
std::deque<IgnitionMsg>& g_queue() {
    static std::deque<IgnitionMsg> q;
    return q;
}

// iteration-G33, Parte N/O: PeekMessageA empty-poll summary counters,
// reset every ~5s -- see the PeekMessageA shim below.
uint32_t g_g33EmptyPeekCount = 0;
uint32_t g_g33MessagesSeenCount = 0;
uint64_t g_g33LastPeekSummaryMs = 0;

// iteration-G75, Parte 15/18: G33 already gated EMPTY polls (above), but
// every REAL message dispatched through PeekMessageA logged 2 lines
// unconditionally ("Part P: no semantics changed, this is logging
// only") -- if the guest posts/dispatches messages frequently in its own
// loop (e.g. a periodic WM_TIMER), this is exactly the kind of sustained,
// non-empty per-call spam G74's own [G74-LOGGER-PERF] measurement
// (~18.66ms/line, ~28 lines/sec sustained) implicates. Same "first N +
// periodic summary" pattern as the empty-poll case, gated behind
// kG75LowOverheadRuntimeLog so it can be trivially reverted to G33's
// original unconditional behavior.
uint32_t g_g75RealMessageCount = 0;
uint64_t g_g75LastRealMessageSummaryMs = 0;

// iteration-33: kept in sync at every point the queue is mutated, and
// exposed to the vendored C dynarec (see dynarec_arm_00.c's
// ign_loop33_marker(), which cannot link against this C++ translation
// unit or its STL containers) via a single plain C global -- no function
// call across the language boundary, no ABI concerns.
uint32_t sync_queue_depth() {
    uint32_t d = (uint32_t)g_queue().size();
    g_ign_msg_queue_depth = d;
    return d;
}

uint32_t& g_focused_hwnd() {
    static uint32_t h = 0;
    return h;
}
uint32_t next_hwnd() {
    static uint32_t next = 0x00090001u;   // synthetic, well clear of file handles (0x5000_) and engine fake handles (0x8600_/0x8700_)
    return next++;
}

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

const char* msg_name(uint32_t m) {
    switch (m) {
        case 0x0001: return "WM_CREATE";
        case 0x0002: return "WM_DESTROY";
        case 0x0003: return "WM_MOVE";
        case 0x0005: return "WM_SIZE";
        case 0x0006: return "WM_ACTIVATE";
        case 0x0007: return "WM_SETFOCUS";
        case 0x0008: return "WM_KILLFOCUS";
        case 0x000F: return "WM_PAINT";
        case 0x0010: return "WM_CLOSE";
        case 0x0012: return "WM_QUIT";
        case 0x001C: return "WM_ACTIVATEAPP";
        default:     return "WM_?";
    }
}

// IgnitionVita iteration-34 found (HALLAZGO 4) that GetMessageA's
// trap-entry [ESP] can itself be a trap-window address (0x06620BA0,
// inside trap_base=0x06620000 -- see ignition_memlayout.cpp), not a real
// Ign_d3d.exe return address -- confirmed as a genuine data value, not a
// wrong accessor (c.read_u32(c.reg(R_ESP)) is byte-for-byte what the
// engine's own trap_retaddr() does, cpu_box86.cpp). Iteration 34 tried
// walking the stack for a real .text address (HALLAZGO 3: none of the
// first 8 words qualified); iteration 35 removed that approach entirely
// in favor of direct guest-EIP instrumentation -- see dynarec_arm_00.c's
// [GM35] PRECALL_BLOCKING marker at 0x004121F4, the one real guest
// instruction that can reach this shim and block.

void mark_message_loop_once() {
    static bool done = false;
    if (!done) { done = true; set_phase(Phase::MessageLoop, "first Peek/GetMessageA call"); }
}

// Nesting depth for guest->guest wndproc calls (see below).
uint32_t g_wndproc_nesting = 0;

// Invokes a guest WNDPROC synchronously -- deliberately NOT via
// Bridge::call_va(), even though that's the engine's own reusable
// guest-call helper (cdecl-style push order is identical to stdcall's, so
// it would otherwise work fine for a __stdcall WNDPROC too). The reason:
// call_va() always starts ITS OWN frame at a single FIXED address
// (stack_base_+stack_size_-0x1000, confirmed by reading bridge.cpp). That
// is safe for ONE call, but this project's WM_CREATE/WM_SETFOCUS dispatch
// happens from INSIDE an already-running trap (CreateWindowExA/SetFocus/
// DispatchMessageA), and if the guest WNDPROC itself calls another USER32
// function that dispatches ANOTHER message synchronously (e.g. a WM_CREATE
// handler that calls SetFocus), a second call at the SAME fixed address
// would silently overwrite the first call's still-in-progress frame.
//
// So this replicates call_va()'s own logic (push args right-to-left, push
// the sentinel return address, run()) but offsets the frame by the current
// nesting depth, inside the SAME guest stack region ignition_memlayout.cpp
// already owns via WX86_STACKBASE -- 2 KiB per level is far more than a
// couple of stdcall frames need, and stack_size (2 MiB) allows over 1000
// levels before this would ever matter. EBP is saved/restored too: the
// disassembled WinMain preserves incoming EBP (`push ebp`/`pop ebp`) but
// may use it as a scratch register in between, so clobbering it here would
// corrupt WinMain's own state across the call.
bool call_guest_wndproc(Bridge& br, Cpu& c, uint32_t wndproc, uint32_t hwnd,
                        uint32_t msg, uint32_t wParam, uint32_t lParam, uint32_t& outRet) {
    outRet = 0;
    if (!wndproc) return false;

    const MemLayout& L = ignition_mem_layout();
    uint32_t esp = L.stack_base + L.stack_size - 0x1000u - (g_wndproc_nesting * 0x2000u);
    ++g_wndproc_nesting;

    const uint32_t args[4] = { hwnd, msg, wParam, lParam };
    for (int i = 3; i >= 0; --i) { esp -= 4; c.write_u32(esp, args[(size_t)i]); }
    esp -= 4; c.write_u32(esp, br.sentinel());

    uint32_t savedEsp = c.reg(R_ESP), savedEbp = c.reg(R_EBP);
    c.set_reg(R_ESP, esp);
    c.set_reg(R_EBP, 0);
    const char* fault = nullptr;
    bool ok = c.run(wndproc, &fault);
    if (ok) outRet = c.reg(R_EAX);
    c.set_reg(R_ESP, savedEsp);
    c.set_reg(R_EBP, savedEbp);
    --g_wndproc_nesting;

    if (!ok) {
        IGN_LOG("[ERROR]", "guest wndproc 0x%08x faulted handling msg %s(0x%x): %s",
                wndproc, msg_name(msg), msg, fault ? fault : "(unknown)");
    }
    return ok;
}

} // namespace

void user32_window_install(Bridge& br) {
    auto K = [&](const char* name, uint32_t argc, std::function<uint32_t(Cpu&)> fn) {
        Shim s; s.argc = argc; s.stdcall_cleanup = true;
        s.tag = std::string("USER32.dll!") + name;
        // iteration-G30, Parte A/B/C: one of several central per-call
        // wrapper points the Vita input poll now hooks into (see
        // dinput.h). PeekMessageA in particular is the single most
        // frequently-called USER32 entry point in a typical Win32 message
        // pump -- the best available proxy for "the guest's own main loop
        // just iterated" without any new thread.
        s.fn = [fn](Cpu& c) -> uint32_t { ++g_ign_user32_call_count; dinput_poll_vita_input(c); return fn(c); };
        br.register_shim("USER32.dll", name, s);
    };

    K("RegisterClassA", 1, [](Cpu& c) -> uint32_t {
        uint32_t p = c.arg(0);
        uint32_t style = c.read_u32(p + 0x00);
        uint32_t wndproc = c.read_u32(p + 0x04);
        int32_t cbClsExtra = (int32_t)c.read_u32(p + 0x08);
        int32_t cbWndExtra = (int32_t)c.read_u32(p + 0x0C);
        uint32_t hInstance = c.read_u32(p + 0x10);
        uint32_t hIcon = c.read_u32(p + 0x14);
        uint32_t hCursor = c.read_u32(p + 0x18);
        uint32_t hbrBackground = c.read_u32(p + 0x1C);
        uint32_t lpszMenuName = c.read_u32(p + 0x20);
        std::string className = read_guest_cstr(c, c.read_u32(p + 0x24));

        IGN_LOG("[USER32]", "RegisterClassA");
        IGN_LOG("[USER32]", "  style         = 0x%08x", style);
        IGN_LOG("[USER32]", "  lpfnWndProc   = 0x%08x", wndproc);
        IGN_LOG("[USER32]", "  cbClsExtra    = %d", cbClsExtra);
        IGN_LOG("[USER32]", "  cbWndExtra    = %d", cbWndExtra);
        IGN_LOG("[USER32]", "  hInstance     = 0x%08x", hInstance);
        IGN_LOG("[USER32]", "  hIcon         = 0x%08x", hIcon);
        IGN_LOG("[USER32]", "  hCursor       = 0x%08x", hCursor);
        IGN_LOG("[USER32]", "  hbrBackground = 0x%08x", hbrBackground);
        IGN_LOG("[USER32]", "  lpszMenuName  = 0x%08x", lpszMenuName);
        IGN_LOG("[USER32]", "  lpszClassName = \"%s\"", className.c_str());

        if (!wndproc) {
            IGN_LOG("[USER32]", "RegisterClassA(\"%s\") -> 0 (no lpfnWndProc)", className.c_str());
            return 0;
        }

        IgnitionWindowClass wc;
        wc.atom = (uint16_t)(0xC000u + g_classes().size());
        wc.guest_wndproc = wndproc;
        wc.hinstance = hInstance;
        wc.style = style;
        wc.class_name = className;
        g_classes()[className] = wc;

        set_phase(Phase::RegisterClass, className.c_str());
        IGN_LOG("[USER32]", "RegisterClassA(\"%s\") -> ATOM 0x%04x", className.c_str(), wc.atom);
        return wc.atom;
    });

    K("CreateWindowExA", 12, [&br](Cpu& c) -> uint32_t {
        uint32_t exStyle = c.arg(0);
        std::string className = read_guest_cstr(c, c.arg(1));
        std::string windowName = read_guest_cstr(c, c.arg(2));
        uint32_t style = c.arg(3);
        int32_t x = (int32_t)c.arg(4), y = (int32_t)c.arg(5);
        int32_t width = (int32_t)c.arg(6), height = (int32_t)c.arg(7);
        uint32_t hWndParent = c.arg(8), hMenu = c.arg(9), hInstance = c.arg(10), lpParam = c.arg(11);

        IGN_LOG("[USER32]", "CreateWindowExA");
        IGN_LOG("[USER32]", "  exStyle    = 0x%08x", exStyle);
        IGN_LOG("[USER32]", "  className  = \"%s\"", className.c_str());
        IGN_LOG("[USER32]", "  windowName = \"%s\"", windowName.c_str());
        IGN_LOG("[USER32]", "  style      = 0x%08x", style);
        IGN_LOG("[USER32]", "  x,y        = %d,%d", x, y);
        IGN_LOG("[USER32]", "  w,h        = %d,%d", width, height);
        IGN_LOG("[USER32]", "  hWndParent = 0x%08x", hWndParent);
        IGN_LOG("[USER32]", "  hMenu      = 0x%08x", hMenu);
        IGN_LOG("[USER32]", "  hInstance  = 0x%08x", hInstance);
        IGN_LOG("[USER32]", "  lpParam    = 0x%08x", lpParam);

        auto itClass = g_classes().find(className);
        if (itClass == g_classes().end()) {
            IGN_LOG("[USER32]", "CreateWindowExA(\"%s\") -> NULL (class not registered)", className.c_str());
            return 0;
        }

        uint32_t hwnd = next_hwnd();
        IgnitionWindow w;
        w.hwnd = hwnd;
        w.guest_wndproc = itClass->second.guest_wndproc;
        w.hinstance = hInstance;
        w.class_name = className;
        w.x = x; w.y = y; w.width = width; w.height = height;
        w.style = style; w.ex_style = exStyle;

        // Real Windows sends WM_NCCREATE then WM_CREATE synchronously before
        // CreateWindowExA returns. WM_NCCREATE is deliberately NOT sent this
        // iteration: an unhandled WM_NCCREATE falls to DefWindowProc, whose
        // real contract is "return TRUE to continue" -- but WinVita's own
        // DefWindowProcA (win32_shims_user32.cpp) unconditionally returns 0,
        // which we'd have to read as "creation refused". That would be an
        // artifact of the engine's simplified stub, not a real Ignition
        // failure, so sending it now would risk a false negative. WM_CREATE
        // IS sent: the disassembled WNDPROC (0x004122d0) has a genuine
        // switch(msg) case for it, not a default/DefWindowProc fallthrough.
        uint32_t cs = wx86_scratch_alloc(0x30);   // CREATESTRUCTA, 0x30 bytes
        if (cs) {
            c.write_u32(cs + 0x00, lpParam);
            c.write_u32(cs + 0x04, hInstance);
            c.write_u32(cs + 0x08, hMenu);
            c.write_u32(cs + 0x0C, hWndParent);
            c.write_u32(cs + 0x10, (uint32_t)height);
            c.write_u32(cs + 0x14, (uint32_t)width);
            c.write_u32(cs + 0x18, (uint32_t)y);
            c.write_u32(cs + 0x1C, (uint32_t)x);
            c.write_u32(cs + 0x20, style);
            c.write_u32(cs + 0x24, c.arg(2));   // lpszName -- caller's own guest string, still valid
            c.write_u32(cs + 0x28, c.arg(1));   // lpszClass
            c.write_u32(cs + 0x2C, exStyle);
        } else {
            IGN_LOG("[ERROR]", "CreateWindowExA: wx86_scratch_alloc(CREATESTRUCTA) failed");
        }

        g_windows()[hwnd] = w;   // visible before dispatch, in case WM_CREATE looks itself up by hwnd

        uint32_t wcRet = 0;
        bool wcOk = cs && call_guest_wndproc(br, c, w.guest_wndproc, hwnd, WM_CREATE, 0, cs, wcRet);
        if (!wcOk) {
            IGN_LOG("[USER32]", "CreateWindowExA(\"%s\") -> NULL (WM_CREATE dispatch failed)", className.c_str());
            g_windows().erase(hwnd);
            return 0;
        }
        IGN_LOG("[USER32]", "WM_CREATE -> %d", (int32_t)wcRet);
        if ((int32_t)wcRet == -1) {
            IGN_LOG("[USER32]", "CreateWindowExA(\"%s\") -> NULL (WndProc returned -1 from WM_CREATE)", className.c_str());
            g_windows().erase(hwnd);
            return 0;
        }

        set_phase(Phase::CreateWindow, className.c_str());
        IGN_LOG("[USER32]", "CreateWindowExA(\"%s\") -> hwnd 0x%08x", className.c_str(), hwnd);
        return hwnd;
    });

    K("ShowWindow", 2, [&br](Cpu& c) -> uint32_t {
        uint32_t hwnd = c.arg(0);
        int32_t nCmdShow = (int32_t)c.arg(1);
        auto it = g_windows().find(hwnd);
        if (it == g_windows().end()) {
            IGN_LOG("[USER32]", "ShowWindow(hwnd=0x%08x, nCmdShow=%d) -> FALSE (unknown hwnd)", hwnd, nCmdShow);
            return 0;
        }
        bool wasVisible = it->second.visible;
        bool show = nCmdShow != 0;   // SW_HIDE=0; every other documented value shows the window
        it->second.visible = show;

        // WM_SHOWWINDOW: disassembly of the real WndProc (0x004122d0) shows
        // its dispatcher only special-cases msg==1..5 (WM_CREATE/DESTROY/
        // MOVE/(unused)/SIZE) and msg==0x1C (WM_ACTIVATEAPP) -- every other
        // message in [6,0x1B], which INCLUDES WM_SHOWWINDOW (0x18), falls
        // through to the default branch (a real call to DefWindowProcA, a
        // no-op in this engine). So per the standing rule ("only enqueue if
        // the WndProc actually handles it"), WM_SHOWWINDOW is deliberately
        // NOT sent -- it would be inert even if we did.
        //
        // WM_ACTIVATEAPP, however, IS specially handled -- and it is the
        // ONLY code in the whole binary that writes the global flag at
        // 0x004C5384 (MOV EAX,[ESP+0x18]; MOV [0x4C5384],EAX inside the
        // handler at VA 0x41238F), which is the SAME flag WinMain's message
        // loop (VA 0x4121A2) tests every iteration to choose between two
        // completely different code paths:
        //   flag != 0: PeekMessageA(&msg,0,0,0,PM_NOREMOVE) first; if empty,
        //              call an idle-work routine (VA 0x412230) repeatedly --
        //              this is the real per-frame update/render path.
        //   flag == 0: call GetMessageA(&msg,0,0,0) directly and block --
        //              exactly the behavior seen on real hardware.
        // Real Win32 sends WM_ACTIVATEAPP(TRUE) synchronously (SendMessage,
        // not posted) as part of a window becoming the active/foreground
        // app -- normally bundled into the ShowWindow/SetForegroundWindow
        // activation sequence for a process's first window. Since our
        // engine never dispatches it at all, that flag stays at its
        // zero-initialized default forever, which is the actual, disassembly
        // -confirmed reason the loop always takes the "block on GetMessageA"
        // path. This is sent exactly once (first hidden->visible
        // transition, matching "app becomes foreground for the first
        // time") -- not on every ShowWindow call, to avoid a fabricated
        // repeat Windows itself wouldn't produce here.
        static bool s_activateAppSent = false;
        bool willSendActivateApp = !wasVisible && show && !s_activateAppSent;
        IGN_LOG("[MSG]", "ShowWindow semantic analysis: hwnd=0x%08x hidden->visible=%d WM_SHOWWINDOW=skip(default-handled only) WM_ACTIVATEAPP=%s",
                hwnd, (!wasVisible && show) ? 1 : 0, willSendActivateApp ? "send(TRUE)" : "skip");

        if (willSendActivateApp) {
            s_activateAppSent = true;
            uint32_t ret = 0;
#ifdef IGNITION_RENDERER_3DFX
            // iteration-G1: the [0x004C5384] readback below is Ign_d3d.exe
            // -specific (that exact global, and the "0=blocks/nonzero=Peek"
            // interpretation, come from disassembling Ign_d3d.exe's WinMain
            // loop -- neither is demonstrated for Ign_3dfx.exe's own WndProc,
            // which the G0 log shows lives at a different address entirely
            // (0x00445720). Log only what's actually known for this build:
            // that the message was dispatched, to which WndProc, and what
            // it returned -- no claim about what any global flag means.
            IGN_LOG("[3DFX-MSG]", "dispatch WM_ACTIVATEAPP(TRUE) hwnd=0x%08x WndProc=0x%08x", hwnd, it->second.guest_wndproc);
            call_guest_wndproc(br, c, it->second.guest_wndproc, hwnd, WM_ACTIVATEAPP, 1u, 0u, ret);
            IGN_LOG("[3DFX-MSG]", "WM_ACTIVATEAPP -> %d (no global-flag interpretation for this build -- not yet demonstrated)", (int32_t)ret);
#else
            IGN_LOG("[MSGQ]", "dispatch WM_ACTIVATEAPP hwnd=0x%08x wParam=1(TRUE) lParam=0", hwnd);
            call_guest_wndproc(br, c, it->second.guest_wndproc, hwnd, WM_ACTIVATEAPP, 1u, 0u, ret);
            // iteration-32: read the flag straight back instead of just
            // trusting the WndProc's return value -- this is the one
            // piece of hard evidence that will tell a future iteration
            // whether WinMain's loop (VA 0x4121A2, tests this exact
            // global) actually takes the PeekMessageA/idle-work path
            // instead of the blocking GetMessageA path we've observed.
            // No behavior change: this is a read, not a write.
            uint32_t flagAfter = c.read_u32(0x004C5384u);
            IGN_LOG("[MSGQ]", "WM_ACTIVATEAPP -> %d ; readback [0x004C5384]=0x%08x (0=loop will still block on GetMessageA, nonzero=loop should take the PeekMessageA/idle-work path instead)",
                    (int32_t)ret, flagAfter);
#endif
        }

        IGN_LOG("[USER32]", "ShowWindow(hwnd=0x%08x, nCmdShow=%d): was %s, now %s",
                hwnd, nCmdShow, wasVisible ? "visible" : "hidden", show ? "visible" : "hidden");
        return wasVisible ? 1u : 0u;
    });

    K("SetFocus", 1, [&br](Cpu& c) -> uint32_t {
        uint32_t hwnd = c.arg(0);
        uint32_t prev = g_focused_hwnd();
        auto it = g_windows().find(hwnd);
        if (it == g_windows().end()) {
            IGN_LOG("[USER32]", "SetFocus(0x%08x) -> NULL (unknown hwnd)", hwnd);
            return 0;
        }
        if (prev == hwnd) {
            IGN_LOG("[USER32]", "SetFocus(0x%08x) -> 0x%08x (already focused)", hwnd, prev);
            return prev;
        }
        if (prev) {
            auto pit = g_windows().find(prev);
            if (pit != g_windows().end()) pit->second.active = false;
        }
        it->second.active = true;
        g_focused_hwnd() = hwnd;
        IGN_LOG("[USER32]", "SetFocus(0x%08x) -> previous 0x%08x", hwnd, prev);
        uint32_t ret = 0;
        call_guest_wndproc(br, c, it->second.guest_wndproc, hwnd, WM_SETFOCUS, prev, 0, ret);
        return prev;
    });

    K("SetWindowLongA", 3, [](Cpu& c) -> uint32_t {
        uint32_t hwnd = c.arg(0);
        int32_t index = (int32_t)c.arg(1);
        uint32_t value = c.arg(2);
        auto it = g_windows().find(hwnd);
        if (it == g_windows().end()) {
            IGN_LOG("[USER32]", "SetWindowLongA(bad hwnd 0x%08x)", hwnd);
            return 0;
        }
        uint32_t old;
        if (index == GWL_WNDPROC) {
            old = it->second.guest_wndproc;
            it->second.guest_wndproc = value;
            IGN_LOG("[USER32]", "SetWindowLongA(hwnd=0x%08x, GWL_WNDPROC, 0x%08x) -> old 0x%08x", hwnd, value, old);
        } else {
            auto eit = it->second.extra.find(index);
            old = eit != it->second.extra.end() ? eit->second : 0;
            it->second.extra[index] = value;
            IGN_LOG("[USER32]", "SetWindowLongA(hwnd=0x%08x, index=%d, value=0x%08x) -> old 0x%08x (unrecognized index, stored as-is)",
                    hwnd, index, value, old);
        }
        return old;
    });

    K("GetWindowLongA", 2, [](Cpu& c) -> uint32_t {
        uint32_t hwnd = c.arg(0);
        int32_t index = (int32_t)c.arg(1);
        auto it = g_windows().find(hwnd);
        if (it == g_windows().end()) return 0;
        if (index == GWL_WNDPROC) return it->second.guest_wndproc;
        auto eit = it->second.extra.find(index);
        uint32_t v = eit != it->second.extra.end() ? eit->second : 0;
        IGN_LOG("[USER32]", "GetWindowLongA(hwnd=0x%08x, index=%d) -> 0x%08x", hwnd, index, v);
        return v;
    });

    K("PostMessageA", 4, [](Cpu& c) -> uint32_t {
        ++g_ign_postmessage_count;
        uint32_t callerEip = c.read_u32(c.reg(R_ESP));
        IgnitionMsg m;
        m.hwnd = c.arg(0); m.message = c.arg(1); m.wParam = c.arg(2); m.lParam = c.arg(3);
        m.time = (uint32_t)wx86_now_ms();
        g_queue().push_back(m);
        sync_queue_depth();
        IGN_LOG("[MSG]", "PostMessageA hwnd=0x%08x msg=%s(0x%x) wParam=0x%08x lParam=0x%08x",
                m.hwnd, msg_name(m.message), m.message, m.wParam, m.lParam);
        // iteration-33: full caller-context breadcrumb for EVERY PostMessageA
        // (the game's whole reachable message traffic is small enough that
        // this isn't noisy), with an extra, louder line specifically for
        // WM_CLOSE (0x0010) -- the exact message idle-work's 0x412530
        // sub-routine posts under a still-unconfirmed condition. This is
        // what answers "is idle-work actually asking to quit, and from
        // where" without guessing from static analysis alone.
        IGN_LOG("[POST33]", "caller_eip=0x%08x hwnd=0x%08x msg=0x%x wParam=0x%08x lParam=0x%08x",
                callerEip, m.hwnd, m.message, m.wParam, m.lParam);
        if (m.message == 0x0010u) {   // WM_CLOSE
            IGN_LOG("[WM_CLOSE33]", "PostMessageA posted WM_CLOSE! caller_eip=0x%08x hwnd=0x%08x -- "
                                     "this is the exact call idle-work's 0x412530 helper makes; if "
                                     "caller_eip==0x00412530 this confirms that path fired for real, "
                                     "not just statically identified", callerEip, m.hwnd);
            // iteration-G117: breadcrumb that survives the log allowlist (main + never-truncated critical log)
            IGN_LOG("[G117-EXIT]", "PostMessageA(WM_CLOSE) posted by the game caller_eip=0x%08x hwnd=0x%08x queue_depth=%u",
                    callerEip, m.hwnd, (unsigned)g_queue().size());
            IGN_CRITICAL("[G117-EXIT]", "PostMessageA(WM_CLOSE) posted by the game caller_eip=0x%08x hwnd=0x%08x", callerEip, m.hwnd);
        }
        // iteration-G35, Parte M/P: WM_CLOSE/WM_QUIT/WM_DESTROY specifically
        // dual-logged to the never-truncated critical log -- these are
        // exactly the messages that could end a session, and G34's own log
        // came back too short to show whether they ever posted. Not
        // suppressed or altered (Part P: "no bloquearlo").
        if (m.message == 0x0010u || m.message == WM_QUIT || m.message == 0x0002u) {
            IGN_CRITICAL("[G35-TRANS]", "PostMessageA msg=%s(0x%x) caller_eip=0x%08x hwnd=0x%08x wParam=0x%08x lParam=0x%08x",
                    msg_name(m.message), m.message, callerEip, m.hwnd, m.wParam, m.lParam);
        }
        return 1u;
    });

    // Overrides the engine's own no-op (win32_shims_window.cpp: `return 0u`,
    // never actually arms anything a message loop could observe). Last
    // registration wins (bridge.h) -- install this AFTER
    // win32_shims_window_install so ours is the one that sticks.
    K("PostQuitMessage", 1, [](Cpu& c) -> uint32_t {
        IgnitionMsg m;
        m.message = WM_QUIT; m.wParam = c.arg(0);
        m.time = (uint32_t)wx86_now_ms();
        g_queue().push_back(m);
        sync_queue_depth();
        IGN_LOG("[MSG]", "PostQuitMessage(%d) -> WM_QUIT queued", (int32_t)c.arg(0));
        IGN_LOG("[G117-EXIT]", "PostQuitMessage(%d) called by the game -> WM_QUIT queued", (int32_t)c.arg(0));
        IGN_CRITICAL("[G117-EXIT]", "PostQuitMessage(%d) called by the game -> WM_QUIT queued", (int32_t)c.arg(0));
        return 0u;
    });

    K("PeekMessageA", 5, [](Cpu& c) -> uint32_t {
        ++g_ign_peekmessage_count;
        mark_message_loop_once();
        uint32_t trapEip = c.read_u32(c.reg(R_ESP));
        uint32_t pMsg = c.arg(0);
        uint32_t wRemoveMsg = c.arg(4);
        uint32_t queueBefore = (uint32_t)g_queue().size();
        if (g_queue().empty()) {
            // iteration-G33 (G34, Parte E: interval widened 5s->10s), Parte
            // N/O/Q: G32's real-hardware log showed thousands of
            // "[PEEK33] ... result=0(empty)" lines a few ms apart after
            // WM_CLOSE -- log.cpp opens/writes/closes the log file on
            // EVERY line (an already-established fact), so that spam has a
            // real host-side cost, not just noise. Empty polls are now
            // counted and summarized every ~10s instead of logged
            // individually; a REAL message (the branch below) is still
            // logged every time, unconditionally -- Part P: no semantics
            // changed, this is logging only.
            ++g_g33EmptyPeekCount;
            uint64_t nowMs = wx86_now_ms();
            if (nowMs - g_g33LastPeekSummaryMs >= 10000) {
                IGN_LOG("[G34-PEEK]", "empty_calls_last_10s=%u messages_seen_last_10s=%u",
                        g_g33EmptyPeekCount, g_g33MessagesSeenCount);
                g_g33EmptyPeekCount = 0;
                g_g33MessagesSeenCount = 0;
                g_g33LastPeekSummaryMs = nowMs;
            }
            return 0u;
        }
        ++g_g33MessagesSeenCount;
        const IgnitionMsg& m = g_queue().front();
        if (pMsg) {
            c.write_u32(pMsg + 0x00, m.hwnd);
            c.write_u32(pMsg + 0x04, m.message);
            c.write_u32(pMsg + 0x08, m.wParam);
            c.write_u32(pMsg + 0x0C, m.lParam);
            c.write_u32(pMsg + 0x10, m.time);
            c.write_u32(pMsg + 0x14, (uint32_t)m.pt_x);
            c.write_u32(pMsg + 0x18, (uint32_t)m.pt_y);
        }
        if (!kG75LowOverheadRuntimeLog || g_g75RealMessageCount < 5) {
            IGN_LOG("[MSG]", "PeekMessageA -> hwnd=0x%08x msg=%s(0x%x) (wRemoveMsg=%u)",
                    m.hwnd, msg_name(m.message), m.message, wRemoveMsg);
            IGN_LOG("[PEEK33]", "trap_eip=0x%08x queue_before=%u result=1(msg=0x%x)", trapEip, queueBefore, m.message);
        }
        ++g_g75RealMessageCount;
        if (kG75LowOverheadRuntimeLog) {
            uint64_t nowMs2 = wx86_now_ms();
            if (nowMs2 - g_g75LastRealMessageSummaryMs >= 5000) {
                IGN_LOG("[G75-PEEK-SUMMARY]", "real_messages_last_interval=%u last_msg=%s(0x%x)",
                        g_g75RealMessageCount, msg_name(m.message), m.message);
                g_g75LastRealMessageSummaryMs = nowMs2;
            }
        }
        if (wRemoveMsg & 1u) { g_queue().pop_front(); sync_queue_depth(); }   // PM_REMOVE
        return 1u;
    });

    K("GetMessageA", 4, [](Cpu& c) -> uint32_t {
        ++g_ign_getmessage_count;
        mark_message_loop_once();
        uint32_t pMsg = c.arg(0);
        // iteration-35: the [MSG34] trap-stack-walk (HALLAZGO 3) found
        // that none of the first 8 stack words land in real .text for
        // THIS import path -- it doesn't resolve the caller here, so it's
        // removed rather than kept as noise. Replaced by direct guest-EIP
        // instrumentation: [GM35] PRECALL_BLOCKING (dynarec_arm_00.c,
        // 0x004121F4) fires unconditionally right before the one guest
        // instruction that can reach this shim and block, with a full
        // register dump and the flag read straight from guest memory --
        // strictly more informative than anything derivable from ESP here.
        if (g_queue().empty()) {
            IGN_LOG("[MSG]", "GetMessageA: queue empty -- real Windows blocks here; nothing in this engine "
                             "will ever post a NEW message asynchronously yet (no input, no timers), so "
                             "blocking would just hang the app instead of stopping cleanly");
            // iteration-32: explicit event-source inventory at the exact
            // moment we'd otherwise hang. timeSetEvent (winmm_ign.cpp) is
            // still a stub that always fails (returns 0/TIMERR_NOCANDO),
            // so there is no timer bookkeeping to consult yet -- this
            // logs that honestly (0, not a guess) rather than reporting a
            // number we can't back up. If a future iteration gives
            // timeSetEvent a real implementation with an armed-timer
            // registry, this line is where its count/deadline belong.
            IGN_LOG("[ASYNC32]", "queue=0 active_timers=0 next_timer_deadline=none input_source=none "
                                  "(timeSetEvent is still an unimplemented stub -- see [WIN32] timeSetEvent "
                                  "log lines earlier in this run, if any, for what the guest actually asked for)");
            request_halt("GetMessageA blocked on an empty queue (no async message source implemented yet)");
            return 0u;
        }
        IgnitionMsg m = g_queue().front();
        g_queue().pop_front();
        sync_queue_depth();
        if (pMsg) {
            c.write_u32(pMsg + 0x00, m.hwnd);
            c.write_u32(pMsg + 0x04, m.message);
            c.write_u32(pMsg + 0x08, m.wParam);
            c.write_u32(pMsg + 0x0C, m.lParam);
            c.write_u32(pMsg + 0x10, m.time);
            c.write_u32(pMsg + 0x14, (uint32_t)m.pt_x);
            c.write_u32(pMsg + 0x18, (uint32_t)m.pt_y);
        }
        IGN_LOG("[MSG]", "GetMessageA -> hwnd=0x%08x msg=%s(0x%x)", m.hwnd, msg_name(m.message), m.message);
        if (m.message == WM_QUIT) {
            IGN_LOG("[G117-EXIT]", "GetMessageA returns 0 (WM_QUIT) -> the game's message loop ends, WinMain returns");
            IGN_CRITICAL("[G117-EXIT]", "GetMessageA returns 0 (WM_QUIT) -> the game's message loop ends, WinMain returns");
        }
        return m.message == WM_QUIT ? 0u : 1u;
    });

    K("DispatchMessageA", 1, [&br](Cpu& c) -> uint32_t {
        ++g_ign_dispatchmessage_count;
        uint32_t pMsg = c.arg(0);
        uint32_t hwnd = c.read_u32(pMsg + 0x00);
        uint32_t msg = c.read_u32(pMsg + 0x04);
        uint32_t wParam = c.read_u32(pMsg + 0x08);
        uint32_t lParam = c.read_u32(pMsg + 0x0C);
        IGN_LOG("[MSG]", "DispatchMessageA hwnd=0x%08x msg=%s(0x%x) wParam=0x%08x lParam=0x%08x",
                hwnd, msg_name(msg), msg, wParam, lParam);
        // iteration-G35, Parte M: dual-logged to the critical log for the
        // same three messages as PostMessageA above -- this is the point
        // where the guest's own WNDPROC actually RUNS in response.
        if (msg == 0x0010u || msg == WM_QUIT || msg == 0x0002u) {
            IGN_CRITICAL("[G35-TRANS]", "DispatchMessageA begin msg=%s(0x%x) hwnd=0x%08x", msg_name(msg), msg, hwnd);
        }
        auto it = g_windows().find(hwnd);
        if (it == g_windows().end()) {
            IGN_LOG("[MSG]", "DispatchMessageA: unknown hwnd 0x%08x -- nothing to call", hwnd);
            return 0u;
        }
        IGN_LOG("[MSG]", "  wndproc = 0x%08x", it->second.guest_wndproc);
        uint32_t ret = 0;
        if (msg == WM_CLOSE || msg == WM_DESTROY) {
            IGN_LOG("[G117-EXIT]", "DispatchMessageA begin msg=%s(0x%x) hwnd=0x%08x wndproc=0x%08x", msg_name(msg), msg, hwnd, it->second.guest_wndproc);
            IGN_CRITICAL("[G117-EXIT]", "DispatchMessageA begin msg=%s(0x%x) hwnd=0x%08x", msg_name(msg), msg, hwnd);
        }
        call_guest_wndproc(br, c, it->second.guest_wndproc, hwnd, msg, wParam, lParam, ret);
        if (msg == 0x0010u || msg == WM_QUIT || msg == 0x0002u) {
            IGN_CRITICAL("[G35-TRANS]", "DispatchMessageA end msg=%s(0x%x) hwnd=0x%08x wndproc_ret=0x%08x", msg_name(msg), msg, hwnd, ret);
        }
        if (msg == WM_CLOSE || msg == WM_DESTROY) {
            IGN_LOG("[G117-EXIT]", "DispatchMessageA end msg=%s(0x%x) hwnd=0x%08x wndproc_ret=0x%08x", msg_name(msg), msg, hwnd, ret);
            IGN_CRITICAL("[G117-EXIT]", "DispatchMessageA end msg=%s(0x%x) hwnd=0x%08x", msg_name(msg), msg, hwnd);
        }
        return ret;
    });

    // iteration-G117: DefWindowProcA(WM_CLOSE) == DestroyWindow(hwnd).
    //
    // Ign_3dfx.exe's WNDPROC (0x00445720) has NO WM_CLOSE case: a WM_CLOSE
    // falls through to DefWindowProcA (0x0044586B), and the game relies on the
    // real Windows contract that the default handler destroys the window --
    // which sends WM_DESTROY synchronously, whose handler (case 2 at
    // 0x004457C9) runs the shutdown (0x004459C0 -> grSstWinClose via
    // [0x004F333C]) and then calls PostQuitMessage(0). The engine's own
    // DefWindowProcA (win32_shims_window.cpp) is `return 0` for everything, so
    // after "Salir del juego" (menu choice 3 -> idle stage 2 -> cleanup ->
    // PostMessageA(WM_CLOSE) once, 0x004459A0) the WM_CLOSE was dispatched,
    // ignored, and the game kept spinning in its message loop on a dark
    // screen forever. Every OTHER message keeps the old `return 0` exactly.
    // No exit is forced here: the game asked to close, this only delivers
    // the consequence Windows itself would have delivered.
    K("DefWindowProcA", 4, [&br](Cpu& c) -> uint32_t {
        uint32_t hwnd = c.arg(0), msg = c.arg(1);
        if (msg != WM_CLOSE) return 0u;
        auto it = g_windows().find(hwnd);
        if (it == g_windows().end()) {
            IGN_LOG("[G117-EXIT]", "DefWindowProcA(WM_CLOSE) for unknown/already destroyed hwnd=0x%08x -- ignored", hwnd);
            return 0u;
        }
        uint32_t wndproc = it->second.guest_wndproc;
        g_windows().erase(it);   // destroyed: no second WM_CLOSE/WM_DESTROY can reach it
        IGN_LOG("[G117-EXIT]", "DefWindowProcA(WM_CLOSE) -> DestroyWindow(hwnd=0x%08x): sending WM_DESTROY to wndproc=0x%08x", hwnd, wndproc);
        IGN_CRITICAL("[G117-EXIT]", "DefWindowProcA(WM_CLOSE) -> DestroyWindow(hwnd=0x%08x): sending WM_DESTROY", hwnd);
        uint32_t ret = 0;
        bool ok = call_guest_wndproc(br, c, wndproc, hwnd, WM_DESTROY, 0u, 0u, ret);
        IGN_LOG("[G117-EXIT]", "WM_DESTROY handler returned ok=%d (a halt requested inside it, e.g. grSstWinClose, makes the guest stop here)", (int)ok);
        IGN_CRITICAL("[G117-EXIT]", "WM_DESTROY handler returned ok=%d", (int)ok);
        return 0u;
    });
}

} // namespace ign
