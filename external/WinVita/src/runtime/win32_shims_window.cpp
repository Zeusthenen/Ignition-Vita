// src/runtime/win32_shims_window.cpp — see win32_shims_window.h. Fake
// handles, honest no-ops, and pure geometry — the parts of the USER32
// window-semantics group with zero window-subclass/message-queue/
// wndproc-dispatch state and no dependency on any consumer-hosted helper.
//
// Stays on the consumer side, each for a concrete reason, not a blanket
// "too entangled":
//   - RegisterClassA/RegisterClassExA/UnregisterClassA/CreateWindowExA:
//     CreateWindowExA builds its guest WM_NCCREATE/WM_CREATE dispatch stub
//     via a consumer-owned arena allocator bound to the guest's memory
//     layout and diagnosed via the consumer's crash log — not a generic
//     winx86 primitive.
//   - ShowWindow/PeekMessageA/GetMessageA/DispatchMessageA/SetFocus: share
//     window/message-queue/wndproc state with the window-creation quartet
//     above, plus DispatchMessageA hardcodes a literal RVA into one guest
//     binary's own wndproc hook-chain check, found by disassembly for that
//     specific binary. PeekMessageA also drives the consumer's real-clock
//     scheduler wait, remote control, and physical-input tick.
//   - GetKeyState/GetAsyncKeyState/GetKeyboardState: the key-state table is
//     written directly by the consumer's scripted-input injection code at
//     several call sites — input handling has a real regression history in
//     this codebase (window-activation/focus bugs, select-pump starvation),
//     so it stays put rather than multiplying that surface.
//
// No consumer-side tracing here either (see the other extractions):
// functional behavior is unchanged, only debug-log coverage of these calls
// lives on the consumer side.
#include "win32_shims_window.h"
#include "win32_shims_gdi32.h"   // wx86_screen_w/h
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <string>

using namespace d2rt;

namespace {
uint32_t g_curX = 0, g_curY = 0;   // virtual cursor; clamped on set to the current screen size
}

void wx86_set_cursor(uint32_t x, uint32_t y){
    uint32_t w = wx86_screen_w(), h = wx86_screen_h();
    g_curX = (w && x >= w) ? w - 1 : x;
    g_curY = (h && y >= h) ? h - 1 : y;
}
void wx86_get_cursor(uint32_t* x, uint32_t* y){ if(x) *x = g_curX; if(y) *y = g_curY; }

