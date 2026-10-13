// src/runtime/win32_shims_user32.cpp — see win32_shims_user32.h. These
// bodies are pure functions of their arguments (RECT geometry, string
// scan/lower) or honest constant no-op stubs — no game-window state, no
// dependency on any consumer-hosted helper. The rest of the same group —
// window-subclassing (SetWindowLongA/GetWindowLongA/CallWindowProcA),
// message routing (PostMessageA/SendMessageA), the diagnostic MessageBoxA,
// and wsprintfA/wvsprintfA (need a consumer-hosted string formatter) —
// stays on the consumer side (win32_shims_user32_d2.cpp).
//
// No consumer-side tracing here either (see win32_shims_advapi32.cpp):
// functional behavior is unchanged, only debug-log coverage of these calls
// lives on the consumer side.
#include "win32_shims_user32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_user32_install(Bridge& br){
    auto U=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("USER32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("USER32.dll",name,s);
    };
    U("LoadStringA",4,[](Cpu&c){ uint32_t buf=c.arg(2); if(buf){ uint8_t z=0; c.write(buf,&z,1);} return 0u; });
    U("LoadStringW",4,[](Cpu&c){ uint32_t buf=c.arg(2); if(buf){ uint16_t z=0; c.write(buf,&z,2);} return 0u; });
    U("LoadAcceleratorsA",2,[](Cpu&){ return 0x86000010u; });
    // CharLowerBuffA (stdcall 2 args) — lowercases cchLength chars in place, returns
    // cchLength. Unshimmed = ESP drift by 8 on the game-load path. Real impl.
    U("CharLowerBuffA",2,[](Cpu&c){ uint32_t p=c.arg(0),n=c.arg(1);
        for(uint32_t i=0;i<n;i++){ uint8_t ch=0; c.read(p+i,&ch,1);
            if(ch>='A'&&ch<='Z'){ ch=(uint8_t)(ch+32); c.write(p+i,&ch,1);} }
        return n; });
    // PtInRect(const RECT*, POINT): POINT is passed BY VALUE = 2 dwords.
    U("PtInRect",3,[](Cpu&c)->uint32_t{ uint32_t r=c.arg(0); if(!r) return 0u;
        int32_t l=(int32_t)c.read_u32(r),t=(int32_t)c.read_u32(r+4),
                ri=(int32_t)c.read_u32(r+8),b=(int32_t)c.read_u32(r+12),
                x=(int32_t)c.arg(1),y=(int32_t)c.arg(2);
        return (x>=l&&x<ri&&y>=t&&y<b)?1u:0u; });
    // IntersectRect/UnionRect: real Win32 contract handles a degenerate
    // (empty) input specially — IntersectRect zero-fills the dest and
    // returns FALSE when the boxes don't overlap; UnionRect returns the
    // other rect verbatim when one input is empty rather than min/max-ing
    // degenerate (0,0,0,0) coordinates into the result. A naive min/max
    // without these checks silently corrupts the union when one side is
    // empty, which is why this shape (not the simpler one) is the version
    // kept.
    U("IntersectRect",3,[](Cpu&c)->uint32_t{ uint32_t d=c.arg(0),ra=c.arg(1),rb=c.arg(2); if(!d) return 0u;
        auto R=[&](uint32_t p,int i){ return (int32_t)c.read_u32(p+4u*i); };
        int32_t l=std::max(R(ra,0),R(rb,0)),t=std::max(R(ra,1),R(rb,1));
        int32_t r=std::min(R(ra,2),R(rb,2)),bo=std::min(R(ra,3),R(rb,3));
        if(l>=r||t>=bo){ for(int i=0;i<4;i++) c.write_u32(d+4u*i,0); return 0u; }
        c.write_u32(d,l); c.write_u32(d+4,t); c.write_u32(d+8,r); c.write_u32(d+12,bo); return 1u; });
    U("UnionRect",3,[](Cpu&c)->uint32_t{ uint32_t d=c.arg(0),ra=c.arg(1),rb=c.arg(2); if(!d) return 0u;
        auto R=[&](uint32_t p,int i){ return (int32_t)c.read_u32(p+4u*i); };
        bool ea=R(ra,0)>=R(ra,2)||R(ra,1)>=R(ra,3), eb=R(rb,0)>=R(rb,2)||R(rb,1)>=R(rb,3);
        if(ea&&eb){ for(int i=0;i<4;i++) c.write_u32(d+4u*i,0); return 0u; }
        if(ea||eb){ uint32_t src=ea?rb:ra; for(int i=0;i<4;i++) c.write_u32(d+4u*i,(uint32_t)R(src,i)); return 1u; }
        c.write_u32(d,std::min(R(ra,0),R(rb,0))); c.write_u32(d+4,std::min(R(ra,1),R(rb,1)));
        c.write_u32(d+8,std::max(R(ra,2),R(rb,2))); c.write_u32(d+12,std::max(R(ra,3),R(rb,3))); return 1u; });
    U("OffsetRect",3,[](Cpu&c)->uint32_t{ uint32_t r=c.arg(0); if(!r) return 0u;
        int32_t dx=(int32_t)c.arg(1),dy=(int32_t)c.arg(2);
        c.write_u32(r,(uint32_t)((int32_t)c.read_u32(r)+dx)); c.write_u32(r+4,(uint32_t)((int32_t)c.read_u32(r+4)+dy));
        c.write_u32(r+8,(uint32_t)((int32_t)c.read_u32(r+8)+dx)); c.write_u32(r+12,(uint32_t)((int32_t)c.read_u32(r+12)+dy));
        return 1u; });
    U("CopyRect",2,[](Cpu&c){ uint32_t d=c.arg(0),s=c.arg(1); if(!d||!s) return 0u;
        uint8_t b[16]; c.read(s,b,16); c.write(d,b,16); return 1u; });
    U("SetRect",5,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        for(int i=0;i<4;i++) c.write_u32(p+4u*i,c.arg(1+i)); return 1u; });
    U("SetRectEmpty",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        for(int i=0;i<4;i++) c.write_u32(p+4u*i,0); return 1u; });
    U("IsRectEmpty",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 1u;
        int32_t l=(int32_t)c.read_u32(p),t=(int32_t)c.read_u32(p+4),r=(int32_t)c.read_u32(p+8),bo=(int32_t)c.read_u32(p+12);
        return (l>=r||t>=bo)?1u:0u; });
    U("EqualRect",2,[](Cpu&c){ uint32_t a2=c.arg(0),b2=c.arg(1); if(!a2||!b2) return 0u;
        for(int i=0;i<4;i++) if(c.read_u32(a2+4u*i)!=c.read_u32(b2+4u*i)) return 0u; return 1u; });
    U("InflateRect",3,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        int32_t dx=(int32_t)c.arg(1),dy=(int32_t)c.arg(2);
        c.write_u32(p+0,(uint32_t)((int32_t)c.read_u32(p+0)-dx)); c.write_u32(p+4,(uint32_t)((int32_t)c.read_u32(p+4)-dy));
        c.write_u32(p+8,(uint32_t)((int32_t)c.read_u32(p+8)+dx)); c.write_u32(p+12,(uint32_t)((int32_t)c.read_u32(p+12)+dy));
        return 1u; });
    U("TranslateAcceleratorA",3,[](Cpu&){ return 0u; });
    U("TranslateAccelerator",3,[](Cpu&){ return 0u; });
    U("CharNextA",1,[](Cpu&c){ uint32_t p=c.arg(0); uint8_t b=0; c.read(p,&b,1); return p+(b?1:0); });
    // GetSystemMetrics: real impl (SM_CXSCREEN=0/SM_CYSCREEN=1) lives in
    // win32_shims_window.cpp, which reads the shared wx86_screen_w/h() — a
    // single registration, not duplicated here.
    U("IsWindow",1,[](Cpu&){ return 0u; });
    U("FindWindowA",2,[](Cpu&){ return 0u; });
    U("GetDesktopWindow",0,[](Cpu&){ return 0u; });
    U("DestroyWindow",1,[](Cpu&){ return 1u; });
    // Generic Win32 coverage: no known guest calls this, kept so another
    // Win32 binary already finds it here — standard arity, honest return,
    // same caution as the rest of this file.
    U("EnableWindow",2,[](Cpu&){ return 0u; });                 // previous state: the window was not disabled
    U("MessageBeep",1,[](Cpu&){ return 1u; });                  // no audio device
}
