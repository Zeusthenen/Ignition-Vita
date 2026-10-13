// src/runtime/win32_shims_gdi32.cpp — see win32_shims_gdi32.h. Fake handles,
// honest no-ops, and pure functions of their arguments — no game-window
// state, no dependency on any consumer-hosted helper.
//
// Stays consumer-side (not generalized here), each for a concrete reason:
// CreateDIBSection, SetDIBColorTable (own the DIB bits/palette state),
// BitBlt/StretchBlt (the frame-boundary hook driving the per-image
// orchestration, not a generic GDI operation), and TextOutA (needs
// consumer-hosted string/logging helpers). GetDIBColorTable is a pure
// constant stub — unlike its Set counterpart it never touches the palette,
// so it lives here instead.
#include "win32_shims_gdi32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <string>

using namespace d2rt;

namespace {
// Neutral placeholder default — never a real guest's actual resolution.
// wx86_set_screen_size() is expected to be called once at boot by the
// consumer before any shim that reads it actually executes (registration
// order doesn't matter; only first *execution* does, and shims only run
// once the guest is pumping).
uint32_t g_screenW = 640, g_screenH = 480;
}

void wx86_set_screen_size(uint32_t w, uint32_t h){ g_screenW = w; g_screenH = h; }
uint32_t wx86_screen_w(){ return g_screenW; }
uint32_t wx86_screen_h(){ return g_screenH; }

void win32_shims_gdi32_install(Bridge& br){
    auto GD=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("GDI32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("GDI32.dll",name,s);
    };
    GD("GetStockObject",1,[](Cpu&){ return 0x88000001u; });
    GD("GetDeviceCaps",2,[](Cpu&c){ uint32_t cap=c.arg(1);
        if(cap==8) return wx86_screen_w();    // HORZRES
        if(cap==10) return wx86_screen_h();   // VERTRES
        if(cap==12) return 32u;               // BITSPIXEL
        if(cap==14) return 1u;                // PLANES
        return 0u; });
    GD("CreateCompatibleDC",1,[](Cpu&){ return 0x87000002u; });
    GD("DeleteDC",1,[](Cpu&){ return 1u; });
    GD("DeleteObject",1,[](Cpu&){ return 1u; });
    GD("SelectObject",2,[](Cpu&){ return 0x88000011u; });
    GD("GetSystemPaletteEntries",4,[](Cpu&){ return 0u; });
    GD("CreatePalette",1,[](Cpu&){ return 0x88000012u; });
    GD("SelectPalette",3,[](Cpu&){ return 0x88000013u; });
    GD("RealizePalette",1,[](Cpu&){ return 0u; });
    GD("SetDeviceGammaRamp",2,[](Cpu&){ return 1u; });
    GD("GetDeviceGammaRamp",2,[](Cpu&){ return 1u; });
    GD("StretchDIBits",13,[](Cpu&){ return 0u; });
    GD("GdiFlush",0,[](Cpu&){ return 1u; });
    GD("GdiSetBatchLimit",1,[](Cpu&){ return 1u; });
    GD("CreateFontA",14,[](Cpu&){ return 0x88000020u; });
    GD("SetTextColor",2,[](Cpu&){ return 0u; });
    GD("SetBkMode",2,[](Cpu&){ return 1u; });
    GD("SetBkColor",2,[](Cpu&){ return 0u; });
    GD("GetTextExtentPoint32A",4,[](Cpu&c){ uint32_t p=c.arg(3); if(p){ c.write_u32(p,8*c.arg(2)); c.write_u32(p+4,16);} return 1u; });
    GD("GetTextExtentPointA",4,[](Cpu&c){ uint32_t p=c.arg(3); if(p){ c.write_u32(p,8*c.arg(2)); c.write_u32(p+4,16);} return 1u; });
    GD("GetDIBColorTable",4,[](Cpu&){ return 256u; });
    GD("SetPaletteEntries",4,[](Cpu&){ return 256u; });
    GD("AnimatePalette",4,[](Cpu&){ return 1u; });
    GD("UnrealizeObject",1,[](Cpu&){ return 1u; });
    GD("SetStretchBltMode",2,[](Cpu&){ return 1u; });
    GD("PatBlt",6,[](Cpu&){ return 1u; });
    // Line-drawing family (never called by D2 binaries, per the original
    // note above EnableWindow): TRUE / a non-null handle, no real drawing —
    // a caller is entitled to read back the previous pen position.
    static uint32_t s_penX=0, s_penY=0;
    GD("MoveToEx",4,[](Cpu&c){ uint32_t p=c.arg(3);
        if(p){ c.write_u32(p,s_penX); c.write_u32(p+4,s_penY); }
        s_penX=c.arg(1); s_penY=c.arg(2); return 1u; });
    GD("LineTo",3,[](Cpu&c){ s_penX=c.arg(1); s_penY=c.arg(2); return 1u; });
    GD("CreatePen",3,[](Cpu&){ static uint32_t h=0x88000040u; return h++; });
    GD("CreateSolidBrush",1,[](Cpu&){ static uint32_t h=0x88000080u; return h++; });
    GD("CreateBitmap",5,[](Cpu&){ static uint32_t h=0x87000030u; return h++; });
    GD("CreateCompatibleBitmap",3,[](Cpu&){ static uint32_t h=0x87000060u; return h++; });
    GD("CreateDCA",4,[](Cpu&){ return 0x87000005u; });
    GD("GetCharWidthA",4,[](Cpu&c){ uint32_t f=c.arg(1),l=c.arg(2),b=c.arg(3);
        if(b) for(uint32_t i=0;i<=l-f&&i<256;i++) c.write_u32(b+4*i,8); return 1u; });
    GD("GetPixel",3,[](Cpu&){ return 0u; });
}