// PC/AT set-1 scancode for a VK/ASCII code (letters, digits, Enter/Backspace/
// Escape/Space): real Windows always fills lParam bits 16-23 on WM_KEY*, and
// some handlers check them.
static uint32_t wx86_scan_code(int v){
    static const uint8_t L[26]={0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    if(v>='A'&&v<='Z') return L[v-'A'];
    if(v>='a'&&v<='z') return L[v-'a'];
    if(v>='1'&&v<='9') return 0x02+(v-'1');
    if(v=='0') return 0x0B;
    if(v==0x0D) return 0x1C;   // Enter
    if(v==0x08) return 0x0E;   // Backspace
    if(v==0x1B) return 0x01;   // Escape
    if(v==' ')  return 0x39;
    return 0x1E;
}

void win32_shims_window_install(Bridge& br){
    auto U=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("USER32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("USER32.dll",name,s);
    };
    U("DefWindowProcA",4,[](Cpu&){ return 0u; });
    U("LoadIconA",2,[](Cpu&){ return 0x86000002u; });
    U("LoadCursorA",2,[](Cpu&){ return 0x86000003u; });
    U("LoadImageA",6,[](Cpu&){ return 0x86000001u; });
    U("UpdateWindow",1,[](Cpu&){ return 1u; });
    U("SetWindowPos",7,[](Cpu&){ return 1u; });
    U("SetForegroundWindow",1,[](Cpu&){ return 1u; });
    U("BringWindowToTop",1,[](Cpu&){ return 1u; });
    U("AdjustWindowRectEx",4,[](Cpu&){ return 1u; });
    U("AdjustWindowRect",3,[](Cpu&){ return 1u; });
    U("GetClientRect",2,[](Cpu&c){ uint32_t r=c.arg(1); c.write_u32(r,0); c.write_u32(r+4,0);
        c.write_u32(r+8,wx86_screen_w()); c.write_u32(r+12,wx86_screen_h()); return 1u; });
    U("GetWindowRect",2,[](Cpu&c){ uint32_t r=c.arg(1); c.write_u32(r,0); c.write_u32(r+4,0);
        c.write_u32(r+8,wx86_screen_w()); c.write_u32(r+12,wx86_screen_h()); return 1u; });
    U("ScreenToClient",2,[](Cpu&){ return 1u; });
    U("ClientToScreen",2,[](Cpu&){ return 1u; });
    U("GetDC",1,[](Cpu&){ return 0x87000001u; });
    U("ReleaseDC",2,[](Cpu&){ return 1u; });
    // BeginPaint/EndPaint (never called by D2 binaries — same note as
    // EnableWindow in win32_shims_user32.cpp): the hdc returned is the same
    // fake handle as GetDC — two distinct handles for the same window would
    // be a gratuitous lie.
    U("BeginPaint",2,[](Cpu&c)->uint32_t{ uint32_t ps=c.arg(1);
        if(ps){ c.write_u32(ps+0x00,0x87000001u);      // hdc
                c.write_u32(ps+0x04,0u);               // fErase
                c.write_u32(ps+0x08,0u);               // rcPaint.left
                c.write_u32(ps+0x0c,0u);               // rcPaint.top
                c.write_u32(ps+0x10,wx86_screen_w());  // rcPaint.right
                c.write_u32(ps+0x14,wx86_screen_h());  // rcPaint.bottom
                c.write_u32(ps+0x18,0u);               // fRestore
                c.write_u32(ps+0x1c,0u);               // fIncUpdate
                for(uint32_t i=0;i<32;i+=4) c.write_u32(ps+0x20+i,0u); }   // rgbReserved
        return 0x87000001u; });
    U("EndPaint",2,[](Cpu&){ return 1u; });
    U("SetCursor",1,[](Cpu&){ return 0u; });
    U("ShowCursor",1,[](Cpu&c){ static int32_t disp=0; disp += c.arg(0)?1:-1; return (uint32_t)disp; });   // real display counter
    U("ClipCursor",1,[](Cpu&){ return 1u; });
    // 1.14d's WM_MOUSEMOVE handler calls this on first move (TME_LEAVE).
    // Success is enough: the virtual cursor never leaves the window, so no
    // WM_MOUSELEAVE needs to be synthesized.
    U("TrackMouseEvent",1,[](Cpu&){ return 1u; });
    U("GetCursorPos",1,[](Cpu&c){ uint32_t p=c.arg(0); uint32_t x,y; wx86_get_cursor(&x,&y);
        c.write_u32(p,x); c.write_u32(p+4,y); return 1u; });
    U("SetCursorPos",2,[](Cpu&c){ wx86_set_cursor(c.arg(0),c.arg(1)); return 1u; });
    U("PostQuitMessage",1,[](Cpu&){ return 0u; });
    U("TranslateMessage",1,[](Cpu&){ return 0u; });
    U("SystemParametersInfoA",4,[](Cpu&){ return 1u; });
    U("GetSystemMetrics",1,[](Cpu&c){ uint32_t i=c.arg(0);
        if(i==0) return wx86_screen_w(); if(i==1) return wx86_screen_h(); return 0u; });
    // Without this shim, MapVirtualKeyA returned 0 for EVERY conversion
    // ("this key doesn't exist") — some callers use it to convert between
    // virtual-key and scan codes in both directions. MAPVK_VK_TO_VSC=0,
    // MAPVK_VSC_TO_VK=1, MAPVK_VK_TO_CHAR=2.
    U("MapVirtualKeyA",2,[](Cpu&c)->uint32_t{ uint32_t code=c.arg(0),type=c.arg(1);
        if(type==0) return wx86_scan_code((int)code);
        if(type==1){ for(int v=1;v<256;v++) if(wx86_scan_code(v)==code) return (uint32_t)v; return 0u; }
        if(type==2){ if(code>=0x41&&code<=0x5A) return code;
                     if(code>=0x30&&code<=0x39) return code;
                     if(code==0x20) return 0x20; return 0u; }
        return 0u; });
    U("SetWindowTextA",2,[](Cpu&){ return 1u; });
    U("GetWindowThreadProcessId",2,[](Cpu&c){ if(c.arg(1)) c.write_u32(c.arg(1),1); return 1u; });
    U("EnumDisplaySettingsA",3,[](Cpu&c){ uint32_t dm=c.arg(2);
        if(dm){ c.write_u32(dm+0x2c,32); c.write_u32(dm+0x30,wx86_screen_w()); c.write_u32(dm+0x34,wx86_screen_h()); c.write_u32(dm+0x38,60); } return 1u; });
    U("ChangeDisplaySettingsA",2,[](Cpu&){ return 0u; });   // DISP_CHANGE_SUCCESSFUL
}
